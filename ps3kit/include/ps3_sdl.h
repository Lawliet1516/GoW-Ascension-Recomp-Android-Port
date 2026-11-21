/* SDL2 / SDL3 selector for the pad, audio and Vulkan-window code.
 *
 * Default is SDL2, exactly as upstream. Define PS3_SDL3 (the Android build does)
 * and the SDL2 names the runtime uses are mapped onto SDL3 with the same
 * semantics, including the places where the return-value convention flipped
 * (SDL_InitSubSystem: 0 = ok in SDL2, true = ok in SDL3). Call sites stay SDL2-style. */
#ifndef PS3_SDL_H
#define PS3_SDL_H

#if !defined(PS3_SDL3)
#  if defined(_WIN32) && defined(__has_include) && !__has_include(<SDL2/SDL.h>)
#    include <SDL.h>
#  else
#    include <SDL2/SDL.h>
#  endif
#  define PS3_SDL_KEY(e) ((e).key.keysym.sym)
#else
#  include <SDL3/SDL.h>
#  include <SDL3/SDL_vulkan.h>
#  include <string.h>

#  define PS3_SDL_KEY(e) ((e).key.key)

/* --- init: SDL3 returns bool (true = ok); SDL2 callers test for 0 / <0 --- */
static inline int ps3sdl_init_subsystem(SDL_InitFlags f) { return SDL_InitSubSystem(f) ? 0 : -1; }
#  undef SDL_InitSubSystem
#  define SDL_InitSubSystem ps3sdl_init_subsystem
#  define SDL_INIT_GAMECONTROLLER SDL_INIT_GAMEPAD

/* --- events / window --- */
#  define SDL_QUIT    SDL_EVENT_QUIT
#  define SDL_KEYDOWN SDL_EVENT_KEY_DOWN
#  define SDL_WINDOWPOS_CENTERED 0
#  define SDL_WINDOW_SHOWN 0
#  define SDL_WINDOW_FULLSCREEN_DESKTOP SDL_WINDOW_FULLSCREEN
#  define SDL_TICKS_PASSED(a, b) ((Sint32)((b) - (a)) <= 0)
static inline SDL_Window* ps3sdl_create_window(const char* t, int x, int y, int w, int h, Uint32 f)
{ (void)x; (void)y; return SDL_CreateWindow(t, w, h, (SDL_WindowFlags)f); }
#  undef SDL_CreateWindow
#  define SDL_CreateWindow ps3sdl_create_window

/* --- Vulkan: SDL3 has no per-window extension query and CreateSurface takes an allocator --- */
static inline int ps3sdl_vk_exts(SDL_Window* w, unsigned* n, const char** out)
{
    (void)w; Uint32 c = 0;
    const char* const* e = SDL_Vulkan_GetInstanceExtensions(&c);
    if (!e) return 0;
    if (!out) { *n = c; return 1; }
    for (unsigned i = 0; i < *n && i < c; i++) out[i] = e[i];
    *n = *n < c ? *n : c;
    return 1;
}
#  undef SDL_Vulkan_GetInstanceExtensions
#  define SDL_Vulkan_GetInstanceExtensions ps3sdl_vk_exts
#  define SDL_Vulkan_CreateSurface(w, i, s) SDL_Vulkan_CreateSurface((w), (i), NULL, (s))
#  define SDL_Vulkan_GetDrawableSize(w, pw, ph) SDL_GetWindowSizeInPixels((w), (pw), (ph))

/* --- game controller -> gamepad (SDL2 device index -> SDL3 instance id) --- */
typedef SDL_Gamepad SDL_GameController;
#  define SDL_CONTROLLER_BUTTON_A             SDL_GAMEPAD_BUTTON_SOUTH
#  define SDL_CONTROLLER_BUTTON_B             SDL_GAMEPAD_BUTTON_EAST
#  define SDL_CONTROLLER_BUTTON_X             SDL_GAMEPAD_BUTTON_WEST
#  define SDL_CONTROLLER_BUTTON_Y             SDL_GAMEPAD_BUTTON_NORTH
#  define SDL_CONTROLLER_BUTTON_BACK          SDL_GAMEPAD_BUTTON_BACK
#  define SDL_CONTROLLER_BUTTON_GUIDE         SDL_GAMEPAD_BUTTON_GUIDE
#  define SDL_CONTROLLER_BUTTON_START         SDL_GAMEPAD_BUTTON_START
#  define SDL_CONTROLLER_BUTTON_LEFTSTICK     SDL_GAMEPAD_BUTTON_LEFT_STICK
#  define SDL_CONTROLLER_BUTTON_RIGHTSTICK    SDL_GAMEPAD_BUTTON_RIGHT_STICK
#  define SDL_CONTROLLER_BUTTON_LEFTSHOULDER  SDL_GAMEPAD_BUTTON_LEFT_SHOULDER
#  define SDL_CONTROLLER_BUTTON_RIGHTSHOULDER SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER
#  define SDL_CONTROLLER_BUTTON_DPAD_UP       SDL_GAMEPAD_BUTTON_DPAD_UP
#  define SDL_CONTROLLER_BUTTON_DPAD_DOWN     SDL_GAMEPAD_BUTTON_DPAD_DOWN
#  define SDL_CONTROLLER_BUTTON_DPAD_LEFT     SDL_GAMEPAD_BUTTON_DPAD_LEFT
#  define SDL_CONTROLLER_BUTTON_DPAD_RIGHT    SDL_GAMEPAD_BUTTON_DPAD_RIGHT
#  define SDL_CONTROLLER_AXIS_LEFTX           SDL_GAMEPAD_AXIS_LEFTX
#  define SDL_CONTROLLER_AXIS_LEFTY           SDL_GAMEPAD_AXIS_LEFTY
#  define SDL_CONTROLLER_AXIS_RIGHTX          SDL_GAMEPAD_AXIS_RIGHTX
#  define SDL_CONTROLLER_AXIS_RIGHTY          SDL_GAMEPAD_AXIS_RIGHTY
#  define SDL_CONTROLLER_AXIS_TRIGGERLEFT     SDL_GAMEPAD_AXIS_LEFT_TRIGGER
#  define SDL_CONTROLLER_AXIS_TRIGGERRIGHT    SDL_GAMEPAD_AXIS_RIGHT_TRIGGER
#  define SDL_GameControllerButton SDL_GamepadButton

static inline SDL_JoystickID ps3sdl_pad_id(int index)
{
    int n = 0; SDL_JoystickID* ids = SDL_GetGamepads(&n);
    SDL_JoystickID id = (ids && index >= 0 && index < n) ? ids[index] : 0;
    SDL_free(ids);
    return id;
}
static inline int ps3sdl_num_pads(void)
{ int n = 0; SDL_free(SDL_GetGamepads(&n)); return n; }
static inline int ps3sdl_is_pad(int index) { return ps3sdl_pad_id(index) != 0; }
static inline SDL_Gamepad* ps3sdl_pad_open(int index)
{ SDL_JoystickID id = ps3sdl_pad_id(index); return id ? SDL_OpenGamepad(id) : NULL; }
static inline int ps3sdl_pad_rumble(SDL_Gamepad* g, Uint16 lo, Uint16 hi, Uint32 ms)
{ return SDL_RumbleGamepad(g, lo, hi, ms) ? 0 : -1; }

#  define SDL_NumJoysticks()               ps3sdl_num_pads()
#  define SDL_IsGameController(i)          ps3sdl_is_pad(i)
#  define SDL_GameControllerOpen(i)        ps3sdl_pad_open(i)
#  define SDL_GameControllerClose(g)       SDL_CloseGamepad(g)
#  define SDL_GameControllerName(g)        SDL_GetGamepadName(g)
#  define SDL_GameControllerPath(g)        SDL_GetGamepadPath(g)
#  define SDL_GameControllerGetAttached(g) SDL_GamepadConnected(g)
#  define SDL_GameControllerGetButton(g, b) ((Uint8)SDL_GetGamepadButton((g), (b)))
#  define SDL_GameControllerGetAxis(g, a)   SDL_GetGamepadAxis((g), (a))
#  define SDL_GameControllerUpdate()       SDL_UpdateGamepads()
#  define SDL_GameControllerRumble(g, lo, hi, ms) ps3sdl_pad_rumble((g), (lo), (hi), (ms))

/* --- audio: the SDL2 queue API on an SDL3 stream (one output device) --- */
typedef struct { SDL_AudioStream* s; } ps3sdl_audio;
static inline int ps3sdl_audio_open(ps3sdl_audio* a, int rate, int channels)
{
    SDL_AudioSpec spec; memset(&spec, 0, sizeof spec);
    spec.freq = rate; spec.format = SDL_AUDIO_F32; spec.channels = channels;
    a->s = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
    if (!a->s) return -1;
    SDL_ResumeAudioStreamDevice(a->s);
    return 0;
}
static inline void ps3sdl_audio_close(ps3sdl_audio* a) { if (a->s) { SDL_DestroyAudioStream(a->s); a->s = NULL; } }
static inline void ps3sdl_audio_queue(ps3sdl_audio* a, const void* d, Uint32 bytes) { if (a->s) SDL_PutAudioStreamData(a->s, d, (int)bytes); }
static inline Uint32 ps3sdl_audio_queued(ps3sdl_audio* a) { return a->s ? (Uint32)SDL_GetAudioStreamQueued(a->s) : 0; }
#endif
#endif
