# Download (event / challenge) quests and the Korean patch

Status: open. Notes so far (2026-11-14).

## What the reports are
Players say DL quests show "broken" or untranslated text with the Korean patch.

## Known
- The Switch Korean patch (MHXX_NX_1.5_kor_v2) replaces all 223 event-range quest arcs in the RomFS
  (`nativeNX/arc/quest/q1010001.arc` .. `q1120013.arc`). They match Team Galmuri's 3DS DL quest translation
  (`event.que` / `challenge.que`, 0x1400-byte slots `[u32 id][u32 size][ARC]`): 136 of 140 event quests the same size.
- The game opens only `system` and `system_backup` in the save folder (MHGU_LOG_FILES): no file of their own for
  downloads. `system` is read as +0 0x10, +0x10 0x80000, +0x80010 0x401d9c.
- MHGU (runtime/services/bonus.c, upstream): downloads live in the shared part of `system`: flags
  [0xb2a5, 0xb310) (empty: the Event list says "no quest data"), the area [0xc000, first character slot) with
  160 event quest slots from 0xf899 and 40 challenge slots after, 0x1c00 apart, `[u32 id][u32 size][ARC]`.
  The quests' files are in the RomFS; the server (BCAT `eventQuestList.bin`) only said which were out.
  So the save keeps its own copy of each downloaded quest's ARC: a copy made without the patch is Japanese,
  and the patch's RomFS arcs aren't used for it. This is the likely cause.
- MHXX's save is the 3DS one with a 36-byte header (MHXX Switch save editors). The first character slot is at
  0x126498 (MHGU's layout doesn't fit: its challenge slots end past that). A well-played 3DS MHXX save has other
  records in [0xc000, 0x126498) (0x25886.., 0x2fad5.., 0xccfbd.., 0x11ea4e..), so MHXX's download slots are
  elsewhere or smaller (0x1400, as the 3DS files).
- XX code: `quest\q%07d` used at main+0x151ce0; `eventQuestList.bin` at main+0x3eb1c (BCAT download into a
  0x40000 buffer via main+0x2e1e8); `quest\questData\questData_%07d_%s` at main+0x37648.

## Plan
- Fix (inside our line: no unlocking): when the game reads `system`, put the RomFS (mod) arc of the same quest id
  in each download slot that has one, so a save's Japanese copies show the patch's text. Nothing is added.
- Needs the slot layout: a Switch save with downloads (issue template "DL(이벤트) 퀘스트 한글 깨짐"), or following
  the code from main+0x151ce0 / the BCAT handler to where the ARC is copied into the save.
