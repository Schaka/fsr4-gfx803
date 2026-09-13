/*
 * Autotune: measure every variant of a shader in the real frame and keep the fastest.
 * Included by fsr4_layer.c, which owns the device dispatch table and the config.
 */

static void tune_init(struct device_data *dd)
{
    struct tune_state *t = &dd->tune;
    t->enabled = getenv("FSR4_AUTOTUNE") != NULL;
    if (!t->enabled)
        return;

    const char *home = getenv("HOME");
    const char *v = getenv("FSR4_VARIANTS");
    if (v)
        snprintf(t->variants_dir, sizeof(t->variants_dir), "%s", v);
    else
        snprintf(t->variants_dir, sizeof(t->variants_dir), "%s/.cache/fsr4_opt/variants", home ? home : "/tmp");

    const char *c = getenv("FSR4_TUNE_CACHE");
    if (c)
        snprintf(t->cache_file, sizeof(t->cache_file), "%s", c);
    else
        snprintf(t->cache_file, sizeof(t->cache_file), "%s/.cache/fsr4_opt/choice.txt", home ? home : "/tmp");

    t->samples_wanted = getenv("FSR4_TUNE_SAMPLES") ? (unsigned)atoi(getenv("FSR4_TUNE_SAMPLES")) : 60;
    if (t->samples_wanted < 4)
        t->samples_wanted = 4;

    VkQueryPoolCreateInfo qi = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
    qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
    qi.queryCount = TUNE_QUERIES;
    if (dd->create_query_pool(dd->device, &qi, NULL, &t->pool) != VK_SUCCESS) {
        fprintf(stderr, "fsr4_layer: no timestamp queries, autotune is off\n");
        t->enabled = false;
        return;
    }
    fprintf(stderr, "fsr4_layer: autotune on, variants from %s, %u samples per variant\n",
            t->variants_dir, t->samples_wanted);
}

/* Reads the remembered choice for one shader. Returns -1 when there is none. */
static void tune_cached_choice(struct tune_state *t, uint64_t hash, char *out, size_t n)
{
    out[0] = '\0';
    FILE *f = fopen(t->cache_file, "r");
    if (!f)
        return;
    char line[160];
    while (fgets(line, sizeof(line), f)) {
        unsigned long long h;
        char name[64];
        if (sscanf(line, "%llx %63s", &h, name) == 2 && h == (unsigned long long)hash) {
            snprintf(out, n, "%s", name);
            break;
        }
    }
    fclose(f);
}

static void tune_remember_choice(struct tune_state *t, uint64_t hash, const char *name)
{
    FILE *f = fopen(t->cache_file, "a");
    if (!f)
        return;
    fprintf(f, "%016llx %s\n", (unsigned long long)hash, name);
    fclose(f);
}

/* Loads one variant's replacement for this shader, or NULL. */
static uint32_t *tune_variant_code(struct tune_state *t, const char *variant, uint64_t hash, size_t *size)
{
    char path[700];
    snprintf(path, sizeof(path), "%s/%s/%016llx.spv", t->variants_dir, variant, (unsigned long long)hash);
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint32_t *code = NULL;
    if (n >= 20 && n % 4 == 0 && (code = malloc(n)) && fread(code, 1, n, f) == (size_t)n && code[0] == 0x07230203) {
        fclose(f);
        *size = n;
        return code;
    }
    free(code);
    fclose(f);
    return NULL;
}

/* Builds a pipeline per variant. Returns the shader record, or NULL when there is nothing to tune.
 * The variant code becomes a module of its own, so this works whether the game passed a module
 * handle or an inline module. */
static struct tune_shader *tune_build(struct device_data *dd, uint64_t hash,
                                      const VkComputePipelineCreateInfo *info,
                                      VkPipelineCache cache, const VkAllocationCallbacks *alloc)
{
    struct tune_state *t = &dd->tune;
    if (!t->enabled || t->shader_count >= TUNE_MAX_SHADERS)
        return NULL;

    DIR *d = opendir(t->variants_dir);
    if (!d)
        return NULL;

    char chosen[64];
    tune_cached_choice(t, hash, chosen, sizeof(chosen));

    struct tune_shader *sh = &t->shaders[t->shader_count];
    memset(sh, 0, sizeof(*sh));
    sh->hash = hash;
    sh->chosen = -1;

    /* The game's own shader is always one of the candidates. */
    snprintf(sh->variants[0].name, sizeof(sh->variants[0].name), "original");
    sh->count = 1;

    struct dirent *e;
    while ((e = readdir(d)) && sh->count < TUNE_MAX_VARIANTS) {
        if (e->d_name[0] == '.')
            continue;
        size_t size = 0;
        uint32_t *code = tune_variant_code(t, e->d_name, hash, &size);
        if (!code)
            continue;

        VkShaderModuleCreateInfo mi = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
        mi.pCode = code;
        mi.codeSize = size;
        VkShaderModule mod = VK_NULL_HANDLE;
        VkResult r = dd->create_shader_module(dd->device, &mi, alloc, &mod);
        if (r != VK_SUCCESS) {
            free(code);
            continue;
        }

        VkComputePipelineCreateInfo ci = *info;
        ci.stage.module = mod;
        ci.stage.pNext = NULL;
        VkPipeline p = VK_NULL_HANDLE;
        r = dd->create_compute_pipelines(dd->device, cache, 1, &ci, alloc, &p);
        dd->destroy_shader_module(dd->device, mod, alloc);
        free(code);
        if (r != VK_SUCCESS || p == VK_NULL_HANDLE)
            continue;

        struct tune_variant *var = &sh->variants[sh->count];
        snprintf(var->name, sizeof(var->name), "%s", e->d_name);
        var->pipeline = p;
        var->usable = true;
        if (chosen[0] && !strcmp(chosen, var->name))
            sh->chosen = (int)sh->count;
        sh->count++;
    }
    closedir(d);

    if (sh->count < 2)
        return NULL;                    /* only the original, nothing to compare */

    if (chosen[0] && !strcmp(chosen, "original"))
        sh->chosen = 0;
    if (g_debug)
        fprintf(stderr, "fsr4_layer: %016llx has %u variants%s%s\n", (unsigned long long)hash,
                sh->count, chosen[0] ? ", remembered " : "", chosen[0] ? chosen : "");
    t->shader_count++;
    return sh;
}

static struct tune_shader *tune_find(struct tune_state *t, VkPipeline app_handle)
{
    for (unsigned i = 0; i < t->shader_count; i++)
        if (t->shaders[i].app_handle == app_handle)
            return &t->shaders[i];
    return NULL;
}

/* Which pipeline should run now: the winner, or the variant still being measured. */
static VkPipeline tune_pick(struct tune_shader *sh, unsigned *variant_out)
{
    unsigned v = sh->chosen >= 0 ? (unsigned)sh->chosen : sh->current;
    *variant_out = v;
    return v == 0 ? sh->app_handle : sh->variants[v].pipeline;
}

/* Collects finished measurements. Called on submit, and never waits on the GPU. */
static void tune_collect(struct device_data *dd)
{
    struct tune_state *t = &dd->tune;
    for (unsigned i = 0; i < TUNE_QUERIES / 2; i++) {
        if (!t->slots[i].pending)
            continue;
        uint64_t ts[2];
        VkResult r = dd->get_query_pool_results(dd->device, t->pool, i * 2, 2, sizeof(ts), ts,
                                                sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);
        if (r != VK_SUCCESS)
            continue;                   /* not ready, try again next time */
        t->slots[i].pending = false;
        struct tune_shader *sh = &t->shaders[t->slots[i].shader];
        struct tune_variant *var = &sh->variants[t->slots[i].variant];
        if (ts[1] > ts[0]) {
            var->total_ms += (double)(ts[1] - ts[0]) * t->timestamp_period / 1e6;
            var->samples++;
        }
    }

    /* Move on once the variant under test has enough samples, and decide when all are done. */
    for (unsigned s = 0; s < t->shader_count; s++) {
        struct tune_shader *sh = &t->shaders[s];
        if (sh->chosen >= 0)
            continue;
        if (sh->variants[sh->current].samples < t->samples_wanted)
            continue;
        unsigned next = sh->current + 1;
        while (next < sh->count && !sh->variants[next].usable && next != 0)
            next++;
        if (next < sh->count) {
            sh->current = next;
            continue;
        }
        /* Every variant measured. Keep the fastest. */
        int best = 0;
        double best_ms = 1e30;
        for (unsigned v = 0; v < sh->count; v++) {
            if (!sh->variants[v].samples)
                continue;
            double avg = sh->variants[v].total_ms / sh->variants[v].samples;
            if (avg < best_ms) {
                best_ms = avg;
                best = (int)v;
            }
        }
        sh->chosen = best;
        tune_remember_choice(t, sh->hash, sh->variants[best].name);
        fprintf(stderr, "fsr4_layer: %016llx -> %s at %.3f ms", (unsigned long long)sh->hash,
                sh->variants[best].name, best_ms);
        for (unsigned v = 0; v < sh->count; v++)
            if (sh->variants[v].samples)
                fprintf(stderr, "  [%s %.3f]", sh->variants[v].name,
                        sh->variants[v].total_ms / sh->variants[v].samples);
        fprintf(stderr, "\n");
    }
}
