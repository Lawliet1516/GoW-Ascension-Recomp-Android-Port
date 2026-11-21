/* What the phone's Vulkan driver offers, for the RSX backend design. Loads libvulkan.so at run time (as the backend does). */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#define L(n) PFN_##n n = (PFN_##n)gipa(inst, #n)
int main(void)
{
    void* lib = dlopen("libvulkan.so", RTLD_NOW); if (!lib) { printf("no libvulkan: %s\n", dlerror()); return 1; }
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
    VkInstance inst = VK_NULL_HANDLE;
    PFN_vkCreateInstance ci = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
    VkApplicationInfo ai = { VK_STRUCTURE_TYPE_APPLICATION_INFO, 0, "probe", 1, "probe", 1, VK_API_VERSION_1_1 };
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, 0, 0, &ai };
    if (ci(&ici, NULL, &inst) != VK_SUCCESS) { printf("vkCreateInstance failed\n"); return 1; }
    L(vkEnumeratePhysicalDevices); L(vkGetPhysicalDeviceProperties); L(vkGetPhysicalDeviceFeatures); L(vkGetPhysicalDeviceMemoryProperties);
    L(vkGetPhysicalDeviceQueueFamilyProperties); L(vkEnumerateDeviceExtensionProperties); L(vkGetPhysicalDeviceFormatProperties);
    uint32_t n = 0; vkEnumeratePhysicalDevices(inst, &n, NULL); VkPhysicalDevice pd[4]; if (n > 4) n = 4; vkEnumeratePhysicalDevices(inst, &n, pd);
    for (uint32_t d = 0; d < n; d++) {
        VkPhysicalDeviceProperties p; vkGetPhysicalDeviceProperties(pd[d], &p);
        printf("device: %s  type=%d  api=%u.%u.%u  driver=0x%x vendor=0x%x\n", p.deviceName, p.deviceType, VK_API_VERSION_MAJOR(p.apiVersion), VK_API_VERSION_MINOR(p.apiVersion), VK_API_VERSION_PATCH(p.apiVersion), p.driverVersion, p.vendorID);
        VkPhysicalDeviceFeatures f; vkGetPhysicalDeviceFeatures(pd[d], &f);
        printf("features: textureCompressionBC=%u ETC2=%u ASTC_LDR=%u samplerAnisotropy=%u shaderInt16=%u shaderFloat64=%u geometryShader=%u tessellation=%u independentBlend=%u depthClamp=%u fillModeNonSolid=%u multiDrawIndirect=%u occlusionQueryPrecise=%u fragmentStoresAndAtomics=%u dualSrcBlend=%u wideLines=%u\n",
               f.textureCompressionBC, f.textureCompressionETC2, f.textureCompressionASTC_LDR, f.samplerAnisotropy, f.shaderInt16, f.shaderFloat64, f.geometryShader, f.tessellationShader, f.independentBlend, f.depthClamp, f.fillModeNonSolid, f.multiDrawIndirect, f.occlusionQueryPrecise, f.fragmentStoresAndAtomics, f.dualSrcBlend, f.wideLines);
        VkPhysicalDeviceLimits* l = &p.limits;
        printf("limits: maxImageDimension2D=%u maxUniformBufferRange=%u maxStorageBufferRange=%u maxPushConstants=%u maxBoundDescriptorSets=%u maxPerStageSamplers=%u maxColorAttachments=%u maxFragmentInputComponents=%u maxVertexInputAttributes=%u maxSamplerAnisotropy=%.0f nonCoherentAtomSize=%llu minUniformBufferOffsetAlignment=%llu timestampPeriod=%.1f\n",
               l->maxImageDimension2D, l->maxUniformBufferRange, l->maxStorageBufferRange, l->maxPushConstantsSize, l->maxBoundDescriptorSets, l->maxPerStageDescriptorSamplers, l->maxColorAttachments, l->maxFragmentInputComponents, l->maxVertexInputAttributes, l->maxSamplerAnisotropy, (unsigned long long)l->nonCoherentAtomSize, (unsigned long long)l->minUniformBufferOffsetAlignment, l->timestampPeriod);
        VkPhysicalDeviceMemoryProperties m; vkGetPhysicalDeviceMemoryProperties(pd[d], &m);
        for (uint32_t i = 0; i < m.memoryHeapCount; i++) printf("heap %u: %.0f MB flags=0x%x\n", i, m.memoryHeaps[i].size / 1048576.0, m.memoryHeaps[i].flags);
        for (uint32_t i = 0; i < m.memoryTypeCount; i++) printf("  type %u heap %u props=0x%x%s%s%s%s\n", i, m.memoryTypes[i].heapIndex, m.memoryTypes[i].propertyFlags,
               (m.memoryTypes[i].propertyFlags & 1) ? " DEVICE_LOCAL" : "", (m.memoryTypes[i].propertyFlags & 2) ? " HOST_VISIBLE" : "", (m.memoryTypes[i].propertyFlags & 4) ? " HOST_COHERENT" : "", (m.memoryTypes[i].propertyFlags & 8) ? " HOST_CACHED" : "");
        uint32_t q = 0; vkGetPhysicalDeviceQueueFamilyProperties(pd[d], &q, NULL); VkQueueFamilyProperties qp[8]; if (q > 8) q = 8; vkGetPhysicalDeviceQueueFamilyProperties(pd[d], &q, qp);
        for (uint32_t i = 0; i < q; i++) printf("queue family %u: count=%u flags=0x%x%s%s%s\n", i, qp[i].queueCount, qp[i].queueFlags, (qp[i].queueFlags & 1) ? " GRAPHICS" : "", (qp[i].queueFlags & 2) ? " COMPUTE" : "", (qp[i].queueFlags & 4) ? " TRANSFER" : "");
        static const struct { VkFormat f; const char* n; } fm[] = { {VK_FORMAT_BC1_RGBA_UNORM_BLOCK,"BC1"}, {VK_FORMAT_BC3_UNORM_BLOCK,"BC3"}, {VK_FORMAT_BC7_UNORM_BLOCK,"BC7"}, {VK_FORMAT_D24_UNORM_S8_UINT,"D24S8"}, {VK_FORMAT_D32_SFLOAT_S8_UINT,"D32S8"}, {VK_FORMAT_B8G8R8A8_UNORM,"BGRA8"}, {VK_FORMAT_R16G16B16A16_SFLOAT,"RGBA16F"}, {VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK,"ETC2"}, {VK_FORMAT_ASTC_4x4_UNORM_BLOCK,"ASTC4x4"} };
        printf("sampled-image support:"); for (unsigned i = 0; i < sizeof fm / sizeof *fm; i++) { VkFormatProperties fp; vkGetPhysicalDeviceFormatProperties(pd[d], fm[i].f, &fp); printf(" %s=%s", fm[i].n, (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) ? "yes" : "NO"); } printf("\n");
        uint32_t ne = 0; vkEnumerateDeviceExtensionProperties(pd[d], NULL, &ne, NULL); VkExtensionProperties* ex = malloc(ne * sizeof *ex); vkEnumerateDeviceExtensionProperties(pd[d], NULL, &ne, ex);
        printf("%u device extensions; of interest:", ne); const char* want[] = { "VK_KHR_swapchain", "VK_KHR_timeline_semaphore", "VK_KHR_dynamic_rendering", "VK_KHR_synchronization2", "VK_EXT_extended_dynamic_state", "VK_KHR_push_descriptor", "VK_EXT_descriptor_indexing", "VK_KHR_16bit_storage", "VK_KHR_shader_float16_int8", "VK_QCOM_render_pass_transform", "VK_EXT_depth_clip_enable", "VK_KHR_draw_indirect_count", "VK_EXT_shader_demote_to_helper_invocation", "VK_EXT_conservative_rasterization", "VK_KHR_driver_properties", "VK_EXT_pipeline_creation_cache_control", "VK_KHR_pipeline_executable_properties", "VK_KHR_depth_stencil_resolve", "VK_KHR_create_renderpass2", "VK_EXT_queue_family_foreign", "VK_ANDROID_external_memory_android_hardware_buffer", "VK_EXT_fragment_density_map" };
        for (unsigned w = 0; w < sizeof want / sizeof *want; w++) { int has = 0; for (uint32_t i = 0; i < ne; i++) if (!strcmp(ex[i].extensionName, want[w])) has = 1; printf(" %s%s", want[w] + 3, has ? "+" : "-"); } printf("\n");
    }
    return 0;
}
