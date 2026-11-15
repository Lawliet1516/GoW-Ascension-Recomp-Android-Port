/*
 * ps3recomp - cellAudio HLE implementation
 *
 * Real audio mixing and output. A background mixing thread reads audio data
 * from each active port's buffer in guest memory, mixes them, and outputs
 * to the host audio device.
 *
 * Backend selection:
 *   - Windows default: WASAPI (via mmdeviceapi)
 *   - Everywhere else / if PS3RECOMP_AUDIO_USE_SDL2 defined: SDL2 audio
 *
 * Define PS3RECOMP_AUDIO_USE_SDL2 to force SDL2 backend on Windows.
 */

#include "../../runtime/memory/vm.h"   /* vm_commit */
#include "cellAudio.h"
#include "../../runtime/ppu/ppu_memory.h"   /* vm_base, vm_read64, vm_write32 */
#include "../../runtime/platform/host_pin.h" /* PS3_MMCSS: the mixer joins MMCSS "Audio" */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <math.h>

/* Event-queue push/lookup (runtime/syscalls/sys_event.c). Forward-declared
 * rather than including sys_event.h to avoid pulling PPU context/syscall
 * table types into the HLE audio module. */
extern int      sys_event_queue_push_by_id(uint32_t queue_id,
                                            uint64_t source, uint64_t data1,
                                            uint64_t data2,  uint64_t data3);
extern uint32_t sys_event_find_queue_by_key(uint64_t key);
extern uint32_t sys_event_queue_create_direct(uint64_t key, int32_t size);

/* ---------------------------------------------------------------------------
 * Backend selection
 * -----------------------------------------------------------------------*/

#if defined(PS3RECOMP_AUDIO_USE_SDL2)
  #define AUDIO_BACKEND_SDL2    1
  #define AUDIO_BACKEND_WASAPI  0
#elif defined(_WIN32)
  #define AUDIO_BACKEND_SDL2    0
  #define AUDIO_BACKEND_WASAPI  1
#else
  #define AUDIO_BACKEND_SDL2    1
  #define AUDIO_BACKEND_WASAPI  0
#endif

#if AUDIO_BACKEND_WASAPI
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
  #include <mmdeviceapi.h>
  #include <audioclient.h>
  #include <process.h>

  /* Define WASAPI COM GUIDs (avoids needing uuid.lib linkage for these) */
  #ifdef __cplusplus
    #define GUID_SECT
  #else
    #define GUID_SECT
  #endif
  #ifndef DEFINE_AUDIO_GUID
    #ifdef INITGUID
      #define DEFINE_AUDIO_GUID(name, l, w1, w2, b1, b2, b3, b4, b5, b6, b7, b8) \
              const GUID name = { l, w1, w2, { b1, b2, b3, b4, b5, b6, b7, b8 } }
    #else
      #define DEFINE_AUDIO_GUID(name, l, w1, w2, b1, b2, b3, b4, b5, b6, b7, b8) \
              const GUID name = { l, w1, w2, { b1, b2, b3, b4, b5, b6, b7, b8 } }
    #endif
  #endif
  DEFINE_AUDIO_GUID(ps3r_CLSID_MMDeviceEnumerator, 0xBCDE0395,0xE52F,0x467C,0x8E,0x3D,0xC4,0x57,0x92,0x91,0x69,0x2E);
  DEFINE_AUDIO_GUID(ps3r_IID_IMMDeviceEnumerator,  0xA95664D2,0x9614,0x4F35,0xA7,0x46,0xDE,0x8D,0xB6,0x36,0x17,0xE6);
  DEFINE_AUDIO_GUID(ps3r_IID_IAudioClient,         0x1CB9AD4C,0xDBFA,0x4c32,0xB1,0x78,0xC2,0xF5,0x68,0xA7,0x03,0xB2);
  DEFINE_AUDIO_GUID(ps3r_IID_IAudioRenderClient,   0xF294ACFC,0x3146,0x4483,0xA7,0xBF,0xAD,0xDC,0xA7,0xC2,0x60,0xE2);

  /* Map to the names used in the code */
  #define IID_IMMDeviceEnumerator  ps3r_IID_IMMDeviceEnumerator
  #define CLSID_MMDeviceEnumerator ps3r_CLSID_MMDeviceEnumerator
  #define IID_IAudioClient         ps3r_IID_IAudioClient
  #define IID_IAudioRenderClient   ps3r_IID_IAudioRenderClient
#endif

#if AUDIO_BACKEND_SDL2
  #include <SDL2/SDL.h>
#endif

/* Portable threading */
#ifdef _WIN32
  #include <windows.h>
  typedef HANDLE thread_t;
  typedef CRITICAL_SECTION mutex_t;
  #define mutex_init(m)    InitializeCriticalSection(m)
  #define mutex_destroy(m) DeleteCriticalSection(m)
  #define mutex_lock(m)    EnterCriticalSection(m)
  #define mutex_unlock(m)  LeaveCriticalSection(m)
#else
  #include <pthread.h>
  #include <unistd.h>
  #include <time.h>
  typedef pthread_t thread_t;
  typedef pthread_mutex_t mutex_t;
  #define mutex_init(m)    pthread_mutex_init(m, NULL)
  #define mutex_destroy(m) pthread_mutex_destroy(m)
  #define mutex_lock(m)    pthread_mutex_lock(m)
  #define mutex_unlock(m)  pthread_mutex_unlock(m)
#endif

/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/

/* Per-port audio buffer allocated on the host side.
 * In a full emulator these would be in guest VM memory; here we allocate
 * host buffers and expose their addresses through portAddr/readIndexAddr. */
#define AUDIO_PORT_BUF_MAX  (CELL_AUDIO_BLOCK_32 * CELL_AUDIO_BLOCK_SAMPLES * CELL_AUDIO_PORT_8CH)

typedef struct {
    int                  in_use;
    int                  running;
    CellAudioPortParam   param;
    float*               buffer;        /* host audio buffer (float samples) */
    u32                  buf_size;       /* buffer size in bytes */
    u64                  read_index;     /* current read position (block index) */
    u64                  write_index;    /* where the game is writing */
    /* For address reporting to guest */
    u64                  port_addr;      /* guest-visible buffer address */
    u64                  read_idx_addr;  /* guest-visible read index address */
} AudioPortSlot;

static int            s_audio_initialized = 0;
static AudioPortSlot  s_ports[CELL_AUDIO_PORT_MAX];

/* Event queue notification */
typedef struct {
    int  in_use;
    u64  key;
} AudioNotifySlot;
static AudioNotifySlot s_notify_queues[CELL_AUDIO_MAX_NOTIFY_EVENT_QUEUES];

/* Timestamp anchor for cellAudioGetPortTimestamp (microseconds, set at init) */
static u64            s_audio_start_us = 0;

/* Mixing thread */
static unsigned long long g_audio_dropped;   /* frames the device had no room for */
volatile unsigned long long g_audio_blocks_mixed;   /* blocks mixed (LD_FLOOR_ZERO's hold lines; AUDIO_WAV block index) */
static volatile int  s_mix_thread_running = 0;
static thread_t      s_mix_thread;
#ifndef _WIN32
/* Is s_mix_thread a thread that exists and has not been joined? The Win32
 * branch asks that of the HANDLE itself, which it nulls after closing;
 * pthread_t has no such value, so carry the flag. */
static int           s_mix_thread_live = 0;
#endif
static mutex_t       s_audio_mutex;

/* Output mix buffer (stereo, one block worth) */
static float s_mix_buffer[CELL_AUDIO_BLOCK_SAMPLES * 2];
/* SURROUND (2026-11-15): the game configures 5.1 (AudioOutConfigure channel=6) and writes its 8-channel
 * port in the PS3 order L R C LFE Ls Rs Lb Rb, but every block was folded down to stereo, so a 5.1/7.1
 * speaker setup played nothing on its surround speakers (players, 2026-11-15). When Windows' default
 * device is set to 6 or more channels, the output is now a 5.1 stream (L R C LFE Ls Rs; the two extra
 * channels folded into Ls/Rs; Windows maps it onto 7.1). AUDIO_SURROUND=0: stereo as before;
 * =1: 5.1 even on a stereo device (Windows folds it down; for tests). AUDIO_OUT_CH=2 keeps stereo. */
static float s_mix_mc[CELL_AUDIO_BLOCK_SAMPLES * 6];
static u32   s_out_ch = 2;

/* ---------------------------------------------------------------------------
 * Audio health (2026-11-07, AUDIO-NOTES.md). One "[audio]" line per 5 s, ON by
 * default (AUDIO_HEALTH=0: off; =2: also per-channel input peaks), so a
 * player's log shows audio problems. Costs a few compares per sample.
 *   lead    blocks the producer had written ahead of the read cursor (incl.
 *           the one being read), min/avg over the window, while sound plays
 *   holes   short zero runs (<= 100 ms) inside sound: the producer was late
 *           and the mixer played silence there (ms = silence inserted)
 *   torn    blocks read with a zero tail: producer still writing it
 *   stall   longest time the producer wrote no new block while sound played
 *   clip    output samples past +-1.0 before the clamp (per s) and the peak
 *   dev     WASAPI frames still queued when the mixer looked (min), times it
 *           was below one block, times it was empty (an underrun = a click)
 *   gap     longest time between two mixed blocks (mixing-thread delay)
 * Long silences (>= 300 ms inside play) get their own line when they end.
 * -----------------------------------------------------------------------*/
static int s_health = -1;
typedef struct {
    unsigned blocks, lead_n, lead_sum, lead_min, lead_h[4];
    unsigned holes, hole_blocks, torn, silences, unwritten;
    double   stall_max_ms, silence_max_ms;
    unsigned clip;
    float    peak, ch_peak[8];
    unsigned dev_low, dev_under, dev_n; int dev_min;
    double   gap_max_ms; unsigned gap_over;
    unsigned waits; double wait_ms;
    /* AUDIO_ELASTIC: blocks waited for, of them arrived in time (saved),
     * concealment blocks written, producer-ahead skips, give-ups, silent
     * blocks dropped to repay concealment, concealed blocks not to be repaid
     * (past AUDIO_ELASTIC_DEBT, or owed at a give-up: 'forgiven') */
    unsigned el_waits, el_saved, el_conceal, el_skip, el_giveup, el_repaid, el_forgiven;
    double   el_wait_ms, el_wait_max_ms;
} audio_health_t;
static audio_health_t s_h;
typedef struct {
    unsigned prev_lead;        /* lead at the previous mix */
    unsigned zero_run;         /* consecutive all-zero blocks consumed */
    unsigned since_sound;      /* blocks since the last non-zero block */
    unsigned zr_lead0;         /* lead when the current zero run began */
    int      had_sound;        /* any non-zero block yet */
    unsigned unw_run;          /* never-written blocks in the current zero run */
    double   last_write_ms;    /* host time the producer last added a block */
} audio_port_health_t;
static audio_port_health_t s_ph[CELL_AUDIO_PORT_MAX];

static double audio_now_ms(void)
{
#ifdef _WIN32
    static LARGE_INTEGER f; LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
#else
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
#endif
}

static int s_sentinel = 0;   /* AUDIO_SENTINEL=1 (diagnostic) */
/* -0.0f in guest (BE) byte order, as a host u32: the 'consumed, not yet rewritten' mark. */
static int audio_block_written(const float* b, u32 n)
{
    const u32* w = (const u32*)b;
    for (u32 i = 0; i < n; i++) if (w[i] != 0x00000080u) return 1;
    return 0;
}

static int audio_block_nonzero(const float* b, u32 n)
{
    /* Raw bit test on BE floats: any set bit except the sign = non-zero. The
     * BE sign bit is the top bit of byte 0 = bit 7 of the host (LE) word. */
    const u32* w = (const u32*)b;
    for (u32 i = 0; i < n; i++) if (w[i] & 0xFFFFFF7Fu) return 1;
    return 0;
}

/* Fix switches (all default off, AUDIO-NOTES.md):
 *   AUDIO_LIMIT=1   a smooth peak limiter in place of the hard clamp at +-1.0
 *                   (instant attack per sample, ~150 ms release, linked L/R).
 *   AUDIO_HEADROOM=<dB>  fixed gain on the 8ch->2ch downmix (e.g. -6). */
static int   s_limit = -1;
static int   s_wait_ms = 0;   /* AUDIO_WAIT_MS */
static float s_headroom = 1.0f;
static float s_lim_gain = 1.0f;

/* AUDIO_ELASTIC=1 (off; host\aud2\NOTES.md): never play a port block the game
 * has not written yet while the device still holds sound. On a CPU at 100 %
 * the game's sound job writes its blocks late; the mixer used to consume one
 * block whenever WASAPI had room, so a block not yet written played as
 * silence (a 'hole', 5-43 ms). With the switch:
 *   - consumed blocks are refilled with -0.0f (the AUDIO_SENTINEL mark, as
 *     RPCS3 tags them), so 'not written yet' differs from silence;
 *   - for a running port that wrote within ~1 s, an unwritten block at the
 *     cursor is neither mixed nor consumed: the mixer waits in 0.5 ms steps
 *     while WASAPI still holds at least the floor (one engine period + one
 *     block), for every block of a hole; it goes on at once when a later
 *     ring block is written (the producer skipped this one), and after
 *     AUDIO_ELASTIC_CAP_MS (150) for one block it gives up: today's
 *     behaviour, and the port counts as quiet (no more waits) until the game
 *     writes a block again;
 *   - at the floor with the block still missing it writes concealment to
 *     WASAPI without consuming the game's block (the last output sample faded
 *     to 0 over 128 frames, then silence a block at a time); the late block
 *     still plays when it arrives. Every port's first written block after
 *     concealment, or after it played an unwritten block (a give-up or a
 *     skip), is faded in over 128 frames;
 *   - each block concealed leaves the cellAudio read index one block behind
 *     real time (a clock offset, not added output latency: that stays the
 *     ring lead + the device buffer). Up to AUDIO_ELASTIC_DEBT (8) such
 *     blocks are repaid by dropping blocks the game WROTE as digital silence
 *     (or a quiet port's unwritten ones), at most one per block mixed; past
 *     that limit, and all of it at a give-up, the offset is forgiven (as in
 *     RPCS3). No written sound is ever dropped;
 *   - AUDIO_ELASTIC_TS=1 (off, untested: Ascension never calls it in play):
 *     cellAudioGetPortTimestamp returns the QPC time of each read-index
 *     advance (RPCS3 cellAudio.cpp: the last advance minus the tag delta)
 *     instead of start + tag * period.
 * Best with AUDIO_BUF_MS=60 (the reserve to wait on: ~35-45 ms). The health
 * line gets "| elastic on saved N wait N (ms, max) conceal N (ms) ...".
 * AUDIO_ELASTIC_AB=<ms>: the waits on and off in turns of <ms> (whole health
 * windows; for an A/B in one run; the -0.0f fill stays on). */
static int    s_elastic = 0;          /* AUDIO_ELASTIC=1 */
static int    s_el_on = 0;            /* the waits active now (AUDIO_ELASTIC_AB turns) */
static int    s_el_ab_ms = 0;         /* AUDIO_ELASTIC_AB */
static int    s_el_ts = 0;            /* AUDIO_ELASTIC_TS=1: real advance times for cellAudioGetPortTimestamp */
static double s_el_cap_ms = 150.0;    /* AUDIO_ELASTIC_CAP_MS */
static int    s_el_floor = 0;         /* frames; AUDIO_ELASTIC_FLOOR (0 = engine period + one block) */
static float  s_el_last_l, s_el_last_r;   /* last output frame */
static unsigned s_el_debt = 0;        /* concealed blocks not yet repaid */
static unsigned s_el_debt_max = 8;    /* AUDIO_ELASTIC_DEBT */
static int    s_el_repay_ok = 0;      /* a block was mixed since the last repayment */
#define AUDIO_EL_TS_N 64
typedef struct {
    double   last_written_ms;   /* host time a written block was last seen at the cursor */
    int      quiet;             /* gave up on this port: no waits until it writes again */
    int      fade_in;           /* fade this port's next written block in (after concealment / an unwritten block) */
    double   wait_t0;           /* start of the wait for the block at the cursor (0 = none) */
    u64      wait_ridx;         /* the read index that wait is for */
    u64      ts[AUDIO_EL_TS_N]; /* AUDIO_ELASTIC_TS: QPC us at which read_index became each value */
    u64      ts_last;           /* the latest of them */
} audio_el_port_t;
static audio_el_port_t s_el[CELL_AUDIO_PORT_MAX];
static float s_el_cbuf[CELL_AUDIO_BLOCK_SAMPLES * 2];
/* AUDIO_CONCEAL_REPEAT=1 (q4 sound, 2026-11-10; off by default = the fade to silence below, unchanged): a block
 * of concealment repeats the last block mixed (joined to the last frame played over 64 frames) instead of
 * fading to silence -- up to 3 in a row at gain 1, 0.6, 0.3, then the old fade. After the 1st repeat (gain 1)
 * no fade-in is needed (the late block follows the repeat's end as it followed the original's); a quieter one
 * fades out over its last 128 frames and the next block fades in, as after the old concealment.
 * Q4_SND_AB=1: only in every other [draws] window (g_q4_snd_on, rsx_live_draw.c). */
static int   s_el_rep = -1;           /* AUDIO_CONCEAL_REPEAT */
static float s_el_prev[CELL_AUDIO_BLOCK_SAMPLES * 2];   /* the last block mixed */
static int   s_el_prev_ok;            /* s_el_prev holds a block */
static unsigned s_el_rep_n;           /* repeats since the last block mixed */
static unsigned s_el_rep_count;       /* repeats this health window ([audio] "rep N") */
static volatile unsigned long g_audio_ts_calls, g_audio_tag_calls;   /* cellAudioGetPortTimestamp / GetPortBlockTag calls */

static u64 audio_qpc_us(void)
{
#ifdef _WIN32
    LARGE_INTEGER f, c; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c);
    return (u64)(c.QuadPart / f.QuadPart) * 1000000ULL + (u64)(c.QuadPart % f.QuadPart) * 1000000ULL / (u64)f.QuadPart;
#else
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64)ts.tv_sec * 1000000ULL + (u64)ts.tv_nsec / 1000ULL;
#endif
}

/* AUDIO_ELASTIC: 0 = every word still -0.0f (not written since consumed),
 * 1 = written (its last 16 frames hold data), 2 = written at the head only
 * (the producer is writing it right now). */
static inline int audio_block_state(const float* b, u32 bs, u32 nch)
{
    const u32* w = (const u32*)b;
    const u32 tail = bs > 16 * nch ? bs - 16 * nch : 0;
    for (u32 i = tail; i < bs; i++) if (w[i] != 0x00000080u) return 1;
    for (u32 i = 0; i < tail; i++) if (w[i] != 0x00000080u) return 2;
    return 0;
}

/* ---------------------------------------------------------------------------
 * Host audio output backend
 * -----------------------------------------------------------------------*/

#if AUDIO_BACKEND_SDL2

static SDL_AudioDeviceID s_sdl_audio_dev = 0;

static int audio_backend_init(void)
{
    if (SDL_WasInit(SDL_INIT_AUDIO) == 0) {
        if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0) {
            printf("[cellAudio] SDL audio init failed: %s\n", SDL_GetError());
            return -1;
        }
    }

    SDL_AudioSpec want, have;
    memset(&want, 0, sizeof(want));
    want.freq     = CELL_AUDIO_SAMPLE_RATE;
    want.format   = AUDIO_F32SYS;
    want.channels = 2;
    want.samples  = CELL_AUDIO_BLOCK_SAMPLES;
    want.callback = NULL; /* We'll use SDL_QueueAudio */

    s_sdl_audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (s_sdl_audio_dev == 0) {
        printf("[cellAudio] SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
        return -1;
    }

    SDL_PauseAudioDevice(s_sdl_audio_dev, 0); /* Start playback */
    return 0;
}

static void audio_backend_shutdown(void)
{
    if (s_sdl_audio_dev) {
        SDL_CloseAudioDevice(s_sdl_audio_dev);
        s_sdl_audio_dev = 0;
    }
}

static void audio_backend_submit(const float* stereo_samples, u32 num_samples)
{
    if (s_sdl_audio_dev) {
        SDL_QueueAudio(s_sdl_audio_dev, stereo_samples,
                       num_samples * 2 * sizeof(float));
    }
}

/* SDL queues without bound: keep about four blocks ahead of the device. */
static int audio_backend_room(void)
{
    if (!s_sdl_audio_dev) return -1;
    return SDL_GetQueuedAudioSize(s_sdl_audio_dev) / (2 * sizeof(float)) <
           CELL_AUDIO_BLOCK_SAMPLES * 4 ? CELL_AUDIO_BLOCK_SAMPLES : 0;
}

static void audio_backend_wait(void) { SDL_Delay(1); }

static u32 audio_backend_queued_samples(void)
{
    if (s_sdl_audio_dev) {
        return SDL_GetQueuedAudioSize(s_sdl_audio_dev) / (2 * sizeof(float));
    }
    return 0;
}

#endif /* AUDIO_BACKEND_SDL2 */

#if AUDIO_BACKEND_WASAPI

static unsigned long        s_dev_fail = 0;   /* GetCurrentPadding failures (device lost) */
static void audio_backend_shutdown(void);
static int  audio_backend_init(void);
static IAudioClient*        s_wasapi_client = NULL;
static IAudioRenderClient*  s_wasapi_render = NULL;
static HANDLE               s_wasapi_event  = NULL;
static UINT32               s_wasapi_buf_frames = 0;
static UINT32               s_wasapi_period_frames = 480;   /* engine period (GetDevicePeriod), AUDIO_ELASTIC's floor */

static int audio_backend_init(void)
{
    HRESULT hr;

    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != S_FALSE && hr != RPC_E_CHANGED_MODE) {
        printf("[cellAudio] CoInitializeEx failed: 0x%08lX\n", hr);
        return -1;
    }

    IMMDeviceEnumerator* enumerator = NULL;
    hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                          &IID_IMMDeviceEnumerator, (void**)&enumerator);
    if (FAILED(hr)) {
        printf("[cellAudio] Failed to create device enumerator: 0x%08lX\n", hr);
        return -1;
    }

    IMMDevice* device = NULL;
    hr = enumerator->lpVtbl->GetDefaultAudioEndpoint(enumerator, eRender,
                                                      eConsole, &device);
    enumerator->lpVtbl->Release(enumerator);
    if (FAILED(hr)) {
        printf("[cellAudio] Failed to get default audio endpoint: 0x%08lX\n", hr);
        return -1;
    }

    hr = device->lpVtbl->Activate(device, &IID_IAudioClient, CLSCTX_ALL,
                                   NULL, (void**)&s_wasapi_client);
    device->lpVtbl->Release(device);
    if (FAILED(hr)) {
        printf("[cellAudio] Failed to activate audio client: 0x%08lX\n", hr);
        return -1;
    }

    WAVEFORMATEX wfx;
    wfx.wFormatTag      = WAVE_FORMAT_IEEE_FLOAT;
    wfx.nChannels       = 2;
    wfx.nSamplesPerSec  = CELL_AUDIO_SAMPLE_RATE;
    wfx.wBitsPerSample  = 32;
    wfx.nBlockAlign     = wfx.nChannels * (wfx.wBitsPerSample / 8);
    wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;
    wfx.cbSize          = 0;

    /* Request event-driven mode with a ~20ms buffer */
    REFERENCE_TIME buf_duration = 200000; /* 20ms in 100ns units */
    /* AUDIO_BUF_MS=<ms> (off = 20): a longer device buffer, more margin
     * against a late mixing thread (the mixer keeps it full). */
    { const char* e = getenv("AUDIO_BUF_MS");
      if (e && atoi(e) >= 10 && atoi(e) <= 200) buf_duration = (REFERENCE_TIME)atoi(e) * 10000; }
    s_wasapi_event = CreateEventW(NULL, FALSE, FALSE, NULL);

    /* The device's own (mix) format, for the log: a shared-mode stream in any
     * other rate or channel count is refused unless the audio engine may
     * convert, which is what AUTOCONVERTPCM asks for (Windows 7+). Without it
     * a tester's device at 44.1 kHz (or 5.1/7.1) answered
     * AUDCLNT_E_UNSUPPORTED_FORMAT (0x88890008) and the game ran silent
     * (2026-11-03). We keep writing 48 kHz stereo float either way. */
#ifndef AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
#define AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM 0x80000000
#endif
#ifndef AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY
#define AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY 0x08000000
#endif
    unsigned mix_ch = 2; DWORD mix_mask = 0;
    { WAVEFORMATEX* mix = NULL;
      if (SUCCEEDED(s_wasapi_client->lpVtbl->GetMixFormat(s_wasapi_client, &mix)) && mix) {
          printf("[cellAudio] device mix format: %lu Hz, %u channels, %u bits (tag 0x%X)\n",
                 (unsigned long)mix->nSamplesPerSec, mix->nChannels, mix->wBitsPerSample, mix->wFormatTag);
          mix_ch = mix->nChannels;
          if (mix->wFormatTag == 0xFFFEu && mix->cbSize >= 22)
              memcpy(&mix_mask, (const BYTE*)mix + 20, 4);   /* WAVEFORMATEXTENSIBLE.dwChannelMask */
          CoTaskMemFree(mix);
      } }
    const DWORD convert = AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;

    /* SURROUND: a 5.1 float stream when the device has 6+ channels (see s_mix_mc) */
    s_out_ch = 2;
    { const char* e = getenv("AUDIO_SURROUND"); const char* o = getenv("AUDIO_OUT_CH");
      int want = (e && *e == '0') || (o && atoi(o) == 2) ? 0 : (e && *e == '1') ? 1 : mix_ch >= 6;
      if (want) {
#pragma pack(push, 1)
          struct { WAVEFORMATEX f; WORD valid; DWORD mask; GUID sub; } x;
#pragma pack(pop)
          static const GUID float_sub = { 0x00000003, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };
          memset(&x, 0, sizeof x);
          x.f.wFormatTag = 0xFFFEu;   /* WAVE_FORMAT_EXTENSIBLE */
          x.f.nChannels = 6; x.f.nSamplesPerSec = CELL_AUDIO_SAMPLE_RATE; x.f.wBitsPerSample = 32;
          x.f.nBlockAlign = 6 * 4; x.f.nAvgBytesPerSec = x.f.nSamplesPerSec * x.f.nBlockAlign;
          x.f.cbSize = 22; x.valid = 32;
          x.mask = mix_ch == 6 && mix_mask ? mix_mask : 0x60Fu;   /* the device's own 5.1 layout, else FL FR FC LFE SL SR */
          x.sub = float_sub;
          hr = s_wasapi_client->lpVtbl->Initialize(s_wasapi_client, AUDCLNT_SHAREMODE_SHARED,
                  AUDCLNT_STREAMFLAGS_EVENTCALLBACK | convert, buf_duration, 0, &x.f, NULL);
          if (SUCCEEDED(hr)) {
              s_out_ch = 6;
              printf("[cellAudio] output: 5.1 surround (6 channels, mask 0x%lX; device %u channels; AUDIO_SURROUND=0: stereo)%c",
                     (unsigned long)x.mask, mix_ch, 10);
          } else
              printf("[cellAudio] 5.1 stream refused (0x%08lX): stereo%c", (unsigned long)hr, 10);
      } else
          printf("[cellAudio] output: stereo (device %u channels)%c", mix_ch, 10);
    }

    if (s_out_ch == 2)
    hr = s_wasapi_client->lpVtbl->Initialize(
        s_wasapi_client,
        AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_EVENTCALLBACK | convert,
        buf_duration, 0, &wfx, NULL);

    if (FAILED(hr)) {
        printf("[cellAudio] WASAPI event-driven Initialize failed: 0x%08lX, trying timer-driven\n", hr);
        /* Fallback: try without event callback */
        hr = s_wasapi_client->lpVtbl->Initialize(
            s_wasapi_client,
            AUDCLNT_SHAREMODE_SHARED,
            convert, buf_duration, 0, &wfx, NULL);
        if (FAILED(hr)) {
            printf("[cellAudio] WASAPI Initialize failed: 0x%08lX\n", hr);
            s_wasapi_client->lpVtbl->Release(s_wasapi_client);
            s_wasapi_client = NULL;
            CloseHandle(s_wasapi_event);
            s_wasapi_event = NULL;
            return -1;
        }
        CloseHandle(s_wasapi_event);
        s_wasapi_event = NULL;
    } else {
        s_wasapi_client->lpVtbl->SetEventHandle(s_wasapi_client, s_wasapi_event);
    }

    s_wasapi_client->lpVtbl->GetBufferSize(s_wasapi_client, &s_wasapi_buf_frames);
    { REFERENCE_TIME defp = 0, minp = 0;
      s_wasapi_client->lpVtbl->GetDevicePeriod(s_wasapi_client, &defp, &minp);
      if (defp > 0 && defp <= 1000000) s_wasapi_period_frames = (UINT32)(defp * CELL_AUDIO_SAMPLE_RATE / 10000000);
      printf("[cellAudio] device buffer %u frames (%.1f ms), engine period %.1f ms\n",
             (unsigned)s_wasapi_buf_frames, s_wasapi_buf_frames / 48.0, defp / 10000.0); }

    hr = s_wasapi_client->lpVtbl->GetService(
        s_wasapi_client, &IID_IAudioRenderClient, (void**)&s_wasapi_render);
    if (FAILED(hr)) {
        printf("[cellAudio] Failed to get render client: 0x%08lX\n", hr);
        s_wasapi_client->lpVtbl->Release(s_wasapi_client);
        s_wasapi_client = NULL;
        return -1;
    }

    s_wasapi_client->lpVtbl->Start(s_wasapi_client);
    return 0;
}

static void audio_backend_shutdown(void)
{
    if (s_wasapi_client) {
        s_wasapi_client->lpVtbl->Stop(s_wasapi_client);
    }
    if (s_wasapi_render) {
        s_wasapi_render->lpVtbl->Release(s_wasapi_render);
        s_wasapi_render = NULL;
    }
    if (s_wasapi_client) {
        s_wasapi_client->lpVtbl->Release(s_wasapi_client);
        s_wasapi_client = NULL;
    }
    if (s_wasapi_event) {
        CloseHandle(s_wasapi_event);
        s_wasapi_event = NULL;
    }
}

static void audio_backend_submit(const float* stereo_samples, u32 num_samples)
{
    if (!s_wasapi_render) return;

    UINT32 padding = 0;
    s_wasapi_client->lpVtbl->GetCurrentPadding(s_wasapi_client, &padding);

    UINT32 available = s_wasapi_buf_frames - padding;
    if (num_samples > available) {
        g_audio_dropped += num_samples - available;
        num_samples = available;
    }
    if (num_samples == 0) return;

    BYTE* buf = NULL;
    HRESULT hr = s_wasapi_render->lpVtbl->GetBuffer(s_wasapi_render, num_samples, &buf);
    if (SUCCEEDED(hr) && buf) {
        /* AUDIO_MUTE=1 (the test scripts set it): the same buffers at the same
         * pace, flagged silent, so a muted run keeps the audio clock and its
         * timing exactly as an audible one. */
        static int mute = -1;
        if (mute < 0) { const char* e = getenv("AUDIO_MUTE"); mute = e && *e == '1'; }
        if (mute) { }
        else if (s_out_ch == 6 && stereo_samples == s_mix_buffer)   /* SURROUND: the mixed 5.1 block */
            memcpy(buf, s_mix_mc, num_samples * 6 * sizeof(float));
        else if (s_out_ch == 6) {   /* a stereo fill (AUDIO_ELASTIC): on the front pair */
            float* o = (float*)buf;
            for (u32 i = 0; i < num_samples; i++) {
                o[i * 6 + 0] = stereo_samples[i * 2]; o[i * 6 + 1] = stereo_samples[i * 2 + 1];
                o[i * 6 + 2] = o[i * 6 + 3] = o[i * 6 + 4] = o[i * 6 + 5] = 0.0f;
            }
        } else memcpy(buf, stereo_samples, num_samples * 2 * sizeof(float));
        s_wasapi_render->lpVtbl->ReleaseBuffer(s_wasapi_render, num_samples,
                                               mute ? 0x2u /* AUDCLNT_BUFFERFLAGS_SILENT */ : 0);
    }
}

/* Frames the device can take right now; -1 with no device open. */
static int audio_backend_room(void)
{
    if (!s_wasapi_client) return -1;
    UINT32 padding = 0;
    const HRESULT hr = s_wasapi_client->lpVtbl->GetCurrentPadding(s_wasapi_client, &padding);
    if (FAILED(hr)) {
        /* Device gone (AUDCLNT_E_DEVICE_INVALIDATED on a default-device
         * change, sleep, ...): pace on the clock rather than spin. */
        static int _n = 0; if (_n < 4 && ++_n)
            fprintf(stderr, "[cellAudio] GetCurrentPadding failed 0x%08lX -- clock pacing%c", (unsigned long)hr, 10);
        s_dev_fail++;
        /* AUDIO_REOPEN=1 (off): open the (new) default device again, once a
         * second, instead of staying silent until the game is restarted. */
        { static int on = -1; static ULONGLONG last;
          if (on < 0) { const char* e = getenv("AUDIO_REOPEN"); on = e && *e == '1'; }
          const ULONGLONG now = GetTickCount64();
          if (on && now - last >= 1000) {
              last = now;
              audio_backend_shutdown();
              const int ok = audio_backend_init() == 0;
              fprintf(stderr, "[cellAudio] device lost (0x%08lX): reopened %s%c", (unsigned long)hr, ok ? "ok" : "FAILED", 10);
          } }
        return -1;
    }
    return (int)(s_wasapi_buf_frames - padding);
}

/* Block until the device has consumed some of its buffer. */
static void audio_backend_wait(void)
{
    if (s_wasapi_event) WaitForSingleObject(s_wasapi_event, 10);
    else Sleep(1);
}


#endif /* AUDIO_BACKEND_WASAPI */

/* ---------------------------------------------------------------------------
 * Mixing
 * -----------------------------------------------------------------------*/

/* Guest PCM is BIG-ENDIAN float32; the port buffer is a raw vm_base view, so
 * every sample must be byte-swapped before use. Reading it as a host float
 * turned the whole mix into denormal noise -- silence after clipping (LBP
 * had a verified end-to-end pipeline with no audible output). */
static inline float ld_be_f32(const float* p)
{
    u32 v;
    memcpy(&v, p, 4);
    v = (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24);
    float f;
    memcpy(&f, &v, 4);
    return f;
}

/* Mix one block from all active ports into s_mix_buffer (stereo float) */
static void audio_mix_one_block(void)
{
    memset(s_mix_buffer, 0, sizeof(s_mix_buffer));
    if (s_out_ch == 6) memset(s_mix_mc, 0, sizeof(s_mix_mc));

    mutex_lock(&s_audio_mutex);

    for (int p = 0; p < CELL_AUDIO_PORT_MAX; p++) {
        AudioPortSlot* port = &s_ports[p];
        /* Diagnostic: a port that HOLDS PCM but was never started is inaudible
         * by design -- flag it once so "no PortStart" vs "no data" is decidable
         * from the log (LBP's Bink movie audio port sat exactly there). */
        if (port->in_use && !port->running && port->buffer) {
            static u8 warned[CELL_AUDIO_PORT_MAX];
            if (!warned[p]) {
                u32 probe = (u32)(port->param.nBlock * CELL_AUDIO_BLOCK_SAMPLES *
                                  port->param.nChannel);
                int nz = 0;
                for (u32 s = 0; s < probe; s += 64) if (port->buffer[s] != 0.0f) { nz = 1; break; }
                if (nz) {
                    warned[p] = 1;
                    fprintf(stderr, "[cellAudio] port %d HAS DATA but never started"
                            " -- game withheld PortStart\n", p);
                }
            }
        }
        if (!port->in_use || !port->running || !port->buffer)
            continue;

        u32 nch    = (u32)port->param.nChannel;
        u32 nblock = (u32)port->param.nBlock;
        float level = port->param.level;
        if (level <= 0.0f) level = 1.0f;
        if (level > 1.0f) level = 1.0f;

        /* Read one block at the current read_index */
        u32 block_idx = (u32)(port->read_index % nblock);
        u32 block_offset = block_idx * CELL_AUDIO_BLOCK_SAMPLES * nch;
        float* src = port->buffer + block_offset;

        /* Health: how far ahead the producer is, holes, torn blocks, stalls. */
        if (s_health) {
            const u32 bs = CELL_AUDIO_BLOCK_SAMPLES * nch;
            audio_port_health_t* ph = &s_ph[p];
            unsigned lead = 0, slead = 0;
            while (slead < nblock &&
                   audio_block_nonzero(port->buffer + ((block_idx + slead) % nblock) * bs, bs)) slead++;
            /* AUDIO_SENTINEL=1: consumed blocks hold -0.0f, so 'never written' and 'written as silence'
             * differ; the producer's lead is then counted in written blocks. */
            if (s_sentinel) {
                while (lead < nblock &&
                       audio_block_written(port->buffer + ((block_idx + lead) % nblock) * bs, bs)) lead++;
                if (!lead) { s_h.unwritten++; ph->unw_run++; }
            } else lead = slead;
            const double now = audio_now_ms();
            const int playing = ph->since_sound < 200;     /* sound within ~1 s */
            const unsigned left = ph->prev_lead ? ph->prev_lead - 1 : 0;
            if (lead > left || !ph->last_write_ms) ph->last_write_ms = now;
            else if (playing) {
                const double st = now - ph->last_write_ms;
                if (st > s_h.stall_max_ms) s_h.stall_max_ms = st;
            }
            ph->prev_lead = lead;
            if (playing) {
                s_h.lead_n++; s_h.lead_sum += lead;
                if (lead < s_h.lead_min) s_h.lead_min = lead;
                s_h.lead_h[lead < 3 ? lead : 3]++;
            }
            if (slead == 0) {
                if (!ph->zero_run) { ph->zr_lead0 = left; ph->unw_run = s_sentinel && !lead; }
                ph->zero_run++;
                ph->since_sound++;
            } else {
                if (ph->zero_run && ph->had_sound) {   /* sound before the run */
                    const double ms = ph->zero_run * 256000.0 / 48000.0;
                    if (ms <= 100.0) { s_h.holes++; s_h.hole_blocks += ph->zero_run; }
                    else {
                        s_h.silences++;
                        if (ms > s_h.silence_max_ms) s_h.silence_max_ms = ms;
                        if (ms >= 300.0) {
                            static int n_sil = 0;
                            if (n_sil < 200 && ++n_sil)
                                fprintf(stderr, s_sentinel ? "[audio] port %d: %.0f ms of silence ended at %.1f s (lead before it %u, %u of %u blocks never written)\n" : "[audio] port %d: %.0f ms of silence ended at %.1f s (lead before it %u)\n",
                                        p, ms, (now - s_audio_start_us / 1000.0) / 1000.0, ph->zr_lead0, ph->unw_run, ph->zero_run);
                        }
                    }
                }
                ph->zero_run = 0; ph->unw_run = 0; ph->since_sound = 0; ph->had_sound = 1;
                /* Zero tail (last 16 frames) under a non-zero head: torn. */
                if (!audio_block_nonzero(src + (CELL_AUDIO_BLOCK_SAMPLES - 16) * nch, 16 * nch)) s_h.torn++;
            }
        }

        /* AUDIO_PEAK diag: which blocks of this port's WHOLE ring hold data,
         * vs which block the read cursor is on -- distinguishes "writer never
         * writes" from "writer and reader cycle out of phase". */
        { static int s_pk = -1; if (s_pk < 0) s_pk = getenv("AUDIO_PEAK") ? 1 : 0;
          if (s_pk) { static _Thread_local unsigned _c;
            if ((++_c % 400) == 0) {
                char map[36]; u32 b;
                for (b = 0; b < nblock && b < 32; b++) {
                    const float* bp = port->buffer + b * CELL_AUDIO_BLOCK_SAMPLES * nch;
                    int nz = 0;
                    for (u32 s2 = 0; s2 < CELL_AUDIO_BLOCK_SAMPLES * nch; s2 += 16)
                        if (bp[s2] != 0.0f) { nz = 1; break; }
                    map[b] = nz ? '#' : '.';
                }
                map[b] = 0;
                fprintf(stderr, "[audio-ring] port %d ridx=%llu blk=%u ring=[%s]\n",
                        p, (unsigned long long)port->read_index, block_idx, map);
            } } }

        /* AUDIO_ELASTIC: an unwritten block plays as silence; this port's
         * next written block is faded in (no step at the end of a hole). */
        int el_fade = 0;
        if (s_elastic) {
            audio_el_port_t* e = &s_el[p];
            if (!audio_block_written(src, CELL_AUDIO_BLOCK_SAMPLES * nch)) e->fade_in = 1;
            else if (e->fade_in) { e->fade_in = 0; el_fade = s_el_on; }
        }

        for (u32 s = 0; s < CELL_AUDIO_BLOCK_SAMPLES; s++) {
            float left, right;
            if (nch >= 2) {
                left  = ld_be_f32(&src[s * nch + 0]) * level;
                right = ld_be_f32(&src[s * nch + 1]) * level;
            } else {
                /* Mono: duplicate to both channels */
                left = right = ld_be_f32(&src[s]) * level;
            }

            if (s_health == 2) {
                for (u32 c = 0; c < nch && c < 8; c++) {
                    float a = ld_be_f32(&src[s * nch + c]); if (a < 0) a = -a;
                    if (a > s_h.ch_peak[c]) s_h.ch_peak[c] = a;
                }
            }
            /* If 7.1, mix center and other channels into stereo */
            if (nch == 8) {
                float center = ld_be_f32(&src[s * 8 + 2]) * level * 0.707f;
                float lfe    = ld_be_f32(&src[s * 8 + 3]) * level * 0.5f;
                float rl     = ld_be_f32(&src[s * 8 + 4]) * level * 0.5f;
                float rr     = ld_be_f32(&src[s * 8 + 5]) * level * 0.5f;
                float sl     = ld_be_f32(&src[s * 8 + 6]) * level * 0.3f;
                float sr     = ld_be_f32(&src[s * 8 + 7]) * level * 0.3f;
                left  += center + lfe + rl + sl;
                right += center + lfe + rr + sr;
                left *= s_headroom; right *= s_headroom;
            }
            if (el_fade && s < 128) { const float g = (float)(s + 1) / 128.0f; left *= g; right *= g; }
            if (s_out_ch == 6) {   /* SURROUND: the port's channels as they are */
                float* m = &s_mix_mc[s * 6];
                const float g = level * (el_fade && s < 128 ? (float)(s + 1) / 128.0f : 1.0f);
                if (nch >= 6) {
                    for (u32 c = 0; c < 6; c++) m[c] += ld_be_f32(&src[s * nch + c]) * g;
                    if (nch >= 8) {
                        m[4] += ld_be_f32(&src[s * nch + 6]) * g * 0.707f;
                        m[5] += ld_be_f32(&src[s * nch + 7]) * g * 0.707f;
                    }
                } else if (nch >= 2) {
                    m[0] += ld_be_f32(&src[s * nch + 0]) * g; m[1] += ld_be_f32(&src[s * nch + 1]) * g;
                } else {
                    const float v = ld_be_f32(&src[s]) * g; m[0] += v; m[1] += v;
                }
            }

            s_mix_buffer[s * 2 + 0] += left;
            s_mix_buffer[s * 2 + 1] += right;
        }

        /* Firmware clears a ring-buffer block after consuming it. Besides
         * preventing stale PCM from replaying when a producer stalls, the
         * cleared block is an observable consumption signal for producers. */
        if (s_sentinel) { u32* w = (u32*)src; for (u32 i = 0; i < CELL_AUDIO_BLOCK_SAMPLES * nch; i++) w[i] = 0x00000080u; }
        else memset(src, 0, CELL_AUDIO_BLOCK_SAMPLES * nch * sizeof(float));

        /* Keep the monotonic counter for tags/timestamps, but publish the
         * ring slot to the guest. SurMixer multiplies this value by the block
         * size directly; an unbounded counter walks beyond the audio buffer. */
        port->read_index++;
        if (port->read_idx_addr)
            vm_write64((u32)port->read_idx_addr, port->read_index % nblock);
        if (s_el_ts) {   /* AUDIO_ELASTIC_TS: real time of each advance (cellAudioGetPortTimestamp) */
            const u64 t = audio_qpc_us();
            s_el[p].ts[port->read_index % AUDIO_EL_TS_N] = t; s_el[p].ts_last = t;
        }
    }

    mutex_unlock(&s_audio_mutex);

    /* Health: output over full scale before the clamp. */
    if (s_health) {
        float pk = s_h.peak;
        for (u32 i = 0; i < CELL_AUDIO_BLOCK_SAMPLES * 2; i++) {
            float a = s_mix_buffer[i]; if (a < 0) a = -a;
            if (a > pk) pk = a;
            if (a > 1.0f) s_h.clip++;
        }
        s_h.peak = pk;
    }

    /* AUDIO_LIMIT=1: a peak limiter (linked stereo, instant attack, smooth
     * release) brings the sum under full scale without the square-topped
     * waveforms the clamp makes; the clamp below then never acts. */
    if (s_limit > 0) {
        const float thr = 0.98f;
        const float rel = 1.0f - 1.0f / (0.150f * CELL_AUDIO_SAMPLE_RATE);   /* ~150 ms */
        float g = s_lim_gain;
        for (u32 s = 0; s < CELL_AUDIO_BLOCK_SAMPLES; s++) {
            float l = s_mix_buffer[s * 2], r = s_mix_buffer[s * 2 + 1];
            float a = fabsf(l) > fabsf(r) ? fabsf(l) : fabsf(r);
            float want = a * g > thr ? thr / a : 1.0f;
            if (want < g) g = want;                       /* attack: at once */
            else g = 1.0f - (1.0f - g) * rel;             /* release toward 1 */
            if (a * g > thr) g = thr / a;
            s_mix_buffer[s * 2] = l * g; s_mix_buffer[s * 2 + 1] = r * g;
        }
        s_lim_gain = g;
    }

    if (s_out_ch == 6)   /* SURROUND: clip the 5.1 block too */
        for (u32 i = 0; i < CELL_AUDIO_BLOCK_SAMPLES * 6; i++) {
            if (s_mix_mc[i] > 1.0f) s_mix_mc[i] = 1.0f;
            if (s_mix_mc[i] < -1.0f) s_mix_mc[i] = -1.0f;
        }
    /* Clip to [-1.0, 1.0] */
    for (u32 i = 0; i < CELL_AUDIO_BLOCK_SAMPLES * 2; i++) {
        if (s_mix_buffer[i] > 1.0f) s_mix_buffer[i] = 1.0f;
        if (s_mix_buffer[i] < -1.0f) s_mix_buffer[i] = -1.0f;
    }
}

/* AUDIO_WAV=<file>: append the final output as raw f32le stereo 48 kHz
 * (ffmpeg -f f32le -ar 48000 -ac 2 -i <file>). */
static void audio_wav_out(const float* b)
{
    static FILE* _wf = (FILE*)-1;
    if (_wf == (FILE*)-1) { const char* e = getenv("AUDIO_WAV"); _wf = e ? fopen(e, "wb") : NULL; }
    if (_wf) {   /* SURROUND: the 5.1 stream as sent (-ac 6) */
        if (s_out_ch == 6 && b == s_mix_buffer) fwrite(s_mix_mc, sizeof(float), CELL_AUDIO_BLOCK_SAMPLES * 6, _wf);
        else fwrite(b, sizeof(float), CELL_AUDIO_BLOCK_SAMPLES * 2, _wf);
    }
}

#if defined(_WIN32) && AUDIO_BACKEND_WASAPI
/* AUDIO_ELASTIC (see the switch at the top). AUDIO_EL_WAIT: a port that is
 * feeding sound has no block at its cursor yet and waiting can still help
 * (the caller waits while the device holds the floor, else conceals). */
enum { AUDIO_EL_MIX = 0, AUDIO_EL_WAIT = 1 };
static void audio_notify_event_queues(void);
static int audio_el_active(const audio_el_port_t* e, double now)
{
    return !e->quiet && e->last_written_ms > 0 && now - e->last_written_ms <= 1000.0;
}
static int audio_elastic_gate(void)
{
    const double now = audio_now_ms();
    int late = 0;
    mutex_lock(&s_audio_mutex);
    for (int p = 0; p < CELL_AUDIO_PORT_MAX; p++) {
        AudioPortSlot* port = &s_ports[p];
        audio_el_port_t* e = &s_el[p];
        if (!port->in_use || !port->running || !port->buffer) { e->wait_t0 = 0; continue; }
        const u32 nch = (u32)port->param.nChannel, nblock = (u32)port->param.nBlock;
        if (!nblock || port->read_index < nblock) continue;   /* first lap: PortOpen's zeros */
        const u32 bs = CELL_AUDIO_BLOCK_SAMPLES * nch;
        const u32 bi = (u32)(port->read_index % nblock);
        const int st = audio_block_state(port->buffer + bi * bs, bs, nch);
        if (st == 1) {
            if (e->wait_t0) {   /* it came while the mixer waited: saved */
                const double w = now - e->wait_t0;
                s_h.el_saved++; s_h.el_wait_ms += w;
                if (w > s_h.el_wait_max_ms) s_h.el_wait_max_ms = w;
                e->wait_t0 = 0;
            }
            e->last_written_ms = now; e->quiet = 0;
            continue;
        }
        if (st == 2) { e->last_written_ms = now; e->quiet = 0; }   /* being written now */
        if (!audio_el_active(e, now)) { e->wait_t0 = 0; continue; }
        if (st == 0) {   /* a later block written, this one not: the producer skipped it */
            int ahead = 0;
            for (u32 k = 1; k < nblock && !ahead; k++)
                ahead = audio_block_written(port->buffer + ((bi + k) % nblock) * bs, bs);
            if (ahead) { s_h.el_skip++; e->wait_t0 = 0; continue; }
        }
        if (!e->wait_t0 || e->wait_ridx != port->read_index) { e->wait_t0 = now; e->wait_ridx = port->read_index; s_h.el_waits++; }
        else if (now - e->wait_t0 >= s_el_cap_ms) {   /* the game stopped feeding it */
            /* The clock offset of the concealment so far is forgiven, not
             * repaid later in one burst from the quiet port's blocks. */
            const unsigned owed = s_el_debt;
            s_h.el_giveup++; e->quiet = 1; e->wait_t0 = 0;
            s_h.el_forgiven += owed; s_el_debt = 0;
            { static int n = 0; if (n < 50 && ++n)
                fprintf(stderr, "[audio] port %d: no block for %.0f ms at %.1f s: elastic gives up, port quiet until the game writes again (%u blocks owed, forgiven)\n",
                        p, s_el_cap_ms, (now - s_audio_start_us / 1000.0) / 1000.0, owed); }
            continue;
        }
        late = 1;
    }
    mutex_unlock(&s_audio_mutex);
    return late ? AUDIO_EL_WAIT : AUDIO_EL_MIX;
}

/* AUDIO_ELASTIC: consume the block at every running port's cursor without
 * playing it (-0.0f fill, read index + 1, its time). Under s_audio_mutex. */
static void audio_el_advance_all(void)
{
    const u64 t = s_el_ts ? audio_qpc_us() : 0;
    for (int p = 0; p < CELL_AUDIO_PORT_MAX; p++) {
        AudioPortSlot* port = &s_ports[p];
        if (!port->in_use || !port->running || !port->buffer || !port->param.nBlock) continue;
        const u32 nblock = (u32)port->param.nBlock, bs = CELL_AUDIO_BLOCK_SAMPLES * (u32)port->param.nChannel;
        u32* w = (u32*)(port->buffer + (u32)(port->read_index % nblock) * bs);
        for (u32 i = 0; i < bs; i++) w[i] = 0x00000080u;
        port->read_index++;
        if (port->read_idx_addr) vm_write64((u32)port->read_idx_addr, port->read_index % nblock);
        if (s_el_ts) { s_el[p].ts[port->read_index % AUDIO_EL_TS_N] = t; s_el[p].ts_last = t; }
    }
}

/* AUDIO_ELASTIC: consume one block on every running port without playing it,
 * when that loses nothing -- each block is digital silence the game wrote, or
 * the unwritten block of a port that is not feeding -- to repay concealment
 * (the read index lags real time by the blocks concealed). The caller allows
 * one per block mixed, so the read index runs at most 2x. 1 = dropped. */
static int audio_elastic_repay(void)
{
    const double now = audio_now_ms();
    int any = 0, ok = 1;
    mutex_lock(&s_audio_mutex);
    for (int p = 0; p < CELL_AUDIO_PORT_MAX && ok; p++) {
        AudioPortSlot* port = &s_ports[p];
        if (!port->in_use || !port->running || !port->buffer) continue;
        const u32 nch = (u32)port->param.nChannel, nblock = (u32)port->param.nBlock;
        if (!nblock || port->read_index < nblock) { ok = 0; break; }
        const u32 bs = CELL_AUDIO_BLOCK_SAMPLES * nch;
        const float* b = port->buffer + (u32)(port->read_index % nblock) * bs;
        const int st = audio_block_state(b, bs, nch);
        if (st == 2 || (st == 1 && audio_block_nonzero(b, bs)) || (st == 0 && audio_el_active(&s_el[p], now))) ok = 0;
        any = 1;
    }
    if (ok && any) { audio_el_advance_all(); s_el_debt--; s_h.el_repaid++; }
    mutex_unlock(&s_audio_mutex);
    if (ok && any) audio_notify_event_queues();   /* a period passed for the game */
    return ok && any;
}

/* AUDIO_ELASTIC at the floor: one block of concealment to the device, the
 * game's block not consumed (it plays when it arrives; one block owed, up to
 * AUDIO_ELASTIC_DEBT, the rest forgiven) -- the last output frame faded to 0
 * over 128 frames, then silence; every port's next written block is faded in. */
static void audio_elastic_conceal(void)
{
    s_h.el_conceal++;
    if (s_el_debt < s_el_debt_max) s_el_debt++; else s_h.el_forgiven++;
    if (s_el_rep > 0 && s_el_prev_ok && s_el_rep_n < 3) {
        extern volatile int g_q4_snd_on; static int sab = -1;
        if (sab < 0) { const char* e = getenv("Q4_SND_AB"); sab = e && *e >= '1' && *e <= '9'; }
        if (!sab || g_q4_snd_on) {
            static const float gain[3] = { 1.0f, 0.6f, 0.3f };
            const float g = gain[s_el_rep_n++];
            const u32 J = 64, T = 128;
            for (u32 s = 0; s < CELL_AUDIO_BLOCK_SAMPLES; s++) {
                float l = s_el_prev[s * 2] * g, r = s_el_prev[s * 2 + 1] * g;
                if (s < J) { const float a = (float)(s + 1) / (float)(J + 1);
                    l = l * a + s_el_last_l * (1.0f - a); r = r * a + s_el_last_r * (1.0f - a); }
                if (g < 1.0f && s >= CELL_AUDIO_BLOCK_SAMPLES - T) { const float b = (float)(CELL_AUDIO_BLOCK_SAMPLES - 1 - s) / (float)T; l *= b; r *= b; }
                s_el_cbuf[s * 2] = l; s_el_cbuf[s * 2 + 1] = r;
            }
            s_el_last_l = s_el_cbuf[(CELL_AUDIO_BLOCK_SAMPLES - 1) * 2];
            s_el_last_r = s_el_cbuf[(CELL_AUDIO_BLOCK_SAMPLES - 1) * 2 + 1];
            s_el_rep_count++;
            if (g < 1.0f) {   /* a quieter repeat (2nd/3rd) ends in silence, so the next real block fades in: no step (q4 REVIEW.md 2) */
                mutex_lock(&s_audio_mutex);
                for (int p = 0; p < CELL_AUDIO_PORT_MAX; p++) s_el[p].fade_in = 1;
                mutex_unlock(&s_audio_mutex);
            }
            audio_wav_out(s_el_cbuf);
            audio_backend_submit(s_el_cbuf, CELL_AUDIO_BLOCK_SAMPLES);
            return;
        }
    }
    if (s_el_rep > 0) s_el_rep_n = 3;   /* faded to silence: no repeat until a block is mixed again */
    const u32 F = 128;
    for (u32 s = 0; s < CELL_AUDIO_BLOCK_SAMPLES; s++) {
        const float g = s < F ? (float)(F - 1 - s) / (float)F : 0.0f;
        s_el_cbuf[s * 2] = s_el_last_l * g; s_el_cbuf[s * 2 + 1] = s_el_last_r * g;
    }
    s_el_last_l = s_el_last_r = 0.0f;
    mutex_lock(&s_audio_mutex);
    for (int p = 0; p < CELL_AUDIO_PORT_MAX; p++) s_el[p].fade_in = 1;
    mutex_unlock(&s_audio_mutex);
    audio_wav_out(s_el_cbuf);
    audio_backend_submit(s_el_cbuf, CELL_AUDIO_BLOCK_SAMPLES);
}

static void audio_sleep_half_ms(void)
{
    static HANDLE ht;
    if (!ht) ht = CreateWaitableTimerExW(NULL, NULL, 0x2 /* HIGH_RESOLUTION */, TIMER_ALL_ACCESS);
    LARGE_INTEGER due; due.QuadPart = -5000;   /* 0.5 ms */
    if (ht && SetWaitableTimer(ht, &due, 0, NULL, NULL, FALSE)) WaitForSingleObject(ht, 5); else Sleep(1);
}
#endif

/* ---------------------------------------------------------------------------
 * Mixing thread
 * -----------------------------------------------------------------------*/

static void audio_notify_event_queues(void)
{
    /* Push the audio-period event (data1 = CELL_AUDIO_EVENT_MIX = 0) to each
     * registered notify queue, so the game's audio loop (blocked on
     * sys_event_queue_receive) wakes once per block and writes the next one.
     * Resolve the queue by its ipc_key (set via cellAudioSetNotifyEventQueue). */
    for (int i = 0; i < CELL_AUDIO_MAX_NOTIFY_EVENT_QUEUES; i++) {
        if (!s_notify_queues[i].in_use) continue;
        uint32_t qid = sys_event_find_queue_by_key(s_notify_queues[i].key);
        if (qid)
            sys_event_queue_push_by_id(qid, s_notify_queues[i].key,
                                       0 /*CELL_AUDIO_EVENT_MIX*/, 0, 0);
    }
}

#ifdef _WIN32
static unsigned __stdcall audio_mix_thread_func(void* arg)
{
    (void)arg;
    printf("[cellAudio] Mixing thread started\n");
    /* PS3_MMCSS: MMCSS "Audio" (runtime/platform/host_pin.c). Placement stays
     * the process default (the light set) under PS3_HOST_PIN. */
    host_pin_thread(HOST_ROLE_AUDIO, 0);

    /* Pace to the device clock. The PS3 consumes one block every 5.33 ms;
     * the old Sleep(2)/Sleep(5) loop keyed on a queue depth the 1056-frame
     * WASAPI buffer can never reach, so it mixed ~225 blocks/s and the device
     * dropped the surplus: 20% of the audio thrown away, and a guest mixer
     * (GH3's FMOD) that could not keep ahead of the read index played gaps.
     * With no device, pace on the performance counter instead. */
    /* AUDIO_PRIO=<n> (off): the mixer's THREAD_PRIORITY_* (15 = TIME_CRITICAL;
     * the game runs ABOVE_NORMAL and its walker HIGHEST). AUDIO_MMCSS=1 (off):
     * join MMCSS "Pro Audio" (=2: "Audio"), as audio engines do. */
    { const char* e = getenv("AUDIO_PRIO");
      if (e && *e) { const BOOL ok = SetThreadPriority(GetCurrentThread(), atoi(e));
          fprintf(stderr, "[cellAudio] mixer thread priority %d%s\n", atoi(e), ok ? "" : " FAILED"); }
      e = getenv("AUDIO_MMCSS");
      if (e && *e && *e != '0') {
          typedef HANDLE (WINAPI *pfn_set)(LPCWSTR, LPDWORD);
          HMODULE av = LoadLibraryExW(L"avrt.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
          pfn_set set = av ? (pfn_set)(void*)GetProcAddress(av, "AvSetMmThreadCharacteristicsW") : NULL;
          DWORD idx = 0; HANDLE h = set ? set(*e == '2' ? L"Audio" : L"Pro Audio", &idx) : NULL;
          fprintf(stderr, "[cellAudio] mixer MMCSS \"%s\": %s\n", *e == '2' ? "Audio" : "Pro Audio", h ? "ok" : "failed");
      }
      fprintf(stderr, "[cellAudio] mixer thread priority now %d (health %d, limit %d, headroom %.3f)\n",
              GetThreadPriority(GetCurrentThread()), s_health, s_limit, s_headroom); }
#if AUDIO_BACKEND_WASAPI
    if (s_elastic) {
        if (s_el_floor <= 0) s_el_floor = (int)s_wasapi_period_frames + CELL_AUDIO_BLOCK_SAMPLES;
        fprintf(stderr, "[cellAudio] elastic consume ON: floor %d frames (%.1f ms; engine period %u), device buffer %u frames (%.1f ms), cap %.0f ms, debt %u blocks, timestamps %s%s%s\n",
                s_el_floor, s_el_floor / 48.0, (unsigned)s_wasapi_period_frames, (unsigned)s_wasapi_buf_frames,
                s_wasapi_buf_frames / 48.0, s_el_cap_ms, s_el_debt_max, s_el_ts ? "real (AUDIO_ELASTIC_TS)" : "start + tag * period",
                s_el_ab_ms > 0 ? ", A/B turns" : "",
                s_el_floor + CELL_AUDIO_BLOCK_SAMPLES > (int)s_wasapi_buf_frames ? " -- buffer too small to wait: conceals at once (AUDIO_BUF_MS=60)" : "");
    }
#endif
    LARGE_INTEGER qf, q0; QueryPerformanceFrequency(&qf); QueryPerformanceCounter(&q0);
    unsigned long long blocks = 0;
    double h_t0 = audio_now_ms(), h_last = 0; unsigned long long h_d0 = 0;
    memset(&s_h, 0, sizeof s_h); s_h.lead_min = 99; s_h.dev_min = 1 << 30;
    while (s_mix_thread_running) {
        const int room = audio_backend_room();
        if (s_health && room >= 0) {
#if AUDIO_BACKEND_WASAPI
            const int pad = (int)s_wasapi_buf_frames - room;
            if (blocks > 200) {   /* past the start-up fill */
                s_h.dev_n++;
                if (pad < s_h.dev_min) s_h.dev_min = pad;
                if (pad < CELL_AUDIO_BLOCK_SAMPLES) s_h.dev_low++;
                if (pad == 0) s_h.dev_under++;
            }
#endif
        }
        if (room >= 0 && room < CELL_AUDIO_BLOCK_SAMPLES) { audio_backend_wait(); continue; }
        if (room < 0) {
            LARGE_INTEGER now; QueryPerformanceCounter(&now);
            const long long due = q0.QuadPart +
                (long long)(blocks * CELL_AUDIO_BLOCK_SAMPLES * qf.QuadPart / CELL_AUDIO_SAMPLE_RATE);
            if (now.QuadPart < due) { Sleep(1); continue; }
            /* More than 50 ms behind (device just went away, a debugger
             * stop): drop the backlog instead of mixing it in a burst. */
            if (now.QuadPart - due > qf.QuadPart / 20)
                q0.QuadPart = now.QuadPart -
                    (long long)(blocks * CELL_AUDIO_BLOCK_SAMPLES * qf.QuadPart / CELL_AUDIO_SAMPLE_RATE);
        }
#if AUDIO_BACKEND_WASAPI
        /* AUDIO_ELASTIC: repay concealment on silence; wait for a late block
         * while the device holds the floor, conceal below it. */
        if (s_elastic && room >= 0 && blocks > 200) {
            if (s_el_debt && s_el_repay_ok && audio_elastic_repay()) { s_el_repay_ok = 0; continue; }
            if (s_el_on && audio_elastic_gate() == AUDIO_EL_WAIT) {
                if ((int)s_wasapi_buf_frames - room >= s_el_floor) audio_sleep_half_ms();
                else audio_elastic_conceal();
                h_last = audio_now_ms();   /* 'gap' stays the mixer thread's own delay */
                continue;
            }
        }
#endif
        /* AUDIO_WAIT_MS=<ms> (off): a producer that is late by a moment gets
         * that moment, instead of the mixer playing its block as silence (a
         * hole). Only while the port was playing sound up to the previous
         * block, only while the device still holds 2+ blocks, and at most
         * <ms> per block. Best with AUDIO_BUF_MS=40 (margin). */
        if (!s_el_on && s_wait_ms > 0 && s_health && room >= 0 && blocks > 200) {
            const double w0 = audio_now_ms();
            int waited = 0;
            for (;;) {
                int late = 0;
                for (int p = 0; p < CELL_AUDIO_PORT_MAX; p++) {
                    AudioPortSlot* port = &s_ports[p];
                    if (!port->in_use || !port->running || !port->buffer || s_ph[p].since_sound) continue;
                    const u32 bs = CELL_AUDIO_BLOCK_SAMPLES * (u32)port->param.nChannel;
                    const float* b = port->buffer + (u32)(port->read_index % port->param.nBlock) * bs;
                    if (s_sentinel ? !audio_block_written(b, bs) : !audio_block_nonzero(b, bs)) late = 1;
                }
                if (!late) break;
                const double el = audio_now_ms() - w0;
                const int rm = audio_backend_room();
#if AUDIO_BACKEND_WASAPI
                const int pad = rm >= 0 ? (int)s_wasapi_buf_frames - rm : 0;
#else
                const int pad = 0;
#endif
                if (el >= s_wait_ms || pad < 2 * CELL_AUDIO_BLOCK_SAMPLES) break;
                waited = 1;
                { static HANDLE ht; if (!ht) ht = CreateWaitableTimerExW(NULL, NULL, 0x2 /* HIGH_RESOLUTION */, TIMER_ALL_ACCESS);
                  LARGE_INTEGER due; due.QuadPart = -5000; /* 0.5 ms */
                  if (ht && SetWaitableTimer(ht, &due, 0, NULL, NULL, FALSE)) WaitForSingleObject(ht, 5); else Sleep(1); }
            }
            if (waited) { s_h.waits++; s_h.wait_ms += audio_now_ms() - w0; }
        }
        blocks++;
        /* Mix and submit one block */
        audio_mix_one_block();
        g_audio_blocks_mixed++;
        if (s_elastic) {   /* keep the last frame for a concealment fade-out (the fade-in is per port, in the mix) */
            s_el_repay_ok = 1;
            s_el_last_l = s_mix_buffer[(CELL_AUDIO_BLOCK_SAMPLES - 1) * 2];
            s_el_last_r = s_mix_buffer[(CELL_AUDIO_BLOCK_SAMPLES - 1) * 2 + 1];
            if (s_el_rep < 0) { const char* e = getenv("AUDIO_CONCEAL_REPEAT"); s_el_rep = e && *e == '1'; }
            if (s_el_rep > 0) { memcpy(s_el_prev, s_mix_buffer, sizeof s_el_prev); s_el_prev_ok = 1; s_el_rep_n = 0; }
        }
        if (s_health) {
            const double now = audio_now_ms();
            if (h_last && blocks > 200) {
                const double gap = now - h_last;
                if (gap > s_h.gap_max_ms) s_h.gap_max_ms = gap;
                if (gap > 15.0) s_h.gap_over++;
            }
            h_last = now;
            s_h.blocks++;
            if (now - h_t0 >= 5000.0) {
                const double sec = (now - h_t0) / 1000.0;
                char lead[64], dev_note[48] = "", wait_note[240] = "";
                if (s_wait_ms > 0) snprintf(wait_note, sizeof wait_note, " | waited %u (%.0f ms)", s_h.waits, s_h.wait_ms);
                if (s_elastic) {   /* AUDIO_ELASTIC: this window's state and counters */
                    snprintf(wait_note, sizeof wait_note, " | elastic %s saved %u wait %u (%.0f ms, max %.1f) conceal %u (%.0f ms)"
                             " skip %u giveup %u repaid %u debt %u forgiven %u | ts calls %lu tag %lu",
                             s_el_on ? "on" : "off", s_h.el_saved, s_h.el_waits, s_h.el_wait_ms, s_h.el_wait_max_ms,
                             s_h.el_conceal, s_h.el_conceal * 256000.0 / 48000.0, s_h.el_skip, s_h.el_giveup,
                             s_h.el_repaid, s_el_debt, s_h.el_forgiven, g_audio_ts_calls, g_audio_tag_calls);
                    if (s_el_rep > 0) {   /* AUDIO_CONCEAL_REPEAT: repeats this window */
                        const size_t n = strlen(wait_note);
                        snprintf(wait_note + n, sizeof wait_note - n, " rep %u", s_el_rep_count);
                        s_el_rep_count = 0;
                    }
                }
#if AUDIO_BACKEND_WASAPI
                { static unsigned long f0; if (s_dev_fail != f0) snprintf(dev_note, sizeof dev_note, " | DEVICE LOST %lu", s_dev_fail - f0); f0 = s_dev_fail; }
#endif
                if (s_h.lead_n) snprintf(lead, sizeof lead, "%u/%.1f (0:%u 1:%u 2:%u)", s_h.lead_min,
                                         (double)s_h.lead_sum / s_h.lead_n, s_h.lead_h[0], s_h.lead_h[1], s_h.lead_h[2]);
                else snprintf(lead, sizeof lead, "- (silent)");
                fprintf(stderr, "[audio] t=%.0fs %.1f blk/s | lead min/avg %s | holes %u (%.0f ms) torn %u unwritten %u silences %u (max %.0f ms)"
                        " | stall max %.0f ms | clip %.0f/s peak %.2f | dev min %d fr low %u empty %u | gap max %.1f ms >15ms %u | drop %llu%s%s\n",
                        (now - s_audio_start_us / 1000.0) / 1000.0, s_h.blocks / sec, lead,
                        s_h.holes, s_h.hole_blocks * 256000.0 / 48000.0, s_h.torn, s_h.unwritten, s_h.silences, s_h.silence_max_ms,
                        s_h.stall_max_ms, s_h.clip / sec, s_h.peak,
                        s_h.dev_n ? s_h.dev_min : -1, s_h.dev_low, s_h.dev_under, s_h.gap_max_ms, s_h.gap_over,
                        (unsigned long long)(g_audio_dropped - h_d0), dev_note, wait_note);
                if (s_health == 2)
                    fprintf(stderr, "[audio] ch peaks L %.2f R %.2f C %.2f LFE %.2f Ls %.2f Rs %.2f Lb %.2f Rb %.2f\n",
                            s_h.ch_peak[0], s_h.ch_peak[1], s_h.ch_peak[2], s_h.ch_peak[3],
                            s_h.ch_peak[4], s_h.ch_peak[5], s_h.ch_peak[6], s_h.ch_peak[7]);
                memset(&s_h, 0, sizeof s_h); s_h.lead_min = 99; s_h.dev_min = 1 << 30;
                h_d0 = g_audio_dropped; h_t0 = now;
                /* AUDIO_ELASTIC_AB=<ms>: the waits on and off in turns of whole windows. */
                if (s_elastic && s_el_ab_ms > 0) {
                    static double ab_t0;
                    if (!ab_t0) ab_t0 = now;
                    if (now - ab_t0 >= s_el_ab_ms - 100.0) {
                        ab_t0 = now; s_el_on = !s_el_on;
                        mutex_lock(&s_audio_mutex);
                        for (int p = 0; p < CELL_AUDIO_PORT_MAX; p++) s_el[p].wait_t0 = 0;
                        mutex_unlock(&s_audio_mutex);
                    }
                }
            }
        }
        /* AUDIO_PEAK=1: report the mixed block's peak amplitude periodically so
         * "is any port producing sound" is answerable (LBP Bink movie audio). */
        { static int _ap = -1; if (_ap < 0) _ap = getenv("AUDIO_PEAK") ? 1 : 0;
          if (_ap) { static unsigned _n = 0; float pk = 0.0f;
            for (u32 i = 0; i < CELL_AUDIO_BLOCK_SAMPLES * 2; i++) {
                float a = s_mix_buffer[i]; if (a < 0) a = -a; if (a > pk) pk = a; }
            if ((++_n % 200) == 0 || (pk > 0.001f && _n < 40))
                fprintf(stderr, "[audio-peak] block#%u peak=%.4f\n", _n, pk); } }
        audio_wav_out(s_mix_buffer);   /* AUDIO_WAV */
        audio_backend_submit(s_mix_buffer, CELL_AUDIO_BLOCK_SAMPLES);
        /* AUDIO_RATE=1: blocks mixed vs real time, and frames dropped. */
        { static int on = -1; if (on < 0) on = getenv("AUDIO_RATE") ? 1 : 0;
          if (on) { static ULONGLONG t0; static unsigned n, z; static unsigned long long d0;
            ULONGLONG now = GetTickCount64(); if (!t0) t0 = now; n++;
            { int nz = 0; for (u32 i = 0; i < CELL_AUDIO_BLOCK_SAMPLES * 2; i++) if (s_mix_buffer[i] != 0.0f) { nz = 1; break; }
              if (!nz) z++; }
            if (now - t0 >= 5000) {
                fprintf(stderr, "[audio-rate] %.1f blocks/s (real time 187.5), silent %.1f%%, dropped %.0f frames/s, device buf %u%c",
                        n * 1000.0 / (now - t0), n ? 100.0 * z / n : 0.0, (g_audio_dropped - d0) * 1000.0 / (now - t0),
                #if AUDIO_BACKEND_WASAPI
                        (unsigned)s_wasapi_buf_frames, 10);
#else
                        0u, 10);
#endif
                n = z = 0; d0 = g_audio_dropped; t0 = now; } } }

        /* Notify event queues */
        audio_notify_event_queues();
    }

    printf("[cellAudio] Mixing thread stopped\n");
    return 0;
}
#else
static void* audio_mix_thread_func(void* arg)
{
    (void)arg;
    printf("[cellAudio] Mixing thread started\n");

    while (s_mix_thread_running) {
        audio_mix_one_block();
        audio_backend_submit(s_mix_buffer, CELL_AUDIO_BLOCK_SAMPLES);
        audio_notify_event_queues();

        u32 queued = audio_backend_queued_samples();
        if (queued > CELL_AUDIO_BLOCK_SAMPLES * 4) {
            usleep(5000);
        } else {
            usleep(2000);
        }
    }

    printf("[cellAudio] Mixing thread stopped\n");
    return NULL;
}
#endif

static int audio_start_mix_thread(void)
{
    s_mix_thread_running = 1;

#ifdef _WIN32
    s_mix_thread = (HANDLE)_beginthreadex(NULL, 0, audio_mix_thread_func,
                                           NULL, 0, NULL);
    if (!s_mix_thread) {
        s_mix_thread_running = 0;
        return -1;
    }
#else
    if (pthread_create(&s_mix_thread, NULL, audio_mix_thread_func, NULL) != 0) {
        s_mix_thread_running = 0;
        return -1;
    }
    s_mix_thread_live = 1;
#endif
    return 0;
}

static void audio_stop_mix_thread(void)
{
    s_mix_thread_running = 0;
#ifdef _WIN32
    if (s_mix_thread) {
        WaitForSingleObject(s_mix_thread, 2000);
        CloseHandle(s_mix_thread);
        s_mix_thread = NULL;
    }
#else
    /* Same guard. Init leaves s_audio_initialized set even when the mixing
     * thread failed to start ("continuing silently" -- a title without audio
     * still runs), so Quit reaches here with s_mix_thread never assigned.
     * pthread_t has no null value to test, and joining an unset one, or
     * joining the same one twice, is undefined rather than an error the way
     * WaitForSingleObject on a null HANDLE is. */
    if (s_mix_thread_live) {
        pthread_join(s_mix_thread, NULL);
        s_mix_thread_live = 0;
    }
#endif
}

/* ---------------------------------------------------------------------------
 * API implementations
 * -----------------------------------------------------------------------*/

s32 cellAudioInit(void)
{
    /* PS3_NO_AUDIO=1: report that the audio library is unavailable. Middleware
     * that drives its mixer through SPU jobs will otherwise block the whole boot
     * waiting on work we cannot yet complete; failing here lets a title take its
     * silent path and keep going. Diagnostic first, workaround second. */
    if (getenv("PS3_NO_AUDIO")) {
        printf("[cellAudio] PS3_NO_AUDIO -- reporting audio unavailable\n");
        return (s32)CELL_AUDIO_ERROR_NOT_INIT;
    }
    printf("[cellAudio] Init()\n");

    if (s_audio_initialized)
        return CELL_AUDIO_ERROR_ALREADY_INIT;

    memset(s_ports, 0, sizeof(s_ports));
    memset(s_notify_queues, 0, sizeof(s_notify_queues));
    { const char* e = getenv("AUDIO_HEALTH"); s_health = e && *e ? atoi(e) : 1;
      e = getenv("AUDIO_LIMIT"); s_limit = e && *e ? atoi(e) : 0;
      e = getenv("AUDIO_SENTINEL"); s_sentinel = e && *e == '1';
      e = getenv("AUDIO_WAIT_MS"); s_wait_ms = e && *e ? atoi(e) : 0;
      e = getenv("AUDIO_HEADROOM"); if (e && *e) s_headroom = powf(10.0f, (float)atof(e) / 20.0f);
      /* AUDIO_ELASTIC=1: the -0.0f fill (the sentinel) is part of it. */
      e = getenv("AUDIO_ELASTIC"); s_elastic = e && *e == '1';
      if (s_elastic) {
          s_sentinel = 1; s_el_on = 1;
          e = getenv("AUDIO_ELASTIC_CAP_MS"); if (e && atoi(e) > 0) s_el_cap_ms = atoi(e);
          e = getenv("AUDIO_ELASTIC_AB"); s_el_ab_ms = e && *e ? atoi(e) : 0;
          e = getenv("AUDIO_ELASTIC_FLOOR"); s_el_floor = e && *e ? atoi(e) : 0;
          e = getenv("AUDIO_ELASTIC_DEBT"); if (e && *e && atoi(e) >= 0) s_el_debt_max = (unsigned)atoi(e);
          e = getenv("AUDIO_ELASTIC_TS"); s_el_ts = e && *e == '1';
      }
      memset(s_el, 0, sizeof s_el); s_el_debt = 0; s_el_repay_ok = 0;
      memset(s_ph, 0, sizeof s_ph); }
    mutex_init(&s_audio_mutex);

    if (audio_backend_init() < 0) {
        printf("[cellAudio] WARNING: Audio backend init failed, continuing silently\n");
        /* Don't fail -- games should still run without audio */
    }


#ifdef _WIN32
    { LARGE_INTEGER f, c;
      QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c);
      s_audio_start_us = (u64)(c.QuadPart / f.QuadPart) * 1000000ULL +
          (u64)(c.QuadPart % f.QuadPart) * 1000000ULL / (u64)f.QuadPart; }
#else
    { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
      s_audio_start_us = (u64)ts.tv_sec * 1000000ULL + (u64)ts.tv_nsec / 1000ULL; }
#endif
    if (audio_start_mix_thread() < 0) {
        printf("[cellAudio] WARNING: Could not start mixing thread\n");
    }

    s_audio_initialized = 1;
    return CELL_OK;
}

s32 cellAudioQuit(void)
{
    printf("[cellAudio] Quit()\n");

    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;

    audio_stop_mix_thread();
    audio_backend_shutdown();

    /* Port buffers live in the guest vm_base arena (bump-allocated, not host
     * malloc) -- just drop the references; never free() them. */
    for (int i = 0; i < CELL_AUDIO_PORT_MAX; i++) {
        s_ports[i].buffer = NULL;
        s_ports[i].in_use = 0;
        s_ports[i].running = 0;
    }

    mutex_destroy(&s_audio_mutex);
    s_audio_initialized = 0;
    return CELL_OK;
}

s32 cellAudioPortOpen(const CellAudioPortParam* param, u32* portNum)
{
    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;

    /* param / portNum are GUEST addresses; translate, and read the BE u64 param
     * fields via vm_read64 (a raw param->nChannel faults / is host-endian). */
    uint32_t param_ea   = (uint32_t)(uintptr_t)param;
    uint32_t portNum_ea = (uint32_t)(uintptr_t)portNum;
    if (!param_ea || !portNum_ea)
        return CELL_AUDIO_ERROR_PARAM;

    u64 nch  = vm_read64(param_ea + 0);   /* nChannel */
    u64 nblk = vm_read64(param_ea + 8);   /* nBlock   */
    printf("[cellAudio] PortOpen(nChannel=%llu, nBlock=%llu)\n",
           (unsigned long long)nch, (unsigned long long)nblk);

    if (nch != CELL_AUDIO_PORT_2CH && nch != CELL_AUDIO_PORT_8CH)
        return CELL_AUDIO_ERROR_PARAM;
    if (nblk != CELL_AUDIO_BLOCK_8 && nblk != CELL_AUDIO_BLOCK_16 && nblk != CELL_AUDIO_BLOCK_32)
        return CELL_AUDIO_ERROR_PARAM;

    mutex_lock(&s_audio_mutex);

    /* Find a free port slot */
    s32 found = -1;
    for (u32 i = 0; i < CELL_AUDIO_PORT_MAX; i++) {
        if (!s_ports[i].in_use) {
            found = (s32)i;
            break;
        }
    }

    if (found < 0) {
        mutex_unlock(&s_audio_mutex);
        return CELL_AUDIO_ERROR_PORT_FULL;
    }

    AudioPortSlot* port = &s_ports[found];
    port->in_use  = 1;
    port->running = 0;
    /* Store the decoded (host-endian) param values, not the raw BE guest bytes. */
    memset(&port->param, 0, sizeof(port->param));
    port->param.nChannel = nch;
    port->param.nBlock   = nblk;
    port->read_index  = 0;
    port->write_index = 0;
    memset(&s_el[found], 0, sizeof s_el[found]);   /* AUDIO_ELASTIC: no state (wait, quiet, timestamps) from a port closed here */

    /* Allocate the audio buffer in GUEST memory so portAddr/readIndexAddr are
     * real guest addresses (the game's mixer -- FMOD here -- writes PCM there
     * and validates them as 1 MB-aligned guest pointers; a host pointer trips
     * its "invalid parameter" check). Bump from a free main-memory window; the
     * host-side `buffer` is just the vm_base view of the same bytes. */
    u32 buf_samples = (u32)(nblk * CELL_AUDIO_BLOCK_SAMPLES * nch);
    port->buf_size = buf_samples * (u32)sizeof(float);

    /* Keep audio outside sys_memory (0x40000000..0x50000000), the HLE heap
     * (0x50000000..0x58000000), and sys_vm (0x60000000..0x70000000).
     * Reserve two 1-MB pages per port so reopening reuses its own storage. */
    u32 guest_buf = 0x58000000u + (u32)found * 0x200000u;
    u32 guest_ridx = guest_buf + 0x100000u;
    if (vm_commit(guest_buf, 0x200000u) != CELL_OK) {
        port->in_use = 0;
        mutex_unlock(&s_audio_mutex);
        return CELL_AUDIO_ERROR_AUDIOSYSTEM;
    }

    port->buffer = (float*)(vm_base + guest_buf);     /* host view of guest buffer */
    memset(port->buffer, 0, port->buf_size);
    vm_write64(guest_ridx, 0);                        /* read index counter (u64, see below) */

    port->port_addr     = guest_buf;
    port->read_idx_addr = guest_ridx;

    vm_write32(portNum_ea, (u32)found);   /* guest out-param */

    mutex_unlock(&s_audio_mutex);
    return CELL_OK;
}

s32 cellAudioPortClose(u32 portNum)
{
    printf("[cellAudio] PortClose(port=%u)\n", portNum);

    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;

    if (portNum >= CELL_AUDIO_PORT_MAX)
        return CELL_AUDIO_ERROR_PORT_NOT_OPEN;

    mutex_lock(&s_audio_mutex);

    if (!s_ports[portNum].in_use) {
        mutex_unlock(&s_audio_mutex);
        return CELL_AUDIO_ERROR_PORT_NOT_OPEN;
    }

    /* buffer points INTO the guest vm_base arena (reserved per port in PortOpen),
     * not a host malloc -- do NOT free() it (that corrupts the host heap).
     * The guest window is reclaimed wholesale when vm_base is released. */
    s_ports[portNum].buffer = NULL;

    s_ports[portNum].in_use  = 0;
    s_ports[portNum].running = 0;

    mutex_unlock(&s_audio_mutex);
    return CELL_OK;
}

s32 cellAudioPortStart(u32 portNum)
{
    printf("[cellAudio] PortStart(port=%u)\n", portNum);

    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;

    if (portNum >= CELL_AUDIO_PORT_MAX || !s_ports[portNum].in_use)
        return CELL_AUDIO_ERROR_PORT_NOT_OPEN;

    if (s_ports[portNum].running)
        return CELL_AUDIO_ERROR_PORT_ALREADY_RUN;

    mutex_lock(&s_audio_mutex);
    s_ports[portNum].running = 1;
    mutex_unlock(&s_audio_mutex);

    return CELL_OK;
}

s32 cellAudioPortStop(u32 portNum)
{
    printf("[cellAudio] PortStop(port=%u)\n", portNum);

    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;

    if (portNum >= CELL_AUDIO_PORT_MAX || !s_ports[portNum].in_use)
        return CELL_AUDIO_ERROR_PORT_NOT_OPEN;

    if (!s_ports[portNum].running)
        return CELL_AUDIO_ERROR_PORT_NOT_RUN;

    mutex_lock(&s_audio_mutex);
    s_ports[portNum].running = 0;
    mutex_unlock(&s_audio_mutex);

    return CELL_OK;
}

s32 cellAudioGetPortBlockTag(u32 portNum, u64 blockNo, u64* tag)
{
    uint32_t tag_ea = (uint32_t)(uintptr_t)tag;
    g_audio_tag_calls++;
    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;
    if (portNum >= CELL_AUDIO_PORT_MAX)
        return CELL_AUDIO_ERROR_PARAM;
    if (!s_ports[portNum].in_use)
        return CELL_AUDIO_ERROR_PORT_NOT_OPEN;
    if (!tag_ea)
        return CELL_AUDIO_ERROR_PARAM;

    mutex_lock(&s_audio_mutex);
    AudioPortSlot* port = &s_ports[portNum];
    u64 nblk = port->param.nBlock ? port->param.nBlock : 1;
    if (blockNo >= nblk) {
        mutex_unlock(&s_audio_mutex);
        return CELL_AUDIO_ERROR_PARAM;
    }
    u64 t = port->read_index + blockNo - (port->read_index % nblk);
    if (blockNo < port->read_index % nblk) t += nblk;
    mutex_unlock(&s_audio_mutex);

    vm_write64(tag_ea, t);
    return CELL_OK;
}

s32 cellAudioGetPortTimestamp(u32 portNum, u64 tag, u64* stamp)
{
    uint32_t stamp_ea = (uint32_t)(uintptr_t)stamp;
    if (++g_audio_ts_calls == 1)
        fprintf(stderr, "[cellAudio] GetPortTimestamp first call (port %u, tag %llu)\n", portNum, (unsigned long long)tag);
    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;
    if (portNum >= CELL_AUDIO_PORT_MAX)
        return CELL_AUDIO_ERROR_PARAM;
    if (!s_ports[portNum].in_use)
        return CELL_AUDIO_ERROR_PORT_NOT_OPEN;
    if (!stamp_ea)
        return CELL_AUDIO_ERROR_PARAM;

    mutex_lock(&s_audio_mutex);
    u64 global_counter = s_ports[portNum].read_index;
    /* AUDIO_ELASTIC_TS=1 (off, untested in play): the read index waits for the
     * game, so it no longer follows an ideal clock: the real time read_index
     * became <tag> (RPCS3: the last advance minus the tag delta in periods,
     * for tags older than the record). */
    u64 el_t = 0;
    if (s_el_ts && tag <= global_counter && s_el[portNum].ts_last) {
        const u64 d = global_counter - tag;
        el_t = d < AUDIO_EL_TS_N && s_el[portNum].ts[tag % AUDIO_EL_TS_N] ? s_el[portNum].ts[tag % AUDIO_EL_TS_N]
                                                                       : s_el[portNum].ts_last - d * 256000000ULL / 48000ULL;
    }
    mutex_unlock(&s_audio_mutex);
    if (tag > global_counter)
        return CELL_AUDIO_ERROR_TAG_NOT_FOUND;

    u64 t = el_t ? el_t : s_audio_start_us + tag * 256000000ULL / 48000ULL;
    vm_write64(stamp_ea, t);
    return CELL_OK;
}

/* cellAudioCreateNotifyEventQueue(sys_event_queue_t* id, u64* key)
 *
 * Creates the queue the game will block on for audio-period events, and
 * registers it for notification in one step -- the game never calls
 * SetNotifyEventQueue for this one.
 *
 * This was missing entirely. YDKJ imports it, got the unimplemented-NID path,
 * and then blocked on an id nothing had created: sys_event_queue_receive
 * answered CELL_ESRCH every time and the title logged 1.7 MILLION
 * "ERROR - LIBAUDIO DROPOUT" lines in a 60 s run.
 *
 * Both params are GUEST addresses and both are big-endian out-values; the id
 * is 32-bit and the key 64-bit, so they cannot be written with a plain store. */
s32 cellAudioCreateNotifyEventQueue(u32* id, u64* key)
{
    uint32_t id_ea  = (uint32_t)(uintptr_t)id;
    uint32_t key_ea = (uint32_t)(uintptr_t)key;

    printf("[cellAudio] CreateNotifyEventQueue(id_ea=0x%08X key_ea=0x%08X)\n",
           id_ea, key_ea);

    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;
    if (!id_ea || !key_ea)
        return CELL_AUDIO_ERROR_PARAM;

    /* A distinct key per queue: the notify path resolves queues BY KEY, and the
     * game creates its other queues with key 0, so reusing 0 here would make
     * find_queue_by_key ambiguous and post audio events to whichever matched. */
    static uint64_t s_next_key = 0x8000000000000001ull;
    uint64_t k = s_next_key++;

    uint32_t qid = sys_event_queue_create_direct(k, 8);
    if (!qid)
        return CELL_AUDIO_ERROR_EVENT_QUEUE;

    mutex_lock(&s_audio_mutex);
    int slot = -1;
    for (int i = 0; i < CELL_AUDIO_MAX_NOTIFY_EVENT_QUEUES; i++) {
        if (!s_notify_queues[i].in_use) { slot = i; break; }
    }
    if (slot < 0) { mutex_unlock(&s_audio_mutex); return CELL_AUDIO_ERROR_PARAM; }
    s_notify_queues[slot].in_use = 1;
    s_notify_queues[slot].key    = k;
    mutex_unlock(&s_audio_mutex);

    vm_write32(id_ea, qid);
    vm_write64(key_ea, k);
    printf("[cellAudio] CreateNotifyEventQueue -> id=%u key=0x%llX\n",
           qid, (unsigned long long)k);
    return CELL_OK;
}

s32 cellAudioSetNotifyEventQueue(u64 key)
{
    printf("[cellAudio] SetNotifyEventQueue(key=0x%llX)\n",
           (unsigned long long)key);

    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;

    mutex_lock(&s_audio_mutex);

    for (int i = 0; i < CELL_AUDIO_MAX_NOTIFY_EVENT_QUEUES; i++) {
        if (!s_notify_queues[i].in_use) {
            s_notify_queues[i].in_use = 1;
            s_notify_queues[i].key = key;
            mutex_unlock(&s_audio_mutex);
            return CELL_OK;
        }
    }

    mutex_unlock(&s_audio_mutex);
    return CELL_AUDIO_ERROR_PARAM; /* no free slots */
}

s32 cellAudioRemoveNotifyEventQueue(u64 key)
{
    printf("[cellAudio] RemoveNotifyEventQueue(key=0x%llX)\n",
           (unsigned long long)key);

    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;

    mutex_lock(&s_audio_mutex);

    for (int i = 0; i < CELL_AUDIO_MAX_NOTIFY_EVENT_QUEUES; i++) {
        if (s_notify_queues[i].in_use && s_notify_queues[i].key == key) {
            s_notify_queues[i].in_use = 0;
            mutex_unlock(&s_audio_mutex);
            return CELL_OK;
        }
    }

    mutex_unlock(&s_audio_mutex);
    return CELL_AUDIO_ERROR_PARAM;
}

s32 cellAudioGetPortConfig(u32 portNum, CellAudioPortConfig* config)
{
    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;

    if (portNum >= CELL_AUDIO_PORT_MAX || !s_ports[portNum].in_use)
        return CELL_AUDIO_ERROR_PORT_NOT_OPEN;

    if (!config)
        return CELL_AUDIO_ERROR_PARAM;

    /* `config` is a GUEST address; write the BE struct field-by-field at its
     * guest offsets (8-byte aligned u64s). */
    uint32_t cfg = (uint32_t)(uintptr_t)config;
    if (!cfg)
        return CELL_AUDIO_ERROR_PARAM;

    mutex_lock(&s_audio_mutex);
    AudioPortSlot* port = &s_ports[portNum];

    /* CellAudioPortConfig (cell/audio.h). sys_addr_t is uintptr_t, which on the
     * 32-bit PPU ABI is FOUR bytes -- not eight. Writing the two sys_addr_t
     * fields as u64 shifted every field past it and left the game reading the
     * high (zero) half of readIndexAddr as its pointer:
     *   sys_addr_t readIndexAddr;  u32 @  0
     *   uint32_t   status;         u32 @  4
     *   uint64_t   nChannel;       u64 @  8
     *   uint64_t   nBlock;         u64 @ 16
     *   uint32_t   portSize;       u32 @ 24
     *   sys_addr_t portAddr;       u32 @ 28      -> 32 bytes */
    vm_write32(cfg +  0, (u32)port->read_idx_addr);                          /* readIndexAddr */
    vm_write32(cfg +  4, port->running ? CELL_AUDIO_STATUS_RUN
                                       : CELL_AUDIO_STATUS_READY);           /* status */
    vm_write64(cfg +  8, port->param.nChannel);                             /* nChannel */
    vm_write64(cfg + 16, port->param.nBlock);                               /* nBlock */
    vm_write32(cfg + 24, port->buf_size);                                   /* portSize */
    vm_write32(cfg + 28, (u32)port->port_addr);                             /* portAddr */

    { static int _n = 0; if (_n < 24 && ++_n)
        fprintf(stderr, "[cellAudio] GetPortConfig(port=%u) status=%s bufEA=0x%08X ridxEA=0x%08X\n",
                portNum, port->running ? "RUN" : "READY",
                (u32)port->port_addr, (u32)port->read_idx_addr); }

    mutex_unlock(&s_audio_mutex);
    return CELL_OK;
}

s32 cellAudioPortGetStatus(u32 portNum, u32* status)
{
    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;

    if (portNum >= CELL_AUDIO_PORT_MAX || !status)
        return CELL_AUDIO_ERROR_PARAM;

    if (!s_ports[portNum].in_use) {
        vm_write32((u32)(uintptr_t)status, CELL_AUDIO_STATUS_CLOSE);
    } else if (s_ports[portNum].running) {
        vm_write32((u32)(uintptr_t)status, CELL_AUDIO_STATUS_RUN);
    } else {
        vm_write32((u32)(uintptr_t)status, CELL_AUDIO_STATUS_READY);
    }

    return CELL_OK;
}

s32 cellAudioSetPersonalDevice(s32 iPersonalStream, s32 iDevice)
{
    (void)iPersonalStream;
    (void)iDevice;
    printf("[cellAudio] SetPersonalDevice(stream=%d, device=%d) - stub\n",
           iPersonalStream, iDevice);
    return CELL_OK;
}

s32 cellAudioUnsetPersonalDevice(s32 iPersonalStream)
{
    (void)iPersonalStream;
    printf("[cellAudio] UnsetPersonalDevice(stream=%d) - stub\n", iPersonalStream);
    return CELL_OK;
}
