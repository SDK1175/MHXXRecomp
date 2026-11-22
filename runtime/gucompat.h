#pragma once
// Plugins made for the English game (MHGU 1.4.0) in MHXX 1.5.1 (gucompat.c): they are given a base address in a
// region of its own where the game's code is laid out at GU's addresses. Reading it shows MHXX's code as GU has it,
// calling into it runs MHXX's function, and Forge's hooks and patches there go to MHXX's code.
#include <stdbool.h>
#include <stdint.h>

// At the top of the stack region, which is told to the game 64 MB smaller (rt.h STACK_SIZE): nothing else is there,
// and it stays below 0xF0000000, the bound KatLib and MapAssist check addresses against. (It was at 0xA0000000, inside
// the heap MHXX asks for: 0x08000000 + 2.5 GB. QueryMemory said heap there, not code, and World Sharpness Gauge and
// Key Quest Markers stopped.)
#define GU_SHADOW      0xEC000000u
#define GU_SHADOW_SIZE 0x01800000u

// Built once, when the first plugin for GU asks for the game's base. false: this build has no table
bool gucompat_build(void);
bool gucompat_shown_as_gu(uint32_t a);
void gucompat_check_signatures(const char *plugin, uint32_t ro, uint32_t len);  // (log a plugin's checks of GU code)  // (a word of GU2XX_WORD: GU's, not MHXX's code)
static inline bool gucompat_in_shadow(uint32_t a) { return a - GU_SHADOW < GU_SHADOW_SIZE; }
// The address in MHXX of an address in the region, 0: not one the table knows (the first few are logged)
uint32_t gucompat_real(uint32_t a);
// MHXX's code at [real, real + n) changed (a hook, a patch): the region shows the same bytes
void gucompat_mirror(uint32_t real, uint32_t n);
// How many addresses were asked for that the table does not know (the menu's note)
int gucompat_unknown(void);
// After a plugin's forge_onInit: the game's import slots it wrote where the region shows them, given to MHXX's
void gucompat_imports_sync(const char *plugin);
// A plugin made for MHGU, loaded and not run yet: the offsets into GU's data its code holds (movw/movt pairs) changed
// to give MHXX's addresses when added to the region's base. How many were
int gucompat_fix_plugin(const char *name, uint32_t text, uint32_t size);
// The address in MHXX of GU's data at main + gu (an offset from GU's base), 0: not one the table knows
uint32_t gucompat_data(uint32_t gu);
// The address in MHXX of GU's code at main + gu, 0: not in the table
uint32_t gucompat_code(uint32_t gu);
