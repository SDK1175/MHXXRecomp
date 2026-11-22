// Quest clear skip: after a quest is cleared, R3 (the right stick's click) ends the wait for the return at once,
// as the later games' "skip" does, instead of the 20 (or 60) seconds and the closing screens.
//
// The quest's end is run by a sequence object (r0 of the handlers below; MHXX 1.5.1, file offsets + 0x200000):
//   0x895ff0  while playing: when sQuest's end state (+0x50, read by 0x5aaac0) says the quest is over, +0xbc = sQuest
//             +0xd0, the wait in frames (20 s at 30 fps after a clear), and +0xc4 the kind of end: 1 runs 0x896140,
//             2 runs 0x896304 (2 for states 3..7, 1 for 8..9; which of them is the clear: the log tells)
//   both      +6 (u8) their step (0 start, 1 the wait, 2 leaving); +0xbc counts down a frame at a time and the
//             closing stages (+7) wait on it (0x896140's: the wait, another 15 s with its fade at 3 s, then the camera)
// While the skip is on, the handler gets +0xbc at 0 before it runs, so each stage that waits on it ends at once;
// stages that wait for anything else (a fade, the camera) still do. The skip is asked for in either kind.
// Only alone (no other player in the network): the others' games keep the host's clock. [Application] QuestClearSkip.
#include "../hle.h"
#include "../config.h"
#include "../overlay.h"
#include "../settings_locale.h"
#include "../net/net.h"
#include "input.h"

#define FN_END_A     0x896140u  // the quest's end, one of its two kinds (r0 = the sequence object)
#define FN_END_B     0x896304u  // the other kind; which is the clear is told by the log ([questskip] ... kind)
#define SEQ_STEP     6u         // u8: 0 start, 1 the wait, 2 leaving
#define SEQ_STAGE    7u         // u8: the closing stage
#define SEQ_WAIT     0xbcu      // s32: frames left
#define SEQ_KIND     0xc4u      // which end (1: 0x896140's, 2: 0x896304's)

#define UI_TEXT(s) s

static bool alone(void) {
    uint32_t ips[8];
    char names[8][33];
    return !ldn_net_up() || ldn_players(ips, names, 8) == 0;
}

static void quest_end(Cpu *c, uint32_t fn) {
    uint32_t seq = c->r[0];
    bool valid = seq >= 0x08000000u && seq < 0xEC000000u && !(seq & 3);
    static bool skip, was, told;
    if (valid) {
        uint8_t step = LD8(seq + SEQ_STEP);
        if (step == 0) {  // (a new end: nothing pressed yet)
            LOG("[questskip] quest end, kind %u (%#x), wait %d\n", LD32(seq + SEQ_KIND), fn,
                                   (int32_t)LD32(seq + SEQ_WAIT));
            skip = false;
            told = false;
        }
        bool on = config_live_quest_skip() && alone();
        if (on && step == 1 && !told) {
            told = true;
            int lang = config_language();
            overlay_notice(settings_text(lang, UI_TEXT("Quest over: press R3 to return now")),
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
    fnhook_call_original(c, fn);
}

static void end_a(Cpu *c) { quest_end(c, FN_END_A); }
static void end_b(Cpu *c) { quest_end(c, FN_END_B); }

void questskip_init(void) {
    fnhook_set(FN_END_A, hle_register("questskip:end_a", end_a));
    fnhook_set(FN_END_B, hle_register("questskip:end_b", end_b));
}
