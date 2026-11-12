#pragma once
// Voice chat (0.10.0): the microphone and the speakers through Opus (48 kHz mono, 10 ms frames, voice.c), sent to the
// other games of the multiplayer network as the port's own messages (net/ldn.c F_AUX: the host passes them on), each
// player's voice kept in a small jitter buffer and mixed. [Voice] VoiceChat: Off (the default: the microphone stays
// closed), Always (sent while it hears more than a breath) or PushToTalk (while TalkKey / TalkButton is held). The F1 menu's microphone test plays your voice back through the
// same path, packed and unpacked as it is sent.
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VOICE_RATE 48000
#define VOICE_FRAME 480  // samples in one packet: 10 ms

// Each frame, from the game's thread: the devices opened while the test runs or voice chat is on in a network, closed
// when neither
void voice_tick(void);
// A voice packet from another game (ldn.c, its F_AUX messages: those that aren't one are left alone)
void voice_aux_received(const void *data, uint32_t len);
// The microphone test, on or off (the menu: off when it closes)
void voice_test(bool on);
bool voice_test_on(void);
// The microphone's level over the last 100 ms, 0..1 (after [Voice] MicVolume), while it is open
float voice_mic_level(void);
// How many other players' voices are playing now (0..), and which (their network addresses: ldn_players' names)
int voice_talking(void);
int voice_speakers(uint32_t *ids, int max);
// A player's hunter name as their game sent it (their save's, UTF-8), false: not heard yet (ldn_players asks)
bool voice_player_name(uint32_t id, char out[33]);
// This player's voice is being sent now (the overlay's microphone mark)
bool voice_sending(void);
// A player's volume in this game only (percent, 0: muted; 100: as the rest), until the game closes
void voice_set_player_volume(uint32_t id, int percent);
int voice_player_volume(uint32_t id);
// What went wrong opening it (no microphone, Opus), in English (tr in the menu); "" while it works
const char *voice_error(void);
// Its Opus round trip: 10 ms of samples (VOICE_FRAME, S16) packed and unpacked, the packet's size or -1 (a test)
int voice_selftest(void);
// While the test runs: the volumes as the menu shows them, not saved yet (percent; -1: the saved ones)
void voice_preview(int mic_percent, int voice_percent);
// Closing the game: the devices closed, the thread ended
void voice_shutdown(void);

#ifdef __cplusplus
}
#endif
