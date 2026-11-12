// Voice chat (voice.h). One thread, while the devices are open:
// - the microphone: 10 ms frames (48 kHz mono S16, SDL converts the device's own) times [Voice] MicVolume, packed by
//   Opus (VoIP, 24 kbit/s) while louder than a breath (and 300 ms after), sent to the other games (ldn_send_aux) - in
//   the microphone test given to this game's own player instead;
// - the speakers: each player's packets (by their network address) in a jitter buffer that starts playing at 30 ms,
//   decoded in order (a missing one made up by Opus), all mixed, at [Voice] VoiceVolume times the game's volume, with
//   at most ~40 ms queued in the playback stream. A player silent for 100 ms stops until they're heard again; one
//   whose buffer grows past 80 ms is skipped ahead (the voice stays near real time).
#include "voice.h"

#include <SDL3/SDL.h>

#include "config.h"
#include "rt.h"

#if !__has_include(<opus/opus.h>)  // (built without Opus: voice chat off, the test says so)
static char g_error[] = "Opus didn't start";
void voice_tick(void) {}
void voice_aux_received(const void *data, uint32_t len) { (void)data, (void)len; }
int voice_talking(void) { return 0; }
int voice_speakers(uint32_t *ids, int max) { (void)ids, (void)max; return 0; }
bool voice_sending(void) { return false; }
void voice_set_player_volume(uint32_t id, int percent) { (void)id, (void)percent; }
int voice_player_volume(uint32_t id) { (void)id; return 100; }
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

#include "net/net.h"

#define VOICE_BITRATE 24000
#define VOICE_MAX_PACKET 400
#define VOICE_MAGIC 0xE7u   // (F_AUX: emsync.c's are 0xE5, hpscale.c's 0xE6)
#define SPEAKERS 8
#define RING 16             // packets kept per player (160 ms)
#define START_FRAMES 3      // buffered before a player's voice starts (30 ms)
#define SKIP_FRAMES 8       // buffered past this: skipped ahead
#define QUIET_FRAMES 10     // made up this many in a row: the player stopped
#define GATE 0.015f         // louder than this (of full scale) is sent
#define AGC_TARGET 0.5f     // a voice's peaks are raised towards this
#define AGC_MAX 4.0f        // at most this many times
#define HANGOVER 30         // and this many frames after (300 ms)
#define SELF_ID 0xFFFFFFFFu // the microphone test's own voice

typedef struct {  // on the wire, after it the Opus packet
    uint8_t magic, pad;
    uint16_t seq;
    uint32_t id;  // the sender's address in the network
} Header;
typedef struct {
    uint16_t seq;
    uint16_t len;  // 0: empty
    uint8_t data[VOICE_MAX_PACKET];
} Slot;
typedef struct {
    uint32_t id;  // 0: free
    OpusDecoder *dec;
    bool playing;
    uint16_t next;  // the next seq to play
    int made_up;    // frames made up in a row
    uint64_t heard; // last packet (ns)
    Slot ring[RING];
} Speaker;

static SDL_Mutex *g_lock;  // (opening and closing: voice_tick, voice_test, voice_shutdown)
static SDL_Mutex *g_rx;    // (the speakers: the network threads add, the voice thread plays)
static SDL_Thread *g_thread;
static SDL_AtomicInt g_running, g_level, g_test, g_talking, g_sending, g_ptt, g_preview_mic = {-1}, g_preview_voice = {-1};
static struct { uint32_t id; int percent; } g_volumes[SPEAKERS];  // (each player's volume, this game only; g_rx)
static SDL_AudioStream *g_mic, *g_out;
static OpusEncoder *g_enc;
static Speaker g_spk[SPEAKERS];
static char g_error[96];

static SDL_Mutex *lock(void) {
    static SDL_SpinLock once;
    SDL_LockSpinlock(&once);
    if (!g_lock) g_lock = SDL_CreateMutex(), g_rx = SDL_CreateMutex();
    SDL_UnlockSpinlock(&once);
    return g_lock;
}
// Up to 0.7 of full scale as it is, above it rounded off towards 1 (no hard clipping when raised)
static float soft_clip(float x) {
    float a = fabsf(x);
    if (a <= 0.7f) return x;
    float y = 0.7f + 0.3f * tanhf((a - 0.7f) / 0.3f);
    return x < 0 ? -y : y;
}
static float agc = 1.0f;  // (the microphone's automatic raise: the voice thread's)
static float mic_gain(void) {
    int p = SDL_GetAtomicInt(&g_preview_mic);
    return p >= 0 ? p / 100.0f : config_mic_gain();
}
static float voice_gain(void) {
    int p = SDL_GetAtomicInt(&g_preview_voice);
    return p >= 0 ? p / 100.0f * VOICE_BOOST : config_voice_gain();  // (the test: the menu is open, the window focused)
}
static bool encoder_open(void) {
    int err = 0;
    if (g_enc) return true;
    g_enc = opus_encoder_create(VOICE_RATE, 1, OPUS_APPLICATION_VOIP, &err);
    if (!g_enc) return LOG("[voice] Opus encoder: %s\n", opus_strerror(err)), false;
    opus_encoder_ctl(g_enc, OPUS_SET_BITRATE(VOICE_BITRATE));
    opus_encoder_ctl(g_enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    opus_encoder_ctl(g_enc, OPUS_SET_COMPLEXITY(5));  // (cheap: the game runs beside it)
    opus_encoder_ctl(g_enc, OPUS_SET_INBAND_FEC(1));  // (a lost packet partly rebuilt from the next one)
    opus_encoder_ctl(g_enc, OPUS_SET_PACKET_LOSS_PERC(5));
    return true;
}

// ---- receiving: a player's packet into their ring (any thread)
static void received(uint32_t id, uint16_t seq, const uint8_t *data, int len) {
    if (len <= 0 || len > VOICE_MAX_PACKET || !id) return;
    SDL_LockMutex(g_rx);
    Speaker *s = NULL, *free_one = NULL, *oldest = &g_spk[0];
    for (int i = 0; i < SPEAKERS; i++) {
        if (g_spk[i].id == id) s = &g_spk[i];
        if (!g_spk[i].id && !free_one) free_one = &g_spk[i];
        if (g_spk[i].heard < oldest->heard) oldest = &g_spk[i];
    }
    if (!s) {
        s = free_one ? free_one : oldest;
        if (!s->dec) {
            int err = 0;
            s->dec = opus_decoder_create(VOICE_RATE, 1, &err);
        } else {
            opus_decoder_ctl(s->dec, OPUS_RESET_STATE);
        }
        s->id = id, s->playing = false, s->made_up = 0;
        memset(s->ring, 0, sizeof s->ring);
    }
    s->heard = SDL_GetTicksNS();
    if (s->playing && (int16_t)(seq - s->next) < 0) {  // (too late: already played or made up)
        SDL_UnlockMutex(g_rx);
        return;
    }
    Slot *sl = &s->ring[seq % RING];
    sl->seq = seq, sl->len = (uint16_t)len;
    memcpy(sl->data, data, (size_t)len);
    SDL_UnlockMutex(g_rx);
}
void voice_aux_received(const void *data, uint32_t len) {
    Header h;
    if (len <= sizeof h) return;
    memcpy(&h, data, sizeof h);
    if (h.magic != VOICE_MAGIC || !SDL_GetAtomicInt(&g_running) || !config_voice_mode()) return;
    received(h.id, h.seq, (const uint8_t *)data + sizeof h, (int)(len - sizeof h));
}

// ---- playing: one 10 ms frame of every player's voice, mixed (the voice thread)
// The packets waiting (from `from` on while playing; all of them before), and the earliest
static int buffered(const Speaker *s, uint16_t *lowest) {
    int n = 0;
    bool any = false;
    for (int i = 0; i < RING; i++) {
        const Slot *sl = &s->ring[i];
        if (!sl->len || (s->playing && (int16_t)(sl->seq - s->next) < 0)) continue;
        n++;
        if (!any || (int16_t)(sl->seq - *lowest) < 0) *lowest = sl->seq;
        any = true;
    }
    return n;
}
static bool mix_frame(int16_t *out) {
    int32_t sum[VOICE_FRAME] = {0};
    int16_t pcm[VOICE_FRAME];
    int talking = 0;
    SDL_LockMutex(g_rx);
    for (int i = 0; i < SPEAKERS; i++) {
        Speaker *s = &g_spk[i];
        if (!s->id || !s->dec) continue;
        int percent = 100;
        for (int v = 0; v < SPEAKERS; v++)
            if (g_volumes[v].id == s->id) percent = g_volumes[v].percent;
        uint16_t lowest = 0;
        int n = buffered(s, &lowest);
        if (!s->playing) {
            if (n < START_FRAMES) continue;
            s->playing = true, s->next = lowest, s->made_up = 0;
        } else if (n > SKIP_FRAMES) {  // (behind: catch up, the oldest dropped)
            s->next = (uint16_t)(lowest + (n - START_FRAMES));
        }
        Slot *sl = &s->ring[s->next % RING];
        int got;
        if (sl->len && sl->seq == s->next) {
            got = opus_decode(s->dec, sl->data, sl->len, pcm, VOICE_FRAME, 0);
            sl->len = 0, s->made_up = 0;
        } else {
            Slot *after = &s->ring[(uint16_t)(s->next + 1) % RING];
            got = after->len && after->seq == (uint16_t)(s->next + 1)
                      ? opus_decode(s->dec, after->data, after->len, pcm, VOICE_FRAME, 1)  // (from the next one's FEC)
                      : opus_decode(s->dec, NULL, 0, pcm, VOICE_FRAME, 0);
            if (++s->made_up >= QUIET_FRAMES) {  // (stopped talking: what's left is stale)
                s->playing = false;
                memset(s->ring, 0, sizeof s->ring);
            }
        }
        s->next++;
        if (got == VOICE_FRAME && percent > 0) {
            for (int k = 0; k < VOICE_FRAME; k++) sum[k] += pcm[k] * percent / 100;
            if (s->id != SELF_ID && !s->made_up) talking++;
        }
    }
    SDL_UnlockMutex(g_rx);
    SDL_SetAtomicInt(&g_talking, talking);
    bool any = false;
    float g = voice_gain();  // (here, not the stream's gain: past full scale it is rounded off, not cut)
    for (int k = 0; k < VOICE_FRAME; k++) {
        any |= sum[k] != 0;
        out[k] = (int16_t)(soft_clip(sum[k] * g / 32768.0f) * 32767.0f);
    }
    return any;
}

static int SDLCALL run(void *unused) {
    (void)unused;
    int16_t pcm[VOICE_FRAME], mixed[VOICE_FRAME];
    uint8_t msg[sizeof(Header) + VOICE_MAX_PACKET];
    float peak = 0.0f;
    int frames = 0, hangover = 0;
    uint16_t seq = 0;
    while (SDL_GetAtomicInt(&g_running)) {
        bool idle = true;
        // the microphone
        if (g_mic && SDL_GetAudioStreamAvailable(g_mic) >= (int)sizeof pcm &&
            SDL_GetAudioStreamData(g_mic, pcm, sizeof pcm) == (int)sizeof pcm) {
            idle = false;
            // the microphone's volume, then a quiet voice raised (up to AGC_MAX times, towards AGC_TARGET at its
            // loudest; slowly up, quickly down), rounded off near full scale
            float g = mic_gain(), raw_peak = 0.0f, frame_peak = 0.0f;
            for (int i = 0; i < VOICE_FRAME; i++) {
                float a = fabsf(pcm[i] * g) / 32768.0f;
                if (a > raw_peak) raw_peak = a;
            }
            if (raw_peak > GATE) {
                float want = AGC_TARGET / raw_peak;
                want = want < 1.0f ? 1.0f : want > AGC_MAX ? AGC_MAX : want;
                agc += (want - agc) * (want < agc ? 0.5f : 0.01f);
            }
            for (int i = 0; i < VOICE_FRAME; i++) {
                float v = soft_clip(pcm[i] * g * agc / 32768.0f);
                pcm[i] = (int16_t)(v * 32767.0f);
                if (fabsf(v) > frame_peak) frame_peak = fabsf(v);
            }
            if (frame_peak > peak) peak = frame_peak;
            if (++frames == 10) {  // (100 ms)
                SDL_SetAtomicInt(&g_level, (int)(peak * 1000.0f));
                peak = 0.0f, frames = 0;
            }
            bool test = SDL_GetAtomicInt(&g_test) != 0;
            int mode = config_voice_mode();
            bool net = mode && ldn_net_up();
            // Always: louder than a breath, and 300 ms after; push to talk: held, and 100 ms after (the last word)
            bool loud = mode == 2 ? SDL_GetAtomicInt(&g_ptt) != 0 : frame_peak > GATE;
            hangover = loud ? (mode == 2 ? 10 : HANGOVER) : hangover > 0 ? hangover - 1 : 0;
            SDL_SetAtomicInt(&g_sending, net && !test && hangover > 0);
            if ((test || net) && (hangover > 0 || (test && mode != 2))) {
                int n = opus_encode(g_enc, pcm, VOICE_FRAME, msg + sizeof(Header), VOICE_MAX_PACKET);
                if (n > 0) {
                    Header h = {VOICE_MAGIC, 0, seq, test ? SELF_ID : ldn_own_ip()};
                    memcpy(msg, &h, sizeof h);
                    if (test) received(SELF_ID, seq, msg + sizeof h, n);
                    else ldn_send_aux(msg, (uint32_t)(sizeof h + (size_t)n));
                }
            }
            seq++;
        }
        // the speakers: kept 20..40 ms ahead
        if (g_out && SDL_GetAudioStreamQueued(g_out) < 2 * VOICE_FRAME * (int)sizeof(int16_t)) {
            SDL_SetAudioStreamGain(g_out, 1.0f);
            if (mix_frame(mixed)) SDL_PutAudioStreamData(g_out, mixed, sizeof mixed), idle = false;
        }
        if (idle) SDL_Delay(2);
    }
    return 0;
}

static void close_all(void) {  // (g_lock held)
    if (g_thread) {
        SDL_SetAtomicInt(&g_running, 0);
        SDL_WaitThread(g_thread, NULL);
        g_thread = NULL;
        LOG("[voice] closed\n");
    }
    if (g_mic) SDL_DestroyAudioStream(g_mic), g_mic = NULL;
    if (g_out) SDL_DestroyAudioStream(g_out), g_out = NULL;
    SDL_LockMutex(g_rx);
    for (int i = 0; i < SPEAKERS; i++) {
        if (g_spk[i].dec) opus_decoder_destroy(g_spk[i].dec);
        memset(&g_spk[i], 0, sizeof g_spk[i]);
    }
    SDL_UnlockMutex(g_rx);
    SDL_SetAtomicInt(&g_level, 0);
    SDL_SetAtomicInt(&g_talking, 0);
    SDL_SetAtomicInt(&g_sending, 0);
}
static void open_all(void) {  // (g_lock held)
    g_error[0] = 0;
    SDL_AudioSpec spec = {SDL_AUDIO_S16, 1, VOICE_RATE};
    if (!SDL_WasInit(SDL_INIT_AUDIO) && !SDL_InitSubSystem(SDL_INIT_AUDIO))
        SDL_strlcpy(g_error, "No sound device", sizeof g_error);
    else if (!encoder_open())
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
        LOG("[voice] open: %s -> %s\n", SDL_GetAudioDeviceName(SDL_GetAudioStreamDevice(g_mic)),
            SDL_GetAudioDeviceName(SDL_GetAudioStreamDevice(g_out)));
}

void voice_tick(void) {
    SDL_SetAtomicInt(&g_ptt, host_talk_held());  // (read here, on the game's thread, beside the game's own input)
    static bool failed;  // (an open that failed isn't tried again every frame: once more after voice chat was off)
    bool want = SDL_GetAtomicInt(&g_test) || (config_voice_mode() && ldn_net_up());
    if (!want) failed = false;
    if (want == (g_thread != NULL) || (want && failed)) return;
    SDL_LockMutex(lock());
    if (want && !g_thread) open_all(), failed = g_error[0] != 0;
    else if (!want && g_thread) close_all();
    SDL_UnlockMutex(g_lock);
}
void voice_test(bool on) {
    SDL_LockMutex(lock());
    SDL_SetAtomicInt(&g_test, on);
    if (!on) SDL_SetAtomicInt(&g_preview_mic, -1), SDL_SetAtomicInt(&g_preview_voice, -1);
    bool want = on || (config_voice_mode() && ldn_net_up());
    if (want && !g_thread) open_all();
    else if (!want && g_thread) close_all();
    if (on && g_error[0]) SDL_SetAtomicInt(&g_test, 0);
    LOG("[voice] mic test %s\n", on ? "on" : "off");
    SDL_UnlockMutex(g_lock);
}
bool voice_test_on(void) { return SDL_GetAtomicInt(&g_test) != 0; }
float voice_mic_level(void) { return SDL_GetAtomicInt(&g_level) / 1000.0f; }
int voice_talking(void) { return SDL_GetAtomicInt(&g_talking); }
bool voice_sending(void) { return SDL_GetAtomicInt(&g_sending) != 0; }
int voice_speakers(uint32_t *ids, int max) {
    int n = 0;
    if (!g_rx) return 0;
    SDL_LockMutex(g_rx);
    for (int i = 0; i < SPEAKERS && n < max; i++)
        if (g_spk[i].id && g_spk[i].id != SELF_ID && g_spk[i].playing && !g_spk[i].made_up) ids[n++] = g_spk[i].id;
    SDL_UnlockMutex(g_rx);
    return n;
}
void voice_set_player_volume(uint32_t id, int percent) {
    lock();
    SDL_LockMutex(g_rx);
    int at = -1;  // (its own slot, else a free one)
    for (int i = 0; i < SPEAKERS; i++)
        if (g_volumes[i].id == id) at = i;
    for (int i = 0; i < SPEAKERS && at < 0; i++)
        if (!g_volumes[i].id) at = i;
    if (at >= 0) g_volumes[at].id = percent == 100 ? 0 : id, g_volumes[at].percent = percent;
    SDL_UnlockMutex(g_rx);
}
int voice_player_volume(uint32_t id) {
    int percent = 100;
    if (!g_rx) return percent;
    SDL_LockMutex(g_rx);
    for (int i = 0; i < SPEAKERS; i++)
        if (g_volumes[i].id == id) percent = g_volumes[i].percent;
    SDL_UnlockMutex(g_rx);
    return percent;
}
const char *voice_error(void) { return g_error; }
void voice_preview(int mic_percent, int voice_percent) {
    SDL_SetAtomicInt(&g_preview_mic, mic_percent);
    SDL_SetAtomicInt(&g_preview_voice, voice_percent);
}
void voice_shutdown(void) {
    SDL_LockMutex(lock());
    SDL_SetAtomicInt(&g_test, 0);
    close_all();
    if (g_enc) opus_encoder_destroy(g_enc), g_enc = NULL;
    SDL_UnlockMutex(g_lock);
}

int voice_selftest(void) {
    lock();
    if (!encoder_open()) return -1;
    int16_t pcm[VOICE_FRAME], back[VOICE_FRAME];
    uint8_t packet[VOICE_MAX_PACKET];
    int last = -1;
    for (int f = 0; f < 50; f++) {  // (half a second of a 440 Hz tone, through the jitter buffer as the test's voice)
        for (int i = 0; i < VOICE_FRAME; i++)
            pcm[i] = (int16_t)(8000.0 * sin(2.0 * 3.14159265358979 * 440.0 * (f * VOICE_FRAME + i) / VOICE_RATE));
        int n = opus_encode(g_enc, pcm, VOICE_FRAME, packet, sizeof packet);
        if (n <= 0) return -1;
        if (f % 7 != 3) received(SELF_ID, (uint16_t)f, packet, n);  // (every 7th lost: made up)
        if (f >= 2 && !mix_frame(back)) return -1;
        last = n;
    }
    close_all();
    return last;
}
#endif
