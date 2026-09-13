/*
 * Picking the fastest variant of a shader, on the card the game is running on.
 *
 * The layer knows several rewrites of the same FSR4 shader. Which one wins depends on the card: on a
 * Vega 56 the packed pass9 beats FSR4's own, and on Polaris it loses. Rather than trusting a table,
 * this measures the real dispatches.
 *
 * For a shader with variants, every variant gets its own pipeline at creation time, and the game
 * receives the handle of the first one. The layer substitutes another handle at bind time, so it can
 * put any variant into the frame without the game noticing. Dispatches are bracketed by timestamps,
 * the results are collected on later submits, and once every variant has enough samples the fastest
 * one is kept for the rest of the run and written to a small text cache. A later launch reads the
 * cache and skips the search.
 *
 * The measurement is of the real workload, so it needs no fake descriptors and no dummy resources.
 *
 * Environment:
 *   FSR4_AUTOTUNE     1 turns this on. Off by default.
 *   FSR4_VARIANTS     directory holding <variant name>/<spirv hash>.spv. Default: the sets
 *                     directory next to the cache.
 *   FSR4_TUNE_CACHE   file that records the choice per shader. Default ~/.cache/fsr4_opt/choice.txt
 *   FSR4_TUNE_SAMPLES dispatches to time per variant before deciding. Default 60.
 */
#ifndef FSR4_AUTOTUNE_H
#define FSR4_AUTOTUNE_H

#include <dirent.h>
#include <stdbool.h>

#define TUNE_MAX_VARIANTS 8
#define TUNE_MAX_SHADERS 64
#define TUNE_QUERIES 256

struct tune_variant {
    char name[32];
    VkPipeline pipeline;
    double total_ms;
    unsigned samples;
    bool usable;
};

struct tune_shader {
    uint64_t hash;                       /* hash of the original module */
    VkPipeline app_handle;               /* what the game holds */
    struct tune_variant variants[TUNE_MAX_VARIANTS];
    unsigned count;
    unsigned current;                    /* variant being measured */
    int chosen;                          /* -1 while still searching */
};

struct tune_state {
    bool enabled;
    char variants_dir[512];
    char cache_file[512];
    unsigned samples_wanted;
    struct tune_shader shaders[TUNE_MAX_SHADERS];
    unsigned shader_count;

    VkQueryPool pool;
    float timestamp_period;
    /* Each dispatch takes two slots. A slot is free again once its result has been read. */
    struct {
        unsigned shader;
        unsigned variant;
        bool pending;
    } slots[TUNE_QUERIES / 2];
    unsigned next_slot;
};

#endif
