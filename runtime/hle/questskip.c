// Quest end skip: when a quest is over, R3 (the right stick's click) ends the wait for the return at once, as the
// later games' skip does, instead of the 20 seconds and the closing screens.
//
// The quest's end is run by a sequence object (r0 of the handlers below; MHXX 1.5.1, file offsets + 0x200000):
//   0x895ff0  while playing: when sQuest's end state (+0x50, read by 0x5aaac0) says the quest is over, +0xbc = sQuest
//             +0xd0, the wait in frames (20 s at 30 fps after a clear), and +0xc4 the kind of end: 1 runs 0x896140,
//             2 runs 0x896304 (2 for states 3..7, 1 for 8..9)
//   both      +6 (u8) their step (0 start, 1 the wait, 2 leaving); +0xbc counts down a frame at a time and the
//             closing stages (+7) wait on it (0x896140's: the wait, another 15 s with its fade at 3 s, then the camera)
// While the skip is on, the handler gets +0xbc at 0 before it runs, so each stage that waits on it ends at once;
// stages that wait for anything else (a fade, the camera) still do.
//
// Alone (no other game in the network): R3 skips. With others: a vote. Every game whose quest is ending says so to
// the others (ldn_send_aux, 0xE9) twice a second and on every change, with whether its player has pressed R3 (whatever
// its settings: a game with the vote off still says it is ending, and never votes, so nobody leaves it behind). The skip
// comes when every game that is ending has pressed; those in the lobby or in another quest send nothing and don't count.
// A game counts the others only after a second of its own wait, so a member whose end started a moment later is heard
// first. Each game decides by itself on the same messages, so all go within a moment of each other.
// [Application] QuestClearSkip (the skip at all), QuestClearSkipOnline (the vote).
#include <stdio.h>
#include <string.h>

#include "../hle.h"
#include "../config.h"
#include "../overlay.h"
#include "../platform.h"
#include "../settings_locale.h"
#include "../net/net.h"
#include "input.h"

#define FN_END_A     0x896140u  // the quest's end, one of its two kinds (r0 = the sequence object)
#define FN_END_B     0x896304u  // the other kind
#define SEQ_STEP     6u         // u8: 0 start, 1 the wait, 2 leaving
#define SEQ_STAGE    7u         // u8: the closing stage
#define SEQ_WAIT     0xbcu      // s32: frames left
#define SEQ_KIND     0xc4u      // which end (1: 0x896140's, 2: 0x896304's)

#define VOTE_MAGIC   0xE9u      // (F_AUX: emsync 0xE5, hpscale 0xE6, voice 0xE7 / 0xE8)
#define VOTE_STALE   1500000000ull  // a game not heard from for this long is no longer ending
#define VOTE_SETTLE  1000000000ull  // our own wait this old before the others are counted
#define VOTE_EVERY   500000000ull
#define PEERS        8

#define UI_TEXT(s) s

typedef struct { uint8_t magic, ending, voted, pad; uint32_t ip; } Vote;
static struct { uint32_t ip; volatile uint64_t at; volatile uint8_t voted; } g_peer[PEERS];  // (network threads write)

void questskip_aux_received(const void *data, uint32_t len) {
    Vote m;
    if (len < sizeof m) return;
    memcpy(&m, data, sizeof m);
    if (m.magic != VOTE_MAGIC || !m.ip || m.ip == ldn_own_ip()) return;
    int s = 0, old = 0;
    while (s < PEERS && g_peer[s].ip != m.ip) {
        if (g_peer[s].at < g_peer[old].at) old = s;
        s++;
    }
    if (s == PEERS) s = old, g_peer[s].ip = m.ip;
    g_peer[s].voted = m.voted;
    g_peer[s].at = m.ending ? plat_now_ns() : 0;
}

// The others ending now: how many, and how many of them have pressed.
static int others(uint64_t now, int *voted) {
    int n = 0;
    *voted = 0;
    for (int s = 0; s < PEERS; s++) {
        uint64_t at = g_peer[s].at;
        if (at && now - at < VOTE_STALE) {
            n++;
            *voted += g_peer[s].voted != 0;
        }
    }
    return n;
}

static void send_vote(bool ending, bool voted) {
    Vote m = {VOTE_MAGIC, ending, voted, 0, ldn_own_ip()};
    if (m.ip) ldn_send_aux(&m, sizeof m);
}

static bool alone(void) {
    uint32_t ips[8];
    char names[8][33];
    return !ldn_net_up() || ldn_players(ips, names, 8) == 0;
}

static void show(int lang, const char *title, const char *text) {
    overlay_notice(settings_text(lang, title), settings_text(lang, text));
}

static void quest_end(Cpu *c, uint32_t fn) {
    uint32_t seq = c->r[0];
    bool valid = seq >= 0x08000000u && seq < 0xEC000000u && !(seq & 3);
    static bool skip, voted, was, told, ending, holding;
    static uint64_t since, sent;
    static int shown = -1;
    uint64_t now = plat_now_ns();
    if (valid) {
        uint8_t step = LD8(seq + SEQ_STEP);
        if (step == 0) {  // (a new end: nothing pressed yet)
            LOG("[questskip] quest end, kind %u (%#x), wait %d\n", LD32(seq + SEQ_KIND), fn, (int32_t)LD32(seq + SEQ_WAIT));
            skip = voted = told = false;
            shown = -1;
            since = now;
        }
        bool solo = alone(), on = config_live_quest_skip() && (solo || config_live_quest_skip_online());
        int lang = config_language();
        if (on && step == 1 && !told && solo) {
            told = true;
            show(lang, UI_TEXT("Quest over: press R3 to return now"),
                 UI_TEXT("Right stick click (keyboard: Alt). Turn it off in the settings menu (F1)."));
        }
        int32_t sticks[4];
        uint64_t buttons;
        input_get(&buttons, sticks);
        bool down = (buttons >> BTN_RSTICK) & 1;
        bool press = on && step == 1 && down && !was && !overlay_menu_open();
        was = down;
        if (step == 1 && !solo) {  // (the vote: said whatever the settings, so a game with it off is waited for)
            if (press && !voted) {
                voted = true;
                LOG("[questskip] voted to skip\n");
            }
            if (!ending || press || now - sent >= VOTE_EVERY) send_vote(true, voted), sent = now;
            ending = true;
            int yes, n = others(now, &yes);
            // (kept up for everyone, voted or not, until all have pressed or the wait is over, as in Wilds)
            int count = n + 1, done = yes + (voted ? 1 : 0), key = count * 16 + done + (voted ? 256 : 0);
            if (on && !skip && key != shown) {
                shown = key;
                char line[160];
                snprintf(line, sizeof line, settings_text(lang, UI_TEXT("Agreed: %d / %d hunters")), done, count);
                overlay_notice_hold(settings_text(lang, voted ? UI_TEXT("Return now: waiting for the others to press R3")
                                                              : UI_TEXT("Quest over: press R3 to vote to return now")),
                                    line);
                holding = true;
            }
            if (voted && !skip && now - since >= VOTE_SETTLE) {
                if (yes == n) {
                    skip = true;
                    LOG("[questskip] all %d voted: skipping the wait (%d frames left, stage %u)\n", count,
                        (int32_t)LD32(seq + SEQ_WAIT), LD8(seq + SEQ_STAGE));
                }
            }
        } else if (press && !skip) {
            skip = true;
            LOG("[questskip] skipping the wait (%d frames left, stage %u)\n", (int32_t)LD32(seq + SEQ_WAIT), LD8(seq + SEQ_STAGE));
        }
        if (holding && (skip || step != 1 || !on)) {  // (all agreed, or the wait is over: the box goes)
            overlay_notice_release();
            holding = false;
        }
        if (step != 1 && ending) {  // (our wait is over, skipped or not: no longer ending)
            send_vote(false, false);
            ending = false;
        }
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
