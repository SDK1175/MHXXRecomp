// The game this port runs: Monster Hunter XX Nintendo Switch Ver. (Japan), version 1.5.1.
// Everything that names the game, its version or its files lives here, so another version (or the game this port
// was forked from, MHGU 1.4.0) is one header to compare against.
//
// Checked 2026-11-06 against a dump: the exefs of the Korean fan translation (MHXX_NX_1.5_kor_v2) is byte for byte
// the 1.5.1 update's exefs (rtld, main, subsdk0, sdk: same sha256). The translation is RomFS data only, so one
// recompiled main serves the Japanese game and the Korean patch alike; the patch goes in mods/ (runtime/mods.h).
#pragma once
#include <stdint.h>

#define GAME_TITLE_ID      0x0100C3800049C000ull
#define GAME_TITLE_ID_STR  "0100C3800049C000"
#define GAME_NAME          "Monster Hunter XX Nintendo Switch Ver."
#define GAME_SHORT_NAME    "MHXX"
#define GAME_VERSION       "1.5.1"

// exefs/main of 1.5.1 (NSO header +0x40, first 8 bytes of the build id). The recompiled code is this build's.
#define GAME_MAIN_BUILD_ID     {0x9d, 0x4c, 0x86, 0xe6, 0xef, 0x74, 0x50, 0x4a}
#define GAME_MAIN_BUILD_ID_STR "9D4C86E6EF74504A"
// the other modules of 1.5.1, for messages and for telling a mod's copy of them from the game's (runtime/loader.c)
#define GAME_SDK_BUILD_ID_STR     "CB8E7F9A74A4A854"
#define GAME_SUBSDK0_BUILD_ID_STR "C2774FA4CACE9ECC"

// The smallest file count of a complete 1.5.1 RomFS (base game + update merged). 0 until it is measured on a real
// dump: the count check is off then, the shader package check (main.c) still runs. GU 1.4.0's was 35531 (checked
// against 35000).
#define GAME_ROMFS_MIN_FILES 0

// The port's own name: window title, data folder (%APPDATA%\<PORT_DIR>, ~/.local/share/<PORT_DIR>), so its saves and
// settings never mix with MHGU Recomp's.
#define PORT_NAME "MHXX Recomp"
#define PORT_DIR  "MHXXRecomp"

// Language the game is told the system uses (MHGU_LANG overrides). The Japanese release has only Japanese.
#define GAME_DEFAULT_LANG "ja"
