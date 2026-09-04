# -*- coding: utf-8 -*-
"""
CDLoot: проверка сигнатур на текущем exe игры.

    py -3 sigcheck.py           - прогнать все шаблоны из src/core.cpp
    py -3 sigcheck.py --relax   - для промахов попробовать урезанные варианты

Ничего не инжектит: читает exe с диска, разбирает PE и ищет по секциям кода.
"""
import re, os, sys, struct

EXE = r"C:\Steam\steamapps\common\Crimson Desert\bin64\CrimsonDesert.exe"
CORE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "src", "core.cpp")

# ---------------------------------------------------------------- PE ---------
def pe_sections(buf):
    e_lfanew = struct.unpack_from("<I", buf, 0x3C)[0]
    assert buf[e_lfanew:e_lfanew+4] == b"PE\0\0"
    fh = e_lfanew + 4
    nsec  = struct.unpack_from("<H", buf, fh+2)[0]
    optsz = struct.unpack_from("<H", buf, fh+16)[0]
    opt   = fh + 20
    imagebase = struct.unpack_from("<Q", buf, opt+24)[0]
    sect = opt + optsz
    secs = []
    for i in range(nsec):
        o = sect + i*40
        name = buf[o:o+8].rstrip(b"\0").decode(errors="replace")
        vsize, vaddr, rawsize, rawptr = struct.unpack_from("<IIII", buf, o+8)
        chars = struct.unpack_from("<I", buf, o+36)[0]
        secs.append(dict(name=name, vsize=vsize, rva=vaddr,
                         rawsize=rawsize, raw=rawptr, chars=chars))
    return imagebase, secs

def pat2re(pat):
    out = b""
    for tok in pat.split():
        out += b"." if tok in ("??", "?") else re.escape(bytes([int(tok, 16)]))
    return re.compile(out, re.DOTALL)

def scan(data, secs, pat, limit=8, code_only=True):
    rx = pat2re(pat); hits = []
    for s in secs:
        if code_only and not (s["chars"] & 0x20000000):
            continue
        seg = data[s["raw"]: s["raw"] + s["rawsize"]]
        for m in rx.finditer(seg):
            hits.append(s["rva"] + m.start())
            if len(hits) > limit:
                return hits
    return hits

# ------------------------------------------------- шаблоны из core.cpp -------
# Берём прямо из исходника: aob("имя", "шаблон") - чтобы список не разъезжался
# с кодом мода. Строки в C++ склеены переносами, собираем обратно.
def patterns_from_core(path):
    src = open(path, encoding="utf-8", errors="replace").read()
    rx = re.compile(r'aob\(\s*"([^"]+)"\s*,\s*((?:"[^"]*"\s*)+)\)', re.S)
    out = []
    for m in rx.finditer(src):
        parts = re.findall(r'"([^"]*)"', m.group(2))
        out.append((m.group(1), " ".join("".join(parts).split())))
    return out

# Урезанные запасные варианты: только начало шаблона (пролог функции), чтобы
# понять, функция переехала целиком или у неё изменилась середина.
def relaxed(pat, keep):
    toks = pat.split()
    return " ".join(toks[:keep]) if len(toks) > keep else None

def main():
    do_relax = "--relax" in sys.argv
    data = open(EXE, "rb").read()
    base, secs = pe_sections(data)
    print("exe        : %s" % EXE)
    print("размер     : %d байт" % len(data))
    print("imagebase  : 0x%X" % base)
    print()
    pats = patterns_from_core(CORE)
    bad = []
    for name, pat in pats:
        hits = scan(data, secs, pat)
        n = len(hits)
        st = "ПРОМАХ" if n == 0 else ("уникально" if n == 1 else "неоднозначно(%d)" % n)
        print("%-16s %-18s %s" % (name, st,
              " ".join("+0x%X" % h for h in hits[:4])))
        if n != 1:
            bad.append((name, pat))
    if do_relax and bad:
        print("\n--- урезанные варианты для промахов ---")
        for name, pat in bad:
            for keep in (24, 16, 12, 8, 6):
                r = relaxed(pat, keep)
                if not r:
                    continue
                h = scan(data, secs, r, limit=40)
                print("%-16s первые %2d байт: %d совпад. %s" % (
                      name, keep, len(h), " ".join("+0x%X" % x for x in h[:4])))
                if 1 <= len(h) <= 3:
                    break
    print("\nитог: %d из %d сигнатур уникальны" % (len(pats) - len(bad), len(pats)))

if __name__ == "__main__":
    main()
