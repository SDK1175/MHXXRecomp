// Main-module addresses the port's own features use, for the build in game/game.h (MHXX 1.5.1).
//
// Every value starts at 0: not found in MHXX yet. A feature runs only when all of its addresses are filled in
// (<FEATURE>_PORTED below); until then it stays off and the log says so. The value MHGU Recomp uses in GU 1.4.0
// is kept beside each one, with what it is: find the same function or global in MHXX 1.5.1's main (same engine,
// so usually the same code with other addresses), check the struct offsets the feature reads as well, and put
// the address here. All are runtime addresses (main at MAIN_BASE, 0x200000), as in the GU sources.
// tools/gen.py has the other half: the instructions replaced at generation time (MHXX_PATCHES).
#pragma once

// LDG(global): the pointer in a game global, or 0 while that global's address isn't known (callers already treat a
// null singleton as "not made yet"). PORT_OFF(FEATURE, "name"): leaves the function (once logged) while FEATURE_PORTED
// is false; PORT_OFF_RET also gives the value to return.
#define LDG(a) ((a) ? LD32(a) : 0u)
#define PORT_OFF_RET(feat, name, ret)                                                                         \
    do {                                                                                                      \
        if (!(feat##_PORTED)) {                                                                               \
            static int port_off_logged_;                                                                      \
            if (!port_off_logged_++) LOG("[port] %s: not ported to MHXX yet (runtime/game/addr.h): off\n", name); \
            return ret;                                                                                       \
        }                                                                                                     \
    } while (0)
#define PORT_OFF(feat, name) PORT_OFF_RET(feat, name, )

// ---- shared singletons
#define A_G_GAMECONTROL                      0x0u  // GU 1.4.0: 0x1a8ca48  sGameControl *: +0x30 our player slot
#define A_G_SELECTOR                         0x0u  // GU 1.4.0: 0x1a8ae38  sQuestSelector *: +0xd750 the quest's join count (hunters), +0xd74c its maximum
#define A_G_SENEMY                           0x0u  // GU 1.4.0: 0x1a51fe0  sEnemy *: +0x1c the 10 live enemies
#define A_G_SNETWORK                         0x0u  // GU 1.4.0: 0x1a843f0
#define A_G_SPLAYER                          0x0u  // GU 1.4.0: 0x1a85b58
#define A_G_SQUEST                           0x0u  // GU 1.4.0: 0x1a8ad90  sQuest *: +0xa4 cQuestData * of the quest being played (+0x8 quest number)
#define A_G_SYSTEM                           0x0u  // GU 1.4.0: 0x2326764  engine system object *: +0x68 the frame step in 60 fps frames

// ---- runtime/hle/buffcycle.c
#define BUFFCYCLE_PORTED (A_G_SYSTEM)

// ---- runtime/hle/camera_jitter.c
#define A_CAMERA_JITTER_FN_LOOKAT            0x0u  // GU 1.4.0: 0x21c33c  look-at: r0 = out view matrix, r1 = camera
#define A_CAMERA_JITTER_FN_PROJECTION        0x0u  // GU 1.4.0: 0x21c358  projection: r0 = out matrix, r1 = camera
#define A_CAMERA_JITTER_FN_EVENT_LOOKAT      0x0u  // GU 1.4.0: 0xb57a10  the same for event cameras (cutscenes: the meal), their class's own
#define A_CAMERA_JITTER_FN_EVENT_PROJECTION  0x0u  // GU 1.4.0: 0xb57fa4
#define A_CAMERA_JITTER_FN_SCREENSHOT_BUFFERS 0x0u  // GU 1.4.0: 0xa6f854  sScreenShot's buffers made: r0 = sScreenShot, r1 = how many, r2 / r3 size
#define CAMERA_JITTER_PORTED (A_CAMERA_JITTER_FN_LOOKAT && A_CAMERA_JITTER_FN_PROJECTION && A_CAMERA_JITTER_FN_EVENT_LOOKAT && A_CAMERA_JITTER_FN_EVENT_PROJECTION && A_CAMERA_JITTER_FN_SCREENSHOT_BUFFERS)

// ---- runtime/hle/chat.c
#define A_CHAT_FN_SEND_CHAT                  0x0u  // GU 1.4.0: 0x41e204  (net object, type, name, text[0x78], int on the stack (1)): builds and sends it
#define A_CHAT_FN_NET_INDEX                  0x0u  // GU 1.4.0: 0x427ae4  (sNetwork) -> our player index in the session
#define A_CHAT_FN_NET_ONLINE                 0x0u  // GU 1.4.0: 0x427ba8  (sNetwork) -> in a session (else the index is 0)
#define A_CHAT_FN_NET_OBJECT                 0x0u  // GU 1.4.0: 0x427b3c  (sNetwork, 0x6b) -> the object chat messages are sent through
#define A_CHAT_FN_MY_PLAYER                  0x0u  // GU 1.4.0: 0x476278  (sPlayer) -> our player; +0x13c4 -> its data, +0x514 there: the hunter's name
#define CHAT_PORTED (A_CHAT_FN_SEND_CHAT && A_CHAT_FN_NET_INDEX && A_CHAT_FN_NET_ONLINE && A_CHAT_FN_NET_OBJECT && A_CHAT_FN_MY_PLAYER && A_G_SNETWORK && A_G_SPLAYER)

// ---- runtime/hle/combowin.c
#define COMBOWIN_PORTED (A_G_SYSTEM)

// ---- runtime/hle/dummy.c
#define A_DUMMY_FN_COLLISION                 0x0u  // GU 1.4.0: 0x36b15c  the collision manager's update: attacks against targets, every update
#define A_DUMMY_FN_EN_DAMAGE                 0x0u  // GU 1.4.0: 0x29d6f8  (enemy, part, damage, kind): takes the damage from its HP
#define A_DUMMY_FN_EN_SPEED                  0x0u  // GU 1.4.0: 0x2b0870  (enemy, speed s0): its motion's speed
#define DUMMY_PORTED (A_DUMMY_FN_COLLISION && A_DUMMY_FN_EN_DAMAGE && A_DUMMY_FN_EN_SPEED && A_G_GAMECONTROL && A_G_SENEMY && A_G_SPLAYER)

// ---- runtime/hle/emjump.c
#define EMJUMP_PORTED (A_G_SYSTEM)

// ---- runtime/hle/emtrace.c
#define A_EMTRACE_FN_FRAME                   0x0u  // GU 1.4.0: 0xda5824  engine frame start: sleeps to the target, sets the step (+0x68)
#define A_EMTRACE_FN_SET_ACTION              0x0u  // GU 1.4.0: 0x276154  (monster, group, index, mode): starts a monster action
#define A_EMTRACE_FN_HATE_RATE               0x0u  // GU 1.4.0: 0x2bb828
#define EMTRACE_PORTED (A_EMTRACE_FN_FRAME && A_EMTRACE_FN_SET_ACTION && A_EMTRACE_FN_HATE_RATE && A_G_SENEMY && A_G_SPLAYER && A_G_SYSTEM)

// ---- runtime/hle/hatestep.c
#define A_HATESTEP_FN_HATE                   0x0u  // GU 1.4.0: 0x2bbf98  (monster): the update of its attention scores
#define HATESTEP_PORTED (A_HATESTEP_FN_HATE)

// ---- runtime/hle/hpscale.c
#define A_HPSCALE_FN_NANDO_ROW               0x0u  // GU 1.4.0: 0x2baaac  (monster, column) -> the monster's row of the difficulty table
#define HPSCALE_PORTED (A_HPSCALE_FN_NANDO_ROW && A_G_GAMECONTROL && A_G_SELECTOR && A_G_SENEMY && A_G_SPLAYER && A_G_SQUEST)

// ---- runtime/hle/inflate.c
#define A_INFLATE_FN_INFLATE_FAST            0x0u  // GU 1.4.0: 0x1422ce8  (z_stream *strm, unsigned start: avail_out when inflate() was entered)
#define A_INFLATE_FN_ADLER32                 0x0u  // GU 1.4.0: 0x1422774  (adler, buf, len) -> adler
#define A_INFLATE_MSG_LITLEN                 0x0u  // GU 1.4.0: 0x17860b5  "invalid literal/length code"
#define A_INFLATE_MSG_DISTCODE               0x0u  // GU 1.4.0: 0x17860d1  "invalid distance code"
#define A_INFLATE_MSG_DISTFAR                0x0u  // GU 1.4.0: 0x17860e7  "invalid distance too far back"
#define INFLATE_PORTED (A_INFLATE_FN_INFLATE_FAST && A_INFLATE_FN_ADLER32 && A_INFLATE_MSG_LITLEN && A_INFLATE_MSG_DISTCODE && A_INFLATE_MSG_DISTFAR)

// ---- runtime/hle/itembar.c
#define A_ITEMBAR_FN_ITEM_BAR                0x0u  // GU 1.4.0: 0x4f7f38  the player's item bar handler (r0 = the player)
#define A_ITEMBAR_FN_PL_IS_LOCAL             0x0u  // GU 1.4.0: 0x47bcd0  (player) 1: the handler goes on
#define A_ITEMBAR_FN_PL_BUSY                 0x0u  // GU 1.4.0: 0x47b918  (player) 0: the handler goes on
#define A_ITEMBAR_FN_ITEM_NEXT               0x0u  // GU 1.4.0: 0x4880fc  (player, slot, 0 right / 1 left): the next slot holding an item
#define ITEMBAR_PORTED (A_ITEMBAR_FN_ITEM_BAR && A_ITEMBAR_FN_PL_IS_LOCAL && A_ITEMBAR_FN_PL_BUSY && A_ITEMBAR_FN_ITEM_NEXT)

// ---- runtime/hle/kotrace.c
#define A_KOTRACE_FN_KO_HIT                  0x0u  // GU 1.4.0: 0x29db54  (monster, ?, part, KO value): a hit's KO into +0x4d2
#define A_KOTRACE_FN_KO_APPLY                0x0u  // GU 1.4.0: 0x2a2e18  (monster, amount): into the buildup; 1 when the KO starts
#define A_KOTRACE_FN_KO_START                0x0u  // GU 1.4.0: 0x2a2f0c  (monster): stunned for the entry's duration
#define A_KOTRACE_FN_KO_RAISE                0x0u  // GU 1.4.0: 0x2a2f80  (monster): the threshold for the next KO
#define A_KOTRACE_FN_DECAY                   0x0u  // GU 1.4.0: 0x2a398c  (monster): every status's decay, each update
#define KOTRACE_PORTED (A_KOTRACE_FN_KO_HIT && A_KOTRACE_FN_KO_APPLY && A_KOTRACE_FN_KO_START && A_KOTRACE_FN_KO_RAISE && A_KOTRACE_FN_DECAY && A_G_SYSTEM)

// ---- runtime/hle/mashrate.c
#define A_MASHRATE_FN_MASH                   0x0u  // GU 1.4.0: 0x47f590  (player) -> 1: mashed this update
#define A_MASHRATE_MASH_BITS                 0x0u  // GU 1.4.0: 0xff05f0  (the stick's directions, the buttons 0x47f590 counts)
#define MASHRATE_PORTED (A_MASHRATE_FN_MASH && A_MASHRATE_MASH_BITS)

// ---- runtime/hle/mousecam.c
#define A_MOUSECAM_FN_CAMERA_STICK           0x0u  // GU 1.4.0: 0x2255cc  uQuestCamera's right stick handler: r0 = the camera, r1 = the invert bits
#define MOUSECAM_PORTED (A_MOUSECAM_FN_CAMERA_STICK)

// ---- runtime/hle/movelog.c
#define A_MOVELOG_FN_SET_CMDTBL              0x0u  // GU 1.4.0: 0x4e9b7c  (player, id, start frame)
#define A_MOVELOG_FN_SET_CMDTBL2             0x0u  // GU 1.4.0: 0x4e9c50  (player, id, start frame, mode)
#define A_MOVELOG_FN_SET_ACTION              0x0u  // GU 1.4.0: 0x4831e0  (player, act_st, act_no, act_param)
#define A_MOVELOG_FN_DO_ACTION               0x0u  // GU 1.4.0: 0x4ab670  (player, act_st, act_no, act_param)
#define A_MOVELOG_FN_PL_INPUT                0x0u  // GU 1.4.0: 0x47ca28  (player): its input update, every update
#define MOVELOG_PORTED (A_MOVELOG_FN_SET_CMDTBL && A_MOVELOG_FN_SET_CMDTBL2 && A_MOVELOG_FN_SET_ACTION && A_MOVELOG_FN_DO_ACTION && A_MOVELOG_FN_PL_INPUT && A_G_SYSTEM)

// ---- runtime/hle/presence.c
#define A_PRESENCE_MON_PAIRS                 0x0u  // GU 1.4.0: 0x1797ea4
#define A_PRESENCE_MON_UI                    0x0u  // GU 1.4.0: 0x182e7f8
#define PRESENCE_PORTED (A_PRESENCE_MON_PAIRS && A_PRESENCE_MON_UI && A_G_GAMECONTROL && A_G_SELECTOR && A_G_SPLAYER && A_G_SQUEST)

// ---- runtime/hle/quickhunt.c
#define A_QUICKHUNT_FN_TOWN_IDLE             0x0u  // GU 1.4.0: 0x8bd0fc  town area update, mode 1 (r0: the area)
#define QUICKHUNT_PORTED (A_QUICKHUNT_FN_TOWN_IDLE && A_G_SELECTOR)

// ---- runtime/hle/urgentcredit.c
#define A_URGENTCREDIT_FN_URGENT             0x0u  // GU 1.4.0: 0x5ae75c  (sQuest) -> an Urgent quest
#define A_URGENTCREDIT_FN_UNLOCKED           0x0u  // GU 1.4.0: 0x74af8c  (quest number, 0xff, 0) -> unlocked in this save
#define URGENTCREDIT_PORTED (A_URGENTCREDIT_FN_URGENT && A_URGENTCREDIT_FN_UNLOCKED)

// ---- runtime/hle/vi.c
#define VI_PORTED (A_G_SYSTEM)

// ---- runtime/hle/villagehub.c
#define A_VILLAGEHUB_FN_QUEST_FILTER         0x0u  // GU 1.4.0: 0x5b8090  (list, quest number, counter type) -> listed?
#define A_VILLAGEHUB_FN_CATEGORY_BUILD       0x0u  // GU 1.4.0: 0x5b85d8  sQuestSelector: categories from the list
#define A_VILLAGEHUB_FN_CATEGORY_FILL        0x0u  // GU 1.4.0: 0x5bb318  sQuestSelector: category contents
#define A_VILLAGEHUB_FN_CATEGORY_GET         0x0u  // GU 1.4.0: 0x5bc96c  (selector, slot) -> category
#define A_VILLAGEHUB_FN_UI_ROWS              0x0u  // GU 1.4.0: 0x983074  uUIQuestCounter: the star rows of a rank
#define A_VILLAGEHUB_FN_UI_UPDATE            0x0u  // GU 1.4.0: 0x981e34  uUIQuestCounter vtable slot 9
#define A_VILLAGEHUB_FN_UI_DRAW              0x0u  // GU 1.4.0: 0x981bb0  uUIQuestCounter vtable slot 18
#define A_VILLAGEHUB_FN_LIST_LOAD            0x0u  // GU 1.4.0: 0x5b7fd8  sQuestSelector: start the list job
#define A_VILLAGEHUB_FN_LIST_DONE            0x0u  // GU 1.4.0: 0x5b8070  sQuestSelector: the list job has finished
#define A_VILLAGEHUB_FN_CATEGORIES           0x0u  // GU 1.4.0: 0x5ba8fc  sQuestSelector: build the categories anew (at a stage's start)
#define A_VILLAGEHUB_FN_QUEST_LOOKUP         0x0u  // GU 1.4.0: 0x5ba50c  (selector, quest number) -> its rQuestData (DLC, loaded, or in the list)
#define A_VILLAGEHUB_FN_QUEST_UNLOCKED       0x0u  // GU 1.4.0: 0x74af8c  (quest number, 0xff, 0) -> unlocked in this save (the counters' check)
#define A_VILLAGEHUB_G_RESOURCES             0x0u  // GU 1.4.0: 0x2326a64  the resource manager * (vtable +0x30: getResource(dti, path, mode))
#define A_VILLAGEHUB_DTI_QUESTDATA           0x0u  // GU 1.4.0: 0x1a8ab98  rQuestData
#define VILLAGEHUB_PORTED (A_VILLAGEHUB_FN_QUEST_FILTER && A_VILLAGEHUB_FN_CATEGORY_BUILD && A_VILLAGEHUB_FN_CATEGORY_FILL && A_VILLAGEHUB_FN_CATEGORY_GET && A_VILLAGEHUB_FN_UI_ROWS && A_VILLAGEHUB_FN_UI_UPDATE && A_VILLAGEHUB_FN_UI_DRAW && A_VILLAGEHUB_FN_LIST_LOAD && A_VILLAGEHUB_FN_LIST_DONE && A_VILLAGEHUB_FN_CATEGORIES && A_VILLAGEHUB_FN_QUEST_LOOKUP && A_VILLAGEHUB_FN_QUEST_UNLOCKED && A_VILLAGEHUB_G_RESOURCES && A_VILLAGEHUB_DTI_QUESTDATA && A_G_GAMECONTROL && A_G_SELECTOR)

// ---- runtime/hle/abort.c
#define A_ABORT_FATAL_EXIT_RET               0x0u  // GU 1.4.0: 0xa3e1c8
#define ABORT_PORTED (A_ABORT_FATAL_EXIT_RET)

// ---- runtime/net/emsync.c
#define EMSYNC_PORTED (A_G_GAMECONTROL && A_G_SENEMY && A_G_SPLAYER)

// ---- runtime/forge.c
#define A_FORGE_OFF_APP_RUN                  0x0u  // GU 1.4.0: 0xb8692c  sApp::run
#define A_FORGE_OFF_UPDATE                   0x0u  // GU 1.4.0: 0x3da8fc  the frame update (this: dt at +0x68)
#define A_FORGE_OFF_SYSTEM_CTOR              0x0u  // GU 1.4.0: 0x887810  cSystem::cSystem / ~cSystem: every singleton
#define A_FORGE_OFF_SYSTEM_DTOR              0x0u  // GU 1.4.0: 0x88784c
#define A_FORGE_OFF_MOUNT_SAVE               0x0u  // GU 1.4.0: 0x878da0  sSavedata's mount
#define A_FORGE_OFF_JOB_SAFE                 0x0u  // GU 1.4.0: 0x212041c
#define FORGE_PORTED (A_FORGE_OFF_APP_RUN && A_FORGE_OFF_UPDATE && A_FORGE_OFF_SYSTEM_CTOR && A_FORGE_OFF_SYSTEM_DTOR && A_FORGE_OFF_MOUNT_SAVE && A_FORGE_OFF_JOB_SAFE)

// ---- runtime/services/bonus.c: the save file layout (not addresses), checked by hand for MHXX
#define BONUS_PORTED 0
