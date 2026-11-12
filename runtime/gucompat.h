#pragma once
// Plugins made for the English game (MHGU 1.4.0) in MHXX 1.5.1 (gucompat.c): they are given a base address in a
// region of its own where the game's code is laid out at GU's addresses. Reading it shows MHXX's code as GU has it,
// calling into it runs MHXX's function, and Forge's hooks and patches there go to MHXX's code.
#include <stdbool.h>
#include <stdint.h>

#define GU_SHADOW      0xA0000000u  // (between the heap's end and the alias region: nothing else is there)
#define GU_SHADOW_SIZE 0x01800000u

// Built once, when the first plugin for GU asks for the game's base. false: this build has no table
bool gucompat_build(void);
static inline bool gucompat_in_shadow(uint32_t a) { return a - GU_SHADOW < GU_SHADOW_SIZE; }
// The address in MHXX of an address in the region, 0: not one the table knows (the first few are logged)
uint32_t gucompat_real(uint32_t a);
// MHXX's code at [real, real + n) changed (a hook, a patch): the region shows the same bytes
void gucompat_mirror(uint32_t real, uint32_t n);
// How many addresses were asked for that the table does not know (the menu's note)
int gucompat_unknown(void);
