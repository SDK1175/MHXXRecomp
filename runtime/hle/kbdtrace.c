// (debug, MHXX) The software keyboard's way through the game, logged: at character creation the name entry shows no
// keyboard (2026-11-06), and the port's swkbd.c logs nothing (no Appear, no ShowKeyboard), so the game stops before
// asking for one. Logged here: the keyboard driver's state (its update 0xa56ff0, state at +0x2ec8 and the flags at
// +0x2ecc..+0x2ecf) whenever it changes, its keyboard setup (0xa57b0c), the full-screen keyboard object being made
// (0xa5697c) and its job that shows it (0xa56bd8, ShowKeyboard). MHXX 1.5.1 addresses, found in its main
// (runtime/game/addr.h, KBDTRACE_*). MHGU_NO_KBDTRACE=1: off.
#include "../game/addr.h"
#include <stdlib.h>

#include "../hle.h"

#define FN_KBD_UPDATE   A_KBDTRACE_FN_KBD_UPDATE
#define FN_KBD_SETUP    A_KBDTRACE_FN_KBD_SETUP
#define FN_SOFTKBD_CTOR A_KBDTRACE_FN_SOFTKBD_CTOR
#define FN_SOFTKBD_JOB  A_KBDTRACE_FN_SOFTKBD_JOB
#define FN_ENTRY_UPDATE A_KBDTRACE_FN_ENTRY_UPDATE
#define FN_MGR_OPEN     A_KBDTRACE_FN_MGR_OPEN

static uint32_t g_last_obj, g_last_state = ~0u, g_last_flags = ~0u, g_calls;

static void kbd_update(Cpu *c) {
    uint32_t obj = c->r[0];
    if (!(g_calls++)) LOG("[kbdtrace] keyboard driver updated for the first time (object %08x)\n", obj);
    fnhook_call_original(c, FN_KBD_UPDATE);
    uint32_t state = LD32(obj + 0x2ec8), flags = LD32(obj + 0x2ecc);
    if (obj != g_last_obj || state != g_last_state || flags != g_last_flags) {
        LOG("[kbdtrace] driver %08x: state %u, flags %02x %02x %02x %02x, inline state %u (update #%u)\n", obj, state,
            flags & 0xff, (flags >> 8) & 0xff, (flags >> 16) & 0xff, flags >> 24, LD32(obj + 0x24), g_calls);
        g_last_obj = obj, g_last_state = state, g_last_flags = flags;
    }
}

static void kbd_setup(Cpu *c) {
    uint32_t obj = c->r[0];
    LOG("[kbdtrace] keyboard setup (object %08x, called from %08x)\n", obj, c->r[14]);
    fnhook_call_original(c, FN_KBD_SETUP);
}

static void softkbd_ctor(Cpu *c) {
    uint32_t obj = c->r[0];
    LOG("[kbdtrace] full-screen keyboard object made (%08x, called from %08x)\n", obj, c->r[14]);
    fnhook_call_original(c, FN_SOFTKBD_CTOR);
}

static void softkbd_job(Cpu *c) {
    uint32_t obj = c->r[0];
    LOG("[kbdtrace] full-screen keyboard job runs (%08x)\n", obj);
    fnhook_call_original(c, FN_SOFTKBD_JOB);
    LOG("[kbdtrace] full-screen keyboard job done: state %u\n", LD32(obj + 0x10));
}

// The text entry that uses a keyboard (its update 0x755a40: step at +0x1c, 0 idle .. 4 keyboard shown; +0x20 set up,
// +0x40 asked to open, +0x3124 its keyboard driver), logged when its step or flags change
static uint32_t e_obj, e_step = ~0u, e_flags = ~0u;
static void entry_update(Cpu *c) {
    uint32_t obj = c->r[0];
    fnhook_call_original(c, FN_ENTRY_UPDATE);
    uint32_t step = LD32(obj + 0x1c), flags = LD8(obj + 0x20) | LD8(obj + 0x40) << 8 | LD8(obj + 0x2ac) << 16 |
                    (uint32_t)LD8(obj + 0x2ad) << 24;
    if (obj != e_obj || step != e_step || flags != e_flags) {
        LOG("[kbdtrace] text entry %08x: step %u, set up %u, open asked %u, +2ac %u, +2ad %u, driver %08x\n", obj, step,
            flags & 0xff, (flags >> 8) & 0xff, (flags >> 16) & 0xff, flags >> 24, LD32(obj + 0x3124));
        e_obj = obj, e_step = step, e_flags = flags;
    }
}

// The keyboard manager's open request (0xa6068c(manager, keyboard, ?) -> opened): every call while it fails is one
// log line a second at most
static void mgr_open(Cpu *c) {
    uint32_t mgr = c->r[0], kbd = c->r[1], arg = c->r[2];
    uint32_t kind = kbd ? LD32(kbd + 4) : 0, before = kbd ? LD32(kbd + 0x2ec8) : 0;
    fnhook_call_original(c, FN_MGR_OPEN);
    static uint64_t last;
    uint64_t now = plat_now_ns();
    if (c->r[0] || now - last > 1000000000ull) {
        LOG("[kbdtrace] manager %08x open(keyboard %08x kind %u, %u): %u (driver state %u -> %u; queue %u of %u)\n", mgr,
            kbd, kind, arg, c->r[0], before, kbd ? LD32(kbd + 0x2ec8) : 0, LD32(mgr + 0x20), LD32(mgr + 0x1c));
        last = now;
    }
}

void kbdtrace_init(void) {
    PORT_OFF(KBDTRACE, "keyboard trace (debug)");
    if (getenv("MHGU_NO_KBDTRACE")) return;
    fnhook_set(FN_KBD_UPDATE, hle_register("kbdtrace:update", kbd_update));
    fnhook_set(FN_KBD_SETUP, hle_register("kbdtrace:setup", kbd_setup));
    fnhook_set(FN_SOFTKBD_CTOR, hle_register("kbdtrace:softkbd_ctor", softkbd_ctor));
    fnhook_set(FN_SOFTKBD_JOB, hle_register("kbdtrace:softkbd_job", softkbd_job));
    fnhook_set(FN_ENTRY_UPDATE, hle_register("kbdtrace:entry_update", entry_update));
    fnhook_set(FN_MGR_OPEN, hle_register("kbdtrace:mgr_open", mgr_open));
    LOG("[kbdtrace] keyboard driver traced (MHGU_NO_KBDTRACE=1: off)\n");
}
