// (debug, MHXX) The software keyboard's way through the game, logged: at character creation the name entry shows no
// keyboard (2026-11-06), and the port's swkbd.c logs nothing (no Appear, no ShowKeyboard), so the game stops before
// asking for one. Logged here: the keyboard driver's state (its update 0xa56ff0, state at +0x2ec8 and the flags at
// +0x2ecc..+0x2ecf) whenever it changes, its keyboard setup (0xa57b0c), the full-screen keyboard object being made
// (0xa5697c) and its job that shows it (0xa56bd8, ShowKeyboard). MHXX 1.5.1 addresses, found in its main
// (runtime/game/addr.h, KBDTRACE_*). MHGU_NO_KBDTRACE=1: off.
#include "../game/addr.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../hle.h"

#define FN_KBD_UPDATE   A_KBDTRACE_FN_KBD_UPDATE
#define FN_KBD_SETUP    A_KBDTRACE_FN_KBD_SETUP
#define FN_SOFTKBD_CTOR A_KBDTRACE_FN_SOFTKBD_CTOR
#define FN_SOFTKBD_JOB  A_KBDTRACE_FN_SOFTKBD_JOB
#define FN_ENTRY_UPDATE A_KBDTRACE_FN_ENTRY_UPDATE
#define FN_MGR_OPEN     A_KBDTRACE_FN_MGR_OPEN
#define FN_FACTORY      A_KBDTRACE_FN_FACTORY

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

// The keyboard manager's factory (0xa60514(manager, kind) -> keyboard object; 0 full-screen, 1 the inline driver,
// 2-5 others): which kind the game asks for, from where
static void factory(Cpu *c) {
    uint32_t kind = c->r[1], from = c->r[14];
    fnhook_call_original(c, FN_FACTORY);
    LOG("[kbdtrace] keyboard of kind %u asked for (from %08x): %08x\n", kind, from, c->r[0]);
}

// The 16 places that open the global text entry (0x758360) sit in these 13 functions (MHXX 1.5.1). Each one's first
// call is logged with the frame, and the open itself with its caller: the character creation's name entry (no
// keyboard) should be the one whose function runs without the open following.
static const uint32_t g_sites[] = {0x3c70f0, 0x3cc3fc, 0x790f38, 0x796a38, 0x8664b4, 0x87d830, 0x88a0d0,
                                   0x91dd00, 0x922280, 0x94b38c, 0x94ed10, 0x969114, 0x96c83c};
#define NSITES (sizeof g_sites / sizeof *g_sites)
static int g_site_seen[NSITES];
static void site_hit(Cpu *c, unsigned i) {
    if (!g_site_seen[i]++)
        LOG("[kbdtrace] entry site %08x first runs (frame %llu, from %08x)\n", g_sites[i],
            (unsigned long long)g_present_count, c->r[14]);
    fnhook_call_original(c, g_sites[i]);
}
#define SITE(i) static void site_##i(Cpu *c) { site_hit(c, i); }
SITE(0) SITE(1) SITE(2) SITE(3) SITE(4) SITE(5) SITE(6) SITE(7) SITE(8) SITE(9) SITE(10) SITE(11) SITE(12)
static const HostFn g_site_fns[NSITES] = {site_0, site_1, site_2, site_3, site_4, site_5, site_6,
                                          site_7, site_8, site_9, site_10, site_11, site_12};

// The message box the name step shows ("please enter a name", 0x842420(box): +0x4c its step, +0x50 its kind: most
// kinds close on the decide button, kind 11 waits for a sub-object at +0x68 instead): logged when anything changes
#define FN_MSGBOX_UPDATE 0x842420u
static uint32_t m_obj, m_sig = ~0u;
static void msgbox_update(Cpu *c) {
    uint32_t obj = c->r[0];
    fnhook_call_original(c, FN_MSGBOX_UPDATE);
    uint32_t step = LD32(obj + 0x4c), kind = LD32(obj + 0x50), res = LD32(obj + 0x54), btn = LD32(obj + 0x58),
             wait = LD32(obj + 0x5c), sub = LD32(obj + 0x68), subvt = sub && sub >= 0x08000000u ? LD32(sub) : 0;
    uint32_t sig = step ^ kind << 4 ^ res << 8 ^ btn ^ wait << 16 ^ sub;
    if (obj != m_obj || sig != m_sig) {
        LOG("[kbdtrace] message box %08x: step %u, kind %u, result %u, button %x, wait %u, sub-object %08x (vtable %08x)\n",
            obj, step, kind, res, btn, wait, sub, subvt);
        m_obj = obj, m_sig = sig;
    }
}

#define FN_ENTRY_OPEN 0x758360u  // the global text entry's open (entry), as the 16 places call it
static void entry_open(Cpu *c) {
    LOG("[kbdtrace] text entry opened (from %08x, frame %llu)\n", c->r[14], (unsigned long long)g_present_count);
    fnhook_call_original(c, FN_ENTRY_OPEN);
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
    fnhook_set(FN_FACTORY, hle_register("kbdtrace:factory", factory));
    fnhook_set(FN_ENTRY_OPEN, hle_register("kbdtrace:entry_open", entry_open));
    fnhook_set(FN_MSGBOX_UPDATE, hle_register("kbdtrace:msgbox_update", msgbox_update));
    for (unsigned i = 0; i < NSITES; i++) {
        char name[32];
        snprintf(name, sizeof name, "kbdtrace:site_%x", g_sites[i]);
        fnhook_set(g_sites[i], hle_register(strdup(name), g_site_fns[i]));
    }
    LOG("[kbdtrace] keyboard driver traced (MHGU_NO_KBDTRACE=1: off)\n");
}
