/*
 * A Vulkan layer that swaps FSR4's compute shaders for tuned ones.
 *
 * The layer hashes every SPIR-V module the application creates and looks for a replacement in the
 * cache directory. A replacement is used only if it holds a valid SPIR-V header. This is the same
 * idea as VKD3D_SHADER_OVERRIDE, but it sits below the translation layer, so it also covers native
 * Vulkan and any other D3D12 layer.
 *
 * Environment:
 *   FSR4_LAYER_CACHE   directory that holds <spirv-hash>.spv. Default ~/.cache/fsr4_opt/spirv
 *   FSR4_LAYER_DUMP    directory to write every module to, named by its hash. Off by default.
 *   FSR4_LAYER_DEBUG   1 prints one line per module
 *
 * Build: see meson.build. Install the .so and fsr4_layer.json where the loader looks for layers.
 */
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifndef VK_LAYER_EXPORT
#define VK_LAYER_EXPORT __attribute__((visibility("default")))
#endif

#define MAX_DEVICES 8

struct device_data {
    VkDevice device;
    PFN_vkCreateShaderModule create_shader_module;
    PFN_vkDestroyShaderModule destroy_shader_module;
    PFN_vkCreateComputePipelines create_compute_pipelines;
    PFN_vkGetDeviceProcAddr get_device_proc;
};

static struct device_data g_devices[MAX_DEVICES];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static char g_cache[512];
static char g_dump[512];
static int g_debug;

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

    for (uint32_t i = 0; i < count; i++) {
        const VkShaderModuleCreateInfo *m = infos[i].stage.pNext;
        while (m && m->sType != VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO)
            m = m->pNext;
        if (!m)
            continue;

        uint64_t h = spirv_hash(m->pCode, m->codeSize);
        if (g_dump[0])
            dump_module(h, m->pCode, m->codeSize);
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
        if (r == VK_SUCCESS)
            return r;
        /* A replacement that the driver rejects must never break the game. */
        if (g_debug)
            fprintf(stderr, "fsr4_layer: replacement %016lx rejected, using the original\n", (unsigned long)h);
    }
    return dd->create_shader_module(device, info, alloc, module);
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
