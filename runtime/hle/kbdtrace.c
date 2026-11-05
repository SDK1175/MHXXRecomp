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

void kbdtrace_init(void) {
    PORT_OFF(KBDTRACE, "keyboard trace (debug)");
    if (getenv("MHGU_NO_KBDTRACE")) return;
    fnhook_set(FN_KBD_UPDATE, hle_register("kbdtrace:update", kbd_update));
    fnhook_set(FN_KBD_SETUP, hle_register("kbdtrace:setup", kbd_setup));
    fnhook_set(FN_SOFTKBD_CTOR, hle_register("kbdtrace:softkbd_ctor", softkbd_ctor));
    fnhook_set(FN_SOFTKBD_JOB, hle_register("kbdtrace:softkbd_job", softkbd_job));
    LOG("[kbdtrace] keyboard driver traced (MHGU_NO_KBDTRACE=1: off)\n");
}
