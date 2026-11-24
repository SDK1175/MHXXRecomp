// The smithy's weapon trees: what the panel over the smithy shows (runtime/hle/smithytree.c; the panel:
// runtime/smithytree_ui.cpp).
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SMITHY_CHILDREN 8

typedef struct {
    char name[96];
    int low, top, attack;  // the levels with this name, and the attack at the top one
} SmithyTier;

typedef struct {
    uint32_t weapon;
    int top, attack;
    char name[96];
} SmithyChild;

typedef struct {
    int type;               // the weapon type (0 great sword .. 14 charge blade)
    uint32_t weapon, level; // under the cursor
    uint8_t screen, sub;    // the smithy's screen
    int ntiers;
    SmithyTier tier[3];
    uint32_t parent;  // made from (0: from materials)
    char parent_name[96];
    int nchildren;
    SmithyChild child[SMITHY_CHILDREN];
} SmithyView;

void smithytree_init(void);
bool smithytree_view(SmithyView *v);

#ifdef __cplusplus
}
#endif
