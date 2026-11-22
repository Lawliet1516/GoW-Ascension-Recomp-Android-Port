/* Minimal, independent Vulkan repro for gl_ClipDistance. No RSX/Ascension code: hand-written GLSL (shaders/ (clip*.vert)) compiled by glslc.
 * Offscreen 64x64 RGBA8, clear = blue (0,0,255), the quad is drawn solid red; the result is read back and judged from the pixels.
 * Cases per vertex shader variant: A all distances +1 (all red), B all -1 (nothing red), C d = -x (the left half red, boundary at column 32),
 * plus a control (quad outside NDC: nothing red) that proves the harness can see "no triangle".
 * Output: device/feature evidence, then one PASS/FAIL line per case. Exit code = number of FAILs. */
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <fcntl.h>
#include <unistd.h>

static const uint32_t spv_clip1[]    = 
#include "clip1.vert.inc"
;
static const uint32_t spv_clip4all[] = 
#include "clip4all.vert.inc"
;
static const uint32_t spv_clip4rsx[] = 
#include "clip4rsx.vert.inc"
;
static const uint32_t spv_hlslmin[]  =
#include "hlsl_min.vert.inc"
;
static const uint32_t spv_hlslrsx[]  =
#include "hlsl_rsx.vert.inc"
;
static const uint32_t spv_asm_block[]     = 
#include "asm_block.inc"
;
static const uint32_t spv_asm_clipsolo[]  = 
#include "asm_clip_solo.inc"
;
static const uint32_t spv_asm_possolo[]   = 
#include "asm_pos_solo.inc"
;
static const uint32_t spv_asm_bothsolo[]  = 
#include "asm_both_solo.inc"
;
static const uint32_t spv_asm_bothsoloarr[] =
#include "asm_both_solo_arr.inc"
;
static const uint32_t spv_fix_hlslmin[]   =
#include "fix_hlsl_min.vert.inc"
;
static const uint32_t spv_fix_hlslrsx[]   =
#include "fix_hlsl_rsx.vert.inc"
;
static const uint32_t spv_fix_bothsolo[]  =
#include "fix_asm_both_solo.inc"
;
static const uint32_t spv_fix_bothsoloarr[] =
#include "fix_asm_both_solo_arr.inc"
;
static const uint32_t spv_hlslsix[]       =
#include "hlsl_six.vert.inc"
;
static const uint32_t spv_fix_hlslsix[]   =
#include "fix_hlsl_six.vert.inc"
;
static const uint32_t spv_frag[]     = 
#include "solid.frag.inc"
;

#define W 64
#define H 64
#define CK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { printf("FATAL %s = %d (line %d)\n", #x, (int)r_, __LINE__); return 100; } } while (0)

typedef struct { float x, y, d; } Vtx;
typedef struct { const char* name; const uint32_t* code; size_t bytes; } VsVariant;
typedef struct { const char* name; float d[4]; float xoff; int expect; /* 0 none, 1 all, 2 left half */ } Case;

static uint32_t find_mem(VkPhysicalDeviceMemoryProperties* mp, uint32_t bits, VkMemoryPropertyFlags want)
{
    for (uint32_t i = 0; i < mp->memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp->memoryTypes[i].propertyFlags & want) == want) return i;
    return 0xFFFFFFFFu;
}

static int clip_main(void)
{
    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "clip_repro", .apiVersion = VK_API_VERSION_1_1 };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
    VkInstance inst; CK(vkCreateInstance(&ici, NULL, &inst));
    uint32_t n = 1; VkPhysicalDevice phys; VkResult er = vkEnumeratePhysicalDevices(inst, &n, &phys);
    if (er != VK_SUCCESS && er != VK_INCOMPLETE) { printf("FATAL enumerate %d\n", (int)er); return 100; }
    VkPhysicalDeviceProperties pr; vkGetPhysicalDeviceProperties(phys, &pr);
    VkPhysicalDeviceFeatures feat; vkGetPhysicalDeviceFeatures(phys, &feat);
    printf("EVIDENCE device: %s | vendorID 0x%04x deviceID 0x%08x | apiVersion %u.%u.%u | driverVersion 0x%08x (%u)\n", pr.deviceName, pr.vendorID, pr.deviceID,
           VK_VERSION_MAJOR(pr.apiVersion), VK_VERSION_MINOR(pr.apiVersion), VK_VERSION_PATCH(pr.apiVersion), pr.driverVersion, pr.driverVersion);
    printf("EVIDENCE VkPhysicalDeviceFeatures.shaderClipDistance = %u, shaderCullDistance = %u\n", feat.shaderClipDistance, feat.shaderCullDistance);
    printf("EVIDENCE limits.maxClipDistances = %u, maxCullDistances = %u, maxCombinedClipAndCullDistances = %u\n",
           pr.limits.maxClipDistances, pr.limits.maxCullDistances, pr.limits.maxCombinedClipAndCullDistances);
    if (!feat.shaderClipDistance) { printf("FATAL the device does not report shaderClipDistance\n"); return 100; }

    uint32_t nq = 0; vkGetPhysicalDeviceQueueFamilyProperties(phys, &nq, NULL);
    VkQueueFamilyProperties qp[16]; if (nq > 16) nq = 16; vkGetPhysicalDeviceQueueFamilyProperties(phys, &nq, qp);
    uint32_t qf = 0xFFFFFFFFu; for (uint32_t i = 0; i < nq; i++) if (qp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { qf = i; break; }
    if (qf == 0xFFFFFFFFu) { printf("FATAL no graphics queue\n"); return 100; }
    float prio = 1.0f; VkDeviceQueueCreateInfo dq = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = qf, .queueCount = 1, .pQueuePriorities = &prio };
    VkPhysicalDeviceFeatures enabled; memset(&enabled, 0, sizeof enabled); enabled.shaderClipDistance = VK_TRUE;
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &dq, .pEnabledFeatures = &enabled };
    printf("EVIDENCE VkDeviceCreateInfo.pEnabledFeatures->shaderClipDistance = %u (the exact struct passed to vkCreateDevice)\n", dci.pEnabledFeatures->shaderClipDistance);
    VkDevice dev; CK(vkCreateDevice(phys, &dci, NULL, &dev));
    VkQueue q; vkGetDeviceQueue(dev, qf, 0, &q);
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(phys, &mp);

    /* colour target + readback buffer */
    VkImageCreateInfo imi = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = { W, H, 1 },
        .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
    VkImage img; CK(vkCreateImage(dev, &imi, NULL, &img));
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(dev, img, &mr);
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = find_mem(&mp, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
    VkDeviceMemory imem; CK(vkAllocateMemory(dev, &mai, NULL, &imem)); CK(vkBindImageMemory(dev, img, imem, 0));
    VkImageViewCreateInfo vci = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    VkImageView view; CK(vkCreateImageView(dev, &vci, NULL, &view));
    VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = W * H * 4, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT };
    VkBuffer rb; CK(vkCreateBuffer(dev, &bci, NULL, &rb)); vkGetBufferMemoryRequirements(dev, rb, &mr);
    mai.allocationSize = mr.size; mai.memoryTypeIndex = find_mem(&mp, mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkDeviceMemory rmem; CK(vkAllocateMemory(dev, &mai, NULL, &rmem)); CK(vkBindBufferMemory(dev, rb, rmem, 0));
    uint8_t* rmap; CK(vkMapMemory(dev, rmem, 0, VK_WHOLE_SIZE, 0, (void**)&rmap));
    VkBufferCreateInfo vbi = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 4 * sizeof(Vtx), .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT };
    VkBuffer vb; CK(vkCreateBuffer(dev, &vbi, NULL, &vb)); vkGetBufferMemoryRequirements(dev, vb, &mr);
    mai.allocationSize = mr.size; mai.memoryTypeIndex = find_mem(&mp, mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkDeviceMemory vmem; CK(vkAllocateMemory(dev, &mai, NULL, &vmem)); CK(vkBindBufferMemory(dev, vb, vmem, 0));
    Vtx* vmap; CK(vkMapMemory(dev, vmem, 0, VK_WHOLE_SIZE, 0, (void**)&vmap));

    VkAttachmentDescription ad = { .format = VK_FORMAT_R8G8B8A8_UNORM, .samples = VK_SAMPLE_COUNT_1_BIT, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, .finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL };
    VkAttachmentReference ar = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sp = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = 1, .pColorAttachments = &ar };
    VkRenderPassCreateInfo rpi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 1, .pAttachments = &ad, .subpassCount = 1, .pSubpasses = &sp };
    VkRenderPass rp; CK(vkCreateRenderPass(dev, &rpi, NULL, &rp));
    VkFramebufferCreateInfo fbi = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = rp, .attachmentCount = 1, .pAttachments = &view, .width = W, .height = H, .layers = 1 };
    VkFramebuffer fb; CK(vkCreateFramebuffer(dev, &fbi, NULL, &fb));
    VkPipelineLayoutCreateInfo pli = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    VkPipelineLayout pl; CK(vkCreatePipelineLayout(dev, &pli, NULL, &pl));
    VkCommandPoolCreateInfo cpi = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = qf, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT };
    VkCommandPool pool; CK(vkCreateCommandPool(dev, &cpi, NULL, &pool));
    VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    VkCommandBuffer cb; CK(vkAllocateCommandBuffers(dev, &cai, &cb));
    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFence fence; CK(vkCreateFence(dev, &fci, NULL, &fence));

    VkShaderModule fsm; { VkShaderModuleCreateInfo s = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof spv_frag, .pCode = spv_frag }; CK(vkCreateShaderModule(dev, &s, NULL, &fsm)); }

    const VsVariant vs[] = { { "clip1 (float[1])", spv_clip1, sizeof spv_clip1 }, { "clip4all (float[4], all = d)", spv_clip4all, sizeof spv_clip4all },
                             { "clip4rsx (float[4] = 0.5,d,0.5,0.5)", spv_clip4rsx, sizeof spv_clip4rsx },
                             { "HLSL minimal (pos + float4 SV_ClipDistance0)", spv_hlslmin, sizeof spv_hlslmin },
                             { "HLSL rsx-shaped output struct (14 varyings + clipd0)", spv_hlslrsx, sizeof spv_hlslrsx },
                             { "asm block: Position+ClipDistance in a Block", spv_asm_block, sizeof spv_asm_block },
                             { "asm clip_solo: Position in Block, ClipDistance standalone", spv_asm_clipsolo, sizeof spv_asm_clipsolo },
                             { "asm pos_solo: ClipDistance in Block, Position standalone", spv_asm_possolo, sizeof spv_asm_possolo },
                             { "asm both_solo: both standalone, direct stores", spv_asm_bothsolo, sizeof spv_asm_bothsolo },
                             { "asm both_solo_arr: both standalone, whole-array store", spv_asm_bothsoloarr, sizeof spv_asm_bothsoloarr },
                             { "HLSL six distances, only plane 5 tested (float[6])", spv_hlslsix, sizeof spv_hlslsix },
                             { "FIXED HLSL six distances (post-pass)", spv_fix_hlslsix, sizeof spv_fix_hlslsix },
                             { "FIXED HLSL minimal (rsx_spv_clipblock post-pass)", spv_fix_hlslmin, sizeof spv_fix_hlslmin },
                             { "FIXED HLSL rsx-shaped (post-pass)", spv_fix_hlslrsx, sizeof spv_fix_hlslrsx },
                             { "FIXED asm both_solo (post-pass)", spv_fix_bothsolo, sizeof spv_fix_bothsolo },
                             { "FIXED asm both_solo_arr, whole-array store (post-pass)", spv_fix_bothsoloarr, sizeof spv_fix_bothsoloarr } };
    /* d[0..3]: distance at the vertices (-1,-1) (1,-1) (-1,1) (1,1) */
    const Case cases[] = {
        { "A all +1.0",             { 1, 1, 1, 1 },     0.0f, 1 },
        { "B all -1.0",             { -1, -1, -1, -1 }, 0.0f, 0 },
        { "C d=-x (left half kept)", { 1, -1, 1, -1 },  0.0f, 2 },
        { "control: quad outside NDC", { 1, 1, 1, 1 },  4.0f, 0 },
    };
    int fails = 0, total = 0;
    for (size_t vi = 0; vi < sizeof vs / sizeof vs[0]; vi++) {
        VkShaderModule vsm; VkShaderModuleCreateInfo s = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = vs[vi].bytes, .pCode = vs[vi].code };
        CK(vkCreateShaderModule(dev, &s, NULL, &vsm));
        VkPipelineShaderStageCreateInfo st[2] = {
            { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vsm, .pName = "main" },
            { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fsm, .pName = "main" } };
        VkVertexInputBindingDescription bd = { 0, sizeof(Vtx), VK_VERTEX_INPUT_RATE_VERTEX };
        VkVertexInputAttributeDescription at[2] = { { 0, 0, VK_FORMAT_R32G32_SFLOAT, 0 }, { 1, 0, VK_FORMAT_R32_SFLOAT, 8 } };
        VkPipelineVertexInputStateCreateInfo vin = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, .vertexBindingDescriptionCount = 1, .pVertexBindingDescriptions = &bd,
            .vertexAttributeDescriptionCount = 2, .pVertexAttributeDescriptions = at };
        VkPipelineInputAssemblyStateCreateInfo ia = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP };
        VkViewport vp = { 0, 0, W, H, 0, 1 }; VkRect2D sc = { { 0, 0 }, { W, H } };
        VkPipelineViewportStateCreateInfo vps = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .pViewports = &vp, .scissorCount = 1, .pScissors = &sc };
        VkPipelineRasterizationStateCreateInfo rs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE, .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f };
        VkPipelineMultisampleStateCreateInfo ms = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
        VkPipelineColorBlendAttachmentState cba = { .colorWriteMask = 0xF };
        VkPipelineColorBlendStateCreateInfo cbs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1, .pAttachments = &cba };
        VkGraphicsPipelineCreateInfo gp = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = 2, .pStages = st, .pVertexInputState = &vin, .pInputAssemblyState = &ia,
            .pViewportState = &vps, .pRasterizationState = &rs, .pMultisampleState = &ms, .pColorBlendState = &cbs, .layout = pl, .renderPass = rp };
        VkPipeline pipe; CK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gp, NULL, &pipe));
        printf("VARIANT %s\n", vs[vi].name);
        for (size_t ci = 0; ci < sizeof cases / sizeof cases[0]; ci++) {
            const Case* c = &cases[ci];
            const float px[4] = { -1, 1, -1, 1 }, py[4] = { -1, -1, 1, 1 };
            for (int k = 0; k < 4; k++) { vmap[k].x = px[k] + c->xoff; vmap[k].y = py[k]; vmap[k].d = c->d[k]; }
            printf("  WRITTEN %-28s vertices (x,y,d): (%.1f,%.1f,%.1f) (%.1f,%.1f,%.1f) (%.1f,%.1f,%.1f) (%.1f,%.1f,%.1f)\n", c->name,
                   vmap[0].x, vmap[0].y, vmap[0].d, vmap[1].x, vmap[1].y, vmap[1].d, vmap[2].x, vmap[2].y, vmap[2].d, vmap[3].x, vmap[3].y, vmap[3].d);
            VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
            CK(vkResetCommandBuffer(cb, 0)); CK(vkBeginCommandBuffer(cb, &bi));
            VkClearValue cv = { .color = { { 0, 0, 1, 1 } } };
            VkRenderPassBeginInfo rbi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = rp, .framebuffer = fb, .renderArea = { { 0, 0 }, { W, H } }, .clearValueCount = 1, .pClearValues = &cv };
            vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE); vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
            VkDeviceSize off = 0; vkCmdBindVertexBuffers(cb, 0, 1, &vb, &off); vkCmdDraw(cb, 4, 1, 0, 0); vkCmdEndRenderPass(cb);
            VkBufferImageCopy bc = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { W, H, 1 } };
            vkCmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb, 1, &bc);
            VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT };
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
            CK(vkEndCommandBuffer(cb));
            VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb };
            CK(vkResetFences(dev, 1, &fence)); CK(vkQueueSubmit(q, 1, &si, fence)); CK(vkWaitForFences(dev, 1, &fence, VK_TRUE, 5000000000ull));
            int red = 0, blue = 0, other = 0, firstnon = -1;
            for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
                const uint8_t* p = rmap + (y * W + x) * 4;
                if (p[0] == 255 && p[1] == 0 && p[2] == 0) red++; else if (p[0] == 0 && p[1] == 0 && p[2] == 255) blue++; else other++;
                if (y == H / 2 && firstnon < 0 && !(p[0] == 255 && p[1] == 0 && p[2] == 0)) firstnon = x;
            }
            int ok = 0;
            if (c->expect == 0) ok = (red == 0 && other == 0);
            else if (c->expect == 1) ok = (red == W * H);
            else ok = (red > W * H * 40 / 100 && red < W * H * 60 / 100 && firstnon >= 28 && firstnon <= 36 && other == 0);
            printf("  %s  %-28s red %4d/%d blue %4d other %d | row %d: first non-red column %d | expected %s\n", ok ? "PASS" : "FAIL", c->name, red, W * H, blue, other, H / 2, firstnon,
                   c->expect == 0 ? "nothing drawn" : c->expect == 1 ? "everything drawn" : "left half drawn, edge near column 32");
            total++; if (!ok) fails++;
        }
        vkDestroyPipeline(dev, pipe, NULL); vkDestroyShaderModule(dev, vsm, NULL);
    }
    printf("SUMMARY %d/%d cases pass, %d fail\n", total - fails, total, fails);
    vkDeviceWaitIdle(dev);
    return fails;
}

#ifdef CLIP_REPRO_JNI
#include <jni.h>
JNIEXPORT jint JNICALL Java_com_gowa_scenes_Repro_run(JNIEnv* env, jclass c, jstring jlog)
{
    const char* log = (*env)->GetStringUTFChars(env, jlog, 0);
    int fd = open(log, O_WRONLY | O_CREAT | O_TRUNC, 0644); if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); close(fd); }
    setvbuf(stdout, NULL, _IONBF, 0);
    int rc = clip_main(); fflush(stdout);
    (*env)->ReleaseStringUTFChars(env, jlog, log); return rc;
}
#else
int main(void) { setvbuf(stdout, NULL, _IONBF, 0); return clip_main(); }
#endif
