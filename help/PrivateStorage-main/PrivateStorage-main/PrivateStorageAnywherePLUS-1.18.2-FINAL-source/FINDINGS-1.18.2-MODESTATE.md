# 1.18.2 mode-state findings (supersedes the packet-producer investigation)

Date: 2026-08-18. Verified by static analysis of the live game EXE
`C:\Steam\Steam\steamapps\common\Crimson Desert\bin64\CrimsonDesert.exe`
(SHA-256 `3416FDBF03D9C871BE7C35055520E98513388ABAADA33625793BA244D4C963C7`)
and of `original/PrivateStorageAnywhere.asi`
(SHA-256 `4F514298B2BC5DB7E804B0166AD2269BAE97A414F7DAE425A0F736CDA7F56F3E`).

Game addresses are RVAs on base `0x140000000`. ASI addresses are RVAs on base
`0x180000000`.

## 1. Root cause

The mod opens storage by driving the game's own mode state machine into the
`store` sub-mode; the panel-manager then mounts the warehouse view, and the
mounted view's UI script emits the command packets. (Author's v1.5.5 changelog
describes exactly this.)

`tools/patch_private_storage_118.py` concluded 1.18 had *removed* the
mode/sub-mode fields and therefore:

- overwrote the layout resolver at ASI `0x1460` with a stub publishing
  `0xCAB` for all four offsets,
- rewrote the safe-state gate at ASI `0xB4D6`/`0xB4DF` to `+0xCAB == 0`,
- NOP'd ASI `0xB52C`, `0xB5B7`, `0xB759` — the `SafeSetupModeForWarehouse` /
  `SafeRestoreMode` calls.

Those three calls are the open mechanism. With them gone the `store` tag never
enters the tag list, no view is mounted, and the handler runs against an
unmounted controller — the observed `childCount=0x0` and empty packets.

**The fields were not removed. They moved.** The mod's resolver only scans for
struct offsets in the hardcoded range `0xC00`–`0xCFF`
(`tools/compare_modeswitch.py:30`); the 1.18.2 offsets are single-byte
displacements and fall outside it. Running that tool against the live EXE
returns `layout=None` with zero `0xCxx` immediates in the anchor function.

## 2. Game side (1.18.2)

### Mode-tag list builder — `game+0x529410`

`BuildModeTagList(outList, mode /*dl*/, submode /*r8b*/, vec /*r9*/)`.
Appends `"global"`, switches on `mode` via jump table `game+0x529B94` (7 entries),
then on `submode` via jump table `game+0x529BB0` (17 entries).

Tag strings live at `game+0x4CB5068`.. (`hud-info`, `ingame-global`, `global`,
`subtitle`, `quickslot`, `interaction`, `hud-play`, `gimmick`, `cinema`, `store`,
`fadeout`, …).

Relevant resolved cases:

| mode | tags | submode | tags |
|---|---|---|---|
| 4 | `ingame-global` | 4 | `alert, cinema, subtitle` |
| | | **5** | **`dialog, store`** |
| | | 15 | `hud-info, hud-play` |
| | | 16 | `hud-info, hud-play, interaction, quickslot, subtitle, alert` |

This reproduces the mod's semantics exactly: `mode == 4` in-game,
`submode ∈ {15,16}` safe gameplay, `submode == 5` store. It also explains the
disproven notifier build — sub-mode 4 is `cinema`, matching
`evidence/cutscene-controls-after-hotkey.png`.

### Mode state object layout

From `ModeSwitch` at `game+0x529C00` (the per-frame resolver):

```
mode         = modeObj + 0x18      ; current, committed at game+0x52A109
submode      = modeObj + 0x19      ; current, committed at game+0x52A10D
flags[7]     = modeObj + 0x21      ; mode request flags,    scanned game+0x529C52
subtypes[17] = modeObj + 0x28      ; submode request flags, scanned game+0x529C73
dirty        = modeObj + 0x4B      ; forces re-resolve,     tested game+0x529C9E
```

`ModeSwitch` picks the **first non-zero index** of each array. Note
`subtypes == flags + 7` — the exact relationship the mod's resolver expects.

`modeObj + 0x39` holds a 17-byte shadow copy of the subtypes array, written
byte-by-byte at `game+0x529D89`.. It is *not* mode/submode, despite looking like
an adjacent-store pair — see the note in `tools/derive_modestate.py`. The
authoritative reads are `game+0x52A0DA` / `game+0x52A0E7`, which pass
`[rbx+0x18]` and `[rbx+0x19]` directly to `BuildModeTagList` as the mode and
sub-mode arguments.

`game+0x52A11D` diffs the old and new tag lists; that diff is what mounts and
unmounts views.

### The dirty flag is mandatory

```
game+0x529C98  cmp byte [rbx+0x18], r12b   ; mode unchanged?
game+0x529C9C  jne do_transition
game+0x529C9E  cmp byte [rbx+0x4b], 0      ; dirty?
game+0x529CA2  je  skip
```

Opening storage leaves `mode == 4` unchanged, so without `modeObj[0x4B] = 1`
the transition is skipped entirely. The game's own code follows this
convention — see `game+0x7DD74A` / `game+0x7DD798`, which set a subtype flag
and then `[rdx+0x4B] = 1`.

### Reaching the object

```
modeObj = *(menuMgr + 0x1158)
```

Confirmed by `game+0x7DD6B0`, which uses `[rbx+0x105E]`, `[rbx+0x105F]` **and**
`[rbx+0x1158]` off the same register — so the object the mod already calls
`menuMgr` (menu-layer state bytes at `+0x105E`/`+0x105F`) is the object that
holds the mode-state pointer at `+0x1158`.

Note `game+0x7DD7EB`: `menuMgr+0x1190 -> +0x8E8 -> call game+0x3A91A0`. That is
the notifier candidate A called. It is the *last* step of a menu-layer
transition and sets the cinema subtype on the way — which is why calling it in
isolation produced cutscene controls.

## 3. ASI side (original v1.5.10)

```
0x1460..~0x172A  legacy layout resolver (the 0xC00-0xCFF scanner) — dead on 1.18.2
0xAA30..0xAAC0   GetMenuMgr(): *(0x3D518) -> +0x90 -> menuMgr; 0xAA8C backlink check
0xAEE0..0xAFE8   SafeRestoreMode(rcx = base)
0xAFF0..0xB137   SafeSetupModeForWarehouse(rcx = base)
0xB430..0xB874   hotkey open/close path: gate, Safe* calls, open/close logging
0xC0B0..0xC58D   cleanup path; calls SafeRestoreMode at 0xC514
```

Globals:

```
0x3C01C  MODE_OFF        0x3C020  SUB_OFF
0x3C024  FLAGS_OFF       0x3C028  SUBTYPES_OFF
0x3C02C  StoreSubIndex (auto-derives to 5 on 1.18.2 — still correct)
0x3D4B0  MODE_ANCHOR     0x3D518  game global slot     0x3D520  g_mainChar
0x4061C..0x40622  saved flags[7]
0x40628..0x40636  saved subtypes[15]
0x40638  Safe* spinlock
```

Base pointer: the gate at `0xB447` atomically loads `g_mainChar` (`0x3D520`)
into `rsi`, and every mode read plus both `Safe*` calls use it. The cleanup
path does the same at `0xC4FE`. **These two loads are the only places the mode
base is established.**

Safe-state gate at `0xB4C2`:
```
mov eax,[MODE_OFF]; movzx edx,[rax+rsi]
mov eax,[SUB_OFF];  movzx ecx,[rax+rsi]
cmp dl,4 / jne unsafe            ; mode == 4
lea eax,[rcx-0xF] / cmp al,1 / ja unsafe   ; submode in {15,16}
```
Correct as written for 1.18.2 — it needs the right offsets and base, nothing more.

`SafeSetupModeForWarehouse` body (`0xB05E`..`0xB0F4`) saves flags[0..6] and
subtypes[0..14], zeroes them, then sets `flags[4] = 1` and
`subtypes[StoreSubIndex] = 1`. That is precisely mode 4 + submode 5.
Subtypes 15/16 are deliberately left alone, so restore returns to gameplay.

**Blocker:** both `Safe*` functions guard their offsets with
`(off - 0xC00) <= 0x200` (`0xB006`/`0xB01E`, `0xAEF6`/`0xAF0E`). With the real
1.18.2 offsets (0x21/0x28) that check fails and both functions log SKIPPED. The
`SUBTYPES_OFF == FLAGS_OFF + 7` check passes unchanged.

## 4. Required changes

1. Resolver at ASI `0x1460`: publish `MODE_OFF=0x18`, `SUB_OFF=0x19`,
   `FLAGS_OFF=0x21`, `SUBTYPES_OFF=0x28`; return success.
2. Base: patch the two loads at ASI `0xB447` and `0xC4FE` to yield
   `modeObj = *(*( *(0x3D518) + 0x90) + 0x1158)` instead of `g_mainChar`.
3. Range guards in both `Safe*` functions: accept small offsets.
4. Add `modeObj[0x4B] = 1` on both setup and restore.
5. Do **not** NOP `0xB52C`, `0xB5B7`, `0xB759`.
6. Keep the safe-state gate at `0xB4D6`/`0xB4DF` as the original
   `mode == 4 && submode ∈ {15,16}`.
7. Re-apply the 1.18.2 menu-manager backlink fix at ASI `0xAA8C`
   (`4D 3B C1 75 09` -> five NOPs).

## 5. Independent re-derivation

`tools/derive_modestate.py` recovers the whole layout from the executable with
no hardcoded addresses, anchored only on the UI tag strings. Run against
1.18.2 it reports:

```text
BuildModeTagList: game+0x529410..0x529BF4
ModeSwitch:       game+0x529C00..0x52A1C8
  mode        modeObj + 0x18
  submode     modeObj + 0x19
  flags[]     modeObj + 0x21
  subtypes[]  modeObj + 0x28
  dirty       modeObj + 0x4B
  in-game mode value   = 4
  store sub-mode value = 5
  sanity: subtypes == flags + 7  -> OK
```

Every constant used by `tools/patch_private_storage_1182_modestate.py` matches.
This tool is also the prototype for replacing the mod's own resolver, so a
future Pearl Abyss patch that moves these fields again can be re-derived
instead of hand-traced.

## 6. Build W

```text
script  tools/patch_private_storage_1182_modestate.py
input   original/PrivateStorageAnywhere.asi
        4F514298B2BC5DB7E804B0166AD2269BAE97A414F7DAE425A0F736CDA7F56F3E
output  patched/PrivateStorageAnywhere-1.18.2W-modestate-candidate.asi
        2D1B88C1B7B7BDA79B30FC393728A9565895A26ED3AA4D7C67D41203966EF8C8
zip     patched/PrivateStorageAnywhere-v1.5.10-CD-1.18.2W-modestate-DMM.zip
        90CAE9F636AF3C0082F9E0B2C858DD123DE4EBE25216F400C1C3A2CF6157B2D6
```

253 bytes changed across 23 runs; PE size, section table, entry point and the
589-entry exception directory are all unchanged. Not yet tested in game.

## 7. Build W test result and the menu-layer conflict

Build W was tested in game. It **froze after F4**, but the log proves the mode
machinery is fixed:

```text
build U:   Warehouse opened (mode=0x00 sub=0x00)
build W:   Warehouse opened (mode=0x04 sub=0x10)
```

Mode 4 / sub-mode 16 are the correct live gameplay values, which requires both
the `*(0x3D518) -> +0x90 -> +0x1158` walk and the `+0x18`/`+0x19` offsets to be
right. Neither `SafeSetupModeForWarehouse SKIPPED` nor `EXCEPTION` appears, so
the relaxed range guard passed and the routine wrote `flags[4]=1`,
`subtypes[5]=1`, `dirty=1`.

### Cause of the freeze

Build U ran the identical panel-init sequence without crashing, because nothing
ever mounted. W actually triggers a mount, so the game's UI script and the mod
now write the same Warehouse2 controller concurrently. `childCount=0x0` in the
final log line confirms the mount had not landed when the mod did its work — it
arrives afterwards.

### The menu layer sets the cinema sub-mode

The mod's `ViewMount` (ASI `0xB880`) writes `menuMgr+0x105E..0x105F = 0x0101`,
i.e. current=1, **request=1**. The game's `game+0x7DD6B0` reacts:

```text
game+0x7DD72E  cmp byte [menuMgr+0x105F], 3
game+0x7DD735  jne -> game+0x7DD7AE   mov byte [modeObj+0x2C], 1
```

`modeObj+0x2C` is `subtypes[0x28+4]` = sub-mode 4 = `alert, cinema, subtitle`.
`ModeSwitch` picks the lowest set index, so cinema (4) beats store (5). Request
`1` means "open the menu layer" — an alternative UI path to the store sub-mode,
not a complement to it. The `== 3` branch is the one that *clears* cinema, and
3 is the idle value.

This is the 1.18.2 restatement of the author's v1.5.5 bug and explains the
cutscene controls from the notifier build.

## 8. Build X (W2)

Changes on top of W, all in the same script:

```text
0xB8DE  9  NOP  ViewMount   menuMgr+0x105E = 0x0101
0xB950  7  NOP  ViewUnmount menuMgr+0x105F = 3
0xB965  9  NOP  ViewUnmount menuMgr+0x105E = 0x0304
0x47C8  4  NOP  clear modal pointer
0x487E  3  NOP  call handler -- empty 0x15 packet
0x4911  3  NOP  call handler -- 0x0E command packet
0x5335  6  NOP  SetInventory
0x53B2  6  NOP  SetInventory
0x53D2  6  NOP  SetInventory
0x42E7 12  ->   call telemetry stub at 0x1500
0x1500 98  new  leaf stub: re-read mode/submode, tail-jmp to the logger
```

The telemetry stub is deliberately a leaf that tail-jumps rather than calls: it
sits inside the dead resolver's `.pdata` range, whose unwind info describes a
different prologue, and a leaf unwinds correctly regardless.

```text
output  patched/PrivateStorageAnywhere-1.18.2X-w2-candidate.asi
        34C5A9A05E0EC41B0B271B22932D1D2DBAF70F7E7527D62DEA2C44B0D5377073
zip     patched/PrivateStorageAnywhere-v1.5.10-CD-1.18.2X-w2-DMM.zip
        9BEA0C468518763EEDE69ACC3B036C07B889D175E19913257CDEBD553CC0953A
```

Spinlock releases at ASI `0xAFC8` and `0xB117` re-verified intact. Not yet
tested in game.

## 9. Build X test result — the re-entrant init

X crashed on the first F4, but down a **different route** than W, and the route
is the finding. Its log contains two lines W's does not:

```text
AUTO-CAPTURED warehouse controller: 0x2AB52439C00 (panelId=0x0000)
  CanShow: running InitWarehousePanel inline (first-open fix)
```

The game called `CanShow` while mounting the warehouse view, the mod's hook
fired, and the mod ran the whole of `InitWarehousePanel` **re-entrantly inside
the game's own mount call**. Cutting individual writes cannot fix that; the
routine has to stay out of the mount entirely.

Which route runs depends on whether the controller was already captured:

```text
ASI 0x4319  fn 0x4270  deferred route      (W took this one)
ASI 0xBE3F  fn 0xBC20  handler route
ASI 0xC375  fn 0xC0B0  CanShow first-open  (X took this one)
```

All three discard the return value, so the function can be neutered safely.

Two things X did confirm:

- **The menu-layer suppression works.** In W the second `ViewMount` reported
  `layer busy (+0x105E=1)`; in X both report `state set`, which is only
  possible if the write is genuinely gone.
- `childCount` went from `0x0` in U and W to a large non-zero value, and
  `modalView` from null to a real pointer — something is now being built.

X's telemetry never fired because it was placed only on the deferred route.

## 10. Build Y

The minimal experiment: request the sub-mode, touch nothing else.

```text
0x4730   5  ->  xor eax,eax ; ret      InitWarehousePanel neutered
0x42E7  12  ->  call telemetry         deferred route
0xC345  12  ->  call telemetry         CanShow first-open route
```

Everything from W and X is retained, including the `SafeRestoreMode` call on
the close path (ASI `0xC514`), which is outside `InitWarehousePanel` and so
still restores the game state on close.

```text
output  patched/PrivateStorageAnywhere-1.18.2Y-nomodinit-candidate.asi
        E59019666CE72BE477A4753244AAA9CF8CBD7AE48C5CD1B9721F452FAEC31501
zip     patched/PrivateStorageAnywhere-v1.5.10-CD-1.18.2Y-nomodinit-DMM.zip
        5B8AF538C35F785FFAC27265D2E4DD2E3DA9CCA3630C9D7239D0A4E6E80A97BE
```

This build discriminates cleanly:

- opens -> the approach is proven and the legacy panel-faking code is obsolete
- no crash, no panel -> read the sub-mode from the telemetry and close lines
- still crashes -> the mod is no longer the cause. The next suspect is the
  mount itself needing an interaction target ("which warehouse"), which the
  real chest sets before the mode change. Trace from `game+0x52A11D`.

Not yet tested in game.

## 11. Build Y test result — the mount works

First success on 1.18.2. A storage window opened, the game did not crash, and
ESC closed cleanly:

```text
HOTKEY Private (vk=0x73) -> OPEN
  Warehouse opened (mode=0x04 sub=0x10)     before the request
AUTO-CAPTURED warehouse controller: 0x247E6522000
  Warehouse opened (mode=0x04 sub=0x05)     telemetry: dialog,store
ESC pressed -> closing warehouse
  Warehouse closed (mode=0x04 sub=0x05)
```

The mode-state fix is confirmed end to end: base pointer walk, field offsets,
dirty flag and menu-layer suppression all correct.

The panel was unconfigured. The `store` tag mounts the whole store-tagged family
at once — Camp Provisions donation UI, Trade Goods Storage, Mount Inventory,
Hold — with package defaults, which is the overlapping window in the user's
screenshot. Configuration is what `InitWarehousePanel` provides, and Y had it
neutered.

Note also: **no `Handler: blocking` line and no `CAPTURED ... via handler`
line**, so the handler hook never saw a sub-command packet. The capture line is
suppressed because CanShow had already captured, but the absence of any blocked
sub-commands means the game sent no UI-script commands to this controller.

### The three routes into InitWarehousePanel

```text
ASI 0x4319  posted message 0x65B, handled in fn 0x4270   message pump -- SAFE
ASI 0xBE3F  Handler hook  "lazy-instantiation fix"        packet dispatch
ASI 0xC375  CanShow hook  "first-open fix"                inside the mount -- crashed X
```

W took the deferred route but ran before the mount landed; X took the CanShow
route and ran init inside the game's mount call. The retry machinery for the
safe route already exists: `fn 0x57B0` arms `0x408F4` and posts `0x65B`
(ASI `0x5CC0`..`0x5CD8`) with an 1800-tick budget, and whichever route runs init
clears the arm flag.

## 12. Build Z

```text
0x4730   5  restore  InitWarehousePanel prologue (un-neutered)
0x5335/0x53B2/0x53D2  restore  SetInventory calls -- the CampWareHouse binding
0xBE05   2  74 3D -> EB 3D          handler-hook inline route declines
0xC337   6  0F 84 .. -> E9 .. 90    CanShow inline route declines
0x4319   5  call 0x4730 -> call 0x1570   route through the disarm stub
0x1570  13  new  xor eax,eax / xchg [0x408F4],eax / jmp 0x4730
0xBE6D   3  NOP  stop blanking the game's type-0x15 sub-commands
```

Both inline routes deliberately skip *over* their arm-flag clears
(`0xBE09`, `0xC33F`), so the input thread keeps re-posting until the message
pump route takes it — outside the game's mount call stack, which is the
crash-relevant property. The disarm stub then ensures init runs once.

Still NOP'd: the two fake command packets (`0x487E`, `0x4911`) and the
modal-pointer clear (`0x47C8`).

```text
output  patched/PrivateStorageAnywhere-1.18.2Z-deferredinit-candidate.asi
        DCF4C618258A356003431694C21C1A6464A9BBF850D3EBB53274D60B270E8E0C
zip     patched/PrivateStorageAnywhere-v1.5.10-CD-1.18.2Z-deferredinit-DMM.zip
        E2ED763F5BC5A0F2D7A534CD1FC92AF69FF310F0623904CACE47319477B43E16
```

Not yet tested in game.

## 13. Build Z test result, and what the panel actually is

Z crashed, but it confirmed the routing work and narrowed the fault precisely.

```text
CAPTURED warehouse controller via handler: 0x21F9F75F400   real chest open
HOTKEY Private (vk=0x73) -> OPEN
  Warehouse opened (mode=0x04 sub=0x10)
  Warehouse opened (mode=0x04 sub=0x05)                    sub-mode 5
  ... full init ran ...
  InitWarehousePanel done state: ... modalView=0x2204ADED980 childCount=0x4B6A62A8
```

- No `AUTO-CAPTURED` line, so the **deferred message route took the init** --
  the inline-route declines and the disarm stub worked as designed.
- Init returned normally (the "done state" line printed) and the crash came
  *after*. This is a corrupted-state crash on the next tick, not a fault inside
  init.

### The panel is one view with several focusable sections

```text
UI_WareHouse_KeyGuideFocusCampWareHouse
UI_WareHouse_KeyGuideFocusWagon
UI_WareHouse_KeyGuideFocusWareHouse
UI_WareHouse_KeyGuideFocusInventory
```

`SetInventory(Character,Focus,True,Default;CampWareHouse,Focus,True,Default)`
selects which sections render. Build Y applied none, so all of them drew at once
-- Trade Goods, Mount Inventory, Hold, plus the donation footer. That is exactly
the user's screenshot, and it confirms `SetInventory` is the right lever.

Only one `ShowPackage*` string exists in the whole executable
(`ShowPackageCampMoneyList`), so there is no package-name selector family to
chase -- the configuration really does travel as script sub-commands.

## 14. Build AA — packet trace

Diagnostic build. Reverts to Y's non-crashing behaviour and opens the gates on
the handler hook's existing packet inspection:

```text
0x4730   5  re-neuter InitWarehousePanel (back to Y)
0xBE4E   2  74 22 -> 90 90     log regardless of the init-pending flag
0xBE58   2  75 16 -> 90 90     log every packet type, not just 0x15
0xBE5A   3  8b 56 10 -> 0f b6 16   print the TYPE, not the sub-command count
0xBE5F   2  74 0f -> 90 90     do not skip type 0
0x29488 35  "  Handler: blocking %u sub-commands" -> "  [PKT] handler type=%u"
```

The `test rsi,rsi` null guard at `0xBE50` is kept, `0xBE6D` stays NOP'd, and all
structural fixes from W/X/Y/Z are retained. Every replacement is the same
length, so nothing reflows. The string is referenced only from `0xBE61`.

```text
output  patched/PrivateStorageAnywhere-1.18.AA-pkttrace-candidate.asi
        2C4E988534D020D86C26B701EDF100BD86F6185253CB0ECFC5F07D99C0688CFB
zip     patched/PrivateStorageAnywhere-v1.5.10-CD-1.18.AA-pkttrace-DMM.zip
        7907C7343C58905F545CCE65C4A118AC2412A0CFAD15F5504B9A2B5600E6A64F
```

Test protocol: one session, real chest first, then F4. The two `[PKT] handler
type=` runs are the deliverable. Expect roughly `18, 13, 21, 14, 15` (decimal)
for the chest, matching the `12 -> 0D -> 15 -> 0E -> 0F` sequence from the
original build-U trace.

## 15. Build AA test result — correct panel, but on primed state

AA produced the first fully correct Private Storage panel on 1.18.2: correct
title, 96/440 storage grid, 73/320 inventory grid, correct key guides.

**AA has `InitWarehousePanel` neutered, so the mod applied no configuration at
all.** The test protocol opened a real camp chest first; that chest configured
the controller, and F4 re-mounted the same already-configured controller.

The per-panel table at ASI `0x3C058` (6 entries x `0x128`) is the proof:

```text
panel[0] Private       Character,Focus,True;CampWareHouse,Focus,True
panel[1] Gatherables   Character,Focus,True;Housing_GatheredMaterials,Focus,True
panel[2] Dresser       Character,Focus,True;Housing_Dresser,Focus,True
panel[3] Refrigerator  Character,Focus,True;Housing_Refrigerator,Focus,True
panel[4] Symbol        Character,Focus,True;Housing_Symbol,Focus,True
panel[5] Collecting    Character,Focus,True;Housing_Collecting,Focus,True
```

`InitWarehousePanel` applies these. With it off, F5-F9 cannot work at all — no
mechanism selects their container — and cold-start F4 is unproven (build Y, same
configuration without a prior chest visit, gave the jumbled all-sections panel).

### Packet trace was inconclusive

Five packets, all `type=126` (`0x7E`), all during the real chest open, none
during F4. That does not match the `12 -> 0D -> 15 -> 0E -> 0F` sequence from the
build-U trace, so the handler hook's `rdx` is probably not the structure the
P-era logger inspected. Not worth chasing — the panel table answers the question
the trace was meant to answer.

## 16. Build AB

`InitWarehousePanel` restored **byte-for-byte identical to the original**
(verified across the whole `0x4730..0x57A2` body), running only from the message
pump. No build so far has combined the complete original init with all three
structural fixes.

The reasoning for restoring all of it rather than another subset: the routine
was always designed for a natively mounted view. Even in the working era the
flow was set store sub-mode -> game mounts -> init configures. W2 and Z ran half
the sequence, which corrupted the panel rather than configuring it. W ran the
full init but still had the menu-layer write forcing cinema.

Retained: mode-state layout, relaxed Safe\* guards, menu-layer suppression,
inline-route declines plus the disarm stub, sub-command blanking off, telemetry,
packet trace.

```text
output  patched/PrivateStorageAnywhere-1.18.AB-fullinit-candidate.asi
        819423B9AA40785B540DF20DD7E4AF6939F6B2B16903ACD15EAEC9FB6AACF122
zip     patched/PrivateStorageAnywhere-v1.5.10-CD-1.18.AB-fullinit-DMM.zip
        A7A02DA9683EC65FB2A52EA9EB6EDBD5FDC5A800996CF52B89B1FEC9697861C8
```

466 bytes changed in 40 runs. Test protocol: **cold start F4 first** (no chest
visit), then the menu-regression check, then F5-F9. Not yet tested in game.

## 17. Build AB test result — the init tears the panel down

AB crashed, but ran exactly as designed:

```text
AUTO-CAPTURED warehouse controller: 0x36628C79400   CanShow captured, then declined
  Warehouse opened (mode=0x04 sub=0x05)             message-pump route
  Cleared stale modal pointer at +0x258
  ... full original init ...
  InitWarehousePanel done state: +0x130=1 modalView=0x0 childCount=0x0
```

The CanShow hook captured and correctly declined the inline init; the message
pump ran init once. Routing is solid, so the fault is inside the init.

**`childCount=0x0`.** Build Y, same mount with init off, rendered real children.
The init destroys the panel's child widgets and the crash follows on the next
render. `modalView` also went from a live pointer to `0x0` via the restored
`+0x258` clear.

Correlation across safe-route builds (W full, Z partial, AB full: all crash;
Y and AA with init off: no crash) implicates the whole routine, so mechanism
decides: `childCount` -> 0 is what tearing down container bindings looks like.

### Teardown vs configure

The init was written for a world where the mod created and prepared the panel
itself, so several steps clean up leftovers from a previous mod-driven open. The
game owns the panel now, so those steps destroy live state:

```text
teardown (destructive now)          configure (still needed)
  clear modalView +0x258  0x47C8      prepare packet 0x15   0x487E
  SetInventory unbind CUR 0x53B2      command 0x0E          0x4911
                                      SetInventory bind CUR 0x53D2
                                      donation clear, titles
```

Confirmed by log strings: `0x53B2` -> `"SetInventory unbind CUR"`,
`0x53D2` -> `"SetInventory bind CUR"`. The third site `0x5335`
("unbind PREV") did not fire.

## 18. Build AC

AB minus exactly the two teardown writes:

```text
0x47C8  4  NOP  clear modal pointer +0x258
0x53B2  6  NOP  SetInventory unbind CUR
```

Everything else in the init stays byte-identical to the original, and all
structural fixes are retained. The kept steps are asserted pristine at build
time so a future edit cannot silently ship a half-run init again.

```text
output  patched/PrivateStorageAnywhere-1.18.AC-noteardown-candidate.asi
        4CBEEE60673D0AD69C86CF6693FA696D37B60811400F17AD96163E4D0D534B45
zip     patched/PrivateStorageAnywhere-v1.5.10-CD-1.18.AC-noteardown-DMM.zip
        F674D45D3A9A87FA0A5217494BA2DDA4EFBE2BCE872E05AA662586182FD225AE
```

Key metric on test: `InitWarehousePanel done state` must report a **non-zero
`childCount`**. Fallback order if it still crashes: title writes, donation clear
(`0x496A`/`0x4971`), PanelValue save, then the `0x15`/`0x0E` sends -- keeping the
bind longest, since F5-F9 cannot work without it. Not yet tested in game.

## 19. Crash telemetry — how to get the fault address

The game ships Sentry/crashpad. Windows logs no `Application Error` for
`CrimsonDesert.exe` because crashpad intercepts the fault, and crashpad then
faults itself inside `CharacterCreatorHead.asi` (offset `0x1194`), so no
minidump is ever written. But crashpad writes its event first:

```text
%LOCALAPPDATA%\Pearl Abyss\DumpCache\<uuid>.run\__sentry-event
```

MessagePack-ish but readable as text. The AC crash yielded:

```text
ExceptionCode:    STATUS_ACCESS_VIOLATION(c0000005)
ExceptionAddress: 000000014A0318F4
DumpKey:          7r5026190_25292_000000014A0318F4_LIVE_20260819_083734280
```

**Read this file after every crashing build.** Disabling
`CharacterCreatorHead.asi` would additionally let crashpad survive and write a
full minidump with a call stack.

## 20. Build AC test result — the fault located

```text
game+0xA0318D0   prologue
game+0xA0318F1   mov rdi, rcx
game+0xA0318F4   mov rcx, qword ptr [rcx+0x168]    <-- FAULT
```

Owning function `game+0xA0318D0..0xA031BA6`, reached only through a thunk at
`game+0xB38820` which has ~130 call sites across the UI code — a common
"walk this control's children" helper.

The fault is on the **first dereference of `this`**, so `rcx` itself is
unmapped: the game was handed a garbage control pointer, not an object with a
corrupted field. Init had already returned (its "done state" and `[CAP]` lines
print), so the game tripped over the wreckage afterwards.

### Retraction

Reading `childCount=0x0` in AB as "children were destroyed" was wrong. Across
builds that field reads `0x0`, `0x4B6A62A8`, `0xF5D6D430` — it is not a count.
AC's modal-clear NOP did work as intended (`modalView` stayed live at
`0x3E7F581F780` instead of being zeroed) and changed nothing about the crash.

### The surviving hypothesis

Every subset of the init crashes — full (W, AB), minus the packet sends (Z),
minus the teardown writes (AC). When every subset fails, the cause is unlikely
to be one bad write. The better fit is that the init runs **while the game is
still building the panel**: the message pump fixed re-entrancy, but it still
runs inside the same frame and the mount completes over several.

## 21. Build AD — readiness gate

The disarm stub at ASI `0x1570` becomes a gate. It waits for the captured
controller's child vector — the exact field whose staleness produced the fault —
to be populated before committing:

```asm
0x1570  mov  rax,[0x3D528]        ; g_capturedHandler
0x1577  test rax,rax / je 0x1595
0x157C  mov  rax,[rax+0x168]      ; UI child vector
0x1583  test rax,rax / je 0x1595
0x1588  xor  eax,eax
0x158A  xchg [0x408F4],eax        ; disarm only on commit
0x1590  jmp  0x4730               ; InitWarehousePanel
0x1595  ret                       ; still armed -> retried next tick
```

Returning without disarming relies on the mod's existing retry: the input thread
re-posts `0x65B` while `0x408F4` stays armed, bounded by its own 1800-tick
budget. Leaf plus tail jump, so unwind-safe like the other stubs.

Cave layout: stub `0x1460+0x32`, helper `0x14C0+0x3F`, telemetry `0x1500+0x62`,
gate `0x1570+0x26`, end `0x15A9`.

Everything else is identical to AC. One variable changed.

```text
output  patched/PrivateStorageAnywhere-1.18.AD-readygate-candidate.asi
        A0CACC40D52FF1BA1390D24945708A2BE255AE29514B36A42B25BB901E766831
zip     patched/PrivateStorageAnywhere-v1.5.10-CD-1.18.AD-readygate-DMM.zip
        D6349C17CEB6ED96A39E60B2A0409BE9958C2A38CD25D4BEAB1CB708CEDA440C
```

Three possible outcomes: works; crashes again (compare the new
`ExceptionAddress` against `0x14A0318F4`); or opens unconfigured with no init
lines, meaning the gate never fired and the readiness signal is not `+0x168`.

If the gate does not help, bisect with the fault address as the instrument:
disable everything in the init except the `SetInventory` **bind** (`0x53D2`) in
one build. Not yet tested in game.

## 22. Build AD test result — the timing hypothesis is dead

Same fault address as AC, byte-identical:

```text
ExceptionAddress: 000000014A0318F4
```

The log shows the init ran, so the readiness gate **fired** — the controller's
child vector at `+0x168` was already populated. The panel is built by that
measure and the init still wrecks it. Re-entrancy was a real bug and is fixed,
but waiting longer is not the answer.

Standing table:

```text
init off  (Y, AA)                   -> no crash, panel renders
init on, any subset (W,Z,AB,AC,AD)  -> crash at game+0xA0318F4
```

## 23. The container mechanism is the game's own inventory-key system

The game keeps an `InventoryInfoKey` name pool at
`game+0x144FC0490`..`0x144FC0560`:

```text
Housing_Symbol  Housing_Refrigerator  Housing_GatheredMaterials
Housing_Collecting  Housing_Dresser  WareHouse  CampWareHouse
InvisibleInventory  ConvertMoney  ConvertItem  BirdFeed  CampStraw
Recovery  PetAndVehicle  PearlCharacter  PearlUser  Money
```

The mod's per-panel table at ASI `0x3C058` names exactly these keys, and during
a real chest open the game's own UI script calls the same `SetInventory` on the
same panel with a byte-identical argument:

```text
SetInventory(Character,Focus,True,Default;CampWareHouse,Focus,True,Default)
```

So the mod's approach is architecturally correct and its argument is right.
There is no separate package-selector to chase — only one `ShowPackage*` string
exists in the whole executable.

## 24. Build AE — isolate SetInventory

Single change from AD: NOP the bind at ASI `0x53D2`. Both `SetInventory` calls
are now off; the rest of the init is untouched.

`SetInventory` bind is the last untested variable — present in every crashing
build, absent from every clean one, and the only init step F4–F9 need.

```text
output  patched/PrivateStorageAnywhere-1.18.AE-nosetinv-candidate.asi
        448D94BA9FAB00D76716AB6865C898FBF021197728AA1792668A252084BC3FFB
zip     patched/PrivateStorageAnywhere-v1.5.10-CD-1.18.AE-nosetinv-DMM.zip
        20279AE2E221DE2DAF5AEF95C2076C525B690AC657D8BC0BD845DEE41FA021FC
```

Test also asks for `CharacterCreatorHead.asi` to be disabled: it crashes
`crashpad_handler.exe` at offset `0x1194` on every game crash (five occurrences
in the Windows event log), which is why `DumpCache
eports` is empty and we
have never had a call stack.

- no crash -> `SetInventory` is the culprit; rest of init is safe
- crash at the same address -> the init is not the proximate cause; stop
  reviving it and change approach

## 25. Interim fallback

**Build AA is a working mod today** with one caveat: a real camp-chest visit
once per session primes the panel, after which F4 opens correct Private Storage
from anywhere. The original mod already required an NPC visit per session for
the housing chests, so this is a comparable limitation and a legitimate release
point if the AE round does not resolve the init.

## 26. ROOT CAUSE — the donation write targets a stale offset

Disabling `CharacterCreatorHead.asi` let crashpad survive and write a real
minidump (`DumpCache
eports\*.dmp`). Parsing it gave registers and a stack.

### Evidence

```text
Rip = 0x14A0318F4      mov rcx, qword ptr [rcx+0x168]
Rcx = 0x0000FFFFFFFFFFFF        <- 48 bits of ones, a poison value
Rbx = 0x42DD8840800             <- identical to `handler=` in our own log
```

The faulting function's frame is `0x188` bytes (five pushes + `sub rsp,0x160`),
so the return address is at `[rsp+0x188] = 0x140B32AB7`, i.e. the caller is
`game+0xB32AB2` inside `fn 0xB32A40..0xB32ACD`:

```text
game+0xB32AAB   mov rcx, qword ptr [rbx + 0x330]
game+0xB32AB2   call ...                            <-- FAULT
```

**No `PrivateStorageAnywhere.asi` frames anywhere on the stack.** The mod is not
on the call path; it left a bad value for the game to trip over later.

`controller+0x330` is written by the mod's donation clear (ASI `0x496A`/`0x4971`):

```asm
mov dword ptr [ctrl+0x330], 0xFFFFFFFF
mov word  ptr [ctrl+0x334], cx          ; cx == 0xFFFF
```

Six bytes of `0xFF` = `0x0000FFFFFFFFFFFF` as a qword. Exactly `Rcx`.

### Why

The mod's boot log, in every run since the start:

```text
SetDonationFaction: FAIL base+0x0
DonationOff:  FALLBACK offset=0x330 (SetDonationFaction -> MOV [reg+N],BX)
```

`0x330` is a **fallback guess**. On an older build it was a donation-faction ID
where all-ones meant "none". On 1.18.2 `controller+0x330` is a **pointer** the
game dereferences and calls a method on.

```text
donation write ON   W, Z, AB, AC, AD, AE  -> crash, every one at 0xA0318F4
donation write OFF  Y, AA                 -> no crash
```

Perfect correlation, and now with a mechanism. This is also why bisecting every
other init step never helped.

### Minidump parsing

`scratchpad/mdmp.py` parses the MDMP header, stream directory, module list,
exception stream, AMD64 CONTEXT (Rax at `+0x78`, Rsp `+0x98`, Rip `+0xF8`) and
both memory-list stream forms, then walks the stack for return addresses.

## 27. Build AF

One byte: ASI `0x4968` `74 18` (`je`) -> `EB 18` (`jmp`), taking the
"already cleared" branch so the donation write and its log line are skipped.
The absence of `Cleared donation state at +0x330` from the log is positive
confirmation the fix is live.

Everything else in `InitWarehousePanel` is restored to the original — verified:
the whole `0x4730..0x57A2` body differs from the pristine ASI in **exactly one
byte**. `SetInventory` is enabled again, so this is the first build where F5–F9
can work.

```text
output  patched/PrivateStorageAnywhere-1.18.AF-donationfix-candidate.asi
        1975B4587624C302B38916411323350687BE89647642B4D926A8AD0F68878D4B
zip     patched/PrivateStorageAnywhere-v1.5.10-CD-1.18.AF-donationfix-DMM.zip
        1E4E38AE383C88066B737B4E027EB111ED15B4436087481D4C6442E6B0F54616
```

Known cosmetic side effect: the donation footer may show, since that state is no
longer cleared. Resolving `SetDonationFaction` properly on 1.18.2 is the
follow-up. Not yet tested in game.

## 28. Build AF result — the fix missed, and revealed landmine 2

### AF's donation patch was ineffective

AF patched only the `je` at ASI `0x4968`, which is the branch taken when the
field is **already** all-ones. Two earlier branches reach the write directly:

```text
0x4951  mov ecx, 0xffff
0x4956  cmp word [rdx+rdi], cx
0x495A  jne 0x496A          <- taken in the normal case, bypasses the patch
0x495C  cmp word [rdx+rdi+2], cx
0x4961  jne 0x496A          <- also bypasses
0x4963  cmp word [rdx+rdi+4], cx
0x4968  je  0x4982          <- the only one AF patched
```

Confirmed by the log still containing `Cleared donation state at +0x330`.
(`cx = 0xFFFF` at `0x4951` also confirms the six-0xFF write, so the landmine-1
analysis itself stands.)

### Landmine 2 — modal-view pointer, controller+0x258

AF crashed at a **new** address and the dump named it:

```text
game+0xB2F626   mov rax, qword ptr [rsi+0x258]   ; rsi == our controller
game+0xB2F62D   mov rcx, qword ptr [rax+8]       ; FAULT, rax == 0
```

The game dereferences the modal-view pointer with **no null check**. The mod's
`Cleared stale modal pointer at +0x258` writes NULL there.

AC had disabled this write and still crashed, which led me to call it innocent.
That was wrong — AC was still hitting landmine 1.

### The complete model

```text
modal off, donation off   Y, AA           -> NO CRASH
modal off, donation on    Z, AC, AD, AE   -> crash 0xA0318F4 (donation)
modal on,  donation on    W, AB           -> crash 0xA0318F4
modal on,  donation on    AF              -> crash 0xB2F62D  (modal, hits first)
```

Every build accounted for. Both landmines are the same class: "clean up
leftovers from the previous mod-driven open" steps that were correct when the
mod built the panel and are destructive now that the game does.

## 29. Build AG

28 bytes, all NOPs, both in `InitWarehousePanel`:

```text
0x47C8   4  NOP  clear modal pointer +0x258        (landmine 2)
0x496A  24  NOP  donation write block + its log    (landmine 1, all paths)
```

`0x4968` reverted to the original `74 18`. Everything else in the init is the
original byte for byte, including both `SetInventory` calls — so F5–F9 are live
for the first time.

Confirmation signal: the log must **not** contain `Cleared donation state at
+0x330`. The `Cleared stale modal pointer` line will still print, since only its
write is NOP'd.

```text
output  patched/PrivateStorageAnywhere-1.18.AG-twolandmines-candidate.asi
        C164BAF4133F81C3591FF9CFE44B03DE2D98D6C8EC2C56A20E2A0262763A7536
zip     patched/PrivateStorageAnywhere-v1.5.10-CD-1.18.AG-twolandmines-DMM.zip
        E262054FAA8B236D53896552AAF31ED82CD228CD881CCAADA54CFAA7897FA689
```

Remaining mod writes to the controller not yet cleared of suspicion:
`+0x110` (PanelValue), `+0xF0`/`+0x380` (titles), `+0x130` (active flag),
`+0x2CD`. If a third landmine exists, the dump pipeline finds it in one round.
Not yet tested in game.

## 30. Build AG — WORKING

Tested 2026-08-19. All six panels, twelve opens, no crash, item transfer
verified by the user.

```text
OPENING WAREHOUSE (Private)      x3      OPENING WAREHOUSE (Refrigerator)  x3
OPENING WAREHOUSE (Gatherables)  x2      OPENING WAREHOUSE (Symbol)        x1
OPENING WAREHOUSE (Dresser)      x2      OPENING WAREHOUSE (Collecting)    x1
```

Also confirmed working: toggle-to-close (`HOTKEY Refrigerator -> CLOSE`),
switching directly between chests, and repeated open/close cycles. Zero
`Cleared donation state` lines, so the fix is live. **The first F4 was a cold
start with no prior chest visit**, so the AA priming caveat is gone.

Deployed ASI verified byte-identical to
`C164BAF4133F81C3591FF9CFE44B03DE2D98D6C8EC2C56A20E2A0262763A7536`.

### The complete fix, from the pristine v1.5.10 ASI

Structural (required for 1.18.2):

```text
0x1460  stub    publish real mode layout: 0x18/0x19/0x21/0x28
0x14C0  helper  get_mode_obj: *(0x3D518) -> +0x90 -> +0x1158
0x1500  stub    telemetry (diagnostic)
0x1570  gate    readiness gate + disarm
0xAA8C  NOP     1.18.2 menu-manager backlink check
0xB006/0xB01E/0xAEF6/0xAF0E   relax Safe* offset range guards
0xB0DA  rewrite SafeSetup tail: flags[4]=1, subtypes[5]=1, dirty=1
0xAF4E  rewrite SafeRestore body + dirty
0xB447/0xC4FE  re-point the mode base to modeObj
0xB8DE/0xB950/0xB965  NOP menu-layer writes (stop forcing cinema sub-mode)
0xBE05/0xC337  inline init routes decline (fix re-entrancy)
0x4319  route the deferred init through the gate
0xBE6D  NOP sub-command blanking
```

The two landmines (the actual crash causes):

```text
0x47C8   4 bytes NOP   clear modal pointer +0x258
0x496A  24 bytes NOP   donation write block +0x330 (all three entry paths)
```

Diagnostics still present (candidates for removal in a release build):
telemetry redirects at `0x42E7`/`0xC345`, packet trace at
`0xBE4E`/`0xBE58`/`0xBE5A`/`0xBE5F`, and the repurposed format string at
`0x29488`.

### Known remaining issues (none blocking)

1. **All housing panels share one PanelValue.** The log shows
   `handler+0x110 = 0x2D1` for Dresser, Refrigerator, Symbol and Collecting
   alike — the Gatherables ID, via `INI fallback (Gatherables)`, because
   `Gatherables panel-id lookup FAILED (hash-map not yet populated?)`.
   `SetInventory` does the real container selection so it appears harmless, but
   each chest should be checked for distinct contents.
2. **Inventory slot expansion disabled**: `Inventory mgr ptr: WARN dynamic scan
   failed`. The 10 -> 1000 slot feature is off this session.
3. **`LangByte: FAIL`** — language detection falls back to defaults. Fine for
   English, matters for a public release.
4. **Donation footer** may be visible, since that state is deliberately no
   longer cleared. Resolving `SetDonationFaction` on 1.18.2 would fix it
   properly.
5. **Menu regression unverified** — vendor / camp provisions / bank after using
   the mod (the v1.5.6 failure class) has still not been tested.

## 31. Slot expansion — why it was off, and Build AH

### Resolver: secondary validation too strict (fixed, 1 byte)

`Inventory mgr ptr: WARN dynamic scan failed` originates at ASI `0xA4B3`. The
**primary** scan of `game+0xB32AD0` (`SetInventory`) succeeds — exactly one
candidate exists in the whole function:

```text
SetInventory+0x225   mov r10, [rip+0x578EFE4]   -> RVA 0x62C1CE0, dest = r10
```

That global is genuine: 110 code sites in the main code section load it.

The **secondary** check at ASI `0xA560` requires, within 0x60 bytes, a
`mov r64,[<same reg>+disp8]` with `disp8` in `{0x60,0x70,0x78}` (0x68 skipped).
1.18.2 offers exactly one qualifying instruction:

```text
SetInventory+0x24F   4D 03 5A 78   add r11, [r10+0x78]
```

REX.W ok, `mod=01` ok, base `r10` ok, disp `0x78` ok — only the opcode differs
(`0x03` add vs the hardcoded `0x8B` mov). Fix: ASI `0xA583` `8b` -> `03`.

### Entry array offset moved (fixed, 1 byte)

Patcher at ASI `0x8720` reads count `mgr+0x08`, array `mgr+0x50`. The game's own
linear accessor gives the 1.18.2 layout:

```text
game+0x3AA5BC   cmp edi, dword [rbx+0x08]    ; count  -- mod already correct
game+0x3AA5CD   mov rax, qword [rbx+0x58]    ; array  -- mod had 0x50
game+0x3AA5D1   mov rax, [r14+rax]           ; 8-byte stride
```

Fix: ASI `0x874F` `50` -> `58` (disp8 of `mov r10,[rax+0x50]`).

### Per-entry slot field `+0x48` — UNVERIFIED

`_defaultSlotCount`, `_maxSlotCount`, `_slotCount` exist only inside localized
Korean error-message strings. No offset metadata, no qword refs, no RVA-dword
refs, no `lea` references anywhere in the code section. Not statically
recoverable at reasonable cost.

Because this patcher walks **every** entry of a manager with 110 users, a wrong
offset corrupts inventory data rather than merely crashing, and a subsequent
save could persist it. So AH suppresses the write and lets the counter report.

### Build AH

```text
0xA583  1  8b -> 03            resolver accepts the add form
0x874F  1  50 -> 58            entry array offset
0x8790  5  NOP                 suppress `mov word [rax+0x48], r11w`
```

8 bytes total vs AG (3 sites + tag byte); the entire storage-opening path is
byte-identical to AG. The `cmp word [rax+0x48],0xa` guard and `inc r8d` counter
are left intact, so the existing log line reports the would-be count with zero
writes.

Re-application needs no new code: the worker thread (ASI `0x13F6`, 1000 ms then
3000 ms cadence) and the on-open patch (ASI `0x4781`) were both already wired,
just gated behind the failing resolver.

```text
output  patched/PrivateStorageAnywhere-1.18.AH-slotdiag-candidate.asi
        D91938FE68BF08D9DAC3017531CB10B7D485BDDDA2F9CF3E91C41730C57DBDC1
zip     patched/PrivateStorageAnywhere-v1.5.10-CD-1.18.AH-slotdiag-DMM.zip
        FB5B9E2C7FAB89EBD1EE71A5A2C428109D270E5F0CEF01DC9584AA7782307289
```

Decision rule on the reported count N: a handful (5–15) -> enable the write in
AI; 0 or dozens+ -> the field is wrong, locate the real one first.

## 32. Key-binding audit — fully INI-driven, no changes needed

No per-panel key names exist in the binary; only templates, each referenced
exactly once inside a loop bounded by `cmp rdi, 6` at ASI `0x6ADD`:

```text
0x29100 '%sHotkey'            ref 0x6888      0x29150 '%sInitString'  ref 0x6A5B
0x29110 '%sModifier'          ref 0x68F5      0x29180 '%sPSButton'    ref 0x6BF8
0x29120 '%sControllerButton'  ref 0x696A      0x29190 '%sPSModifier'  ref 0x6CBA
0x29138 '%sControllerModifier' ref 0x69E2
```

Per panel it formats `<PanelName>` (pool at `0x28400`–`0x28438`) + template into
a 0x40 buffer and calls `GetPrivateProfileStringA` on `[Settings]`.

- **No hardcoded F-keys** — F4–F9 are INI defaults only.
- **`00` = disabled works** — every poll guards
  `mov ecx,[panel+0x104] / test ecx,ecx / je skip` before `GetAsyncKeyState`
  (panel hotkeys `0x44A3`, reload key `0x5839`, also `0x588A`).
- **Hot reload works** — `ReloadKey` read at `0x66D5`; handler logs
  `INI reloaded (hotkey 0x%02X)` at `0x5872`.
- **VK catalog** — the mod never generates an INI, only reads one. The keyboard
  / XInput / PS5-PS4 tables live in the shipped `PrivateStorageAnywhere.ini`
  (lines 127–192) and survive only because that file ships with the package.

## 33. Build AH test result — slot field confirmed

Both AH fixes landed exactly as predicted:

```text
Inventory mgr ptr:   OK base+0x62C1CE0 (SetInventory scan @ +0x225, dest=r10)
```

`WARN dynamic scan failed` is gone, and so is `InventoryInfo worker: NOT
STARTED`. The worker and the on-open patch (`Slot patch (on open): 5 entries`,
6 occurrences) both run.

### The count

167 worker log lines, **every one reporting exactly 5**, zero variance. The
volume is the ~3s worker cadence, not the entry count.

Five is exact, not merely plausible. The `InventoryInfoKey` pool at
`game+0x144FC0490`..`0x144FC0560` holds 18 keys, of which exactly five are
`Housing_*`:

```text
Housing_Symbol  Housing_Refrigerator  Housing_GatheredMaterials
Housing_Collecting  Housing_Dresser
```

Those are the five housing chests that default to 10 slots. `CampWareHouse`
(Private Storage, 440) and the other 13 keys correctly fail the `== 10` guard.
So `word[entry+0x48]` is the slot-count field and the walk selects exactly the
intended containers.

### The log spam was an artifact of the suppression

The worker skips logging when the count is zero (`test eax,eax / jle` at ASI
`0x1405`). With the write NOP'd the five entries stayed at 10 forever, so every
pass re-matched and re-logged. With the write enabled the first pass sets them to
1000, later passes match nothing, and the worker goes quiet. Enabling the write
removes the spam.

## 34. Build AI — slot expansion enabled

AH minus the suppression. Net delta from the working AG build is **3 bytes**:

```text
0xA583   8b -> 03    resolver accepts `add r11,[r10+0x78]`
0x874F   50 -> 58    entry array mgr+0x50 -> mgr+0x58
0x2D117              build tag byte (AG -> AI)
```

`0x8790` (`66 44 89 58 48`, `mov word [rax+0x48], r11w`) is byte-identical to the
pristine ASI. Verified unchanged vs AG: both landmine NOPs, init prologue,
`SetInventory` bind, menu-layer NOPs, readiness gate, both inline-route
declines, both spinlock releases, `SafeSetup` call and the safe-state gate.

```text
output  patched/PrivateStorageAnywhere-1.18.AI-slots1000-candidate.asi
        42F5B36B61450406D8AEDEA31E6BC0F32172B9C4BC107200C43221DEA0A3014C
zip     patched/PrivateStorageAnywhere-v1.5.10-CD-1.18.AI-slots1000-DMM.zip
        306F433252579E5283C0E51B4B202399C32A7808F328810978BFBB7009127F8C
```

Expected: F5-F9 report 1000 capacity, F4 stays at 440, one startup log line then
silence. If capacity still reads 10, `+0x48` is a default the UI does not read
directly and the live per-save capacity lives elsewhere — a separate thread, not
a regression.

## 35. Build AI confirmed working; Private Storage expansion is NOT original behaviour

AI verified in game: F5-F9 report 1000 slots, F4 (Private) stays at 440, all six
panels open, no crash. The 3-byte delta from AG achieved the full 1000-slot
restoration.

### Nexus description check

The user believed the original mod also raised Private Storage to 1000. It does
not. Full-page check of the description (26,222 chars) - every capacity mention:

```text
"A housing chest could briefly show its vanilla capacity (10 slots instead of 1000)"
"Fixed: Inventory slot expansion (10 -> 1000) works again."
"This mod has not been tested with any inventory slot expansion or
 Private Storage expansion mods."
```

`10 -> 1000` is the housing default. The third line explicitly lists Private
Storage expansion as a separate third-party category the author does not
support. The 440 the user sees is game progression; the game reaches 1000 near
endgame on its own.

Raising Private is therefore a **net-new local feature**, requested as an INI
option.

## 36. Executable slack found in .text

The old resolver cave is nearly exhausted (19 bytes) and all `0xCC` padding runs
are 16-21 bytes. Usable space instead:

```text
.text  VA 0x1000  VSize 0x26A20  RawSize 0x26C00
slack  rva 0x27A20..0x27C00  = 480 bytes
```

All zeros, executable, inside the mapped range (`.text` maps to `0x28000` since
`0x26A20` rounds up to `0x27000`), and no pointer anywhere in the ASI targets
it. Section table also has room for 7 more headers if a bigger cave is ever
needed, but it is not.

## 37. Build AJ — entry-table dump

Diagnostic only; behaviour identical to AI.

```text
0x27A20   35B  format string "  [SLOT] key=0x%04X slots=%u idx=%u"
0x27A60  151B  dump_once routine (265 bytes of slack still free)
0x140F    12B  lea+call -> call dump_once + 7 NOPs
```

Hooked on the worker's first-run-only log path (guarded by `test bl,bl`), so it
fires exactly once per session rather than every 3s tick. `dump_once` re-emits
the original line, then walks the table the same way the patcher does
(`[0x3D598]` -> deref -> count `+0x08`, array `+0x58`, 8-byte stride) and logs
each entry's key (`+0x04`) and slot count (`+0x48`).

Uses only volatile registers plus `sub rsp,0x58` (0x20 shadow + three locals at
`+0x30`/`+0x38`/`+0x40`), since the logger spills `rcx/rdx/r8/r9` to home space.
All control flow is rel32 — the loop body exceeds rel8 range.

```text
output  patched/PrivateStorageAnywhere-1.18.AJ-slotdump-candidate.asi
        4566884271221FBA560CB2238E462E4BA338206E18FAEA5685FF40DD1A1F08C6
zip     patched/PrivateStorageAnywhere-v1.5.10-CD-1.18.AJ-slotdump-DMM.zip
        C57BD6E33582A63E1D92E4FEF5DB59E81B1E09569AED4ED53CD5134E0A15AECD
```

Target: the entry reading `slots=440` is CampWareHouse; its `key=` is what the
follow-up build matches. Round 2 then adds `PrivateStorageSlots` to the INI
(default 0 = off) and applies it inside the existing patcher loop, so the worker
and the on-open patch both re-apply it for free.

Open item for round 2: the INI value needs a writable global, so it must live in
`.data` (`.text` is read-execute). Locate a genuinely unreferenced dword rather
than guessing into the BSS tail.

## 38. Build AJ result — two retractions, and the identifier that works

AJ ran clean (no regressions). It also invalidated two of my assumptions.

**Retraction 1: `entry+0x04` is not the inventory key.** Nearly every entry
reported `key=0x0290`, one `0x0000`. The game's `cmp word [r9+4], cx` operates on
the bucket array at `mgr+0x80`, not the linear array at `mgr+0x58` the patcher
walks.

**Retraction 2: no entry holds 440.** The dump found 20 entries:

```text
idx  0-13 : 20 50 20 20 300 1 1 240 240 300 50 300 240 5
idx 14-18 : 1000 x5        <- the housing chests patched by AI
idx 19    : 50
```

Private's displayed 440 is base capacity plus save-side expansions, so it cannot
be matched by value either.

### Interned-key globals

Each container name is interned into a global by a C++ static initialiser
(registrars clustered around `game+0x181C71..0x182B6B`, two `lea`+`call`
`0x14031CBB0` per name, result stored to a `0x624xxxx` global):

```text
CampWareHouse              0x623FD88     WareHouse             0x623FDE8
Housing_GatheredMaterials  0x6240148     Housing_Symbol        0x6240088
Housing_Refrigerator       0x6240028     Housing_Collecting    0x62400E8
Housing_Dresser            0x623FFC8     Money                 0x623FAE8
InvisibleInventory         0x6240208     Kuku                  0x62401A8
BirdFeed                   0x623FF08     CampStraw             0x623FEA8
Recovery                   0x623FF68     PetAndVehicle         0x623FCC8
PearlCharacter             0x623FC08     PearlUser             0x623FBA8
```

Independently confirmed: the mod's own boot line
`HGM string slot: OK base+0x6240148` matches the computed global for
`Housing_GatheredMaterials` exactly.

## 39. Build AK — Private Storage slot option

Net-new feature, default off. Identifies the CampWareHouse entry at **runtime**
by scanning each entry's first `0x48` bytes for the interned key at
`gameBase+0x623FD88` — no hardcoded index, no save-specific literal.

```text
cave 0x27A20  "PrivateStorageSlots"
     0x27A40  "  [PRIV] slots %u -> %u (idx=%u)"
     0x27A70  "  [PRIV] CampWareHouse entry not found"
     0x27AA0  wrapper (0x138 bytes, ends 0x27BD8, cave ends 0x27C00)
0x1400  -> call wrapper   (worker thread)
0x4776  -> call wrapper   (on-open, InitWarehousePanel)
0x140F  restored to the original lea+call (AJ diagnostic removed)
```

The wrapper calls the original patcher first, preserves its return value, then
reads `PrivateStorageSlots` via `GetPrivateProfileIntA` (IAT `0x28100`) against
the mod's own INI path buffer at `0x3D3A0`. No new writable global was needed —
`.text` is read-execute, and the apparent 2 KB gaps in `.data` are buffer
interiors reached from a base pointer. Reading per pass also gives hot-reload
for free (~3s, no F11).

Hooking both patcher callers means the option re-applies on the worker cadence
and the instant a storage opens, matching the housing behaviour.

Failure is safe by construction: option 0 -> no writes; key not found -> logs
`not found`, no writes; value already correct -> no write.

INI also had a stray bare `LF` after `TraceMode=0` (introduced when TraceMode was
first set to 0); normalised.

```text
output  patched/PrivateStorageAnywhere-1.18.AK-privslots-candidate.asi
        3E8695DBD484289B7C78A27B81F4FFD93F81BAE06C91B784F7B7C09BD15E8969
ini     6B884377FBA657FDB998E8B1A95C9978B30C9B97046BD0FAAEA46D80493BBB5E
zip     patched/PrivateStorageAnywhere-v1.5.10-CD-1.18.AK-privslots-DMM.zip
        8E84B7D4DACAE45477CC7B8079CD0AE9E0199794CAD75B625496B0005DA619BF
```

Everything else byte-identical to AI. Not yet tested in game.

## 40. Build AK result — wrong identifier, failed safe

`[PRIV] CampWareHouse entry not found`, no writes, no crash. The INI plumbing is
confirmed working: that message is only reachable past the `test eax,eax` gate,
so `PrivateStorageSlots=1000` was read correctly.

The identification was wrong. The entry does not store the interned-key global
from `game+0x623FD88`, at least not within its first `0x48` bytes.

## 41. The real name -> key -> entry path

Tracing `SetInventory`'s token loop:

```text
game+0xB32CC6  mov  word [rsp+0x20], bx     ; 0xFF invalid-key default
game+0xB32CCB  mov  rax,[rbp-0x60]          ; parsed token
game+0xB32CD4  mov  rcx,[rax]               ; const char* name
game+0xB32CCF  lea  rdx,[rsp+0x20]          ; uint16* outKey
game+0xB32CD7  call game+0x1E141D0          ; NameToKey -> al
game+0xB32CDE  jne  game+0xB32CF5           ; success -> manager lookup
```

`game+0x1E141D0` = `bool NameToKey(const char*, uint16_t*)`. The manager then
resolves key -> entry at `SetInventory+0x225`:

```text
guard  dword[mgr+0x6c] != 0
mod    r8d = dword[mgr+0x68]
bucket ((key % r8d) << 8) + [mgr+0x78]
count  dword[bucket]
match  dword[bucket + i*8 + 8]  == key
index  dword[bucket + i*8 + 0xc]
entry  [[mgr+0x80] + idx*8]
verify word[entry+4] == key
```

Note this yields objects from the `+0x80` array, whereas the housing patcher
walks `+0x58` — hence the extra plausibility guard in AL.

## 42. Build AL

```text
cave 0x27A20  "PrivateStorageSlots"
     0x27A34  "  [PRIV] slots %u -> %u"
     0x27A4C  "  [PRIV] lookup failed"
     0x27A64  "  [PRIV] rejected (%u)"
     0x27A7C  wrapper, 388 bytes, ends 0x27BFE (cave ends 0x27C00)
0x4776  -> wrapper   (on-open only)
0x1400  -> reverted to calling the patcher directly
```

Only the on-open caller is hooked, so `NameToKey` is never called from the mod's
background worker thread. Housing expansion is unaffected — the patcher still
runs from both callers.

Write is gated by two independent checks: the game's own `word[entry+4] == key`,
and `0 < word[entry+0x48] <= 2000`. Distinct log lines for success / lookup
failure / rejection.

**Build-time bug caught in review:** the bucket loop head was emitted as
`41 39 CA` (`cmp r10d, ecx`) instead of `44 39 CA` (`cmp edx, r9d`) — REX.B where
REX.R was needed. That would have compared uninitialised garbage and either
bailed immediately or read out of bounds. Fixed before shipping.

```text
output  patched/PrivateStorageAnywhere-1.18.AL-privslots2-candidate.asi
        FCDD2399090EED85194EA81813D8DC7049E54F2C0DEFEB7261599EB0B9542137
zip     patched/PrivateStorageAnywhere-v1.5.10-CD-1.18.AL-privslots2-DMM.zip
        80D4EE629426E8470E90A7026782F24EA0E0F746E0544F6DD46A82DB80AA4DA9
```

If AL also misses, stop: the core work (opening on 1.18.2, both crash fixes,
housing 1000-slot restoration) is complete and tested, and Private reaches 1000
through normal progression regardless.

## 43. Build AL result — lookup works, wrong array

```text
[PRIV] slots 39 -> 1000
```

`NameToKey` + the bucket walk resolved a CampWareHouse-keyed record, both guards
passed, and the write landed (logged once; idempotent thereafter). But the UI
still read 440 and the value found was **39**, which appears nowhere in AJ's
dump of the 20 static entries.

### The manager has two parallel arrays

```text
mgr+0x08   count   (20)
mgr+0x58   array A   <- housing patch writes here; +0x48 drives the UI
mgr+0x80   array B   <- what the bucket lookup returns
```

Both are indexed by the same integer with an 8-byte stride:

```text
game+0x3AA5BC   cmp edi,[rbx+0x08] / mov rax,[rbx+0x58] / mov rax,[r14+rax]
game+0xB32D37   mov edx,[r11+r8*8+0xc] / mov rax,[r10+0x80] / mov r9,[rax+rdx*8]
```

AL computed the right index and applied it to the wrong array. Array B's `+0x48`
is not capacity.

The stray write is harmless: `StaticInfoManager2` data is rebuilt from the
game's data files each launch, never persisted to the save.

## 44. Build AM

Same front half (INI read, `NameToKey`, manager guards, bucket walk). The tail
now takes the bucket's index, bounds-checks it against `dword[mgr+0x08]`, and
resolves through **array A**:

```text
0x27B53  mov edx,[rax+rdx*8+0xc]     ; index from bucket
0x27B5C  cmp edx,[rcx+8]             ; bounds  (NEW)
0x27B69  mov rcx,[rcx+0x58]          ; array A (was +0x80)
0x27B76  mov rcx,[rcx+rdx*8]         ; entry
0x27B83  movzx eax,word[rcx+0x48]    ; capacity
         0 < eax <= 2000 gate
0x27BA6  mov word[rcx+0x48], dx      ; write
         log "  [PRIV] idx=%u slots=%u -> %u"
```

The `word[entry+4] == key` check is dropped — array A entries do not carry the
key there (AJ dumped `0x0290` for nearly all). Safety rests on the bounds check,
two null checks, the plausibility gate, and the logged `slots=` value making a
wrong pick immediately visible.

Array B is no longer referenced anywhere in the cave (verified by byte search).

```text
strings 0x27A20 (72B)   code 0x27A68..0x27BDB   cave ends 0x27C00 (36B spare)
output  1DB030EEF418C0AC0194C627004FE9B206029F11B781C9E07E578A781202E1AD
zip     6052D82054865812EDDF4AE3A0E4F6A8E6220D2BDEA1F06E36EB9434A3EC0B31
```

Decision rule: if the logged `slots=` is one of AJ's dumped values (20/50/240/
300/1/5) the parallel-array model holds. If not, identify the entry directly by
scanning array-A entries for a pointer into the interned-name region
(`game+0x623F000..0x6241000`) and map it via the table in §38.

## 45. Build AM result — right record, wrong moment

```text
  [PRIV] idx=7 slots=240 -> 1000
```

`240` is exactly AJ's dumped value at position 7 (`20 50 20 20 300 1 1 240 ...`,
§38). The parallel-array model **holds**: the bucket index carries across, and
the write landed on the genuine `CampWareHouse` `InventoryInfo` record. AM's
open question is closed — identification is solved.

The UI still read 440, for a different reason.

### Capacity is seeded at container creation, not read at panel open

Three independent points in the same log:

1. After the write (log line 304) the user opened Private three more times
   (lines 360–383) and it still read 440. The static entry held 1000 for all of
   those opens, so the panel does **not** consult the static table when it
   opens. Capacity lives in the runtime container.
2. The housing chests work because their patch lands early: the worker's first
   pass logs `5 entries default 10 -> 1000` at line 39, immediately after
   `READY!` — before the save is loaded, therefore before the containers exist.
3. `240 base + 200 purchased expansions = 440`. Base comes from `InventoryInfo`;
   the expansions come from the save. This also independently corroborates that
   idx 7 is CampWareHouse.

AM hooked only the on-open caller (`0x4776`), which cannot fire until after the
save has loaded. The container was already built at 240 by then.

## 46. Build AN — apply before the save loads

Three changes to the AM wrapper; everything outside the cave and the two call
rel32s is byte-identical to AI.

### Worker hook restored

```text
0x1400  -> call 0x27A68   quiet entry (worker thread, ~1s then ~3s)
0x4776  -> call 0x27A73   loud  entry (on-open, game thread)
```

AL/AM had narrowed to `0x4776` to keep game calls off the mod's own thread. That
caution is what cost the feature. The worker is the same cadence the housing
expansion already relies on, and the housing patcher has walked this same
manager off-thread since AI without incident.

### `NameToKey` gated behind manager readiness (reorder)

Order is now manager guards → `NameToKey` → bucket walk:

```text
0x27AAF  mov  rax,[0x3D598] / [rax]        ; manager
0x27ACB  cmp  dword[rax+0x6c],0  je nf     ; guard
0x27AD5  cmp  dword[rax+0x68],0  je nf     ; bucket count (also protects the div)
0x27ADF  mov  [rsp+0x48],rax               ; save across the call
0x27B0D  call NameToKey                    ; only now
0x27B1D  mov  rax,[rsp+0x48] / r8d,[rax+0x68]
```

A fully-constructed `StaticInfoManager2` is the cheapest available proof that the
game finished loading its data files, which is exactly the precondition
`NameToKey`'s table needs. This is the only game function the feature calls.

### Quiet worker, loud on-open

```text
0x27A68  quiet:  sub rsp,0x68 ; mov byte[rsp+0x60],0 ; jmp +9
0x27A73  loud:   sub rsp,0x68 ; mov byte[rsp+0x60],1
0x27BDD  nf:     cmp byte[rsp+0x60],0 ; je done   ; skip the logger when quiet
```

Without this the worker would log `[PRIV] no entry` every ~3 s before data load.
The success log stays naturally one-shot because the write is idempotent.

```text
strings 0x27A20 (72B)   code 0x27A68..0x27BF9   cave ends 0x27C00 (7B spare)
output  579C1F4A4DC93AF0D3614137FA9F3582115D428AE7D505344EF39764B57B7FE2
ini     78A5B7AD98E33A7B2FD31A02E1AC4ABC301B29758EC004B35991110F4D3C7675
zip     4AAF78009DEF2C0057065A7D04DBB9E0C4BB1F7EDFA0D1B28CFC57EB2604777F
```

Verified: AN vs AI differs only at `0x1401` (3B), `0x4777` (3B), the cave
`0x27A20..0x27BF9`, and the tag byte `0x2D117`. Both landmine NOPs, the housing
path, the readiness gate, both spinlock releases, `SafeSetup`/`SafeRestore`, the
mode stub and helper are byte-identical; sections, entry point and the 589-entry
exception directory unchanged. No `+0x80` reference remains in the cave.

The INI note changed: the option must be set **before launch**, since it now
matters pre-save-load.

### Decision rule

If Private reads 1000 (or 1200 with expansions), done. If it still reads 440,
capacity is persisted in the save rather than derived at container creation, and
the static table is the wrong lever. The distinguishing diagnostic is a dump of
`arrayA[7]`'s first `0x100` bytes against `arrayA[14]` (a known housing chest):
matching vtable proves the record type, and a pointer into
`game+0x623F000..0x6241000` maps the name via §38.

## 47. Build AN result — feature works, and two corrections

Private read **1200**. `[PRIV] idx=7 slots=240 -> 1000` plus 200 purchased
expansions.

### Reload was never broken; §46's INI note was wrong

```text
238   [PRIV] idx=7 slots=240 -> 1000     <- worker pass, warehouse closed
239 INI reloaded (hotkey 0x7A)           <- F11, one line later
```

The write landed **before** the F11 press, from the quiet worker path re-reading
the INI on its ~3 s cadence, and the next panel open reflected it. So the value
is picked up mid-session and the panel resolves capacity **at open**, not at
container creation. §45's "seeded at container creation" reading was too strong:
what AM actually demonstrated is that a write landing *inside* `InitWarehousePanel`
is too late for that same open.

### The displayed total is computed, not stored

`game+0x1DDE300`:

```text
0x1DDE38E  call GetInventoryInfo(&container+0x10)    ; game+0x3AA5A0
0x1DDE3A3  movzx ecx, word [info+0x48]               ; base slots
0x1DDE3AA  add   cx,  word [container+0x1a]          ; + purchased expansions
0x1DDE3AE  mov   word [container+0x14], cx           ; = displayed total
```

Layouts:

```text
InventoryInfo   +0x48 base slots    +0x4a max slots
container       +0x10 key   +0x14 total   +0x16 expansions   +0x1a expansions
owner           +0x18 container array     +0x20 count
```

`+0x16` and `+0x1a` are both written by the "set expansion" handler, but the
incremental path at `game+0x1DD3FB9` updates only `+0x16`. `+0x1a` is the one the
total formula reads, so it is the one AO uses — and AO logs both.

The container-list walk (`owner+0x18` / `owner+0x20`, match `word[+0x10] == key`)
is identical in `game+0x84A5A0`, `game+0x5328B0` and `game+0x1DDE300`.
`game+0x1DD2C60` maps an actor to its owner (`mov rcx,rdi / call` at
`game+0x53291F`). The ASI already caches mainChar at **ASI `0x3D520`** (stored by
the `xchg` at ASI `0x591E`).

## 48. Build AO — `PrivateStorageSlots` is the exact total

`base = configured − expansions`, clamped to `1 … 2000`.

The container lookup runs **only on the on-open path**, on the game thread with
the warehouse live: `GetInventoryOwner` dereferences `actor+0x68 → +0x20 → +0x30`
and that is not a chain to walk from a background thread against a cached
pointer. The expansion count is cached in the mod's own data section, so the
worker keeps the value applied with arithmetic alone. Until that cache is
populated (`0xFFFFFFFF`) **nothing is written at all** — showing the game's own
number briefly beats showing total+expansions.

### Two new sections

`.text` had one 480-byte cave in the whole image and AN left 7 bytes of it. AO
appends its own space instead and restores the cave to zeros:

```text
.pstext  rva 0x46000  raw 0x3E800  0x1000  chars 0x60000020  code, R+X
.psdata  rva 0x47000  raw 0x3F800  0x1000  chars 0xC0000040  data, R+W
```

Split in two deliberately: a single R+W+X section trips pefile's (and AV's)
"packed executable" heuristic, and the feature needs exactly four writable bytes
(`EXP_CACHE` at `0x47000`, initialised to `-1`). Header offsets are derived from
`e_lfanew` and cross-checked against pefile before anything is written — the
first cut hardcoded `0x108` when the real value is `0xF0` and silently corrupted
the optional header, which the AI diff caught.

Code occupies `0x46090..0x4633A` of `.pstext`; `.psdata` holds one dword.

```text
output  E8D0A5BFB304895D075B8AF6FF70E825C0401B0B09BE3AC048A0121BF6424D81
ini     CB75B12959FBE8AB95014D29B0B29D498FE21366A0A53A8180947E5C7EEC9B0C
zip     D71387E3E3B744CC0E702D3CE755E0D79DEEA1B645DFE471924551812395317E
```

Verified: inside the original 256000 bytes AO differs from AI only at the header
fields (`0xF6` NumberOfSections, `0x141` SizeOfImage), the two new section
headers (`0x2E8..0x337`), the two call rel32s (`0x1401`, `0x4777`) and the tag
byte (`0x2D117`). The old cave is all zeros. Both landmine NOPs, housing path,
readiness gate, spinlock releases, mode stub and helper are byte-identical;
exception directory still 589 entries; pefile reports no new warnings.

## 49. Build AO crash — wrong actor, and the verified idiom

AO faulted on the first F4 with `PrivateStorageSlots` non-zero. The log stops
between the two lines that bracket the on-open hook (ASI `0x4776`):

```text
46   Warehouse opened (mode=0x04 sub=0x05)
47 [CAP] ...
     <no [PRIV] line, no "Cleared stale modal pointer">
```

The worker had already run the identical front half at startup (line 39,
`InventoryInfo slot patch: 5 entries`), so the new sections, the INI read,
`NameToKey`, the manager gate and the bucket walk were all fine. Only the
on-open branch differs, and the fault is inside the container lookup.

### Root cause

The mod's cached `mainChar` (ASI `0x3D520`) is **not** the actor the inventory
code wants. ASI `0xAA10` builds it as:

```text
root     = [[ASI 0x3D518]]      ; = [game+0x62C1500]
mainChar = [root + 0x48]
```

The game reaches its actor from `root+0x00`, not `root+0x48`. The idiom repeats
at `game+0xAECF8F`, `game+0xAF819B`, `game+0xB02A0C` and ~700 other sites:

```text
mov  rax, [game+0x62C1500]     ; root
mov  rcx, [rax]                ; handle holder
lea  rdx, [out]                ; 24-byte out struct
call game+0x751D20             ; resolve; returns &out
cmp  byte [out+0x10], 0        ; resolved?
mov  rax, [out+0x08]           ; the actor
```

`GetInventoryOwner` then dereferenced `[mainChar+0x68] → [+0x20] → word[+0x30]`
on an object that has none of those fields.

## 50. Build AP — the fix

1. **Actor by the game's own sequence** (above) instead of the mod's mainChar.
2. **Owner inlined**: `[[actor+0x68]+0xb8]`, which is what
   `GetInventoryOwner`'s common path returns and what the game computes inline
   at `game+0x532903`. That removes both nested calls, including the type lookup
   at `game+0x312740`. One game call (`game+0x751D20`) remains in the feature.
3. **Every dereference guarded** with the test the mod already uses on its own
   globals at ASI `0xAA58`, extended with a user-mode range check:

```text
cmp rax, 0x10000 ; jbe bail
mov r11, rax ; shr r11, 47 ; jnz bail
```

   The scratch copy matters: the first cut shifted `rax` itself and then
   compared the *shift result* against `0x10000`, so every guard would have
   bailed and the chain could never have succeeded. Caught in disassembly
   review before shipping.

4. **Failure is now diagnosable**: if the walk finds no container the on-open
   path logs `  [PRIV] no container - root=%llX actor=%llX owner=%llX n=%u`,
   naming the step that broke instead of faulting.

Frame grew `0x78 → 0xB8` for the out struct (`[rsp+0x70..0x87]`) and the three
chain slots (`0x88`/`0x90`/`0x98`); `0xB8 ≡ 8 (mod 16)` keeps every call
16-byte aligned. Code occupies `0x460A0..0x46498` of `.pstext`.

```text
output  2F5F93C9C6F5B0DF5E0BA4284FD7E00CE9C4806CCFC3BBB56DFE68B94157D0A1
zip     36F3C415E760B81994F529A298EEC8F51F3702637A6EB41BE0F5FC65F621C9A6
```

Verified: no `0x1DD2C60` immediate anywhere in `.pstext`; diffs against AI
inside the original image are still only the two header fields, the two section
headers, the two call rel32s and the tag byte; old cave zeroed; landmine NOPs,
housing path, readiness gate, spinlocks, mode stub and helper byte-identical;
589 exception entries; no pefile warnings beyond the two pre-existing unwind
ones.

Note for any future build: the crash is gated behind the INI option —
`PrivateStorageSlots=0` short-circuits before the manager lookup — so the option
doubles as the rollback switch.

## 51. Build AP result — chain works, wrong container, and an unexplained delta

No crash. The actor/owner chain resolved and the walk ran:

```text
78   [PRIV] exp=0 (0x16=0 tot=240) base 240 -> 1000
```

Two things follow.

**The matched container is not the player's warehouse.** It reports
`+0x1a = 0` and `+0x14 = 240`, which is self-consistent under
`total = base + expansions` (240 + 0), so it is a genuine record — but it cannot
back a 440 display. Either the owner holds more than one CampWareHouse-keyed
container and the first-match walk took the wrong one, or the resolved actor is
not the one holding the player's storage.

**The base no longer moves the display.** After line 78 the static base is 1000
for every subsequent open, yet Private still read 440. AN wrote the identical
value to the identical field and produced 1200. The only difference is where the
write landed: AN's came from the worker with the panel closed, AP's from the
on-open hook inside `InitWarehousePanel`. Unresolved, and not safe to build on.

Also confirmed this round: the game never destroys the out struct from
`game+0x751D20` (`game+0xAECFA3` onward just reads it), so the resolver call
leaks nothing.

## 52. Build AQ — instrumentation plus a manual override

Behaviour unchanged from AP; two additions, both on the loud path only.

**Status line, unconditional** — AP logged only on change, so a standing write
was invisible on later opens:

```text
  [PRIV] base=%u max=%u exp=%d owner=%llX n=%u
```

`max` is `InventoryInfo+0x4a`, worth watching because the expansion path clamps
against it.

**Container dump** — every entry in the owner's list:

```text
  [PRIV]   c%u key=%u tot=%u e16=%u e1a=%u
```

The walk cursor moved from `r8` into `[rsp+0xA0/0xA8]` and the container into
`[rsp+0xB0]`, because the logger clobbers every volatile register. First-match
semantics for the cached expansion are preserved with a `[rsp+0x64]` flag, so
only the dump changed; the walk continues past the first hit purely to enumerate.

**`PrivateStorageExpansions`** — new INI key, default `-1`. Read with the same
`GetPrivateProfileIntA` plumbing into `[rsp+0x34]`; any value `0..2000`
overrides the detected one:

```text
mov eax,[rsp+0x34] ; cmp eax,0x7d0 ; jbe use  ; -1 falls through
mov eax,[EXP_CACHE]
```

This decouples the arithmetic from the container lookup, so an exact total is
reachable now regardless of whether auto-detection lands.

```text
output  D3F83F435E6613D432E8E3CC786213B620E59E6B2862135841B190CC4526D80B
zip     4540C435D093D2B270C9EA56DD006E1B59BD653482DA6674553EE4EAAFFFCFE1
.pstext used 0x60D of 0x1000
```

Shipped with `PrivateStorageSlots=800`, `PrivateStorageExpansions=-1`. Decision
rule on what Private then reads: **1000** means `display = base + 200` still
holds and the override is the delivery mechanism; **800** means the expansion
term is gone; **440** means the base no longer drives the display at all, which
would make §51's second finding the real problem rather than the container.

Verification unchanged from AP: one game call (`0x751D20`), no `0x1DD2C60`, every
deref guarded, frame `0xB8`, old cave zeroed, 589 exception entries, diffs
against AI confined to the two header fields, two section headers, two call
rel32s and the tag byte, no new pefile warnings.

## 53. Build AQ result — `container+0x10` is the ARRAY INDEX, not the key

The dump settled it. 18 containers, and Private Storage is right there:

```text
  [PRIV]   c5 key=7  tot=440 e16=200 e1a=200     <- Private Storage
  [PRIV]   c6 key=8  tot=240 e16=0   e1a=0       <- what AP/AQ matched
```

Checking every row against AJ's array-A dump
(`20 50 20 20 300 1 1 240 240 300 50 300 240 5 1000x5 50`):

```text
c+0x10   arrayA[that]   tot   tot-e1a
  0        20            20     20     match
  1        50           320    320     NO  (character inventory, own mechanic)
  2..19    ...                          match  (16 of 16)
```

17 of 18 satisfy `arrayA[c+0x10] == tot - expansions` exactly, and the game
confirms it directly — `GetInventoryInfo` at `game+0x3AA5B2` does
`movzx edi, word[rcx]` on `&container+0x10` and uses it as the array index:

```text
0x3AA5B2  movzx edi, word [rcx]        ; rcx = &container+0x10
0x3AA5BC  cmp   edi, [mgr+0x08]        ; bounds against the entry count
0x3AA5CD  mov   rax, [mgr+0x58]        ; array A
0x3AA5D1  mov   rax, [r14+rax]         ; index*8
```

So `container+0x10` is the **row number**, while `NameToKey` returns the
**key**. For CampWareHouse those are 7 and 8 — adjacent, and both rows happen to
hold base 240, which is why the wrong pick looked plausible. AN's bucket lookup
had the right index (`idx=7`) all along; only the container match used the wrong
number.

Private Storage: row 7, base 240, expansions 200, total 440.

### And why base changes stayed invisible

Every dump in the AQ log shows `c5 tot=440` even with `base=800` in the static
table. `container+0x14` is a **cached** total; the game recomputes it only when
expansions change (`game+0x1DDE300`). That resolves §51's second finding: AQ's
base write was correct and simply never reached the screen.

## 54. Build AR — match on the index, and fix both numbers

```text
0x461xx  mov [rsp+0x4C], edx      ; keep the bucket's array index
walk:    cmp word[c+0x10], [rsp+0x4C]    ; was [rsp+0x48] (the key)
match:   mov [rsp+0xB8], rdx      ; keep the container
apply:   word[info+0x48] = total - expansions      ; future recomputes correct
tfix:    word[cont+0x14] = total                   ; the screen, now
```

Both writes agree by construction: `base + expansions == total`. For this save,
base 240 → 800 and total 440 → 1000.

The per-container dump is removed; the status line stays. Frame grew
`0xB8 → 0xC8` for the matched-container slot. `.pstext` uses `0x62B` of `0x1000`.

`container+0x14` is the first write this project makes to character-side rather
than static data. It is a derived field the game rewrites on any expansion
change, so it is self-correcting, but it is the reason the AR README asks for a
save backup and an explicit revert check.

```text
output  D6B5FFA34F086344EBC1A6677A56F7229047E9E91BD7B7B9E5F86F784ACA69EB
zip     9B6DB0FA3DBB9A52AB8D0B3209616BA3495D0176003E72A7752BC84EB814D230
```

Verification unchanged: one game call, no `0x1DD2C60`, every deref guarded,
old cave zeroed, 589 exception entries, diffs against AI confined to the two
header fields, two section headers, two call rel32s and the tag byte, no new
pefile warnings.

## 55. Build AR result — feature works; 0 did not revert

```text
78   [PRIV] base=240 max=1000 exp=200 owner=4F757851E00 n=18
79   [PRIV] exp=200 (0x16=200 tot=440) base 240 -> 800
80   [PRIV] container total 440 -> 1000
```

Right container (index 7), right expansion count (200), and Private reads 1000.
The index-vs-key fix from §53 was correct.

The gap: `PrivateStorageSlots=0` was implemented as an early `return` before any
lookup, so it left both prior writes standing. Log line 327 reloads the INI, the
open at 328 emits no `[PRIV]` lines at all, and the panel keeps showing 1000.
Not a new mechanism — a missing one.

## 56. Build AS — 0 restores instead of returning

New `.psdata` dword `BASE_ORIG` at `PD_RVA+4`, initialised to `-1` and captured
the first time the wrapper reads `word[info+0x48]`, before any write of ours:

```text
rip 8B 15 BASE_ORIG ; cmp edx,-1 ; jne +6 ; rip 89 05 BASE_ORIG
```

Placed immediately after the `0 < base <= 2000` plausibility gate, where `edx`
is dead (the bucket index is already spilled to `[rsp+0x4C]`). The static table
is rebuilt from the game's data files every launch, so the first read of any
process is always the game's own number.

`apply` now branches on the configured value:

```text
exp = manual override, else EXP_CACHE          -> [rsp+0x3C]
if configured != 0:  targetTotal = configured
                     targetBase  = configured - exp   (clamped 1..2000)
else:                targetBase  = BASE_ORIG          (bail if never captured)
                     targetTotal = BASE_ORIG + exp
write base  if different
write total if different and the container is known
```

Both writes remain compare-then-write, so a save that never enabled the option
is untouched even though the lookup now runs when the option is 0. The removed
early-exit is the only behavioural change for the disabled case.

`PS_DIRTY` was drafted and dropped: with `BASE_ORIG` captured every session the
restore is unconditional, so the flag had no reader.

```text
output  A7F7D38169250E1FE1D570613369022CA4933721943E5C113687B7F9D0AA51EE
zip     CFEBD7A842839E07680CAE321F81C376DEDBFFF4A73B646B9F6E40210AC125D2
.pstext used 0x672 of 0x1000 ; frame still 0xC8 ; targetTotal at [rsp+0xC0]
```

Verification unchanged: one game call, no `0x1DD2C60`, every deref guarded, old
cave zeroed, 589 exception entries, diffs against AI confined to the two header
fields, two section headers, two call rel32s and the tag byte, no new pefile
warnings.

**Open question for the next test:** whether `container+0x14` reaches the save
file. Set 0, restart, and read Private. 440 means the write is session-only and
the feature is clean; anything else means the game persists it and the restore
needs to run on load rather than on panel open.
