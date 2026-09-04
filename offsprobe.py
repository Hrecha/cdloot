# -*- coding: utf-8 -*-
"""
CDLoot: вытащить смещения структур прямо из кода игры.

Смещения (ent+0x68, sub+0x1A0, transform+0xB4 и прочие) нельзя проверить
глазами - но игра сама их использует. Находим функцию по сигнатуре и читаем
disp из её инструкций. Это и есть "смещения выводятся, а не хранятся".

    py -3 offsprobe.py            - разобрать ключевые функции
    py -3 offsprobe.py get_pos 80 - показать 80 инструкций одной функции
"""
import sys, io, os
sys.stdout.reconfigure(encoding="utf-8", errors="replace")
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
from sigcheck import EXE, CORE, pe_sections, scan, patterns_from_core

def rva2off(secs, rva):
    for s in secs:
        if s["rva"] <= rva < s["rva"] + max(s["vsize"], s["rawsize"]):
            return s["raw"] + (rva - s["rva"])
    return None

def resolve_all(data, secs):
    out = {}
    for name, pat in patterns_from_core(CORE):
        h = scan(data, secs, pat)
        if len(h) == 1:
            out[name] = h[0]
    return out

def disasm(data, secs, rva, count):
    off = rva2off(secs, rva)
    md = Cs(CS_ARCH_X86, CS_MODE_64)
    md.detail = False
    return list(md.disasm(data[off:off+count*8], 0x140000000 + rva, count))

def main():
    data = open(EXE, "rb").read()
    base, secs = pe_sections(data)
    fns = resolve_all(data, secs)

    if len(sys.argv) > 2:
        name, n = sys.argv[1], int(sys.argv[2])
        rva = fns[name] - (0x0F if name == "area_sweep" else 0)
        print("=== %s @ +0x%X ===" % (name, rva))
        for i in disasm(data, secs, rva, n):
            print("  %012X  %-24s %s" % (i.address, i.mnemonic, i.op_str))
        return

    print("exe 1.0.0.2625 - смещения, прочитанные из инструкций\n")

    # get_pos: 40 53 48 83 EC 50 | 48 8B 41 68 | 48 8B 88 <disp32> | 48 8B 01
    #          пролог             ent+SUBOBJ    sub+TRANSFORM
    off = rva2off(secs, fns["get_pos"])
    body = data[off:off+0x100]
    ent_subobj = body[9]                      # [rcx+0x68]
    sub_tf = int.from_bytes(body[13:17], "little")
    print("  ENT_SUBOBJ      = 0x%02X   (в моде 0x68)  %s" %
          (ent_subobj, "СОВПАЛО" if ent_subobj == 0x68 else "ИЗМЕНИЛОСЬ"))
    print("  SUB_TRANSFORM   = 0x%X  (в моде 0x1A0) %s" %
          (sub_tf, "СОВПАЛО" if sub_tf == 0x1A0 else "ИЗМЕНИЛОСЬ"))

    # дальше в get_pos читается сама позиция - ищем обращение к transform+disp
    print("\n  тело get_pos (первые 40 инструкций):")
    for i in disasm(data, secs, fns["get_pos"], 40):
        print("    %012X  %-8s %s" % (i.address, i.mnemonic, i.op_str))

main()
