// Quest clear skip: after a quest is cleared, R3 (the right stick's click) ends the wait for the return at once,
// as the later games' "skip" does, instead of the 20 (or 60) seconds and the closing screens.
//
// The quest's end is run by a sequence object (r0 of the handlers below; MHXX 1.5.1, file offsets + 0x200000):
//   0x895ff0  while playing: when sQuest says cleared (1) or failed (2), +0xbc = sQuest +0xd0, the wait in frames
//             (20 s at 30 fps; 60 s without mFlgReturnTime20), and +0xc4 = 1 cleared / 2 failed
//   0x896140  cleared: +6 (u8) its step (0 start, 1 the wait, 2 leaving); +0xbc counts down a frame at a time and
//             0x89651c moves +7 (u8) through the closing stages on it: 0 the wait itself, 1 another 15 s (its fade
//             at 3 s), 2 until below 0, 3..5 the camera and fade, then the return
// While the skip is on, the cleared handler gets +0xbc at 0 before it runs, so each stage that waits on it ends at
// once; stages that wait for anything else (a fade, the camera) still do. Failed quests keep their wait.
// Only alone (no other player in the network): the others' games keep the host's clock. [Application] QuestClearSkip.
#include "../hle.h"
#include "../config.h"
#include "../overlay.h"
#include "../settings_locale.h"
#include "../net/net.h"
#include "input.h"

#define FN_CLEARED   0x896140u  // the cleared quest's handler (r0 = the sequence object); GU 1.4.0: not looked up
#define SEQ_STEP     6u         // u8: 0 start, 1 the wait, 2 leaving
#define SEQ_STAGE    7u         // u8: the closing stage (0..5)
#define SEQ_WAIT     0xbcu      // s32: frames left
#define SEQ_RESULT   0xc4u      // 1 cleared, 2 failed

#define UI_TEXT(s) s

static bool alone(void) {
    uint32_t ips[8];
    char names[8][33];
    return !ldn_net_up() || ldn_players(ips, names, 8) == 0;
}

static void cleared(Cpu *c) {
    uint32_t seq = c->r[0];
    bool valid = seq >= 0x08000000u && seq < 0xEC000000u && !(seq & 3);
    static bool skip, was, told;
    if (valid && LD32(seq + SEQ_RESULT) == 1) {
        uint8_t step = LD8(seq + SEQ_STEP);
        if (step == 0) {  // (a new clear: nothing pressed yet)
            skip = false;
            told = false;
        }
        bool on = config_live_quest_skip() && alone();
        if (on && step == 1 && !told) {
            told = true;
            int lang = config_language();
            overlay_notice(settings_text(lang, UI_TEXT("Quest cleared: press R3 to return now")),
                           settings_text(lang, UI_TEXT("Right stick click (keyboard: Alt). Turn it off in the settings menu (F1).")));
        }
        int32_t sticks[4];
        uint64_t buttons;
        input_get(&buttons, sticks);
        bool down = (buttons >> BTN_RSTICK) & 1;
        if (on && step == 1 && down && !was && !overlay_menu_open()) {
            if (!skip) LOG("[questskip] skipping the wait (%d frames left, stage %u)\n", (int32_t)LD32(seq + SEQ_WAIT),
                           LD8(seq + SEQ_STAGE));
            skip = true;
        }
        was = down;
        if (on && skip && step == 1 && (int32_t)LD32(seq + SEQ_WAIT) > 0) ST32(seq + SEQ_WAIT, 0);
    }
    fnhook_call_original(c, FN_CLEARED);
}

void questskip_init(void) {
    fnhook_set(FN_CLEARED, hle_register("questskip:cleared", cleared));
}
