// JNI entry that runs upstream's scene runner (host_posix.c, built with -Dmain=gowa_host_main) inside the app process,
// so the Vulkan loader applies debug layers (debug.vulkan.layers) to it. stdout/stderr go to a file the Activity names.
#include <jni.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <vulkan/vulkan.h>
/* Positive control for the validation layer: vkCreateInstance with a wrong sType is a
 * parameter-validation error that the layer reports. */
static void vvl_control(void)
{
    void* lib = dlopen("libvulkan.so", RTLD_NOW); if (!lib) return;
    PFN_vkCreateInstance ci = (PFN_vkCreateInstance)dlsym(lib, "vkCreateInstance"); if (!ci) return;
    VkInstanceCreateInfo info; memset(&info, 0, sizeof info);
    info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;   /* wrong sType: VUID-VkInstanceCreateInfo-sType-sType */
    VkInstance inst = 0; VkResult r = ci(&info, NULL, &inst);
    fprintf(stderr, "[vvl-control] vkCreateInstance(bad) = %d (the layer should have logged a VUID to logcat)\n", (int)r);
}
extern int gowa_host_main(int argc, char** argv);
JNIEXPORT jint JNICALL Java_com_gowa_scenes_Runner_run(JNIEnv* env, jclass c, jstring jlog, jstring jargs)
{
    const char* log = (*env)->GetStringUTFChars(env, jlog, 0);
    const char* args = (*env)->GetStringUTFChars(env, jargs, 0);
    int fd = open(log, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); close(fd); }
    char* copy = strdup(args); char* argv[64]; int argc = 0; argv[argc++] = "ps3recomp_host";
    for (char* t = strtok(copy, " "); t && argc < 63; t = strtok(0, " ")) argv[argc++] = t;
    argv[argc] = 0;
    if (getenv("GOWA_VVL_CONTROL")) vvl_control();
    int rc = gowa_host_main(argc, argv);
    fflush(stdout); fflush(stderr);
    (*env)->ReleaseStringUTFChars(env, jlog, log); (*env)->ReleaseStringUTFChars(env, jargs, args);
    return rc;
}
JNIEXPORT void JNICALL Java_com_gowa_scenes_Runner_setenv(JNIEnv* env, jclass c, jstring k, jstring v)
{
    const char* ks = (*env)->GetStringUTFChars(env, k, 0); const char* vs = (*env)->GetStringUTFChars(env, v, 0);
    setenv(ks, vs, 1);
    (*env)->ReleaseStringUTFChars(env, k, ks); (*env)->ReleaseStringUTFChars(env, v, vs);
}
