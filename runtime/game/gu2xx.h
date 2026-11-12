// GU 1.4.0 -> MHXX 1.5.1 addresses (offsets from the image base), for plugins made for the English game
// (runtime/gucompat.c). Numbers only. Each code pair stands for the whole function around it. Found 2026-11-12 from
// the code checks Damage Numbers r42 carries (its 15 addresses, and the calls in them).
static const struct { uint32_t gu, xx; } GU2XX_CODE[] = {
    {0x29d48, 0x294f8},
    {0x71078, 0x6ffa4},
    {0x812a8, 0x801d4},
    {0x97ddc, 0x96d08},
    {0x9d6f8, 0x9c508},
    {0xa2664, 0xa1474},
    {0xaac54, 0xa9990},
    {0xbab60, 0xb989c},
    {0xbad1c, 0xb9a58},
    {0xc03b8, 0xbf0f4},
    {0xdf814, 0xde550},
    {0x16dc6c, 0x16b25c},
    {0x16dd90, 0x16b380},
    {0x170968, 0x16df60},
    {0x178660, 0x175c58},
    {0x276278, 0x2734d0},
    {0x2762a0, 0x2734f8},
    {0x27bcc0, 0x278f04},
    {0x285c6c, 0x282c48},
    {0x2a87a8, 0x2a5708},
    {0x3f8398, 0x3f353c},
    {0x86a24c, 0x85ec7c},
    {0x885844, 0x87a260},
    {0xb8909c, 0xb7dab4},
    {0xb89900, 0xb7e318},
    {0xbc0874, 0xbb528c},
};
// Data shown at a GU address: len bytes of MHXX's at xx
static const struct { uint32_t gu, xx, len; } GU2XX_DATA[] = {
    {0x172e52c, 0x174fea4, 0x40},  // the build id note (its id: the words below)
};
// Words that differ where the code is the same (offsets to data, GU's build id)
static const struct { uint32_t gu, word; } GU2XX_WORD[] = {
    {0x172e53c, 0xd2f108fb},
    {0x172e540, 0x4f20d10f},
    {0x172e544, 0x720607e7},
    {0x172e548, 0x9b867f77},
    {0x27629c, 0x015c2928},
};
