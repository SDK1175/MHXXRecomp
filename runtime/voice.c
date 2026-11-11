// Voice chat, its first part (voice.h): the microphone and the speakers through Opus, and the F1 menu's microphone test.
// One thread moves 10 ms frames: the recording stream's samples (48 kHz mono S16, SDL converts the device's own),
// times [Voice] MicVolume, packed by Opus (VoIP, 24 kbit/s) and unpacked, into the playback stream at [Voice]
// VoiceVolume times the game's volume. Less than 100 ms is kept queued to play: a voice late by more is dropped.
#include "voice.h"

#include <SDL3/SDL.h>

#include "config.h"
#include "rt.h"

#if !__has_include(<opus/opus.h>)  // (built without Opus: voice chat off, the test says so)
static char g_error[] = "Opus didn't start";
void voice_test(bool on) { (void)on; }
bool voice_test_on(void) { return false; }
float voice_mic_level(void) { return 0.0f; }
const char *voice_error(void) { return g_error; }
int voice_selftest(void) { return -1; }
void voice_preview(int mic_percent, int voice_percent) { (void)mic_percent, (void)voice_percent; }
void voice_shutdown(void) {}
#else
#include <math.h>
#include <opus/opus.h>
#include <string.h>

#define VOICE_BITRATE 24000
#define VOICE_MAX_PACKET 400

static SDL_Mutex *g_lock;  // (voice_test from the menu, voice_shutdown at exit)
static SDL_Thread *g_thread;
static SDL_AtomicInt g_running, g_level, g_preview_mic = {-1}, g_preview_voice = {-1};
static SDL_AudioStream *g_mic, *g_out;
static OpusEncoder *g_enc;
static OpusDecoder *g_dec;
static char g_error[96];

static SDL_Mutex *lock(void) {
    static SDL_SpinLock once;
    SDL_LockSpinlock(&once);
    if (!g_lock) g_lock = SDL_CreateMutex();
    SDL_UnlockSpinlock(&once);
    return g_lock;
}
static float mic_gain(void) {
    int p = SDL_GetAtomicInt(&g_preview_mic);
    return p >= 0 ? p / 100.0f : config_mic_gain();
}
static float voice_gain(void) {
    int p = SDL_GetAtomicInt(&g_preview_voice);
    return p >= 0 ? p / 100.0f * config_audio_gain() : config_voice_gain();
}

static bool codec_open(void) {
    int err = 0;
    if (!g_enc) {
        g_enc = opus_encoder_create(VOICE_RATE, 1, OPUS_APPLICATION_VOIP, &err);
        if (!g_enc) return LOG("[voice] Opus encoder: %s\n", opus_strerror(err)), false;
        opus_encoder_ctl(g_enc, OPUS_SET_BITRATE(VOICE_BITRATE));
        opus_encoder_ctl(g_enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
        opus_encoder_ctl(g_enc, OPUS_SET_COMPLEXITY(5));  // (cheap: the game runs beside it)
    }
    if (!g_dec) {
        g_dec = opus_decoder_create(VOICE_RATE, 1, &err);
        if (!g_dec) return LOG("[voice] Opus decoder: %s\n", opus_strerror(err)), false;
    }
    return true;
}

static int SDLCALL run(void *unused) {
    (void)unused;
    int16_t pcm[VOICE_FRAME], back[VOICE_FRAME];
    uint8_t packet[VOICE_MAX_PACKET];
    float peak = 0.0f;
    int frames = 0;
    while (SDL_GetAtomicInt(&g_running)) {
        if (SDL_GetAudioStreamAvailable(g_mic) < (int)sizeof pcm) {
            SDL_Delay(2);
            continue;
        }
        if (SDL_GetAudioStreamData(g_mic, pcm, sizeof pcm) != (int)sizeof pcm) continue;
        float g = mic_gain();
        for (int i = 0; i < VOICE_FRAME; i++) {
            float v = pcm[i] * g;
            v = v > 32767.0f ? 32767.0f : v < -32768.0f ? -32768.0f : v;
            pcm[i] = (int16_t)v;
            float a = fabsf(v) / 32768.0f;
            if (a > peak) peak = a;
        }
        if (++frames == 10) {  // (100 ms)
            SDL_SetAtomicInt(&g_level, (int)(peak * 1000.0f));
            peak = 0.0f, frames = 0;
        }
        int n = opus_encode(g_enc, pcm, VOICE_FRAME, packet, sizeof packet);
        if (n <= 0) continue;
        int m = opus_decode(g_dec, packet, n, back, VOICE_FRAME, 0);
        if (m <= 0) continue;
        SDL_SetAudioStreamGain(g_out, voice_gain());
        if (SDL_GetAudioStreamQueued(g_out) > VOICE_RATE / 10 * (int)sizeof(int16_t)) SDL_ClearAudioStream(g_out);
        SDL_PutAudioStreamData(g_out, back, m * (int)sizeof(int16_t));
    }
    return 0;
}

static void close_all(void) {  // (lock held)
    if (g_thread) {
        SDL_SetAtomicInt(&g_running, 0);
        SDL_WaitThread(g_thread, NULL);
        g_thread = NULL;
    }
    if (g_mic) SDL_DestroyAudioStream(g_mic), g_mic = NULL;
    if (g_out) SDL_DestroyAudioStream(g_out), g_out = NULL;
    SDL_SetAtomicInt(&g_level, 0);
    SDL_SetAtomicInt(&g_preview_mic, -1);
    SDL_SetAtomicInt(&g_preview_voice, -1);
}

void voice_test(bool on) {
    SDL_LockMutex(lock());
    if (!on) {
        if (g_thread) LOG("[voice] mic test off\n");
        close_all();
        SDL_UnlockMutex(g_lock);
        return;
    }
    if (g_thread) {
        SDL_UnlockMutex(g_lock);
        return;
    }
    g_error[0] = 0;
    SDL_AudioSpec spec = {SDL_AUDIO_S16, 1, VOICE_RATE};
    if (!SDL_WasInit(SDL_INIT_AUDIO) && !SDL_InitSubSystem(SDL_INIT_AUDIO))
        SDL_strlcpy(g_error, "No sound device", sizeof g_error);
    else if (!codec_open())
        SDL_strlcpy(g_error, "Opus didn't start", sizeof g_error);
    else if (!(g_mic = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_RECORDING, &spec, NULL, NULL))) {
        LOG("[voice] no microphone: %s\n", SDL_GetError());
        SDL_strlcpy(g_error, "No microphone", sizeof g_error);
    } else if (!(g_out = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL))) {
        LOG("[voice] no speakers: %s\n", SDL_GetError());
        SDL_strlcpy(g_error, "No sound device", sizeof g_error);
    }
    if (!g_error[0]) {
        SDL_ResumeAudioStreamDevice(g_mic);
        SDL_ResumeAudioStreamDevice(g_out);
        SDL_SetAtomicInt(&g_running, 1);
        if (!(g_thread = SDL_CreateThread(run, "voice", NULL))) SDL_strlcpy(g_error, "No sound device", sizeof g_error);
    }
    if (g_error[0]) close_all();
    else
        LOG("[voice] mic test on: %s -> %s\n", SDL_GetAudioDeviceName(SDL_GetAudioStreamDevice(g_mic)),
            SDL_GetAudioDeviceName(SDL_GetAudioStreamDevice(g_out)));
    SDL_UnlockMutex(g_lock);
}
bool voice_test_on(void) { return SDL_GetAtomicInt(&g_running) != 0; }
float voice_mic_level(void) { return SDL_GetAtomicInt(&g_level) / 1000.0f; }
const char *voice_error(void) { return g_error; }
void voice_preview(int mic_percent, int voice_percent) {
    SDL_SetAtomicInt(&g_preview_mic, mic_percent);
    SDL_SetAtomicInt(&g_preview_voice, voice_percent);
}
void voice_shutdown(void) {
    SDL_LockMutex(lock());
    close_all();
    if (g_enc) opus_encoder_destroy(g_enc), g_enc = NULL;
    if (g_dec) opus_decoder_destroy(g_dec), g_dec = NULL;
    SDL_UnlockMutex(g_lock);
}

int voice_selftest(void) {
    if (!codec_open()) return -1;
    int16_t pcm[VOICE_FRAME], back[VOICE_FRAME];
    uint8_t packet[VOICE_MAX_PACKET];
    int last = -1;
    for (int f = 0; f < 50; f++) {  // (half a second of a 440 Hz tone: Opus settles in a few frames)
        for (int i = 0; i < VOICE_FRAME; i++)
            pcm[i] = (int16_t)(8000.0 * sin(2.0 * 3.14159265358979 * 440.0 * (f * VOICE_FRAME + i) / VOICE_RATE));
        int n = opus_encode(g_enc, pcm, VOICE_FRAME, packet, sizeof packet);
        if (n <= 0 || opus_decode(g_dec, packet, n, back, VOICE_FRAME, 0) != VOICE_FRAME) return -1;
        last = n;
    }
    return last;
}
#endif
