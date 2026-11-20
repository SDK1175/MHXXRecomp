// Data mods made for MHGU 1.4.0 (English), put into MHXX's files (gumods.c).
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The textures a mod's MHGU files (nativeNX/eng, nativeNX/loc) changed. Whole ones as archives holding only those, at
// XX's paths: `out` gets the folder (a RomFS root, nativeNX inside) to lay over the game's after the mod's own (false:
// none). Menu textures as pictures of the mod's tiles over XX's (gumods_picture). `current` gives the host file a
// RomFS path ("nativeNX/arc/resident.arc") is now, mods so far laid over the game's.
bool gumods_build(const char *mod_romfs, const char *game_romfs, const char *(*current)(const char *rel), char *out,
                  size_t outsz);

// Pictures made for the texture with this texture pack key (texpack.c): RGBA8, w x h (free it); NULL: none
bool gumods_pictures(void);
uint8_t *gumods_picture(uint64_t key, uint32_t *w, uint32_t *h);
