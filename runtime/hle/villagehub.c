// Village quests at the Hub's and the Hunters' Pub's quest counter (ini [Village Quests] VillageQuestsInHub), so they
// can be played together: L / R in the counter's rank menu or star menu switches between the Hub's quests and the
// Village quests.
//
// Quest numbers: group * 10000 + star * 100 + index (group 0 Village, 1 Hub, 2 Arena, 3 Village training, 4 Special
// Permit...). A counter's quests come from sQuestSelector: when an area loads, a job (main 0x5b7cf8) walks every quest
// of quest\quest_group (village\common.arc) through the filter 0x5b8090(list, quest, counter type) and loads the data
// of those it takes; categories (one per star, Training, Urgent...) are filled from that (0x5b85d8, 0x5bb318) and the
// counter UI (uUIQuestCounter) gets them through 0x5bc96c(selector, slot), which maps slots to categories by a table
// per counter type. The counter type is the area type, sGameControl +0x4c: 0 Hub, 1..6 the villages (all Village
// quests are pooled: one category row for all), 7 the G-rank Hunters' Pub. The counter UI also reads it to lay out its
// menus (which ranks, which stars: a village has Low 1-6 and High 7-10 stars, the Hub Low 1-3, High 4-7 and G).
// In Village mode, all of these see a village while the area is the Hub or the Pub: the filter gets the village
// counter type (the list job runs on a worker thread, so the type is passed, not the area changed), and the others
// run with +0x4c reading 1 for their duration (the game thread; the counter UI only in its menus, see
// as_village_menu). Quests are then accepted, posted and departed on as at the Hub's counter; the game loads a Village
// quest anywhere (its data is in its own arc, quest\q<number>).
// The switch: the list job runs again (as at the area's loading; ~0.4 s the first time, then a few ms) and the UI goes
// back to its first state, which lays out the menu anew. Only in the rank menu (UI state 1) and star menu (2): in the
// quest list L / R page through a quest's details.
//   MHGU_VQ_TRACE=1   log the filter's verdicts (once per counter type and quest group)
#include "../game/addr.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../config.h"
#include "../hle.h"
#include "input.h"

// main v1.4.0, runtime addresses
#define FN_QUEST_FILTER   A_VILLAGEHUB_FN_QUEST_FILTER   // (list, quest number, counter type) -> listed?
#define FN_CATEGORY_BUILD A_VILLAGEHUB_FN_CATEGORY_BUILD   // sQuestSelector: categories from the list
#define FN_CATEGORY_FILL  A_VILLAGEHUB_FN_CATEGORY_FILL   // sQuestSelector: category contents
#define FN_CATEGORY_GET   A_VILLAGEHUB_FN_CATEGORY_GET   // (selector, slot) -> category
#define FN_UI_ROWS        A_VILLAGEHUB_FN_UI_ROWS   // uUIQuestCounter: the star rows of a rank
#define FN_UI_UPDATE      A_VILLAGEHUB_FN_UI_UPDATE   // uUIQuestCounter vtable slot 9
#define FN_UI_DRAW        A_VILLAGEHUB_FN_UI_DRAW   // uUIQuestCounter vtable slot 18
#define FN_LIST_LOAD      A_VILLAGEHUB_FN_LIST_LOAD   // sQuestSelector: start the list job
#define FN_LIST_DONE      A_VILLAGEHUB_FN_LIST_DONE   // sQuestSelector: the list job has finished
#define FN_CATEGORIES     A_VILLAGEHUB_FN_CATEGORIES   // sQuestSelector: build the categories anew (at a stage's start)
#define FN_QUEST_LOOKUP   A_VILLAGEHUB_FN_QUEST_LOOKUP   // (selector, quest number) -> its rQuestData (DLC, loaded, or in the list)
#define FN_QUEST_UNLOCKED A_VILLAGEHUB_FN_QUEST_UNLOCKED   // (quest number, 0xff, 0) -> unlocked in this save (the counters' check)
#define G_RESOURCES       A_VILLAGEHUB_G_RESOURCES  // the resource manager * (vtable +0x30: getResource(dti, path, mode))
#define DTI_QUESTDATA     A_VILLAGEHUB_DTI_QUESTDATA  // rQuestData
#define DTI_ARCHIVE       A_VILLAGEHUB_DTI_ARCHIVE  // rArchive
#define SEL_LIST          0x24u       // sQuestSelector: rQuestData *[LIST_MAX], +0x28 their number
#define SEL_COUNT         0x28u
#define LIST_MAX          0x360u
#define G_GAMECONTROL     A_G_GAMECONTROL  // sGameControl *: +0x4c area type
#define G_SELECTOR        A_G_SELECTOR  // sQuestSelector *
#define GC_AREA_TYPE      0x4cu
#define UI_STATE          0x3cu       // uUIQuestCounter: 1 rank menu, 2 star menu, 5 quest list
#define UI_RANK_MENU      1
#define UI_STAR_MENU      2
#define AREA_HUB          0
#define AREA_VILLAGE      1           // (any village: they share the Village quest categories)
#define AREA_PUB          7

static bool g_village;  // the counter shows Village quests
static volatile bool g_list_village;  // the list job takes Village quests (set at the switch; the job runs on a worker)
static volatile bool g_pending;  // a switch waits for its list job (MHXX: the job needs the game thread running, see ui_update)
static uint64_t g_pending_at;
static volatile uint32_t g_filter_calls, g_filter_last, g_preloaded, g_preload_failed;  // (the job's progress, for the log while a switch waits)
static int g_trace;

static bool hub_area(uint8_t a) { return a == AREA_HUB || a == AREA_PUB; }

// The setting is on, or was turned off while the counter shows Village quests: those stay whole until ui_update
// switches back, so a quest list or accept screen that is open isn't changed underneath it.
static bool village_active(void) { return g_village || config_live_village_in_hub(); }

static uint32_t area_addr(void) {
    uint32_t gc = LDG(G_GAMECONTROL);
    return gc ? gc + GC_AREA_TYPE : 0;
}

// Runs the original with the area type reading Village, in Village mode while the area is the Hub or the Pub.
static void as_village(Cpu *c, uint32_t fn) {
    uint32_t a = area_addr();
    uint8_t was = a ? LD8(a) : 0xff;
    bool flip = g_village && hub_area(was);
    if (flip) ST8(a, AREA_VILLAGE);
    fnhook_call_original(c, fn);
    if (flip) ST8(a, was);
}

// Calls a guest function from inside a hook (the hooked call's registers are kept).
static uint32_t call4(Cpu *c, uint32_t fn, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3) {
    uint32_t saved[16];
    memcpy(saved, c->r, sizeof saved);
    c->r[0] = a0, c->r[1] = a1, c->r[2] = a2, c->r[3] = a3;
    guest_call(c, fn);
    uint32_t r = c->r[0];
    memcpy(c->r, saved, sizeof saved);
    return r;
}
static uint32_t call1(Cpu *c, uint32_t fn, uint32_t a0) { return call4(c, fn, a0, 0, 0, 0); }

// Village quests (and village training / Prowler ones) this save hasn't unlocked. A village's counter lists all
// Village quests of its star ranges and only hides the locked ones in its menus (FN_QUEST_UNLOCKED); joining a posted
// quest from the Quest Board asks the list alone. A village is never online, so the game has no check for joining a
// locked Village quest - in the Hub and Pub they are kept out of the list (and out of the on-demand load below), and
// the board refuses them as it refuses a quest one doesn't meet the requirements for.
static bool locked_village_quest(Cpu *c, uint32_t q) {
    uint32_t group = q / 10000 % 100;
    if (q > 999999 || (group != 0 && group != 3 && group != 10)) return false;
    return !(call4(c, FN_QUEST_UNLOCKED, q, 0xff, 0, 0) & 0xff);
}

// MHXX: the list job asks for each quest's data to load in the background (getResource mode 2) and waits until all
// have; at the area's loading they do, but at the counter they never did (32 asked, then the job waited for good). So
// while a switch waits, each quest the filter takes is loaded here first, at once (mode 1, on the job's thread):
// the job's own request then finds it loaded. (The extra reference keeps it loaded, as the list does anyway.)
static uint32_t g_job_path;  // (guest memory: the list job's thread)
static void preload(Cpu *c, uint32_t q) {
    uint32_t mgr = LD32(G_RESOURCES);
    if (!mgr) return;
    snprintf((char *)G2H(g_job_path), 64, "quest\\questData\\questData_%07u", q);
    if (call4(c, LD32(LD32(mgr) + 0x30), mgr, DTI_QUESTDATA, g_job_path, 1)) g_preloaded++;
    else g_preload_failed++;
}

static void quest_filter(Cpu *c) {
    uint32_t q = c->r[1], type = c->r[2];
    g_filter_calls++, g_filter_last = q;
    if (!village_active()) { fnhook_call_original(c, FN_QUEST_FILTER); return; }
    if (g_list_village && hub_area((uint8_t)type)) c->r[2] = AREA_VILLAGE;
    fnhook_call_original(c, FN_QUEST_FILTER);
    if ((c->r[0] & 1) && hub_area((uint8_t)type) && locked_village_quest(c, q)) c->r[0] = 0;
    if ((c->r[0] & 1) && g_pending && hub_area((uint8_t)type)) preload(c, q);
    if (g_trace) {
        static uint8_t seen[8][10][2];
        uint32_t ok = c->r[0] & 1, group = q / 10000 % 10;
        if (type < 8 && !seen[type][group][ok]) {
            seen[type][group][ok] = 1;
            LOG("[vqhub] filter: counter type %u%s, quest %07u (group %u): %s\n", type,
                g_list_village && hub_area((uint8_t)type) ? " as a village" : "", q, group, ok ? "listed" : "no");
        }
    }
}

static void category_build(Cpu *c) { as_village(c, FN_CATEGORY_BUILD); }
static void category_fill(Cpu *c) { as_village(c, FN_CATEGORY_FILL); }
static void category_get(Cpu *c) { as_village(c, FN_CATEGORY_GET); }
static void ui_rows(Cpu *c) { as_village(c, FN_UI_ROWS); }

// The counter UI sees a village only while it lays out and shows the rank and star menus (states 0-2). In the quest
// list and while accepting (state 5) it stays the Hub's / Pub's, which posts the quest to the hub like any other.
static void as_village_menu(Cpu *c, uint32_t fn) {
    if (LD8(c->r[0] + UI_STATE) <= UI_STAR_MENU) as_village(c, fn);
    else fnhook_call_original(c, fn);
}
static void ui_draw(Cpu *c) { as_village_menu(c, FN_UI_DRAW); }

// A quest someone else posted in the hub may be from the other list (a Village quest while this counter shows the
// Hub's, or the other way round): joining looks its data up in the list (0x5ba50c(selector, quest number)) and says
// "You do not meet the requirements" when it isn't there. So a quest missing from the list is loaded as the list job
// loads each (the resource quest\questData\questData_<number>; GU: loc\quest\...) and added to the list, which has room for 864.
static uint32_t g_path;  // (guest memory for the resource path)
static void quest_lookup(Cpu *c) {
    uint32_t sel = c->r[0], q = c->r[1], args[2] = {c->r[0], c->r[1]};
    fnhook_call_original(c, FN_QUEST_LOOKUP);
    uint32_t a = area_addr();
    if (c->r[0] || !village_active() || !sel || !q || q > 9999999 || !a || !hub_area(LD8(a))) return;
    static uint32_t failed[16];  // (asked every frame for what the board shows: don't load what isn't there again)
    for (int i = 0; i < 16; i++) if (failed[i] == q) return;
    if (locked_village_quest(c, q)) {
        static uint32_t said;
        if (said != q) LOG("[vqhub] posted Village quest %07u: not unlocked in this save, can't be joined\n", q);
        said = q;
        return;
    }
    uint32_t list = LD32(sel + SEL_LIST), n = LD32(sel + SEL_COUNT), mgr = LD32(G_RESOURCES);
    if (!list || n >= LIST_MAX || !mgr) return;
    snprintf((char *)G2H(g_path), 64, "quest\\questData\\questData_%07u", q);
    uint32_t res = call4(c, LD32(LD32(mgr) + 0x30), mgr, DTI_QUESTDATA, g_path, 1);  // (getResource, loading now: MHXX mode 1; 2 loads in the background)
    if (!res) {
        static int next;
        failed[next++ % 16] = q;
        return;
    }
    ST32(list + 4 * n, res);
    ST32(sel + SEL_COUNT, n + 1);
    LOG("[vqhub] quest %07u added to the counter's list (posted in the hub)\n", q);
    c->r[0] = args[0], c->r[1] = args[1];
    fnhook_call_original(c, FN_QUEST_LOOKUP);
}

// The switch: the list job is started and the counter waits for it, a frame at a time (MHGU Recomp waited in place:
// GU's job finishes on its worker; MHXX's needs the game thread running meanwhile, and a wait in place never ended).
// Until then the counter UI isn't updated (no input, the menu as it was); then the categories are built anew, as when
// a stage starts, and the UI goes back to its first state, which lays the menu out for the new mode.
// MHXX keeps the Village quests' data in the archive quest\vq (nativeNX/arc/quest/vq.arc), which the villages load and
// the Hub doesn't (it has gq / gq2: 0x351cec picks them by area type). So the first switch to Village quests loads it
// here, at once, and keeps it (the reference is never given back: the quests stay loadable, as the list keeps them).
static uint32_t g_vq;
static void load_village_archive(Cpu *c) {
    uint32_t mgr = LD32(G_RESOURCES);
    if (g_vq || !mgr) return;
    static const char *const paths[] = {"arc\\quest\\vq", "quest\\vq"};  // (the first, checked live: ~10 ms)
    for (int i = 0; i < 2 && !g_vq; i++) {
        uint64_t t0 = plat_now_ns();
        snprintf((char *)G2H(g_path), 64, "%s", paths[i]);
        g_vq = call4(c, LD32(LD32(mgr) + 0x30), mgr, DTI_ARCHIVE, g_path, 1);
        LOG("[vqhub] the Village quests' archive %s: %s (%.0f ms)\n", paths[i], g_vq ? "loaded" : "not found",
            (plat_now_ns() - t0) / 1e6);
    }
}

static void list_start(Cpu *c, bool village) {
    uint32_t sel = LDG(G_SELECTOR);
    if (!sel) return;
    if (village) load_village_archive(c);
    g_list_village = village;
    g_filter_calls = 0, g_filter_last = 0, g_preloaded = 0, g_preload_failed = 0;
    call1(c, FN_LIST_LOAD, sel);
    g_pending = true;
    g_pending_at = plat_now_ns();
}

static bool list_finish(Cpu *c, uint32_t ui) {
    uint32_t sel = LDG(G_SELECTOR);
    if (!sel) { g_pending = false; return true; }
    uint64_t now = plat_now_ns();
    bool late = now - g_pending_at > 15000000000ull;
    static uint64_t said;
    if (now - said > 2000000000ull) {  // (every 2 s while waiting: the job's state (+0x40: 1 running, 2 done) and where it is)
        said = now;
        uint32_t job = LD32(sel + 0x1c);
        LOG("[vqhub] waiting for the list: job %#x state %u, %u quests filtered (last %07u), %u in the list, %u loaded"
            " here (%u failed)\n", job, job ? LD32(job + 0x40) : 0, g_filter_calls, g_filter_last, LD32(sel + SEL_COUNT),
            g_preloaded, g_preload_failed);
    }
    if (!call1(c, FN_LIST_DONE, sel) && !late) return false;
    g_pending = false;
    g_village = g_list_village;
    uint32_t a = area_addr();
    uint8_t was = a ? LD8(a) : 0xff;
    if (g_village && hub_area(was)) ST8(a, AREA_VILLAGE);
    call1(c, FN_CATEGORIES, sel);
    if (a) ST8(a, was);
    ST8(ui + UI_STATE, 0);  // (lays the menu out again)
    LOG("[vqhub] quest counter: %s quests (list loaded in %.0f ms, %u in it, %u loaded here%s)\n",
        g_village ? "Village" : "Hub", (plat_now_ns() - g_pending_at) / 1e6, LD32(sel + SEL_COUNT), g_preloaded,
        late ? ", not finished: given up" : "");
    return true;
}

static void ui_update(Cpu *c) {
    static uint64_t held;
    uint64_t b;
    int32_t sticks[4];
    input_get(&b, sticks);
    uint64_t pressed = b & ~held;
    held = b;
    uint32_t ui = c->r[0], a = area_addr();
    if (g_pending) {
        if (!list_finish(c, ui)) return;  // (still loading: the counter waits)
        as_village_menu(c, FN_UI_UPDATE);
        return;
    }
    uint8_t st = LD8(ui + UI_STATE);
    bool on = config_live_village_in_hub();
    if (a && hub_area(LD8(a)) && (st == UI_RANK_MENU || st == UI_STAR_MENU) &&
        (on ? (pressed & ((1ull << BTN_L) | (1ull << BTN_R))) != 0 : g_village)) {  // (off: back to the Hub's quests)
        list_start(c, !g_village);
        return;
    }
    as_village_menu(c, FN_UI_UPDATE);
}

// The hooks are set whatever the setting: each follows [Village Quests] VillageQuestsInHub as it is now (a Save in the
// settings menu turns it on or off; when off, the counter goes back to the Hub's quests at its rank or star menu).
void villagehub_init(void) {
    PORT_OFF(VILLAGEHUB, "Village quests in the Hub");
    g_trace = getenv("MHGU_VQ_TRACE") != NULL;
    g_path = hle_alloc(64);
    g_job_path = hle_alloc(64);
    fnhook_set(FN_QUEST_FILTER, hle_register("villagehub:quest_filter", quest_filter));
    fnhook_set(FN_QUEST_LOOKUP, hle_register("villagehub:quest_lookup", quest_lookup));
    fnhook_set(FN_CATEGORY_BUILD, hle_register("villagehub:category_build", category_build));
    fnhook_set(FN_CATEGORY_FILL, hle_register("villagehub:category_fill", category_fill));
    fnhook_set(FN_CATEGORY_GET, hle_register("villagehub:category_get", category_get));
    fnhook_set(FN_UI_ROWS, hle_register("villagehub:ui_rows", ui_rows));
    fnhook_set(FN_UI_UPDATE, hle_register("villagehub:ui_update", ui_update));
    fnhook_set(FN_UI_DRAW, hle_register("villagehub:ui_draw", ui_draw));
}
