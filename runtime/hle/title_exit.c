// B on the title screen's Main Menu asks to quit, as closing the window (Alt+F4, the X) does: the settings menu's
// "quit?" question (from MHGU Recomp's title_exit.c). The title sequence's update keeps its state at +4 (u8); MHXX's
// has 0x1c states to GU's 0x20, matched by their handlers' code: 0x0e is where the Main Menu arrives from (GU 0x10)
// and 0x0f the Main Menu (GU 0x11). Elsewhere (save select, options) B stays the game's Back. [Controls] TitleBackExit.
#include "../hle.h"
#include "../app_actions.h"
#include "../config.h"
#include "../overlay.h"
#include "input.h"

#define FN_TITLE_UPDATE 0x871764u  // the title sequence's update (r0 = the title object); GU 1.4.0: 0x87b544
#define TITLE_STATE     4u
#define STATE_MENU_IN   0x0eu
#define STATE_MAIN_MENU 0x0fu

static void title_update(Cpu *c) {
    uint32_t title = c->r[0];
    // (+4 before the call: the Main Menu's handler acts on B in the same update and moves +4 on)
    bool valid = title >= 0x08000000u && title < 0xEC000000u && !(title & 3);
    uint8_t state = valid ? LD8(title + TITLE_STATE) : 0xffu;
    static uint8_t last = 0xff;
    if (state != last) {
        static int logged;
        if (logged++ < 64) LOG("[title] state %#x\n", state);  // (which state is which screen, for checking)
        last = state;
    }
    bool main_menu = valid && (state == STATE_MAIN_MENU || state == STATE_MENU_IN);
    fnhook_call_original(c, FN_TITLE_UPDATE);

    int32_t sticks[4];
    uint64_t buttons;
    input_get(&buttons, sticks);  // (the buttons as the game sees them: B is its Back, whatever the pad's layout)
    static bool was, swallow, armed;
    bool down = (buttons >> BTN_B) & 1, pressed = down && !was;
    was = down;
    if (overlay_menu_open()) { swallow = true; armed = false; return; }  // (B there is the menu's)
    if (swallow) {
        if (!down) swallow = armed = false;
        return;
    }
    // (asked on the release: the menu opens on the next frame, and a B still held there would close it again)
    if (main_menu && pressed && config_live_title_back_exit()) armed = true;
    if (armed && !down) { armed = false; ui_request_exit_question(); }
}

void title_exit_init(void) {
    fnhook_set(FN_TITLE_UPDATE, hle_register("titleexit:update", title_update));
}
