/*
 * Measuring how long FSR4's network really takes on the GPU.
 *
 * A frametime says what the whole game did. The number that matters here is the GPU time of the
 * upscaler alone, and the only honest way to get it is to bracket the real dispatches with
 * timestamps. This does that, for whichever FSR4 build is loaded, without the game or the DLL
 * knowing.
 *
 * A network shader is recognised by size. FSR4's passes compile to hundreds of kilobytes of SPIR-V,
 * where a game's own compute shaders are a few kilobytes, so a threshold separates them cleanly.
 * FSR4_PROFILE_MIN moves the threshold.
 *
 * The layer cannot see the game's frames, so the report is per second of wall time. Divide by the
 * frame rate to get the cost per frame.
 *
 * Environment:
 *   FSR4_PROFILE       1 turns this on.
 *   FSR4_PROFILE_MIN   module size in bytes that counts as a network shader. Default 40000.
 *   FSR4_PROFILE_EVERY seconds between report lines. Default 5.
 */

#ifndef FSR4_PROFILE_H
#define FSR4_PROFILE_H

#include <stdbool.h>
#include <time.h>

#define PROF_QUERIES 512
#define PROF_MAX_PIPELINES 4096

struct prof_state {
    bool enabled;
    size_t min_size;
    double report_every;
    float timestamp_period;
    VkQueryPool pool;

    /* Pipelines built from a module big enough to be a network shader. */
    VkPipeline tracked[PROF_MAX_PIPELINES];
    unsigned long tracked_runs[PROF_MAX_PIPELINES];
    unsigned tracked_count;

    struct {
        bool pending;
        unsigned pipeline;
    } slots[PROF_QUERIES / 2];
    unsigned next_slot;

    double total_ms;                 /* GPU time in tracked dispatches */
    unsigned long dispatches;
    double started;
    double last_report;
    /* The previous report, so each line covers one interval rather than the whole run. */
    double last_ms;
    unsigned long last_dispatches;
};


#endif
