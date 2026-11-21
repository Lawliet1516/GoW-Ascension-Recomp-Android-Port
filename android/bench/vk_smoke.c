/* The project's Vulkan RSX backend on the phone, headless (no window): create instance/device, run its clear + present +
 * readback path and print the centre pixel. Environment: PS3RECOMP_VK_DEVICE etc. as documented in the backend. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
typedef uint32_t u32;
extern int  rsx_vulkan_backend_init(u32 width, u32 height, const char* title);
extern void rsx_vulkan_backend_shutdown(void);
extern void rsx_vulkan_backend_present(void);
extern u32  rsx_vulkan_backend_debug_color(void);
extern u32  rsx_vulkan_backend_readback_center(void);
extern int  rsx_vulkan_backend_guest_programs(void);
int main(void)
{
    int r = rsx_vulkan_backend_init(1280, 720, "vk_smoke");
    printf("rsx_vulkan_backend_init -> %d\n", r);
    if (r != 0) return 1;
    rsx_vulkan_backend_present();
    printf("debug colour 0x%08x, centre pixel after present 0x%08x, guest programs: %d\n", rsx_vulkan_backend_debug_color(), rsx_vulkan_backend_readback_center(), rsx_vulkan_backend_guest_programs());
    rsx_vulkan_backend_shutdown();
    printf("shutdown ok\n");
    return 0;
}
