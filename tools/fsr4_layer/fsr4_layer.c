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
 *   FSR4_LAYER_CACHE   directory that holds <spirv-hash>.spv. Default ~/.cache/fsr4_opt/spirv
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
    PFN_vkGetDeviceProcAddr get_device_proc;
    struct tune_state tune;
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
} g_module_hash[MAX_MODULES];
static unsigned g_module_count;

static void remember_module(VkShaderModule module, uint64_t hash)
{
    pthread_mutex_lock(&g_lock);
    unsigned i = g_module_count % MAX_MODULES;
    g_module_hash[i].module = module;
    g_module_hash[i].hash = hash;
    g_module_count++;
    pthread_mutex_unlock(&g_lock);
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
static char g_dump[512];
static int g_debug;
static int g_autotune;
static float g_timestamp_period = 1.0f;

static void init_config(void)
{
    static int done;
    if (done)
        return;
    done = 1;
    const char *c = getenv("FSR4_LAYER_CACHE");
    if (c)
        snprintf(g_cache, sizeof(g_cache), "%s", c);
    else {
        const char *home = getenv("HOME");
        snprintf(g_cache, sizeof(g_cache), "%s/.cache/fsr4_opt/spirv", home ? home : "/tmp");
    }
    const char *d = getenv("FSR4_LAYER_DUMP");
    if (d) {
        snprintf(g_dump, sizeof(g_dump), "%s", d);
        mkdir(g_dump, 0755);
    }
    g_debug = getenv("FSR4_LAYER_DEBUG") != NULL;
    g_autotune = getenv("FSR4_AUTOTUNE") != NULL;
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

/* Reads a replacement module. Returns NULL when there is none. */
static uint32_t *load_replacement(uint64_t hash, size_t *out_size)
{
    char path[700];
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
        if (g_dump[0])
            dump_module(h, m->pCode, m->codeSize);

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
        if (g_debug)
            fprintf(stderr, "fsr4_layer: pipeline module %016lx, %zu bytes%s\n", (unsigned long)h,
                    m->codeSize, code ? " -> replaced" : "");
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
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL fsr4_CreateShaderModule(
        VkDevice device, const VkShaderModuleCreateInfo *info,
        const VkAllocationCallbacks *alloc, VkShaderModule *module)
{
    struct device_data *dd = device_data(device);
    init_config();

    uint64_t h = spirv_hash(info->pCode, info->codeSize);
    if (g_dump[0])
        dump_module(h, info->pCode, info->codeSize);

    size_t size = 0;
    uint32_t *code = load_replacement(h, &size);
    if (g_debug)
        fprintf(stderr, "fsr4_layer: module %016lx, %zu bytes%s\n", (unsigned long)h,
                info->codeSize, code ? " -> replaced" : "");

    if (code) {
        VkShaderModuleCreateInfo patched = *info;
        patched.pCode = code;
        patched.codeSize = size;
        VkResult r = dd->create_shader_module(device, &patched, alloc, module);
        free(code);
        if (r == VK_SUCCESS) {
            remember_module(*module, h);
            return r;
        }
        /* A replacement that the driver rejects must never break the game. */
        if (g_debug)
            fprintf(stderr, "fsr4_layer: replacement %016lx rejected, using the original\n", (unsigned long)h);
    }
    VkResult r = dd->create_shader_module(device, info, alloc, module);
    if (r == VK_SUCCESS)
        remember_module(*module, h);
    return r;
}

static PFN_vkGetInstanceProcAddr g_next_gipa;

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
    return create(info, alloc, instance);
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
            (PFN_vkGetPhysicalDeviceProperties)gipa(NULL, "vkGetPhysicalDeviceProperties");
    if (gpdp) {
        VkPhysicalDeviceProperties props;
        gpdp(physical_device, &props);
        g_timestamp_period = props.limits.timestampPeriod;
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
            g_devices[i].tune.timestamp_period = g_timestamp_period;
            tune_init(&g_devices[i]);
            break;
        }
    }
    pthread_mutex_unlock(&g_lock);
    init_config();
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL fsr4_DestroyDevice(VkDevice device, const VkAllocationCallbacks *alloc)
{
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
    if (g_autotune) {
        ENTRY(CmdBindPipeline)
        ENTRY(CmdDispatch)
        ENTRY(QueueSubmit)
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
