/* God of War: Ascension Android port -- bring-up app, milestone B: window, swapchain, presentation, lifecycle.
 *
 * What this is: the Android side of presentation, written to survive what Android does to a Vulkan app. It draws a test
 * pattern (clear colour, a moving bar, orientation markers) so a screenshot proves the frame arrives, in the right orientation, at the
 * right size. It is NOT the game's renderer (see android/docs/RENDERER_AUDIT.md) and contains no stub that pretends to be one.
 *
 * Design rules (the user's list):
 *  - Activity lifecycle: SDL3 delivers WILL/DID_ENTER_BACKGROUND/FOREGROUND. The ANativeWindow dies when the app is paused, so the
 *    surface and swapchain are destroyed at DID_ENTER_BACKGROUND (after the device is idle) and recreated at DID_ENTER_FOREGROUND.
 *    The instance, device, render pass, pipeline and command pool live for the whole process.
 *  - Surface loss / out-of-date: VK_ERROR_OUT_OF_DATE_KHR recreates the swapchain (old swapchain passed as oldSwapchain);
 *    VK_ERROR_SURFACE_LOST_KHR destroys swapchain+surface and builds both again.
 *  - Rotation / resolution: the Activity is not recreated (configChanges); SDL reports a pixel-size change, the swapchain extent follows
 *    surface capabilities. A zero extent (minimised) is waited out, not an error.
 *  - No needless CPU blocking: 2 frames in flight, one fence per frame slot waited only when the slot is reused, acquire with a short
 *    timeout instead of an infinite wait; no vkQueueWaitIdle/vkDeviceWaitIdle in the frame loop (only on teardown).
 *  - Debug keys (adb shell input keyevent KEYCODE_F1 / F2): F1 forces a swapchain recreation, F2 forces the surface-loss path.
 */
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <SDL3/SDL_vulkan.h>
#include <vulkan/vulkan.h>
#include <android/log.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rect_spv.h"

#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "gowa", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "gowa", __VA_ARGS__)
#define VKCHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { LOGE("%s failed: %d (%s:%d)", #x, (int)r_, __FILE__, __LINE__); return 0; } } while (0)
#define MAX_IMG 8
#define NFRAMES 2

typedef struct { VkCommandBuffer cb; VkFence fence; VkSemaphore acquire; int submitted; } Frame;

static struct {
    SDL_Window* win;
    VkInstance inst; VkPhysicalDevice phys; VkDevice dev; VkQueue q; uint32_t qfam; int dev_ready;
    VkSurfaceKHR surf; VkSwapchainKHR sc; VkFormat fmt; VkExtent2D ext; VkSurfaceTransformFlagBitsKHR xform;
    uint32_t nimg; VkImage img[MAX_IMG]; VkImageView view[MAX_IMG]; VkFramebuffer fb[MAX_IMG]; VkSemaphore done[MAX_IMG];
    VkRenderPass rp; VkFormat rp_fmt; VkPipelineLayout pl; VkPipeline pipe; VkCommandPool pool; Frame fr[NFRAMES]; uint32_t cur;
    int fg, dirty, running;
    /* counters: every create is matched by a destroy, checked by the lifecycle test */
    unsigned surf_create, surf_destroy, sc_create, sc_destroy, recreate_ood, surface_lost, frames, suboptimal_ignored, acquire_timeouts;
    double t_wait, t_acquire, t_record, t_present; unsigned stat_frames; uint64_t stat_t0;
} g;

static double now_ms(void) { return (double)SDL_GetPerformanceCounter() * 1000.0 / (double)SDL_GetPerformanceFrequency(); }

/* ------------------------------------------------------------------ instance / device (process lifetime) */
static int create_instance(void)
{
    Uint32 n = 0; const char* const* sdlx = SDL_Vulkan_GetInstanceExtensions(&n);
    if (!sdlx) { LOGE("SDL_Vulkan_GetInstanceExtensions: %s", SDL_GetError()); return 0; }
    VkApplicationInfo ai = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "gowa", .apiVersion = VK_API_VERSION_1_1 };
    VkInstanceCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &ai, .enabledExtensionCount = n, .ppEnabledExtensionNames = sdlx };
    VKCHECK(vkCreateInstance(&ci, NULL, &g.inst));
    return 1;
}

/* needs a surface once, to check that the queue family can present */
static int create_device(void)
{
    uint32_t n = 1; VkResult er = vkEnumeratePhysicalDevices(g.inst, &n, &g.phys);
    if ((er != VK_SUCCESS && er != VK_INCOMPLETE) || n == 0) { LOGE("no Vulkan device (%d)", (int)er); return 0; }
    VkPhysicalDeviceProperties p; vkGetPhysicalDeviceProperties(g.phys, &p);
    LOG("device: %s (Vulkan %u.%u.%u)", p.deviceName, VK_API_VERSION_MAJOR(p.apiVersion), VK_API_VERSION_MINOR(p.apiVersion), VK_API_VERSION_PATCH(p.apiVersion));
    uint32_t qn = 0; vkGetPhysicalDeviceQueueFamilyProperties(g.phys, &qn, NULL); VkQueueFamilyProperties qp[8]; if (qn > 8) qn = 8;
    vkGetPhysicalDeviceQueueFamilyProperties(g.phys, &qn, qp);
    g.qfam = ~0u;
    for (uint32_t i = 0; i < qn; i++) { VkBool32 ok = 0; vkGetPhysicalDeviceSurfaceSupportKHR(g.phys, i, g.surf, &ok); if ((qp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && ok) { g.qfam = i; break; } }
    if (g.qfam == ~0u) { LOGE("no graphics+present queue family"); return 0; }
    float prio = 1.0f; const char* ext = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    VkDeviceQueueCreateInfo qi = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = g.qfam, .queueCount = 1, .pQueuePriorities = &prio };
    VkDeviceCreateInfo di = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qi, .enabledExtensionCount = 1, .ppEnabledExtensionNames = &ext };
    VKCHECK(vkCreateDevice(g.phys, &di, NULL, &g.dev));
    vkGetDeviceQueue(g.dev, g.qfam, 0, &g.q);
    VkCommandPoolCreateInfo pi = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = g.qfam };
    VKCHECK(vkCreateCommandPool(g.dev, &pi, NULL, &g.pool));
    VkCommandBufferAllocateInfo ca = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = g.pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    for (int i = 0; i < NFRAMES; i++) {
        VKCHECK(vkAllocateCommandBuffers(g.dev, &ca, &g.fr[i].cb));
        VkFenceCreateInfo fi = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT };
        VKCHECK(vkCreateFence(g.dev, &fi, NULL, &g.fr[i].fence));
        VkSemaphoreCreateInfo si = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        VKCHECK(vkCreateSemaphore(g.dev, &si, NULL, &g.fr[i].acquire));
    }
    g.dev_ready = 1; return 1;
}

/* render pass + pipeline depend on the swapchain format only; made once per format (the format does not change in practice) */
static int create_pass_and_pipeline(VkFormat fmt)
{
    if (g.rp && g.rp_fmt == fmt) return 1;
    if (g.pipe) { vkDestroyPipeline(g.dev, g.pipe, NULL); g.pipe = 0; }
    if (g.pl) { vkDestroyPipelineLayout(g.dev, g.pl, NULL); g.pl = 0; }
    if (g.rp) { vkDestroyRenderPass(g.dev, g.rp, NULL); g.rp = 0; }
    VkAttachmentDescription ad = { .format = fmt, .samples = VK_SAMPLE_COUNT_1_BIT, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, .finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR };
    VkAttachmentReference ar = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sp = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = 1, .pColorAttachments = &ar };
    VkSubpassDependency dep = { .srcSubpass = VK_SUBPASS_EXTERNAL, .dstSubpass = 0, .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT };
    VkRenderPassCreateInfo ri = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 1, .pAttachments = &ad, .subpassCount = 1, .pSubpasses = &sp, .dependencyCount = 1, .pDependencies = &dep };
    VKCHECK(vkCreateRenderPass(g.dev, &ri, NULL, &g.rp)); g.rp_fmt = fmt;

    VkPushConstantRange pcr = { VK_SHADER_STAGE_VERTEX_BIT, 0, 32 };
    VkPipelineLayoutCreateInfo li = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr };
    VKCHECK(vkCreatePipelineLayout(g.dev, &li, NULL, &g.pl));
    VkShaderModule vs, fs;
    VkShaderModuleCreateInfo smv = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = rect_vert_spv_len, .pCode = (const uint32_t*)rect_vert_spv };
    VkShaderModuleCreateInfo smf = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = rect_frag_spv_len, .pCode = (const uint32_t*)rect_frag_spv };
    VKCHECK(vkCreateShaderModule(g.dev, &smv, NULL, &vs)); VKCHECK(vkCreateShaderModule(g.dev, &smf, NULL, &fs));
    VkPipelineShaderStageCreateInfo st[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs, .pName = "main" } };
    VkPipelineVertexInputStateCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP };
    VkPipelineViewportStateCreateInfo vp = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1 };
    VkPipelineRasterizationStateCreateInfo rs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE, .lineWidth = 1.0f };
    VkPipelineMultisampleStateCreateInfo ms = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
    VkPipelineColorBlendAttachmentState ba = { .colorWriteMask = 0xF };
    VkPipelineColorBlendStateCreateInfo cb = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1, .pAttachments = &ba };
    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo ds = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2, .pDynamicStates = dyn };
    VkGraphicsPipelineCreateInfo pi = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = 2, .pStages = st, .pVertexInputState = &vi, .pInputAssemblyState = &ia,
        .pViewportState = &vp, .pRasterizationState = &rs, .pMultisampleState = &ms, .pColorBlendState = &cb, .pDynamicState = &ds, .layout = g.pl, .renderPass = g.rp };
    VkResult r = vkCreateGraphicsPipelines(g.dev, VK_NULL_HANDLE, 1, &pi, NULL, &g.pipe);
    vkDestroyShaderModule(g.dev, vs, NULL); vkDestroyShaderModule(g.dev, fs, NULL);
    if (r != VK_SUCCESS) { LOGE("vkCreateGraphicsPipelines: %d", (int)r); return 0; }
    return 1;
}

/* ------------------------------------------------------------------ surface and swapchain (recreated) */
static void destroy_swapchain(int final_destroy)
{
    for (uint32_t i = 0; i < g.nimg; i++) {
        if (g.fb[i]) vkDestroyFramebuffer(g.dev, g.fb[i], NULL);
        if (g.view[i]) vkDestroyImageView(g.dev, g.view[i], NULL);
        if (g.done[i]) vkDestroySemaphore(g.dev, g.done[i], NULL);
        g.fb[i] = 0; g.view[i] = 0; g.done[i] = 0;
    }
    g.nimg = 0;
    if (final_destroy && g.sc) { vkDestroySwapchainKHR(g.dev, g.sc, NULL); g.sc = 0; g.sc_destroy++; }
}

static void destroy_surface(void)
{
    if (!g.dev_ready) return;
    vkDeviceWaitIdle(g.dev);                 /* teardown only: nothing may still use the swapchain images */
    destroy_swapchain(1);
    if (g.surf) { vkDestroySurfaceKHR(g.inst, g.surf, NULL); g.surf = 0; g.surf_destroy++; }
    for (int i = 0; i < NFRAMES; i++) g.fr[i].submitted = 0;
    LOG("SURFACE destroyed  (surface create/destroy=%u/%u swapchain create/destroy=%u/%u)", g.surf_create, g.surf_destroy, g.sc_create, g.sc_destroy);
}

static int create_surface(void)
{
    if (g.surf) return 1;
    if (!SDL_Vulkan_CreateSurface(g.win, g.inst, NULL, &g.surf)) { LOGE("SDL_Vulkan_CreateSurface: %s", SDL_GetError()); g.surf = 0; return 0; }
    g.surf_create++;
    return 1;
}

/* returns 1 = ready, 0 = failed, -1 = not now (zero-sized surface) */
static int create_swapchain(void)
{
    VkSurfaceCapabilitiesKHR caps;
    VkResult r = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g.phys, g.surf, &caps);
    if (r == VK_ERROR_SURFACE_LOST_KHR) return 0;
    if (r != VK_SUCCESS) { LOGE("surface caps: %d", (int)r); return 0; }
    VkExtent2D ext = caps.currentExtent;
    if (ext.width == 0xFFFFFFFFu) { int w, h; SDL_GetWindowSizeInPixels(g.win, &w, &h); ext.width = (uint32_t)w; ext.height = (uint32_t)h; }
    if (ext.width == 0 || ext.height == 0) return -1;

    uint32_t nf = 0; vkGetPhysicalDeviceSurfaceFormatsKHR(g.phys, g.surf, &nf, NULL); VkSurfaceFormatKHR sf[32]; if (nf > 32) nf = 32;
    vkGetPhysicalDeviceSurfaceFormatsKHR(g.phys, g.surf, &nf, sf);
    VkSurfaceFormatKHR pick = sf[0];
    for (uint32_t i = 0; i < nf; i++) if (sf[i].format == VK_FORMAT_R8G8B8A8_UNORM || sf[i].format == VK_FORMAT_B8G8R8A8_UNORM) { pick = sf[i]; break; }
    if (!create_pass_and_pipeline(pick.format)) return 0;

    { VkPresentModeKHR pm[8]; uint32_t np = 8; vkGetPhysicalDeviceSurfacePresentModesKHR(g.phys, g.surf, &np, pm);
      char b[96] = ""; for (uint32_t i = 0; i < np && i < 8; i++) { char t[16]; snprintf(t, sizeof t, "%d ", (int)pm[i]); strcat(b, t); }
      LOG("present modes (0=IMMEDIATE 1=MAILBOX 2=FIFO 3=FIFO_RELAXED): %s", b); }
    /* FIFO is the one mode every driver has. Image count: one more than the minimum so acquire does not stall on the compositor. */
    uint32_t want = caps.minImageCount + 1; if (want < 3) want = 3; if (caps.maxImageCount && want > caps.maxImageCount) want = caps.maxImageCount; if (want > MAX_IMG) want = MAX_IMG;
    /* Transform: identity when the surface accepts it (the compositor rotates if the display is rotated: simple, costs a pass on some devices;
     * the renderer will pre-rotate with the display transform later). */
    VkSurfaceTransformFlagBitsKHR xf = (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR : caps.currentTransform;
    VkCompositeAlphaFlagBitsKHR ca = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR : VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    VkSwapchainKHR old = g.sc;
    VkSwapchainCreateInfoKHR si = { .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, .surface = g.surf, .minImageCount = want, .imageFormat = pick.format, .imageColorSpace = pick.colorSpace,
        .imageExtent = ext, .imageArrayLayers = 1, .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform = xf, .compositeAlpha = ca, .presentMode = VK_PRESENT_MODE_FIFO_KHR, .clipped = VK_TRUE, .oldSwapchain = old };
    VkSwapchainKHR sc = 0; r = vkCreateSwapchainKHR(g.dev, &si, NULL, &sc);
    if (old) { vkDestroySwapchainKHR(g.dev, old, NULL); g.sc = 0; g.sc_destroy++; }   /* retired by the call above in any case */
    if (r != VK_SUCCESS) { LOGE("vkCreateSwapchainKHR: %d", (int)r); return 0; }
    g.sc = sc; g.sc_create++; g.fmt = pick.format; g.ext = ext; g.xform = xf;
    g.nimg = MAX_IMG; VKCHECK(vkGetSwapchainImagesKHR(g.dev, g.sc, &g.nimg, g.img));
    for (uint32_t i = 0; i < g.nimg; i++) {
        VkImageViewCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = g.img[i], .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = g.fmt,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
        VKCHECK(vkCreateImageView(g.dev, &vi, NULL, &g.view[i]));
        VkFramebufferCreateInfo fi = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = g.rp, .attachmentCount = 1, .pAttachments = &g.view[i], .width = ext.width, .height = ext.height, .layers = 1 };
        VKCHECK(vkCreateFramebuffer(g.dev, &fi, NULL, &g.fb[i]));
        VkSemaphoreCreateInfo se = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        VKCHECK(vkCreateSemaphore(g.dev, &se, NULL, &g.done[i]));
    }
    LOG("SWAPCHAIN %ux%u images=%u format=%d preTransform=0x%x (current 0x%x, supported 0x%x) mode=FIFO  (surface %u/%u swapchain %u/%u)",
        ext.width, ext.height, g.nimg, (int)g.fmt, (unsigned)xf, (unsigned)caps.currentTransform, (unsigned)caps.supportedTransforms, g.surf_create, g.surf_destroy, g.sc_create, g.sc_destroy);
    g.dirty = 0; return 1;
}

/* swapchain only (rotation / resize / out of date): the old one is passed as oldSwapchain */
static int recreate_swapchain(void)
{
    vkDeviceWaitIdle(g.dev);
    destroy_swapchain(0);
    return create_swapchain();
}

/* the whole surface path again (after loss, or at foreground) */
static int ensure_surface_and_swapchain(void)
{
    if (g.sc && !g.dirty) return 1;
    if (!g.surf && !create_surface()) return 0;
    if (!g.dev_ready) { if (!create_device()) return 0; }
    int r = g.sc ? recreate_swapchain() : create_swapchain();
    if (r == 0) { destroy_surface(); g.surface_lost++; return 0; }
    return r == 1;
}

/* ------------------------------------------------------------------ one frame */
static void rect(VkCommandBuffer cb, float x, float y, float w, float h, float r, float gg, float b)
{
    float pc[8] = { x, y, w, h, r, gg, b, 1.0f };
    vkCmdPushConstants(cb, g.pl, VK_SHADER_STAGE_VERTEX_BIT, 0, 32, pc);
    vkCmdDraw(cb, 4, 1, 0, 0);
}

static void render_frame(void)
{
    Frame* f = &g.fr[g.cur];
    double t0 = now_ms();
    if (f->submitted) {                                   /* the only wait: this slot's previous frame */
        VkResult w = vkWaitForFences(g.dev, 1, &f->fence, VK_TRUE, 1000000000ull);
        if (w != VK_SUCCESS) { LOGE("fence wait: %d", (int)w); return; }
    }
    double t1 = now_ms();
    uint32_t idx; VkResult r = vkAcquireNextImageKHR(g.dev, g.sc, 100000000ull, f->acquire, VK_NULL_HANDLE, &idx);
    double t2 = now_ms();
    if (r == VK_ERROR_OUT_OF_DATE_KHR) { g.recreate_ood++; g.dirty = 1; return; }
    if (r == VK_ERROR_SURFACE_LOST_KHR) { LOG("acquire: surface lost"); destroy_surface(); g.surface_lost++; return; }
    if (r == VK_TIMEOUT || r == VK_NOT_READY) { g.acquire_timeouts++; return; }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) { LOGE("acquire: %d", (int)r); return; }

    vkResetFences(g.dev, 1, &f->fence);
    VkCommandBuffer cb = f->cb; vkResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    vkBeginCommandBuffer(cb, &bi);
    double tt = (double)SDL_GetTicks() / 1000.0;
    VkClearValue cv = { .color = { { 0.10f + 0.08f * (float)sin(tt * 0.9), 0.12f + 0.08f * (float)sin(tt * 0.7 + 2.0), 0.20f + 0.08f * (float)sin(tt * 0.5 + 4.0), 1.0f } } };
    VkRenderPassBeginInfo rb = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = g.rp, .framebuffer = g.fb[idx], .renderArea = { { 0, 0 }, g.ext }, .clearValueCount = 1, .pClearValues = &cv };
    vkCmdBeginRenderPass(cb, &rb, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g.pipe);
    VkViewport vp = { 0, 0, (float)g.ext.width, (float)g.ext.height, 0, 1 }; VkRect2D sc = { { 0, 0 }, g.ext };
    vkCmdSetViewport(cb, 0, 1, &vp); vkCmdSetScissor(cb, 0, 1, &sc);
    /* NDC: x right, y down in Vulkan. Orientation markers: top-left red, top-right green, bottom-left blue, bottom-right white. */
    const float m = 0.06f, s = 0.12f;
    rect(cb, -1 + m, -1 + m, s * 0.5f, s, 0.95f, 0.15f, 0.15f);           /* top-left, tall: also tells the aspect */
    rect(cb, 1 - m - s * 0.5f, -1 + m, s * 0.5f, s, 0.15f, 0.9f, 0.2f);
    rect(cb, -1 + m, 1 - m - s, s * 0.5f, s, 0.2f, 0.3f, 0.95f);
    rect(cb, 1 - m - s * 0.5f, 1 - m - s, s * 0.5f, s, 1, 1, 1);
    float bar = (float)fmod(tt * 0.4, 1.0) * 2.0f - 1.0f;                  /* a bar sweeping across: proves frames keep arriving */
    rect(cb, bar, -0.05f, 0.04f, 0.10f, 1.0f, 0.85f, 0.1f);
    float cx = (float)((double)g.frames / 120.0 - floor((double)g.frames / 120.0));
    rect(cb, -1 + cx * 2.0f, 0.80f, 0.02f, 0.05f, 0.9f, 0.9f, 0.9f);       /* frame-counter bar: wraps every 120 frames */
    vkCmdEndRenderPass(cb);
    vkEndCommandBuffer(cb);
    double t3 = now_ms();

    VkPipelineStageFlags ws = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo sub = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = 1, .pWaitSemaphores = &f->acquire, .pWaitDstStageMask = &ws, .commandBufferCount = 1, .pCommandBuffers = &cb,
        .signalSemaphoreCount = 1, .pSignalSemaphores = &g.done[idx] };
    VkResult sr = vkQueueSubmit(g.q, 1, &sub, f->fence);
    if (sr != VK_SUCCESS) { LOGE("vkQueueSubmit: %d", (int)sr); if (sr == VK_ERROR_DEVICE_LOST) g.running = 0; return; }
    f->submitted = 1;
    VkPresentInfoKHR pi = { .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = 1, .pWaitSemaphores = &g.done[idx], .swapchainCount = 1, .pSwapchains = &g.sc, .pImageIndices = &idx };
    VkResult pr = vkQueuePresentKHR(g.q, &pi);
    double t4 = now_ms();
    g.cur = (g.cur + 1) % NFRAMES;
    if (pr == VK_ERROR_OUT_OF_DATE_KHR) { g.recreate_ood++; g.dirty = 1; }
    else if (pr == VK_ERROR_SURFACE_LOST_KHR) { LOG("present: surface lost"); destroy_surface(); g.surface_lost++; return; }
    else if (pr == VK_SUBOPTIMAL_KHR || r == VK_SUBOPTIMAL_KHR) {
        /* With preTransform != the display's transform Android reports SUBOPTIMAL on every present: only a changed extent justifies recreation. */
        VkSurfaceCapabilitiesKHR caps; if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g.phys, g.surf, &caps) == VK_SUCCESS &&
            (caps.currentExtent.width != g.ext.width || caps.currentExtent.height != g.ext.height)) g.dirty = 1; else g.suboptimal_ignored++;
    } else if (pr != VK_SUCCESS) LOGE("present: %d", (int)pr);

    g.frames++; g.stat_frames++; g.t_wait += t1 - t0; g.t_acquire += t2 - t1; g.t_record += t3 - t2; g.t_present += t4 - t3;
    uint64_t nowt = SDL_GetTicks();
    if (nowt - g.stat_t0 >= 2000) {
        double n = g.stat_frames;
        LOG("STATS %.1f fps | per frame ms: fence-wait %.2f acquire %.2f record %.2f submit+present %.2f | frames=%u recreate(ood)=%u surface_lost=%u suboptimal_ignored=%u acquire_timeouts=%u",
            n * 1000.0 / (double)(nowt - g.stat_t0), g.t_wait / n, g.t_acquire / n, g.t_record / n, g.t_present / n, g.frames, g.recreate_ood, g.surface_lost, g.suboptimal_ignored, g.acquire_timeouts);
        g.t_wait = g.t_acquire = g.t_record = g.t_present = 0; g.stat_frames = 0; g.stat_t0 = nowt;
    }
}

/* ------------------------------------------------------------------ main loop and lifecycle */
int main(int argc, char** argv)
{
    (void)argc; (void)argv;
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) { LOGE("SDL_Init: %s", SDL_GetError()); return 1; }
    g.win = SDL_CreateWindow("gowa", 0, 0, SDL_WINDOW_VULKAN | SDL_WINDOW_FULLSCREEN);
    if (!g.win) { LOGE("SDL_CreateWindow: %s", SDL_GetError()); return 1; }
    if (!create_instance()) return 1;
    g.fg = 1; g.running = 1; g.stat_t0 = SDL_GetTicks();
    LOG("started");
    while (g.running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
            case SDL_EVENT_QUIT: case SDL_EVENT_TERMINATING: LOG("EVENT quit/terminating"); g.running = 0; break;
            case SDL_EVENT_WILL_ENTER_BACKGROUND: LOG("EVENT will-enter-background"); g.fg = 0; break;
            case SDL_EVENT_DID_ENTER_BACKGROUND: LOG("EVENT did-enter-background"); g.fg = 0; destroy_surface(); break;
            case SDL_EVENT_WILL_ENTER_FOREGROUND: LOG("EVENT will-enter-foreground"); break;
            case SDL_EVENT_DID_ENTER_FOREGROUND: LOG("EVENT did-enter-foreground"); g.fg = 1; break;
            case SDL_EVENT_LOW_MEMORY: LOG("EVENT low-memory"); break;
            case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED: case SDL_EVENT_WINDOW_RESIZED:
                LOG("EVENT window size %dx%d", e.window.data1, e.window.data2); g.dirty = 1; break;
            case SDL_EVENT_KEY_DOWN:
                if (e.key.key == SDLK_F1) { LOG("DEBUG forced swapchain recreation"); g.dirty = 1; }
                if (e.key.key == SDLK_F2) { LOG("DEBUG forced surface loss"); destroy_surface(); g.surface_lost++; }
                break;
            default: break;
            }
        }
        if (!g.fg) { SDL_Delay(20); continue; }
        if (!g.sc || g.dirty || !g.surf) {
            if (!ensure_surface_and_swapchain()) { SDL_Delay(10); continue; }
        }
        if (g.sc) render_frame();
    }
    if (g.dev_ready) { vkDeviceWaitIdle(g.dev); }
    destroy_surface();
    if (g.dev_ready) {
        for (int i = 0; i < NFRAMES; i++) { vkDestroyFence(g.dev, g.fr[i].fence, NULL); vkDestroySemaphore(g.dev, g.fr[i].acquire, NULL); }
        if (g.pipe) vkDestroyPipeline(g.dev, g.pipe, NULL);
        if (g.pl) vkDestroyPipelineLayout(g.dev, g.pl, NULL);
        if (g.rp) vkDestroyRenderPass(g.dev, g.rp, NULL);
        vkDestroyCommandPool(g.dev, g.pool, NULL);
        vkDestroyDevice(g.dev, NULL);
    }
    if (g.inst) vkDestroyInstance(g.inst, NULL);
    LOG("EXIT clean: surface create/destroy=%u/%u swapchain create/destroy=%u/%u frames=%u", g.surf_create, g.surf_destroy, g.sc_create, g.sc_destroy, g.frames);
    SDL_DestroyWindow(g.win); SDL_Quit();
    return 0;
}
