/* The measuring itself. profile.h carries the state and says what this is for. */

static double prof_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void prof_init(struct device_data *dd)
{
    struct prof_state *p = &dd->prof;
    p->enabled = getenv("FSR4_PROFILE") != NULL;
    if (!p->enabled)
        return;

    const char *m = getenv("FSR4_PROFILE_MIN");
    p->min_size = m ? (size_t)strtoul(m, NULL, 10) : 40000;
    const char *e = getenv("FSR4_PROFILE_EVERY");
    p->report_every = e ? atof(e) : 5.0;

    VkQueryPoolCreateInfo qi = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
    qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
    qi.queryCount = PROF_QUERIES;
    if (dd->create_query_pool(dd->device, &qi, NULL, &p->pool) != VK_SUCCESS) {
        fprintf(stderr, "fsr4_layer: no timestamp queries, profiling is off\n");
        p->enabled = false;
        return;
    }
    p->started = p->last_report = prof_now();
    fprintf(stderr, "fsr4_layer: profiling dispatches of shaders over %zu bytes\n", p->min_size);
}

/* Called for every compute pipeline the layer sees created. */
static void prof_note_pipeline(struct device_data *dd, VkPipeline pipeline, size_t module_size)
{
    struct prof_state *p = &dd->prof;
    if (!p->enabled || pipeline == VK_NULL_HANDLE)
        return;
    if (module_size < p->min_size)
        return;
    pthread_mutex_lock(&g_lock);
    if (p->tracked_count < PROF_MAX_PIPELINES)
        p->tracked[p->tracked_count++] = pipeline;
    pthread_mutex_unlock(&g_lock);
}

/* The index of a tracked pipeline, or -1. */
static int prof_index(struct prof_state *p, VkPipeline pipeline)
{
    for (unsigned i = 0; i < p->tracked_count; i++)
        if (p->tracked[i] == pipeline)
            return (int)i;
    return -1;
}

static int prof_take_slot(struct prof_state *p, unsigned pipeline_index)
{
    pthread_mutex_lock(&g_lock);
    unsigned q = p->next_slot;
    unsigned tries = 0;
    while (p->slots[q].pending && tries++ < PROF_QUERIES / 2)
        q = (q + 1) % (PROF_QUERIES / 2);
    if (p->slots[q].pending) {
        pthread_mutex_unlock(&g_lock);
        return -1;
    }
    p->slots[q].pending = true;
    p->slots[q].pipeline = pipeline_index;
    p->tracked_runs[pipeline_index]++;
    p->next_slot = (q + 1) % (PROF_QUERIES / 2);
    pthread_mutex_unlock(&g_lock);
    return (int)q;
}

/* Reads whatever has finished. This never waits on the GPU. */
static void prof_collect(struct device_data *dd)
{
    struct prof_state *p = &dd->prof;
    for (unsigned i = 0; i < PROF_QUERIES / 2; i++) {
        if (!p->slots[i].pending)
            continue;
        uint64_t ts[2];
        if (dd->get_query_pool_results(dd->device, p->pool, i * 2, 2, sizeof(ts), ts,
                                       sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) != VK_SUCCESS)
            continue;
        p->slots[i].pending = false;
        if (ts[1] > ts[0]) {
            p->total_ms += (double)(ts[1] - ts[0]) * p->timestamp_period / 1e6;
            p->dispatches++;
        }
    }
}

/* One line per interval: how much GPU time the upscaler took, per frame. */
static void prof_report(struct device_data *dd)
{
    struct prof_state *p = &dd->prof;
    double now = prof_now();
    if (now - p->last_report < p->report_every)
        return;

    double ms = p->total_ms - p->last_ms;
    unsigned long disp = p->dispatches - p->last_dispatches;
    double secs = now - p->last_report;

    /* Per second of wall time, because the layer cannot see the game's frames. Divide by the
     * frame rate to get the cost per frame. */
    fprintf(stderr, "fsr4_layer: upscaler %.1f ms/s over %.1f s, %.0f dispatches/s\n",
            ms / secs, secs, disp / secs);

    p->last_report = now;
    p->last_ms = p->total_ms;
    p->last_dispatches = p->dispatches;
}

static void prof_summary(struct device_data *dd)
{
    struct prof_state *p = &dd->prof;
    if (!p->enabled)
        return;
    double secs = prof_now() - p->started;
    if (!p->dispatches || secs <= 0)
        return;
    fprintf(stderr, "fsr4_layer: total upscaler %.1f ms/s over %.1f s, %lu dispatches, "
                    "%u network pipelines\n",
            p->total_ms / secs, secs, p->dispatches, p->tracked_count);
}
