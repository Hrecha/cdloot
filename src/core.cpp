// ============================================================================
//  CDLoot core - вся логика мода. Пересобирается и перезагружается на лету.
//
//  Загрузчик зовёт CoreStart() при старте и CoreStop() перед выгрузкой.
//  CoreStop обязан снять ВСЕ перехваты и остановить свои потоки - иначе
//  горячая перезагрузка уронит игру.
//
//  Клавиши:
//    F4  - автосбор: включить или выключить непрерывный сбор
//    F9  - обзор: что вокруг, со всеми полями каждого объекта
//    F6  - проверка без отправки: событие собирается и показывается, но не шлётся
//    F8  - взять ближайшее по-настоящему
//    Numpad0 - взять ИМЕННО то, на что смотришь; обходит фильтр владельца,
//              радиусы и тумблеры видов - это и кража, и мебель (розыск)
//    F5  - взять ближайшее, что не растение (когда трава мешает выбору)
//    F7  - подслушать 10 секунд: что игра шлёт себе сама
//    F10 - перезагрузить логику (это уже загрузчик)
// ============================================================================

#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

#define CDEXPORT extern "C" __declspec(dllexport)

// ---------------------------------------------------------------- лог -------

static HANDLE g_log = INVALID_HANDLE_VALUE;
static CRITICAL_SECTION g_logCs;
static bool g_logReady = false;

// Разговорчивость. В готовой сборке лог должен показывать, что мод делает, а
// не как он это выяснял: разбор структур, тайминги, зарядка каждого узла и
// прочая разведка выключаются ключом Debug=0. Фильтруем по началу ФОРМАТА -
// это дёшево и не требует собирать строку, которую всё равно выбросим.
static bool g_debug = true;
static bool noisy_line(const char *fmt) {
    static const char *noise[] = {
        "[время]", "[кат?]", "[имя]", "[данные]", "[владелец]", "[узел]",
        "[зарядка] авто", "  [трансформ", "  [инвентарь]", "  [далеко]",
        "  [метки]", "      ", "[пойманное]", "  [данные]", "  [имя]",
    };
    for (int i = 0; i < 15; i++) {
        const char *n = noise[i];
        size_t len = strlen(n);
        if (strncmp(fmt, n, len) == 0) return true;
    }
    return false;
}

static void L(const char *fmt, ...) {
    if (!g_logReady) return;
    if (!g_debug && noisy_line(fmt)) return;
    char buf[2048]; DWORD wr; SYSTEMTIME st; GetLocalTime(&st);
    EnterCriticalSection(&g_logCs);
    int n = sprintf(buf, "[%02u:%02u:%02u.%03u] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list ap; va_start(ap, fmt);
    n += vsnprintf(buf + n, sizeof buf - n - 3, fmt, ap);
    va_end(ap);
    buf[n++] = 13; buf[n++] = 10; buf[n] = 0;
    if (g_log != INVALID_HANDLE_VALUE) WriteFile(g_log, buf, n, &wr, 0);
    LeaveCriticalSection(&g_logCs);
}

// Шестнадцатеричная строка для дампов. Без выделений памяти - зовём из хуков.
static void hexs(const void *p, int n, char *out, int outSize) {
    const unsigned char *b = (const unsigned char *)p;
    int w = 0;
    for (int i = 0; i < n && w + 4 < outSize; i++)
        w += sprintf(out + w, "%02X ", b[i]);
    if (w) out[w - 1] = 0; else out[0] = 0;
}

// ------------------------------------------------------------- модуль игры --

struct Module { unsigned char *base; size_t size; };
static Module g_game;

static bool in_image(const void *p) {
    return (const unsigned char *)p >= g_game.base &&
           (const unsigned char *)p <  g_game.base + g_game.size;
}

// Сколько раз мы сходили в ядро за проверкой памяти. VirtualQuery не бесплатен
// и берёт блокировку адресного пространства процесса - при большом счёте
// подтормаживает вся игра, а не только наш поток.
static volatile LONG g_vqCount;

// Здесь была VirtualQuery на каждое чтение. Замер в этом процессе: 240 мкс на
// вызов - системный вызов явно кем-то перехвачен. 5400 проверок за такт
// складывались в 730 мс на игровом потоке, то есть в намертво вставшую игру.
//
// Голая проверка правдоподобности адреса вместо неё тоже не годится: мусор из
// массива объектов проходит её насквозь, и такт даёт под четыре сотни
// аппаратных исключений - а они не бесплатны, да и краш-хендлер игры их видит.
//
// Поэтому VirtualQuery осталась, но с кэшем областей: адреса кучи, где живут
// сущности, укладываются в несколько десятков регионов, и повторный вопрос об
// уже известном регионе не стоит ничего. Запись живёт 5 секунд - столько
// регион точно не переедет, а если всё же переедет, промах поймает __try.
struct Region { uintptr_t base, end; DWORD when; bool ok; };
static Region g_regs[256];
static int g_regN;
static int g_regNext;

static bool readable(const void *p, SIZE_T n) {
    uintptr_t a = (uintptr_t)p;
    if (a < 0x10000 || a >= 0x7FFFFFFFFFFFull || (a >> 47)) return false;
    DWORD now = GetTickCount();
    for (int i = 0; i < g_regN; i++) {
        if (a < g_regs[i].base || a + n > g_regs[i].end) continue;
        if (now - g_regs[i].when < 5000) return g_regs[i].ok;
        break;                                   // запись устарела - спросим заново
    }
    MEMORY_BASIC_INFORMATION mbi;
    InterlockedIncrement(&g_vqCount);
    if (!VirtualQuery(p, &mbi, sizeof mbi)) return false;
    bool ok = mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD));
    uintptr_t base = (uintptr_t)mbi.BaseAddress;
    uintptr_t end  = base + mbi.RegionSize;
    int slot;
    if (g_regN < 256) slot = g_regN++;
    else { slot = g_regNext; g_regNext = (g_regNext + 1) % 256; }
    g_regs[slot].base = base; g_regs[slot].end = end;
    g_regs[slot].when = now;  g_regs[slot].ok = ok;
    return ok && (end - a) >= n;
}

// Точечная защита чтения. Раньше от промаха спасала VirtualQuery, но она в
// этом процессе стоит 240 мкс, и теперь вместо неё - перехват исключения на
// каждую сущность. Промах тут дело житейское: массив объектов живёт своей
// жизнью и может измениться прямо между чтениями. Цена ошибки - один
// пропущенный объект, а не весь такт.
static volatile LONG g_faults;

static bool read_u32(const void *p, uint32_t *out) {
    if (!readable(p, 4)) return false;
    __try { *out = *(const uint32_t *)p; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { InterlockedIncrement(&g_faults); return false; }
}

static bool read_u64(const void *p, uint64_t *out) {
    if (!readable(p, 8)) return false;
    __try { *out = *(const uint64_t *)p; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { InterlockedIncrement(&g_faults); return false; }
}

// ---------------------------------------------------------------- RTTI ------
// Имя класса объекта по стандартному MSVC64 RTTI:
//   объект[0] = vtable, vtable[-8] = COL(_R4), COL+0x00 = сигнатура (1 на x64),
//   COL+0x0C = смещение TypeDescriptor от базы модуля, имя лежит в td+0x10.
// Имена классов - символы из исходников игры и патчи их не трогают, в отличие
// от номеров событий. Возвращает строку вида ".?AVTrocTr<Класс>@pa@@".
static const char *rtti_class(void *obj) {
    if (!obj || !readable(obj, 8)) return 0;
    __try {
        unsigned char *vt = *(unsigned char **)obj;
        if (!in_image(vt)) return 0;
        unsigned char *col = *(unsigned char **)(vt - 8);
        if (!in_image(col)) return 0;
        if (*(uint32_t *)col != 1) return 0;          // сигнатура x64
        uint32_t tdOff = *(uint32_t *)(col + 0x0C);
        if (!tdOff) return 0;
        unsigned char *td = g_game.base + tdOff;
        if (!in_image(td)) return 0;
        return (const char *)(td + 0x10);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&g_faults);
        return 0;
    }
}

// То же, но без украшений компилятора - для лога.
static const char *rtti_short(void *obj) {
    const char *n = rtti_class(obj);
    if (!n) return 0;
    if (n[0] == '.' && n[1] == '?' && n[2] == 'A') n += 4;
    return n;
}

// Поиск подстроки без учёта регистра: имена типов приходят вперемешку -
// Collection_Prop_Bottle, Item_gimmick_abyss_quest_...
static const char *stristr_ru(const char *hay, const char *needle) {
    if (!hay || !needle || !*needle) return 0;
    for (const char *h = hay; *h; h++) {
        const char *a = h, *b = needle;
        while (*a && *b) {
            char ca = *a, cb = *b;
            if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
            if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
            if (ca != cb) break;
            a++; b++;
        }
        if (!*b) return h;
    }
    return 0;
}

static unsigned char *aob(const char *name, const char *pattern) {
    unsigned char bytes[160]; char mask[160]; int len = 0;
    for (const char *p = pattern; *p && len < 160; ) {
        if (*p == ' ') { p++; continue; }
        if (*p == '?') { bytes[len] = 0; mask[len] = '?'; len++; p += (p[1] == '?') ? 2 : 1; }
        else { char h[3] = { p[0], p[1], 0 };
               bytes[len] = (unsigned char)strtoul(h, 0, 16); mask[len] = 'x'; len++; p += 2; }
    }
    unsigned char *found = 0; int hits = 0;
    unsigned char *end = g_game.base + g_game.size - len;
    for (unsigned char *cur = g_game.base; cur < end; cur++) {
        int i = 0;
        for (; i < len; i++) if (mask[i] == 'x' && cur[i] != bytes[i]) break;
        if (i == len) { if (!hits) found = cur; if (++hits > 1) break; }
    }
    if (hits == 1) { L("[aob] %-14s = +0x%llX", name, (unsigned long long)(found - g_game.base)); return found; }
    L("[aob] %-14s НЕ НАЙДЕНА (совпадений: %d)", name, hits);
    return 0;
}

struct GameFns {
    unsigned char *tlsInit, *descLookup, *allocEvent, *enqueue, *areaFn, *ownCheck, *getPos;
    uint32_t *descMask;      // маска таблицы дескрипторов
    void    **queue;         // очередь событий
} g_fn;

// Достаёт адрес из инструкции с rip-относительной адресацией.
static void *rip_target(unsigned char *insn, int dispOff, int insnLen) {
    int32_t rel = *(int32_t *)(insn + dispOff);
    return insn + insnLen + rel;
}

static bool resolve_functions() {
    // tls+desc больше НЕ ищется своим шаблоном. Он был самым длинным и хрупким
    // в моде: захватывал тело функции целиком плюс начало следующей, и на
    // сборке 2760 умер от смены распределения регистров.
    // Обе функции выводятся из места вызова, которое мы и так находим ниже
    // (desc_mask+queue). Оно устроено так:
    //     +0x00  E8 rel32              -> инициализация TLS
    //     +0x05  44 8B 05 rel32        -> DESC_MASK
    //     +0x0C  0F B7 54 24 xx
    //     +0x11  E8 rel32              -> поиск дескриптора
    //     +0x16  4C 8B 25 rel32        -> очередь
    // Проверено на 2760: разница между двумя вызовами ровно 0x60, ровно как и
    // было зашито прежним кодом (descLookup = tls + 0x60). То есть одно место
    // вызова заменяет собой целую сигнатуру.
    g_fn.allocEvent = aob("alloc_event",
        "48 89 5C 24 ?? 4C 89 44 24 ?? 57 48 83 EC 20 8B ?? BA ?? ?? 00 00");
    g_fn.enqueue = aob("enqueue",
        "48 89 5C 24 08 57 48 83 EC 20 48 8B ?? 38 65 48 8B 04 25 58 00 00 00");
    // Сигнатура попадает на 0xF байт внутрь тела функции - её начало лежит
    // раньше. Проверяем, что там действительно пролог, а не случайные байты.
    unsigned char *areaHit = aob("area_sweep",
        "55 41 54 41 55 41 56 41 57 48 8B EC 48 83 EC 50 C5 F8 29 74 24 40 4D 8B ?? 44 8B ?? 48 8B ??");
    if (areaHit) {
        unsigned char *start = areaHit - 0x0F;
        L("[aob] area_sweep начало = +0x%llX (байты %02X %02X %02X)",
          (unsigned long long)(start - g_game.base), start[0], start[1], start[2]);
        g_fn.areaFn = start;
    }
    // Эти две функции перехватывает AutoLoot: если он загружен, их прологи
    // изменены и сигнатура не совпадёт. Для разработки CDLoot его лучше убрать.
    // own_check тоже берём из места вызова, а не по телу функции. Это те самые
    // ворота кражи: игра читает у объекта поле +0x120, зовёт проверку и по
    // ответу решает, показать "Взять" или "Украсть".
    //     48 8B 89 20 01 00 00   mov rcx,[rcx+0x120]
    //     E8 rel32               call own_check
    //     84 C0                  test al,al
    //     74 04                  jz +4
    //     B3 02                  mov bl,2
    // На 2760 такое место в образе ровно одно.
    {
        unsigned char *site = aob("own_check_site",
            "48 8B 89 20 01 00 00 E8 ?? ?? ?? ?? 84 C0 74 04 B3 02");
        if (site) {
            g_fn.ownCheck = site + 7 + 5 + *(int32_t *)(site + 8);
            L("[aob] own_check = +0x%llX (из места вызова)",
              (unsigned long long)(g_fn.ownCheck - g_game.base));
        }
    }
    g_fn.getPos = aob("get_pos",
        "40 53 48 83 EC 50 48 8B 41 68 48 8B 88 ?? 01 00 00 48 8B 01");
    // DESC_MASK и очередь берём из кода, а не зашиваем: они переезжают каждый патч.
    //   +00 E8 rel32          (5)
    //   +05 44 8B 05 rel32    (7)   mov r8d,[DESC_MASK]
    //   +12 0F B7 54 24 xx    (5)
    //   +17 E8 rel32          (5)
    //   +22 4C 8B 25 rel32    (7)   mov r12,[очередь]
    unsigned char *dq = aob("desc_mask+queue",
        "E8 ?? ?? ?? ?? 44 8B 05 ?? ?? ?? ?? 0F B7 54 24 ?? E8 ?? ?? ?? ?? 4C 8B 25");
    if (dq) {
        g_fn.descMask = (uint32_t *)rip_target(dq + 5,  3, 7);
        g_fn.queue    = (void **)  rip_target(dq + 22, 3, 7);
        // Цель прямого вызова: адрес = байт после инструкции + смещение.
        g_fn.tlsInit    = dq + 5      + *(int32_t *)(dq + 1);
        g_fn.descLookup = dq + 17 + 5 + *(int32_t *)(dq + 18);
        L("[aob] tls_init = +0x%llX, desc_lookup = +0x%llX (из места вызова)",
          (unsigned long long)(g_fn.tlsInit - g_game.base),
          (unsigned long long)(g_fn.descLookup - g_game.base));
        L("[aob] DESC_MASK = +0x%llX, очередь = +0x%llX",
          (unsigned long long)((unsigned char *)g_fn.descMask - g_game.base),
          (unsigned long long)((unsigned char *)g_fn.queue - g_game.base));
        // Оба обязаны лежать внутри образа игры. Здесь была ошибка на один
        // байт, и очередь уезжала за пределы модуля: первое же событие писало
        // бы в чужую память.
        if (!in_image(g_fn.descMask) || !in_image(g_fn.queue)) {
            L("[aob] ОТКАЗ: адреса вне модуля игры - отправка запрещена");
            g_fn.descMask = 0; g_fn.queue = 0;
        }
    }

    bool ok = g_fn.tlsInit && g_fn.allocEvent && g_fn.enqueue && g_fn.areaFn &&
              g_fn.descMask && g_fn.queue;
    L("[aob] итог: %s", ok ? "ключевые функции найдены" : "часть не найдена");
    return ok;
}

// ------------------------------------------------------ раскладка сущности --

// Раскладка по заметкам стороннего автора (проверена им на 1573, сверена
// нами на 2474: идентификатор и номера событий совпали до байта).
#define ENT_ROUTE      0x90    // route / session id (в заметках для 1573 стоял
                              // +0x58; в 2474 там ноль у всех, живое значение
                              // 0x90100000 лежит по +0x90 - видно в дампе байтов)
#define ENT_EID        0x60    // идентификатор сущности
#define ENT_SUBOBJ     0x68    // -> sub_object
#define ENT_TYPEINFO   0x88    // [[+0x88]+1] = тип (7 - растение)

// ent+0x68 - это МАССИВ КОМПОНЕНТ актора, а не безымянный sub_object. Слоты
// фиксированные, пустые там, где компонента у актора отсутствует, и каждая
// называет себя сама через RTTI. Поэтому компоненты ищем по имени класса:
// перестановка слотов будущим патчем тогда ничего не значит. Смещения ниже
// остались как запасной путь и как подсказка, где что лежало на 2625.
#define SUB_COMP       0x20    // ClientStatusActorComponent
#define SUB_INTER      0x30    // ClientGimmickActorComponent
#define CLS_STATUS     "ClientStatusActorComponent"
#define CLS_GIMMICK    "ClientGimmickActorComponent"
#define CLS_AI         "ClientAiActorComponent"
#define SUB_SLOTS_END  0x80    // дальше идут не компоненты (в 0x1A0 - transform)
#define SUB_TRANSFORM  0x1A0   // sub_obj -> transform
#define TF_POS         0xB4    // позиция внутри transform (2474)

#define COMP_DEAD      0x273   // ==1 -> труп, нужен обыск (2474)
#define COMP_CAT       0x2C8   // категория взаимодействия (2474)
// На 2625 и 2658 категория по +0x2C8 приходит мусором (0x00/0xAF вместо
// набора {01,09,0F,11,16}). Догадка про сдвиг всей структуры на +0x48
// проверена на живых объектах и НЕ подтвердилась - по +0x310 всюду нули.
//
// Нашлось на 2658, по обзору в кустах с жуками: поле переехало на +0x2D0.
// Байт оттуда, 22 образца одним обзором:
//     0x09 - насекомое (8 из 8, тот самый номер из старой таблицы)
//     0x0C - зверь (тип 03, козы)
//     0x1F - собираемое растение
//     0x00 - растение-предмет (узел с данными предмета)
// Читать надо именно БАЙТ: у части объектов старшие байты слова заняты
// чем-то своим (0x08002309 - это тот же жук).
#define COMP_CAT2      0x2D0   // категория взаимодействия (2658)
// 0x09 - это НЕ "насекомое", как значилось в старой таблице для 1573, а любая
// мелкая живность, которую игра кладёт в инвентарь целиком: жук, лысуха,
// курица. Событие так и называется - PushCharacterToInventory. Проверено:
// по этой метке собраны и жуки, и куры.
#define CAT2_SMALL     0x09    // мелкая живность: ловится событием 0x0800
#define CAT2_FISH      0x05    // рыба: тем же событием, снято с живой поимки
#define CAT2_BEAST     0x0C    // крупный зверь: ловле не поддаётся
// Подтверждены живой проверкой: рыба, мелкая живность, зверь. Остальные
// значения байта НЕ разобраны, и гадать по ним больше не будем:
//   0x0D - видели на вещах в доме, помеченных оракулом как чужие, но ровно
//          то же значение стоит на рудной жиле посреди скалы;
//   0x11 - видели на утвари в хижине, но по нему же отсеялось 13 объектов
//          в дикой природе, где никакой утвари нет;
//   0x1F - часто у собираемых растений, но встречается и у пустых кустов.
// Поле явно не чистое перечисление, а объединение, и его смысл зависит от
// чего-то ещё. Решения принимаем по УЗЛУ - он не врёт.
#define CAT2_CLUTTER   0x11    // спорное; фильтр по нему выключен по умолчанию

#define INTER_ITEMDATA 0xC0
#define INTER_GATHER   0xE0
#define INTER_LOCKED   0x3E2

// категории взаимодействия
#define CAT_QUEST      0x01    // квестовое, не трогать
#define CAT_INSECT     0x09    // насекомые
#define CAT_SHOP       0x0F    // товар в лавке, не трогать
#define CAT_DECOR      0x11    // декорация, не трогать
#define CAT_PLAYERDROP 0x16    // выброшено игроком

#define TYPE_PLANT     0x07    // [[entity+0x88]+1]: растение, руда, камень
#define TYPE_ITEM      0x06    // обычный предмет на земле
                              // тип 03 встречается рядом с предметами и на нём
                              // игра вылетела - в белый список не берём

// Игрок узнаётся по старшему байту идентификатора. Так его находит сама игра
// и так же делает AutoLoot: mov eax,eid / shr eax,0x18 / cmp eax,0xA0.
// Это признак движка, а не наша догадка по координатам.
#define EID_PLAYER_TAG 0xA0
#define EID_WORLD_TAG  0xB0

static void *deref(void *p, int off) {
    uint64_t v;
    if (!p || !read_u64((unsigned char *)p + off, &v)) return 0;
    return readable((void *)v, 8) ? (void *)v : 0;
}

// Компонента актора по имени класса. Обходит слоты массива и спрашивает у
// каждого объекта его RTTI-имя. Дороже чтения по смещению, но переживает
// патчи: слот может переехать, имя класса - нет.
// Разбор RTTI стоит дорого: readable() - это VirtualQuery, то есть заход в
// ядро, а слотов шестнадцать. Звать такое на каждый объект каждого кадра
// нельзя - игра встаёт колом. Поэтому слот и vtable запоминаются с первого
// удачного разбора, и дальше проверка сводится к одному сравнению указателя.
// Класс переехал в другой слот - кэш промахнётся и разбор повторится.
struct CompSlot { const char *cls; int off; void *vt; };
static CompSlot g_slots[] = {
    { CLS_STATUS,  -1, 0 },
    { CLS_GIMMICK, -1, 0 },
    { CLS_AI,      -1, 0 },
};
static const int g_slotN = sizeof g_slots / sizeof g_slots[0];

static void *comp_by_class(void *sub, const char *cls) {
    if (!sub) return 0;
    CompSlot *sl = 0;
    for (int i = 0; i < g_slotN; i++)
        if (!strcmp(g_slots[i].cls, cls)) { sl = &g_slots[i]; break; }

    if (sl && sl->off >= 0) {
        void *c = deref(sub, sl->off);
        if (!c) return 0;                       // компоненты нет - это норма
        if (readable(c, 8) && *(void **)c == sl->vt) return c;
    }
    if (!readable(sub, SUB_SLOTS_END)) return 0;
    for (int off = 0; off < SUB_SLOTS_END; off += 8) {
        void *c = deref(sub, off);
        if (!c) continue;
        const char *n = rtti_class(c);
        if (!n || !strstr(n, cls)) continue;
        if (sl && readable(c, 8)) { sl->off = off; sl->vt = *(void **)c; }
        return c;
    }
    return 0;
}

// Состав компонент одной строкой: ClientGimmickActorComponent -> Gimmick.
// Это и есть та примета, по которой объекты различаются на глаз.
static void comps_brief(void *sub, char *out, int cap) {
    int n = 0; out[0] = 0;
    for (int off = 0; off < SUB_SLOTS_END && n < cap - 24; off += 8) {
        void *c = deref(sub, off);
        const char *nm = c ? rtti_short(c) : 0;
        if (!nm) continue;
        if (!strncmp(nm, "Client", 6)) nm += 6;
        int k = 0;
        while (nm[k] && k < 20 && strncmp(nm + k, "ActorComponent", 14)) k++;
        n += sprintf(out + n, "%.*s ", k, nm);
    }
}

struct Vec3 { float x, y, z; };

// Позиция по TF_POS - НЕ мировая, а в системе координат родителя. У травы на
// земле родителя нет, и разницы не видно; а руда на скале - дочерний объект
// скалы, и её метры отсчитываются от скалы. Игрок, зацепившийся за руду, тоже
// переходит в её систему: вместо (-515.9, 716.2, -909.8) он читается как
// (1.2, -0.2, -0.6). Мод при этом считал расстояния между точками из разных
// начал отсчёта - отсюда "руду ниже себя не видит", "вися на железе видит всю
// руду вокруг" и мнимые переходы на сотни метров при зацепе за стену.
//
// Родитель и его МИРОВАЯ позиция лежат в том же трансформе:
//     +0xC8  eid родителя, 0xFFFFFFFF - родителя нет
//     +0xEC  три float: мировая позиция родителя
// Сложение проверено на живых числах: локальные (1.18, -0.24, -0.64) плюс
// родительские (-517.45, 716.73, -906.55) дают ровно ту точку, где игрок
// стоял секундой раньше по земле.
// Поворот игрока: четыре float, единичный кватернион (проверено по дампам -
// сумма квадратов ровно 1.0). Нужен, чтобы понять, на что игрок смотрит:
// наведённую цель игра нам не отдаёт, но направление взгляда есть, и объект
// под прицелом вычисляется углом.
#define TF_ROT         0xA4
#define TF_PARENT_EID  0xC8
#define TF_PARENT_POS  0xEC

// eid родителя из трансформа. Снаряжение и содержимое сумки висят на теле
// игрока, поэтому родителем у них должен быть он сам - это и есть прямой
// признак "вещь наша", а не расстояние и не кучность.
static uint32_t ent_parent(unsigned char *e) {
    void *sub = deref(e, ENT_SUBOBJ);
    void *tf  = sub ? deref(sub, SUB_TRANSFORM) : 0;
    if (!tf || !readable(tf, TF_PARENT_EID + 4)) return 0;
    __try {
        uint32_t p = *(uint32_t *)((unsigned char *)tf + TF_PARENT_EID);
        return (p == 0xFFFFFFFF) ? 0 : p;
    } __except (EXCEPTION_EXECUTE_HANDLER) { InterlockedIncrement(&g_faults); return 0; }
}

// Вектор взгляда из кватернина: поворачиваем (0,0,1).
static bool player_forward(unsigned char *e, Vec3 *out) {
    void *sub = deref(e, ENT_SUBOBJ);
    void *tf  = sub ? deref(sub, SUB_TRANSFORM) : 0;
    if (!tf || !readable((unsigned char *)tf + TF_ROT, 16)) return false;
    float q[4];
    __try { memcpy(q, (unsigned char *)tf + TF_ROT, 16); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    float len = q[0]*q[0] + q[1]*q[1] + q[2]*q[2] + q[3]*q[3];
    if (len < 0.5f || len > 1.5f) return false;      // не кватернион
    float x = q[0], y = q[1], z = q[2], w = q[3];
    // v = (0,0,1), повёрнутый кватернионом
    out->x = 2.0f * (x * z + w * y);
    out->y = 2.0f * (y * z - w * x);
    out->z = 1.0f - 2.0f * (x * x + y * y);
    return true;
}

static bool ent_pos(unsigned char *e, Vec3 *out) {
    void *sub = deref(e, ENT_SUBOBJ);
    void *tf  = sub ? deref(sub, SUB_TRANSFORM) : 0;
    if (!tf || !readable(tf, TF_PARENT_POS + 12)) return false;
    Vec3 v;
    __try {
        unsigned char *t = (unsigned char *)tf;
        v = *(Vec3 *)(t + TF_POS);
        uint32_t parent = *(uint32_t *)(t + TF_PARENT_EID);
        if (parent != 0xFFFFFFFF && parent != 0) {
            Vec3 pw = *(Vec3 *)(t + TF_PARENT_POS);
            if (pw.x == pw.x && pw.y == pw.y && pw.z == pw.z) {
                v.x += pw.x; v.y += pw.y; v.z += pw.z;
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { InterlockedIncrement(&g_faults); return false; }
    if (v.x != v.x || v.y != v.y || v.z != v.z) return false;
    *out = v;
    return true;
}

static uint8_t ent_type(unsigned char *e) {
    void *ti = deref(e, ENT_TYPEINFO);
    if (!ti) return 0xFF;
    __try { return *((unsigned char *)ti + 1); }
    __except (EXCEPTION_EXECUTE_HANDLER) { InterlockedIncrement(&g_faults); return 0xFF; }
}

// --------------------------------------------------------------- секундомер -
// Наш обработчик висит на обходе сцены, то есть на игровом потоке: всё, что мы
// там делаем, игра ждёт. Замер пишется только для тяжёлых тактов, чтобы сам
// лог не стал причиной тормозов.
static double g_qpcFreq;

struct TickTimer {
    LARGE_INTEGER t0;
    LONG vq0;
    const char *tag;
    TickTimer(const char *t) : tag(t) {
        QueryPerformanceCounter(&t0);
        vq0 = InterlockedCompareExchange(&g_vqCount, 0, 0);
    }
    ~TickTimer() {
        LARGE_INTEGER t1; QueryPerformanceCounter(&t1);
        if (g_qpcFreq <= 0) return;
        double ms = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / g_qpcFreq;
        LONG vq = InterlockedCompareExchange(&g_vqCount, 0, 0) - vq0;
        if (ms >= 8.0) L("[время] %s: %.1f мс, проверок памяти %ld", tag, ms, vq);
    }
};

// ------------------------------------------------------------------ плашка --
// Экранная надпись о ближайшем объекте. Окно слоёное, сквозное для мыши и
// поверх всех - игра его не замечает; оформление и приём сняты с CDSteal.
// Живёт на своём потоке: зависание окна не должно трогать игровой.
//
// При горячей перезагрузке класс окна ОБЯЗАТЕЛЬНО снимать с регистрации: он
// принадлежит exe, а оконная процедура лежит в нашей dll. Оставить класс
// зарегистрированным - значит указать новым окном на выгруженный код.
static CRITICAL_SECTION g_hudCs;
static char   g_hudText[3000];
static volatile LONG g_hudRunning;
#ifdef CDLOOT_PLUGIN
static volatile LONG g_hudOn = 0;   // готовая сборка: показываем только табличку
#else
static volatile LONG g_hudOn = 1;
#endif
static HWND   g_hudWnd;
static HANDLE g_hudThread;
static const wchar_t *HUD_CLASS = L"CDLootHud";

// Перерисовка идёт ТОЛЬКО когда текст изменился. Слоёное окно поверх
// полноэкранной игры - вещь тонкая, дёргать его десять раз в секунду просто
// так не стоит.
static volatile LONG g_hudSeq;

// Метки объектов на экране. Считаются в такте по матрице камеры, рисуются
// оконным потоком. Нужны, чтобы ГЛАЗАМИ увидеть, куда смотрит вычисленный
// луч: если метки садятся на предметы - направление верное, если уезжают
// вбок или зеркалятся - видно сразу, без перебора вариантов.
struct Mark { int x, y; char text[40]; bool aim; };
static Mark  g_marks[24];
static int   g_markN;
static int   g_scrW, g_scrH;
// Метки на предметах и перекрестье. Инструмент разведки, но в густом месте он
// закрывает собой экран: сотня крестов с подписями поверх боя. Поэтому
// отдельный выключатель - ключ ShowMarks в ini и Numpad2 на лету. Когда
// выключено, не только не рисуется, но и НЕ СЧИТАЕТСЯ: ни камера, ни прицел,
// ни экранные координаты.
static volatile LONG g_marksOn;

static void hud_set(const char *fmt, ...) {
    char buf[3000];
    va_list ap; va_start(ap, fmt);
    _vsnprintf(buf, sizeof buf - 1, fmt, ap);
    buf[sizeof buf - 1] = 0;
    va_end(ap);
    EnterCriticalSection(&g_hudCs);
    bool same = strcmp(g_hudText, buf) == 0;
    if (!same) memcpy(g_hudText, buf, sizeof buf);
    LeaveCriticalSection(&g_hudCs);
    if (!same) InterlockedIncrement(&g_hudSeq);
}

static LRESULT CALLBACK hud_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg != WM_PAINT) return DefWindowProcW(h, msg, wp, lp);
    PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
    RECT full; GetClientRect(h, &full);
    HBRUSH clear = CreateSolidBrush(RGB(0, 0, 0));   // прозрачный цвет
    FillRect(dc, &full, clear); DeleteObject(clear);

    // Метки объектов и перекрестье - то, ради чего окно во весь экран. Это
    // ИНСТРУМЕНТ РАЗРАБОТЧИКА: в готовой сборке игрок должен видеть только
    // короткую табличку, как в оригинальном AutoLoot, - ни перекрестья
    // посреди экрана, ни крестов на предметах.
    if (g_debug && InterlockedCompareExchange(&g_marksOn, 0, 0)) {
        HPEN pen = CreatePen(PS_SOLID, 1, RGB(120, 200, 255));
        HPEN aimPen = CreatePen(PS_SOLID, 2, RGB(255, 220, 90));
        HGDIOBJ oldPen = SelectObject(dc, pen);
        int cx = full.right / 2, cy = full.bottom / 2;
        MoveToEx(dc, cx - 12, cy, 0); LineTo(dc, cx + 13, cy);
        MoveToEx(dc, cx, cy - 12, 0); LineTo(dc, cx, cy + 13);
        HFONT mf = CreateFontW(15, 0, 0, 0, 600, 0, 0, 0, DEFAULT_CHARSET,
                               0, 0, CLEARTYPE_QUALITY, 0, L"Consolas");
        HGDIOBJ oldF = SelectObject(dc, mf);
        SetBkMode(dc, TRANSPARENT);
        EnterCriticalSection(&g_hudCs);
        for (int i = 0; i < g_markN; i++) {
            int x = g_marks[i].x, y = g_marks[i].y;
            SelectObject(dc, g_marks[i].aim ? aimPen : pen);
            MoveToEx(dc, x - 6, y, 0); LineTo(dc, x + 7, y);
            MoveToEx(dc, x, y - 6, 0); LineTo(dc, x, y + 7);
            if (g_marks[i].aim) { MoveToEx(dc, cx, cy, 0); LineTo(dc, x, y); }
            wchar_t wm[40];
            MultiByteToWideChar(CP_UTF8, 0, g_marks[i].text, -1, wm, 40);
            SetTextColor(dc, g_marks[i].aim ? RGB(255, 220, 90) : RGB(120, 200, 255));
            TextOutW(dc, x + 8, y - 8, wm, (int)wcslen(wm));
        }
        LeaveCriticalSection(&g_hudCs);
        SelectObject(dc, oldF); DeleteObject(mf);
        SelectObject(dc, oldPen); DeleteObject(pen); DeleteObject(aimPen);
    }

    char buf[3000];
    EnterCriticalSection(&g_hudCs);
    memcpy(buf, g_hudText, sizeof buf);
    LeaveCriticalSection(&g_hudCs);
    wchar_t w[3000];
    MultiByteToWideChar(CP_UTF8, 0, buf, -1, w, 3000);
    SetBkMode(dc, TRANSPARENT);
    HFONT f = CreateFontW(17, 0, 0, 0, 600, 0, 0, 0, DEFAULT_CHARSET,
                          0, 0, CLEARTYPE_QUALITY, 0, L"Consolas");
    HGDIOBJ old = SelectObject(dc, f);

    // Рамка. У разработчика она под большую сводку - фиксированные 980x440.
    // В готовой сборке внутрь попадает одна строка вроде "Auto-Loot: ON", и
    // прямоугольник в пол-экрана вокруг неё выглядел дико. Меряем текст и
    // рисуем по нему.
    RECT rc; rc.left = 30; rc.top = 60; rc.right = 30 + 980; rc.bottom = 60 + 440;
    if (!g_debug) {
        RECT m = {0, 0, 1200, 200};
        DrawTextW(dc, w, -1, &m, DT_CALCRECT | DT_LEFT | DT_TOP | DT_NOPREFIX);
        rc.right  = rc.left + (m.right - m.left) + 22;
        rc.bottom = rc.top  + (m.bottom - m.top) + 12;
    }
    HBRUSH bg = CreateSolidBrush(RGB(16, 18, 24));
    FillRect(dc, &rc, bg); DeleteObject(bg);
    HBRUSH fr = CreateSolidBrush(RGB(90, 170, 255));
    FrameRect(dc, &rc, fr); DeleteObject(fr);
    // Заголовок - строка состояния, её рисуем отдельно и ярче: на неё
    // смотрят, чтобы понять, жив ли мод, а не чтобы читать.
    wchar_t *nl = wcschr(w, L'\n');
    RECT tr = rc; tr.left += 9; tr.top += 6; tr.right -= 9;
    if (nl) {
        *nl = 0;
        SetTextColor(dc, RGB(120, 200, 255));
        DrawTextW(dc, w, -1, &tr, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
        tr.top += 22;
        SetTextColor(dc, RGB(220, 230, 240));
        DrawTextW(dc, nl + 1, -1, &tr, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
    } else {
        SetTextColor(dc, RGB(220, 230, 240));
        DrawTextW(dc, w, -1, &tr, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
    }
    SelectObject(dc, old); DeleteObject(f);
    EndPaint(h, &ps);
    return 0;
}

static DWORD WINAPI hud_thread(LPVOID) {
    WNDCLASSEXW wc; ZeroMemory(&wc, sizeof wc);
    wc.cbSize = sizeof wc; wc.lpfnWndProc = hud_proc;
    wc.hInstance = GetModuleHandleW(0); wc.lpszClassName = HUD_CLASS;
    wc.hCursor = LoadCursorW(0, (LPCWSTR)(ULONG_PTR)32512);
    RegisterClassExW(&wc);
    g_hudWnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TRANSPARENT |
                               WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
                               HUD_CLASS, L"", WS_POPUP, 0, 0,
                               GetSystemMetrics(SM_CXSCREEN),
                               GetSystemMetrics(SM_CYSCREEN),
                               0, 0, wc.hInstance, 0);
    if (!g_hudWnd) { L("[плашка] окно создать не удалось"); return 0; }
    // Окно во весь экран: панель рисуется в углу, метки - поверх объектов.
    // Чёрный назначен прозрачным, поэтому всё, кроме нарисованного, не видно
    // и мышь сквозь окно проходит (WS_EX_TRANSPARENT уже стоит).
    g_scrW = GetSystemMetrics(SM_CXSCREEN);
    g_scrH = GetSystemMetrics(SM_CYSCREEN);
    SetLayeredWindowAttributes(g_hudWnd, RGB(0, 0, 0), 235, LWA_COLORKEY | LWA_ALPHA);
    L("[плашка] готова, F3 - скрыть/показать");
    int visible = 0; LONG lastSeq = -1;
    while (InterlockedCompareExchange(&g_hudRunning, 0, 0)) {
        MSG m;
        while (PeekMessageW(&m, 0, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); }
        int want = InterlockedCompareExchange(&g_hudOn, 0, 0) != 0;
        if (want != visible) {
            ShowWindow(g_hudWnd, want ? SW_SHOWNOACTIVATE : SW_HIDE);
            if (want) SetWindowPos(g_hudWnd, HWND_TOPMOST, 0, 0, 0, 0,
                                   SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            visible = want;
            lastSeq = -1;
        }
        LONG seq = InterlockedCompareExchange(&g_hudSeq, 0, 0);
        if (want && seq != lastSeq) { InvalidateRect(g_hudWnd, 0, TRUE); lastSeq = seq; }
        Sleep(100);
    }
    DestroyWindow(g_hudWnd); g_hudWnd = 0;
    UnregisterClassW(HUD_CLASS, GetModuleHandleW(0));
    L("[плашка] снята");
    return 0;
}

// -------------------------------------------------- перехват со снятием -----
// Каждый перехват помнит, что лежало на месте до него. CoreStop возвращает
// байты обратно, и только после этого DLL можно выгружать.

struct Hook {
    unsigned char *site;        // начало перехваченной функции
    unsigned char orig[32];     // что там было
    int origLen;                // сколько байт мы забрали
    unsigned char *cave;        // наш переходник
    bool active;
    const char *name;
};
static Hook g_areaHook = {};
static Hook g_enqHook  = {};
static Hook g_ownHook  = {};
static Hook g_armHook  = {};

// Страница под трамплины. Долго считалось, что она обязана лежать в пределах
// 2 ГБ от кода игры, и адрес подбирался перебором - сначала вслепую, потом по
// карте памяти. И то и другое рано или поздно упиралось в "нет памяти рядом":
// пещеры намеренно не освобождаются (в них может стоять поток игры), и за пару
// десятков горячих перезагрузок ближние слоты кончались, а мод при этом молча
// оставался без перехватов.
//
// Ограничения не существует. И патч на месте функции, и возврат в её тело, и
// вызов обработчика сделаны через `mov rax, imm64; jmp/call rax` - адрес
// абсолютный, 64-битный. Пусть систeма кладёт страницу куда хочет.
static unsigned char *g_cave;      // текущая страница
static int            g_caveUsed;  // сколько в ней занято

// Кусок под один трамплин. Пока в странице есть место - режем от неё, поэтому
// на поколение уходит одна страница на все перехваты, а не по одной на каждый.
static unsigned char *alloc_near(void *anchor, int need) {
    (void)anchor;
    if (g_cave && g_caveUsed + need <= 0x1000) {
        unsigned char *p = g_cave + g_caveUsed;
        g_caveUsed += (need + 15) & ~15;
        return p;
    }
    g_cave = (unsigned char *)VirtualAlloc(0, 0x1000, MEM_COMMIT | MEM_RESERVE,
                                           PAGE_EXECUTE_READWRITE);
    if (!g_cave) return 0;
    g_caveUsed = (need + 15) & ~15;
    return g_cave;
}

static bool write_code(void *dst, const void *src, size_t n) {
    DWORD old;
    if (!VirtualProtect(dst, n, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy(dst, src, n);
    VirtualProtect(dst, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), dst, n);
    return true;
}

// ------------------------------------------------------- отправка события ---
// Порядок восстановлен разбором AutoLoot (его функция отправки лежит по
// +0xD167) и сверен с чужими заметками. Совпало до байта - кроме поля route,
// см. ниже.

typedef void  (__fastcall *FnTlsInit)();
typedef void *(__fastcall *FnDescLookup)(uint64_t zero, uint32_t descId, uint32_t mask);
typedef void *(__fastcall *FnAllocEvent)(uint64_t zero, uint32_t sizeCode);
typedef void  (__fastcall *FnEnqueue)(void *queueObj, void *ev, void *desc, uint64_t zero);

enum Action { ACT_SEARCH = 0, ACT_TAKE = 1, ACT_CATCH = 2, ACT_GATHER = 3 };

static const char *act_name(Action a) {
    switch (a) { case ACT_SEARCH: return "обыскать труп"; case ACT_CATCH: return "поймать";
                 case ACT_GATHER: return "собрать растение"; default: return "подобрать"; }
}

// Единственная таблица событий. Номер и размер - значения для 1.0.0.2625, но
// источником истины считается ИМЯ КЛАССА: движок раздаёт номера сам и сдвигает
// их между версиями, а имена приходят из исходников игры и не меняются. При
// старте resolve_desc_ids перепроверяет номера по именам и правит таблицу.
//
// Проверено на 2625 разбором образа (descdump.py): 0x0807 - это НЕ "старый
// подбор", а удаление предмета из инвентаря; 0x0821 - не "взаимодействие", а
// выпадение вещей при поломке гиммика. Держать их здесь было опасно.
struct DescRow { uint16_t id; uint16_t size; void *ptr; const char *what; const char *cls; };
static DescRow g_descs[] = {
    { 0x07E8,    7, 0, "обыск трупа",    "TrocTrProcessLootingDeadDropOnceTimer" },
    { 0x0809, 0x0D, 0, "подбор/сбор",    "TrocTrProcessPickUpItemOnceTimer" },
    { 0x0800,    8, 0, "ловля",          "TrocTrPushCharacterToInventoryOnceTimer" },
    { 0x080D,    3, 0, "взаимодействие", "TrocTrInteractionDoStepDoInteractionOnceTimer" },
    { 0x0813, 0x10, 0, "кража предмета", "TrocTrStealItemByFrameEventOnceTimer" },
};
static const int g_descN = sizeof g_descs / sizeof g_descs[0];

// Действие -> строка таблицы. Сбор растений идёт тем же событием, что и подбор,
// но со способом 0x05 и другим хвостом.
static int act_row(Action a) {
    switch (a) { case ACT_SEARCH: return 0; case ACT_CATCH: return 2; default: return 1; }
}
static uint16_t act_desc(Action a) { return g_descs[act_row(a)].id; }
static uint16_t act_size(Action a) { return g_descs[act_row(a)].size; }

// Спрашиваем у игры дескриптор по каждому номеру и смотрим на имя класса.
// Один проход по всем номерам, поэтому дёшево. Что не нашлось - остаётся с
// зашитым номером, но об этом будет сказано в логе.
static void resolve_desc_ids(uint32_t mask) {
    int left = g_descN;
    for (int i = 0; i < g_descN; i++) g_descs[i].ptr = 0;
    for (uint32_t id = 1; id <= 0x1FFF && left > 0; id++) {
        void *d = 0;
        __try { d = ((FnDescLookup)g_fn.descLookup)(0, id, mask); }
        __except (EXCEPTION_EXECUTE_HANDLER) { continue; }
        if (!d) continue;
        const char *n = rtti_class(d);
        if (!n) continue;
        for (int i = 0; i < g_descN; i++) {
            if (g_descs[i].ptr || !strstr(n, g_descs[i].cls)) continue;
            uint16_t size = readable((unsigned char *)d + 0x18, 2)
                          ? *(uint16_t *)((unsigned char *)d + 0x18) : 0;
            if (g_descs[i].id != (uint16_t)id || (size && size != g_descs[i].size))
                L("[desc] %s: было 0x%04X/%u, стало 0x%04X/%u - версия игры сменилась",
                  g_descs[i].cls, g_descs[i].id, g_descs[i].size, (uint16_t)id, size);
            g_descs[i].id = (uint16_t)id;
            if (size) g_descs[i].size = size;
            g_descs[i].ptr = d;
            left--;
            break;
        }
    }
    for (int i = 0; i < g_descN; i++)
        if (!g_descs[i].ptr)
            L("[desc] %s НЕ НАЙДЕН - остаётся зашитый 0x%04X", g_descs[i].cls, g_descs[i].id);
}
static bool g_selfTested = false;
static bool g_sendAllowed = false;
// Поток, который прямо сейчас шлёт наше событие: слушатель по нему отличает
// свои события от чужих.
static volatile LONG g_sendTid;

// Собирает событие целиком. Возвращает указатель или 0. Ничего не отправляет.
static void *build_event(Action act, uint32_t targetEid, uint32_t playerEid,
                         uint32_t playerRoute, uint8_t mode, void **descOut) {
    ((FnTlsInit)g_fn.tlsInit)();

    uint16_t id = act_desc(act);
    void *desc = ((FnDescLookup)g_fn.descLookup)(0, id, *g_fn.descMask);
    if (!desc) { L("[send] дескриптор 0x%04X не найден", id); return 0; }

    uint16_t size = act_size(act);
    void *ev = ((FnAllocEvent)g_fn.allocEvent)(0, size);
    if (!ev) { L("[send] не удалось выделить событие"); return 0; }
    unsigned char *e = (unsigned char *)ev;

    // В +0x58 всегда идёт route игрока. «Исключение для ловли, там снова eid»
    // досталось из чужих заметок и оказалось враньём: перехват собственных
    // событий игры на 2658 показал route (90100000) во всех трёх случаях -
    // и в обыске трупа, и в подборе, и в ловле:
    //     [шпион] 0x0800 ловля размер=8 +0x50=A0100001 +0x58=90100000
    uint32_t route = playerRoute;

    *(uint32_t *)(e + 0x30) = 1;
    *(uint32_t *)(e + 0x40) = 0;
    *(uint64_t *)(e + 0x48) = 0;
    *(uint32_t *)(e + 0x50) = playerEid;
    *(uint32_t *)(e + 0x54) = 0;
    // Здесь была наша вторая ошибка: в +0x58 идёт route ИГРОКА (поле +0x58 его
    // сущности), а не его идентификатор. Исключение - ловля: там снова eid.
    *(uint32_t *)(e + 0x58) = route;
    *(void   **)(e + 0x60) = desc;
    *(uint16_t *)(e + 0x68) = size;
    *(uint8_t  *)(e + 0x78) = 1;

    unsigned char *buf = *(unsigned char **)(e + 0x70);
    if (!buf) { L("[send] у события нет буфера"); return 0; }
    buf[0] = (uint8_t)(id & 0xFF); buf[1] = (uint8_t)(id >> 8); buf[2] = 0xFF;
    switch (act) {
        case ACT_SEARCH:                                  // E8 07 FF <eid>
            *(uint32_t *)(buf + 3) = targetEid;
            break;
        case ACT_CATCH:                                   // 00 08 FF <eid> .. 03
            *(uint32_t *)(buf + 3) = targetEid; buf[7] = 3;
            break;
        case ACT_GATHER:                                  // 09 08 FF 05 <eid> 00 00 01 FF 00
            buf[3] = 0x05;
            *(uint32_t *)(buf + 4) = targetEid;
            buf[8] = 0x00; buf[9] = 0x00; buf[10] = 0x01; buf[11] = 0xFF; buf[12] = 0x00;
            break;
        default:                                          // 09 08 FF <способ> <eid> 01 01 00 FF 00
            buf[3] = mode;                                // 0x00 обычный подбор, 0x04 из кармана
            *(uint32_t *)(buf + 4) = targetEid;
            buf[8] = 0x01; buf[9] = 0x01; buf[10] = 0x00; buf[11] = 0xFF; buf[12] = 0x00;
            break;
    }

    char hb[260], hp[160];
    hexs(e + 0x30, 0x50, hb, sizeof hb);
    hexs(buf, size, hp, sizeof hp);
    L("[event] %s: desc=0x%04X(%p) размер=%u цель=%08X игрок=%08X route=%08X",
      act_name(act), id, desc, size, targetEid, playerEid, route);
    L("        тело +0x30: %s", hb);
    L("        буфер:      %s", hp);
    if (descOut) *descOut = desc;
    return ev;
}

// Обёртка с ловлей исключений: чужая память - чужие правила. Лучше строка в
// логе, чем вылет игры без следов.
// ------------------------------------------------- работа вне игрового потока
// Обход сцены стоил 18 мс МЕДИАНЫ на игровом потоке, то есть целый кадр при
// 60 fps, и при обходе каждый кадр игра упиралась в 35 fps. Теперь тяжёлое
// (перечисление сущностей, чтение позиций, фильтры, плашка) считает наш поток,
// а игровой только исполняет готовое.
//
// На игровом потоке ОБЯЗАНЫ выполняться две вещи: сборка и отправка события
// (она зовёт функции игры и её очередь) и зарядка узла. Их наш поток кладёт в
// очередь, а хук разбирает и выполняет.
static volatile LONG  g_armLastEid;           // чей узел кладём в очередь
static void          *volatile g_ctxLatest;   // последний контекст от игры
static volatile LONG  g_gameTid;              // поток, на котором нас зовёт игра
static volatile LONG  g_offThread = 1;        // ключ ScanOffThread

// Очереди под замком. Первая версия обходилась атомарными счётчиками, и в
// ней была гонка, дающая ДУБЛИ: хук забирал n записей, обнулял счётчик и
// начинал их отправлять, а наш поток в это же время дописывал запись с
// индексом n и ставил счётчик n+1 - при следующем сливе первые n записей
// уходили ВТОРОЙ раз. Атомарного счётчика тут мало: нужны атомарными сами
// "дописать" и "забрать всё", а это две операции.
struct PendAct { int act; uint32_t eid, player, route; uint8_t mode; };
static PendAct       g_pendAct[64];
static int           g_pendActN;

struct PendArm { void *node; uintptr_t mode; uint32_t player, eid; };
static PendArm       g_pendArm[32];
static int           g_pendArmN;

static CRITICAL_SECTION g_pendCs;
static volatile LONG    g_pendCsReady;

static void pend_init(void) {
    if (InterlockedExchange(&g_pendCsReady, 1) == 0) InitializeCriticalSection(&g_pendCs);
}

static bool on_game_thread(void) {
    LONG t = InterlockedCompareExchange(&g_gameTid, 0, 0);
    return t == 0 || (DWORD)t == GetCurrentThreadId();
}

static bool send_action(Action act, uint32_t targetEid, uint32_t playerEid,
                        uint32_t playerRoute, uint8_t mode, bool dryRun) {
    if (!g_sendAllowed) { L("[send] отправка запрещена: самопроверка не пройдена"); return false; }
    // Не на игровом потоке - складываем решение, хук отправит его сам.
    if (!dryRun && !on_game_thread()) {
        pend_init();
        EnterCriticalSection(&g_pendCs);
        bool room = g_pendActN < 64;
        if (room) {
            PendAct &q = g_pendAct[g_pendActN++];
            q.act = (int)act; q.eid = targetEid;
            q.player = playerEid; q.route = playerRoute; q.mode = mode;
        }
        LeaveCriticalSection(&g_pendCs);
        return room;
    }
    bool ok = false;
    __try {
        void *desc = 0;
        void *ev = build_event(act, targetEid, playerEid, playerRoute, mode, &desc);
        if (!ev) return false;
        if (dryRun) { L("[send] ПРОВЕРКА: событие собрано, но НЕ отправлено"); return true; }
        if (!readable(g_fn.queue, 8)) { L("[send] глобал очереди нечитаем"); return false; }
        void *q = *g_fn.queue;
        if (!q || !readable(q, 8)) { L("[send] очередь пуста или нечитаема (%p)", q); return false; }
        InterlockedExchange(&g_sendTid, (LONG)GetCurrentThreadId());
        ((FnEnqueue)g_fn.enqueue)(q, ev, desc, 0);
        InterlockedExchange(&g_sendTid, 0);
        L("[send] ОТПРАВЛЕНО в очередь %p", q);
        ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        L("[send] ИСКЛЮЧЕНИЕ 0x%08X - игра цела, отправка отменена", GetExceptionCode());
        ok = false;
    }
    return ok;
}

// Разовая самопроверка на игровом потоке: всё ли живо, прежде чем что-то
// отправлять. Пока она не пройдена, F8 молчит.
static void self_test() {
    if (g_selfTested) return;
    g_selfTested = true;
    L("--- самопроверка ---");
    __try {
        if (!readable(g_fn.descMask, 4)) { L("[тест] DESC_MASK нечитаем"); return; }
        uint32_t mask = *g_fn.descMask;
        L("[тест] DESC_MASK = 0x%08X", mask);
        if (!readable(g_fn.queue, 8)) { L("[тест] глобал очереди нечитаем"); return; }
        void *q = *g_fn.queue;
        L("[тест] очередь = %p %s", q, (q && readable(q, 8)) ? "(читается)" : "(ПУСТА)");
        ((FnTlsInit)g_fn.tlsInit)();
        resolve_desc_ids(mask);
        int found = 0;
        for (int i = 0; i < g_descN; i++) {
            if (!g_descs[i].ptr) continue;
            found++;
            L("[тест] событие 0x%04X разм=%u (%s) -> %p  %s",
              g_descs[i].id, g_descs[i].size, g_descs[i].what, g_descs[i].ptr,
              rtti_short(g_descs[i].ptr));
        }
        // Шлём мы только тремя событиями. Остальные строки таблицы нужны для
        // расшифровки чужих, без них отправку запрещать не за что.
        bool core3 = g_descs[act_row(ACT_SEARCH)].ptr && g_descs[act_row(ACT_TAKE)].ptr
                                                      && g_descs[act_row(ACT_CATCH)].ptr;
        if (!q || !core3) { L("[тест] ПРОВАЛ: нет очереди или ключевых дескрипторов - не шлём"); return; }
        g_sendAllowed = true;
        L("[тест] ПОРЯДОК: дескрипторов %d из %d, отправка разрешена", found, g_descN);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        L("[тест] ИСКЛЮЧЕНИЕ 0x%08X - отправка остаётся запрещённой", GetExceptionCode());
    }
}

// ------------------------------------------- подслушивание чужих событий ----
// Игра шлёт свои события через ту же функцию. Перехватываем её и на несколько
// секунд записываем всё, что проходит: это эталон, с которым сверяем своё.

static volatile LONG g_spyUntil;      // GetTickCount, до которого пишем
static volatile LONG g_spyLeft;       // сколько ещё записей разрешено
static volatile LONG g_spyGen;    // номер сеанса записи, для счётчиков номеров
static volatile LONG g_spyUser;       // запись включена вручную, с F7
static volatile LONG g_dumpEid;       // выложить потроха этой сущности на такте

// Сторож записи в узел УБРАН. Замысел был верный - узел заряжается прямой
// записью в inter+0xE0, и поймать надо инструкцию, которая туда пишет, - но
// исполнение никуда не годилось: PAGE_GUARD ставится на всю страницу в 4 КБ,
// игра трогает её постоянно, и каждое обращение давало исключение, которое
// обработчик ещё и взводил заново. Игра встала намертво.
// --------------------------------------------- сторож записи в узел ---------
// Узел заряжается не событием, а прямой записью в inter+0xE0: две минуты
// записи очереди при подходе к жиле не дали НИ ОДНОГО события. Значит ловить
// надо инструкцию, которая туда пишет.
//
// Первая попытка (PAGE_GUARD на страницу) повесила игру: охрана ставится на
// все 4 КБ, игра трогает их постоянно, и каждое обращение давало исключение.
// А выгрузка логики оставила зарегистрированным обработчик, лежавший в самой
// dll, - отсюда и вылет. Теперь:
//   * следим ровно за 8 байтами через DR0/DR7 - посторонние обращения не
//     трогают нас вовсе;
//   * обработчик снимается в CoreStop, а не остаётся в выгруженном коде;
//   * сторож гаснет сам через 20 секунд или 8 попаданий.
static volatile LONG  g_watchOn;
static void          *g_watchAddr;
static DWORD          g_watchUntil;
static volatile LONG  g_watchHits;
static PVOID          g_watchVeh;

// Пройтись по всем потокам игры и записать в них отладочные регистры.
static void watch_apply(void *addr) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te; te.dwSize = sizeof te;
    DWORD me = GetCurrentThreadId(), pid = GetCurrentProcessId();
    int n = 0;
    if (Thread32First(snap, &te)) do {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == me) continue;
        HANDLE h = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME,
                              FALSE, te.th32ThreadID);
        if (!h) continue;
        SuspendThread(h);
        CONTEXT ctx; ZeroMemory(&ctx, sizeof ctx);
        ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        if (GetThreadContext(h, &ctx)) {
            if (addr) {
                ctx.Dr0 = (DWORD64)addr;
                // DR7: L0=1 (бит 0), RW0=01 запись (биты 16-17), LEN0=10 восемь байт
                ctx.Dr7 = (ctx.Dr7 & ~0xF0003ull) | 0x1 | (0x1 << 16) | (0x2 << 18);
            } else {
                ctx.Dr0 = 0;
                ctx.Dr7 &= ~0xF0003ull;
            }
            ctx.Dr6 = 0;
            if (SetThreadContext(h, &ctx)) n++;
        }
        ResumeThread(h);
        CloseHandle(h);
    } while (Thread32Next(snap, &te));
    CloseHandle(snap);
    L("[сторож] отладочные регистры %s в %d потоках", addr ? "взведены" : "сняты", n);
}

static LONG CALLBACK watch_veh(EXCEPTION_POINTERS *ep) {
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP)
        return EXCEPTION_CONTINUE_SEARCH;
    if (!InterlockedCompareExchange(&g_watchOn, 0, 0)) return EXCEPTION_CONTINUE_SEARCH;
    if (!(ep->ContextRecord->Dr6 & 0x1)) return EXCEPTION_CONTINUE_SEARCH;
    ep->ContextRecord->Dr6 = 0;
    LONG hits = InterlockedIncrement(&g_watchHits);
    L("[сторож] ЗАПИСЬ в узел: инструкция после +0x%llX",
      (unsigned long long)((unsigned char *)ep->ContextRecord->Rip - g_game.base));
    // Сама инструкция оказалась служебной присваивалкой массива - таких в
    // коде тысячи. Настоящий заказчик выше по стеку, поэтому пробегаем по
    // нему и выписываем всё, что похоже на обратные адреса внутрь образа
    // игры. Первые же из них и есть цепочка вызовов, заряжающая узел.
    __try {
        unsigned char **sp = (unsigned char **)ep->ContextRecord->Rsp;
        int found = 0;
        for (int i = 0; i < 128 && found < 8; i++) {
            unsigned char *v = sp[i];
            if (v <= g_game.base || v >= g_game.base + g_game.size) continue;
            // Перед обратным адресом должен стоять call: E8 rel32 (5 байт)
            // или FF /2 (2-6 байт). Проверяем самый частый случай.
            unsigned char *maybe = v - 5;
            bool isCall = readable(maybe, 1) && *maybe == 0xE8;
            L("        стек +%03d: +0x%llX%s", i * 8,
              (unsigned long long)(v - g_game.base), isCall ? "  <- вызов E8" : "");
            found++;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { }
    if (hits >= 8 || (LONG)GetTickCount() > (LONG)g_watchUntil)
        InterlockedExchange(&g_watchOn, 0);
    return EXCEPTION_CONTINUE_EXECUTION;
}

static void watch_stop(void) {
    if (!g_watchAddr && !g_watchVeh) return;
    InterlockedExchange(&g_watchOn, 0);
    if (g_watchAddr) { watch_apply(0); g_watchAddr = 0; }
    if (g_watchVeh) { RemoveVectoredExceptionHandler(g_watchVeh); g_watchVeh = 0; }
}

static void watch_start(void *field) {
    if ((uintptr_t)field & 7) { L("[сторож] адрес %p не выровнен - не берусь", field); return; }
    watch_stop();
    g_watchVeh = AddVectoredExceptionHandler(1, watch_veh);
    g_watchAddr = field;
    g_watchUntil = GetTickCount() + 20000;
    InterlockedExchange(&g_watchHits, 0);
    InterlockedExchange(&g_watchOn, 1);
    watch_apply(field);
    L("[сторож] слежу за %p, 20 секунд - подойдите к жиле", field);
}


// route игрока подсматриваем у самой игры: она ставит его в каждое своё
// событие. Ловим один раз и больше не трогаем - функция очень горячая.
static volatile LONG g_routeKnown;
static volatile LONG g_route;

// Имя события по указателю на дескриптор - из нашей же таблицы.
static const char *desc_name(void *d) {
    for (int i = 0; i < g_descN; i++)
        if (g_descs[i].ptr == d) return g_descs[i].what;
    return 0;
}

extern "C" void __fastcall cdloot_on_enqueue(void *q, void *ev, void *desc, void *z) {
    (void)q; (void)z;
    // Сначала route: он нужен всегда, а не только во время записи.
    if (!InterlockedCompareExchange(&g_routeKnown, 0, 0)) {
        __try {
            if (ev && readable(ev, 0x60)) {
                unsigned char *e = (unsigned char *)ev;
                uint32_t who = *(uint32_t *)(e + 0x50);
                uint32_t rt  = *(uint32_t *)(e + 0x58);
                if ((who >> 24) == EID_PLAYER_TAG && rt) {
                    InterlockedExchange(&g_route, (LONG)rt);
                    InterlockedExchange(&g_routeKnown, 1);
                    L("[route] подсмотрен у игры: игрок=%08X route=%08X", who, rt);
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { }
    }

    if (InterlockedCompareExchange(&g_spyLeft, 0, 0) <= 0) return;
    if ((LONG)GetTickCount() > InterlockedCompareExchange(&g_spyUntil, 0, 0)) return;
    // Свои же события не подслушиваем - иначе запас уходит на них.
    if ((DWORD)InterlockedCompareExchange(&g_sendTid, 0, 0) == GetCurrentThreadId()) return;
    __try {
        if (!ev || !readable(ev, 0x80)) return;
        unsigned char *e = (unsigned char *)ev;
        uint16_t size = *(uint16_t *)(e + 0x68);
        unsigned char *buf = *(unsigned char **)(e + 0x70);
        // Номер события лежит первым словом буфера - и у своих, и у чужих.
        uint16_t bufId = (buf && readable(buf, 2)) ? (uint16_t)(buf[0] | (buf[1] << 8)) : 0;
        // Раньше здесь стоял отсев по таблице известных номеров - против
        // спама 0x0804, который игра шлёт три раза в секунду. Но этим же
        // отсевом отбрасывалось ВСЁ незнакомое, а именно незнакомое мы и
        // ищем: взятие воды из ведра за целую запись не дало ни строчки,
        // потому что его событие в таблицу не входит.
        // Теперь пишем всё, но каждый НОМЕР - не больше трёх раз за сеанс.
        // Спам сам себя гасит после третьей строки, а новое событие видно.
        bool known = false;
        for (int i = 0; i < g_descN; i++) if (g_descs[i].id == bufId) { known = true; break; }
        // Лимит считаем по паре "номер + обработчик". 0x0804 -
        // TrocTrHandleGameEventOnceTimer - это ОБЩАЯ ШИНА: внутри неё ездят
        // разные игровые события, и отличает их указатель на обработчик,
        // лежащий сразу за заголовком. Лимит по одному лишь номеру схлопывал
        // всю шину в три строки и мог отрезать как раз нужное событие.
        // Ключ различения. У шины 0x0804 указатель класса один на всё
        // (144CF2388), а вид события лежит в БАЙТЕ 11 - в записи взятия воды
        // встретились 1A, 1C и 3E. По одному указателю виды сливались в одну
        // кучу и съедали общий лимит.
        uint64_t sub = 0;
        unsigned kind = 0xFFFF;
        if (buf && size >= 11 && readable(buf + 3, 8)) sub = *(uint64_t *)(buf + 3);
        if (buf && size >= 12 && readable(buf + 11, 1)) {
            kind = buf[11];
            sub = (sub << 8) | kind;
        }
        {
            static LONG gen = -1;
            static uint16_t seenId[192];
            static uint64_t seenSub[192];
            static uint8_t  seenCnt[192];
            static int      seenN = 0;
            LONG curGen = InterlockedCompareExchange(&g_spyGen, 0, 0);
            if (gen != curGen) { gen = curGen; seenN = 0; }
            int slot = -1;
            for (int i = 0; i < seenN; i++)
                if (seenId[i] == bufId && seenSub[i] == sub) { slot = i; break; }
            if (slot < 0 && seenN < 192) {
                slot = seenN++; seenId[slot] = bufId; seenSub[slot] = sub; seenCnt[slot] = 0;
            }
            if (slot >= 0) {
                if (seenCnt[slot] >= 20) return;     // эта пара уже показана двадцать раз
                seenCnt[slot]++;
            }
        }
        InterlockedDecrement(&g_spyLeft);
        const char *what = desc_name(desc);
        char hb[260], hp[420];
        hexs(e + 0x30, 0x50, hb, sizeof hb);
        int show = size > 128 ? 128 : size;   // 0x0804 - 115 байт, нужен весь
        hp[0] = 0;
        if (buf && show && readable(buf, show)) hexs(buf, show, hp, sizeof hp);
        L("[шпион] событие 0x%04X %s%s вид=0x%02X desc=%p размер=%u "
          "+0x50=%08X +0x58=%08X",
          bufId, what ? what : "(имя класса не прочиталось)",
          known ? "" : "   <<< НОВОЕ", kind, desc, size,
          *(uint32_t *)(e + 0x50), *(uint32_t *)(e + 0x58));
        L("        тело +0x30: %s", hb);
        L("        буфер:      %s%s", hp, size > 128 ? " ..." : "");
        // Ловля - единственное действие, которое мод пока не умеет опознать
        // сам. Раз игра только что поймала кого-то - запоминаем цель и на
        // ближайшем такте выкладываем её потроха.
        if (bufId == g_descs[act_row(ACT_CATCH)].id && size >= 7 && buf && readable(buf, 7))
            InterlockedExchange(&g_dumpEid, (LONG)*(uint32_t *)(buf + 3));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedExchange(&g_spyLeft, 0);
    }
}

// ----------------------------------------------------------- логика мода ----

static volatile LONG g_watchReq;
static volatile LONG g_armReq;
static volatile LONG g_armAimReq;   // зарядить именно тот узел, на который смотрю
static volatile LONG g_aimReq;
static volatile LONG g_camReq;
// Вариант разбора матрицы камеры. Перебирать в переписке бессмысленно -
// строк три, знака два, да ещё оси мира могут не совпадать с осями матрицы.
// Поэтому переключатель на клавишу: жмёшь и смотришь, когда метки сядут на
// предметы. 12 вариантов: строка (3) x знак (2) x порядок осей (2).
static volatile LONG g_aimVar;
static int g_aimRow, g_aimSgn, g_aimSwp;
static volatile LONG g_aimIdxEid;   // на что смотрит игрок
static char g_aimText[128];
static unsigned char *g_armFn;      // сама функция зарядки, найденная по сигнатуре
static volatile LONG  g_armMode;    // режим из подсмотренного вызова
typedef void (*FnArm)(void *, uintptr_t, void *, uintptr_t);

// Вызов зарядки вынесен в отдельную функцию: в area_body есть объекты с
// деструктором (секундомер), а __try с ними в одном теле не уживается.
static bool arm_call_now(void *inter, uintptr_t mode, void *scratch, uintptr_t eid);

// Возвращает: 1 - вызвали прямо сейчас, 2 - поставили в очередь (исполнит
// хук в ближайшем кадре), 0 - исключение внутри вызова.
// Различать обязательно: при отложенном вызове проверять результат СРАЗУ
// бессмысленно - данные узла ещё не появились. Раньше этого различия не было,
// и после переноса обхода в свой поток мод видел "без изменений" и заносил
// рудные жилы в чёрный список навсегда. Руда перестала собираться.
static int arm_call(void *inter, uintptr_t mode, void *scratch, uintptr_t eid) {
    if (!on_game_thread()) {
        pend_init();
        EnterCriticalSection(&g_pendCs);
        bool room = g_pendArmN < 32;
        if (room) {
            PendArm &q = g_pendArm[g_pendArmN++];
            q.node = inter; q.mode = mode; q.player = (uint32_t)eid;
            q.eid = InterlockedCompareExchange(&g_armLastEid, 0, 0);
        }
        LeaveCriticalSection(&g_pendCs);
        return room ? 2 : 0;         // 2 - отложено, исполнит хук
    }
    return arm_call_now(inter, mode, scratch, eid) ? 1 : 0;
}

static bool arm_call_now(void *inter, uintptr_t mode, void *scratch, uintptr_t eid) {
    __try {
        ((FnArm)g_armFn)(inter, mode, scratch, eid);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static volatile LONG g_scanRequested;
static volatile LONG g_nameRequested;
static volatile LONG g_takeRequested;
static volatile LONG g_aimTakeReq;  // взять ИМЕННО то, на что смотришь
static volatile LONG g_burstRequested;
static volatile LONG g_dryRequested;
static volatile LONG g_forceRequested;
static volatile LONG g_autoOn;          // непрерывный сбор включён
// Сколько чего собрано с загрузки логики - на плашку. Порядок как в Action.
static volatile LONG g_sess[4];
// Короткое уведомление на плашке: включили автосбор, собрали пачку. Живёт
// пару секунд и уступает место обычной сводке.
static char  g_notice[96];
static DWORD g_noticeUntil;
static int   g_notifyOn = 1;      // ключ BurstNotify, ставится в cfg_load

// Короткая табличка и системный звук - как в оригинальном AutoLoot:
// "Auto-Loot: ON" / "OFF" / "BURST" плюс MessageBeep, ключ BurstNotify.
// В готовой сборке это ВСЁ, что видно на экране: большая сводка со списком
// целей и отладкой - панель разработчика и показывается только при Debug=1.
// ЯЗЫК. Всё, что видит игрок в готовой сборке, - это табличка уведомления,
// поэтому двуязычие стоит ровно одну функцию выбора. Ключ Language в ini:
// EN, RU или AUTO (по языку системы). Логи и панель разработчика остаются
// русскими: их читает автор, а не игрок.
static int g_lang;                  // 0 - английский, 1 - русский
#define T(en, ru) (g_lang ? (ru) : (en))

static void notice(const char *text, UINT beep) {
    strncpy(g_notice, text, sizeof g_notice - 1);
    g_notice[sizeof g_notice - 1] = 0;
    g_noticeUntil = GetTickCount() + 2000;
    if (g_notifyOn) {
        InterlockedExchange(&g_hudOn, 1);          // табличку показать
        MessageBeep(beep);
    }
}
// Отладочный блок плашки: последнее отправленное событие и последний
// пойманный переход. Всё пишется и читается на игровом потоке.
static char  g_lastEvt[64];
static DWORD g_lastEvtAt;
static char  g_lastTrans[96];
static DWORD g_lastTransAt;
static volatile LONG g_running;

// ------------------------------------------------------------------ ini -----
// Настройки лежат рядом с модом, в CDLoot.ini, и читаются при каждой загрузке
// логики. Значит F10 перечитывает их без перезапуска игры: поправил радиус -
// нажал F10 - проверил. Ключи названы как в родном AutoLoot.ini, чтобы не
// заставлять никого учить второй словарь.
struct Cfg {
    int   enabled;
    int   lootCorpses, pickUpItems, gatherPlants, catchInsects;
    float scanRange;
    float minRange;         // ближе этого - это мы сами, не лут
    float range[4];         // свой радиус на каждое действие, по Action
    int   maxLootsPerSec;   // темп автосбора
    int   perTick;          // сколько объектов берёт автосбор за один обход
    int   burstPerKey;      // сколько объектов берёт одно нажатие F8
    int   retryAfterMs;     // через сколько разрешено повторить тот же объект
    int   autoMode;         // включать автосбор сразу при загрузке
    int   burstNotify;      // табличка и звук при включении и пачке
    int   autoArm;          // заряжать узлы самим, не дожидаясь игры
    float armRange;         // и не дальше этого - см. ArmRange в ini
    int   takeNoInstance;   // не используется: признак оказался неверным
    int   lootFurnitureName;// брать мебель и декор (по имени типа)
    int   lootTools;        // брать инструменты - лопату, кирку, мотыгу
    int   lootOwned;        // 1 - брать и чужое (розыск), 0 - обходить
    int   keyAuto;          // клавиша "автосбор вкл/выкл", код Windows
    int   keyBurst;         // клавиша "собрать пачку"
    int   keySteal;         // клавиша "взять то, на что смотрю"
    // Геймпад идёт ПАРАЛЛЕЛЬНО клавиатуре: свои ключи, свои значения, работают
    // одновременно. Не "одна переменная, куда можно вписать хоть то, хоть то" -
    // человеку с геймпадом клавиатура всё равно нужна, и наоборот.
    int   padAuto, padBurst, padSteal;
    int   armContainers;    // заряжать ли ёмкости механизмов (лутать - нет)
    int   skipCat11;        // опыт: отсеивать объекты с категорией 0x11
};
static Cfg g_cfg;
static char g_cfgPath[MAX_PATH];
static char g_cfgDir[MAX_PATH];   // чтобы перечитывать файл без лишних рук

static float ini_f(const char *key, float def) {
    char buf[64], dbuf[64];
    sprintf(dbuf, "%g", def);
    GetPrivateProfileStringA("CDLoot", key, dbuf, buf, sizeof buf, g_cfgPath);
    return (float)atof(buf);
}
static int ini_i(const char *key, int def) {
    return (int)GetPrivateProfileIntA("CDLoot", key, def, g_cfgPath);
}

// ------------------------------------------------------------- геймпад ------
// XInput подгружаем сами: так мод не требует ни библиотеки при сборке, ни
// наличия геймпада у игрока. Нет контроллера - опрос просто всегда даёт ноль.
// Кнопки геймпада прячем в код клавиши старшим битом, чтобы всё остальное -
// разбор имени, сравнение, хранение - работало без изменений.
#define PAD_FLAG 0x10000

#define PAD_DUP     0x0001
#define PAD_DDOWN   0x0002
#define PAD_DLEFT   0x0004
#define PAD_DRIGHT  0x0008
#define PAD_START   0x0010
#define PAD_BACK    0x0020
#define PAD_LS      0x0040
#define PAD_RS      0x0080
#define PAD_LB      0x0100
#define PAD_RB      0x0200
#define PAD_LT      0x0400      // курки аналоговые, биты свободны - берём их
#define PAD_RT      0x0800
#define PAD_A       0x1000
#define PAD_B       0x2000
#define PAD_X       0x4000
#define PAD_Y       0x8000

struct XPad { WORD buttons; BYTE lt, rt; SHORT lx, ly, rx, ry; };
struct XState { DWORD packet; XPad pad; };
typedef DWORD (WINAPI *FnXInputGetState)(DWORD, XState *);
static FnXInputGetState g_xinput;
static bool  g_xinputTried;

// Состояние кнопок, с кэшем: опрос отсутствующего контроллера стоит дорого,
// поэтому при неудаче не дёргаем чаще раза в секунду.
static WORD pad_buttons(void) {
    static WORD  cached;
    static DWORD cachedAt, failAt;
    DWORD now = GetTickCount();
    if (now - cachedAt < 15) return cached;
    if (!g_xinputTried) {
        g_xinputTried = true;
        static const wchar_t *dlls[] = { L"xinput1_4.dll", L"xinput1_3.dll",
                                         L"xinput9_1_0.dll" };
        for (int i = 0; i < 3 && !g_xinput; i++) {
            HMODULE m = LoadLibraryW(dlls[i]);
            if (m) g_xinput = (FnXInputGetState)GetProcAddress(m, "XInputGetState");
        }
        L("[геймпад] XInput %s", g_xinput ? "подключён" : "не найден");
    }
    if (!g_xinput) return 0;
    if (failAt && now - failAt < 1000) return 0;
    cachedAt = now;
    XState st; memset(&st, 0, sizeof st);
    for (DWORD i = 0; i < 4; i++) {
        if (g_xinput(i, &st) != 0) continue;          // 0 = ERROR_SUCCESS
        WORD b = st.pad.buttons;
        if (st.pad.lt > 60) b |= PAD_LT;              // курок нажат больше чем на четверть
        if (st.pad.rt > 60) b |= PAD_RT;
        cached = b; failAt = 0;
        return cached;
    }
    failAt = now; cached = 0;
    return 0;
}

// Нажата ли клавиша - хоть на клавиатуре, хоть на геймпаде. Для сочетания
// требуются ВСЕ его кнопки разом.
static bool key_down(int code) {
    if (!code) return false;
    if (code & PAD_FLAG) {
        WORD want = (WORD)(code & 0xFFFF);
        return want && (pad_buttons() & want) == want;
    }
    return (GetAsyncKeyState(code) & 0x8000) != 0;
}

// Имя клавиши из ini в код Windows. Понимает F1..F24, NUM0..NUM9, одиночные
// буквы и цифры и десяток именованных клавиш. Не разобрали - берём умолчание
// и говорим об этом в логе: молча подсунуть не ту клавишу хуже, чем сказать.
static int vk_from_name(const char *key, int def) {
    char s[32];
    GetPrivateProfileStringA("CDLoot", key, "", s, sizeof s, g_cfgPath);
    // обрезаем пробелы по краям
    char *p = s; while (*p == ' ' || *p == '\t') p++;
    int n = (int)strlen(p);
    while (n > 0 && (p[n-1] == ' ' || p[n-1] == '\t' || p[n-1] == '\r')) p[--n] = 0;
    if (!*p) return def;
    if (!_stricmp(p, "NONE") || !_stricmp(p, "-")) return 0;   // не назначено

    // Геймпад: PAD_A, PAD_LB, можно сочетанием через плюс - PAD_LB+PAD_A.
    if (!_strnicmp(p, "PAD", 3)) {
        static const struct { const char *n; int m; } pads[] = {
            { "PAD_A", PAD_A }, { "PAD_B", PAD_B }, { "PAD_X", PAD_X }, { "PAD_Y", PAD_Y },
            { "PAD_LB", PAD_LB }, { "PAD_RB", PAD_RB },
            { "PAD_LT", PAD_LT }, { "PAD_RT", PAD_RT },
            { "PAD_LS", PAD_LS }, { "PAD_RS", PAD_RS },
            { "PAD_BACK", PAD_BACK }, { "PAD_START", PAD_START },
            { "PAD_UP", PAD_DUP }, { "PAD_DOWN", PAD_DDOWN },
            { "PAD_LEFT", PAD_DLEFT }, { "PAD_RIGHT", PAD_DRIGHT },
        };
        int mask = 0;
        char tmp[32]; strncpy(tmp, p, sizeof tmp - 1); tmp[sizeof tmp - 1] = 0;
        for (char *tok = strtok(tmp, "+"); tok; tok = strtok(0, "+")) {
            while (*tok == ' ') tok++;
            int n2 = (int)strlen(tok);
            while (n2 > 0 && tok[n2-1] == ' ') tok[--n2] = 0;
            int found = 0;
            for (int i = 0; i < (int)(sizeof pads / sizeof pads[0]); i++)
                if (!_stricmp(tok, pads[i].n)) { mask |= pads[i].m; found = 1; break; }
            if (!found) { L("[ini] %s: кнопка \"%s\" не понята", key, tok); return def; }
        }
        if (mask) return PAD_FLAG | mask;
        return def;
    }
    if ((p[0] == 'F' || p[0] == 'f') && p[1] >= '0' && p[1] <= '9') {
        int f = atoi(p + 1);
        if (f >= 1 && f <= 24) return VK_F1 + f - 1;
    }
    if (!_strnicmp(p, "NUM", 3) && p[3] >= '0' && p[3] <= '9' && !p[4])
        return VK_NUMPAD0 + (p[3] - '0');
    if (!p[1]) {
        char c = (char)toupper((unsigned char)p[0]);
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return c;
    }
    static const struct { const char *name; int vk; } named[] = {
        { "HOME", VK_HOME }, { "END", VK_END }, { "INSERT", VK_INSERT },
        { "DELETE", VK_DELETE }, { "PAGEUP", VK_PRIOR }, { "PAGEDOWN", VK_NEXT },
        { "TAB", VK_TAB }, { "BACKSPACE", VK_BACK }, { "SPACE", VK_SPACE },
        { "NUMMULT", VK_MULTIPLY }, { "NUMPLUS", VK_ADD },
        { "NUMMINUS", VK_SUBTRACT }, { "NUMDIV", VK_DIVIDE },
        { "NUMDOT", VK_DECIMAL }, { "SCROLLLOCK", VK_SCROLL },
        { "PAUSE", VK_PAUSE },
        // Мышь. Левая и правая намеренно НЕ предлагаются: их занимает сама
        // игра. Дальше пятой Windows кнопок не различает - шестая и
        // последующие существуют только внутри драйвера мыши и до
        // GetAsyncKeyState не доходят.
        { "MOUSE3", VK_MBUTTON }, { "MOUSE4", VK_XBUTTON1 },
        { "MOUSE5", VK_XBUTTON2 },
    };
    for (int i = 0; i < (int)(sizeof named / sizeof named[0]); i++)
        if (!_stricmp(p, named[i].name)) return named[i].vk;

    L("[ini] клавиша %s=\"%s\" не разобрана, оставляю прежнюю", key, p);
    return def;
}

static void cfg_load(const char *dir) {
    sprintf(g_cfgPath, "%sCDLoot.ini", dir);
    strncpy(g_cfgDir, dir, sizeof g_cfgDir - 1);
    bool have = GetFileAttributesA(g_cfgPath) != INVALID_FILE_ATTRIBUTES;

    g_cfg.enabled      = ini_i("Enabled", 1);
    g_debug            = ini_i("Debug", 1) != 0;
    {   // Language=EN | RU | AUTO. По умолчанию EN: языков в мире много, а у
        // нас их два, и для китайца, немца или японца английский - разумный
        // общий знаменатель. AUTO даёт русский только на русской системе,
        // всем остальным тот же английский.
        char lang[16] = "";
        GetPrivateProfileStringA("CDLoot", "Language", "EN", lang, sizeof lang, g_cfgPath);
        if (lang[0] == 'r' || lang[0] == 'R') g_lang = 1;
        else if (lang[0] == 'e' || lang[0] == 'E') g_lang = 0;
        else g_lang = (PRIMARYLANGID(GetUserDefaultUILanguage()) == 0x19) ? 1 : 0;
    }
    g_cfg.lootCorpses  = ini_i("LootCorpses", 1);
    g_cfg.pickUpItems  = ini_i("PickUpItems", 1);
    g_cfg.gatherPlants = ini_i("GatherPlants", 1);
    g_cfg.catchInsects = ini_i("CatchInsects", 1);
    // Радиус обзора шире рабочих: в списке должно быть видно чуть больше, чем
    // мод трогает, иначе не понять, что он пропустил и почему.
    g_cfg.scanRange           = ini_f("ScanRange", 40.0f);
    g_cfg.minRange            = ini_f("MinRange", 0.35f);
    g_cfg.range[ACT_TAKE]     = ini_f("LootRange", 15.0f);
    g_cfg.range[ACT_GATHER]   = ini_f("GatherRange", 20.0f);
    g_cfg.range[ACT_CATCH]    = ini_f("CatchRange", 8.0f);
    g_cfg.range[ACT_SEARCH]   = ini_f("CorpseRange", 12.0f);
    g_cfg.maxLootsPerSec = ini_i("MaxLootsPerSec", 5);
    g_cfg.perTick        = ini_i("PerTick", 3);
    g_cfg.burstPerKey  = ini_i("BurstPerKey", 1);
    g_cfg.retryAfterMs = ini_i("RetryAfterMs", 6000);
    g_cfg.autoMode     = ini_i("AutoMode", 0);
    g_cfg.burstNotify  = ini_i("BurstNotify", 1);
    g_notifyOn         = g_cfg.burstNotify;
    g_cfg.autoArm      = ini_i("AutoArm", 1);
    // Зарядка узлов - самая пробивная часть мода: она заставляет игру считать
    // взаимодействие доступным там, где та сама бы его не дала. Стен эта
    // функция не видит, поэтому с общим радиусом сбора (20 м) мод дотягивался
    // до вещей в соседней комнате и на улице. Свой радиус, заметно короче.
    // 0 - без ограничения, как у прочих радиусов.
    g_cfg.armRange     = ini_f("ArmRange", 8.0f);
    g_cfg.keyAuto      = vk_from_name("KeyAutoLoot", VK_F10);
    g_cfg.keyBurst     = vk_from_name("KeyBurst",    VK_F11);
    g_cfg.keySteal     = vk_from_name("KeyTakeAimed", VK_NUMPAD0);
    g_cfg.padAuto      = vk_from_name("PadAutoLoot",  0);
    g_cfg.padBurst     = vk_from_name("PadBurst",     0);
    g_cfg.padSteal     = vk_from_name("PadTakeAimed", 0);
    g_cfg.armContainers = ini_i("ArmContainers", 1);
    InterlockedExchange(&g_marksOn, ini_i("ShowMarks", 0) ? 1 : 0);
    InterlockedExchange(&g_offThread, ini_i("ScanOffThread", 1) ? 1 : 0);
    g_cfg.takeNoInstance = ini_i("TakeNoInstance", 1);
    g_cfg.lootFurnitureName = ini_i("LootFurniture", 0);
    g_cfg.lootTools    = ini_i("LootTools", 0);
    g_cfg.lootOwned    = ini_i("LootOwned", 0);
    g_cfg.skipCat11    = ini_i("SkipCat11", 0);

    // 0 везде означает одно и то же - БЕЗ ОГРАНИЧЕНИЯ. Раньше ноль в темпе
    // молча превращался в единицу, то есть в самый медленный режим: человек
    // писал 0, ожидая "жми на всю", а получал один обход в секунду.
    if (g_cfg.maxLootsPerSec < 0)  g_cfg.maxLootsPerSec = 0;
    if (g_cfg.maxLootsPerSec > 20) g_cfg.maxLootsPerSec = 20;
    // 0 - без ограничения: берём всё, что разрешено, за один обход.
    if (g_cfg.perTick < 0)  g_cfg.perTick = 0;
    if (g_cfg.perTick > 64) g_cfg.perTick = 64;
    if (g_cfg.burstPerKey < 0)     g_cfg.burstPerKey = 0;   // 0 - всё разом
    if (g_cfg.burstPerKey > 32)    g_cfg.burstPerKey = 32;
    if (g_cfg.scanRange < 1.0f)    g_cfg.scanRange = 1.0f;
    if (g_cfg.scanRange > 200.0f)  g_cfg.scanRange = 200.0f;
    // Радиус больше обзора смысла не имеет: в список объект просто не попадёт.
    for (int i = 0; i < 4; i++) {
        if (g_cfg.range[i] < 0.0f) g_cfg.range[i] = 0.0f;
        if (g_cfg.range[i] > g_cfg.scanRange) g_cfg.range[i] = g_cfg.scanRange;
    }

    L("[ini] %s%s", g_cfgPath, have ? "" : " - файла нет, взяты значения по умолчанию");
    L("[ini] обзор %.0f (ближе %.2f - это мы) | вещи %.0f | сбор %.0f | ловля %.0f | туши %.0f м",
      g_cfg.scanRange, g_cfg.minRange, g_cfg.range[ACT_TAKE], g_cfg.range[ACT_GATHER],
      g_cfg.range[ACT_CATCH], g_cfg.range[ACT_SEARCH]);
    L("[ini] трупы=%d предметы=%d растения=%d ловля=%d | темп %s | пачка %s | повтор через %d мс",
      g_cfg.lootCorpses, g_cfg.pickUpItems, g_cfg.gatherPlants, g_cfg.catchInsects,
      g_cfg.maxLootsPerSec ? "по числу в секунду" : "КАЖДЫЙ КАДР",
      g_cfg.burstPerKey ? "по числу" : "всё разом", g_cfg.retryAfterMs);
    if (g_cfg.maxLootsPerSec)
        L("[ini] обходов в секунду: %d (шаг %d мс)", g_cfg.maxLootsPerSec,
          1000 / g_cfg.maxLootsPerSec);
    if (g_cfg.perTick) L("[ini] за обход: до %d объектов, итого до %d в секунду",
                         g_cfg.perTick, g_cfg.perTick * g_cfg.maxLootsPerSec);
    else L("[ini] за обход: БЕЗ ОГРАНИЧЕНИЯ - всё, что разрешено");
    L("[ini] ёмкости механизмов: %s (лутать - никогда)",
      g_cfg.armContainers ? "заряжаем" : "не трогаем");
    L("[ini] зарядка узлов: %s, не дальше %.0f м",
      g_cfg.autoArm ? "своя" : "только игра",
      g_cfg.armRange > 0.0f ? g_cfg.armRange : g_cfg.range[ACT_GATHER]);
    L("[ini] чужое: %s | отсев 0x11: %s",
      g_cfg.lootOwned ? "БРАТЬ (будет розыск)" : "обходить",
      g_cfg.skipCat11 ? "включён" : "выключен");
}

// Перечитать настройки, если файл изменился. Иначе выходит ловушка: правишь
// ini, проверяешь в игре - а мод живёт со старыми значениями, потому что
// прочитал их один раз при запуске. Ровно на этом ведро уехало в инвентарь:
// запрет был вписан в файл, но не прочитан. Сверяем время записи раз в две
// секунды - это одно обращение к файловой системе, не заметное ни на чём.
static void cfg_watch(void) {
    static ULONGLONG lastCheck = 0;
    static FILETIME  lastWrite = { 0, 0 };
    ULONGLONG now = GetTickCount64();
    if (now - lastCheck < 2000) return;
    lastCheck = now;
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExA(g_cfgPath, GetFileExInfoStandard, &fa)) return;
    if (lastWrite.dwLowDateTime == 0 && lastWrite.dwHighDateTime == 0) {
        lastWrite = fa.ftLastWriteTime;      // первый заход - просто запомнить
        return;
    }
    if (CompareFileTime(&lastWrite, &fa.ftLastWriteTime) == 0) return;
    lastWrite = fa.ftLastWriteTime;
    L("[ini] файл изменился - перечитываю");
    cfg_load(g_cfgDir);
}

// Чтобы не долбить один и тот же объект: игра убирает его не мгновенно, и
// он ещё несколько кадров виден в списке.
// ------------------------------------------------------- инвентарь игрока ---
// Прямой ответ на вопрос "эта вещь моя?": собрать номера экземпляров всего,
// что лежит у игрока, и сверять с номером вещи в мире. Номер экземпляра
// переживает пересоздание сущности, поэтому сверка надёжнее и расстояния, и
// кучности, и чёрного списка по eid.
//
// Путь от персонажа к слотам (разбор Trinity, подтверждён на живом держателе -
// в нём по +0x08 лежит указатель обратно на нашего актора):
//     держатель = *(*(актор+0x68)+0xB8)
//     держатель +0x18  -> массив корзин, +0x20 u32 их число
//     корзина   +0x00  -> массив слотов, +0x08 u16 их число
//     слот      шаг 0xC8, +0x00 номер экземпляра, +0x08 u16 тип предмета
#define INV_HOLDER     0xB8
#define INV_BUCKETS    0x18
#define INV_BUCKETN    0x20
#define INV_SLOTS      0x00
#define INV_SLOTN      0x08
#define INV_SLOT_SIZE  0xC8
#define INV_SLOT_TYPE  0x08

static uint32_t g_invIds[1024];
static int      g_invN;
static DWORD    g_invAt;

static const char *item_type_name(uint16_t typeId);   // объявление: нужно раньше тела

static void inventory_refresh(unsigned char *me, bool verbose) {
    DWORD now = GetTickCount();
    // Кэш на полсекунды - чтобы не читать сумку каждый такт. Но по обзору
    // (F9) читаем всегда: иначе дамп сумки не выводится вовсе.
    if (!me || (!verbose && g_invN && now - g_invAt < 500)) return;
    g_invAt = now;
    int n = 0, buckets = 0, slots = 0;
    __try {
        void *sub = deref(me, ENT_SUBOBJ);
        void *holder = sub ? deref(sub, INV_HOLDER) : 0;
        if (!holder || !readable(holder, INV_BUCKETN + 4)) return;
        unsigned char *h = (unsigned char *)holder;
        void **barr = *(void ***)(h + INV_BUCKETS);
        uint32_t bn = *(uint32_t *)(h + INV_BUCKETN);
        if (!barr || bn > 64 || !readable(barr, bn * 8)) return;
        for (uint32_t b = 0; b < bn && n < 1024; b++) {
            unsigned char *bk = (unsigned char *)barr[b];
            if (!bk || !readable(bk, 0x20)) continue;
            buckets++;
            unsigned char *sl = *(unsigned char **)(bk + INV_SLOTS);
            uint16_t sn = *(uint16_t *)(bk + INV_SLOTN);
            if (!sl || sn > 4096 || !readable(sl, (size_t)sn * INV_SLOT_SIZE)) continue;
            for (uint16_t i = 0; i < sn && n < 1024; i++) {
                unsigned char *e = sl + (size_t)i * INV_SLOT_SIZE;
                uint16_t type = *(uint16_t *)(e + INV_SLOT_TYPE);
                if (type == 0xFFFF || type == 0) continue;      // пустой слот
                uint32_t iid = *(uint32_t *)e;
                if (!iid || iid == 0xFFFFFFFF) continue;
                g_invIds[n++] = iid;
                slots++;
                // По просьбе: выложить сумку с номерами типов и именами.
                // Нужно, чтобы найти конкретную вещь (Axiom Bracelet) и
                // поставить на неё прямой запрет по имени.
                // Дамп сумки убран: он выкладывал тысячу строк на каждый
                // обзор и топил в себе всё остальное. Своё он отработал -
                // по нему нашлись Visione_Chip и семейство AbyssGear.
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { InterlockedIncrement(&g_faults); return; }
    g_invN = n;
    if (verbose) L("  [инвентарь] корзин %d, занятых слотов %d, номеров собрано %d",
                   buckets, slots, n);
}

static bool inventory_has(uint32_t iid) {
    if (!iid) return false;
    for (int i = 0; i < g_invN; i++) if (g_invIds[i] == iid) return true;
    return false;
}

// ---------------------------------------------- имена типов предметов -------
// У объекта в мире имени нет: искали вглубь по сущности, sub, узлу и
// компоненте, в два уровня - пусто. Имя живёт только у ТИПА, в таблице игры
// "iteminfo", и достаётся по номеру типа из данных узла.
//
// Как нашли таблицу: строка "iteminfo" лежит в данных, на неё ссылается
// `lea r8,[rip+..]` по +0x317952, а ближайший резолвер начинается в 82 байтах
// выше, по +0x317900. Из его пролога и читается глобал с таблицей - он же
// последние четыре байта сигнатуры ниже. Раскладка взята у Trinity:
//     таблица +0x08 u32  число записей
//     таблица +0x58 ptr  массив описаний, описание = *(defs + 8*typeId)
//     описание +0x08 ptr -> объект-строка, первое поле которого char*
#define ITEM_TABLE_COUNT 0x08
#define ITEM_TABLE_DEFS  0x58
#define ITEM_DEF_KEY     0x08

static void **g_itemTable;      // глобал, а не сама таблица: её адрес живой
// Вторая таблица - механизмы. У части объектов номер типа лежит ВНЕ
// iteminfo (ведро с водой - 52920, рудная жила - 52923), и по ней имени не
// выходит вовсе. В образе игры больше сотни таких таблиц, и одна из них
// называется gimmickinfo: механизмы, гиммики, точки взаимодействия. Номера
// вида 0xCExx - индексы в ней.
static void **g_gimmickTable;

// Резолверов таблиц в игре больше сотни, и все собраны по одному шаблону -
// сигнатурой их не различить. Зато нужный ССЫЛАЕТСЯ на строку "iteminfo":
// в его теле стоит `lea r8,[rip+..]`, ведущий на неё. По этому и отбираем.
// Все таблицы игры разом. Раньше резолвер искал по одной за полный проход по
// образу в 360 МБ - за две таблицы это две минуты работы впустую. Теперь один
// проход собирает всё, что нашлось, и любую таблицу можно спросить по имени.
struct GameTab { const char *name; void **glob; };
static GameTab g_tabs[256];
static int     g_tabN;

static void scan_tables(void) {
    static const unsigned char pro[] = {
        0x48,0x89,0x5C,0x24,0x10, 0x48,0x89,0x6C,0x24,0x18, 0x56,0x57,0x41,0x56,
        0x48,0x83,0xEC,0x50, 0x0F,0xB7,0x39, 0x48,0x8B,0x1D };
    int checked = 0;
    unsigned char *end = g_game.base + g_game.size - 0x200;
    for (unsigned char *cur = g_game.base; cur < end && g_tabN < 256; cur++) {
        if (memcmp(cur, pro, sizeof pro) != 0) continue;
        checked++;
        // В теле каждого резолвера стоит lea r8,[rip -> "<имя таблицы>"].
        const char *nm = 0;
        for (int off = 0; off < 0x180 && !nm; off++) {
            unsigned char *q = cur + off;
            if (q[0] != 0x4C || q[1] != 0x8D || q[2] != 0x05) continue;
            int32_t d = *(int32_t *)(q + 3);
            char *str = (char *)(q + 7 + d);
            if (!readable(str, 6)) continue;
            int n = 0;
            while (n < 40 && readable(str + n, 1) && str[n] >= 'a' && str[n] <= 'z') n++;
            if (n < 5 || !readable(str + n, 1) || str[n] != 0) continue;
            if (n < 4 || memcmp(str + n - 4, "info", 4) != 0) continue;
            nm = str;
        }
        if (!nm) continue;
        unsigned char *mov = cur + 0x15;          // mov rbx, cs:<глобал>
        int32_t disp = *(int32_t *)(mov + 3);
        bool dup = false;
        for (int i = 0; i < g_tabN; i++) if (!strcmp(g_tabs[i].name, nm)) dup = true;
        if (dup) continue;
        g_tabs[g_tabN].name = nm;
        g_tabs[g_tabN].glob = (void **)(mov + 7 + disp);
        g_tabN++;
    }
    L("[имена] резолверов проверено %d, таблиц найдено %d", checked, g_tabN);
}

static void **table_by_name(const char *want) {
    for (int i = 0; i < g_tabN; i++)
        if (!strcmp(g_tabs[i].name, want)) return g_tabs[i].glob;
    return 0;
}

static void resolve_item_table(void) {
    scan_tables();
    g_itemTable    = table_by_name("iteminfo");
    g_gimmickTable = table_by_name("gimmickinfo");
    L("[имена] iteminfo %s, gimmickinfo %s",
      g_itemTable ? "есть" : "НЕТ", g_gimmickTable ? "есть" : "НЕТ");
}

// Имя типа предмета или 0. Таблица опознаёт себя сама: если по номеру не
// вышло осмысленной строки, значит глобал не тот, и мы просто молчим.
// Сколько записей в таблице, или 0 если её ещё нет. Нужно, чтобы понимать,
// индекс перед нами или что-то другое: номер 52920 при таблице в семь тысяч
// записей - это не индекс, а идентификатор другой природы.
static uint32_t table_count(void **globalPtr) {
    if (!globalPtr || !readable(globalPtr, 8)) return 0;
    __try {
        unsigned char *tab = (unsigned char *)*globalPtr;
        if (!tab || !readable(tab, ITEM_TABLE_DEFS + 8)) return 0;
        return *(uint32_t *)(tab + ITEM_TABLE_COUNT);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

static const char *table_name(void **globalPtr, uint32_t typeId) {
    if (!globalPtr || !readable(globalPtr, 8)) return 0;
    __try {
        unsigned char *tab = (unsigned char *)*globalPtr;
        if (!tab || !readable(tab, ITEM_TABLE_DEFS + 8)) return 0;
        uint32_t count = *(uint32_t *)(tab + ITEM_TABLE_COUNT);
        if (!count || count > 0x40000 || typeId >= count) return 0;
        void **defs = *(void ***)(tab + ITEM_TABLE_DEFS);
        if (!defs || !readable(defs + typeId, 8)) return 0;
        unsigned char *def = (unsigned char *)defs[typeId];
        if (!def || !readable(def, ITEM_DEF_KEY + 8)) return 0;
        char **strObj = *(char ***)(def + ITEM_DEF_KEY);
        if (!strObj || !readable(strObj, 8)) return 0;
        char *sp = *strObj;
        if (!sp || !readable(sp, 4)) return 0;
        int n = 0;
        while (n < 63 && readable(sp + n, 1) && sp[n] >= 0x20 && sp[n] < 0x7F) n++;
        if (n < 3 || sp[n] != 0) return 0;
        return sp;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

// Имя механизма по номеру из данных узла. Сюда попадают ведро с водой,
// рудные жилы и прочее, чего в iteminfo нет.
static const char *gimmick_type_name(uint32_t typeId) {
    if (!typeId) return 0;
    return table_name(g_gimmickTable, typeId);
}

static const char *item_type_name(uint16_t typeId) {
    // Тип 0 - это НЕ предмет, а "типа нет". По нулевому индексу таблица
    // отдаёт свою первую запись, и мод бодро подписывал такие объекты
    // Pyeonjeon_Arrow - имя, к делу не относящееся вовсе. Для списка запретов
    // это яд: запретишь стрелу и не запретишь ничего.
    if (!typeId) return 0;
    return table_name(g_itemTable, typeId);
}

// Имя УЗЛА. Живёт по inter+0x68 объектом-строкой: указатель ведёт на
// структуру, первое поле которой - char*. Ценно тем, что есть даже у
// незаряженных узлов, у которых нет ни номера типа, ни имени предмета:
// "Furniture_Item_Dropset_0" у мебели с выпадающим набором вещей. Это
// единственный способ отличить сундук, ящик и visione-триггер до того, как
// мы их тронем.
#define NODE_NAME 0x68

// rtti_class под защитой: в area_body живёт секундомер с деструктором, а
// __try с ним в одном теле не уживается.
// Спросить у игры, на что наведён игрок. Поле +0x20 в компоненте оказалось
// не целью: обработчик сперва ЗОВЁТ функцию, и та кладёт ответ на стек.
//     mov rcx,rdi ; lea rdx,[rsp+48] ; mov r8b,1 ; call <query>
// Сама функция - переходник в упакованную секцию, сигнатурой не взять.
// Зато адрес достаётся по цепочке: дескриптор 0x080D -> таблица методов ->
// метод[2] -> вызов на фиксированном смещении +0x24 внутри него.
typedef void (*FnAimQuery)(void *, uint32_t *, uintptr_t);
static FnAimQuery g_aimQuery;

static void resolve_aim_query(void) {
    void *d = 0;
    for (int i = 0; i < g_descN; i++)
        if (g_descs[i].id == 0x080D) d = g_descs[i].ptr;
    if (!d || !readable(d, 8)) return;
    void **vt = *(void ***)d;
    if (!vt || !readable(vt, 8 * 4)) return;
    unsigned char *m = (unsigned char *)vt[2];
    if (!m || m < g_game.base || m >= g_game.base + g_game.size) return;
    if (!readable(m + 0x24, 5) || m[0x24] != 0xE8) return;
    int32_t rel = *(int32_t *)(m + 0x25);
    unsigned char *fn = m + 0x24 + 5 + rel;
    if (fn < g_game.base || fn >= g_game.base + g_game.size) return;
    g_aimQuery = (FnAimQuery)fn;
    L("[цель] опрос наведения = +0x%llX (из метода +0x%llX)",
      (unsigned long long)(fn - g_game.base), (unsigned long long)(m - g_game.base));
}

// Буфер под ответ - с большим запасом и обнулённый. Раньше сюда передавался
// указатель на четыре байта, а функция вполне могла писать больше: это порча
// чужого стека, и то, что обошлось, - везение, а не правильность.
static unsigned char g_aimOut[128];

static uint32_t ask_aim(void *ic) {
    if (!g_aimQuery || !ic) return 0;
    memset(g_aimOut, 0, sizeof g_aimOut);
    __try { g_aimQuery(ic, (uint32_t *)g_aimOut, 1); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0xFFFFFFFF; }
    return *(uint32_t *)g_aimOut;
}

// ------------------------------------------------------- поиск камеры ------
// Попытка найти камеру. Наведённой цели в структурах игрока нет - проверено
// четырьмя способами, - и я предположил, что игра считает её лучом из
// камеры. Поиск ниже находит матрицы, которые едут за игроком и
// поворачиваются, но камерой они не оказались: направление из них не
// совпадает с настоящим взглядом. Итог опыта: ОТРИЦАТЕЛЬНЫЙ.
//
// Камера всегда рядом с игроком и несёт ортонормированный поворот: три
// вектора длиной 1, перпендикулярные друг другу. Случайные числа такого не
// дают. Плюс проверка временем: настоящая камера ездит следом, а совпавший
// мусор остаётся на месте.
struct CamHit { unsigned char *addr; Vec3 pos; float fwd[3]; };
static unsigned char *g_cam[16];
static unsigned char *g_camTf;   // сам трансформ: в нём масштабы проекции
static int            g_camN;

static CamHit g_camHits[64];
static int    g_camHitN;

// ПОЗИЦИЯ КАМЕРЫ. Разобран извлекатель ESP (+0xF460): он копирует из
// трансформа камеры 0x120 байт и раскладывает их в свой снимок так:
//     трансформ +0x88 .. +0xC7  -> матрица 4x4 (её три СТОЛБЦА - оси)
//     трансформ +0xC8, +0xCC, +0xD0 -> позиция камеры в мире
//     трансформ +0x08 -> масштаб проекции по X
//     трансформ +0x1C -> масштаб проекции по Y
// Сама проекция (ESP +0x7BE0) вычитает из позиции предмета ИМЕННО эту
// позицию камеры, а не позицию игрока. Мой прежний луч шёл от ног игрока -
// отсюда и весь промах меток по вертикали: камера висит выше и позади, и
// чем сильнее она наклонена, тем дальше уезжали метки.
// ВАЖНО: в +0xC8 позиция АБСОЛЮТНАЯ мировая, а весь мод живёт в локальных
// координатах (объект плюс родитель). В Бездне разница вышла ровно
// (-10000, 0, -3000) - круглый сдвиг сектора мира, - и метки, посчитанные от
// такого "глаза", улетали за горизонт. Но в том же трансформе лежит и
// локальная копия, на 0x24 дальше:
//     +0xC8  позиция в абсолютных мировых (её и берёт ESP)
//     +0xEC  та же камера в кадре, в котором мы считаем всё остальное
//     +0x114 снова абсолютная
// Порядок перебора именно такой: сначала локальная, потом абсолютная (в
// зонах без сдвига сектора они совпадают, и тогда годится любая). Годной
// считаем ту, что лежит в 30 м от игрока: камера не может быть дальше, а
// врать на километры хуже, чем вернуться к лучу от игрока.
static bool cam_origin(const Vec3 &me, Vec3 *out) {
    static const int cand[] = { 0xEC, 0xC8, 0x114 };
    if (!g_camTf || !readable(g_camTf, 0x120)) return false;
    for (int k = 0; k < 3; k++) {
        const float *c = (const float *)(g_camTf + cand[k]);
        bool ok = true;
        for (int i = 0; i < 3; i++)
            if (!(c[i] > -1e7f && c[i] < 1e7f)) ok = false;   // отсекает и nan
        if (!ok) continue;
        float dx = c[0] - me.x, dy = c[1] - me.y, dz = c[2] - me.z;
        if (dx*dx + dy*dy + dz*dz > 900.0f) continue;         // не наш кадр
        out->x = c[0]; out->y = c[1]; out->z = c[2];
        return true;
    }
    return false;
}

// Позиция камеры из матрицы. У матрицы КАМЕРА->МИР она лежит прямо в
// переносе. А у матрицы ВИДА (мир->камера) в переносе стоит -R*pos, и мой
// прежний тест такие матрицы отбрасывал напрочь - потому и не находилось
// ничего в менеджере, хотя параметры проекции лежат там же рядом.
// Восстанавливаем позицию обратным поворотом: pos = -R^T * t.
static void cam_pos_from_view(const float *m, Vec3 *out) {
    float tx = m[12], ty = m[13], tz = m[14];
    out->x = -(m[0]*tx + m[1]*ty + m[2]*tz);
    out->y = -(m[4]*tx + m[5]*ty + m[6]*tz);
    out->z = -(m[8]*tx + m[9]*ty + m[10]*tz);
}

static bool ortho3(const float *m) {
    float l0 = m[0]*m[0] + m[1]*m[1] + m[2]*m[2];
    float l1 = m[4]*m[4] + m[5]*m[5] + m[6]*m[6];
    float l2 = m[8]*m[8] + m[9]*m[9] + m[10]*m[10];
    if (l0 < 0.98f || l0 > 1.02f) return false;
    if (l1 < 0.98f || l1 > 1.02f) return false;
    if (l2 < 0.98f || l2 > 1.02f) return false;
    float d01 = m[0]*m[4] + m[1]*m[5] + m[2]*m[6];
    float d02 = m[0]*m[8] + m[1]*m[9] + m[2]*m[10];
    return (d01 > -0.03f && d01 < 0.03f) && (d02 > -0.03f && d02 < 0.03f);
}

static void camera_hunt(Vec3 me) {
    g_camHitN = 0;
    MEMORY_BASIC_INFORMATION mbi;
    unsigned char *p = 0;
    unsigned long long swept = 0;
    int regions = 0;
    while (VirtualQuery(p, &mbi, sizeof mbi) == sizeof mbi) {
        unsigned char *base = (unsigned char *)mbi.BaseAddress;
        SIZE_T size = mbi.RegionSize;
        p = base + size;
        if (mbi.State != MEM_COMMIT) continue;
        if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) continue;
        if (!(mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE))) continue;
        if (size > 64u * 1024 * 1024) continue;          // гигантские арены мимо
        regions++; swept += size;
        __try {
            for (SIZE_T off = 0; off + 0x40 <= size && g_camHitN < 64; off += 16) {
                const float *f = (const float *)(base + off);
                // Матрица 4x4: поворот в первых трёх строках, позиция в 12..14
                float dx = f[12] - me.x, dy = f[13] - me.y, dz = f[14] - me.z;
                float d2 = dx*dx + dy*dy + dz*dz;
                if (d2 > 400.0f || d2 < 0.01f) continue;  // не дальше 20 м
                if (!ortho3(f)) continue;
                g_camHits[g_camHitN].addr = base + off;
                g_camHits[g_camHitN].pos.x = f[12];
                g_camHits[g_camHitN].pos.y = f[13];
                g_camHits[g_camHitN].pos.z = f[14];
                g_camHits[g_camHitN].fwd[0] = f[8];   // третья строка - взгляд
                g_camHits[g_camHitN].fwd[1] = f[9];
                g_camHits[g_camHitN].fwd[2] = f[10];
                g_camHitN++;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { continue; }
    }
    L("[камера] обойдено %llu МБ в %d участках, кандидатов %d",
      swept / (1024 * 1024), regions, g_camHitN);
    for (int i = 0; i < g_camHitN && i < 12; i++)
        L("    %p  позиция (%.1f %.1f %.1f), от игрока %.1f м", g_camHits[i].addr,
          g_camHits[i].pos.x, g_camHits[i].pos.y, g_camHits[i].pos.z,
          (double)sqrt((g_camHits[i].pos.x - me.x) * (g_camHits[i].pos.x - me.x) +
                       (g_camHits[i].pos.y - me.y) * (g_camHits[i].pos.y - me.y) +
                       (g_camHits[i].pos.z - me.z) * (g_camHits[i].pos.z - me.z)));
}

// Второе нажатие: настоящая камера сдвинулась вместе с игроком, мусор - нет.
static void camera_verify(Vec3 me) {
    int alive = 0;
    for (int i = 0; i < g_camHitN; i++) {
        if (!readable(g_camHits[i].addr, 0x40)) continue;
        const float *f = (const float *)g_camHits[i].addr;
        if (!ortho3(f)) continue;
        float dx = f[12] - me.x, dy = f[13] - me.y, dz = f[14] - me.z;
        float d = (float)sqrt(dx*dx + dy*dy + dz*dz);
        if (d > 20.0f) continue;
        float moved = (float)sqrt((f[12] - g_camHits[i].pos.x) * (f[12] - g_camHits[i].pos.x) +
                                  (f[13] - g_camHits[i].pos.y) * (f[13] - g_camHits[i].pos.y) +
                                  (f[14] - g_camHits[i].pos.z) * (f[14] - g_camHits[i].pos.z));
        // Решающий признак - поворот. Повернись на месте: у камеры взгляд
        // изменится сильно, а позиция почти нет. У всего прочего наоборот.
        float dot = f[8]*g_camHits[i].fwd[0] + f[9]*g_camHits[i].fwd[1]
                  + f[10]*g_camHits[i].fwd[2];
        if (dot > 1.0f) dot = 1.0f; if (dot < -1.0f) dot = -1.0f;
        float turn = (float)(acos(dot) * 57.2958);      // в градусах
        L("    %p  от игрока %.1f м | сдвиг %.1f м | поворот %.0f град%s",
          g_camHits[i].addr, d, moved, turn,
          (turn > 20.0f) ? "   <- ЭТО КАМЕРА" : (moved > 0.5f ? "   (двигается)" : ""));
        if (turn > 20.0f && g_camN < 16) {
            bool dup = false;
            for (int k2 = 0; k2 < g_camN; k2++) if (g_cam[k2] == g_camHits[i].addr) dup = true;
            if (!dup) { g_cam[g_camN++] = g_camHits[i].addr;
                        L("      запомнил как камеру (всего %d)", g_camN); }
        }
        g_camHits[i].pos.x = f[12]; g_camHits[i].pos.y = f[13]; g_camHits[i].pos.z = f[14];
        g_camHits[i].fwd[0] = f[8]; g_camHits[i].fwd[1] = f[9]; g_camHits[i].fwd[2] = f[10];
        alive++;
    }
    if (!alive) L("[камера] ни один кандидат не пережил проверку");
}

// Список адресов с ортонормированной матрицей рядом с игроком. Назывался
// "камерой" - ошибочно: направление из неё не совпадает с тем, куда смотрит
// игрок. Оставлено как заготовка для дальнейшего разбора, в решениях мода
// не используется.

static bool camera_forward(Vec3 me, Vec3 *pos, Vec3 *fwd) {
    for (int i = 0; i < g_camN; i++) {
        if (!readable(g_cam[i], 0x40)) continue;
        const float *f = (const float *)g_cam[i];
        if (!ortho3(f)) continue;
        float dx = f[12] - me.x, dy = f[13] - me.y, dz = f[14] - me.z;
        if (dx*dx + dy*dy + dz*dz > 100.0f) continue;    // отстала - буфер старый
        pos->x = f[12]; pos->y = f[13]; pos->z = f[14];
        fwd->x = f[8];  fwd->y = f[9];  fwd->z = f[10];
        return true;
    }
    return false;
}

static const char *safe_class(void *p) {
    __try { return rtti_class(p); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

static const char *node_name(void *inter) {
    if (!inter || !readable((unsigned char *)inter + NODE_NAME, 8)) return 0;
    __try {
        char **obj = *(char ***)((unsigned char *)inter + NODE_NAME);
        if (!obj || !readable(obj, 8)) return 0;
        char *sp = *obj;
        if (!sp || !readable(sp, 4)) return 0;
        int n = 0;
        while (n < 63 && readable(sp + n, 1) && sp[n] >= 0x20 && sp[n] < 0x7F) n++;
        if (n < 4 || sp[n] != 0) return 0;
        return sp;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

// Ключ учёта: номер экземпляра, если он есть, иначе eid.
static uint64_t cand_key(uint32_t eid, uint32_t iid) {
    return iid ? (0x100000000ull | iid) : (uint64_t)eid;
}
// Память по МЕСТУ, а не по идентификатору. Игрок сообщил, что одно и то же
// оружие с моба подбирается по нескольку раз и забивает инвентарь копиями.
// Память по eid и номеру экземпляра этого не ловит: если игра пересоздаёт
// выпавший предмет с новым eid, ключ меняется, и мод шлёт событие заново -
// а игра выдаёт ещё одну копию. Точка в мире и номер типа при пересоздании
// остаются теми же, поэтому запоминаем именно их.
struct Spot { float x, y, z; uint16_t tid; DWORD when; };
static Spot g_spot[96];
static int  g_spotN;

static bool spot_recent(const Vec3 &p, uint16_t tid, DWORD now) {
    for (int i = 0; i < g_spotN; i++) {
        if (g_spot[i].tid != tid) continue;
        if (now - g_spot[i].when >= (DWORD)g_cfg.retryAfterMs) continue;
        float dx = g_spot[i].x - p.x, dy = g_spot[i].y - p.y, dz = g_spot[i].z - p.z;
        if (dx*dx + dy*dy + dz*dz < 0.36f) return true;      // в 60 см - тот же предмет
    }
    return false;
}

static void spot_mark(const Vec3 &p, uint16_t tid, DWORD now) {
    int slot = -1;
    for (int i = 0; i < g_spotN; i++)
        if (g_spot[i].tid == tid) {
            float dx = g_spot[i].x - p.x, dy = g_spot[i].y - p.y, dz = g_spot[i].z - p.z;
            if (dx*dx + dy*dy + dz*dz < 0.36f) { slot = i; break; }
        }
    if (slot < 0) {
        if (g_spotN < 96) slot = g_spotN++;
        else {                              // вытесняем самую старую запись
            DWORD oldest = 0; slot = 0;
            for (int i = 0; i < 96; i++)
                if (now - g_spot[i].when >= oldest) { oldest = now - g_spot[i].when; slot = i; }
        }
    }
    g_spot[slot].x = p.x; g_spot[slot].y = p.y; g_spot[slot].z = p.z;
    g_spot[slot].tid = tid; g_spot[slot].when = now;
}

// Память о взятом. Была кольцевым буфером на 128 записей: новая затирала
// старую по кругу. При быстром сборе (PerTick=0, обход каждый кадр) мод берёт
// десятки объектов в секунду, и 128 слотов перезаписывались за секунды -
// запись о предмете исчезала, счётчик попыток сбрасывался, порог "не исчезает,
// больше не трогаю" не достигался никогда, и один и тот же предмет уходил в
// инвентарь снова и снова. В логе такой eid встречался четыре раза за девять
// минут. Чем быстрее работал мод, тем скорее он забывал.
// Теперь память большая, а вытесняется САМАЯ СТАРАЯ запись, а не следующая по
// кругу: свежее сохраняется.
#define DONE_MAX 4096
struct Done { uint64_t key; DWORD when; int tries; };
static Done g_done[DONE_MAX];
static int  g_doneN;
static bool recently_done(uint64_t key, DWORD now) {
    for (int i = 0; i < DONE_MAX; i++)
        if (g_done[i].key == key && (now - g_done[i].when) < (DWORD)g_cfg.retryAfterMs) return true;
    return false;
}
// Чёрный список на сессию. Сюда попадает то, что трогать больше нельзя:
// обысканные туши и всё, что не поддаётся сбору. Последнее важнее, чем
// кажется: настоящее растение после сбора исчезает, а объект, который остаётся
// на месте после трёх попыток, сбором не берётся вовсе. Именно так мод по
// кругу "собирал" вещи, привязанные к самому игроку - они таскались за ним по
// всей карте и каждые шесть секунд получали событие.
// Туши, которые мы уже обыскивали. Список НАВСЕГДА, без срока давности - в
// отличие от g_done, который лишь придерживает объект на несколько секунд.
// Повторный обыск уже пустой туши - главный подозреваемый в том, что игрок
// получил копии собственных вещей: в логе видно, как B01013DE ушёл дважды с
// разницей в шесть секунд, ровно на истечении RetryAfterMs.
// Чёрный список сделан хеш-таблицей. Был линейный перебор: на каждый объект
// в каждом такте пробегались все записи, а их за полчаса игры набирается под
// тысячу. При 192 объектах это 150 тысяч сравнений за такт - и всё только
// ради того, чтобы узнать, что труп уже облутан. Открытая адресация, пустая
// ячейка - ноль.
#define SEARCHED_SLOTS 8192
static uint64_t g_searched[SEARCHED_SLOTS];
static int g_searchedN;
static inline int searched_slot(uint64_t key) {
    uint64_t h = key * 0x9E3779B97F4A7C15ull;
    return (int)((h >> 40) & (SEARCHED_SLOTS - 1));
}
static bool already_searched(uint64_t key) {
    if (!key) return false;
    int i = searched_slot(key);
    for (int n = 0; n < 64; n++) {
        uint64_t v = g_searched[i];
        if (!v) return false;
        if (v == key) return true;
        i = (i + 1) & (SEARCHED_SLOTS - 1);
    }
    return false;
}
static void mark_searched(uint64_t key) {
    if (!key || already_searched(key)) return;
    if (g_searchedN >= SEARCHED_SLOTS / 2) return;      // половина - предел заполнения
    int i = searched_slot(key);
    for (int n = 0; n < 64; n++) {
        if (!g_searched[i]) { g_searched[i] = key; g_searchedN++; return; }
        i = (i + 1) & (SEARCHED_SLOTS - 1);
    }
}

// Зарядка ведёт СВОЙ учёт. Считать её попыткой сбора нельзя: получалось, что
// узел зарядили (попытка раз), собрали (попытка два) - и он тут же попадал в
// чёрный список как "не исчезает после двух попыток". Мод сам запрещал себе
// брать то, что только что включил.
static uint64_t g_armed[256];
static DWORD    g_armedAt[256];
static int      g_armedN;
static bool arm_recent(uint64_t key, DWORD now) {
    for (int i = 0; i < 256; i++)
        if (g_armed[i] == key && (now - g_armedAt[i]) < 10000) return true;
    return false;
}
static void arm_mark(uint64_t key, DWORD now) {
    int k = g_armedN & 255;
    g_armed[k] = key; g_armedAt[k] = now; g_armedN++;
}

static void mark_done(uint64_t key, DWORD now) {
    // Уже пробовали? Считаем попытки. Три подряд по одному и тому же объекту
    // означают, что он от нашего события не исчезает - брать его нечем.
    for (int i = 0; i < DONE_MAX; i++) {
        if (g_done[i].key != key) continue;
        g_done[i].when = now;
        if (++g_done[i].tries >= 2) {
            mark_searched(key);
            L("[отказ] %s=%08X не исчезает после двух попыток - больше не трогаю",
              (key >> 32) ? "экземпляр" : "eid", (uint32_t)key);
        }
        return;
    }
    int k;
    if (g_doneN < DONE_MAX) k = g_doneN++;
    else {                                  // вытесняем самую старую запись
        DWORD oldest = 0; k = 0;
        for (int i = 0; i < DONE_MAX; i++)
            if (now - g_done[i].when >= oldest) { oldest = now - g_done[i].when; k = i; }
    }
    g_done[k].key = key; g_done[k].when = now; g_done[k].tries = 1;
}

struct Cand {
    uint32_t eid, route;
    uint32_t iid;             // номер экземпляра вещи с узла - переживает пересоздание
    uint32_t parent;          // eid родителя из трансформа, 0 - нет
    bool     noIid;           // данные есть, а номера экземпляра нет (FFFFFFFF)
    uint16_t tid;             // номер ТИПА предмета - ключ к имени в таблице
    const char *name;         // имя типа из таблицы игры, 0 - не прочиталось
    const char *nodeName;     // имя самого узла - есть даже когда типа нет
    float d;
    uint8_t cat, dead, type, locked;
    bool ai;                  // есть ClientAiActorComponent - это живность
    bool filled;              // подробности прочитаны (см. fill_details)
    bool inter, item, gather;
    uint16_t src;             // из какого списка контекста взят
    uint32_t catWin[4];       // comp +0x2C8 +0x2CC +0x2D0 +0x2D4 - поиск категории
    uint8_t  cat2;            // comp +0x2D0, рабочая категория на 2658
    bool     banned;          // уже в чёрном списке - подробности не читаем
    uint8_t  gkind;           // данные сбора, байт +5: вид взаимодействия
    Vec3     pos;             // мировая позиция, для отсева двойников
    bool     twin;            // пустой двойник объекта рядом - не трогать
    bool     heap;            // одна из многих вещей в одной точке - ёмкость
    int      mine;            // смещение ссылки на игрока внутри объекта, 0 - нет
    unsigned char *ent;       // для подробного разбора
};

// Подробный разбор одного объекта: куда ведут указатели и что лежит в памяти.
// Нужен, чтобы понять, почему у предметов пусто там, где у растений данные.
// Поиск уехавшего поля категории. Проходит блок comp и показывает все места,
// где лежит значение из известного набора и где это похоже на enum, а не на
// кусок указателя: остальные три байта двойного слова должны быть нулевыми.
// Встать рядом с насекомым или лавкой и нажать F9 - у нужного объекта в списке
// появится смещение со значением 0x09 или 0x0F, которого нет у прочих.
// Снимок одного и того же объекта в разных состояниях наведения.
//
// Первый заход провалился, потому что каждое нажатие снимало РАЗНЫЙ объект:
// ближайший менялся, пока крутишь камеру. Поэтому объект закрепляется первым
// нажатием и дальше снимается он же, сколько бы раз ни нажимали.
//
// Поиск глобала с наведённой целью тоже не задался: у этого exe флаги секций
// перемешаны упаковщиком, .data не помечена записываемой, и обходить по ним
// нечего. Вернёмся к нему, если флага в самом объекте не окажется.
// Где игра держит наведённую цель.
//
// Взято не из головы, а из кода игры. Точка, которую перехватывает AutoLoot
// ради "объекта под прицелом", на 2625 выглядит так:
//     mov rax,[r14+0x68]     <- +0x68 это массив компонент, значит r14 = сущность
//     mov rcx,[rax+0x58]     <- компонента в слоте 0x58
//     mov rax,[rcx+0x90]     <- объект взаимодействия
//     movzx edi,byte[rax+0x66]
// Ту же цепочку можно пройти от игрока безо всякого перехвата. А сама цель в
// таблице AutoLoot читается как EID (u32), а не как указатель - поэтому её и
// надо искать числом.
static void aim_find(unsigned char *me, uint32_t eids[], int n) {
    void *sub = deref(me, ENT_SUBOBJ);
    if (!sub) { L("[цель?] у игрока нет массива компонент"); return; }
    void *c58 = deref(sub, 0x58);
    void *ictx = c58 ? deref(c58, 0x90) : 0;
    L("[цель?] игрок=%p компонента+0x58=%p (%s) объект+0x90=%p",
      me, c58, c58 ? (rtti_short(c58) ? rtti_short(c58) : "?") : "-", ictx);

    struct Area { const char *nm; void *p; int span; } areas[] = {
        { "игрок",        me,   0x200 },
        { "комп+0x58",    c58,  0x600 },
        { "объект+0x90",  ictx, 0x400 },
    };
    int hits = 0;
    for (int a = 0; a < 3; a++) {
        if (!areas[a].p) continue;
        for (int off = 0; off + 4 <= areas[a].span; off += 4) {
            uint32_t v;
            if (!read_u32((unsigned char *)areas[a].p + off, &v)) continue;
            for (int k = 0; k < n; k++)
                if (v == eids[k]) {
                    L("[цель?] %s +0x%03X = %08X (%d-й по близости)",
                      areas[a].nm, off, v, k);
                    hits++;
                }
        }
    }
    // Заодно любые значения, похожие на eid мира, - вдруг цель не из списка.
    if (ictx) {
        for (int off = 0; off + 4 <= 0x400; off += 4) {
            uint32_t v;
            if (!read_u32((unsigned char *)ictx + off, &v)) continue;
            if ((v >> 24) == EID_WORLD_TAG)
                L("[цель?] объект+0x90 +0x%03X = %08X (похоже на eid мира)", off, v);
        }
    }
    L("[цель?] --- совпадений: %d ---", hits);
}

static void cat_hunt(unsigned char *e) {
    void *sub  = deref(e, ENT_SUBOBJ);
    void *comp = sub ? deref(sub, SUB_COMP) : 0;
    if (!comp || !readable(comp, 0x400)) return;
    unsigned char *c = (unsigned char *)comp;
    char line[400]; int n = 0;
    for (int off = 0x100; off + 4 <= 0x400 && n < 360; off += 4) {
        uint32_t v = *(uint32_t *)(c + off);
        if (v != CAT_QUEST && v != CAT_INSECT && v != CAT_SHOP &&
            v != CAT_DECOR && v != CAT_PLAYERDROP) continue;
        n += sprintf(line + n, "%03X=%02X ", off, (unsigned)v);
    }
    line[n] = 0;
    L("[кат?] eid=%08X тип=%02X: %s", *(uint32_t *)(e + ENT_EID), ent_type(e),
      n ? line : "ни одного подходящего значения");
}

static void deep_dump(const char *tag, unsigned char *e) {
    if (!e || !readable(e, 0x100)) return;
    char h[400];
    void *sub = deref(e, ENT_SUBOBJ);
    L("[разбор] %s eid=%08X сущность=%p sub=%p", tag, *(uint32_t *)(e + ENT_EID), e, sub);
    hexs(e + 0x40, 0x60, h, sizeof h);
    L("         сущность +0x40: %s", h);
    if (sub && readable(sub, 0x60)) {
        hexs(sub, 0x60, h, sizeof h);
        L("         sub      +0x00: %s", h);
        // Состав компонент - самое полезное, что можно сказать об акторе.
        for (int off = 0; off < SUB_SLOTS_END; off += 8) {
            void *cc = deref(sub, off);
            const char *n = cc ? rtti_short(cc) : 0;
            if (n) L("         компонента +0x%02X: %s", off, n);
        }
        void *comp = comp_by_class(sub, CLS_STATUS);
        L("         sub: comp(+0x20)=%p inter(+0x30)=%p transform(+0x1A0)=%p",
          comp, deref(sub, SUB_INTER), deref(sub, SUB_TRANSFORM));
        // Окно вокруг категории (COMP_CAT). На 2625 в +0x2C8 приходят значения
        // 0xAF/0x00, которых нет в известном наборе - значит поле переехало.
        // Дамп нужен, чтобы найти его на живых объектах разного вида.
        if (comp && readable(comp, 0x340)) {
            for (int off = 0x280; off < 0x340; off += 0x40) {
                hexs((unsigned char *)comp + off, 0x40, h, sizeof h);
                L("         comp   +0x%03X: %s", off, h);
            }
        }
    }
}

static volatile LONG g_ownLeft;
// Постоянные аргументы оракула, подобранные из настоящего вызова игры: a1 -
// контекст (*(что-то+0x68)+0x120), a4 - статический указатель из lea r9. Их
// не вычисляем и не зашиваем, а берём с натуры - переживает любой патч.
static void *g_ownCtx;
static void *g_ownTag;
static unsigned char *g_meEnt;      // игрок текущего такта, для запроса
static uint32_t       g_meEid;      // его eid - по нему узнаём свои вещи
typedef bool (*FnOwnCheck)(void *, void *, void *, void *, long long);

// Вернула не ноль - взять это значит украсть. Пока оракул не подслушан,
// отвечаем "не кража": лучше собрать лишнее, чем молча ничего не собирать.
static volatile LONG g_stealLog;     // сколько ответов оракула ещё записать

// Готов ли оракул. Пока игра ни разу не позвала свою проверку владельца,
// мы не знаем о собственности ничего.
static bool oracle_ready(void) {
    return g_fn.ownCheck && g_ownCtx && g_ownTag && g_meEnt;
}

static bool would_steal(void *target) {
    // Оракул не готов - считаем ВОРОВСТВОМ и не берём. Раньше здесь стояло
    // "не кража", и любая поломка означала, что мод спокойно обчищает чужое:
    // за сессию не сработало ни одного отказа, а розыск игрок словил.
    // Пропустить чужое хуже, чем не подобрать своё.
    if (!g_fn.ownCheck || !target) return false;
    if (!g_ownCtx || !g_ownTag || !g_meEnt) {
        static LONG told = 0;
        if (InterlockedExchange(&told, 1) == 0)
            L("[владелец] оракул ещё не подслушан - чужое пока не отличаю, беру только своё");
        return true;
    }
    bool r = false, boom = false;
    __try {
        r = ((FnOwnCheck)g_fn.ownCheck)(g_ownCtx, g_meEnt, target, g_ownTag, 7);
    } __except (EXCEPTION_EXECUTE_HANDLER) { boom = true; }
    if (boom) {
        static LONG told2 = 0;
        if (InterlockedExchange(&told2, 1) == 0)
            L("[владелец] вызов оракула падает - считаю всё чужим, чтобы не воровать");
        return true;
    }
    if (InterlockedCompareExchange(&g_stealLog, 0, 0) > 0) {
        InterlockedDecrement(&g_stealLog);
        L("[владелец] цель=%p -> %s", target, r ? "КРАЖА" : "можно");
    }
    return r;
}


// Ярлык для лога. Считаем по рабочей категории 2658 (comp+0x2D0) и по тому,
// что висит на узле - ровно так же, как решает skip_reason. Иначе в логе
// лежащая вещь называлась «растением» и путала разбор.
static const char *cat_name(uint8_t cat2, uint8_t dead, uint8_t type,
                            bool item, bool gather) {
    switch (cat2) {
        case CAT2_SMALL: return "мелкая живность";
        case CAT2_FISH:  return "рыба";
        case CAT2_BEAST: return dead == 1 ? "туша зверя" : "зверь";
        default: break;
    }
    if (dead == 1)  return "труп (дроп падает отдельно)";
    if (gather)     return "растение/руда";
    if (item)       return "вещь на земле";
    if (type == TYPE_PLANT) return "жила или куст";
    return "предмет";
}

// Возвращает причину пропуска или 0, если объект можно брать.
// Дочитывает то, за чем надо лезть в компоненты. Зовётся только для тех
// объектов, о которых мод собирается что-то сказать или сделать.
static void fill_details_inner(Cand &k);
static void mark_container(uint32_t eid);   // объявление: зовётся раньше тела

static void fill_details(Cand &k) {
    if (k.filled) return;
    k.filled = true;
    __try { fill_details_inner(k); }
    __except (EXCEPTION_EXECUTE_HANDLER) { InterlockedIncrement(&g_faults); }
}

// Перечитать заново, забыв про кэш. Нужно ровно в одном месте - сразу после
// того, как мы САМИ зарядили узел: до зарядки объект уже был разобран с
// пустым узлом, и обычный fill_details молча выходил по флагу filled. В
// записи оставались сбор=0 и тип=0, а с типом 0 не совпадает ни один запрет
// по номеру - ведро проезжало мимо защиты именно так.
static void fill_details_again(Cand &k) {
    k.filled = false;
    fill_details(k);
}

static void fill_details_inner(Cand &k) {
    void *sub  = deref(k.ent, ENT_SUBOBJ);

    // Принадлежит ли вещь игроку. Оракул воровства тут бесполезен: на своё он
    // отвечает "не кража", ведь у себя не крадут. route тоже не годится - у
    // дикой травы стоит тот же 0x90100000, это идентификатор сессии.
    // Зато вещь, привязанная к игроку, обычно на него и ссылается: ищем в её
    // первых 0x200 байтах указатель на нашу сущность или на её массив
    // компонент. Смещение найденной ссылки запоминаем и пишем в лог - по нему
    // потом станет видно, одно это поле или разные.
    k.mine = 0;
    if (g_meEnt && readable(k.ent, 0x200)) {
        void *meSub = deref(g_meEnt, ENT_SUBOBJ);
        for (int off = 0; off < 0x200; off += 8) {
            void *v = *(void **)(k.ent + off);
            if (v == (void *)g_meEnt || (meSub && v == meSub)) { k.mine = off; break; }
        }
    }
    void *comp = comp_by_class(sub, CLS_STATUS);
    if (comp && readable(comp, COMP_CAT + 1)) {
        k.cat  = *((unsigned char *)comp + COMP_CAT);
        k.dead = *((unsigned char *)comp + COMP_DEAD);
    }
    // Окно вокруг категории. Разбор пойманного жука дал в comp+0x2D0 ровно
    // 0x09 - номер насекомых из старой таблицы категорий, тогда как в 0x2C8
    // (где поле стояло на 1573) лежит мусор. Пока это одна точка, поэтому
    // поле не переносим, а показываем соседей в обзоре: один F9 в смешанном
    // месте даст сразу десятки образцов с уже известными видами.
    if (comp && readable((unsigned char *)comp + 0x2C8, 16)) {
        for (int i = 0; i < 4; i++)
            k.catWin[i] = *(uint32_t *)((unsigned char *)comp + 0x2C8 + i * 4);
        k.cat2 = *((unsigned char *)comp + COMP_CAT2);
    }
    k.ai = comp_by_class(sub, CLS_AI) != 0;
    // Каждое поле со своей проверкой границ: раньше все три читались под
    // одной, и при коротком узле всё молча превращалось в нули.
    void *inter = comp_by_class(sub, CLS_GIMMICK);
    k.inter  = inter != 0;
    k.item   = deref(inter, INTER_ITEMDATA) != 0;
    k.gather = deref(inter, INTER_GATHER) != 0;
    void *gdata = deref(inter, INTER_GATHER);
    if (gdata && readable(gdata, 4) && !k.tid) k.tid = *(uint16_t *)gdata;
    // Байт +5 данных сбора - вид взаимодействия. В отличие от номера типа он
    // стоит в узле СРАЗУ, ещё в заготовке, поэтому годится для решения в тот
    // же такт. Замеры: 0x03 у растений, руды и сундуков; 0x04 у ведра с
    // водой - и у того, что у механизма, и у колодезного.
    if (gdata && readable((unsigned char *)gdata + 5, 1)) {
        k.gkind = *((unsigned char *)gdata + 5);
        // Ёмкостью считаем только ПОДТВЕРЖДЁННУЮ: вид 0x04 плюс номер типа
        // ведра. Одного вида мало - на живой проверке под него попали рудные
        // жилы, и сбор руды встал целиком. Номер 52920 проверен на двух
        // разных вёдрах, у механизма и у колодца.
        if (k.gkind == 0x04 && k.tid == 52920) mark_container(k.eid);
    }
    // Номер экземпляра. Первое поле данных предмета, и оно НЕ МЕНЯЕТСЯ, когда
    // игра выбрасывает объект и спавнит его заново: у фонаря игрока номер
    // 0x000F5B2A оставался тем же под тремя разными eid. Поэтому весь учёт -
    // и повторы, и чёрный список - ведём по нему, а не по eid, иначе каждое
    // пересоздание считается новой вещью и мод ходит по кругу.
    void *idata = deref(inter, INTER_ITEMDATA);
    if (idata && readable(idata, 4)) {
        uint32_t v = *(uint32_t *)idata;
        // 0xFFFFFFFF означает "номера нет". Брать его за ключ нельзя: одна
        // запись в чёрном списке заглушила бы все безномерные вещи. Зато сам
        // факт отсутствия номера - важный признак, см. skip_reason.
        if (v != 0xFFFFFFFF) k.iid = v; else k.noIid = true;
        // Номер типа: у вещи он во втором слове данных, у сбора - в первом.
        if (readable(idata, 12)) k.tid = *(uint16_t *)((unsigned char *)idata + 8);
    }
    // Имя - в самом конце: номер типа приходит из двух мест, и у вещи он
    // читается ПОЗЖЕ, чем у сбора. Раньше имя бралось до этого, фильтр всегда
    // получал тип 0, а по нулевому индексу таблица отдаёт первую запись -
    // отсюда "Pyeonjeon_Arrow" у всего подряд и пропущенный артефакт.
    // Тип 0 считаем неизвестным: настоящих предметов с таким номером нет.
    if (k.tid) k.name = item_type_name(k.tid);
    // Имени в iteminfo нет - значит номер не из неё. У механизмов своя
    // таблица, и ведро с водой находится именно там.
    if (!k.name && k.tid) k.name = gimmick_type_name(k.tid);
    k.nodeName = node_name(inter);
    {
    }
    if (inter && readable((unsigned char *)inter + INTER_LOCKED, 1))
        k.locked = *((unsigned char *)inter + INTER_LOCKED);
}

// Опознанные ёмкости механизмов. До зарядки узел пуст, и ведро от куста не
// отличить - вид взаимодействия появляется только вместе с данными. Поэтому
// один раз опознав, помним до конца сессии: дальше решение принимается уже
// по памяти, ещё до того, как мы к узлу прикоснёмся.
static uint32_t g_contEid[256];
static int      g_contN;

// Когда объект впервые попал в список. Нужно, чтобы не стрелять по узлу,
// данные которого игра ещё не дописала: сразу после появления там пустая
// заготовка с типом 0, и ни один фильтр по типу сработать не может. Ведро с
// водой уезжало ровно в это окно - мод успевал собрать его за миллисекунды
// до того, как узел сообщал, что он такое.
static struct { uint32_t eid; DWORD t; } g_first[512];
static int g_firstN;

static DWORD age_ms(uint32_t eid, DWORD now) {
    for (int i = 0; i < g_firstN; i++)
        if (g_first[i].eid == eid) return now - g_first[i].t;
    int slot = g_firstN < 512 ? g_firstN++ : -1;
    if (slot < 0) {                       // таблица полна - вытесняем старейшего
        DWORD oldest = 0; slot = 0;
        for (int i = 0; i < 512; i++)
            if (now - g_first[i].t >= oldest) { oldest = now - g_first[i].t; slot = i; }
    }
    g_first[slot].eid = eid; g_first[slot].t = now;
    return 0;
}

static bool is_container(uint32_t eid) {
    for (int i = 0; i < g_contN; i++) if (g_contEid[i] == eid) return true;
    return false;
}

static void mark_container(uint32_t eid) {
    if (is_container(eid) || g_contN >= 256) return;
    g_contEid[g_contN++] = eid;
    L("[ёмкость] eid=%08X опознан как ёмкость механизма - лутать не буду", eid);
}

// ---------------------------------------------------------- сторож дюпа ----
// Ошибка плавающая: за всю разработку вылезала пару раз, вызвать насильно не
// получается. Значит ловить надо не воспроизведением, а наблюдением: мод сам
// замечает, что одного и того же типа предмета ушло подозрительно много за
// короткое время, и выкладывает в лог всё, что знал о каждой отправке.
// Порог намеренно низкий: обычный сбор редко даёт больше восьми одинаковых
// предметов за десять секунд, а при дюпе их сотни.
struct SendRec { DWORD when; uint32_t eid, parent; uint16_t tid; uint8_t cat2, act;
                 float d; const char *name; };
static SendRec       g_sendLog[64];
static int           g_sendLogN;
static DWORD         g_dupToldAt;

static void dup_watch(DWORD now, uint32_t eid, uint32_t parent, uint16_t tid,
                      uint8_t cat2, uint8_t act, float d, const char *name) {
    g_sendLog[g_sendLogN & 63].when = now;
    g_sendLog[g_sendLogN & 63].eid = eid;
    g_sendLog[g_sendLogN & 63].parent = parent;
    g_sendLog[g_sendLogN & 63].tid = tid;
    g_sendLog[g_sendLogN & 63].cat2 = cat2;
    g_sendLog[g_sendLogN & 63].act = act;
    g_sendLog[g_sendLogN & 63].d = d;
    g_sendLog[g_sendLogN & 63].name = name;
    g_sendLogN++;
    if (!tid) return;                       // без типа считать нечего
    int same = 0;
    for (int i = 0; i < 64; i++)
        if (g_sendLog[i].tid == tid && g_sendLog[i].when &&
            now - g_sendLog[i].when < 10000) same++;
    if (same < 8) return;
    if (g_dupToldAt && now - g_dupToldAt < 30000) return;   // не чаще раза в 30 с
    g_dupToldAt = now;
    L("[ДЮП?] тип %u (%s) ушёл %d раз за 10 секунд - вот все отправки:",
      tid, name ? name : "без имени", same);
    for (int i = 0; i < 64; i++) {
        int k = (g_sendLogN + i) & 63;
        if (!g_sendLog[k].when || now - g_sendLog[k].when >= 10000) continue;
        L("   +%4lu мс  eid=%08X род=%08X тип=%u к2=0x%02X %.1f м  %s",
          (unsigned long)(now - g_sendLog[k].when), g_sendLog[k].eid,
          g_sendLog[k].parent, g_sendLog[k].tid, g_sendLog[k].cat2,
          g_sendLog[k].d, g_sendLog[k].name ? g_sendLog[k].name : "без имени");
    }
    L("[ДЮП?] --- конец списка. Одинаковые eid = повтор по объекту;"
      " разные eid в одной точке = пересоздание; род != 0 = снаряжение носителя.");
}

static const char *skip_reason(const Cand &c, Action *out) {
    if (c.banned) return "уже брали - в чёрном списке";
    if (c.cat == CAT_QUEST) return "квестовое";
    if (c.cat == CAT_SHOP)  return "товар лавки";
    if (c.cat == CAT_DECOR) return "декорация";
    if (c.locked == 1)      return "заперто";
    if (c.twin)             return "пустой двойник";
    // Вещь в нуле от игрока - это сам игрок. Снаряжение и содержимое сумки
    // игра держит в сцене обычными объектами с честными данными предмета на
    // узле, и отличить их от лежащего на земле лута можно только по тому, что
    // они находятся ровно в нашей точке. Именно так мод раз за разом
    // "подбирал" собственный лук игрока: d=0.0 м, сплошной блок eid.
    if (c.d < g_cfg.minRange) return "на самом игроке - наше";
    // Прямой признак: вещь висит на игроке как на родителе.
    if (c.parent && c.parent == g_meEid) return "надето или в сумке - наше";
    // ЧУЖОЕ СНАРЯЖЕНИЕ. Игрок сообщил, что во время боя подобралось 24 меча
    // одного типа, а после стычки со стражей - полсотни топоров: "оружия
    // больше, чем врагов". Разгадка в том, что оружие живого врага - такая же
    // сущность сцены, как наше собственное, и отличается только родителем.
    // Своё мы отсекали проверкой выше, а чужое уходило в подбор, причём враг
    // оружие сохранял - источник бесконечный.
    //   к2 = 0x11 стоит РОВНО на надетом: во всех дампах его несли лук, меч,
    //   шлем, щит, фонарь и кольцо игрока, и ни разу - лут на земле.
    //   родитель у надетого - сам носитель; у лежащей на земле вещи он нулевой.
    // Отсюда же прежняя загадка опыта SkipCat11: 13 "лишних" объектов на голой
    // скале были снаряжением зверей, а не утварью.
    if (c.item && c.parent && c.cat2 == 0x11)
        return "надето на ком-то - не наше дело";
    if (inventory_has(c.iid)) return "лежит в инвентаре - наше";
    // Отсев по ИМЕНИ типа. Признак "нет номера экземпляра" оказался неверным:
    // такой же FFFFFFFF стоит у обычной бутылки Collection_Prop_Bottle_0006.
    // Зато имя типа читается из таблицы игры и говорит прямо, что это такое.
    // Имя узла есть даже у незаряженных - по нему и отделяем то, что трогать
    // нельзя: мебель с выпадающим набором, сундуки, ящики и visione-триггеры
    // (места, где игра проигрывает особую анимацию памяти).
    if (c.nodeName) {
        static const char *nodeBan[] = { "visione", "quest", "artifact" };
        for (int i = 0; i < 3; i++)
            if (stristr_ru(c.nodeName, nodeBan[i]))
                return "триггер или квестовое - не трогаем";
        if (!g_cfg.lootFurnitureName) {
            static const char *nodeFurn[] = { "furniture", "_chest", "_box", "dropset" };
            for (int i = 0; i < 4; i++)
                if (stristr_ru(c.nodeName, nodeFurn[i])) return "мебель или ящик, выключено";
        }
    }
    if (c.name) {
        // Рецепты сюда же: книга рецептов - разовая вещь, и подобрать её
        // случайно хуже, чем не подобрать. Имя вида Recipe_Book_...
        // Детали механизмов и головоломок. Обход 01.09.2026 дал их семь штук:
        // attach_abyss_core, abysscore, nature_rock_dissolve_stone_core,
        // puzzle_antumbra_tokamak_parts... Часть мод уже собирался брать
        // (сбор=1), а от кражи их спасал только фильтр владельца - то есть с
        // LootOwned=1 или прицельной клавишей головоломка уехала бы в сумку.
        //
        // Сначала запрет стоял на весь префикс "item_gimmick" - и оказался
        // слишком жадным: под него попала обычная керамика
        // Item_gimmick_collection_prop_Ceramic_0021, ровно такая же, как
        // Collection_Prop_Ceramic_0032, которую брать можно. Признак не в
        // префиксе, а в "puzzle" и "core" - именно они отличают деталь
        // механизма от утвари.
        // "visione" - воспоминания, те самые, что подсвечиваются фонарём.
        // Запрет по имени УЗЛА на них стоял давно, а по имени ПРЕДМЕТА не
        // было, и они спокойно уезжали в сумку: у игрока нашёлся
        // Visione_Chip_DemenissFuture. При подборе герой ещё и надевает шлем
        // воспоминаний - об этом писал lsimo на Nexus.
        static const char *forbidden[] = { "quest", "artifact", "sealed_", "_seal",
                                           "recipe", "puzzle", "_core", "abysscore",
                                           "visione",
                                           // Модули прогрессии Бездны. В
                                           // английской локализации семейство
                                           // зовётся Axiom - LuxDragon сообщил,
                                           // что мод утащил Axiom Bracelet, и
                                           // человек откатил сохранение.
                                           // Внутренние имена: Item_Stat_
                                           // AbyssGear_*, Item_Skill_AbyssGear_*.
                                           "abyssgear" };
        for (int i = 0; i < (int)(sizeof forbidden / sizeof forbidden[0]); i++)
            if (stristr_ru(c.name, forbidden[i]))
                return "механизм, квест, артефакт или рецепт - только руками";
        // Инструменты - лопата, кирка, мотыга, топор дровосека, рабочий молот.
        // В игре у них общий префикс Equip_, а оружие названо иначе
        // (Crudell_OneHandAxe, Rusty_Deadwid_TwoHandAxe), так что префикс их
        // не задевает. Отдельный ключ просили дважды: в комментариях и здесь.
        if (!g_cfg.lootTools) {
            static const char *tools[] = { "equip_", "crafttool", "_shovel",
                                           "_pickaxe", "_hoe", "_sickle" };
            for (int i = 0; i < (int)(sizeof tools / sizeof tools[0]); i++)
                if (stristr_ru(c.name, tools[i])) return "инструмент, выключено";
        }
        if (!g_cfg.lootFurnitureName) {
            // Керамика - утварь, а не лут: под тумблер мебели по просьбе.
            // Ловит обе формы имени, Collection_Prop_Ceramic_0032 и
            // Item_gimmick_collection_prop_Ceramic_0021.
            // Утварь и декор игра держит обычными предметами, а не мебелью:
            // лампа, свеча, тарелка, миска, чашка, картина, горшок. Игроки
            // жалуются, что в жилых местах этого больше, чем настоящей
            // добычи, - поэтому вся семья уезжает под LootFurniture.
            // Бутылки и инструменты сюда НЕ включены намеренно: их продают,
            // и для них просили отдельные ключи.
            static const char *furn[] = { "_house_", "furniture", "_deco", "_prop_bed",
                                          "_ceramic", "collection_lamp", "_candle",
                                          "_picture", "_plate", "_bowl", "_cup",
                                          "_flowerpot",
                                          // Обстановка сцены: столы, стойки,
                                          // ломаемые деревяшки. Игрок утащил
                                          // стол торговца - Item_Background_
                                          // Breakable_Wood_10; на Nexus про то
                                          // же писал Pyrion: столы исчезают, а
                                          // товар с них висит в воздухе.
                                          "item_background",
                                          // Светильники: факел взят живьём,
                                          // Collection_Lamp_* уже выше.
                                          "torch", "_lamp" };
            for (int i = 0; i < (int)(sizeof furn / sizeof furn[0]); i++)
                if (stristr_ru(c.name, furn[i])) return "мебель, выключено";
        }
    }
    // ЖЁСТКИЙ запрет по номеру типа. Не настройка: эти объекты - части
    // механизмов, и подъём любого из них ломает то, ради чего они стоят.
    // Имени у них нет вовсе - номер лежит вне всех 46 таблиц игры, поэтому
    // фильтры по именам их не ловят, и опознать можно только по числу.
    //     52920 - ведро с водой. Проверено на двух разных: у механизма и у
    //             колодца, номер один и тот же. Автосбор уносил ведро
    //             целиком, хотя исчезать должна была только вода.
    static const uint16_t banned[] = { 52920 };
    if (c.tid)
        for (int i = 0; i < (int)(sizeof banned / sizeof banned[0]); i++)
            if (c.tid == banned[i]) return "часть механизма - не трогаем";
    // Номер типа появляется в узле не сразу: пока игра не дописала данные,
    // там ноль, и запрет выше совпасть не может - ведро уезжало именно в это
    // окно. Вид взаимодействия стоит в узле сразу, поэтому решаем по нему.
    // Запрет только по памяти об опознанных: широкое правило "любой вид 0x04"
    // выбрасывало руду. Цена сужения честная: ведро, которое мод ни разу не
    // видел с заполненным узлом, он может забрать один раз - до того, как
    // опознает. Терять руду ради этого нельзя.
    if (is_container(c.eid)) return "ёмкость механизма - берите руками";
    if (c.heap)             return "куча в одной точке - ёмкость";
    if (c.mine)             return "ссылается на игрока - наше";
    // Опыт: отсев по категории 0x11 задуман как LootFurniture родного мода,
    // но на живой проверке выбросил 13 объектов на голой скале. Пока не
    // разобрано, что это за метка, фильтр выключен по умолчанию.
    if (g_cfg.skipCat11 && c.cat2 == CAT2_CLUTTER) return "категория 0x11, отсев включён";
    // Узел взаимодействия обязателен для ПОДБОРА. Обработчик подбора берёт
    // [[сущность+0x68]+0x30] и сразу читает [rcx+8], не проверяя на ноль:
    // два дампа, тип 03 и тип 06, упали по одному и тому же адресу
    // +0x25E5F52 с чтением по 0x8. Объект без узла этим событием не берётся
    // в принципе - это не «не сработает», а вылет игры.
    // Живность гиммиком не бывает: у неё Ai/CharacterControl и никогда нет
    // ClientGimmickActorComponent. Раньше такие объекты числились предметами
    // с типом 0x06, и мод пытался их ПОДОБРАТЬ. Ловятся они событием
    // PushCharacterToInventory, но отправку живности пока не включаем.
    //
    // Про труп см. запрет ниже: обыск отключён, дублировал вещи игрока.
    // Мелкая живность снаружи неотличима от козы: тот же тип 06, тот же список
    // сцены, нет узла, есть Ai. Различает их байт comp+0x2D0 - см. COMP_CAT2.
    // Ловля идёт событием 0x0800 и узла не требует, как и обыск трупа.
    // Рыба ловится тем же событием: игра на наших глазах отправила
    // `00 08 FF <eid> 03` по рыбе с категорией 0x05, буфер до байта такой же,
    // как у жука. Зверь (0x0C) в этот список НЕ входит.
    bool catchable = (c.cat2 == CAT2_SMALL || c.cat2 == CAT2_FISH)
               && c.type == TYPE_ITEM && !c.inter;
    if (!catchable && c.dead != 1 && !c.inter && c.ai) return "живность, не лут";
    if (!catchable && c.dead != 1 && !c.inter) return "нет узла - вылет";
    // С мёртвыми в этой игре два РАЗНЫХ дела, и путать их нельзя.
    //
    // Зверь: с туши берётся шкура, и это РАБОТАЕТ через 0x07E8 - проверено в
    // игре, шкура упала в инвентарь. Но ровно один раз на тушу: в логе видно,
    // как мод обыскал B01013DE дважды с разницей в шесть секунд (истёк
    // RetryAfterMs), и примерно тогда же у игрока появились КОПИИ его
    // собственных вещей. Похоже, на уже пустой цели игра отдаёт содержимое
    // своего посмертного мешка - имя класса события как раз про мешок,
    // TrocTrProcessLootingDeadDropOnceTimer. Поэтому обысканные туши помнятся
    // навсегда, без срока давности, и второй раз не трогаются.
    //
    // Человек: обыскивать нечего, с убитого падает обычный дроп со статусом
    // «взять», и его забирает ветка предмета ниже. Слать туда 0x07E8 нельзя.
    bool beastCorpse = c.dead == 1 && (c.cat2 == CAT2_BEAST || c.ai);
    if (c.dead == 1 && !beastCorpse) return "труп: дроп падает отдельно, обыска нет";
    if (already_searched(cand_key(c.eid, c.iid))) {
        if (beastCorpse) return "туша уже обыскана";
        // Клеймо "не поддаётся" ставит зарядка, когда узел на её вызов не
        // ответил. Оно вечное - иначе мод по кругу ковырял бы каждый сундук.
        // Но узел мог молчать просто потому, что игра до него ещё не дошла:
        // Collection_Prop_FlowerPot_0007 в 1.1 м стоял с ПОЛНЫМ узлом
        // (предмет=1) и всё равно получал отказ - вещь была потеряна на всю
        // сессию из-за одного неудачного подхода.
        // Поэтому клеймо действует, только пока узел ПУСТ. Появились данные -
        // объект живой, и запрет с него снимается. Сундукам это ничего не
        // даёт: у них узел пуст и остаётся пустым.
        if (!c.item && !c.gather) return "не поддаётся, отключено";
    }

    // Что делать - решает УЗЕЛ, а не грубый тип сущности. Тип 0x07 носят и
    // куст, и лежащая на земле вещь; отличает их содержимое узла:
    //   inter+0xE0 (сбор)   - это растение или руда, способ 0x05;
    //   inter+0xC0 (предмет)- это вещь, обычный подбор, способ 0x00.
    // Раньше обе ветки шли по типу, и вещам уходил растительный буфер.
    if (beastCorpse)               *out = ACT_SEARCH;
    else if (catchable)               *out = ACT_CATCH;
    else if (c.gather)             *out = ACT_GATHER;
    else if (c.item)               *out = ACT_TAKE;
    // Запасных веток по типу сущности больше нет. Раньше объект типа 0x07 с
    // пустым узлом всё равно уходил в сбор "на всякий случай" - и через эту
    // щель мод каждые шесть секунд дёргал B0101B66: узел есть, данных нет, а
    // сама вещь таскалась за игроком по всей карте в полутора метрах. Пустой
    // узел - это не лут, чем бы объект ни был.
    // Узел есть, а данных на нём нет. Это не поломка и не декорация: игра
    // наполняет данные сбора и предмета ТОЛЬКО когда считает взаимодействие
    // доступным. Рудная жила в 1.9 м даёт "сбор=0", та же жила с того же
    // расстояния, но когда игрок стоит на ней - "сбор=1". Радиусом это не
    // лечится: брать буквально нечего, пока игра не разрешит.
    else                           return "игра ещё не даёт взять";

    // Вид разрешён к сбору? Ключи те же, что в родном AutoLoot.ini.
    switch (*out) {
        case ACT_SEARCH: if (!g_cfg.lootCorpses)  return "трупы выключены";   break;
        case ACT_CATCH:  if (!g_cfg.catchInsects) return "ловля выключена";   break;
        case ACT_GATHER: if (!g_cfg.gatherPlants) return "сбор выключен";     break;
        default:         if (!g_cfg.pickUpItems)  return "подбор выключен";   break;
    }

    // Свой радиус на каждый вид: до травы тянуться можно дальше, чем до жука,
    // а жука с пяти метров хватать глупо. Ноль - без ограничения, как в родном
    // моде.
    float lim = g_cfg.range[*out];
    if (lim > 0.0f && c.d > lim) return "далеко";

    // Владелец - последним: вызов в игру дороже всех проверок выше, и звать
    // его есть смысл только для того, что мы иначе взяли бы. Оракул тот же,
    // которым игра решает, показать «Взять» или «Украсть».
    // Оракул наполняется только тогда, когда игра сама зовёт свою проверку
    // владельца - а делает она это, решая, показать "Взять" или "Украсть".
    // В чистом поле у рудной жилы такого повода нет, и мод оставался слепым:
    // всё подряд считалось чужим, и руда не собиралась вовсе. Именно на это
    // жаловались "стою прямо на жиле, не подбирает в 95% случаев".
    //
    // Пока оракул молчит, отказываем только ПОДБОРУ вещей - воровать можно
    // именно их, и именно они лежат в чужих домах. Сбор растений и руды
    // пропускаем: дикая жила и куст никому не принадлежат, а осторожность,
    // которая выключает половину мода, - плохая осторожность.
    if (!g_cfg.lootOwned) {
        if (!oracle_ready()) {
            if (*out != ACT_GATHER) return "владелец неизвестен, вещи не беру";
        } else if (would_steal(c.ent)) {
            return "чужое, будет розыск";
        }
    }
    return 0;
}

static void area_body(void *ctx, void *a2, void *a3, void *a4) {
    (void)a2; (void)a3; (void)a4;
    self_test();          // один раз, на игровом потоке

    // Обход сцены зовётся каждый кадр. По клавише работаем сразу и подробно,
    // в автосборе - по темпу из ini и молча, иначе лог утонет.
    LONG scanReq = InterlockedExchange(&g_scanRequested, 0);
    bool verbose = scanReq != 0;
    DWORD now = GetTickCount();
    static DWORD lastAuto = 0;
    bool autoTick = false;
    // Темп 0 = без ограничения: обход идёт каждый раз, когда игра зовёт нас,
    // то есть каждый кадр. Самый быстрый и самый дорогой режим.
    DWORD step = g_cfg.maxLootsPerSec ? (DWORD)(1000 / g_cfg.maxLootsPerSec) : 0;
    if (InterlockedCompareExchange(&g_autoOn, 0, 0) && (now - lastAuto) >= step) {
        autoTick = true; lastAuto = now;
    }
    // Плашка живёт своим тактом: она нужна и когда автосбор выключен, но
    // обход сцены зовётся каждый кадр, поэтому не чаще пяти раз в секунду.
    static DWORD lastHud = 0;
    bool hudTick = InterlockedCompareExchange(&g_hudOn, 0, 0) && (now - lastHud) >= 200;
    if (hudTick) lastHud = now;
    LONG dumpEid = InterlockedCompareExchange(&g_dumpEid, 0, 0);
    if (!scanReq && !autoTick && !hudTick && !dumpEid) return;
    TickTimer tt(verbose ? "обзор F9" : (autoTick ? "автосбор" : "плашка"));
    LARGE_INTEGER ph0, ph1, ph2, ph3;
    QueryPerformanceCounter(&ph0);
    if (!ctx || !readable(ctx, 0x200)) { if (verbose) L("[scan] контекст нечитаем"); return; }
    unsigned char *c = (unsigned char *)ctx;

    unsigned char *me = 0;
    uint32_t playerEid = 0, playerRoute = 0;
    int players = 0;
    // Мест было 64, и набивались они В ПОРЯДКЕ ОБХОДА СЦЕНЫ, а не по близости:
    // как только обзор подняли с 25 до 40 м, список стал забиваться дальним
    // хламом, а ближнее в него не попадало вовсе. Отсюда и "вторую руду не
    // берёт", и провал правила "куча в одной точке" - из сорока вещей склада
    // в такт попадали три, и порога в четверо не набиралось.
    // Теперь мест втрое больше, а когда и они кончаются, самый дальний
    // кандидат уступает место более близкому.
    // Обход сцены игра зовёт несколько раз за кадр, и каждый раз отдаёт
    // РАЗНЫЙ кусок: стоя на месте мы видели то 192 объекта, то 7. Из-за
    // этого список в плашке скакал, а прицел метался - в одном такте нужный
    // предмет есть, в другом только дальний фон.
    // Поэтому склеиваем куски: объект, увиденный за последнюю секунду,
    // остаётся в списке со своей последней позицией.
    struct Recent { uint32_t eid; DWORD seen; unsigned char *ent; Vec3 pos; };
    static Recent seen[512];
    static int seenN;

    static const int LIST_MAX = 192;
    Cand list[LIST_MAX]; int nn = 0;

    // Отвергнутые по расстоянию. Без них обзор врёт умолчанием: объект, не
    // попавший в радиус, исчезает бесследно, и не понять - то ли у него
    // кривая позиция, то ли его нет в сценах вовсе. Держим пятерых ближайших
    // с разложением по осям.
    // NB: имена far и near заняты заголовками Windows - отсюда outside.
    struct Outside { uint32_t eid; float d, dx, dy, dz; };
    Outside outside[5]; int farN = 0;
    int total = 0, withPos = 0;
    static int tagCount[256];
    if (verbose) memset(tagCount, 0, sizeof tagCount);
    Vec3 mp = { 0, 0, 0 }; bool haveMe = false;

    // Игрок редко меняется, а полный обход списков ради него стоил почти всего
    // такта. Поэтому помним его между тактами и лишь проверяем, что по адресу
    // по-прежнему сущность с меткой 0xA0. Не сошлось - ищем заново.
    static unsigned char *s_me;
    static uint32_t s_meEid;
    if (s_me) {
        uint32_t id;
        if (read_u32(s_me + ENT_EID, &id) && id == s_meEid &&
            (id >> 24) == EID_PLAYER_TAG) {
            me = s_me; playerEid = id; players = 1;
            read_u32(me + ENT_ROUTE, &playerRoute);
            haveMe = ent_pos(me, &mp);
            if (!haveMe) { me = 0; s_me = 0; }     // позиции нет - доверия тоже
        } else {
            s_me = 0;
        }
    }

    // Проход первый: ищем игрока по метке 0xA0 в старшем байте идентификатора.
    for (int off = 0x100; !me && off + 16 <= 0x200; off += 8) {
        uint32_t count, cap; uint64_t data;
        if (!read_u32(c + off, &count) || !read_u32(c + off + 4, &cap) ||
            !read_u64(c + off + 8, &data)) continue;
        if (!count || !cap || count > cap || cap > 0x10000) continue;
        if (!readable((void *)data, 8)) continue;
        unsigned char **arr = (unsigned char **)data;
        for (uint32_t i = 0; i < count && i < 3000; i++) {
            uint64_t ep;
            if (!read_u64(arr + i, &ep)) break;
            unsigned char *e = (unsigned char *)ep;
            if (!readable(e, 0x100)) continue;
            uint32_t id;
            if (!read_u32(e + ENT_EID, &id)) continue;
            if ((id >> 24) != EID_PLAYER_TAG) continue;
            players++;
            if (me) continue;
            me = e; playerEid = id;
            read_u32(e + ENT_ROUTE, &playerRoute);
            haveMe = ent_pos(e, &mp);
        }
    }
    s_me = me; s_meEid = playerEid;
    g_meEnt = me;                 // оракулу воровства игрок нужен вторым аргументом
    g_meEid = playerEid;
    inventory_refresh(me, verbose);

    // Переход между зонами (быстрое перемещение) - самое опасное для нас
    // место. Сцена уже подменена, а позиции ещё старые: в логе видно, как
    // полтора десятка РАЗНЫХ объектов одновременно оказались "в 0.6 м", и мод
    // отправил событие каждому. Ловим по скачку позиции игрока и молчим пару
    // секунд, пока мир не устоится. Обзор и плашка при этом работают - видно,
    // что происходит.
    // Признаков три, и расстояние из них самый слабый: перемещение в ту же
    // точку его не даёт вовсе, а мод при этом успевает нагрести. Два других
    // от расстояния не зависят - они ловят саму подмену сцены.
    static Vec3           lastPos = {0, 0, 0};
    static unsigned char *lastMe  = 0;
    static int            lastTotal = 0;
    static bool           havePrev = false;
    static DWORD          holdUntil = 0;
    const char           *why = 0;
    DWORD                 hold = 3000;
    char                  whyBuf[96];

    if (havePrev) {
        float jx = mp.x - lastPos.x, jy = mp.y - lastPos.y, jz = mp.z - lastPos.z;
        float jump2 = jx * jx + jy * jy + jz * jz;
        if (jump2 > 400.0f) {                    // больше 20 м за такт - не ходьба
            _snprintf(whyBuf, sizeof whyBuf - 1, "прыжок на %.0f м", (double)sqrt(jump2));
            why = whyBuf;
        } else if (me != lastMe) {
            // Актора игрока игра пересоздаёт не только на перемещении: то же
            // самое происходит при посадке в седло, в роликах и просто при
            // смене участка местности. Настоящим переходом это не является,
            // поэтому и пауза короткая - лишь бы не выстрелить в момент
            // подмены.
            why = "сменился актор игрока (седло, ролик или участок карты)";
            hold = 800;
        } else if (lastTotal > 20 && (total < lastTotal / 2 || total > lastTotal * 2)) {
            // Список сцены сменился целиком - старых объектов уже нет, новые
            // ещё не встали на места.
            _snprintf(whyBuf, sizeof whyBuf - 1, "сцена сменилась: было %d, стало %d",
                      lastTotal, total);
            why = whyBuf;
        }
    }
    if (why) {
        holdUntil = now + hold;
        g_doneN = 0;                 // память о собранном относится к прошлой зоне
        L("[переход] %s - пауза %.1f с", why, (double)hold / 1000.0);
        _snprintf(g_lastTrans, sizeof g_lastTrans - 1, "%s", why);
        g_lastTransAt = now;
    }
    lastPos = mp; lastMe = me; lastTotal = total; havePrev = true;
    bool settling = now < holdUntil;

    QueryPerformanceCounter(&ph1);
    if (!me) { if (verbose) L("[игрок] сущности с меткой 0xA0 в контексте нет"); return; }
    if (verbose) {
        L("[игрок] eid=%08X route=%08X тип=%02X  %s(%.1f, %.1f, %.1f)",
          playerEid, playerRoute, ent_type(me), haveMe ? "позиция " : "ПОЗИЦИИ НЕТ ", mp.x, mp.y, mp.z);
        if (players > 1) L("[игрок] внимание: сущностей с меткой 0xA0 найдено %d, взял первую", players);
    }
    if (!haveMe) { if (verbose) L("[игрок] без позиции расстояния не посчитать"); return; }

    // Проход второй: всё вокруг, с полями каждого объекта.
    for (int off = 0x100; off + 16 <= 0x200; off += 8) {
        uint32_t count, cap; uint64_t data;
        if (!read_u32(c + off, &count) || !read_u32(c + off + 4, &cap) ||
            !read_u64(c + off + 8, &data)) continue;
        if (!count || !cap || count > cap || cap > 0x10000) continue;
        if (!readable((void *)data, 8)) continue;
        if (verbose) L("[список] ctx+0x%03X: %u объектов из %u", off, count, cap);
        unsigned char **arr = (unsigned char **)data;
        for (uint32_t i = 0; i < count && i < 3000; i++) {
            uint64_t ep;
            if (!read_u64(arr + i, &ep)) break;
            unsigned char *e = (unsigned char *)ep;
            if (!readable(e, 0x100)) continue;
            uint32_t id;
            if (!read_u32(e + ENT_EID, &id)) continue;
            // Считаем метки. Мод берёт только 0xB0, и руда, которой не
            // оказалось в списке при том, что игрок стоит на ней, вполне
            // может носить другую. Сводка покажет, кого мы не видим вовсе.
            if (verbose) tagCount[id >> 24]++;
            if ((id >> 24) != EID_WORLD_TAG) continue;
            bool dup = false;
            for (int k = 0; k < nn; k++) if (list[k].eid == id) { dup = true; break; }
            if (dup) continue;
            total++;
            Vec3 q;
            if (!ent_pos(e, &q)) continue;
            withPos++;
            float dx = q.x - mp.x, dy = q.y - mp.y, dz = q.z - mp.z;
            float d = (float)sqrt(dx * dx + dy * dy + dz * dz);
            if (d > g_cfg.scanRange) {
                if (verbose) {
                    int w = -1;
                    if (farN < 5) w = farN++;
                    else { int bad = 0;
                        for (int k2 = 1; k2 < 5; k2++) if (outside[k2].d > outside[bad].d) bad = k2;
                        if (outside[bad].d > d) w = bad; }
                    if (w >= 0) { outside[w].eid = id; outside[w].d = d;
                                  outside[w].dx = dx; outside[w].dy = dy; outside[w].dz = dz; }
                }
                continue;
            }
            // ОДНА сущность лежит сразу в НЕСКОЛЬКИХ списках контекста игры
            // (+0x140, +0x150, +0x1D0, +0x1E0...). Проверки на повтор тут не
            // было, и объект попадал в рабочий список столько раз, в скольких
            // списках он числится. Дальше цикл сбора честно слал событие
            // каждой записи - и игрок получал столько же копий предмета.
            // Это и есть "подбирается по 10-100 штук за раз": в логе видно
            // две отправки на один и тот же eid в один и тот же такт.
            bool dupEid = false;
            for (int k3 = 0; k3 < nn; k3++)
                if (list[k3].eid == id) { dupEid = true; break; }
            if (dupEid) continue;

            int slot = nn;
            if (nn >= LIST_MAX) {
                // Список полон: ищем самого дальнего и, если он дальше нового,
                // отдаём его место. Иначе новый кандидат нам не интересен.
                int worst = 0;
                for (int k2 = 1; k2 < LIST_MAX; k2++)
                    if (list[k2].d > list[worst].d) worst = k2;
                if (list[worst].d <= d) continue;
                slot = worst;
            } else nn++;

            // Запомнить, что видели: пригодится, когда следующий кусок
            // придёт без него.
            {
                int slot2 = -1;
                for (int r = 0; r < seenN && r < 512; r++)
                    if (seen[r].eid == id) { slot2 = r; break; }
                if (slot2 < 0 && seenN < 512) slot2 = seenN++;
                if (slot2 < 0) { for (int r = 0; r < 512; r++)
                        if (now - seen[r].seen > 2000) { slot2 = r; break; } }
                if (slot2 >= 0) { seen[slot2].eid = id; seen[slot2].seen = now;
                                  seen[slot2].ent = e; seen[slot2].pos = q; }
            }

            Cand k = {};
            k.eid = id; k.d = d; k.pos = q;
            k.src = (uint16_t)off; k.ent = e;
            read_u32(e + ENT_ROUTE, &k.route);
            k.type = ent_type(e);
            k.parent = ent_parent(e);
            // Компоненты здесь НЕ читаем: этот цикл проходит по всем сотням
            // объектов сцены каждый кадр. Подробности берём потом и только у
            // тех, до кого дошло дело - см. fill_details.
            list[slot] = k;
        }
    }
    if (verbose) {
        int can = 0;
        for (int i = 0; i < nn; i++) { Action a; fill_details(list[i]); if (!skip_reason(list[i], &a)) can++; }
        L("[scan] всего %d, с позицией %d, в радиусе %.0f м - %d, из них можно взять %d",
          total, withPos, g_cfg.scanRange, nn, can);
        {   char tg[300]; int tn2 = 0;
            for (int t = 0; t < 256; t++) if (tagCount[t])
                tn2 += _snprintf(tg + tn2, sizeof tg - tn2 - 1, " %02X:%d", t, tagCount[t]);
            tg[tn2 > 0 ? tn2 : 0] = 0;
            L("  [метки] сущностей по старшему байту:%s  (берём только %02X)",
              tg, EID_WORLD_TAG);
        }
        for (int i = 0; i < farN; i++)
            L("  [далеко] eid=%08X %.1f м  по осям dx=%.1f dy=%.1f dz=%.1f",
              outside[i].eid, outside[i].d, outside[i].dx, outside[i].dy, outside[i].dz);
        // Сырьё по ближним отвергнутым: локальная позиция, родитель и то, что
        // лежит в поле +0xEC. Формула "мир = локальная + родительская"
        // сошлась на игроке, но на объектах, похоже, нет - надо видеть числа.
        for (int i = 0; i < farN; i++) {
            unsigned char *e2 = 0;
            for (int off = 0x100; off + 16 <= 0x200 && !e2; off += 8) {
                uint32_t count, cap; uint64_t data;
                if (!read_u32(c + off, &count) || !read_u32(c + off + 4, &cap) ||
                    !read_u64(c + off + 8, &data)) continue;
                if (!count || !cap || count > cap || cap > 0x10000) continue;
                if (!readable((void *)data, 8)) continue;
                unsigned char **arr2 = (unsigned char **)data;
                for (uint32_t j = 0; j < count && j < 3000; j++) {
                    uint64_t ep2; if (!read_u64(arr2 + j, &ep2)) break;
                    unsigned char *e3 = (unsigned char *)ep2;
                    uint32_t id2;
                    if (!readable(e3, 0x100) || !read_u32(e3 + ENT_EID, &id2)) continue;
                    if (id2 == outside[i].eid) { e2 = e3; break; }
                }
            }
            if (!e2) continue;
            void *s2 = deref(e2, ENT_SUBOBJ);
            void *t2 = s2 ? deref(s2, SUB_TRANSFORM) : 0;
            if (!t2 || !readable(t2, TF_PARENT_POS + 12)) continue;
            unsigned char *t = (unsigned char *)t2;
            Vec3 loc = *(Vec3 *)(t + TF_POS);
            Vec3 par = *(Vec3 *)(t + TF_PARENT_POS);
            uint32_t pe = *(uint32_t *)(t + TF_PARENT_EID);
            L("      %08X: локально (%.1f %.1f %.1f) родитель %08X его поле (%.1f %.1f %.1f)",
              outside[i].eid, loc.x, loc.y, loc.z, pe, par.x, par.y, par.z);
        }
    }

    // Добираем тех, кого в этом куске не оказалось, но видели только что.
    for (int r = 0; r < seenN && nn < LIST_MAX; r++) {
        if (!seen[r].eid || now - seen[r].seen > 1000) continue;
        bool have = false;
        for (int i = 0; i < nn; i++) if (list[i].eid == seen[r].eid) { have = true; break; }
        if (have) continue;
        if (!readable(seen[r].ent, 0x100)) continue;
        uint32_t id2;
        if (!read_u32(seen[r].ent + ENT_EID, &id2) || id2 != seen[r].eid) continue;
        float dx = seen[r].pos.x - mp.x, dy = seen[r].pos.y - mp.y, dz = seen[r].pos.z - mp.z;
        float d2 = (float)sqrt(dx*dx + dy*dy + dz*dz);
        if (d2 > g_cfg.scanRange) continue;
        Cand k = {};
        k.eid = seen[r].eid; k.d = d2; k.pos = seen[r].pos; k.ent = seen[r].ent;
        read_u32(k.ent + ENT_ROUTE, &k.route);
        k.type = ent_type(k.ent);
        k.parent = ent_parent(k.ent);
        list[nn++] = k;
    }

    for (int a = 0; a < nn; a++)
        for (int b = a + 1; b < nn; b++)
            if (list[b].d < list[a].d) { Cand t = list[a]; list[a] = list[b]; list[b] = t; }

    // Один куст - две сущности. Игра держит рядом пару объектов с соседними
    // eid: у одного на узле лежат данные сбора или предмета, у второго узел
    // пустой. Раньше мод показывал обе строки и слал событие обеим, то есть
    // каждый второй сбор уходил в никуда. Пустого соседа глушим: если в
    // полуметре стоит объект того же типа С данными - этот считается его
    // двойником. Полметра выбрано с запасом, пары стоят вплотную.
    // Уже облутанное не разбираем вовсе. Полный разбор объекта - это чтение
    // компонент, имён через RTTI, таблиц предметов и вызов оракула владельца
    // в саму игру. Делать это каждый такт для сотни трупов, которые мод уже
    // обошёл, - чистая потеря кадров, на что и жалуются в густых местах.
    for (int i = 0; i < nn; i++) {
        if (already_searched((uint64_t)list[i].eid)) {
            list[i].filled = true;          // считаем разобранным и не трогаем
            list[i].banned = true;
            continue;
        }
        fill_details(list[i]);
    }

    // ВСЁ, что под носом, со всеми полями и причиной отказа. Сундук и короб
    // для хранения не показывались ни одной строкой, а гадать, какой из
    // фильтров их съел, дороже, чем один раз напечатать сырьё.
    if (verbose) {
        L("[рядом] всё в 4 м, со всеми полями:");
        int shown4 = 0;
        for (int i = 0; i < nn && shown4 < 20; i++) {
            if (list[i].d > 4.0f) continue;
            Action a7; const char *w7 = skip_reason(list[i], &a7);
            L("  %08X %4.1f м тип %02X кат %d/%02X %s%s%s%s%s%s  узел=%s имя=%s -> %s",
              list[i].eid, list[i].d, list[i].type, (int)list[i].cat, list[i].cat2,
              list[i].inter  ? "inter " : "",
              list[i].item   ? "item "  : "",
              list[i].gather ? "gather " : "",
              list[i].dead   ? "dead "  : "",
              list[i].twin   ? "twin "  : "",
              list[i].locked == 1 ? "locked " : "",
              list[i].nodeName ? list[i].nodeName : "-",
              list[i].name ? list[i].name : "-",
              w7 ? w7 : act_name(a7));
            shown4++;
        }
        if (!shown4) L("  в 4 м вообще ничего - объекта нет в списке сцены");
    }

    // Опыт: позвать зарядку самим. Строго по одному узлу за нажатие и только
    // на игровом потоке. Подсмотренный arg3 у клиентского узла выглядел как
    // адрес в стеке чужого кадра - подставлять его нельзя, к нашему вызову он
    // давно протух. Даём вместо него свой обнулённый буфер: если функции нужна
    // только рабочая область, этого хватит, а если она ждёт там данные -
    // увидим по тому, что ничего не изменится.
    LONG armAim = InterlockedExchange(&g_armAimReq, 0);
    if (InterlockedExchange(&g_armReq, 0) || armAim) {
        if (!g_armFn) L("[зарядка] функция не найдена, звать нечего");
        else {
            void *inter2 = 0; uint32_t aeid = 0; float ad = 0;
            // Numpad1 - зарядить ИМЕННО то, на что смотришь. Ближайший узел
            // (Numpad5) в куче объектов почти никогда не тот, который нужен;
            // прицел теперь работает, так что выбирать можно глазами.
            // Требования "узел должен быть пустым" тут НЕТ: ручное нажатие -
            // это опыт, и повторно позвать зарядку на уже заряженном узле
            // тоже бывает нужно.
            uint32_t aimE = armAim ? (uint32_t)InterlockedCompareExchange(&g_aimIdxEid, 0, 0) : 0;
            for (int i = 0; i < nn && !inter2; i++) {
                if (armAim) {
                    if (!aimE || list[i].eid != aimE) continue;
                } else {
                    if (list[i].item || list[i].gather) continue;
                }
                if (!list[i].inter || list[i].parent == g_meEid) continue;
                void *sub2 = deref(list[i].ent, ENT_SUBOBJ);
                void *g2 = comp_by_class(sub2, CLS_GIMMICK);
                if (!g2) continue;
                inter2 = g2; aeid = list[i].eid; ad = list[i].d;
            }
            if (!inter2) L(armAim ? "[зарядка] прицел ни на чём, или у цели нет узла"
                                  : "[зарядка] рядом нет незаряженного узла");
            else {
                static __declspec(align(16)) unsigned char scratch[256];
                memset(scratch, 0, sizeof scratch);
                void *before = deref(inter2, INTER_GATHER);
                LONG mode = InterlockedCompareExchange(&g_armMode, 0, 0);
                L("[зарядка] зову для eid=%08X (%.1f м), узел=%p, режим=%ld, игрок=%08X",
                  aeid, ad, inter2, mode, playerEid);
                int r = arm_call(inter2, (uintptr_t)mode, scratch, (uintptr_t)playerEid);
                if (r == 2) {
                    L("[зарядка] поставлена в очередь - исполнит игровой поток");
                } else if (r == 1) {
                    void *after = deref(inter2, INTER_GATHER);
                    L("[зарядка] вернулась. данные сбора: было %p, стало %p%s",
                      before, after, (before != after) ? "  <- ЗАРЯДИЛОСЬ" : "");
                } else {
                    L("[зарядка] исключение внутри вызова - так звать нельзя");
                }
            }
        }
    }

    // Наведённая цель. Игра действует по ней в сундуках, карманах и снятии
    // шкуры, а мы ею не управляем. Найти, где она лежит, можно тем же
    // приёмом, что сработал с оракулом воровства: не вычислять, а искать
    // готовое значение. Перебираем поля игрока и его компонент и смотрим, не
    // лежит ли там eid одного из объектов вокруг - тот, на который игра
    // сейчас предлагает нажать.
    // Какой из вариантов взгляда верный. У матрицы вида направление может
    // лежать в любой из трёх строк и в любом знаке - шесть вариантов. Вместо
    // очередной догадки пусть мод покажет, какой объект выбирает каждый:
    // наведись на известную вещь, нажми - и станет видно, кто угадал.
    if (InterlockedExchange(&g_aimReq, 0) && me) {
        // Сторож на входе требовал у матрицы позицию рядом с игроком, а её
        // там нет - матрица чисто поворотная. Проверяем только наличие.
        if (!g_camN) L("[взгляд] камера ещё не найдена - сначала Numpad 7");
        else {
            // Позиции в матрице камеры нет - это чистый поворот. Поэтому
            // берём первую же ортонормированную и считаем луч ОТ ИГРОКА:
            // камера в паре метров, на дистанции подбора это единицы
            // градусов, а раньше проверка требовала позицию и всё отбрасывала.
            const float *f = 0;
            for (int i = 0; i < g_camN; i++)
                if (readable(g_cam[i], 0x40) && ortho3((const float *)g_cam[i])) {
                    f = (const float *)g_cam[i]; break;
                }
            if (!f) { L("[взгляд] живой буфер не нашёлся"); }
            else {
                L("[взгляд] поворот камеры взят, луч от игрока, шесть вариантов");
                static const char *nm[6] = { "строка1 +", "строка1 -", "строка2 +",
                                             "строка2 -", "строка3 +", "строка3 -" };
                for (int v = 0; v < 6; v++) {
                    int row = v / 2, sign = (v & 1) ? -1 : 1;
                    Vec3 dir;
                    dir.x = f[row * 4 + 0] * sign;
                    dir.y = f[row * 4 + 1] * sign;
                    dir.z = f[row * 4 + 2] * sign;
                    int bestI = -1; float best = 1e9f;
                    for (int i = 0; i < nn; i++) {
                        if (list[i].parent == g_meEid) continue;
                        float dx = list[i].pos.x - mp.x;
                        float dy = list[i].pos.y - mp.y;
                        float dz = list[i].pos.z - mp.z;
                        float d = (float)sqrt(dx*dx + dy*dy + dz*dz);
                        if (d < 0.3f || d > 25.0f) continue;
                        float along = dx*dir.x + dy*dir.y + dz*dir.z;
                        if (along <= 0.0f) continue;
                        float o2 = d*d - along*along; if (o2 < 0.0f) o2 = 0.0f;
                        float o = (float)sqrt(o2);
                        float score = o + d * 0.05f;
                        if (score < best) { best = score; bestI = i; }
                    }
                    if (bestI < 0) L("    %s: никого", nm[v]);
                    else L("    %s: %04X %.1f м  %s", nm[v], list[bestI].eid & 0xFFFF,
                           list[bestI].d, list[bestI].name ? list[bestI].name
                           : (list[bestI].nodeName ? list[bestI].nodeName : "без имени"));
                }
            }
        }
    }

    // Камеру надо находить САМОСТОЯТЕЛЬНО: в релизной сборке клавиш разведки
    // нет, а прицел без камеры не работает. Запрос 2 - только цепочка от
    // глобала (десятки миллисекунд, один раз), без слепого обхода памяти:
    // тот стоит полторы секунды и годится лишь для ручной разведки.
    // Только при Debug=1. Камера нужна ради меток и прицельной клавиши, а в
    // готовой сборке нет ни того, ни другого - и поиск, перебирающий восемь
    // мегабайт указателей раз в пять секунд, стал бы чистой тратой чужих
    // кадров. Отрисовку я выключил сразу, а вот про сам поиск забыл.
    if (!g_camN && g_debug && InterlockedCompareExchange(&g_marksOn, 0, 0)) {
        static ULONGLONG lastTry = 0;
        ULONGLONG now9 = GetTickCount64();
        if (now9 - lastTry > 5000) { lastTry = now9; InterlockedExchange(&g_camReq, 2); }
    }
    LONG camReq = InterlockedExchange(&g_camReq, 0);
    if (camReq) {
        // Цепочка к камере, вынутая из кода чужого ESP-мода (он собран под
        // 1.18.02, у нас 2658 - смещения могли уехать, поэтому печатаем всё):
        //     глобал framework = база + 0x6330C80
        //     framework +0x20  -> менеджер
        //     менеджер  +0x10C8-> мир (у ESP сверяется с менеджером акторов)
        //     framework +0x50  -> объект камеры, его vtable = база+0x4CEDC78
        // Соседство обнадёживает: наша очередь событий лежит на +0x6330F98,
        // менеджер персонажей на +0x6330FD0 - та же область глобалов.
        // Глобал из ESP (+0x6330C80) на 2.00 пуст - уехал. Но форма известна:
        // у объекта framework по +0x50 лежит камера. Значит ищем не адрес, а
        // структуру: пробегаем область глобалов и проверяем каждого кандидата
        // по имени класса через RTTI. Узкий поиск, без блужданий по памяти.
        unsigned char *fwp = g_game.base + 0x6330C80;
        if (!readable(fwp, 8) || !*(void **)fwp) {
            L("[камера] глобал ESP пуст - ищу framework по форме в области глобалов");
            unsigned char *from = g_game.base + 0x6000000;
            unsigned char *to   = g_game.base + 0x6800000;
            if (to > g_game.base + g_game.size) to = g_game.base + g_game.size - 8;
            int found = 0;
            for (unsigned char *q = from; q < to && found < 6; q += 8) {
                if (!readable(q, 8)) { q += 0xFF8; continue; }
                unsigned char *cand = *(unsigned char **)q;
                if (!cand || !readable(cand, 0x60)) continue;
                unsigned char *cam2 = *(unsigned char **)(cand + 0x50);
                if (!cam2 || !readable(cam2, 8)) continue;
                const char *cls2 = safe_class(cam2);
                if (!cls2 || !stristr_ru(cls2, "camera")) continue;
                // Отражение (SimpleReflectPropertyBind и подобное) лежит в
                // самом образе игры и к камере отношения не имеет - нам нужен
                // именно менеджер. Берём его и прекращаем поиск: раньше цикл
                // шёл дальше и перезаписывал выбор последним, ложным.
                bool real = stristr_ru(cls2, "CameraManager") != 0;
                L("    глобал +0x%llX -> framework %p -> камера %p (%s)%s",
                  (unsigned long long)(q - g_game.base), cand, cam2, cls2,
                  real ? "   <- берём этот" : "");
                found++;
                if (real) { fwp = q; break; }
            }
            if (!found) L("    по форме ничего не нашлось");
        }
        L("[камера] цепочка ESP, глобал +0x%llX",
          (unsigned long long)(fwp - g_game.base));
        if (!readable(fwp, 8)) L("    глобал не читается");
        else {
            unsigned char *fw = *(unsigned char **)fwp;
            L("    framework = %p", fw);
            if (fw && readable(fw, 0x60)) {
                unsigned char *mgr = *(unsigned char **)(fw + 0x20);
                unsigned char *cam = *(unsigned char **)(fw + 0x50);
                L("    менеджер (+0x20) = %p", mgr);
                L("    камера   (+0x50) = %p", cam);
                if (mgr && readable(mgr + 0x10C8, 8))
                    L("    мир (менеджер+0x10C8) = %p", *(void **)(mgr + 0x10C8));
                // Ищем матрицу ВНУТРИ менеджера камеры: та же проверка, что
                // и при слепом обходе, но теперь в пределах одного объекта,
                // опознанного по RTTI. Ложным срабатываниям тут взяться
                // почти неоткуда.
                if (cam && readable(cam, 0x600)) {
                    // Точный путь, вынутый из функции ESP (+0xF460):
                    //     объект камеры +0x50 -> трансформ
                    //     трансформ     +0x88 -> матрица 4x4 (четыре строки)
                    // Именно его я и не проверял: искал в самом менеджере и в
                    // потомках по первым указателям, а нужный объект - за 0x50.
                    {
                        unsigned char *tf2 = *(unsigned char **)(cam + 0x50);
                        L("    камера+0x50 -> трансформ %p", tf2);
                        if (tf2 && readable(tf2, 0x120)) {
                            const char *tc = safe_class(tf2);
                            L("      класс: %s", tc ? tc : "имя не читается");
                            const float *m2 = (const float *)(tf2 + 0x88);
                            Vec3 cp2; cp2.x = m2[12]; cp2.y = m2[13]; cp2.z = m2[14];
                            float ax = cp2.x - mp.x, ay = cp2.y - mp.y, az = cp2.z - mp.z;
                            float dd2 = (float)sqrt(ax*ax + ay*ay + az*az);
                            L("      матрица +0x88: перенос (%.1f %.1f %.1f), от игрока %.1f м",
                              cp2.x, cp2.y, cp2.z, dd2);
                            L("      строки: (%.2f %.2f %.2f) (%.2f %.2f %.2f) (%.2f %.2f %.2f)",
                              m2[0], m2[1], m2[2], m2[4], m2[5], m2[6], m2[8], m2[9], m2[10]);
                            L("      ортонормирована: %s", ortho3(m2) ? "ДА" : "нет");
                            // Перенос нулевой - значит это чистый поворот, а
                            // позиция лежит отдельно. ESP копирует после
                            // матрицы ещё +0xC8 (8 байт) и +0xD0. Смотрим и
                            // четвёртый столбец: в матрицах по столбцам
                            // перенос стоит именно там.
                            L("      столбец 4: (%.1f %.1f %.1f)", m2[3], m2[7], m2[11]);
                            // Позиция камеры по ESP лежит в +0xC8 - и она там
                            // ЕСТЬ, но в АБСОЛЮТНЫХ мировых координатах, а наши
                            // позиции локальные (мы складываем объект с
                            // родителем). В Бездне разница вышла (-9995, 0,
                            // -3002) - целый сектор мира. Поэтому печатаем весь
                            // трансформ и отдельно ищем в нём тройку, близкую к
                            // игроку: это и будет позиция в НАШЕМ кадре.
                            const float *ex2 = (const float *)(tf2 + 0xC8);
                            L("      +0xC8 (позиция ESP): %.2f %.2f %.2f  -> от игрока по осям "
                              "(%.1f %.1f %.1f)", ex2[0], ex2[1], ex2[2],
                              ex2[0] - mp.x, ex2[1] - mp.y, ex2[2] - mp.z);
                            // Подробный дамп - только по клавише: авто-поиск
                            // работает и в релизе, засорять там лог незачем.
                            if (camReq == 1) {
                            L("      ищу в трансформе тройку рядом с игроком:");
                            int nearN = 0;
                            for (int o6 = 0; o6 + 12 <= 0x120; o6 += 4) {
                                const float *f6 = (const float *)(tf2 + o6);
                                bool ok6 = true;
                                for (int k6 = 0; k6 < 3; k6++)
                                    if (!(f6[k6] > -1e7f && f6[k6] < 1e7f)) ok6 = false;
                                if (!ok6) continue;
                                float gx = f6[0] - mp.x, gy = f6[1] - mp.y, gz = f6[2] - mp.z;
                                float dg = (float)sqrt(gx*gx + gy*gy + gz*gz);
                                if (dg > 30.0f || dg < 0.2f) continue;
                                L("        +0x%03X: (%.2f %.2f %.2f)  %.1f м от игрока",
                                  o6, f6[0], f6[1], f6[2], dg);
                                if (++nearN >= 12) break;
                            }
                            if (!nearN) L("        ничего в 30 м - позиция камеры "
                                          "хранится только абсолютной");
                            L("      весь трансформ по 4 float:");
                            for (int o7 = 0; o7 + 16 <= 0x120; o7 += 16)
                                L("        +0x%03X  %12.3f %12.3f %12.3f %12.3f", o7,
                                  ((const float *)(tf2 + o7))[0], ((const float *)(tf2 + o7))[1],
                                  ((const float *)(tf2 + o7))[2], ((const float *)(tf2 + o7))[3]);
                            }
                            if (g_camN < 16) g_cam[g_camN++] = tf2 + 0x88;
                            g_camTf = tf2;
                        } else L("      трансформ не читается");
                    }

                    L("    ищу матрицу внутри CameraManager:");
                    for (int o3 = 0; o3 + 0x40 <= 0x600; o3 += 4) {
                        const float *f3 = (const float *)(cam + o3);
                        if (!ortho3(f3)) continue;
                        // Пробуем оба вида матрицы: камера->мир (позиция прямо
                        // в переносе) и мир->камера (позиция = -R^T * t).
                        Vec3 cpos; cpos.x = f3[12]; cpos.y = f3[13]; cpos.z = f3[14];
                        float dx = cpos.x - mp.x, dy = cpos.y - mp.y, dz = cpos.z - mp.z;
                        float d3 = dx*dx + dy*dy + dz*dz;
                        const char *kind = "камера->мир";
                        if (d3 > 400.0f || d3 < 0.01f) {
                            cam_pos_from_view(f3, &cpos);
                            dx = cpos.x - mp.x; dy = cpos.y - mp.y; dz = cpos.z - mp.z;
                            d3 = dx*dx + dy*dy + dz*dz;
                            kind = "мир->камера";
                        }
                        if (d3 > 400.0f || d3 < 0.01f) continue;
                        (void)kind;
                        L("      +0x%03X (%s): камера (%.1f %.1f %.1f), от игрока %.1f м, "
                          "стр3 (%.2f %.2f %.2f)", o3, kind, cpos.x, cpos.y, cpos.z,
                          (double)sqrt(d3), f3[8], f3[9], f3[10]);
                        if (g_camN < 16) {
                            bool dup2 = false;
                            for (int k3 = 0; k3 < g_camN; k3++)
                                if (g_cam[k3] == cam + o3) dup2 = true;
                            if (!dup2) g_cam[g_camN++] = cam + o3;
                        }
                    }
                    if (!g_camN) {
                        L("      матрицы в самом менеджере нет - иду к потомкам");
                        // Менеджер держит камеры, а матрица лежит в активной.
                        // Обходим его указатели, называем каждого по RTTI и
                        // ищем матрицу уже внутри.
                        for (int o4 = 8; o4 + 8 <= 0x100; o4 += 8) {
                            unsigned char *kid = *(unsigned char **)(cam + o4);
                            if (!kid || !readable(kid, 0x400)) continue;
                            const char *kc = safe_class(kid);
                            int hits2 = 0;
                            for (int o5 = 0; o5 + 0x40 <= 0x400; o5 += 4) {
                                const float *f4 = (const float *)(kid + o5);
                                float ex = f4[12] - mp.x, ey = f4[13] - mp.y, ez = f4[14] - mp.z;
                                float dd = ex*ex + ey*ey + ez*ez;
                                if (dd > 400.0f || dd < 0.01f) continue;
                                if (!ortho3(f4)) continue;
                                L("      %s+0x%02X (%s) матрица +0x%03X: (%.1f %.1f %.1f), "
                                  "от игрока %.1f м", "менеджер", o4,
                                  kc ? kc : "класс не читается", o5,
                                  f4[12], f4[13], f4[14], (double)sqrt(dd));
                                if (g_camN < 16) g_cam[g_camN++] = kid + o5;
                                hits2++;
                                if (hits2 >= 2) break;
                            }
                            if (!hits2 && kc)
                                L("      менеджер+0x%02X: %s - матрицы нет", o4, kc);
                        }
                    }
                }
                if (cam && readable(cam, 0x80)) {
                    void *vt = *(void **)cam;
                    L("    таблица методов камеры = %p (+0x%llX), у ESP было +0x4CEDC78",
                      vt, (unsigned long long)((unsigned char *)vt - g_game.base));
                    const char *cls = safe_class(cam);
                    L("    класс: %s", cls ? cls : "имя не читается");
                    char hx3[300];
                    for (int o2 = 0; o2 < 0x80; o2 += 0x40) {
                        hexs(cam + o2, 0x40, hx3, sizeof hx3);
                        L("    +0x%02X: %s", o2, hx3);
                    }
                }
            }
        }

        if (camReq == 1) {          // слепой обход - только по клавише
            if (!g_camHitN) { L("[камера] ищу..."); camera_hunt(mp); }
            else { L("[камера] проверяю: ПОВЕРНИСЬ НА МЕСТЕ между нажатиями");
                   camera_verify(mp); }
        }
    }

    // Сторож: берём ближайший узел БЕЗ данных - именно в него игра и запишет,
    // когда решит его зарядить.
    if (InterlockedExchange(&g_watchReq, 0)) {
        void *field = 0; uint32_t weid = 0; float wd = 0;
        for (int i = 0; i < nn && !field; i++) {
            if (list[i].item || list[i].gather) continue;
            if (!list[i].inter || list[i].parent == g_meEid) continue;
            void *sub2 = deref(list[i].ent, ENT_SUBOBJ);
            void *inter2 = comp_by_class(sub2, CLS_GIMMICK);
            if (!inter2) continue;
            field = (unsigned char *)inter2 + INTER_GATHER;
            weid = list[i].eid; wd = list[i].d;
        }
        if (field) { L("[сторож] цель eid=%08X, %.1f м", weid, wd); watch_start(field); }
        else L("[сторож] рядом нет незаряженного узла");
    }

    // Момент включения узла. Данные сбора и предмета игра кладёт на узел
    // только когда считает взаимодействие доступным: одна и та же жила пять
    // замеров подряд читается как "сбор=0" и вдруг становится "сбор=1", при
    // том же расстоянии. Отмечаем переход в лог - тогда, если в этот момент
    // включено подслушивание (F7), рядом окажется событие игры, которым она
    // узел и включает. Это единственный путь взять руду, не наступая на неё.
    {
        // Состояние узла на прошлом такте, чтобы ловить переходы в обе
        // стороны. Первая версия запоминала eid навсегда и про повторное
        // включение молчала - по такому логу нельзя было утверждать, что
        // жила не включалась, а только что мы этого не увидели.
        struct Armed { uint32_t eid; bool on; DWORD seen; };
        static Armed armed[128];
        for (int i = 0; i < nn; i++) {
            // Своя экипировка мигает включением каждый такт и забивает лог.
            if (list[i].parent && list[i].parent == g_meEid) continue;
            bool on = list[i].item || list[i].gather;
            int slot = -1, free2 = -1;
            for (int k2 = 0; k2 < 128; k2++) {
                if (armed[k2].eid == list[i].eid) { slot = k2; break; }
                if (free2 < 0 && (!armed[k2].eid || now - armed[k2].seen > 30000)) free2 = k2;
            }
            if (slot < 0) {
                if (free2 < 0) continue;
                armed[free2].eid = list[i].eid; armed[free2].on = on;
                armed[free2].seen = now;
                continue;
            }
            armed[slot].seen = now;
            if (armed[slot].on == on) continue;
            armed[slot].on = on;
            L("[узел] eid=%08X %s (%s), d=%.1f м h%+.1f", list[i].eid,
              on ? "ВКЛЮЧИЛСЯ" : "погас",
              list[i].gather ? "сбор" : (list[i].item ? "предмет" : "пусто"),
              list[i].d, list[i].pos.y - mp.y);
        }
    }

    // Куча в ОДНОЙ ТОЧКЕ - это не рассыпанный по земле лут, а содержимое
    // ёмкости: на путевой точке мод так разбирал по вещи за такт склад самого
    // игрока - шесть объектов сплошным блоком eid, все на 0.6 м от игрока.
    //
    // Радиус тут критичен. Сначала стояло полметра, и правило начало глотать
    // гроздья руды и кустов: они растут как раз с такими промежутками, вся
    // гроздь считалась "ёмкостью" и пропускалась, а стоило сдвинуться - счёт
    // падал ниже порога и одна штука вдруг становилась доступной. Со стороны
    // это выглядело как "постоял рядом, и оно само собралось". Содержимое
    // ёмкости лежит в одной точке, поэтому 15 см - и ни сантиметром больше.
    for (int i = 0; i < nn; i++) {
        int around = 0;   // NB: имя near занято заголовками Windows
        for (int j = 0; j < nn; j++) {
            if (j == i) continue;
            float dx = list[j].pos.x - list[i].pos.x;
            float dy = list[j].pos.y - list[i].pos.y;
            float dz = list[j].pos.z - list[i].pos.z;
            if (dx * dx + dy * dy + dz * dz <= 0.0225f) around++;  // 15 см
        }
        if (around >= 3) list[i].heap = true;                     // четверо и больше
    }
    for (int i = 0; i < nn; i++) {
        Cand &e = list[i];
        if (e.gather || e.item || e.dead == 1 || !e.inter) continue;
        for (int j = 0; j < nn; j++) {
            if (j == i) continue;
            Cand &o = list[j];
            if (!o.gather && !o.item) continue;
            if (o.type != e.type) continue;
            float dx = o.pos.x - e.pos.x, dy = o.pos.y - e.pos.y, dz = o.pos.z - e.pos.z;
            if (dx * dx + dy * dy + dz * dz > 0.25f) continue;   // 0.5 м
            e.twin = true;
            break;
        }
    }

    // На что смотрит игрок. Наведённую цель игра не отдаёт, поэтому считаем
    // её сами: берём объект с наименьшим углом к вектору взгляда. Ближние
    // при равном угле выигрывают - иначе прицел цепляет дальний фон.
    int aimIdx = -1;
    Vec3 aimFwd = { 0, 0, 0 };   // направление взгляда, для углов в плашке
    bool haveFwd = false;

    // Настоящая цель игры. Обработчик взаимодействия берёт её из компоненты
    // ClientInteractionActorComponent (sub+0x98), поле +0x20; ноль означает
    // "ни на что не наведено". Луч из кватерниона, которым я это подменял,
    // врал - он считал поворот тела, а игрок смотрит камерой.
    {
        void *ic = deref(deref(me, ENT_SUBOBJ), 0x98);
        if (ic && readable(ic, 0x40)) {
            uint32_t tgt = ask_aim(ic);
            if (!tgt) tgt = *(uint32_t *)((unsigned char *)ic + 0x20);
            if (tgt) {
                for (int i = 0; i < nn; i++)
                    if (list[i].eid == tgt) { aimIdx = i; break; }
                // Не нашли среди соседей - всё равно покажем число, пусть
                // будет видно, что игра на что-то навелась.
                if (aimIdx < 0)
                    _snprintf(g_aimText, sizeof g_aimText - 1,
                              "цель игры %08X (нет в списке)", tgt);
                else g_aimText[0] = 0;
            }
        }
    }

    // Луч взгляда как замена цели УБРАН. Он считался по повороту тела, а
    // игрок смотрит камерой: на живой проверке при взгляде на щит в метре
    // строка показывала шкаф в 3.5 м. Врущая подсказка хуже отсутствующей.
    // Настоящая цель игры в структурах не нашлась (см. заметки), так что
    // строка "смотрю на" появляется, только когда игра сама её отдаёт.
    // ПРИЦЕЛ. Проверено живьём: метки садятся на предметы точно, значит и
    // камера, и её оси, и точка отсчёта верны.
    //     взгляд    - ТРЕТИЙ СТОЛБЕЦ матрицы (m[2], m[6], m[10]), без минуса;
    //                 игра держит тот же вектор отдельным полем трансформ+0xD4,
    //                 и он совпал до сотых - берём его, если читается;
    //     начало    - глаз камеры (cam_origin), а не ноги игрока;
    //     масштабы  - трансформ+0x08 и +0x1C.
    // Перебор шести вариантов направления (g_aimVar) больше не нужен и убран:
    // вариант известен и подтверждён.
    if (aimIdx < 0 && g_camN && g_debug &&
        InterlockedCompareExchange(&g_marksOn, 0, 0)) {
        const float *m4 = 0;
        for (int i = 0; i < g_camN; i++)
            if (readable(g_cam[i], 0x40) && ortho3((const float *)g_cam[i])) {
                m4 = (const float *)g_cam[i]; break;
            }
        if (m4) {
            Vec3 fw4;
            fw4.x = m4[2]; fw4.y = m4[6]; fw4.z = m4[10];
            // Готовый вектор взгляда из самой игры, если он на месте.
            if (g_camTf && readable(g_camTf + 0xD4, 12)) {
                const float *fd = (const float *)(g_camTf + 0xD4);
                float ln = fd[0]*fd[0] + fd[1]*fd[1] + fd[2]*fd[2];
                if (ln > 0.9f && ln < 1.1f) { fw4.x = fd[0]; fw4.y = fd[1]; fw4.z = fd[2]; }
            }
            aimFwd = fw4; haveFwd = true;
            float best4 = 1e9f;
            Vec3 eye4 = mp; cam_origin(mp, &eye4);
            for (int i = 0; i < nn; i++) {
                if (list[i].parent == g_meEid) continue;
                // Отбор по расстоянию - от ИГРОКА (подбирает-то он), а угол
                // отклонения - от КАМЕРЫ: смотрит игрок именно ею.
                float px4 = list[i].pos.x - mp.x;
                float py4 = list[i].pos.y - mp.y;
                float pz4 = list[i].pos.z - mp.z;
                float dp4 = (float)sqrt(px4*px4 + py4*py4 + pz4*pz4);
                if (dp4 < 0.3f || dp4 > 25.0f) continue;
                float dx = list[i].pos.x - eye4.x;
                float dy = list[i].pos.y - eye4.y;
                float dz = list[i].pos.z - eye4.z;
                float d4 = (float)sqrt(dx*dx + dy*dy + dz*dz);
                if (d4 < 0.05f) continue;
                float along = dx*fw4.x + dy*fw4.y + dz*fw4.z;
                if (along <= 0.0f) continue;
                float o4 = d4*d4 - along*along; if (o4 < 0.0f) o4 = 0.0f;
                float ofs = (float)sqrt(o4);
                // Считаем УГОЛ, а не отклонение в метрах. Метровый промах в
                // полутора метрах - это полсотни градусов вбок, но прежняя
                // мерка считала его хорошим попаданием, и прицел цеплял
                // огромные ворота, у которых начало координат под ногами.
                float sinang = ofs / d4;
                if (sinang > 0.25f) continue;            // примерно 14 градусов
                float sc = sinang + d4 * 0.002f;         // при равном угле - ближний
                if (sc < best4) { best4 = sc; aimIdx = i; }
            }
        }
    }

    // КАМЕРА НАЙДЕНА И ПРОВЕРЕНА (31.08.2026): метки садятся на предметы
    // точно. Долгая неудача до этого держалась на трёх ошибках подряд: оси
    // брались строками вместо столбцов, началом отсчёта были ноги игрока
    // вместо глаза камеры, а найденная позиция глаза оказалась абсолютной
    // мировой при том, что весь остальной мод считает в локальном кадре.
    //
    // Запасной вариант остаётся: если камеру ещё не искали (Numpad 7 в
    // dev-сборке) или матрица не читается, берём ближайший доступный предмет -
    // на всех замерах нужный оказывался первым по расстоянию.
    if (aimIdx < 0) {
        for (int i = 0; i < nn; i++) {
            if (list[i].parent == g_meEid) continue;
            Action a3; if (skip_reason(list[i], &a3)) continue;
            aimIdx = i; break;
        }
    }

    InterlockedExchange(&g_aimIdxEid, aimIdx >= 0 ? (LONG)list[aimIdx].eid : 0);

    // Игра только что поймала жука сама - разбираем его, пока он ещё в списке.
    // Ищем несколько тактов подряд: сущность исчезает не мгновенно, но и не
    // ждёт нас вечно; через пять секунд сдаёмся, чтобы не искать вечно.
    if (dumpEid) {
        static DWORD dumpSince = 0;
        if (!dumpSince) dumpSince = now;
        for (int i = 0; i < nn; i++) {
            if (list[i].eid != (uint32_t)dumpEid) continue;
            fill_details(list[i]);
            Action a; const char *why = skip_reason(list[i], &a);
            L("[пойманное] eid=%08X d=%.1f м сп=%03X к2=0x%02X кат=0x%02X тип=%02X труп=%d "
              "замок=%d узел=%d предмет=%d сбор=%d ai=%d | мод сказал бы: %s",
              list[i].eid, list[i].d, list[i].src, list[i].cat2, list[i].cat, list[i].type,
              list[i].dead, list[i].locked, list[i].inter, list[i].item, list[i].gather,
              list[i].ai, why ? why : act_name(a));
            deep_dump("ПОЙМАННОЕ", list[i].ent);
            InterlockedExchange(&g_dumpEid, 0);
            dumpSince = 0;
            break;
        }
        if (dumpSince && (now - dumpSince) > 5000) {
            L("[пойманное] eid=%08X из списка уже пропал, разобрать не успел", (uint32_t)dumpEid);
            InterlockedExchange(&g_dumpEid, 0);
            dumpSince = 0;
        }
    }

    QueryPerformanceCounter(&ph2);
    // Плашка. Раньше она печатала сырые поля ближайших трёх объектов - по ним
    // видно разве что автору. Теперь это сводка: жив ли мод, что он видит,
    // что возьмёт следующим и почему пропускает остальное.
    if (hudTick || verbose) {
        int can = 0, byAct[4] = {0,0,0,0};
        int nOwned = 0, nFar = 0, nOff = 0, nNoNode = 0, nOther = 0;
        for (int i = 0; i < nn; i++) {
            Action a; fill_details(list[i]);
            const char *why = skip_reason(list[i], &a);
            if (!why) { can++; byAct[a]++; continue; }
            if (strstr(why, "чужое"))          nOwned++;
            else if (strstr(why, "далеко"))    nFar++;
            else if (strstr(why, "выключен"))  nOff++;
            else if (strstr(why, "узла"))      nNoNode++;
            else                               nOther++;
        }
        int hooks = (g_areaHook.active ? 1 : 0) + (g_enqHook.active ? 1 : 0)
                  + (g_ownHook.active ? 1 : 0);
        char t[3000]; int n = 0;

        // Строка состояния. Если перехваты не встали, мод бесполезен, и это
        // должно быть первым, что видно - раньше он в таком виде бодро писал
        // "готов", и полчаса ушло на выяснение, почему ничего не происходит.
        // Готовая сборка: на экране только короткая табличка, как в оригинале.
        // Показалась - через две секунды окно само гаснет. Большая сводка со
        // списком целей и причинами отказа - панель разработчика, Debug=1.
        if (!g_debug) {
            // Мод, у которого не встали перехваты, молчит и выглядит как
            // "ничего не происходит". В готовой сборке это самая частая
            // жалоба, поэтому один раз, через полминуты после загрузки,
            // говорим об этом прямо. Полминуты - чтобы не пугать тем, что
            // ещё не успело подняться.
            static DWORD firstTick = 0;
            if (!firstTick) firstTick = now;
            if (hooks < 3 && now - firstTick > 30000) {
                hud_set("%s", T("CDLoot: hooks failed - see CDLoot.log",
                                "CDLoot: перехваты не встали - смотрите CDLoot.log"));
                InterlockedExchange(&g_hudOn, 1);
            } else if (g_notice[0] && now < g_noticeUntil) {
                hud_set("%s", g_notice);
            } else {
                g_notice[0] = 0;
                InterlockedExchange(&g_hudOn, 0);
            }
            return;
        }

        // Уведомление важнее сводки: пару секунд после нажатия показываем его.
        if (g_notice[0] && now < g_noticeUntil)
            n += _snprintf(t + n, sizeof t - n - 1, "CDLoot  %s", g_notice);
        else if (settling)
            n += _snprintf(t + n, sizeof t - n - 1,
                "CDLoot  ПЕРЕХОД - пауза, мир ещё не устоялся");
        else if (hooks < 3)
            n += _snprintf(t + n, sizeof t - n - 1,
                "CDLoot  ПЕРЕХВАТЫ НЕ ВСТАЛИ (%d из 3) - мод не работает", hooks);
        else
            n += _snprintf(t + n, sizeof t - n - 1,
                "CDLoot  автосбор %s  %d/с   чужое: %s%s",
                InterlockedCompareExchange(&g_autoOn, 0, 0) ? "ВКЛ" : "выкл",
                g_cfg.maxLootsPerSec,
                g_cfg.lootOwned ? "БЕРЁМ" : "обходим",
                g_ownCtx ? "" : " (оракул ещё молчит)");

        n += _snprintf(t + n, sizeof t - n - 1,
            "\nвы: %.0f %.0f %.0f   вижу %d, в %.0f м %d, брать %d",
            mp.x, mp.y, mp.z, total, g_cfg.scanRange, nn, can);
        {   // Глаз камеры: видно сразу, взята позиция или мы всё ещё считаем
            // от ног игрока (тогда метки уезжают по вертикали).
            Vec3 eye0;
            if (cam_origin(mp, &eye0)) {
                float ex0 = eye0.x - mp.x, ey0 = eye0.y - mp.y, ez0 = eye0.z - mp.z;
                n += _snprintf(t + n, sizeof t - n - 1,
                    "   глаз: %.0f %.0f %.0f (%.1f м от вас)", eye0.x, eye0.y, eye0.z,
                    (double)sqrt(ex0*ex0 + ey0*ey0 + ez0*ez0));
            } else n += _snprintf(t + n, sizeof t - n - 1, "   глаз: нет");
        }
        if (can)
            n += _snprintf(t + n, sizeof t - n - 1,
                "\n   сбор %d | вещи %d | ловля %d | туши %d",
                byAct[ACT_GATHER], byAct[ACT_TAKE], byAct[ACT_CATCH], byAct[ACT_SEARCH]);
        if (nOwned || nFar || nOff || nNoNode || nOther) {
            n += _snprintf(t + n, sizeof t - n - 1, "\n   мимо:");
            if (nOwned)  n += _snprintf(t + n, sizeof t - n - 1, " чужое %d", nOwned);
            if (nFar)    n += _snprintf(t + n, sizeof t - n - 1, " | далеко %d", nFar);
            if (nOff)    n += _snprintf(t + n, sizeof t - n - 1, " | выключено %d", nOff);
            if (nNoNode) n += _snprintf(t + n, sizeof t - n - 1, " | без узла %d", nNoNode);
            if (nOther)  n += _snprintf(t + n, sizeof t - n - 1, " | прочее %d", nOther);
        }

        // Отсеянное - поимённо. Без этого "чужое 102" в доме выглядит как
        // пропажа предметов, хотя мод их прекрасно видит и сознательно
        // обходит. Показываем три ближайших с причиной - брать ничего не
        // надо, чтобы понять, что происходит.
        if (g_debug && (nOwned || nOff || nNoNode || nOther)) {
            int shownSkip = 0;
            for (int i = 0; i < nn && shownSkip < 7; i++) {
                Action a6; const char *w6 = skip_reason(list[i], &a6);
                if (!w6) continue;
                // "без узла" больше НЕ прячем: короб для хранения в поместье
                // не показывался ни одной строкой, и первым подозреваемым была
                // как раз эта причина. Прятать причину, которую ищешь, - способ
                // не найти её никогда.
                if (strstr(w6, "далеко")) continue;
                if (list[i].d > 4.0f) continue;      // только то, что под носом
                n += _snprintf(t + n, sizeof t - n - 1, "\n   мимо %4.1f м %-22s %s",
                    list[i].d,
                    list[i].name ? list[i].name
                    : (list[i].nodeName ? list[i].nodeName
                       : cat_name(list[i].cat2, list[i].dead, list[i].type,
                                  list[i].item, list[i].gather)),
                    w6);
                shownSkip++;
            }
        }

        // На что смотришь - отдельной строкой, это самое частое, что нужно.
        if (aimIdx >= 0) {
            Cand &a2 = list[aimIdx];
            Action ac; const char *why2 = skip_reason(a2, &ac);
            n += _snprintf(t + n, sizeof t - n - 1,
                "\nсмотрю на: %04X %.1f м  %s  -> %s", a2.eid & 0xFFFF, a2.d,
                a2.name ? a2.name : (a2.nodeName ? a2.nodeName
                        : cat_name(a2.cat2, a2.dead, a2.type, a2.item, a2.gather)),
                why2 ? why2 : act_name(ac));
        }

        // Проекция объектов на экран. Матрица камеры даёт три оси: вперёд
        // (минус первая строка, проверено), и две другие - вправо и вверх.
        // Какая из них какая, знака не знаем, поэтому берём как есть: если
        // метки окажутся зеркальными или перевёрнутыми, это видно с одного
        // взгляда и лечится сменой знака.
        if (haveFwd && g_camN && InterlockedCompareExchange(&g_marksOn, 0, 0)) {
            const float *m5 = 0;
            for (int i = 0; i < g_camN; i++)
                if (readable(g_cam[i], 0x40) && ortho3((const float *)g_cam[i])) {
                    m5 = (const float *)g_cam[i]; break;
                }
            EnterCriticalSection(&g_hudCs);
            g_markN = 0;
            if (m5 && g_scrW > 0) {
                // Оси экрана были перепутаны местами: живая проверка показала,
                // что поворот камеры вбок двигал метки по вертикали, а взгляд
                // вверх-вниз - по горизонтали. Значит горизонталь лежит в
                // ТРЕТЬЕЙ строке (и с обратным знаком: влево должно двигать
                // метки вправо), а вертикаль - во второй.
                // Формула взята из кода ESP, а не подобрана:
                //     x' = dx*m[0] + dy*m[4] + dz*m[8]      (первый СТОЛБЕЦ)
                //     y' = dx*m[1] + dy*m[5] + dz*m[9]      (второй столбец)
                //     w  = dx*m[2] + dy*m[6] + dz*m[10]     (третий столбец)
                //     x' = x' * масштабX / w,  y' = y' * масштабY / w
                // Оси лежат в СТОЛБЦАХ - вот главная ошибка, из-за которой
                // метки не садились: я всё время брал строки. Масштабы ESP
                // держит по +0x4C и +0x50 своего снимка, а туда он копирует
                // поля tf+0x08 и tf+0x1C игры - это и есть p00/p11.
                Vec3 rt; rt.x = m5[0]; rt.y = m5[4]; rt.z = m5[8];
                Vec3 up; up.x = m5[1]; up.y = m5[5]; up.z = m5[9];
                Vec3 fz3; fz3.x = m5[2]; fz3.y = m5[6]; fz3.z = m5[10];
                float p00 = 1.0f, p11 = 1.0f;
                if (g_camTf && readable(g_camTf, 0x20)) {
                    p00 = *(float *)(g_camTf + 0x08);
                    p11 = *(float *)(g_camTf + 0x1C);
                    if (p00 < 0.1f || p00 > 20.0f) p00 = 1.0f;
                    if (p11 < 0.1f || p11 > 20.0f) p11 = 1.0f;
                }
                // Начало отсчёта - ГЛАЗ КАМЕРЫ (трансформ+0xC8), а не игрок.
                // Именно это вычитает ESP перед поворотом.
                Vec3 eye = mp; cam_origin(mp, &eye);
                // Два прохода. Сначала то, что мод возьмёт, потом - в режиме
                // разработчика - отсеянное поблизости, с точкой вместо плюса.
                // Иначе получается ложная картина: на сундуке метки нет, и
                // выглядит это как "мод его не видит", хотя он видит и
                // сознательно обходит как чужое имущество.
                for (int pass = 0; pass < 2; pass++) {
                if (pass == 1 && !g_debug) break;
                for (int i = 0; i < nn && g_markN < 24; i++) {
                    if (list[i].parent == g_meEid) continue;
                    Action a5; const char *w5 = skip_reason(list[i], &a5);
                    if (pass == 0 && w5) continue;
                    if (pass == 1) {
                        if (!w5) continue;
                        if (list[i].d > 6.0f) continue;
                        if (strstr(w5, "наше")) continue;   // своё снаряжение - шум
                    }
                    float vx = list[i].pos.x - eye.x;
                    float vy = list[i].pos.y - eye.y;
                    float vz = list[i].pos.z - eye.z;
                    float fz = vx*fz3.x + vy*fz3.y + vz*fz3.z;
                    if (fz < 0.3f) continue;                 // позади
                    float sx = (vx*rt.x + vy*rt.y + vz*rt.z) * p00 / fz;
                    float sy = (vx*up.x + vy*up.y + vz*up.z) * p11 / fz;
                    int px = (int)((sx * 0.5f + 0.5f) * g_scrW);
                    int py = (int)((0.5f - sy * 0.5f) * g_scrH);
                    if (px < -100 || py < -100 || px > g_scrW + 100 || py > g_scrH + 100) continue;
                    g_marks[g_markN].x = px; g_marks[g_markN].y = py;
                    g_marks[g_markN].aim = (i == aimIdx);
                    if (pass == 0)
                        _snprintf(g_marks[g_markN].text, 39, "%04X %.1fм",
                                  list[i].eid & 0xFFFF, list[i].d);
                    else
                        // Отсеянное подписываем именем: на сундуке должно
                        // читаться "Chest", а не голый номер.
                        _snprintf(g_marks[g_markN].text, 39, "%04X %.16s",
                                  list[i].eid & 0xFFFF,
                                  list[i].name ? list[i].name
                                  : (list[i].nodeName ? list[i].nodeName : w5));
                    g_markN++;
                }
                }
            }
            LeaveCriticalSection(&g_hudCs);
            InterlockedIncrement(&g_hudSeq);
        }

        // Что пойдёт следующим - по строке на ближайшие цели.
        int shown = 0;
        for (int i = 0; i < nn && shown < 10; i++) {
            Action a; const char *why = skip_reason(list[i], &a);
            if (why) continue;
            // Угол до объекта от линии взгляда: у того, на что смотришь,
            // он должен быть наименьшим. Так сразу видно, врёт ли
            // направление, взятое из матрицы камеры.
            char ang[16] = " -- ";
            if (haveFwd) {
                Vec3 eye2 = mp; cam_origin(mp, &eye2);
                float ax2 = list[i].pos.x - eye2.x, ay2 = list[i].pos.y - eye2.y;
                float az2 = list[i].pos.z - eye2.z;
                float dl2 = (float)sqrt(ax2*ax2 + ay2*ay2 + az2*az2);
                if (dl2 > 0.01f) {
                    float c2 = (ax2*aimFwd.x + ay2*aimFwd.y + az2*aimFwd.z) / dl2;
                    if (c2 > 1.0f) c2 = 1.0f;
                    if (c2 < -1.0f) c2 = -1.0f;
                    _snprintf(ang, sizeof ang - 1, "%3.0f", acos(c2) * 57.2958);
                }
            }
            n += _snprintf(t + n, sizeof t - n - 1, "\n%s%4.1f м %s гр %04X %-16s %s",
                shown ? "       " : "далее  ", list[i].d,
                ang, list[i].eid & 0xFFFF,
                list[i].name ? list[i].name
                : (list[i].nodeName ? list[i].nodeName
                   : cat_name(list[i].cat2, list[i].dead, list[i].type,
                              list[i].item, list[i].gather)),
                act_name(a));
            shown++;
        }
        if (!shown && nn) {
            // Брать нечего - объясняем на ближайшем, почему именно.
            Action a; const char *why = skip_reason(list[0], &a);
            n += _snprintf(t + n, sizeof t - n - 1, "\nближайшее %4.1f м h%+.1f %04X %s -> %s",
                list[0].d, list[0].pos.y - mp.y, list[0].eid & 0xFFFF, cat_name(list[0].cat2, list[0].dead, list[0].type,
                                    list[0].item, list[0].gather),
                why ? why : "?");
        }

        // Незаряженные узлы рядом. Их не видно в "далее", потому что взять их
        // нельзя, и со стороны кажется, будто мод жилу не замечает. А он
        // замечает и ждёт, пока игра её включит - так и напишем.
        {
            int waitN = 0; char wl[120]; int wn = 0;
            for (int i = 0; i < nn && waitN < 4; i++) {
                if (list[i].item || list[i].gather) continue;
                if (!list[i].inter || list[i].type != TYPE_PLANT) continue;
                if (list[i].parent == g_meEid) continue;
                if (list[i].d > g_cfg.range[ACT_GATHER]) continue;
                wn += _snprintf(wl + wn, sizeof wl - wn - 1, "%s%.1f", waitN ? " / " : "", list[i].d);
                waitN++;
            }
            if (waitN) n += _snprintf(t + n, sizeof t - n - 1,
                "\nждут разрешения игры: %d  (%s м)", waitN, wl);
        }

        LONG sg = InterlockedCompareExchange(&g_sess[ACT_GATHER], 0, 0);
        LONG st = InterlockedCompareExchange(&g_sess[ACT_TAKE], 0, 0);
        LONG sc = InterlockedCompareExchange(&g_sess[ACT_CATCH], 0, 0);
        LONG ss = InterlockedCompareExchange(&g_sess[ACT_SEARCH], 0, 0);
        if (sg || st || sc || ss)
            n += _snprintf(t + n, sizeof t - n - 1,
                "\nвзято: сбор %ld | вещи %ld | ловля %ld | туши %ld", sg, st, sc, ss);

        // Отладка: то, чего в лог придётся лезть, а на экране видно сразу.
        // В готовой сборке этот блок не нужен - только при Debug=1.
        if (!g_debug) { t[n < 0 ? 0 : n] = 0; hud_set("%s", t); return; }
        n += _snprintf(t + n, sizeof t - n - 1,
            "\nотл. игрок %08X route %08X  перехваты %d/3  оракул %s",
            playerEid,
            (uint32_t)InterlockedCompareExchange(&g_route, 0, 0),
            hooks, g_ownCtx ? "да" : "НЕТ");
        if (g_lastEvt[0])
            n += _snprintf(t + n, sizeof t - n - 1, "\n     послал: %s, %.1f с назад",
                g_lastEvt, (double)(now - g_lastEvtAt) / 1000.0);
        if (g_aimText[0])
            n += _snprintf(t + n, sizeof t - n - 1, "\n     цель игры: %s", g_aimText);
        if (g_lastTrans[0])
            n += _snprintf(t + n, sizeof t - n - 1, "\n     переход: %s, %.0f с назад",
                g_lastTrans, (double)(now - g_lastTransAt) / 1000.0);
        t[n < 0 ? 0 : n] = 0;
        hud_set("%s", t);
    }

    QueryPerformanceCounter(&ph3);
    if (g_qpcFreq > 0) {
        double m1 = (double)(ph1.QuadPart - ph0.QuadPart) * 1000.0 / g_qpcFreq;
        double m2 = (double)(ph2.QuadPart - ph1.QuadPart) * 1000.0 / g_qpcFreq;
        double m3 = (double)(ph3.QuadPart - ph2.QuadPart) * 1000.0 / g_qpcFreq;
        static LONG toldFaults = 0;
        LONG f = InterlockedCompareExchange(&g_faults, 0, 0);
        if (f - toldFaults > 200) {
            L("[scan] промахов чтения: %ld (это норма, если немного: массив живой)", f);
            toldFaults = f;
        }
        if (m1 + m2 + m3 >= 8.0)
            L("[время] фазы: поиск игрока %.1f | список %.1f | подробности+плашка %.1f мс (объектов %d)",
              m1, m2, m3, nn);
    }

    for (int i = 0; verbose && i < nn && i < 20; i++) {
        Cand &k = list[i];
        fill_details(k);
        Action act; const char *why = skip_reason(k, &act);
        L("  %5.1f м (h%+.1f)  eid=%08X сп=%03X кат=0x%02X тип=%02X труп=%u замок=%u узел=%u предмет=%u сбор=%u"
          " род=%08X | к2=0x%02X наше@%03X 2C8=%08X 2CC=%08X 2D0=%08X 2D4=%08X | %s -> %s",
          k.d, k.pos.y - mp.y, k.eid, k.src, k.cat, k.type, k.dead, k.locked, k.inter ? 1 : 0,
          k.item ? 1 : 0, k.gather ? 1 : 0, k.parent, k.cat2, k.mine,
          k.catWin[0], k.catWin[1], k.catWin[2], k.catWin[3],
          k.name ? k.name : (k.nodeName ? k.nodeName
                 : cat_name(k.cat2, k.dead, k.type, k.item, k.gather)),
          why ? why : act_name(act));
    }

    // Разведка инвентаря. Хотим сверять номер экземпляра вещи из мира с тем,
    // что лежит у игрока: совпал - значит это наше, чем бы оно ни казалось.
    // Путь взят из разбора Trinity: контейнер - сам персонаж, держатель лежит
    // по *(*(актор+0x68)+0xB8). Пока просто выкладываем начало держателя,
    // чтобы найти в нём массив корзин.
    if (verbose && g_meEnt) {
        void *msub = deref(g_meEnt, ENT_SUBOBJ);
        void *holder = msub ? deref(msub, 0xB8) : 0;
        L("  [инвентарь] актор=%p sub=%p держатель=%p", g_meEnt, msub, holder);
        if (holder && readable(holder, 0x80)) {
            char hx[400];
            for (int off = 0; off < 0x80; off += 0x40) {
                hexs((unsigned char *)holder + off, 0x40, hx, sizeof hx);
                L("  [инвентарь] +0x%02X: %s", off, hx);
            }
        }
    }

    // Трансформ целиком - у игрока и у ближайшего объекта. Ищем в нём ссылку
    // на родителя и мировую матрицу: позиция по +0xB4 оказалась НЕ мировой, а
    // в системе координат родителя. У травы родитель - мир, и всё сходится, а
    // руда на скале - дочерний объект скалы, и её метры отсчитываются от
    // скалы. Когда игрок цепляется за руду, его собственные координаты тоже
    // становятся локальными (видно как "1 2 -0" вместо "-767 562 -416").
    // Значит расстояния мод считает от разных начал отсчёта.
    if (verbose) {
        char hx[400];
        void *msub = deref(g_meEnt, ENT_SUBOBJ);
        void *mtf  = deref(msub, SUB_TRANSFORM);
        if (mtf && readable(mtf, 0x140)) {
            for (int off = 0x80; off < 0x140; off += 0x40) {
                hexs((unsigned char *)mtf + off, 0x40, hx, sizeof hx);
                L("  [трансформ игрока] +0x%02X: %s", off, hx);
            }
        }
        for (int i = 0; i < nn && i < 1; i++) {
            void *osub = deref(list[i].ent, ENT_SUBOBJ);
            void *otf  = deref(osub, SUB_TRANSFORM);
            if (!otf || !readable(otf, 0x140)) continue;
            L("  [трансформ %08X] пос=(%.1f %.1f %.1f)", list[i].eid,
              list[i].pos.x, list[i].pos.y, list[i].pos.z);
            for (int off = 0x80; off < 0x140; off += 0x40) {
                hexs((unsigned char *)otf + off, 0x40, hx, sizeof hx);
                L("  [трансформ объекта] +0x%02X: %s", off, hx);
            }
        }
    }

    // Поиск ИМЕНИ объекта. В .asi чужого мода зашиты строки вида
    // "Item_gimmick_abyss_quest_kuku_seal_01" - значит у гиммика есть
    // читаемое имя, и в нём прямым текстом стоит "quest" и "useartifact".
    // По имени можно разделить всё сразу: квестовое, артефакты, сундуки,
    // мебель, руду и траву - и заодно подписать предметы по-человечески.
    // Ищем указатели на ASCII-строки в самой сущности и в её узле.
    if (verbose) {
        int shownName = 0;
        for (int i = 0; i < nn && shownName < 12; i++) {
            if (list[i].parent == g_meEid) continue;
            void *sub2 = deref(list[i].ent, ENT_SUBOBJ);
            void *inter2 = comp_by_class(sub2, CLS_GIMMICK);
            struct { const char *tag; unsigned char *base; int len; } where[3] = {
                { "сущность", list[i].ent, 0x200 },
                { "узел",     (unsigned char *)inter2, 0x400 },
                { "sub",      (unsigned char *)sub2, 0x100 },
            };
            for (int w = 0; w < 3; w++) {
                if (!where[w].base || !readable(where[w].base, where[w].len)) continue;
                for (int off = 0; off < where[w].len && shownName < 12; off += 8) {
                    char *p1 = *(char **)(where[w].base + off);
                    if (!p1 || !readable(p1, 8)) continue;
                    // Два уровня: строка может лежать прямо по указателю, а
                    // может - объектом, у которого первое поле ведёт на текст.
                    for (int lvl = 0; lvl < 2; lvl++) {
                        char *sp = lvl ? *(char **)p1 : p1;
                        if (lvl && (!sp || !readable(sp, 8))) break;
                        int n2 = 0;
                        while (n2 < 63 && readable(sp + n2, 1) &&
                               sp[n2] >= 0x20 && sp[n2] < 0x7F) n2++;
                        if (n2 < 6 || sp[n2] != 0) continue;
                        if (!strchr(sp, '_') && !strchr(sp, '/')) continue;
                        L("  [имя] eid=%08X %s+0x%03X%s -> \"%s\"",
                          list[i].eid, where[w].tag, off, lvl ? " (через объект)" : "", sp);
                        shownName++;
                        break;
                    }
                }
            }
        }
    }

    // Данные предмета и сбора с узла - в них должен лежать номер типа
    // предмета. Он нужен, чтобы вытащить название из таблицы игры iteminfo:
    // объекты в мире безымянные, а имя есть только у типа. Печатаем по паре
    // образцов на обзор, больше и не надо.
    if (verbose) {
        int shownItem = 0;
        for (int i = 0; i < nn && shownItem < 10; i++) {
            void *sub = deref(list[i].ent, ENT_SUBOBJ);
            void *inter = comp_by_class(sub, CLS_GIMMICK);
            if (!inter) continue;
            void *data = deref(inter, list[i].gather ? INTER_GATHER : INTER_ITEMDATA);
            if (!data || !readable(data, 0x40)) continue;
            char hx[200];
            hexs((unsigned char *)data, 0x20, hx, sizeof hx);
            L("  [данные] eid=%08X d=%.1f %s +0x%02X: %s", list[i].eid, list[i].d,
              list[i].gather ? "сбор" : "предмет",
              list[i].gather ? INTER_GATHER : INTER_ITEMDATA, hx);
            shownItem++;
        }
    }

    // По одному подробному разбору на каждый спорный вид: у растения узел есть,
    // у предмета его нет - надо увидеть, чем эти объекты отличаются в памяти.
    // F2 - подробно об одном БЛИЖАЙШЕМ объекте. Прежний поиск наведённой цели
    // отсюда убран: он ни разу ничего не нашёл, а вот навести на артефакт или
    // рецепт и получить именно его имя - то, что нужно для разделения лута.
    if (InterlockedExchange(&g_nameRequested, 0) && nn) {
        // Три ближайших, а не один: стоя в комнате хочется за одно нажатие
        // увидеть и вещь, и шкаф, и сундук, а не тыкать по очереди.
        // ПЕРВЫМ - тот, на который смотришь. Три ближайших не помогают, когда
        // разобрать надо именно вон ту коробку: она оказывается четвёртой, и
        // в дамп не попадает вовсе. Прицел у нас теперь честный, так что
        // спрашивать надо его, а расстояние оставить запасным вариантом.
        int picks[4], pn = 0;
        uint32_t aimE = (uint32_t)InterlockedCompareExchange(&g_aimIdxEid, 0, 0);
        if (aimE) {
            for (int i = 0; i < nn; i++)
                if (list[i].eid == aimE) { picks[pn++] = i; break; }
        }
        if (!pn) L("[что это] прицел ни на чём - показываю ближайшие");
        for (int i = 0; i < nn && pn < 4; i++) {
            if (list[i].parent == g_meEid) continue;   // не своё снаряжение
            bool dup = false;
            for (int p = 0; p < pn; p++) if (picks[p] == i) dup = true;
            if (dup) continue;
            picks[pn++] = i;
        }
        if (!pn) L("[что это] рядом только своё снаряжение");
        for (int pi = 0; pi < pn; pi++) {
            int pick = picks[pi];
            Cand &k = list[pick];
            fill_details(k);
            Action a; const char *why = skip_reason(k, &a);
            L("=== ЧТО ЭТО%s: eid=%08X, %.1f м, h%+.1f",
              (aimE && k.eid == aimE) ? " (ПРИЦЕЛ)" : "", k.eid, k.d, k.pos.y - mp.y);
            L("    тип=%02X к2=0x%02X узел=%d предмет=%d сбор=%d труп=%d замок=%d",
              k.type, k.cat2, k.inter ? 1 : 0, k.item ? 1 : 0, k.gather ? 1 : 0,
              k.dead, k.locked);
            L("    мод решил: %s", why ? why : act_name(a));
            const char *tn = item_type_name(k.tid);
            L("    тип предмета %u (0x%04X): %s", k.tid, k.tid,
              tn ? tn : "имя не читается");
            L("    имя узла: %s", k.nodeName ? k.nodeName : "нет");
            if (k.tid && !k.name) {
                L("    имени нет: iteminfo %u записей, gimmickinfo %u, номер %u"
                  " - спрашиваю все %d таблиц игры:",
                  table_count(g_itemTable), table_count(g_gimmickTable),
                  k.tid, g_tabN);
                int hits = 0;
                for (int t = 0; t < g_tabN; t++) {
                    uint32_t cnt = table_count(g_tabs[t].glob);
                    if (!cnt || k.tid >= cnt) continue;
                    const char *nm2 = table_name(g_tabs[t].glob, k.tid);
                    if (!nm2) continue;
                    L("      %s (%u записей) -> %s", g_tabs[t].name, cnt, nm2);
                    if (++hits >= 8) break;
                }
                if (!hits) L("      ни одна таблица этот номер не знает");
            }
            void *sub2 = deref(k.ent, ENT_SUBOBJ);
            void *inter2 = comp_by_class(sub2, CLS_GIMMICK);
            void *comp2 = comp_by_class(sub2, CLS_STATUS);
            struct { const char *tag; unsigned char *base; int len; } where[4] = {
                { "сущность", k.ent, 0x400 },
                { "sub", (unsigned char *)sub2, 0x200 },
                { "узел", (unsigned char *)inter2, 0x600 },
                { "comp", (unsigned char *)comp2, 0x400 },
            };
            int found = 0;
            for (int w = 0; w < 4 && found < 24; w++) {
                if (!where[w].base || !readable(where[w].base, where[w].len)) continue;
                for (int off = 0; off < where[w].len && found < 24; off += 8) {
                    char *p1 = *(char **)(where[w].base + off);
                    if (!p1 || !readable(p1, 8)) continue;
                    for (int lvl = 0; lvl < 2; lvl++) {
                        char *sp = lvl ? *(char **)p1 : p1;
                        if (lvl && (!sp || !readable(sp, 8))) break;
                        int n2 = 0;
                        while (n2 < 95 && readable(sp + n2, 1) &&
                               sp[n2] >= 0x20 && sp[n2] < 0x7F) n2++;
                        if (n2 < 5 || sp[n2] != 0) continue;
                        if (!strchr(sp, '_') && !strchr(sp, '/')) continue;
                        L("    строка %s+0x%03X%s: \"%s\"", where[w].tag, off,
                          lvl ? " ->" : "  ", sp);
                        found++;
                        break;
                    }
                }
            }
            if (!found) L("    читаемых строк не нашлось");
            void *data = deref(inter2, k.gather ? INTER_GATHER : INTER_ITEMDATA);
            if (data && readable(data, 0x20)) {
                char hx[200];
                hexs((unsigned char *)data, 0x20, hx, sizeof hx);
                L("    данные узла: %s", hx);
            }
        }
    }

    if (verbose) {
        for (int i = 0; i < nn && i < 6; i++) cat_hunt(list[i].ent);
        int shown6 = 0, shown7 = 0;
        for (int i = 0; i < nn && (!shown6 || !shown7); i++) {
            if (list[i].type == 0x06 && !shown6) { deep_dump("ПРЕДМЕТ", list[i].ent); shown6 = 1; }
            if (list[i].type == TYPE_PLANT && !shown7) { deep_dump("РАСТЕНИЕ", list[i].ent); shown7 = 1; }
        }
    }

    // ---- взятие ближайшего (F8) или проверка без отправки (F6) ----
    LONG burst = InterlockedExchange(&g_burstRequested, 0);
    LONG take  = InterlockedExchange(&g_takeRequested, 0) | burst;
    LONG dry   = InterlockedExchange(&g_dryRequested, 0);
    LONG force = InterlockedExchange(&g_forceRequested, 0);
    LONG aimTake = InterlockedExchange(&g_aimTakeReq, 0);
    if (!take && !dry && !force && !aimTake && !autoTick) return;
    if (settling) {
        if (take || dry || force || aimTake)
            L("[переход] пауза после перемещения, действие пропущено");
        return;
    }

    // route берём тот, что игра сама ставит в свои события. Поле +0x58 у
    // сущности игрока в этой сборке пустое, так что читать его бесполезно.
    uint32_t route = playerRoute;
    if (InterlockedCompareExchange(&g_routeKnown, 0, 0)) {
        route = (uint32_t)InterlockedCompareExchange(&g_route, 0, 0);
    } else {
        if (verbose) L("[цель] route ещё не подсмотрен у игры, шлю с %08X", route);
    }

    // F5 - взять ближайшее, что НЕ растение: удобно, когда трава стоит ближе
    // лута и обычный F8 всё время цепляет её. Проверки те же, что и везде:
    // обхода запрета тут нет и быть не должно.
    if (force) {
        for (int i = 0; i < nn; i++) {
            Action act;
            fill_details(list[i]);
            if (skip_reason(list[i], &act)) continue;
            if (act == ACT_GATHER) continue;
            L("[цель] не-растение: eid=%08X d=%.1f м - %s", list[i].eid, list[i].d, act_name(act));
            if (act == ACT_SEARCH) mark_searched(cand_key(list[i].eid, list[i].iid));
            // Отметки о взятии тут не было - объект можно было слать без конца.
            mark_done(cand_key(list[i].eid, list[i].iid), now);
            spot_mark(list[i].pos, list[i].tid, now);
            InterlockedIncrement(&g_sess[act]);
            _snprintf(g_lastEvt, sizeof g_lastEvt - 1, "%s %04X",
                      act_name(act), list[i].eid & 0xFFFF);
            g_lastEvtAt = now;
            send_action(act, list[i].eid, playerEid, route, 0x00, false);
            return;
        }
        L("[цель] рядом нет ничего, кроме растений");
        return;
    }

    // ВЗЯТЬ ТО, НА ЧТО СМОТРЮ. Отдельная клавиша, одно нажатие - один предмет.
    // Прицел считается лучом из камеры (см. cam_origin), eid публикуется в
    // g_aimIdxEid каждый такт.
    //
    // Чем это отличается от F8: ручное действие обходит ПОЛИТИКУ, но не
    // ЗАЩИТУ.
    //   обходится: "чужое" (это и есть кража с отдельной клавиши из ТЗ),
    //              "далеко" и "выключено" - это настройки АВТОСБОРА, а тут
    //              человек целится сам и видит, во что;
    //   остаётся:  квестовое, артефакты и рецепты, декорации, запертое,
    //              пустой двойник, пустой узел и собственное снаряжение.
    // Проверено живьём: так клавиша берёт и мебель, включая ту, которую игра
    // руками трогать не даёт. Оставлено сознательно - ручное прицельное
    // нажатие на то и ручное. Автосбор мебель по-прежнему не трогает без
    // LootFurniture.
    // Кража чужого поднимает розыск - об этом пишем прямо в лог и в плашку,
    // чтобы это не оказалось сюрпризом.
    if (aimTake) {
        uint32_t aimEid = (uint32_t)InterlockedCompareExchange(&g_aimIdxEid, 0, 0);
        if (!aimEid) { L("[прицел] ни на что не смотрю - брать нечего"); return; }
        for (int i = 0; i < nn; i++) {
            if (list[i].eid != aimEid) continue;
            fill_details(list[i]);
            Action act = ACT_TAKE;
            const char *why = skip_reason(list[i], &act);
            bool policy = why && (strstr(why, "чужое") || strstr(why, "далеко")
                                  || strstr(why, "выключен"));
            const char *nm = list[i].name ? list[i].name
                           : (list[i].nodeName ? list[i].nodeName : "без имени");
            if (why && !policy) {
                L("[прицел] %08X %s - НЕ беру: %s", aimEid, nm, why);
                _snprintf(g_lastEvt, sizeof g_lastEvt - 1, "нельзя: %s", why);
                g_lastEvtAt = now;
                return;
            }
            bool stolen = why && strstr(why, "чужое") != 0;
            L("[прицел] беру %08X %.1f м %s - %s%s", aimEid, list[i].d, nm,
              act_name(act), stolen ? "  (ЧУЖОЕ, будет розыск)" : "");
            if (act == ACT_SEARCH) mark_searched(cand_key(list[i].eid, list[i].iid));
            mark_done(cand_key(list[i].eid, list[i].iid), now);
            InterlockedIncrement(&g_sess[act]);
            _snprintf(g_lastEvt, sizeof g_lastEvt - 1, "%s%s %04X", stolen ? "кража " : "",
                      act_name(act), list[i].eid & 0xFFFF);
            g_lastEvtAt = now;
            send_action(act, list[i].eid, playerEid, route, 0x00, false);
            return;
        }
        L("[прицел] цель %08X уже пропала из списка", aimEid);
        return;
    }

    // Автосбор: за проход - один объект, ближайший из разрешённых, и не тот,
    // что уже брали в последние шесть секунд.
    if (autoTick && !take && !dry && !force) {
        // Сначала зарядка. Узел без данных брать нечем, но теперь мы умеем
        // включать его сами - той же функцией, которой это делает игра, когда
        // подойдёшь вплотную. Заряжаем по одному за такт и помним, что уже
        // трогали, иначе будем долбить один и тот же камень.
        // Кого зарядили ПРЯМО СЕЙЧАС. Игра кладёт в узел сначала пустую
        // заготовку, и настоящие данные появляются позже. Если стрелять по
        // такому узлу в том же такте, мод действует вслепую: в логе видно
        // "собрать растение ... тип=0" - он не знает, что берёт, и никакой
        // фильтр по типу или имени сработать не может. Ведро с водой уехало
        // именно так. Даём игре один такт на заполнение.
        uint32_t armedNow[32]; int armedNowN = 0;
        int armed2 = 0;
        if (g_cfg.autoArm && g_armFn) {
            for (int i = 0; i < nn; i++) {
                Cand &k = list[i];
                if (k.item || k.gather || !k.inter) continue;
                if (k.parent == g_meEid || k.twin || k.heap) continue;
                float armLim = g_cfg.armRange > 0.0f ? g_cfg.armRange
                                                     : g_cfg.range[ACT_GATHER];
                if (k.d > armLim) continue;
                // Ёмкость механизма. Заряжать её осмысленно: игра после этого
                // предлагает взаимодействие, и игрок берёт воду руками. А вот
                // лутать её нельзя никогда - объект должен остаться на месте,
                // иначе ломается сам механизм.
                if (!g_cfg.armContainers && is_container(k.eid)) continue;
                uint64_t key = cand_key(k.eid, k.iid);
                if (arm_recent(key, now) || already_searched(key)) continue;
                void *sub2 = deref(k.ent, ENT_SUBOBJ);
                void *g2 = comp_by_class(sub2, CLS_GIMMICK);
                if (!g2) continue;
                static __declspec(align(16)) unsigned char scratch[256];
                memset(scratch, 0, sizeof scratch);
                arm_mark(key, now);
                InterlockedExchange(&g_armLastEid, (LONG)k.eid);
                int ok = arm_call(g2, (uintptr_t)InterlockedCompareExchange(&g_armMode, 0, 0),
                                  scratch, (uintptr_t)playerEid);
                void *after = deref(g2, INTER_GATHER);
                L("[зарядка] авто: eid=%08X %.1f м -> %s", k.eid, k.d,
                  ok == 0 ? "исключение" :
                  ok == 2 ? "в очередь, проверим позже" :
                  (after ? "заряжено" : "без изменений"));
                // Узел, который после зарядки так и остался пустым, добычей
                // не является. Рудная жила отвечает данными сбора сразу же, а
                // сундук не отвечает ничем - ни закрытый, ни открытый руками:
                // он контейнер, а не предмет, и имени у него нет вовсе.
                // Помним такие навсегда, иначе мод будет ковырять каждый
                // сундук и шкаф в комнате по кругу.
                // Судим только по НАСТОЯЩЕМУ вызову. Отложенный (ok == 2)
                // ещё не выполнен, и его пустой результат ничего не значит.
                if (ok == 1 && !after) {
                    mark_searched(key);
                    L("[зарядка] eid=%08X не поддаётся зарядке - это не добыча,"
                      " больше не трогаю", k.eid);
                }
                fill_details_again(k);       // данные узла только что появились
                if (armedNowN < 32) armedNow[armedNowN++] = k.eid;
                armed2++;
                if (g_cfg.perTick && armed2 >= g_cfg.perTick) break;
            }
        }
        // За один обход берём до PerTick объектов: сцена уже просканирована, и
        // отправка события рядом с этим ничего не стоит. Поднимать частоту
        // обходов было бы куда дороже - обход идёт на игровом потоке.
        int taken = 0;
        int cap = g_cfg.perTick ? g_cfg.perTick : nn;
        // ПОРЯДОК ОБХОДА. Сначала туши, потом всё остальное. Раньше список шёл
        // строго по расстоянию, и в густом месте лимит за такт съедала трава,
        // а туша ждала следующего круга - хотя именно она пропадает первой,
        // когда игра прибирает мир за игроком.
        for (int pass = 0; pass < 2 && taken < cap; pass++)
        for (int i = 0; i < nn && taken < cap; i++) {
            Action act;
            fill_details(list[i]);
            if (skip_reason(list[i], &act)) continue;
            if (pass == 0 && act != ACT_SEARCH) continue;   // первый круг - туши
            if (pass == 1 && act == ACT_SEARCH) continue;   // второй - всё прочее
            // Только что заряженное пропускаем до следующего такта: пусть
            // игра допишет данные, иначе берём неизвестно что.
            bool justArmed = false;
            for (int a2 = 0; a2 < armedNowN; a2++)
                if (armedNow[a2] == list[i].eid) justArmed = true;
            if (justArmed) continue;
            // Сбору даём узлу устояться. Пока тип не прочитался, мы не знаем,
            // что перед нами: у ведра он появляется через доли секунды и
            // оказывается запрещённым, а мод успевал выстрелить раньше.
            // Предметов это не касается - у них данные есть сразу.
            if (act == ACT_GATHER && list[i].tid == 0 &&
                age_ms(list[i].eid, now) < 700) continue;
            uint64_t key = cand_key(list[i].eid, list[i].iid);
            if (recently_done(key, now)) continue;
            if (spot_recent(list[i].pos, list[i].tid, now)) continue;
            mark_done(key, now);
            spot_mark(list[i].pos, list[i].tid, now);
            // mark_done мог только что занести объект в чёрный список - тогда
            // отправлять уже нельзя. Раньше отказ печатался, а событие всё
            // равно уходило в том же такте.
            if (already_searched(key)) continue;
            dup_watch(now, list[i].eid, list[i].parent, list[i].tid, list[i].cat2,
                      (uint8_t)act, list[i].d, list[i].name);
            L("[авто] %s eid=%08X d=%.1f м тип=%u вид=%02X %s", act_name(act),
              list[i].eid, list[i].d, list[i].tid, list[i].gkind,
              list[i].name ? list[i].name
                           : (list[i].nodeName ? list[i].nodeName : "без имени"));
            if (act == ACT_SEARCH) mark_searched(cand_key(list[i].eid, list[i].iid));
            InterlockedIncrement(&g_sess[act]);
            _snprintf(g_lastEvt, sizeof g_lastEvt - 1, "%s %04X",
                      act_name(act), list[i].eid & 0xFFFF);
            g_lastEvtAt = now;
            send_action(act, list[i].eid, playerEid, route, 0x00, false);
            taken++;
        }
        return;
    }

    // Одно нажатие F8 берёт BurstPerKey объектов подряд, ближайших первыми.
    // Проверка без отправки (F6) всегда показывает ровно один - её смысл в
    // том, чтобы разглядеть событие, а не набрать вещей.
    // BurstPerKey = 0 означает "всё разом", как и всюду в этих настройках.
    int want = dry ? 1 : (g_cfg.burstPerKey ? g_cfg.burstPerKey : nn), got = 0;
    int gotAct[4] = {0,0,0,0};
    for (int i = 0; i < nn && got < want; i++) {
        Action act;
        fill_details(list[i]);
        const char *why = skip_reason(list[i], &act);
        if (why) continue;
        // Проверки "недавно брали" тут не было вовсе: пачка слала событие
        // объекту, который автосбор в этот же момент считал занятым.
        uint64_t bkey = cand_key(list[i].eid, list[i].iid);
        if (!dry && (recently_done(bkey, now) ||
                     spot_recent(list[i].pos, list[i].tid, now))) continue;
        if (!dry) dup_watch(now, list[i].eid, list[i].parent, list[i].tid,
                            list[i].cat2, (uint8_t)act, list[i].d, list[i].name);
        L("[цель] eid=%08X d=%.1f м - %s", list[i].eid, list[i].d, act_name(act));
        if (act == ACT_SEARCH && !dry) mark_searched(cand_key(list[i].eid, list[i].iid));
        if (!dry) InterlockedIncrement(&g_sess[act]);
        if (!dry) { _snprintf(g_lastEvt, sizeof g_lastEvt - 1, "%s %04X",
                              act_name(act), list[i].eid & 0xFFFF); g_lastEvtAt = now; }
        send_action(act, list[i].eid, playerEid, route, 0x00, dry != 0);
        if (!dry) { mark_done(bkey, now); spot_mark(list[i].pos, list[i].tid, now); }
        got++;
    }
    if (!got) L("[цель] рядом нечего брать");
    else if (want > 1) L("[цель] взято за нажатие: %d из %d", got, want);
}

// Перехват пролога: забираем STEAL байт с начала функции, кладём их в
// переходник, а на их место пишем переход к нам. Байты пролога должны быть
// свободны от rip-относительной адресации - для этих функций проверено.
// Обёртка с перехватом исключений. Всё чтение игровых структур идёт без
// похода в ядро, поэтому промах по указателю здесь - штатная ситуация:
// такт пропускается, игра продолжает работать.
// Исполнение отложенных решений - строго на игровом потоке.
static void drain_pending(void) {
    // Забираем ОБЕ очереди под замком и сразу освобождаем его: отправка зовёт
    // функции игры и может занять время, держать на ней замок незачем.
    PendArm arms[32]; int armN;
    PendAct acts[64]; int actN;
    pend_init();
    EnterCriticalSection(&g_pendCs);
    armN = g_pendArmN; if (armN > 32) armN = 32;
    memcpy(arms, g_pendArm, sizeof(PendArm) * armN);
    g_pendArmN = 0;
    actN = g_pendActN; if (actN > 64) actN = 64;
    memcpy(acts, g_pendAct, sizeof(PendAct) * actN);
    g_pendActN = 0;
    LeaveCriticalSection(&g_pendCs);

    for (int i = 0; i < armN; i++) {
        if (!arms[i].node || !readable(arms[i].node, 0x400)) continue;
        static __declspec(align(16)) unsigned char scratch[256];
        memset(scratch, 0, sizeof scratch);
        void *before = deref(arms[i].node, INTER_GATHER);
        bool ok = arm_call_now(arms[i].node, arms[i].mode, scratch, arms[i].player);
        void *after = deref(arms[i].node, INTER_GATHER);
        // Отложенная зарядка исполняется здесь, и только тут видно, сработала
        // ли она на самом деле. Без этой строки мы вслепую гадали, почему не
        // собирается руда.
        L("[зарядка] исполнено на игровом потоке: eid=%08X %s (было %p, стало %p)",
          arms[i].eid, !ok ? "ИСКЛЮЧЕНИЕ" : (after ? "заряжено" : "без изменений"),
          before, after);
    }
    for (int i = 0; i < actN; i++)
        send_action((Action)acts[i].act, acts[i].eid, acts[i].player,
                    acts[i].route, acts[i].mode, false);
}

extern "C" void __fastcall cdloot_on_area(void *ctx, void *a2, void *a3, void *a4) {
    InterlockedExchange(&g_gameTid, (LONG)GetCurrentThreadId());
    InterlockedExchangePointer((void * volatile *)&g_ctxLatest, ctx);
    __try {
        if (InterlockedCompareExchange(&g_offThread, 0, 0)) {
            self_test();          // зовёт функции игры - только отсюда
            drain_pending();
        } else {
            area_body(ctx, a2, a3, a4);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        static LONG told = 0;
        if (InterlockedExchange(&told, 1) == 0)
            L("[scan] чтение не удалось, такт пропущен (дальше молча)");
    }
}

// ---------------------------------------------------- оракул воровства ------
// own_check - это функция, которую игра зовёт в самих воротах кражи:
//     mov rcx,[[rbp+0x48]+0x68]; mov rcx,[rcx+0x120]
//     mov rdx,r13 ; mov r8,rdi ; lea r9,[rip+..] ; mov [rsp+0x20],7
//     call own_check ; test al,al ; jz +4 ; mov bl,2   <- «это кража»
// Пять аргументов, и какой из них цель - неизвестно. Поэтому не гадаем, а
// слушаем: пишем в лог первые N вызовов вместе с тем, что видно по каждому
// указателю. Если у аргумента по +0x60 лежит eid с меткой 0xB0 или 0xA0 -
// это сущность, и мы сразу знаем, куда подставлять свою цель.
static void own_body(void *a1, void *a2, void *a3, void *a4) {
    if (InterlockedCompareExchange(&g_ownLeft, 0, 0) <= 0) return;
    InterlockedDecrement(&g_ownLeft);
    void *args[4] = { a1, a2, a3, a4 };
    char line[400]; int n = 0;
    for (int i = 0; i < 4; i++) {
        uint32_t eid = 0;
        const char *tag = "";
        if (args[i] && readable((unsigned char *)args[i] + ENT_EID, 4)) {
            eid = *(uint32_t *)((unsigned char *)args[i] + ENT_EID);
            uint8_t t = (uint8_t)(eid >> 24);
            if (t == EID_WORLD_TAG) tag = " МИР";
            else if (t == EID_PLAYER_TAG) tag = " ИГРОК";
            else eid = 0;
        }
        n += _snprintf(line + n, sizeof line - n - 1, "  a%d=%p", i + 1, args[i]);
        if (eid) n += _snprintf(line + n, sizeof line - n - 1, " eid=%08X%s", eid, tag);
        if (n >= (int)sizeof line - 40) break;
    }
    line[n] = 0;
    L("[владелец]%s", line);
    if (!g_ownCtx && a1 && a4) { g_ownCtx = a1; g_ownTag = a4;
        L("[владелец] аргументы подобраны: ctx=%p tag=%p - теперь можно спрашивать самим", a1, a4); }
}

extern "C" void __fastcall cdloot_on_own(void *a1, void *a2, void *a3, void *a4) {
    __try { own_body(a1, a2, a3, a4); } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedExchange(&g_ownLeft, 0);
    }
}

// ------------------------------------------- кто заряжает узел --------------
// Сторож поймал запись в inter+0xE0 и цепочку вызовов. Наверху цепочки -
// функция +0x1DE1350, и она диспетчер: зовёт виртуальный метод +0x830 (или
// +0x838) у объекта, пришедшего в rcx. Чтобы понять, можно ли звать её самим,
// надо увидеть аргументы и опознать объект по имени класса.
static volatile LONG g_armLeft;

static void arm_body(void *a1, void *a2, void *a3, void *a4) {
    if (InterlockedCompareExchange(&g_armLeft, 0, 0) <= 0) return;
    InterlockedDecrement(&g_armLeft);
    const char *cls = 0;
    __try { cls = rtti_class(a1); } __except (EXCEPTION_EXECUTE_HANDLER) { }
    InterlockedExchange(&g_armMode, (LONG)(((uintptr_t)a2) & 0xFF));
    L("[зарядка] объект=%p (%s) режим=%d arg3=%p arg4=%u",
      a1, cls ? cls : "имя не читается",
      (int)(((uintptr_t)a2) & 0xFF), a3, (uint32_t)(uintptr_t)a4);
}

extern "C" void __fastcall cdloot_on_arm(void *a1, void *a2, void *a3, void *a4) {
    __try { arm_body(a1, a2, a3, a4); }
    __except (EXCEPTION_EXECUTE_HANDLER) { InterlockedExchange(&g_armLeft, 0); }
}

static bool hook_prologue(Hook &hk, unsigned char *target, void *handler,
                          int steal, const char *what) {
    if (!target) { L("[hook] %s: нечего перехватывать", what); return false; }
    if (steal < 12 || steal > 30) { L("[hook] %s: плохая длина пролога %d", what, steal); return false; }

    unsigned char *cave = alloc_near(target, 160);
    if (!cave) { L("[hook] %s: нет памяти рядом", what); return false; }

    unsigned char st[160]; int n = 0;
    // сохраняем аргументы, зовём обработчик, восстанавливаем
    unsigned char pro[] = { 0x48,0x83,0xEC,0x48,
                            0x48,0x89,0x4C,0x24,0x20, 0x48,0x89,0x54,0x24,0x28,
                            0x4C,0x89,0x44,0x24,0x30, 0x4C,0x89,0x4C,0x24,0x38 };
    memcpy(st + n, pro, sizeof pro); n += sizeof pro;
    st[n++] = 0x48; st[n++] = 0xB8; *(void **)(st + n) = handler; n += 8;
    st[n++] = 0xFF; st[n++] = 0xD0;
    unsigned char epi[] = { 0x48,0x8B,0x4C,0x24,0x20, 0x48,0x8B,0x54,0x24,0x28,
                            0x4C,0x8B,0x44,0x24,0x30, 0x4C,0x8B,0x4C,0x24,0x38,
                            0x48,0x83,0xC4,0x48 };
    memcpy(st + n, epi, sizeof epi); n += sizeof epi;
    // перенесённый пролог - выполняется при том же rsp, что и в оригинале
    memcpy(st + n, target, steal); n += steal;
    // и возврат в тело функции
    st[n++] = 0x48; st[n++] = 0xB8; *(void **)(st + n) = target + steal; n += 8;
    st[n++] = 0xFF; st[n++] = 0xE0;
    memcpy(cave, st, n);

    memcpy(hk.orig, target, steal); hk.origLen = steal;
    unsigned char patch[30];
    patch[0] = 0x48; patch[1] = 0xB8; *(void **)(patch + 2) = cave;
    patch[10] = 0xFF; patch[11] = 0xE0;
    for (int i = 12; i < steal; i++) patch[i] = 0x90;     // добиваем NOP-ами
    if (!write_code(target, patch, steal)) { L("[hook] %s: запись не удалась", what); return false; }

    hk.site = target; hk.cave = cave; hk.active = true; hk.name = what;
    L("[hook] %s: пролог перехвачен, +0x%llX -> %p (забрано %d байт)",
      what, (unsigned long long)(target - g_game.base), cave, steal);
    return true;
}

static void unhook_one(Hook &hk) {
    if (!hk.active) return;
    write_code(hk.site, hk.orig, hk.origLen);
    hk.active = false;
    L("[hook] %s: перехват снят, байты возвращены", hk.name ? hk.name : "?");
}

static void unhook_all() {
    unhook_one(g_areaHook);
    unhook_one(g_enqHook);
    unhook_one(g_ownHook);
    unhook_one(g_armHook);
    watch_stop();   // иначе обработчик останется в выгруженной dll - уже роняли игру
    // Пещеру НЕ освобождаем намеренно: в ней может находиться поток игры прямо
    // сейчас. Утечка в 4 КБ на перезагрузку безопаснее падения.
}

// ------------------------------------------------------------- рабочий поток

static HANDLE g_thread;

// Клавиша разработчика считается нажатой, только если она НЕ занята под
// настраиваемые клавиши из ini. Иначе одно нажатие делало бы два дела разом:
// человек ставит KeyAutoLoot=F9, а F9 в dev-сборке - это ещё и обзор.
static bool dev_down(int vk) {
    if (vk == g_cfg.keyAuto || vk == g_cfg.keyBurst ||
        vk == g_cfg.keySteal) return false;
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

static DWORD WINAPI worker(LPVOID) {
    int pF9 = 0, pF8 = 0, pF7 = 0, pF6 = 0, pF5 = 0, pF4 = 0, pF3 = 0, pF2 = 0, pF11 = 0;
    static int pF10 = 0, pF11b = 0;
    while (InterlockedCompareExchange(&g_running, 0, 0)) {
        cfg_watch();          // правку ini подхватываем на ходу
        // Обход сцены. Раньше он шёл внутри игрового вызова и стоил игре
        // кадра; теперь считаем здесь, а игровой поток только отправляет.
        if (InterlockedCompareExchange(&g_offThread, 0, 0) &&
            InterlockedCompareExchange(&g_gameTid, 0, 0)) {
            void *ctx = (void *)InterlockedCompareExchangePointer(
                            (void * volatile *)&g_ctxLatest, 0, 0);
            if (ctx) {
                __try { area_body(ctx, 0, 0, 0); }
                __except (EXCEPTION_EXECUTE_HANDLER) {
                    static LONG told2 = 0;
                    if (InterlockedExchange(&told2, 1) == 0)
                        L("[scan] обход вне игрового потока: чтение не удалось");
                }
            }
        }
        // Игровые клавиши. В готовом плагине их всего две, и работают они
        // всегда: F10 - автосбор, F11 - собрать пачку разом. В сборке с
        // загрузчиком F10 занят перезагрузкой логики, поэтому там его нет.
        // Настраиваемая клавиша автосбора работает в ОБЕИХ сборках. Раньше
        // она стояла под #ifdef и в dev-сборке не читалась вовсе: человек
        // правил KeyAutoLoot в ini, а ничего не менялось.
        int vkAuto = g_cfg.keyAuto;
#ifndef CDLOOT_PLUGIN
        // В сборке с загрузчиком F10 занята перезагрузкой логики.
        if (vkAuto == VK_F10) {
            static int saidF10 = 0;
            if (!saidF10) { saidF10 = 1;
                L("[key] KeyAutoLoot=F10 в dev-сборке занята загрузчиком, "
                  "автосбор остаётся на F4"); }
            vkAuto = 0;
        }
#endif
        int f10 = key_down(vkAuto) || key_down(g_cfg.padAuto);
        if (f10 && !pF10) {
            LONG on = InterlockedCompareExchange(&g_autoOn, 0, 0) ? 0 : 1;
            InterlockedExchange(&g_autoOn, on);
            notice(on ? T("Auto-Loot: ON",  "Автосбор: ВКЛ")
                      : T("Auto-Loot: OFF", "Автосбор: ВЫКЛ"),
                   on ? MB_OK : MB_ICONASTERISK);
            L("[key] F10 - автосбор %s", on ? "ВКЛЮЧЁН" : "выключен");
        }
        pF10 = f10;
        // Взять то, на что смотришь. ОТЛОЖЕНО ДО ПЛАШКИ: прицел выбирает
        // ближайший к перекрестью объект ПО УГЛУ, а не тот, во что игрок
        // целится на самом деле. Пока на экране нет метки выбранной цели,
        // игрок не видит, что возьмётся, - и одно нажатие может унести не ту
        // вещь, а если она чужая, то ещё и с розыском. Поэтому клавиша живёт
        // только при Debug=1, где метки видны. Настройка KeyTakeAimed
        // остаётся в коде, но из ini убрана.
        if (g_debug) {
            static int pSteal = 0;
            int st = key_down(g_cfg.keySteal) || key_down(g_cfg.padSteal);
            if (st && !pSteal) {
                InterlockedExchange(&g_aimTakeReq, 1);
                InterlockedExchange(&g_scanRequested, 1);
                L("[key] взять то, на что смотрю");
            }
            pSteal = st;
        }
        {   // Пачка - тоже в обеих сборках. Прежде она была заперта за
            // !g_debug и в dev-сборке молчала.
            int f11b = key_down(g_cfg.keyBurst) || key_down(g_cfg.padBurst);
            if (f11b && !pF11b) {
                InterlockedExchange(&g_burstRequested, 1);
                InterlockedExchange(&g_scanRequested, 0);
                notice(T("Auto-Loot: BURST", "Автосбор: ПАЧКА"), MB_ICONEXCLAMATION);
                L("[key] F11 - собрать пачку");
            }
            pF11b = f11b;
        }

        // Дальше - клавиши разработчика. Их много, они шумят в лог и часть из
        // них лезет в память игры, поэтому в готовой сборке они молчат.
        if (!g_debug) { Sleep(20); continue; }

        int f9 = dev_down(VK_F9);
        int f8 = dev_down(VK_F8);
        int f7 = dev_down(VK_F7);
        int f6 = dev_down(VK_F6);
        int f5 = dev_down(VK_F5);
        int f4 = dev_down(VK_F4);
        int f3 = dev_down(VK_F3);
        int f2 = dev_down(VK_F2);
        int f11 = dev_down(VK_F11);
        static int pNum8 = 0;
        int num8 = dev_down(VK_NUMPAD8);
        if (num8 && !pNum8) {
            // Перебор вариантов взгляда убран - направление подтверждено.
            // Клавиша теперь показывает состояние камеры одной строкой.
            if (!g_camTf || !readable(g_camTf, 0x120)) L("[камера] ещё не найдена");
            else {
                const float *fd = (const float *)(g_camTf + 0xD4);
                const float *lp = (const float *)(g_camTf + 0xEC);
                const float *wp = (const float *)(g_camTf + 0xC8);
                L("[камера] глаз локально (%.2f %.2f %.2f), мировой (%.2f %.2f %.2f), "
                  "взгляд (%.2f %.2f %.2f), масштабы %.3f %.3f",
                  lp[0], lp[1], lp[2], wp[0], wp[1], wp[2], fd[0], fd[1], fd[2],
                  *(const float *)(g_camTf + 0x08), *(const float *)(g_camTf + 0x1C));
            }
        }
        pNum8 = num8;
        static int pNum7 = 0;
        int num7 = dev_down(VK_NUMPAD7);
        if (num7 && !pNum7) {
            InterlockedExchange(&g_camReq, 1);
            L("[key] Numpad7 - поиск камеры");
        }
        pNum7 = num7;
        static int pNum6 = 0;
        int num6 = dev_down(VK_NUMPAD6);
        if (num6 && !pNum6) {
            InterlockedExchange(&g_aimReq, 1);
            L("[key] Numpad6 - ищу наведённую цель игры");
        }
        pNum6 = num6;
        static int pNum2 = 0;
        int num2 = dev_down(VK_NUMPAD2);
        if (num2 && !pNum2) {
            LONG on = InterlockedCompareExchange(&g_marksOn, 0, 0) ? 0 : 1;
            InterlockedExchange(&g_marksOn, on);
            if (!on) { EnterCriticalSection(&g_hudCs); g_markN = 0; LeaveCriticalSection(&g_hudCs);
                       InterlockedIncrement(&g_hudSeq); }
            L("[key] Numpad2 - метки и камера %s", on ? "ВКЛЮЧЕНЫ" : "выключены");
        }
        pNum2 = num2;
        static int pNum1 = 0;
        int num1 = dev_down(VK_NUMPAD1);
        if (num1 && !pNum1) {
            InterlockedExchange(&g_armAimReq, 1);
            InterlockedExchange(&g_scanRequested, 1);
            L("[key] Numpad1 - зарядить узел, на который смотрю");
        }
        pNum1 = num1;
        static int pNum5 = 0;
        int num5 = dev_down(VK_NUMPAD5);
        if (num5 && !pNum5) {
            InterlockedExchange(&g_armReq, 1);
            L("[key] Numpad5 - попытка зарядить ближайший узел");
        }
        pNum5 = num5;
        if (f11 && !pF11) {
            InterlockedExchange(&g_watchReq, 1);
            InterlockedExchange(&g_armLeft, 30);
            L("[key] F11 - сторож на узел + запись аргументов зарядки");
        }
        pF11 = f11;
        if (f2 && !pF2) {
            InterlockedExchange(&g_nameRequested, 1);
            InterlockedExchange(&g_scanRequested, 1);
            L("[key] F2 - что это: наведённая цель и ближайшие");
        }
        pF2 = f2;
        if (f3 && !pF3) {
            LONG on = InterlockedCompareExchange(&g_hudOn, 0, 0) ? 0 : 1;
            InterlockedExchange(&g_hudOn, on);
            L("[key] F3 - плашка %s", on ? "показана" : "скрыта");
        }
        pF3 = f3;
        if (f4 && !pF4) {
            LONG on = InterlockedCompareExchange(&g_autoOn, 0, 0) ? 0 : 1;
            InterlockedExchange(&g_autoOn, on);
            L("[key] F4 - автосбор %s", on ? "ВКЛЮЧЁН" : "выключен");
        }
        pF4 = f4;
        if (f5 && !pF5) {
            InterlockedExchange(&g_forceRequested, 1);
            InterlockedExchange(&g_scanRequested, 1);
            L("[key] F5 - взять ближайшее, кроме растений");
        }
        pF5 = f5;
        if (f9 && !pF9) { InterlockedExchange(&g_scanRequested, 1); L("[key] F9 - обзор"); }
        if (f8 && !pF8) {
            InterlockedExchange(&g_takeRequested, 1);
            InterlockedExchange(&g_scanRequested, 1);
            L("[key] F8 - взять ближайшее");
        }
        if (f6 && !pF6) {
            InterlockedExchange(&g_dryRequested, 1);
            InterlockedExchange(&g_scanRequested, 1);
            L("[key] F6 - проверка без отправки");
        }
        if (f7 && !pF7) {
            // Переключатель, а не десять секунд. Кражу из кармана в короткое
            // окно не уложить: подойти, навестись, дождаться анимации. Теперь
            // F7 включает запись и держит её, пока не нажмут ещё раз.
            // Считаем включённой только СВОЮ запись. При старте мод сам
            // держит слушатель час, чтобы подсмотреть route, и без этого
            // флага первое же нажатие F7 гасило служебный режим вместо того,
            // чтобы включить пользовательский.
            bool on = InterlockedCompareExchange(&g_spyUser, 0, 0) != 0;
            if (on) {
                InterlockedExchange(&g_spyUser, 0);
                InterlockedExchange(&g_spyLeft, 0);
                L("[key] F7 - запись остановлена");
            } else {
                InterlockedExchange(&g_spyUser, 1);
                InterlockedExchange(&g_spyUntil, (LONG)GetTickCount() + 300000);
                InterlockedExchange(&g_spyLeft, 300);
                InterlockedIncrement(&g_spyGen);   // счётчики номеров - с нуля
                InterlockedExchange(&g_ownLeft, 30);   // заодно послушать оракул
            InterlockedExchange(&g_stealLog, 20);  // и записать его ответы нам
                L("[key] F7 - ПИШУ события игры (до 5 минут или 300 штук). "
                  "Делайте действие руками. F7 ещё раз - остановить.");
            }
        }
        pF9 = f9; pF8 = f8; pF7 = f7; pF6 = f6;
        Sleep(20);
    }
    L("[core] рабочий поток остановлен");
    return 0;
}

// ------------------------------------------------------------------ экспорт -

// Наш ASI лежит в bin64, а туда же смотрит crashpad_handler.exe - служебный
// процесс игры. Загрузчик подхватывает нас и туда, и мы сканируем чужую
// память и держим файлы. Работаем только внутри самой игры.
static bool host_is_game() {
    char exe[MAX_PATH] = "";
    GetModuleFileNameA(NULL, exe, MAX_PATH);
    const char *name = strrchr(exe, (char)92);
    name = name ? name + 1 : exe;
    return _stricmp(name, "CrimsonDesert.exe") == 0;
}

CDEXPORT int CoreStart(const char *dir, int generation) {
    if (!host_is_game()) return 0;      // не игра - тихо уходим
    InitializeCriticalSection(&g_logCs);
    char path[MAX_PATH];
    // Пробуем общий лог загрузчика. Если он держит файл монопольно (старая
    // версия загрузчика), пишем в свой - лучше отдельный файл, чем немой мод.
    sprintf(path, "%sCDLoot.log", dir);
    g_log = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                        0, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
    if (g_log == INVALID_HANDLE_VALUE) {
        sprintf(path, "%sCDLoot_core.log", dir);
        g_log = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                            0, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
    }
    g_logReady = true;
    L("=== CDLoot core запущен, поколение %d ===", generation);
    cfg_load(dir);
    if (!g_cfg.enabled) { L("[ini] Enabled=0 - мод выключен, ничего не делаю"); return 0; }
    if (g_cfg.autoMode) InterlockedExchange(&g_autoOn, 1);

    HMODULE h = GetModuleHandleA(NULL);          // главный модуль процесса
    if (!h) { L("модуль игры не найден"); return 0; }
    g_game.base = (unsigned char *)h;
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)h;
    IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)((unsigned char *)h + dos->e_lfanew);
    g_game.size = nt->OptionalHeader.SizeOfImage;
    L("игра: база=%p размер=0x%llX", g_game.base, (unsigned long long)g_game.size);

    if (!resolve_functions()) { L("[core] не запускаюсь"); return 0; }
    hook_prologue(g_areaHook, g_fn.areaFn, (void *)cdloot_on_area, 15, "обход сцены");
    // Пролог очереди: mov [rsp+8],rbx (5) + push rdi (1) + sub rsp,20 (4)
    // + mov r?,[r?+38] (4) = 14 байт, ни одной rip-относительной ссылки.
    hook_prologue(g_enqHook, g_fn.enqueue, (void *)cdloot_on_enqueue, 14, "очередь событий");
    // Оракул воровства слушаем первые тридцать вызовов после загрузки:
    // этого хватает, чтобы увидеть, каким аргументом приходит цель. Пролог
    // у него - три mov в теневую область плюс push rbp, ссылок на rip нет.
    hook_prologue(g_ownHook, g_fn.ownCheck, (void *)cdloot_on_own, 15, "оракул воровства");
    // Диспетчер зарядки узла. Пролог: mov [rsp+18],rbx / mov [rsp+10],dl /
    // push rbp,rsi,rdi,r12 = ровно 15 байт, ссылок на rip нет.
    unsigned char *armFn = aob("зарядка узла",
        "48 89 5C 24 18 88 54 24 10 55 56 57 41 54 41 55 41 56 41 57 48 8D 6C 24 ?? 48 81 EC 00 01 00 00");
    // 16 байт, не 15: на 15 разрезается пополам "push r13" (41 55), и возврат
    // уходит в середину инструкции. Одна такая ошибка уже уронила игру.
    //   48 89 5C 24 18 (5) + 88 54 24 10 (4) + 55 56 57 (3) + 41 54 (2) + 41 55 (2)
    resolve_item_table();
    resolve_aim_query();
    g_armFn = armFn;
    if (armFn) hook_prologue(g_armHook, armFn, (void *)cdloot_on_arm, 16, "зарядка узла");
    InterlockedExchange(&g_ownLeft, 30);

    // Слушатель включён сразу и молча ждёт: как только вы подберёте что-то
    // руками, в логе окажется эталонное событие игры. F7 обновляет запас.
    InterlockedExchange(&g_spyUntil, (LONG)GetTickCount() + 3600000);
    InterlockedExchange(&g_spyLeft, 40);
    LARGE_INTEGER qf; QueryPerformanceFrequency(&qf); g_qpcFreq = (double)qf.QuadPart;
    {   // Сколько стоит одна проверка памяти именно в этом процессе. У игры
        // огромное и рваное адресное пространство, VirtualQuery обходит его
        // дерево - и может оказаться на порядки дороже обычного.
        MEMORY_BASIC_INFORMATION mbi;
        LARGE_INTEGER a, b; QueryPerformanceCounter(&a);
        for (int i = 0; i < 1000; i++) VirtualQuery((void *)(g_game.base + (i * 4096)), &mbi, sizeof mbi);
        QueryPerformanceCounter(&b);
        L("[время] одна проверка памяти = %.1f мкс",
          (double)(b.QuadPart - a.QuadPart) * 1000000.0 / g_qpcFreq / 1000.0);
    }
    InterlockedExchange(&g_running, 1);
    g_thread = CreateThread(0, 0, worker, 0, 0, 0);
    InitializeCriticalSection(&g_hudCs);
    hud_set("CDLoot готов");
    InterlockedExchange(&g_hudRunning, 1);
    g_hudThread = CreateThread(0, 0, hud_thread, 0, 0, 0);
#ifdef CDLOOT_PLUGIN
    L("[core] готов. F4 АВТОСБОР | F8 взять | F5 не растение | F3 плашка | F9 обзор | F2 что это");
    L("[core] разведка: F6 проверка | F7 запись событий | F11 сторож | Num5 зарядить узел");
#else
    L("[core] готов. F4 АВТОСБОР | F8 взять | F5 не растение | F3 плашка | F9 обзор | F2 что это");
    L("[core] разведка: F6 проверка | F7 запись событий | F11 сторож | Num5 зарядить узел | F10 перезагрузка");
#endif
    return 1;
}

CDEXPORT void CoreStop() {
    L("[core] остановка...");
    InterlockedExchange(&g_running, 0);
    InterlockedExchange(&g_autoOn, 0);
    InterlockedExchange(&g_spyLeft, 0);
    if (g_thread) { WaitForSingleObject(g_thread, 2000); CloseHandle(g_thread); g_thread = 0; }
    // Плашку снимаем до выгрузки: её окно и класс держат наш код.
    InterlockedExchange(&g_hudRunning, 0);
    if (g_hudThread) { WaitForSingleObject(g_hudThread, 3000); CloseHandle(g_hudThread); g_hudThread = 0; }
    DeleteCriticalSection(&g_hudCs);
    unhook_all();
    // даём игровым потокам выйти из нашей пещеры
    Sleep(120);
    L("[core] остановлен чисто");
    g_logReady = false;
    if (g_log != INVALID_HANDLE_VALUE) { CloseHandle(g_log); g_log = INVALID_HANDLE_VALUE; }
    DeleteCriticalSection(&g_logCs);
}

// В обычной сборке логика живёт отдельной dll, а загрузчик CDLoot.asi
// перезагружает её на лету - так удобно разрабатывать. Готовый плагин
// собирается из этого же файла с CDLOOT_PLUGIN: тогда .asi самодостаточен,
// горячей перезагрузки нет, и запуск идёт прямо отсюда.
#ifdef CDLOOT_PLUGIN
static DWORD WINAPI plugin_boot(LPVOID) {
    char dir[MAX_PATH];
    GetModuleFileNameA(GetModuleHandleA(0), dir, MAX_PATH);
    char *slash = strrchr(dir, (char)92);
    if (slash) slash[1] = 0;
    CoreStart(dir, 1);
    return 0;
}
#endif

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
#ifdef CDLOOT_PLUGIN
        // Из DllMain ничего тяжёлого делать нельзя - уходим на свой поток.
        CloseHandle(CreateThread(0, 0, plugin_boot, 0, 0, 0));
#endif
    }
    if (reason == DLL_PROCESS_DETACH) {
#ifdef CDLOOT_PLUGIN
        CoreStop();
#endif
    }
    return TRUE;
}
