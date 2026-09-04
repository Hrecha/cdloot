"""CD 1.18.2 'W' modestate patch.

Rebased directly on the pristine Nexus v1.5.10 ASI. Restores the original open
mechanism -- drive the game's mode state machine into the `store` sub-mode so
the panel-manager mounts the warehouse view -- using the 1.18.2 field layout.

See FINDINGS-1.18.2-MODESTATE.md for the derivation of every constant here.

Game side (RVAs on 0x140000000):
    modeObj = *(menuMgr + 0x1158)
    modeObj + 0x18  mode        (4 == in-game)
    modeObj + 0x19  submode     (5 == dialog/store, 15/16 == gameplay)
    modeObj + 0x21  flags[7]        mode request, first non-zero index wins
    modeObj + 0x28  subtypes[17]    submode request, first non-zero index wins
    modeObj + 0x4B  dirty       must be set or ModeSwitch skips the transition
"""
from __future__ import annotations

import hashlib
import struct
import sys
from pathlib import Path

import pefile

EXPECTED_SHA256 = "4f514298b2bc5db7e804b0166ad2269bae97a414f7dae425a0f736cda7f56f3e"

# --- ASI globals -----------------------------------------------------------
MODE_OFF = 0x3C01C
SUB_OFF = 0x3C020
FLAGS_OFF = 0x3C024
SUBTYPES_OFF = 0x3C028
STORE_SUB_INDEX = 0x3C02C
MODE_ANCHOR = 0x3D4B0
SAVED_FLAGS = 0x4061C      # 7 bytes
SAVED_SUBTYPES = 0x40628   # 15 bytes

# --- ASI code --------------------------------------------------------------
GET_MENU_MGR = 0xAA30      # kept for reference; helper inlines its guards
GAME_GLOBAL_SLOT = 0x3D518 # holds the address of the game's global object slot
CAVE = 0x1460              # dead legacy layout resolver, 0x1460..0x15A9
CAVE_END = 0x15A9
HELPER = 0x14C0            # get_mode_obj lives here, inside the same cave
TELEMETRY = 0x1500         # W2 mode/sub-mode probe, same cave
DISARM = 0x1570            # Z disarm-then-init stub, same cave
ARM_FLAG = 0x408F4         # input thread arms this and re-posts 0x65B
INIT_PANEL = 0x4730        # InitWarehousePanel
CAPTURED_HANDLER = 0x3D528 # the mod's captured Warehouse2 controller
UI_CHILD_VEC = 0x168       # controller+0x168: the child vector the game walks
LOGGER = 0x6DE0            # the mod's printf-style log sink
FMT_OPENED = 0x2B1D8       # "  Warehouse opened (mode=0x%02X sub=0x%02X)"
FMT_DEFERRED = 0x2BF00     # "  Deferred init: handler captured, initializing panel"
FMT_BLOCKING = 0x29488     # repurposed by AA into the packet-type trace line
FMT_SLOTPATCH = 0x2D058    # "InventoryInfo slot patch: %d entries default 10 -> 1000"
INVMGR_PTR = 0x3D598       # holds the address of the game's inventory-manager global
SLOT_PATCHER = 0x8720      # the InventoryInfo slot patcher
SECTION_STR = 0x29020      # "Settings"
INI_PATH = 0x3D3A0         # the mod's MAX_PATH INI path buffer
GPP_INT = 0x28100          # IAT: GetPrivateProfileIntA
GAME_BASE = 0x3D350        # game image base
CAMP_NAME = 0x4FC0560      # game rva: "CampWareHouse" literal
NAME_TO_KEY = 0x1E141D0    # game rva: bool NameToKey(const char*, uint16_t*)
RESOLVE_ACTOR = 0x751D20   # game rva: resolve(handleHolder, &out24)
MAINCHAR_GLOBAL = 0x62C1500  # game rva: root; root+0x00 is the handle
PS_RVA = 0x46000           # .pstext: appended code section
PS_RAW = 0x3E800           # its file offset (the image ends here)
PS_SIZE = 0x1000
PD_RVA = 0x47000           # .psdata: its read/write companion
PD_RAW = 0x3F800
PD_SIZE = 0x1000
EXP_CACHE = PD_RVA         # dword: cached expansion count, -1 = unknown
BASE_ORIG = PD_RVA + 4     # dword: the game's own base, captured untouched

# --- 1.18.2 field layout ---------------------------------------------------
L_MODE, L_SUB, L_FLAGS, L_SUBTYPES = 0x18, 0x19, 0x21, 0x28
L_DIRTY = 0x4B
MODE_OBJ_IN_MENUMGR = 0x1158


def rel32(insn_rva: int, insn_size: int, target_rva: int) -> bytes:
    return struct.pack("<i", target_rva - (insn_rva + insn_size))


class Asm:
    """Tiny position-aware emitter so rip-relative displacements stay correct."""

    def __init__(self, rva: int):
        self.start = rva
        self.buf = bytearray()

    @property
    def rva(self) -> int:
        return self.start + len(self.buf)

    def raw(self, *chunks: bytes) -> "Asm":
        for chunk in chunks:
            self.buf += chunk
        return self

    def riprel(self, opcode: bytes, target: int, tail: bytes = b"") -> "Asm":
        size = len(opcode) + 4 + len(tail)
        self.buf += opcode + rel32(self.rva, size, target) + tail
        return self

    def call(self, target: int) -> "Asm":
        self.buf += b"\xE8" + rel32(self.rva, 5, target)
        return self

    def jmp32(self, target: int) -> "Asm":
        self.buf += b"\xE9" + rel32(self.rva, 5, target)
        return self

    def jmp8(self, target: int) -> "Asm":
        delta = target - (self.rva + 2)
        if not -128 <= delta <= 127:
            raise ValueError(f"rel8 out of range: {delta}")
        self.buf += b"\xEB" + struct.pack("<b", delta)
        return self

    def pad_to(self, size: int) -> "Asm":
        if len(self.buf) > size:
            raise ValueError(f"overflow: {len(self.buf)} > {size}")
        self.buf += b"\x90" * (size - len(self.buf))
        return self

    def bytes(self) -> bytes:
        return bytes(self.buf)


def patch(pe: pefile.PE, image: bytearray, rva: int, expected: bytes, new: bytes) -> None:
    if len(expected) != len(new):
        raise ValueError(f"size mismatch at RVA 0x{rva:X}: {len(expected)} vs {len(new)}")
    off = pe.get_offset_from_rva(rva)
    actual = bytes(image[off:off + len(expected)])
    if actual != expected:
        raise RuntimeError(
            f"unexpected bytes at RVA 0x{rva:X}\n"
            f"  expected {expected.hex(' ')}\n"
            f"  got      {actual.hex(' ')}"
        )
    image[off:off + len(new)] = new


def main() -> None:
    source, destination = Path(sys.argv[1]), Path(sys.argv[2])
    image = bytearray(source.read_bytes())
    digest = hashlib.sha256(image).hexdigest()
    if digest != EXPECTED_SHA256:
        raise RuntimeError(f"unsupported input ASI SHA-256: {digest}")
    pe = pefile.PE(data=image)

    # ------------------------------------------------------------------ (A)
    # Replace the legacy 0xC00-0xCFF layout scanner with a stub publishing the
    # real 1.18.2 offsets. MODE_ANCHOR must stay non-zero or the caller skips
    # StoreSubIndex derivation.
    asm = Asm(CAVE)
    asm.riprel(b"\x48\x89\x15", MODE_ANCHOR)                       # mov [MODE_ANCHOR],rdx
    for glob, value in ((MODE_OFF, L_MODE), (SUB_OFF, L_SUB),
                        (FLAGS_OFF, L_FLAGS), (SUBTYPES_OFF, L_SUBTYPES)):
        asm.riprel(b"\xC7\x05", glob, struct.pack("<I", value))    # mov dword [g],imm32
    asm.raw(b"\xB0\x01", b"\xC3")                                  # mov al,1 ; ret
    stub = asm.bytes()
    off = pe.get_offset_from_rva(CAVE)
    if bytes(image[off:off + 6]) != b"\x48\x89\x5c\x24\x08\x57":
        raise RuntimeError("legacy resolver prolog was not found")
    if CAVE + len(stub) > HELPER:
        raise RuntimeError("stub collides with helper")
    image[off:off + len(stub)] = stub

    # ------------------------------------------------------------------ (B)
    # get_mode_obj -> rax = *(menuMgr + 0x1158), ZF reflects rax == 0.
    #
    # Deliberately a LEAF function: it makes no call and touches no stack, so
    # it needs no unwind info. It lives inside the dead resolver's .pdata range,
    # whose unwind data describes the original prologue -- an exception
    # unwinding through a non-leaf helper here would be misdescribed. As a leaf
    # the return address is at [rsp] and the unwinder is correct either way.
    #
    # The pointer walk and its guards mirror the mod's own GetMenuMgr (0xAA30).
    hlp = Asm(HELPER)
    fails = []
    hlp.riprel(b"\x48\x8B\x05", GAME_GLOBAL_SLOT)                  # mov rax,[g_slotHolder]
    hlp.raw(b"\x48\x85\xC0")                                       # test rax,rax
    fails.append(hlp.rva)
    hlp.raw(b"\x74\x00")                                           # je fail
    hlp.raw(b"\x48\x8B\x00")                                       # mov rax,[rax]
    for deref in (0x90, MODE_OBJ_IN_MENUMGR):
        hlp.raw(b"\x48\x3D\x00\x00\x01\x00")                       # cmp rax,0x10000
        fails.append(hlp.rva)
        hlp.raw(b"\x76\x00")                                       # jbe fail
        hlp.raw(b"\x48\x8B\x80" + struct.pack("<I", deref))        # mov rax,[rax+deref]
    hlp.raw(b"\x48\x3D\x00\x00\x01\x00")                           # cmp rax,0x10000
    fails.append(hlp.rva)
    hlp.raw(b"\x76\x00")                                           # jbe fail
    hlp.raw(b"\x48\x85\xC0")                                       # test rax,rax  -> ZF=0
    hlp.raw(b"\xC3")
    fail_target = hlp.rva
    hlp.raw(b"\x31\xC0")                                           # xor eax,eax
    hlp.raw(b"\x48\x85\xC0")                                       # test rax,rax  -> ZF=1
    hlp.raw(b"\xC3")
    helper = bytearray(hlp.bytes())
    for site in fails:
        delta = fail_target - (site + 2)
        if not 0 <= delta <= 127:
            raise ValueError(f"helper rel8 out of range at 0x{site:X}: {delta}")
        helper[site - HELPER + 1] = delta
    if HELPER + len(helper) > CAVE_END:
        raise RuntimeError("helper overruns the cave")
    hoff = pe.get_offset_from_rva(HELPER)
    image[hoff:hoff + len(helper)] = helper

    # --------------------------------------------------------------- (C/D)
    # Re-point the two mode-base loads from g_mainChar to modeObj.
    for rva, expected_hex in (
        (0xB447, "33 c0 f0 48 0f b1 3d ce 20 03 00"),   # hotkey gate
        (0xC4FE, "33 c0 f0 48 0f b1 1d 17 10 03 00"),   # cleanup path
    ):
        expected = bytes.fromhex(expected_hex)
        patch(pe, image, rva, expected,
              Asm(rva).call(HELPER).pad_to(len(expected)).bytes())

    # ------------------------------------------------------------------ (E)
    # Both Safe* functions gate their offsets on (off - 0xC00) <= 0x200, which
    # rejects the real 1.18.2 offsets. Rebase the check at zero: off <= 0x200.
    for rva in (0xB006, 0xAEF6):
        patch(pe, image, rva, bytes.fromhex("8d 82 00 f4 ff ff"),
              b"\x8D\x82\x00\x00\x00\x00")
    for rva in (0xB01E, 0xAF0E):
        patch(pe, image, rva, bytes.fromhex("41 8d 80 00 f4 ff ff"),
              b"\x41\x8D\x80\x00\x00\x00\x00")

    # ----------------------------------------------------------------- (F1)
    # SafeSetupModeForWarehouse tail: same writes, compacted to free five bytes
    # for the mandatory dirty flag.
    tail_rva = 0xB0DA
    tail = Asm(tail_rva)
    tail.riprel(b"\x8B\x05", SUBTYPES_OFF)        # mov eax,[SUBTYPES_OFF]   eax = 0x28
    tail.raw(b"\x8D\x48\xFD")                     # lea ecx,[rax-3]          FLAGS_OFF+4
    tail.raw(b"\x42\xC6\x04\x09\x01")             # mov byte [rcx+r9],1      flags[4]=1 -> mode 4
    tail.riprel(b"\x03\x05", STORE_SUB_INDEX)     # add eax,[StoreSubIndex]
    tail.raw(b"\x42\xC6\x04\x08\x01")             # mov byte [rax+r9],1      subtypes[5]=1 -> store
    tail.raw(b"\x41\xC6\x41" + bytes([L_DIRTY]) + b"\x01")   # mov byte [r9+0x4B],1
    tail.jmp8(0xB115)
    expected_tail = bytes.fromhex(
        "8b 05 44 0f 03 00 83 c0 04 42 c6 04 08 01 8b 0d 3a 0f 03 00 "
        "03 0d 38 0f 03 00 42 c6 04 09 01 eb 1a"
    )
    patch(pe, image, tail_rva, expected_tail, tail.pad_to(len(expected_tail)).bytes())

    # ----------------------------------------------------------------- (F2)
    # SafeRestoreMode body: restore the saved arrays with wider moves, then set
    # the dirty flag so the game resolves back to the gameplay sub-mode.
    body_rva = 0xAF4E
    body = Asm(body_rva)
    body.riprel(b"\x8B\x0D", FLAGS_OFF)           # mov ecx,[FLAGS_OFF]
    body.raw(b"\x49\x03\xC9")                     # add rcx,r9          rcx = &flags[0]
    body.riprel(b"\x8B\x05", SAVED_FLAGS)         # mov eax,[saved+0]
    body.raw(b"\x89\x01")                         # mov [rcx],eax
    body.riprel(b"\x8B\x05", SAVED_FLAGS + 3)     # mov eax,[saved+3]   overlapping tail
    body.raw(b"\x89\x41\x03")                     # mov [rcx+3],eax
    body.riprel(b"\x48\x8B\x05", SAVED_SUBTYPES)  # mov rax,[savedsub+0]
    body.raw(b"\x48\x89\x41\x07")                 # mov [rcx+7],rax     subtypes = flags+7
    body.riprel(b"\x48\x8B\x05", SAVED_SUBTYPES + 7)
    body.raw(b"\x48\x89\x41\x0E")                 # mov [rcx+0xE],rax
    body.raw(b"\x41\xC6\x41" + bytes([L_DIRTY]) + b"\x01")   # mov byte [r9+0x4B],1
    body.jmp8(0xAFC6)
    expected_body = bytes.fromhex(
        "8b 0d d0 10 03 00 8b 05 c2 56 03 00 42 89 04 09 0f b7 05 bb 56 03 00 "
        "66 42 89 44 09 04 0f b6 05 b0 56 03 00 42 88 44 09 06 8b 0d ab 10 03 00 "
        "49 03 c9 f2 0f 10 05 a0 56 03 00 f2 0f 11 01 8b 05 9e 56 03 00 89 41 08 "
        "0f b7 05 98 56 03 00 66 89 41 0c 0f b6 05 8f 56 03 00 88 41 0e eb 1a"
    )
    patch(pe, image, body_rva, expected_body, body.pad_to(len(expected_body)).bytes())

    # ------------------------------------------------------------------ (G)
    # 1.18.2 menu-manager backlink: +0x11C0 now holds a different valid object,
    # so the old equality check rejects the right manager.
    patch(pe, image, 0xAA8C, bytes.fromhex("4d 3b c1 75 09"), b"\x90" * 5)

    # ================================================================== W2
    # Build W proved the mode transition works (it logged the true live
    # mode=0x04 sub=0x10), then froze. The mod's legacy code was written for a
    # world where no view ever mounts, so it fakes the whole panel by hand.
    # Now that a real mount happens, those writes race the game's own UI script
    # against the same controller. Stop fighting the mount.

    # ------------------------------------------------------------------ (I)
    # ViewMount (0xB880) sets the menu-layer request byte to 1. game+0x7DD6B0
    # answers any request other than 3 by setting modeObj+0x2C -- subtypes[4],
    # which is `alert, cinema, subtitle`. ModeSwitch takes the FIRST non-zero
    # index, so cinema (4) would beat store (5). The menu-layer path and the
    # store path are alternatives; asking for both is the v1.5.5 bug restated.
    #
    # Leave the layer alone entirely. Its idle request value is 3, which is the
    # branch that *clears* cinema for us. Nothing is set on open, so nothing
    # needs restoring on close -- the v1.5.6 leaked-layer failure mode goes away
    # by construction.
    for rva, expected_hex in (
        (0xB8DE, "66 c7 80 5e 10 00 00 01 01"),  # ViewMount   current=1, request=1
        (0xB950, "c6 80 5f 10 00 00 03"),        # ViewUnmount request=3
        (0xB965, "66 c7 80 5e 10 00 00 04 03"),  # ViewUnmount current=4, request=3
    ):
        expected = bytes.fromhex(expected_hex)
        patch(pe, image, rva, expected, b"\x90" * len(expected))

    # ------------------------------------------------------------------ (J)
    # InitWarehousePanel (0x4730) is left BYTE-IDENTICAL to the original.
    #
    # Earlier builds NOP'd parts of it -- the prepare packet, the 0x0E command,
    # the SetInventory calls, the modal-pointer clear -- on the theory that a
    # native mount made them redundant. That was wrong twice over. It is a
    # coherent sequence, and running half of it corrupted the panel rather than
    # configuring it. And the routine was always designed for a mounted view:
    # even in the working era the flow was set store sub-mode -> game mounts ->
    # init configures. The real bug was never this routine, it was the two
    # re-entrant routes that called it from inside the game's own mount.
    #
    # The per-panel table at 0x3C058 is why it cannot simply be skipped: it
    # carries the container string for each hotkey, and applying it is the only
    # thing that distinguishes F4 from F5-F9.
    #     panel[0] CampWareHouse              panel[3] Housing_Refrigerator
    #     panel[1] Housing_GatheredMaterials  panel[4] Housing_Symbol
    #     panel[2] Housing_Dresser            panel[5] Housing_Collecting

    # ------------------------------------------------------------------ (K)
    # Telemetry: re-read mode/sub-mode one frame after the request, from the
    # deferred-init path, so the log says whether the game actually reached the
    # store sub-mode. Reuses the existing format string at 0x2B1D8, so no new
    # data is needed.
    #
    # This routine is a LEAF that TAIL-JUMPS to the logger: no call, no stack
    # frame. It lives inside the dead resolver's .pdata range, whose unwind info
    # describes a different prologue, and a leaf is the one shape that unwinds
    # correctly regardless. The tail jump also lets the logger reuse the
    # caller's shadow space.
    tel = Asm(TELEMETRY)
    fails = []
    tel.riprel(b"\x48\x8B\x05", GAME_GLOBAL_SLOT)                  # mov rax,[g_slotHolder]
    tel.raw(b"\x48\x85\xC0")                                       # test rax,rax
    fails.append(tel.rva)
    tel.raw(b"\x74\x00")                                           # je fallback
    tel.raw(b"\x48\x8B\x00")                                       # mov rax,[rax]
    for deref in (0x90, MODE_OBJ_IN_MENUMGR):
        tel.raw(b"\x48\x3D\x00\x00\x01\x00")                       # cmp rax,0x10000
        fails.append(tel.rva)
        tel.raw(b"\x76\x00")                                       # jbe fallback
        tel.raw(b"\x48\x8B\x80" + struct.pack("<I", deref))        # mov rax,[rax+deref]
    tel.raw(b"\x48\x3D\x00\x00\x01\x00")                           # cmp rax,0x10000
    fails.append(tel.rva)
    tel.raw(b"\x76\x00")                                           # jbe fallback
    tel.riprel(b"\x8B\x0D", MODE_OFF)                              # mov ecx,[MODE_OFF]
    tel.raw(b"\x0F\xB6\x14\x01")                                   # movzx edx,byte [rcx+rax]
    tel.riprel(b"\x8B\x0D", SUB_OFF)                               # mov ecx,[SUB_OFF]
    tel.raw(b"\x44\x0F\xB6\x04\x01")                               # movzx r8d,byte [rcx+rax]
    tel.riprel(b"\x48\x8D\x0D", FMT_OPENED)                        # lea rcx,[fmt]
    tel.jmp32(LOGGER)                                              # tail call
    fallback = tel.rva
    tel.riprel(b"\x48\x8D\x0D", FMT_DEFERRED)                      # lea rcx,[original string]
    tel.jmp32(LOGGER)
    telemetry = bytearray(tel.bytes())
    for site in fails:
        delta = fallback - (site + 2)
        if not 0 <= delta <= 127:
            raise ValueError(f"telemetry rel8 out of range at 0x{site:X}: {delta}")
        telemetry[site - TELEMETRY + 1] = delta
    if TELEMETRY + len(telemetry) > CAVE_END:
        raise RuntimeError("telemetry overruns the cave")
    toff = pe.get_offset_from_rva(TELEMETRY)
    image[toff:toff + len(telemetry)] = telemetry

    # Redirect the two "about to init the panel" log lines to the telemetry
    # stub. There are two independent routes into InitWarehousePanel -- the
    # deferred one and the CanShow first-open one -- and which fires depends on
    # whether the controller was already captured. Build X instrumented only the
    # deferred route and the run took the other one, so instrument both.
    for site, expected_hex in (
        (0x42E7, "48 8d 0d 12 7c 02 00 e8 ed 2a 00 00"),  # "Deferred init: handler captured"
        (0xC345, "48 8d 0d d4 d9 01 00 e8 8f aa ff ff"),  # "CanShow: running ... inline"
    ):
        expected = bytes.fromhex(expected_hex)
        patch(pe, image, site, expected,
              Asm(site).call(TELEMETRY).pad_to(len(expected)).bytes())

    # =================================================================== Z
    # Build Y proved the native mount works: sub-mode 0x05, no crash, ESC clean.
    # What it lacked was configuration -- the store tag mounts the whole
    # store-tagged family (Camp Provisions donation, Trade Goods, Mount
    # Inventory, Hold) with package defaults. InitWarehousePanel is what turns
    # that into Private Storage, so it has to run after all. The problem was
    # never what it does, only when.
    #
    # Three routes reach it, two of them re-entrant:
    #     0x4319  posted message 0x65B, handled on the message pump   SAFE
    #     0xBE3F  Handler hook   -- inside the game's packet dispatch
    #     0xC375  CanShow hook   -- inside the game's mount call      (crashed X)

    # ------------------------------------------------------------------ (L)
    # Make both inline routes decline. Critically they must NOT clear the arm
    # flag 0x408F4 on the way out: the input thread (0x5CC0) re-posts 0x65B
    # while armed, and that retry is what eventually lands the init on the
    # message pump once the controller is captured and the mount is past
    # CanShow. Both arm-flag clears (0xBE09, 0xC33F) sit inside the skipped
    # region, and both jumps land exactly where the original branch went.
    patch(pe, image, 0xBE05, bytes.fromhex("74 3d"), b"\xEB\x3D")
    patch(pe, image, 0xC337, bytes.fromhex("0f 84 f3 00 00 00"),
          Asm(0xC337).jmp32(0xC430).pad_to(6).bytes())

    # ------------------------------------------------------------------ (M)
    # Readiness gate (AD).
    #
    # Every subset of the init crashed -- full, minus the packet sends, minus
    # the teardown writes -- and the Sentry crash event pins the fault at
    # game+0xA0318F4, on the FIRST dereference of `this` in a UI helper's
    # prologue: `mov rcx,[rcx+0x168]`. So the game was handed a garbage control
    # pointer and tripped over it while walking children, after our init had
    # already returned.
    #
    # What explains 'every subset fails' is not one bad write. It is that the
    # init runs while the game is still building the panel. Moving it to the
    # message pump fixed re-entrancy, but the pump still runs inside the frame;
    # the mount completes over several.
    #
    # So gate on the panel being real before touching it, using the very field
    # whose staleness produced the fault. Returning without disarming means
    # 'try again next tick' -- the input thread re-posts 0x65B while 0x408F4
    # stays armed, bounded by its own 1800-tick budget.
    #
    # Leaf plus tail jump, as with the other stubs: no frame, so the cave's
    # inherited unwind info is never consulted, and rcx/rdx pass through.
    gate = Asm(DISARM)
    waits = []
    gate.riprel(bytes.fromhex("48 8b 05"), CAPTURED_HANDLER)   # mov rax,[captured]
    gate.raw(bytes.fromhex("48 85 c0"))                        # test rax,rax
    waits.append(gate.rva)
    gate.raw(bytes.fromhex("74 00"))                           # jz not_ready
    gate.raw(bytes.fromhex("48 8b 80") + struct.pack("<I", UI_CHILD_VEC))
    gate.raw(bytes.fromhex("48 85 c0"))                        # test rax,rax
    waits.append(gate.rva)
    gate.raw(bytes.fromhex("74 00"))                           # jz not_ready
    gate.raw(bytes.fromhex("31 c0"))                           # xor eax,eax
    gate.riprel(bytes.fromhex("87 05"), ARM_FLAG)              # xchg [ARM_FLAG],eax
    gate.jmp32(INIT_PANEL)                                     # commit
    not_ready = gate.rva
    gate.raw(bytes.fromhex("c3"))                              # ret, still armed
    gate_bytes = bytearray(gate.bytes())
    for site in waits:
        delta = not_ready - (site + 2)
        if not 0 <= delta <= 127:
            raise ValueError(f"gate rel8 out of range at 0x{site:X}: {delta}")
        gate_bytes[site - DISARM + 1] = delta
    if TELEMETRY + len(telemetry) > DISARM or DISARM + len(gate_bytes) > CAVE_END:
        raise RuntimeError("readiness gate does not fit the cave")
    doff = pe.get_offset_from_rva(DISARM)
    image[doff:doff + len(gate_bytes)] = gate_bytes
    patch(pe, image, 0x4319, bytes.fromhex("e8 12 04 00 00"),
          Asm(0x4319).call(DISARM).bytes())

    # ------------------------------------------------------------------ (N)
    # "Handler: blocking %u sub-commands" -- the mod zeroes the sub-command
    # count on the game's own type-0x15 packets while an init is pending. That
    # was deliberate when the mod faked the whole panel; now it can only destroy
    # the very commands that configure it.
    patch(pe, image, 0xBE6D, bytes.fromhex("89 7e 10"), b"\x90" * 3)

    # ================================================================== AA
    # Four builds have now crashed on variations of "which parts of the mod's
    # init to run". Stop guessing and capture the packet trace instead.
    #
    # AA is deliberately diagnostic and non-crashing: revert to Y's behaviour
    # (init neutered -- the one configuration that reliably survives) and open
    # the gates on the handler hook's existing packet inspection.

    # ------------------------------------------------------------------ (O)
    # ROOT CAUSE (AG): the mod plants two landmines in fields the game owns.
    #
    # Both are 'clean up leftovers from the previous mod-driven open' steps.
    # They made sense when the mod built the panel itself. On 1.18.2 the game
    # builds it, so these now overwrite live state with values the game then
    # dereferences without checking.
    #
    # LANDMINE 1 -- donation clear -> controller+0x330
    #   AE minidump:  game+0xA0318F4  mov rcx,[rcx+0x168]
    #                 Rcx = 0x0000FFFFFFFFFFFF   (48 bits of ones)
    #   caller:       game+0xB32AAB  mov rcx,[rbx+0x330]   (rbx == our controller)
    #   the mod writes: dword 0xFFFFFFFF at +0x330, word cx (==0xFFFF) at +0x334
    #                 -> six 0xFF bytes -> 0x0000FFFFFFFFFFFF as a qword. Exact match.
    #   why: the boot log says 'SetDonationFaction: FAIL' then
    #        'DonationOff: FALLBACK offset=0x330'. It is a guess. On an older
    #        build that field was a faction ID where all-ones meant 'none'; on
    #        1.18.2 it is a POINTER the game calls a method on.
    #
    # LANDMINE 2 -- modal clear -> controller+0x258
    #   AF minidump:  game+0xB2F62D  mov rcx,[rax+8]   with rax == 0
    #                 preceded by     mov rax,[rsi+0x258]  (rsi == our controller)
    #   the game dereferences the modal-view pointer with NO null check, and the
    #   mod writes NULL into it.
    #
    # Together these explain every build:
    #   modal off, donation off  Y, AA           -> no crash
    #   modal off, donation on   Z, AC, AD, AE   -> crash at 0xA0318F4 (donation)
    #   modal on,  donation on   W, AB           -> crash at 0xA0318F4
    #   modal on,  donation on   AF              -> crash at 0xB2F62D (modal, hits first)
    #
    # AF only patched the je at 0x4968, which is the branch taken when the field
    # is ALREADY all-ones. Two earlier `jne 0x496A` at 0x495A and 0x4961 jump
    # straight past it in the normal case, so the write still happened. NOP the
    # write block itself instead -- unreachable by construction, from any path.
    patch(pe, image, 0x496A,
          bytes.fromhex("c7 04 3a ff ff ff ff 66 89 4c 3a 04"
                        "48 8d 0d 23 5c 02 00 e8 5e 24 00 00"),
          bytes.fromhex("90" * 24))   # both writes + the now-false log line

    # Landmine 2. Same reasoning: stop zeroing a pointer the game owns.
    patch(pe, image, 0x47C8, bytes.fromhex("48 89 34 3a"),
          bytes.fromhex("90 90 90 90"))            # clear modal pointer +0x258

    # Everything else in the init is the original, byte for byte. SetInventory
    # was cleared by AE (disabled, still crashed), and it is the one step F5-F9
    # cannot work without. Assert rather than patch so a future edit that
    # reintroduces surgery here fails loudly.
    for rva, expect in ((INIT_PANEL, "48 89 5c 24 18"), (0x487E, "41 ff d2"),
                        (0x4911, "41 ff d2"), (0x4968, "74 18"),
                        (0x5335, "ff 15 85 81 03 00"), (0x53B2, "ff 15 08 81 03 00"),
                        (0x53D2, "ff 15 e8 80 03 00")):
        off = pe.get_offset_from_rva(rva)
        want = bytes.fromhex(expect)
        if bytes(image[off:off + len(want)]) != want:
            raise RuntimeError(f"init body at 0x{rva:X} is not pristine")

    # ------------------------------------------------------------------ (P)
    # The handler-hook tail already inspects packet bytes; it just gates the
    # logging behind "init pending AND type == 0x15". Open every gate and print
    # the packet TYPE rather than the sub-command count. All replacements are
    # the same length, so nothing reflows. The `test rsi,rsi` null guard at
    # 0xBE50 is deliberately left in place.
    patch(pe, image, 0xBE4E, bytes.fromhex("74 22"), bytes.fromhex("90 90"))  # ignore pending flag
    patch(pe, image, 0xBE58, bytes.fromhex("75 16"), bytes.fromhex("90 90"))  # log every type
    patch(pe, image, 0xBE5A, bytes.fromhex("8b 56 10"),
          bytes.fromhex("0f b6 16"))                                  # movzx edx, byte [rsi]
    patch(pe, image, 0xBE5F, bytes.fromhex("74 0f"), bytes.fromhex("90 90"))  # do not skip type 0

    # Retitle the message so the trace reads honestly. The string is referenced
    # only from 0xBE61, and has 35 bytes plus 5 of padding to work with.
    fmt_off = pe.get_offset_from_rva(FMT_BLOCKING)
    old_fmt = b"  Handler: blocking %u sub-commands"
    if bytes(image[fmt_off:fmt_off + len(old_fmt)]) != old_fmt:
        raise RuntimeError("blocking format string was not found")
    new_fmt = b"  [PKT] handler type=%u"
    image[fmt_off:fmt_off + len(old_fmt)] = new_fmt.ljust(len(old_fmt), bytes(1))
    # =================================================================== AH
    # Restore the 1000-slot inventory expansion.
    #
    # The boot log has said 'Inventory mgr ptr: WARN dynamic scan failed' since
    # 1.18.2, which disables both the worker thread and the on-open slot patch.
    # Three things are involved; two are proven and the third is why the write
    # stays suppressed in this build.
    #
    # (1) The resolver's secondary validation is too strict.
    #     The primary scan of game SetInventory (game+0xB32AD0) SUCCEEDS -- one
    #     candidate in the whole function:
    #         SetInventory+0x225  mov r10,[rip+0x578EFE4]  -> RVA 0x62C1CE0
    #     and that global is real (110 code sites load it). The secondary check
    #     at 0xA560 then wants mov r64,[r10+disp8] with disp8 in {60,70,78}
    #     within 0x60 bytes. 1.18.2 has exactly one qualifying instruction:
    #         SetInventory+0x24F  4D 03 5A 78   add r11,[r10+0x78]
    #     REX.W ok, mod=01 ok, base r10 ok, disp 0x78 ok -- only the opcode
    #     differs: 0x03 (add) where the validator hardcodes 0x8B (mov).
    patch(pe, image, 0xA583, bytes.fromhex("8b"), bytes.fromhex("03"))

    # (2) The entry array moved. The patcher reads count from mgr+0x08 and the
    #     array from mgr+0x50. The game's own linear accessor shows 1.18.2:
    #         game+0x3AA5BC  cmp edi, dword [rbx+0x08]   ; count  -- still right
    #         game+0x3AA5CD  mov rax, qword [rbx+0x58]   ; array  -- was 0x50
    #         game+0x3AA5D1  mov rax, [r14+rax]          ; 8-byte stride
    #     Only the array pointer shifted. Patch the disp8 of
    #     `mov r10,[rax+0x50]` (4c 8b 50 50) at 0x874C.
    patch(pe, image, 0x874F, bytes.fromhex("50"), bytes.fromhex("58"))
    # =================================================================== AO
    # Private Storage capacity as an EXACT TOTAL.
    #
    # AN proved the mechanism: writing the static base made Private read 1200,
    # i.e. base 1000 plus the 200 expansion slots already on the save. The game
    # computes the displayed capacity itself, at game+0x1DDE3A3:
    #
    #   movzx ecx, word [info+0x48]      ; base slots  (what we set)
    #   add   cx,  word [container+0x1a] ; + purchased expansions
    #   mov   word [container+0x14], cx  ; = displayed total
    #
    # So to make PrivateStorageSlots mean the total, read the expansion count
    # and write base = total - expansions.
    #
    # The container is found the way the game finds it (game+0x84A5A0,
    # game+0x5328B0 and game+0x1DDE300 all use this identical walk):
    #
    #   owner = GetInventoryOwner(actor)      ; game+0x1DD2C60
    #   for c in owner+0x18[0 .. owner+0x20]: if word[c+0x10] == key: found
    #
    # The actor is the mainChar the mod already caches at ASI 0x3D520.
    #
    # That lookup runs ONLY on the on-open path, where we are on the game thread
    # with the warehouse live and mainChar certainly valid -- GetInventoryOwner
    # dereferences actor+0x68 -> +0x20 -> +0x30, which is not something to do
    # from a background thread against a cached pointer. The expansion count is
    # cached in this section so the worker can keep the value applied using
    # arithmetic alone, with no game call of its own beyond NameToKey.
    #
    # Until the expansion count is known (before the first open) nothing is
    # written at all: showing the unmodified number briefly is better than
    # showing total+expansions, which is the exact thing this build removes.
    e_lfanew = struct.unpack_from("<I", image, 0x3C)[0]
    opt_hdr = e_lfanew + 24
    sect_hdr = (opt_hdr + pe.FILE_HEADER.SizeOfOptionalHeader
                + pe.FILE_HEADER.NumberOfSections * 40)
    if struct.unpack_from("<I", image, e_lfanew)[0] != 0x00004550:
        raise RuntimeError("e_lfanew does not point at the PE signature")
    if struct.unpack_from("<H", image, e_lfanew + 6)[0] != pe.FILE_HEADER.NumberOfSections:
        raise RuntimeError("NumberOfSections offset disagrees with pefile")
    if struct.unpack_from("<I", image, opt_hdr + 56)[0] != pe.OPTIONAL_HEADER.SizeOfImage:
        raise RuntimeError("SizeOfImage offset disagrees with pefile")
    last = max(pe.sections, key=lambda x: x.VirtualAddress)
    if sect_hdr != last.get_file_offset() + 40:
        raise RuntimeError("section-table slot is not where it was expected")
    if sect_hdr + 40 > pe.OPTIONAL_HEADER.SizeOfHeaders:
        raise RuntimeError("no room in the section table for .pstext")
    # Two sections rather than one RWX section: a lone write+execute section
    # trips the "packed executable" heuristic in PE scanners, and this feature
    # needs exactly four writable bytes.
    added = ((b".pstext", PS_RVA, PS_RAW, PS_SIZE, 0x60000020),   # code, R+X
             (b".psdata", PD_RVA, PD_RAW, PD_SIZE, 0xC0000040))   # data, R+W
    for n, (name, rva, raw, size, chars) in enumerate(added):
        slot = sect_hdr + n * 40
        if slot + 40 > pe.OPTIONAL_HEADER.SizeOfHeaders:
            raise RuntimeError("no room in the section table")
        if any(image[slot:slot + 40]):
            raise RuntimeError("section-table slot is not zero-filled")
        if len(image) != raw:
            raise RuntimeError(f"image is {len(image)} bytes, expected {raw:#x}")
        image[slot:slot + 40] = (
            name + bytes(8 - len(name))
            + struct.pack("<IIIIIIHHI", size, rva, size, raw, 0, 0, 0, 0, chars))
        image.extend(bytes(size))
    struct.pack_into("<H", image, e_lfanew + 6,
                     pe.FILE_HEADER.NumberOfSections + len(added))
    struct.pack_into("<I", image, opt_hdr + 56, PD_RVA + PD_SIZE)

    def ps_off(rva):
        if rva >= PD_RVA:
            return PD_RAW + (rva - PD_RVA)
        return PS_RAW + (rva - PS_RVA)
    image[ps_off(EXP_CACHE):ps_off(EXP_CACHE) + 4] = bytes([0xFF] * 4)
    image[ps_off(BASE_ORIG):ps_off(BASE_ORIG) + 4] = bytes([0xFF] * 4)

    strs = {}
    cur = PS_RVA
    for nm, txt in (
            ("key", b"PrivateStorageSlots"),
            ("set", b"  [PRIV] exp=%u (0x16=%u tot=%u) base %u -> %u"),
            ("nc",  b"  [PRIV] no container - root=%llX actor=%llX owner=%llX n=%u"),
            ("nf",  b"  [PRIV] no entry"),
            ("exp", b"PrivateStorageExpansions"),
            ("st",  b"  [PRIV] base=%u max=%u exp=%d owner=%llX n=%u"),
            ("tot", b"  [PRIV] container total %u -> %u")):
        strs[nm] = cur
        blob = txt + bytes(1)
        image[ps_off(cur):ps_off(cur) + len(blob)] = blob
        cur = (cur + len(blob) + 3) & ~3
    code_at = (cur + 15) & ~15

    code = bytearray()
    fix = []
    lbl = {}

    def emit(h):
        code.extend(bytes.fromhex(h))

    def imm(h, value):
        code.extend(bytes.fromhex(h))
        code.extend(struct.pack("<I", value))

    def rip(h, tgt):
        code.extend(bytes.fromhex(h))
        fix.append((len(code), tgt))
        code.extend(bytes(4))

    def jr(h, label):
        code.extend(bytes.fromhex(h))
        fix.append((len(code), label))
        code.extend(bytes(4))

    # Two entry points: the worker must stay silent, and the on-open path is the
    # only one allowed to touch the game's container list.
    lbl["quiet"] = 0                           # <- worker  (0x1400)
    emit("48 81 EC C8 00 00 00")
    emit("C6 44 24 60 00")
    skip = len(code)
    emit("EB 00")
    lbl["loud"] = len(code)                    # <- on-open (0x4776)
    emit("48 81 EC C8 00 00 00")
    emit("C6 44 24 60 01")
    code[skip + 1] = len(code) - (skip + 2)
    rip("E8", SLOT_PATCHER)                    # original housing patcher
    emit("89 44 24 30")                        # preserve its return value

    rip("48 8D 0D", SECTION_STR)               # "Settings"
    rip("48 8D 15", strs["key"])               # "PrivateStorageSlots"
    emit("45 31 C0")
    rip("4C 8D 0D", INI_PATH)
    rip("FF 15", GPP_INT)                      # GetPrivateProfileIntA
    emit("89 44 24 38")                        # desired TOTAL; 0 = put it back

    # Manual expansion count. -1 (the default) means "use what the container
    # walk finds"; any 0..2000 value overrides it, which delivers an exact
    # total even while the automatic lookup is still being nailed down.
    rip("48 8D 0D", SECTION_STR)
    rip("48 8D 15", strs["exp"])
    emit("41 B8 FF FF FF FF")
    rip("4C 8D 0D", INI_PATH)
    rip("FF 15", GPP_INT)
    emit("89 44 24 34")

    # Manager first: a constructed StaticInfoManager2 is the cheapest proof that
    # the game finished loading its data, and it gates every game call below.
    rip("48 8B 05", INVMGR_PTR)
    emit("48 85 C0")
    jr("0F 84", "nf")
    emit("48 8B 00")
    emit("48 85 C0")
    jr("0F 84", "nf")
    emit("83 78 6C 00")
    jr("0F 84", "nf")
    emit("83 78 68 00")
    jr("0F 84", "nf")
    emit("48 89 44 24 40")                     # save mgr

    rip("48 8B 05", GAME_BASE)
    emit("48 85 C0")
    jr("0F 84", "done")
    emit("66 C7 44 24 28 FF FF")               # outKey = 0xFFFF
    imm("48 8D 88", CAMP_NAME)                 # lea rcx,[base+"CampWareHouse"]
    emit("48 8D 54 24 28")
    imm("48 05", NAME_TO_KEY)
    emit("FF D0")
    emit("84 C0")
    jr("0F 84", "nf")
    emit("0F B7 44 24 28")
    emit("89 44 24 48")                        # key (survives the calls below)

    emit("48 8B 44 24 40")
    emit("44 8B 40 68")                        # r8d = bucket count
    emit("8B 44 24 48")
    emit("31 D2")
    emit("41 F7 F0")                           # edx = key % bucketCount
    emit("48 8B 4C 24 40")
    emit("89 D0")
    emit("48 C1 E0 08")
    emit("48 03 41 78")                        # bucket = (key%n)<<8 + [mgr+0x78]
    emit("44 8B 08")
    emit("45 85 C9")
    jr("0F 84", "nf")
    emit("31 D2")
    lbl["scan"] = len(code)
    emit("44 39 CA")
    jr("0F 83", "nf")
    emit("44 8B 54 D0 08")
    emit("44 3B 54 24 48")
    jr("0F 84", "hit")
    emit("FF C2")
    jr("E9", "scan")
    lbl["hit"] = len(code)
    emit("8B 54 D0 0C")                        # entry index
    emit("48 8B 4C 24 40")
    emit("3B 51 08")
    jr("0F 83", "nf")
    emit("89 54 24 4C")                        # the container list keys on THIS
    emit("48 8B 49 58")                        # array A
    emit("48 85 C9")
    jr("0F 84", "nf")
    emit("48 8B 0C D1")
    emit("48 85 C9")
    jr("0F 84", "nf")
    emit("48 89 4C 24 68")                     # info entry
    emit("0F B7 41 48")
    emit("85 C0")
    jr("0F 84", "nf")
    emit("3D D0 07 00 00")
    jr("0F 87", "nf")
    # First sight of the base in this process, before any write of ours. The
    # static table is rebuilt from the game's data files every launch, so this
    # is always the game's own number -- and it is what "0" restores to.
    rip("8B 15", BASE_ORIG)
    emit("83 FA FF")
    emit("75 06")
    rip("89 05", BASE_ORIG)

    # --- the player's expansion count, on-open path only -------------------
    #
    # AO crashed here. It passed the mod's cached mainChar (ASI 0x3D520, built
    # by ASI 0xAA10 as [[0x62C1500]+0x48]) to GetInventoryOwner, which promptly
    # dereferenced [that+0x68] -> [+0x20] -> word[+0x30]. That field is not the
    # component table, so the chain walked into garbage.
    #
    # The game's own way of reaching the actor repeats at game+0xAECF8F,
    # game+0xAF819B, game+0xB02A0C and ~700 other sites -- and it starts from
    # root+0x00, not root+0x48:
    #
    #   rax = [game+0x62C1500]            ; root
    #   rcx = [rax]                       ; handle holder
    #   call game+0x751D20(rcx, &out)     ; resolve  -> returns &out
    #   if !byte[out+0x10]: invalid
    #   actor = [out+0x08]
    #
    # The owner is then inlined rather than called for: GetInventoryOwner's
    # common path returns [[actor+0x68]+0xb8], which the game itself computes
    # inline at game+0x532903. That leaves one game call in the whole feature
    # and drops the type lookup at game+0x312740 that AO also ran.
    #
    # Every pointer is range-checked before use, with the same test the mod
    # already applies to its own globals at ASI 0xAA58, so a wrong field now
    # logs the chain instead of faulting.
    emit("C7 44 24 58 00 00 00 00")            # +0x16 cross-check
    emit("C7 44 24 5C 00 00 00 00")            # +0x14 cross-check
    emit("31 C0")
    emit("48 89 44 24 50")                     # root  = 0
    emit("48 89 84 24 88 00 00 00")            # actor = 0
    emit("48 89 84 24 90 00 00 00")            # owner = 0
    emit("89 84 24 98 00 00 00")               # count = 0
    emit("48 89 84 24 B8 00 00 00")            # matched container = none
    emit("80 7C 24 60 00")
    jr("0F 84", "apply")                       # worker -> cached value only

    def guard(bail):
        """rax must look like a user-mode heap pointer, or bail.

        The test runs on a scratch copy in r11: shifting rax itself would
        leave the caller holding the shift result instead of the pointer.
        """
        emit("48 3D 00 00 01 00")              # not a small integer
        jr("0F 86", bail)
        emit("49 89 C3")
        emit("49 C1 EB 2F")                    # high 17 bits clear?
        jr("0F 85", bail)

    rip("48 8B 05", GAME_BASE)
    emit("48 85 C0")
    jr("0F 84", "apply")
    imm("48 8B 80", MAINCHAR_GLOBAL)           # rax = [base+0x62C1500]  (root)
    emit("48 89 44 24 50")
    emit("48 89 C1")                           # keep a copy for the guard
    guard("apply")
    emit("48 8B 09")                           # rcx = [root]  (handle holder)
    emit("48 89 C8")
    guard("apply")
    emit("48 8D 54 24 70")                     # rdx = &out
    emit("48 C7 02 00 00 00 00")
    emit("48 C7 42 08 00 00 00 00")
    emit("48 C7 42 10 00 00 00 00")            # zero the 24-byte out struct
    rip("48 8B 05", GAME_BASE)
    imm("48 05", RESOLVE_ACTOR)
    emit("FF D0")                              # rax = resolve(holder, &out)
    emit("48 85 C0")
    jr("0F 84", "apply")
    emit("80 78 10 00")                        # out+0x10: resolved?
    jr("0F 84", "apply")
    emit("48 8B 40 08")                        # rax = actor
    guard("apply")
    emit("48 89 84 24 88 00 00 00")
    emit("48 8B 40 68")                        # rax = component table
    guard("apply")
    imm("48 8B 80", 0xB8)                      # rax = [+0xb8]  = inventory owner
    guard("apply")
    emit("48 89 84 24 90 00 00 00")
    emit("49 89 C2")                           # r10 = owner
    emit("4D 8B 42 18")                        # r8 = container array
    emit("4C 89 C0")
    guard("apply")
    emit("41 8B 4A 20")                        # ecx = count
    emit("85 C9")
    jr("0F 84", "apply")
    emit("83 F9 40")
    jr("0F 87", "apply")                       # sanity: at most 64 containers
    emit("89 8C 24 98 00 00 00")               # count
    emit("4C 89 84 24 A0 00 00 00")            # array
    emit("31 C0")
    emit("89 84 24 A8 00 00 00")               # idx = 0
    emit("C6 44 24 64 00")                     # nothing recorded yet
    # The cursor lives on the stack, not in a register: the walk now logs each
    # container and the logger clobbers every volatile.
    lbl["cloop"] = len(code)
    emit("8B 84 24 A8 00 00 00")
    emit("3B 84 24 98 00 00 00")
    jr("0F 83", "apply")
    emit("48 8B 8C 24 A0 00 00 00")
    emit("48 8B 14 C1")                        # rdx = array[idx]
    emit("48 89 D0")
    guard("cnext")                             # skip a bad slot, keep walking
    # AQ's dump settled what container+0x10 holds: it is the INDEX into the
    # manager's array A, not the inventory key. 17 of the 18 containers satisfy
    # arrayA[c+0x10] == tot - expansions, and game+0x3AA5B2 uses word[c+0x10]
    # directly as that index. AP compared it against the key NameToKey returns
    # (8 for CampWareHouse) and so matched index 8 -- a decoy sitting right
    # beside the real one, 240 slots with no expansions. Private is index 7:
    # base 240 + 200 expansions = the 440 on screen.
    emit("0F B7 42 10")
    emit("3B 44 24 4C")
    jr("0F 84", "cfound")
    lbl["cnext"] = len(code)
    emit("8B 84 24 A8 00 00 00")
    emit("FF C0")
    emit("89 84 24 A8 00 00 00")
    jr("E9", "cloop")
    lbl["cfound"] = len(code)
    # First match wins, exactly as AP behaved; the walk continues only so the
    # dump covers the whole list.
    emit("80 7C 24 64 00")
    jr("0F 85", "cnext")
    emit("C6 44 24 64 01")
    emit("48 89 94 24 B8 00 00 00")            # keep it: the total is fixed below
    emit("0F B7 42 1A")                        # expansions (what the total uses)
    rip("89 05", EXP_CACHE)
    emit("0F B7 42 16")
    emit("89 44 24 58")
    emit("0F B7 42 14")
    emit("89 44 24 5C")
    jr("E9", "cnext")
    lbl["apply"] = len(code)
    # Unconditional status on the on-open path: AP only logged when the value
    # changed, so there was no way to tell whether an earlier write was still
    # standing on later opens.
    emit("80 7C 24 60 00")
    jr("0F 84", "stdone")
    emit("48 8B 4C 24 68")
    emit("48 85 C9")
    jr("0F 84", "stdone")
    emit("8B 84 24 98 00 00 00")
    emit("48 89 44 24 28")                     # n
    emit("48 8B 84 24 90 00 00 00")
    emit("48 89 44 24 20")                     # owner
    rip("44 8B 0D", EXP_CACHE)
    emit("44 0F B7 41 4A")                     # max
    emit("0F B7 51 48")                        # base
    rip("48 8D 0D", strs["st"])
    rip("E8", LOGGER)
    lbl["stdone"] = len(code)
    emit("8B 44 24 34")                        # manual override?
    emit("3D D0 07 00 00")
    emit("76 06")
    rip("8B 05", EXP_CACHE)                    # no -> what the walk found
    emit("3D D0 07 00 00")
    jr("0F 87", "nocont")
    emit("89 44 24 3C")                        # expansions

    emit("8B 54 24 38")
    emit("85 D2")
    jr("0F 85", "onpath")

    # PrivateStorageSlots=0 means "put it back", not "do nothing" -- AR left
    # whatever it had last written in place. Both numbers go to what the game
    # itself would hold: the captured base, and that base plus the player's own
    # expansions. Both writes are skipped when the values already match, so a
    # save that never had the option on is never touched.
    rip("8B 15", BASE_ORIG)
    emit("83 FA FF")
    jr("0F 84", "done")                        # never captured -> nothing to undo
    emit("8B 44 24 3C")
    emit("01 D0")
    emit("89 84 24 C0 00 00 00")               # target total = base + expansions
    jr("E9", "wr")                             # edx already holds the base

    lbl["onpath"] = len(code)
    emit("89 94 24 C0 00 00 00")               # target total = what was asked for
    emit("8B 44 24 3C")
    emit("29 C2")                              # base = total - expansions
    emit("83 FA 01")
    emit("7D 05")
    emit("BA 01 00 00 00")
    emit("81 FA D0 07 00 00")
    emit("7E 05")
    emit("BA D0 07 00 00")

    lbl["wr"] = len(code)
    emit("48 8B 4C 24 68")
    emit("0F B7 41 48")
    emit("39 D0")
    jr("0F 84", "tfix")                        # already correct -> just the total
    emit("66 89 51 48")                        # word[info+0x48] = base
    emit("48 89 44 24 20")                     # 4th vararg: old base
    emit("89 D1")
    emit("48 89 4C 24 28")                     # 5th vararg: new base
    rip("8B 15", EXP_CACHE)
    emit("44 8B 44 24 58")
    emit("44 8B 4C 24 5C")
    rip("48 8D 0D", strs["set"])
    rip("E8", LOGGER)

    # The container caches its own total at +0x14, and the game only recomputes
    # it when expansions change -- which is why AQ's base=800 never reached the
    # screen. Setting the base keeps any future recompute correct; setting this
    # makes the number right now. The two agree: base + expansions == the total
    # written here.
    lbl["tfix"] = len(code)
    emit("48 8B 8C 24 B8 00 00 00")
    emit("48 85 C9")
    jr("0F 84", "done")
    emit("8B 84 24 C0 00 00 00")               # the target total
    emit("85 C0")
    jr("0F 84", "done")
    emit("3D D0 07 00 00")
    jr("0F 87", "done")
    emit("0F B7 51 14")
    emit("39 C2")
    jr("0F 84", "done")                        # already right
    emit("66 89 41 14")
    emit("44 8B 84 24 C0 00 00 00")
    rip("48 8D 0D", strs["tot"])
    rip("E8", LOGGER)
    jr("E9", "done")

    lbl["nocont"] = len(code)
    emit("80 7C 24 60 00")
    jr("0F 84", "done")
    emit("48 8B 54 24 50")                     # root
    emit("4C 8B 84 24 88 00 00 00")            # actor
    emit("4C 8B 8C 24 90 00 00 00")            # owner
    emit("8B 84 24 98 00 00 00")
    emit("48 89 44 24 20")                     # count
    rip("48 8D 0D", strs["nc"])
    rip("E8", LOGGER)
    jr("E9", "done")

    lbl["nf"] = len(code)
    emit("80 7C 24 60 00")
    jr("0F 84", "done")
    rip("48 8D 0D", strs["nf"])
    rip("E8", LOGGER)

    lbl["done"] = len(code)
    emit("8B 44 24 30")
    emit("48 81 C4 C8 00 00 00")
    emit("C3")

    for at, tgt in fix:
        dest = lbl[tgt] + code_at if isinstance(tgt, str) else tgt
        code[at:at + 4] = struct.pack("<i", dest - (code_at + at + 4))
    if code_at + len(code) > PS_RVA + PS_SIZE:
        raise RuntimeError(f"AO wrapper overruns .pstext by "
                           f"{code_at + len(code) - (PS_RVA + PS_SIZE)} bytes")
    image[ps_off(code_at):ps_off(code_at) + len(code)] = code

    # The old .text cave is left untouched, so everything in .text outside these
    # two call sites is byte-identical to AI again.
    for site, entry in ((0x1400, "quiet"), (0x4776, "loud")):
        o = pe.get_offset_from_rva(site)
        if image[o] != 0xE8:
            raise RuntimeError(f"patcher call site {site:#x} is not a direct call")
        patch(pe, image, site, bytes(image[o:o + 5]),
              Asm(site).call(code_at + lbl[entry]).bytes())






    # (3) The per-entry slot field at +0x48 is CONFIRMED by the AH diagnostic.
    #     AH ran this same walk with the write NOP'd and reported 5 matches on
    #     every one of 167 worker passes, zero variance. The game's
    #     InventoryInfoKey pool holds exactly five Housing_* keys (Symbol,
    #     Refrigerator, GatheredMaterials, Collecting, Dresser) -- the five
    #     housing chests that default to 10 slots. CampWareHouse (440) and the
    #     other 13 keys correctly fail the == 10 guard. So the walk selects
    #     exactly the intended containers, and the write is enabled here.
    #
    #     Leaving the original instruction in place also stops the log spam AH
    #     produced: once the entries read 1000 the guard stops matching, the
    #     count goes to zero and the worker's `jle` skips logging entirely.


    # ------------------------------------------------------------------ (H)
    old_tag, new_tag = b"CD 1.13.01", b"CD 1.18.AS"
    at = image.find(old_tag)
    if at < 0 or image.find(old_tag, at + 1) >= 0:
        raise RuntimeError("expected exactly one build tag")
    image[at:at + len(old_tag)] = new_tag

    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_bytes(image)
    print(f"input_sha256={digest}")
    print(f"layout mode=0x{L_MODE:X} sub=0x{L_SUB:X} flags=0x{L_FLAGS:X} "
          f"subtypes=0x{L_SUBTYPES:X} dirty=0x{L_DIRTY:X}")
    print(f"stub=0x{CAVE:X}+0x{len(stub):X} helper=0x{HELPER:X}+0x{len(helper):X} "
          f"telemetry=0x{TELEMETRY:X}+0x{len(telemetry):X} "
          f"gate=0x{DISARM:X}+0x{len(gate_bytes):X} cave_end=0x{CAVE_END:X}")
    print(f"output_sha256={hashlib.sha256(image).hexdigest()}")
    print(f"wrote={destination} bytes={len(image)}")


main()
