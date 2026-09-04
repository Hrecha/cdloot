# -*- coding: utf-8 -*-
"""
CDLoot: вся таблица событий (desc id -> имя класса) прямо из exe.

Оффлайн-замена dump_event_descriptors.lua из help/: не нужен ни Cheat Engine,
ни запущенная игра. Номера событий - не рантайм-значения, они лежат в образе
константами внутри глобальных объектов-дескрипторов.

Как устроен объект (проверено на 1.0.0.2625, сошлось с заметками для 1573):
    +0x00  vtable
    +0x08  0x7FFFFFFE           - метка, по ней объекты и опознаются
    +0x0C  номер события (dword)
    +0x18  размер буфера (word), продублирован в +0x1A
    +0x1C  1

Имя класса берём через инициализатор: `lea rax,[rip+vtable]; mov [rip+obj],rax`
даёт пару объект<-vtable, дальше обычный MSVC-RTTI: vtable-8 -> COL ->
TypeDescriptor -> имя. Инициализаторов у объекта ДВА: базовый кладёт vtable
класса `Troc`, производный - настоящий. Берём все и выбираем производный.

    py -3 descdump.py           - вся таблица
    py -3 descdump.py loot      - только классы, где встречается подстрока
"""
import sys, re, struct
sys.stdout.reconfigure(encoding="utf-8", errors="replace")
from descmap import Img, IMAGEBASE

MARK = 0x7FFFFFFE
BASE_CLASS = "Troc"      # общий базовый класс всех дескрипторов

def objects(img):
    """Кандидаты в дескрипторы: ищем метку и проверяем форму."""
    out = {}
    for s in img.secs:
        if s["chars"] & 0x20000000:            # код пропускаем
            continue
        seg = img.data[s["raw"]: s["raw"] + s["rawsize"]]
        for m in re.finditer(re.escape(struct.pack("<I", MARK)), seg):
            p = m.start() - 8                  # метка лежит по +0x08
            if p < 0 or p + 0x20 > len(seg):
                continue
            vt, mark, did = struct.unpack_from("<QII", seg, p)
            size, size2, one = struct.unpack_from("<HHI", seg, p + 0x18)
            if not (0 < did <= 0x2000):        continue
            if size != size2 or size > 0x400:  continue
            if one != 1:                       continue
            out[s["rva"] + p] = dict(id=did, size=size, vt_file=vt)
    return out

# lea rax,[rip+X] ; mov [rip+Y],rax  - статический инициализатор объекта
INIT_RX = re.compile(rb"\x48\x8d\x05(....)\x48\x89\x05(....)", re.DOTALL)

def init_map(img):
    """obj_rva -> список vtable_rva (их несколько: база и производный класс)."""
    out = {}
    for s in img.code_secs():
        seg = img.data[s["raw"]: s["raw"] + s["rawsize"]]
        for m in INIT_RX.finditer(seg):
            p = m.start()
            a = struct.unpack_from("<i", seg, p + 3)[0]
            b = struct.unpack_from("<i", seg, p + 10)[0]
            vt  = s["rva"] + p + 7 + a
            obj = s["rva"] + p + 14 + b
            out.setdefault(obj, []).append(vt)
    return out

def rtti_of(img, vt_rva):
    """vtable -> имя класса через COL/TypeDescriptor."""
    o = img.rva2off(vt_rva - 8)
    if o is None: return None
    col_va = struct.unpack_from("<Q", img.data, o)[0]
    if not (IMAGEBASE < col_va < IMAGEBASE + 0x20000000): return None
    co = img.rva2off(col_va - IMAGEBASE)
    if co is None: return None
    sig, _, _, td_rva = struct.unpack_from("<IIII", img.data, co)
    if sig != 1: return None
    to = img.rva2off(td_rva + 0x10)
    if to is None: return None
    end = img.data.find(b"\0", to, to + 300)
    name = img.data[to:end].decode("latin1", "replace")
    m = re.match(r"\.\?A[VU]([\w_]+)@", name)
    return m.group(1) if m else name

def main():
    filt = sys.argv[1].lower() if len(sys.argv) > 1 else None
    img = Img()
    objs = objects(img)
    inits = init_map(img)
    rows = []
    for rva, o in objs.items():
        names = [n for n in (rtti_of(img, vt) for vt in inits.get(rva, [])) if n]
        # Базовый класс есть у всех - интересен производный, он же самый длинный.
        derived = [n for n in names if n != BASE_CLASS]
        best = max(derived or names, key=len) if names else None
        rows.append((o["id"], o["size"], best, rva))
    rows.sort(key=lambda r: r[0])
    named = sum(1 for r in rows if r[2])
    print("дескрипторов найдено: %d, с именем класса: %d\n" % (len(rows), named))
    print("%-8s %-6s %-52s %s" % ("номер", "размер", "класс", "объект"))
    for did, size, name, rva in rows:
        if filt and (not name or filt not in name.lower()):
            continue
        print("0x%04X   %-6d %-52s +0x%X" % (did, size, name or "?", rva))

if __name__ == "__main__":
    main()
