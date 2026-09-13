// Times one FSR4 shader at its real dispatch size.
// Layout matches the SPIR-V that vkd3d-proton emits for the FSR4 passes. It is bindless: sets 0 and
// 1 are empty, set 2 binding 0 is an array of storage buffers, and two push constant words carry the
// root constants, of which the second is the base index into that array.
// Every descriptor points at the same scratch buffer, which is filled with random int8 bytes. The
// loops in these shaders have fixed bounds, so the timing does not depend on the data.
//
// Usage: bench_layer shader.spv x y z [runs] [scratch_mb]

#define DESCRIPTORS 64
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { fprintf(stderr, "%s failed: %d\n", #x, r_); exit(1); } } while (0)

typedef struct { VkBuffer b; VkDeviceMemory m; void *p; VkDeviceSize n; } Buf;

static VkPhysicalDevice pd;
static VkDevice dev;

static uint32_t memtype(uint32_t bits, VkMemoryPropertyFlags f) {
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & f) == f) return i;
    fprintf(stderr, "no memtype\n"); exit(1);
}

static Buf mkbuf(VkDeviceSize n, VkBufferUsageFlags u) {
    Buf b = { .n = n };
    VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = n, .usage = u };
    CHK(vkCreateBuffer(dev, &bi, NULL, &b.b));
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(dev, b.b, &mr);
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
        .memoryTypeIndex = memtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
    CHK(vkAllocateMemory(dev, &ai, NULL, &b.m));
    CHK(vkBindBufferMemory(dev, b.b, b.m, 0));
    CHK(vkMapMemory(dev, b.m, 0, n, 0, &b.p));
    return b;
}

static int cmpd(const void *a, const void *b) { double x = *(double *)a, y = *(double *)b; return x < y ? -1 : x > y; }

int main(int argc, char **argv) {
    if (argc < 5) { fprintf(stderr, "usage: %s shader.spv x y z [runs] [scratch_mb]\n", argv[0]); return 1; }
    uint32_t gx = atoi(argv[2]), gy = atoi(argv[3]), gz = atoi(argv[4]);
    int runs = argc > 5 ? atoi(argv[5]) : 15;
    VkDeviceSize scratch = (argc > 6 ? atoi(argv[6]) : 96) * 1024ull * 1024ull;

    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_3 };
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
    VkInstance inst; CHK(vkCreateInstance(&ici, NULL, &inst));
    uint32_t n = 1; CHK(vkEnumeratePhysicalDevices(inst, &n, &pd) == VK_INCOMPLETE ? VK_SUCCESS : VK_SUCCESS);
    VkPhysicalDeviceProperties props; vkGetPhysicalDeviceProperties(pd, &props);

    uint32_t qf = 0;
    float prio = 1;
    VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = qf, .queueCount = 1, .pQueuePriorities = &prio };
    // Enable every feature the dxil-spirv output can ask for.
    VkPhysicalDeviceVulkan13Features f13 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
    VkPhysicalDeviceVulkan12Features f12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = &f13 };
    VkPhysicalDeviceVulkan11Features f11 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, .pNext = &f12 };
    VkPhysicalDeviceFeatures2 f2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f11 };
    vkGetPhysicalDeviceFeatures2(pd, &f2);
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &f2, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci };
    CHK(vkCreateDevice(pd, &dci, NULL, &dev));
    VkQueue q; vkGetDeviceQueue(dev, qf, 0, &q);
    fprintf(stderr, "device %s\n", props.deviceName);

    Buf sb = mkbuf(scratch, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    srand(1);
    for (VkDeviceSize i = 0; i < scratch; i++) ((uint8_t *)sb.p)[i] = (uint8_t)(rand() % 255 - 127);

    FILE *f = fopen(argv[1], "rb"); if (!f) { perror(argv[1]); return 1; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint32_t *code = malloc(sz); fread(code, 1, sz, f); fclose(f);
    VkShaderModuleCreateInfo smi = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sz, .pCode = code };
    VkShaderModule mod; CHK(vkCreateShaderModule(dev, &smi, NULL, &mod));

    // Sets 0 and 1 are declared and left empty. The shader only ever touches set 2.
    VkDescriptorSetLayoutCreateInfo eli = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    VkDescriptorSetLayout empty; CHK(vkCreateDescriptorSetLayout(dev, &eli, NULL, &empty));
    VkDescriptorSetLayoutBinding lb = { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, DESCRIPTORS, VK_SHADER_STAGE_COMPUTE_BIT };
    VkDescriptorSetLayoutCreateInfo dli = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &lb };
    VkDescriptorSetLayout dsl; CHK(vkCreateDescriptorSetLayout(dev, &dli, NULL, &dsl));
    VkDescriptorSetLayout sets[3] = { empty, empty, dsl };
    VkPushConstantRange pcr = { VK_SHADER_STAGE_COMPUTE_BIT, 0, 8 };
    VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 3,
        .pSetLayouts = sets, .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr };
    VkPipelineLayout pl; CHK(vkCreatePipelineLayout(dev, &pli, NULL, &pl));
    VkComputePipelineCreateInfo cpi = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = mod, .pName = "main" }, .layout = pl };
    VkPipeline pipe; CHK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpi, NULL, &pipe));

    VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, DESCRIPTORS };
    VkDescriptorPoolCreateInfo dpi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &ps };
    VkDescriptorPool dp; CHK(vkCreateDescriptorPool(dev, &dpi, NULL, &dp));
    VkDescriptorSetAllocateInfo dsa = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = dp, .descriptorSetCount = 1, .pSetLayouts = &dsl };
    VkDescriptorSet ds; CHK(vkAllocateDescriptorSets(dev, &dsa, &ds));
    // Every slot points at the same buffer, so any index the root constants produce is valid.
    VkDescriptorBufferInfo dbi[DESCRIPTORS];
    for (int i = 0; i < DESCRIPTORS; i++) { dbi[i].buffer = sb.b; dbi[i].offset = 0; dbi[i].range = VK_WHOLE_SIZE; }
    VkWriteDescriptorSet w = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 0,
        .descriptorCount = DESCRIPTORS, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = dbi };
    vkUpdateDescriptorSets(dev, 1, &w, 0, NULL);

    VkCommandPoolCreateInfo cpc = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = qf };
    VkCommandPool cp; CHK(vkCreateCommandPool(dev, &cpc, NULL, &cp));
    VkCommandBufferAllocateInfo cba = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = cp, .commandBufferCount = 1 };
    VkCommandBuffer cb; CHK(vkAllocateCommandBuffers(dev, &cba, &cb));
    VkQueryPoolCreateInfo qpi = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = 2 };
    VkQueryPool qp; CHK(vkCreateQueryPool(dev, &qpi, NULL, &qp));

    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    CHK(vkBeginCommandBuffer(cb, &bi));
    vkCmdResetQueryPool(cb, qp, 0, 2);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 2, 1, &ds, 0, NULL);
    uint32_t root[2] = { 0, 0 };
    vkCmdPushConstants(cb, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof root, root);
    vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, qp, 0);
    vkCmdDispatch(cb, gx, gy, gz);
    vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, qp, 1);
    CHK(vkEndCommandBuffer(cb));
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb };

    double *ms = calloc(runs, sizeof(double));
    uint64_t ts[2];
    for (int r = -2; r < runs; r++) {
        CHK(vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE));
        CHK(vkQueueWaitIdle(q));
        CHK(vkGetQueryPoolResults(dev, qp, 0, 2, sizeof ts, ts, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT));
        if (r >= 0) ms[r] = (ts[1] - ts[0]) * props.limits.timestampPeriod / 1e6;
    }
    // DUMP=prefix: write the whole scratch buffer, so a CPU reference can check the result.
    if (getenv("DUMP")) {
        char path[512];
        snprintf(path, sizeof path, "%s.scratch", getenv("DUMP"));
        f = fopen(path, "wb"); fwrite(sb.p, 1, scratch, f); fclose(f);
    }
    qsort(ms, runs, sizeof(double), cmpd);
    printf("%s dispatch=%ux%ux%u min_ms=%.3f med_ms=%.3f\n", argv[1], gx, gy, gz, ms[0], ms[runs / 2]);
    return 0;
}
