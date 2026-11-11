#pragma once
// Voice chat (0.10.0), its first part: the microphone and the speakers through Opus (48 kHz mono, 10 ms frames,
// voice.c), and the microphone test of the F1 menu (Audio > Voice chat): what the microphone hears, packed and unpacked
// as it will be sent, played back at once, with its level. Sending it to the other players comes next (net/ldn.c).
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VOICE_RATE 48000
#define VOICE_FRAME 480  // samples in one packet: 10 ms

// The microphone test, on or off (it opens the default recording and playback devices; off closes them)
void voice_test(bool on);
bool voice_test_on(void);
// The microphone's level over the last 100 ms, 0..1 (after [Voice] MicVolume), while it is open
float voice_mic_level(void);
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
