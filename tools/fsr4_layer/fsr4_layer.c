/*
 * A Vulkan layer that swaps FSR4's compute shaders for tuned ones.
 *
 * The layer hashes every SPIR-V module the application creates and looks for a replacement in the
 * cache directory. A replacement is used only if it holds a valid SPIR-V header. This is the same
 * idea as VKD3D_SHADER_OVERRIDE, but it sits below the translation layer, so it also covers native
 * Vulkan and any other D3D12 layer.
 *
 * With FSR4_AUTOTUNE=1 it goes further: it builds every known variant of a shader, measures the real
 * dispatches on this card, keeps the fastest, and remembers the choice. See autotune.h.
 *
 * Environment:
 *   FSR4_SET           name of a shader set, or off. Read from FSR4_SETS, or from
 *                      ~/.local/share/fsr4/sets
 *   FSR4_SETS          directory that holds the sets
 *   FSR4_LAYER_CACHE   a directory of <spirv-hash>.spv, used when FSR4_SET is not given
 *   FSR4_LAYER_DUMP    directory to write every module to, named by its hash. Off by default.
 *   FSR4_LAYER_DEBUG   1 prints one line per module
 *   FSR4_AUTOTUNE      1 measures the variants and keeps the fastest. Off by default.
 *
 * Build: gcc -O2 -fPIC -shared -o libfsr4_layer.so fsr4_layer.c -lpthread
 */
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "autotune.h"
#include "profile.h"
#include "spv_sdot.c.inc"

#ifndef VK_LAYER_EXPORT
#define VK_LAYER_EXPORT __attribute__((visibility("default")))
#endif

#define MAX_DEVICES 8

struct device_data {
    VkDevice device;
    PFN_vkCreateShaderModule create_shader_module;
    PFN_vkDestroyShaderModule destroy_shader_module;
    PFN_vkCreateComputePipelines create_compute_pipelines;
    PFN_vkDestroyPipeline destroy_pipeline;
    PFN_vkCreateQueryPool create_query_pool;
    PFN_vkGetQueryPoolResults get_query_pool_results;
    PFN_vkCmdBindPipeline cmd_bind_pipeline;
    PFN_vkCmdDispatch cmd_dispatch;
    PFN_vkCmdResetQueryPool cmd_reset_query_pool;
    PFN_vkCmdWriteTimestamp cmd_write_timestamp;
    PFN_vkQueueSubmit queue_submit;
    PFN_vkQueueSubmit2 queue_submit2;
    PFN_vkGetDeviceProcAddr get_device_proc;
    struct tune_state tune;
    struct prof_state prof;
};

/* One entry per command buffer that has a tuned pipeline bound. */
#define MAX_CMDBUFS 64
static struct {
    VkCommandBuffer cmd;
    struct device_data *dd;
    unsigned shader;
    unsigned variant;
    bool active;
} g_bound[MAX_CMDBUFS];

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

/* The game creates a module, then a pipeline from it. The tuner needs the hash at pipeline time. */
#define MAX_MODULES 4096
static struct {
    VkShaderModule module;
    uint64_t hash;
    size_t size;
} g_module_hash[MAX_MODULES];
static unsigned g_module_count;

static void remember_module(VkShaderModule module, uint64_t hash, size_t size)
{
    pthread_mutex_lock(&g_lock);
    unsigned i = g_module_count % MAX_MODULES;
    g_module_hash[i].module = module;
    g_module_hash[i].hash = hash;
    g_module_hash[i].size = size;
    g_module_count++;
    pthread_mutex_unlock(&g_lock);
}

/* The size of the SPIR-V a module was built from, or 0 when it was not seen. */
static size_t module_size_of(VkShaderModule module)
{
    for (unsigned i = 0; i < MAX_MODULES; i++)
        if (g_module_hash[i].module == module)
            return g_module_hash[i].size;
    return 0;
}

/* The module create info a pipeline carries inline, if it carries one. */
static const VkShaderModuleCreateInfo *inline_module(const VkComputePipelineCreateInfo *info)
{
    const VkShaderModuleCreateInfo *m = info->stage.pNext;
    while (m && m->sType != VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO)
        m = m->pNext;
    return m;
}

static uint64_t module_hash(VkShaderModule module)
{
    for (unsigned i = 0; i < MAX_MODULES; i++)
        if (g_module_hash[i].module == module)
            return g_module_hash[i].hash;
    return 0;
}

static struct device_data g_devices[MAX_DEVICES];
static char g_cache[512];
static char g_keys_dir[512];
static char g_dump[512];
static int g_debug;
static int g_autotune;
static int g_profile;
static int g_expand_sdot;             /* set when the card has no packed dot product */
static float g_timestamp_period = 1.0f;

static void init_config(void)
{
    static int done;
    if (done)
        return;
    done = 1;
    /* A set name is enough: the layer reads the shaders straight out of the set directory, so it
     * works as an implicit layer with nothing but FSR4_SET in the environment. */
    const char *set = getenv("FSR4_SET");
    const char *sets = getenv("FSR4_SETS");
    const char *c = getenv("FSR4_LAYER_CACHE");
    const char *home = getenv("HOME");

    if (set && strcmp(set, "off")) {
        static const struct { const char *alias, *real; } aliases[] = {
            { "lossless", "exact" }, { "quality", "fin15" },
            { "balanced", "fin25" }, { "speed", "prune16" },
        };
        for (unsigned i = 0; i < sizeof(aliases) / sizeof(aliases[0]); i++)
            if (!strcmp(set, aliases[i].alias)) {
                set = aliases[i].real;
                break;
            }
        if (sets) {
            snprintf(g_cache, sizeof(g_cache), "%s/%s", sets, set);
            snprintf(g_keys_dir, sizeof(g_keys_dir), "%s", sets);
        } else {
            snprintf(g_cache, sizeof(g_cache), "%s/.local/share/fsr4/sets/%s", home ? home : "/tmp", set);
            snprintf(g_keys_dir, sizeof(g_keys_dir), "%s/.local/share/fsr4/sets", home ? home : "/tmp");
        }
    } else if (c) {
        snprintf(g_cache, sizeof(g_cache), "%s", c);
    } else if (set) {
        g_cache[0] = '\0';             /* FSR4_SET=off */
    } else {
        snprintf(g_cache, sizeof(g_cache), "%s/.cache/fsr4_opt/spirv", home ? home : "/tmp");
    }
    const char *d = getenv("FSR4_LAYER_DUMP");
    if (d) {
        snprintf(g_dump, sizeof(g_dump), "%s", d);
        mkdir(g_dump, 0755);
    }
    g_debug = getenv("FSR4_LAYER_DEBUG") != NULL;
    g_autotune = getenv("FSR4_AUTOTUNE") != NULL;
    g_profile = getenv("FSR4_PROFILE") != NULL;
}

/* FNV-1a over the module, the same shape of hash vkd3d uses for its own dumps. */
static uint64_t spirv_hash(const uint32_t *code, size_t size)
{
    uint64_t h = 0xcbf29ce484222325ull;
    const uint8_t *p = (const uint8_t *)code;
    for (size_t i = 0; i < size; i++)
        h = (h * 0x100000001b3ull) ^ p[i];
    return h;
}

static struct device_data *device_data(VkDevice device)
{
    for (unsigned i = 0; i < MAX_DEVICES; i++)
        if (g_devices[i].device == device)
            return &g_devices[i];
    return NULL;
}

/* Sets are named after the shader they come from, which does not change between vkd3d builds. The
 * hash the layer sees does change, so keys.txt maps one to the other, with a line per build. */
static bool resolve_key(uint64_t hash, char *out, size_t n)
{
    char path[700];
    snprintf(path, sizeof(path), "%s/keys.txt", g_keys_dir[0] ? g_keys_dir : g_cache);
    FILE *f = fopen(path, "r");
    if (!f)
        return false;
    char line[160];
    bool found = false;
    while (fgets(line, sizeof(line), f)) {
        unsigned long long seen;
        char name[64];
        if (sscanf(line, "%llx %63s", &seen, name) == 2 && seen == (unsigned long long)hash) {
            snprintf(out, n, "%s", name);
            found = true;
            break;
        }
    }
    fclose(f);
    return found;
}

/* Reads a replacement module. Returns NULL when there is none. */
static uint32_t *load_replacement(uint64_t hash, size_t *out_size)
{
    char path[700];
    char name[64];
    if (!g_cache[0])
        return NULL;
    if (resolve_key(hash, name, sizeof(name)))
        snprintf(path, sizeof(path), "%s/%s.spv", g_cache, name);
    else
        snprintf(path, sizeof(path), "%s/%016lx.spv", g_cache, (unsigned long)hash);
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint32_t *code = NULL;
    if (n >= 20 && n % 4 == 0 && (code = malloc(n)) && fread(code, 1, n, f) == (size_t)n &&
        code[0] == 0x07230203) {
        fclose(f);
        *out_size = n;
        return code;
    }
    free(code);
    fclose(f);
    return NULL;
}

/* Writes the module under the name of the SPIR-V the game handed us, which is the name a
 * replacement has to carry. The contents are what the driver would compile, so a dumped shader whose
 * dot products were rewritten holds the rewritten form. That is what the tuner works on. */
static void dump_module(uint64_t hash, const uint32_t *code, size_t size)
{
    char path[700];
    snprintf(path, sizeof(path), "%s/%016lx.spv", g_dump, (unsigned long)hash);
    FILE *f = fopen(path, "wb");
    if (f) {
        fwrite(code, 1, size, f);
        fclose(f);
    }
}

#include "profile.c.inc"
#include "autotune.c.inc"

/* vkd3d-proton usually skips VkShaderModule and puts the module inline in the pipeline stage, so the
 * same replacement has to happen there. The create-infos are copied, because the caller owns them. */
static VKAPI_ATTR VkResult VKAPI_CALL fsr4_CreateComputePipelines(
        VkDevice device, VkPipelineCache cache, uint32_t count,
        const VkComputePipelineCreateInfo *infos, const VkAllocationCallbacks *alloc,
        VkPipeline *pipelines)
{
    struct device_data *dd = device_data(device);
    init_config();

    VkComputePipelineCreateInfo *copy = NULL;
    VkShaderModuleCreateInfo *mods = NULL;
    uint32_t **codes = NULL;
    unsigned patched = 0;

    if (dd->tune.enabled)
        for (uint32_t i = 0; i < count; i++)
            pipelines[i] = VK_NULL_HANDLE;

    for (uint32_t i = 0; i < count; i++) {
        const VkShaderModuleCreateInfo *m = infos[i].stage.pNext;
        while (m && m->sType != VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO)
            m = m->pNext;

        if (!m && dd->tune.enabled && infos[i].stage.module != VK_NULL_HANDLE) {
            /* The game passed a module handle. Its hash was recorded when it was created. */
            uint64_t mh = module_hash(infos[i].stage.module);
            struct tune_shader *sh = mh ? tune_build(dd, mh, &infos[i], cache, alloc) : NULL;
            if (sh) {
                VkResult rr = dd->create_compute_pipelines(device, cache, 1, &infos[i], alloc, &pipelines[i]);
                if (rr != VK_SUCCESS)
                    return rr;
                sh->app_handle = pipelines[i];
                sh->variants[0].pipeline = pipelines[i];
                sh->variants[0].usable = true;
            }
            continue;
        }
        if (!m)
            continue;

        uint64_t h = spirv_hash(m->pCode, m->codeSize);
        if (dd->tune.enabled) {
            /* Build every variant, then let the game have the original handle. The bind hook puts
             * the variant under test into the frame. */
            struct tune_shader *sh = tune_build(dd, h, &infos[i], cache, alloc);
            if (sh) {
                VkResult rr = dd->create_compute_pipelines(device, cache, 1, &infos[i], alloc, &pipelines[i]);
                if (rr != VK_SUCCESS)
                    return rr;
                sh->app_handle = pipelines[i];
                sh->variants[0].pipeline = pipelines[i];
                sh->variants[0].usable = true;
                continue;
            }
        }

        size_t size = 0;
        uint32_t *code = load_replacement(h, &size);
        bool expanded = false;
        if (!code && g_expand_sdot) {
            code = spv_expand_sdot(m->pCode, m->codeSize, &size);
            expanded = code != NULL;
        }
        if (g_dump[0])
            dump_module(h, expanded ? code : m->pCode, expanded ? size : m->codeSize);
        if (g_debug)
            fprintf(stderr, "fsr4_layer: pipeline module %016lx, %zu bytes%s\n", (unsigned long)h,
                    m->codeSize, code ? (expanded ? " -> dot product expanded" : " -> replaced") : "");
        if (!code)
            continue;

        if (!copy) {
            copy = calloc(count, sizeof(*copy));
            mods = calloc(count, sizeof(*mods));
            codes = calloc(count, sizeof(*codes));
            if (!copy || !mods || !codes) {
                free(code);
                free(copy); free(mods); free(codes);
                return dd->create_compute_pipelines(device, cache, count, infos, alloc, pipelines);
            }
            memcpy(copy, infos, count * sizeof(*copy));
        }
        mods[i] = *m;
        mods[i].pCode = code;
        mods[i].codeSize = size;
        codes[i] = code;
        copy[i].stage.pNext = &mods[i];
        patched++;
    }

    if (dd->tune.enabled && dd->tune.shader_count) {
        /* Any pipeline the tuner did not take still has to be created. */
        for (uint32_t i = 0; i < count; i++)
            if (pipelines[i] == VK_NULL_HANDLE) {
                VkResult rr = dd->create_compute_pipelines(device, cache, 1,
                        copy ? &copy[i] : &infos[i], alloc, &pipelines[i]);
                if (rr != VK_SUCCESS) {
                    free(copy); free(mods); free(codes);
                    return rr;
                }
            }
        if (codes)
            for (uint32_t i = 0; i < count; i++)
                free(codes[i]);
        free(copy); free(mods); free(codes);
        return VK_SUCCESS;
    }

    VkResult r;
    if (patched) {
        r = dd->create_compute_pipelines(device, cache, count, copy, alloc, pipelines);
        if (r != VK_SUCCESS) {
            if (g_debug)
                fprintf(stderr, "fsr4_layer: %u replacements rejected, using the originals\n", patched);
            r = dd->create_compute_pipelines(device, cache, count, infos, alloc, pipelines);
        }
    } else {
        r = dd->create_compute_pipelines(device, cache, count, infos, alloc, pipelines);
    }

    if (r == VK_SUCCESS && dd->prof.enabled)
        for (uint32_t i = 0; i < count; i++) {
            const VkShaderModuleCreateInfo *mi = inline_module(&infos[i]);
            uint64_t ph = mi ? spirv_hash(mi->pCode, mi->codeSize) : module_hash(infos[i].stage.module);
            prof_note_pipeline(dd, pipelines[i],
                               mi ? mi->codeSize : module_size_of(infos[i].stage.module), ph);
        }
    if (codes)
        for (uint32_t i = 0; i < count; i++)
            free(codes[i]);
    free(copy); free(mods); free(codes);
    return r;
}

static struct device_data *cmd_device(VkCommandBuffer cmd, unsigned *slot)
{
    for (unsigned i = 0; i < MAX_CMDBUFS; i++)
        if (g_bound[i].active && g_bound[i].cmd == cmd) {
            *slot = i;
            return g_bound[i].dd;
        }
    return NULL;
}

/* One entry per command buffer that currently has a network shader bound, for profiling. */
static struct {
    VkCommandBuffer cmd;
    struct device_data *dd;
    unsigned pipeline;
    bool active;
} g_prof_bound[MAX_CMDBUFS];

static void prof_bind(struct device_data *dd, VkCommandBuffer cmd, VkPipelineBindPoint point,
                      VkPipeline pipeline)
{
    if (!dd || !dd->prof.enabled)
        return;
    for (unsigned i = 0; i < MAX_CMDBUFS; i++)
        if (g_prof_bound[i].active && g_prof_bound[i].cmd == cmd)
            g_prof_bound[i].active = false;
    if (point != VK_PIPELINE_BIND_POINT_COMPUTE)
        return;
    int idx = prof_index(&dd->prof, pipeline);
    if (idx < 0)
        return;
    for (unsigned i = 0; i < MAX_CMDBUFS; i++)
        if (!g_prof_bound[i].active) {
            g_prof_bound[i].cmd = cmd;
            g_prof_bound[i].dd = dd;
            g_prof_bound[i].pipeline = (unsigned)idx;
            g_prof_bound[i].active = true;
            return;
        }
}

static struct device_data *prof_cmd_device(VkCommandBuffer cmd, unsigned *pipeline_index)
{
    for (unsigned i = 0; i < MAX_CMDBUFS; i++)
        if (g_prof_bound[i].active && g_prof_bound[i].cmd == cmd) {
            *pipeline_index = g_prof_bound[i].pipeline;
            return g_prof_bound[i].dd;
        }
    return NULL;
}

/* The game binds the handle it was given. When that shader is being tuned, bind a variant instead. */
static VKAPI_ATTR void VKAPI_CALL fsr4_CmdBindPipeline(
        VkCommandBuffer cmd, VkPipelineBindPoint bind_point, VkPipeline pipeline)
{
    struct device_data *dd = NULL;
    for (unsigned i = 0; i < MAX_DEVICES; i++)
        if (g_devices[i].device && g_devices[i].tune.enabled) {
            dd = &g_devices[i];
            break;
        }

    /* Pipeline handles are unique per device, so offering it to each device that is profiling
     * lets the right one claim it. vkd3d-proton creates more than one device. */
    for (unsigned i = 0; i < MAX_DEVICES; i++)
        if (g_devices[i].device && g_devices[i].prof.enabled)
            prof_bind(&g_devices[i], cmd, bind_point, pipeline);

    unsigned slot = 0;
    for (unsigned i = 0; i < MAX_CMDBUFS; i++)
        if (g_bound[i].active && g_bound[i].cmd == cmd) {
            g_bound[i].active = false;
            break;
        }

    if (dd && bind_point == VK_PIPELINE_BIND_POINT_COMPUTE) {
        struct tune_shader *sh = tune_find(&dd->tune, pipeline);
        if (sh) {
            unsigned variant = 0;
            VkPipeline use = tune_pick(sh, &variant);
            for (slot = 0; slot < MAX_CMDBUFS; slot++)
                if (!g_bound[slot].active)
                    break;
            if (slot < MAX_CMDBUFS) {
                g_bound[slot].cmd = cmd;
                g_bound[slot].dd = dd;
                g_bound[slot].shader = (unsigned)(sh - dd->tune.shaders);
                g_bound[slot].variant = variant;
                g_bound[slot].active = true;
            }
            dd->cmd_bind_pipeline(cmd, bind_point, use);
            return;
        }
    }

    struct device_data *any = dd;
    if (!any)
        for (unsigned i = 0; i < MAX_DEVICES; i++)
            if (g_devices[i].device) {
                any = &g_devices[i];
                break;
            }
    any->cmd_bind_pipeline(cmd, bind_point, pipeline);
}

/* Brackets a tuned dispatch with timestamps. */
static VKAPI_ATTR void VKAPI_CALL fsr4_CmdDispatch(VkCommandBuffer cmd, uint32_t x, uint32_t y, uint32_t z)
{
    unsigned slot;
    struct device_data *dd = cmd_device(cmd, &slot);
    if (!dd) {
        unsigned pidx = 0;
        struct device_data *pd = prof_cmd_device(cmd, &pidx);
        if (pd) {
            /* A network dispatch, with timestamps around it. */
            int q = prof_take_slot(&pd->prof, pidx);
            if (q >= 0) {
                pd->cmd_reset_query_pool(cmd, pd->prof.pool, q * 2, 2);
                /* Both timestamps are bottom of pipe. A top-of-pipe start is written when the
                 * command reaches the front of the queue, which can be long before it runs, so it
                 * measures waiting as well as work. Completion to completion does not. */
                pd->cmd_write_timestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, pd->prof.pool, q * 2);
                pd->cmd_dispatch(cmd, x, y, z);
                pd->cmd_write_timestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, pd->prof.pool, q * 2 + 1);
                return;
            }
            pd->cmd_dispatch(cmd, x, y, z);
            return;
        }
        for (unsigned i = 0; i < MAX_DEVICES; i++)
            if (g_devices[i].device) {
                g_devices[i].cmd_dispatch(cmd, x, y, z);
                return;
            }
        return;
    }

    struct tune_state *t = &dd->tune;
    struct tune_shader *sh = &t->shaders[g_bound[slot].shader];
    if (sh->chosen >= 0) {
        dd->cmd_dispatch(cmd, x, y, z);
        return;
    }

    pthread_mutex_lock(&g_lock);
    unsigned q = t->next_slot;
    unsigned tries = 0;
    while (t->slots[q].pending && tries++ < TUNE_QUERIES / 2)
        q = (q + 1) % (TUNE_QUERIES / 2);
    if (t->slots[q].pending) {
        pthread_mutex_unlock(&g_lock);
        dd->cmd_dispatch(cmd, x, y, z);      /* no free slot, just run it */
        return;
    }
    t->slots[q].shader = g_bound[slot].shader;
    t->slots[q].variant = g_bound[slot].variant;
    t->slots[q].pending = true;
    t->next_slot = (q + 1) % (TUNE_QUERIES / 2);
    pthread_mutex_unlock(&g_lock);

    dd->cmd_reset_query_pool(cmd, t->pool, q * 2, 2);
    dd->cmd_write_timestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, t->pool, q * 2);
    dd->cmd_dispatch(cmd, x, y, z);
    dd->cmd_write_timestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, t->pool, q * 2 + 1);
}

static VKAPI_ATTR VkResult VKAPI_CALL fsr4_QueueSubmit(
        VkQueue queue, uint32_t count, const VkSubmitInfo *submits, VkFence fence)
{
    struct device_data *dd = NULL;
    for (unsigned i = 0; i < MAX_DEVICES; i++)
        if (g_devices[i].device && g_devices[i].tune.enabled) {
            dd = &g_devices[i];
            break;
        }
    if (!dd)
        for (unsigned i = 0; i < MAX_DEVICES; i++)
            if (g_devices[i].device) {
                dd = &g_devices[i];
                break;
            }

    VkResult r = dd->queue_submit(queue, count, submits, fence);
    if (dd->tune.enabled)
        tune_collect(dd);
    for (unsigned i = 0; i < MAX_DEVICES; i++)
        if (g_devices[i].device && g_devices[i].prof.enabled) {
            prof_collect(&g_devices[i]);
            prof_report(&g_devices[i]);
        }
    return r;
}

/* vkd3d-proton submits through vkQueueSubmit2, so the results have to be collected there as well. */
static VKAPI_ATTR VkResult VKAPI_CALL fsr4_QueueSubmit2(
        VkQueue queue, uint32_t count, const VkSubmitInfo2 *submits, VkFence fence)
{
    struct device_data *dd = NULL;
    for (unsigned i = 0; i < MAX_DEVICES; i++)
        if (g_devices[i].device && g_devices[i].queue_submit2) {
            dd = &g_devices[i];
            break;
        }
    if (!dd)
        return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r = dd->queue_submit2(queue, count, submits, fence);
    for (unsigned i = 0; i < MAX_DEVICES; i++)
        if (g_devices[i].device && g_devices[i].prof.enabled) {
            prof_collect(&g_devices[i]);
            prof_report(&g_devices[i]);
        }
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL fsr4_CreateShaderModule(
        VkDevice device, const VkShaderModuleCreateInfo *info,
        const VkAllocationCallbacks *alloc, VkShaderModule *module)
{
    struct device_data *dd = device_data(device);
    init_config();

    uint64_t h = spirv_hash(info->pCode, info->codeSize);
    size_t size = 0;
    uint32_t *code = load_replacement(h, &size);
    bool expanded = false;
    if (!code && g_expand_sdot) {
        code = spv_expand_sdot(info->pCode, info->codeSize, &size);
        expanded = code != NULL;
    }
    if (g_dump[0])
        dump_module(h, expanded ? code : info->pCode, expanded ? size : info->codeSize);
    if (g_debug)
        fprintf(stderr, "fsr4_layer: module %016lx, %zu bytes%s\n", (unsigned long)h,
                info->codeSize, code ? (expanded ? " -> dot product expanded" : " -> replaced") : "");

    if (code) {
        VkShaderModuleCreateInfo patched = *info;
        patched.pCode = code;
        patched.codeSize = size;
        VkResult r = dd->create_shader_module(device, &patched, alloc, module);
        free(code);
        if (r == VK_SUCCESS) {
            remember_module(*module, h, info->codeSize);
            return r;
        }
        /* A replacement that the driver rejects must never break the game. */
        if (g_debug)
            fprintf(stderr, "fsr4_layer: replacement %016lx rejected, using the original\n", (unsigned long)h);
    }
    VkResult r = dd->create_shader_module(device, info, alloc, module);
    if (r == VK_SUCCESS)
        remember_module(*module, h, info->codeSize);
    return r;
}

static PFN_vkGetInstanceProcAddr g_next_gipa;
static VkInstance g_instance;

static VKAPI_ATTR VkResult VKAPI_CALL fsr4_CreateInstance(
        const VkInstanceCreateInfo *info, const VkAllocationCallbacks *alloc, VkInstance *instance)
{
    VkLayerInstanceCreateInfo *chain = (VkLayerInstanceCreateInfo *)info->pNext;
    while (chain && !(chain->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO &&
                      chain->function == VK_LAYER_LINK_INFO))
        chain = (VkLayerInstanceCreateInfo *)chain->pNext;
    if (!chain)
        return VK_ERROR_INITIALIZATION_FAILED;

    g_next_gipa = chain->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    chain->u.pLayerInfo = chain->u.pLayerInfo->pNext;
    init_config();

    PFN_vkCreateInstance create = (PFN_vkCreateInstance)g_next_gipa(NULL, "vkCreateInstance");
    VkResult r = create(info, alloc, instance);
    if (r == VK_SUCCESS)
        g_instance = *instance;        /* physical-device queries need it */
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL fsr4_CreateDevice(
        VkPhysicalDevice physical_device, const VkDeviceCreateInfo *info,
        const VkAllocationCallbacks *alloc, VkDevice *device)
{
    VkLayerDeviceCreateInfo *chain = (VkLayerDeviceCreateInfo *)info->pNext;
    while (chain && !(chain->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO &&
                      chain->function == VK_LAYER_LINK_INFO))
        chain = (VkLayerDeviceCreateInfo *)chain->pNext;
    if (!chain)
        return VK_ERROR_INITIALIZATION_FAILED;

    PFN_vkGetInstanceProcAddr gipa = chain->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr gdpa = chain->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    chain->u.pLayerInfo = chain->u.pLayerInfo->pNext;

    PFN_vkGetPhysicalDeviceProperties gpdp =
            (PFN_vkGetPhysicalDeviceProperties)gipa(g_instance, "vkGetPhysicalDeviceProperties");
    if (gpdp) {
        VkPhysicalDeviceProperties props;
        gpdp(physical_device, &props);
        g_timestamp_period = props.limits.timestampPeriod;
    }

    /* A card without a packed 4x8 dot product lowers OpSDot in software, which is slower than doing
     * the four multiplies in 32 bits. Ask the device, rather than guessing from the model name. */
    PFN_vkGetPhysicalDeviceProperties2 gpdp2 =
            (PFN_vkGetPhysicalDeviceProperties2)gipa(g_instance, "vkGetPhysicalDeviceProperties2");
    if (gpdp2 && !getenv("FSR4_NO_SDOT_EXPAND")) {
        VkPhysicalDeviceShaderIntegerDotProductProperties dot = {
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_INTEGER_DOT_PRODUCT_PROPERTIES
        };
        VkPhysicalDeviceProperties2 p2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &dot };
        gpdp2(physical_device, &p2);
        g_expand_sdot = !dot.integerDotProduct4x8BitPackedSignedAccelerated;
        if (g_debug)
            fprintf(stderr, "fsr4_layer: packed 4x8 dot product accelerated: %s\n",
                    dot.integerDotProduct4x8BitPackedSignedAccelerated ? "yes" : "no");
    }

    PFN_vkCreateDevice create = (PFN_vkCreateDevice)gipa(NULL, "vkCreateDevice");
    VkResult r = create(physical_device, info, alloc, device);
    if (r != VK_SUCCESS)
        return r;

    pthread_mutex_lock(&g_lock);
    for (unsigned i = 0; i < MAX_DEVICES; i++) {
        if (!g_devices[i].device) {
            g_devices[i].device = *device;
            g_devices[i].get_device_proc = gdpa;
            g_devices[i].create_shader_module = (PFN_vkCreateShaderModule)gdpa(*device, "vkCreateShaderModule");
            g_devices[i].destroy_shader_module = (PFN_vkDestroyShaderModule)gdpa(*device, "vkDestroyShaderModule");
            g_devices[i].create_compute_pipelines = (PFN_vkCreateComputePipelines)gdpa(*device, "vkCreateComputePipelines");
            g_devices[i].destroy_pipeline = (PFN_vkDestroyPipeline)gdpa(*device, "vkDestroyPipeline");
            g_devices[i].create_query_pool = (PFN_vkCreateQueryPool)gdpa(*device, "vkCreateQueryPool");
            g_devices[i].get_query_pool_results = (PFN_vkGetQueryPoolResults)gdpa(*device, "vkGetQueryPoolResults");
            g_devices[i].cmd_bind_pipeline = (PFN_vkCmdBindPipeline)gdpa(*device, "vkCmdBindPipeline");
            g_devices[i].cmd_dispatch = (PFN_vkCmdDispatch)gdpa(*device, "vkCmdDispatch");
            g_devices[i].cmd_reset_query_pool = (PFN_vkCmdResetQueryPool)gdpa(*device, "vkCmdResetQueryPool");
            g_devices[i].cmd_write_timestamp = (PFN_vkCmdWriteTimestamp)gdpa(*device, "vkCmdWriteTimestamp");
            g_devices[i].queue_submit = (PFN_vkQueueSubmit)gdpa(*device, "vkQueueSubmit");
            g_devices[i].queue_submit2 = (PFN_vkQueueSubmit2)gdpa(*device, "vkQueueSubmit2");
            if (!g_devices[i].queue_submit2)
                g_devices[i].queue_submit2 = (PFN_vkQueueSubmit2)gdpa(*device, "vkQueueSubmit2KHR");
            g_devices[i].tune.timestamp_period = g_timestamp_period;
            g_devices[i].prof.timestamp_period = g_timestamp_period;
            tune_init(&g_devices[i]);
            prof_init(&g_devices[i]);
            break;
        }
    }
    pthread_mutex_unlock(&g_lock);
    init_config();
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL fsr4_DestroyDevice(VkDevice device, const VkAllocationCallbacks *alloc)
{
    struct device_data *pd = device_data(device);
    if (pd)
        prof_summary(pd);
    struct device_data *dd = device_data(device);
    PFN_vkDestroyDevice destroy = (PFN_vkDestroyDevice)dd->get_device_proc(device, "vkDestroyDevice");
    pthread_mutex_lock(&g_lock);
    memset(dd, 0, sizeof(*dd));
    pthread_mutex_unlock(&g_lock);
    destroy(device, alloc);
}

#define ENTRY(name) if (!strcmp(pName, "vk" #name)) return (PFN_vkVoidFunction)fsr4_##name;

VK_LAYER_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetDeviceProcAddr(VkDevice device, const char *pName)
{
    ENTRY(CreateShaderModule)
    ENTRY(CreateComputePipelines)
    ENTRY(DestroyDevice)
    /* Both the tuner and the profiler need to see the commands. */
    if (g_autotune || g_profile) {
        ENTRY(CmdBindPipeline)
        ENTRY(CmdDispatch)
        ENTRY(QueueSubmit)
        ENTRY(QueueSubmit2)
    }
    struct device_data *dd = device_data(device);
    return dd ? dd->get_device_proc(device, pName) : NULL;
}

VK_LAYER_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetInstanceProcAddr(VkInstance instance, const char *pName)
{
    ENTRY(CreateInstance)
    ENTRY(CreateDevice)
    ENTRY(CreateShaderModule)
    ENTRY(CreateComputePipelines)
    return g_next_gipa ? g_next_gipa(instance, pName) : NULL;
}
