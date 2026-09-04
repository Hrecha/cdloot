# -*- coding: utf-8 -*-
"""
CDLoot: номера событий (desc id) по СТАБИЛЬНЫМ именам классов из RTTI.

Номера выдаются движком в порядке регистрации и уезжают каждую версию, а имена
классов - символы из исходников игры, они не меняются. Скрипт раскручивает
цепочку прямо в exe, без запуска игры:

    имя в RTTI -> TypeDescriptor -> COL(_R4) -> vtable -> ссылка из кода -> id

    py -3 descmap.py            - разобрать интересующие нас классы
    py -3 descmap.py <подстрока> - поискать другие классы по имени
"""
import io, sys, re, struct
sys.stdout.reconfigure(encoding="utf-8", errors="replace")
from sigcheck import EXE, pe_sections

IMAGEBASE = 0x140000000

# Что ищем -> как это называется у нас в моде.
WANTED = [
    ("ProcessPickUpItemOnceTimer",       "подбор/сбор  ACT_TAKE/ACT_GATHER"),
    ("ProcessLootingDeadDropOnceTimer",  "обыск трупа  ACT_SEARCH"),
    ("PushCharacterToInventory",         "ловля        ACT_CATCH"),
]

class Img:
    def __init__(self):
        self.data = open(EXE, "rb").read()
        self.base, self.secs = pe_sections(self.data)
    def off2rva(self, off):
        for s in self.secs:
            if s["raw"] <= off < s["raw"] + s["rawsize"]:
                return s["rva"] + (off - s["raw"])
        return None
    def rva2off(self, rva):
        for s in self.secs:
            if s["rva"] <= rva < s["rva"] + max(s["vsize"], s["rawsize"]):
                return s["raw"] + (rva - s["rva"])
        return None
    def code_secs(self):
        return [s for s in self.secs if s["chars"] & 0x20000000]

def find_all(data, needle, limit=None):
    out = []
    for m in re.finditer(re.escape(needle), data):
        out.append(m.start())
        if limit and len(out) >= limit:
            break
    return out

def rtti_names(img, substr):
    """Все TypeDescriptor, чьё имя содержит подстроку."""
    res = []
    for off in find_all(img.data, b".?AV"):
        end = img.data.find(b"\0", off, off + 300)
        if end < 0:
            continue
        name = img.data[off:end].decode("latin1")
        if substr in name:
            rva = img.off2rva(off)
            if rva:
                res.append((name, rva - 0x10))     # td = имя - 0x10
    return res

def col_for_td(img, td_rva):
    """COL(_R4): [+0]=1, [+0xC]=image-rel TypeDescriptor."""
    out = []
    for off in find_all(img.data, struct.pack("<I", td_rva)):
        col = off - 0x0C
        if col < 0:
            continue
        if struct.unpack_from("<I", img.data, col)[0] != 1:
            continue
        rva = img.off2rva(col)
        if rva:
            out.append(rva)
    return out

def vtables_for_col(img, col_rva):
    """vtable-8 хранит указатель на COL."""
    va = IMAGEBASE + col_rva
    out = []
    for off in find_all(img.data, struct.pack("<Q", va)):
        rva = img.off2rva(off)
        if rva:
            out.append(rva + 8)
    return out

LEA_RX = re.compile(rb"\x48\x8d[\x05\x0d\x15\x1d\x25\x2d\x35\x3d]", re.DOTALL)

def xrefs_to(img, target_rva):
    """lea r64,[rip+disp] с адресом target."""
    target_va = IMAGEBASE + target_rva
    out = []
    for s in img.code_secs():
        seg = img.data[s["raw"]: s["raw"] + s["rawsize"]]
        for m in LEA_RX.finditer(seg):
            p = m.start()
            if p + 7 > len(seg):
                continue
            disp = struct.unpack_from("<i", seg, p + 3)[0]
            insn_va = IMAGEBASE + s["rva"] + p
            if insn_va + 7 + disp == target_va:
                out.append(s["rva"] + p)
    return out

def ids_near(img, rva, back=0x120, fwd=0x120):
    """Непосредственные значения, похожие на номер события, рядом со ссылкой."""
    off = img.rva2off(rva)
    lo, hi = off - back, off + fwd
    blob = img.data[lo:hi]
    found = []
    # mov edx/ecx/r8d, imm32   и   mov r/m16, imm16
    for m in re.finditer(rb"[\xb8-\xbf]", blob):
        p = m.start()
        if p + 5 > len(blob):
            continue
        v = struct.unpack_from("<I", blob, p + 1)[0]
        if 0x0700 <= v <= 0x0900:
            found.append((img.off2rva(lo + p), "mov r32, 0x%04X" % v, v))
    for m in re.finditer(rb"\x66[\xb8-\xbf]", blob):
        p = m.start()
        v = struct.unpack_from("<H", blob, p + 2)[0]
        if 0x0700 <= v <= 0x0900:
            found.append((img.off2rva(lo + p), "mov r16, 0x%04X" % v, v))
    return found

def main():
    img = Img()
    targets = WANTED
    if len(sys.argv) > 1:
        targets = [(sys.argv[1], "поиск")]
    for substr, what in targets:
        print("=" * 70)
        print("%s   (%s)" % (substr, what))
        tds = rtti_names(img, substr)
        if not tds:
            print("  RTTI: имя не найдено")
            continue
        for name, td in tds:
            print("  RTTI %s   TypeDescriptor +0x%X" % (name, td))
            cols = col_for_td(img, td)
            print("       COL: %s" % (" ".join("+0x%X" % c for c in cols) or "нет"))
            for c in cols:
                for vt in vtables_for_col(img, c):
                    xr = xrefs_to(img, vt)
                    print("       vtable +0x%X, ссылок из кода: %d %s" %
                          (vt, len(xr), " ".join("+0x%X" % x for x in xr[:4])))
                    for x in xr[:4]:
                        for rva, txt, v in ids_near(img, x):
                            print("           рядом +0x%X: %s" % (rva, txt))

if __name__ == "__main__":
    main()
