#pragma once
// A newer release on GitHub while the game runs (updatecheck.c): its version as a small note for a while
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
void update_check_start(void);  // (main.c, once)
// The note to show now (SDL_GetTicksNS): the new version and how long it has shown (seconds)
bool update_notice(uint64_t now, char *version, size_t size, float *age);
#ifdef __cplusplus
}
#endif
