# -*- coding: utf-8 -*-
"""
CDLoot: поиск уехавшего смещения категории.

Читает память запущенной игры снаружи (только чтение) и сравнивает окно байт
в comp у объектов разного вида. Категория обязана различаться там, где объекты
разные, и совпадать у однотипных - плюс лежать в известном наборе значений.

    py -3 catprobe.py <адрес_сущности> [<адрес_сущности> ...]
"""
import sys, struct
sys.path.insert(0, r"C:\Users\user\Desktop\dmm\asi\autoloot-resig")
from peek import pid_of, module_base, Mem

ENT_SUBOBJ, SUB_COMP, SUB_INTER = 0x68, 0x20, 0x30
KNOWN = {0x01: "квест", 0x09: "насекомое", 0x0F: "лавка",
         0x11: "декорация", 0x16: "выброшено игроком"}
LO, HI = 0x260, 0x300          # окно вокруг 0x2C8

def main():
    pid = pid_of("CrimsonDesert.exe")
    if not pid:
        print("игра не запущена"); return
    m = Mem(pid)
    rows = []
    for a in sys.argv[1:]:
        ent = int(a, 16)
        sub = m.u64(ent + ENT_SUBOBJ)
        if not sub:
            print("%016X: sub не читается" % ent); continue
        comp = m.u64(sub + SUB_COMP)
        inter = m.u64(sub + SUB_INTER)
        eid = m.u32(ent + 0x60)
        win = m.read(comp + LO, HI - LO) if comp else None
        if not win:
            print("%016X: comp не читается" % ent); continue
        print("сущность %016X eid=%08X comp=%016X inter=%s" %
              (ent, eid or 0, comp, ("%016X" % inter) if inter else "нет"))
        rows.append((eid, win))
    if len(rows) < 2:
        return
    print("\nсмещения, где байты различаются (и что там лежит):")
    print("  смещ.  " + "  ".join("eid=%08X" % r[0] for r in rows))
    for off in range(HI - LO):
        vals = [r[1][off] for r in rows]
        if len(set(vals)) == 1:
            continue
        note = ""
        for v in vals:
            if v in KNOWN:
                note = "  <- есть известное значение: " + \
                       ", ".join("%02X=%s" % (v, KNOWN[v]) for v in vals if v in KNOWN)
                break
        print("  +0x%03X  %s%s" % (LO + off, "  ".join("      %02X" % v for v in vals), note))

main()
