// ============================================================================
//  CDLoot - автосбор для Crimson Desert, написанный с нуля.
//
//  Этап 1: найти игровые функции по сигнатурам, зацепиться за обход сцены,
//          перечислить сущности вокруг игрока и записать, что видим.
//          Ничего в игру пока не отправляется - только чтение.
//
//  Всё, что здесь зашито, добыто разбором игры версии 1.0.0.2474.
// ============================================================================

#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

// ---------------------------------------------------------------- лог -------

static HANDLE g_log = INVALID_HANDLE_VALUE;
static CRITICAL_SECTION g_logCs;

static void L(const char *fmt, ...) {
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

// ------------------------------------------------------- поиск по сигнатуре --

struct Module { unsigned char *base; size_t size; };
static Module g_game;

// Возвращает адрес единственного совпадения. Если совпадений ноль или больше
// одного - возвращает 0 и пишет об этом в лог: молча брать первое опасно.
static unsigned char *aob(const char *name, const char *pattern) {
    unsigned char bytes[128]; char mask[128]; int len = 0;
    for (const char *p = pattern; *p && len < 128; ) {
        if (*p == ' ') { p++; continue; }
        if (*p == '?') { bytes[len] = 0; mask[len] = '?'; len++; p += (p[1] == '?') ? 2 : 1; }
        else {
            char h[3] = { p[0], p[1], 0 };
            bytes[len] = (unsigned char)strtoul(h, 0, 16); mask[len] = 'x'; len++; p += 2;
        }
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

// --------------------------------------------------------- игровые функции --

struct GameFns {
    unsigned char *tlsInit;      // инициализация локального хранилища потока
    unsigned char *descLookup;   // поиск дескриптора события по номеру
    unsigned char *allocEvent;   // выделение объекта события
    unsigned char *enqueue;      // постановка события в очередь
    unsigned char *areaFn;       // обход сцены: даёт контекст со списком сущностей
    unsigned char *ownCheck;     // проверка владельца предмета
    unsigned char *getPos;       // позиция сущности
} g_fn;

static bool resolve_functions() {
    // Сигнатуры проверены на 1.0.0.2474. tls_init берётся склеенным с
    // desc_lookup - по отдельности первый шаблон встречается трижды.
    unsigned char *tlsDesc = aob("tls+desc",
        "48 83 EC 28 BA 9C 00 00 00 65 48 8B 04 25 58 00 00 00 48 8B 08 8B 04 0A 39 05 ?? ?? ?? ?? "
        "7E ?? 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 83 3D ?? ?? ?? ?? FF 75 ?? 48 8D 0D ?? ?? ?? ?? "
        "E8 ?? ?? ?? ?? 90 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8D 05 ?? ?? ?? ?? 48 83 C4 28 C3 "
        "CC CC CC CC CC CC 40 56 45 33 C9 44 0F B7 D2");
    if (tlsDesc) { g_fn.tlsInit = tlsDesc; g_fn.descLookup = tlsDesc + 0x60; }

    // У alloc_event в прологе лежит индекс статической TLS-переменной, и он
    // меняется каждым патчем игры - поэтому здесь маска, а не константа.
    g_fn.allocEvent = aob("alloc_event",
        "48 89 5C 24 ?? 4C 89 44 24 ?? 57 48 83 EC 20 8B ?? BA ?? ?? 00 00");

    g_fn.enqueue = aob("enqueue",
        "48 89 5C 24 08 57 48 83 EC 20 48 8B ?? 38 65 48 8B 04 25 58 00 00 00");

    g_fn.areaFn = aob("area_sweep",
        "55 41 54 41 55 41 56 41 57 48 8B EC 48 83 EC 50 C5 F8 29 74 24 40 4D 8B ?? 44 8B ?? 48 8B ??");

    g_fn.ownCheck = aob("own_check",
        "48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 55 41 54 41 55 41 56 41 57 48 8B EC "
        "48 81 EC 80 00 00 00 49 8B F0 4C 8B FA");

    g_fn.getPos = aob("get_pos",
        "40 53 48 83 EC 50 48 8B 41 68 48 8B 88 ?? 01 00 00 48 8B 01");

    bool ok = g_fn.tlsInit && g_fn.descLookup && g_fn.allocEvent &&
              g_fn.enqueue && g_fn.areaFn && g_fn.getPos;
    L("[aob] итог: %s", ok ? "все ключевые функции найдены"
                           : "часть функций не найдена, мод работать не будет");
    return ok;
}

// ------------------------------------------------------ раскладка сущности --
// Смещения подтверждены дампом 400 объектов на версии 1.0.0.2474.

#define ENT_EID        0x60    // uint32 - идентификатор сущности
#define ENT_VTABLE     0x180   // указатель на таблицу виртуальных функций
#define ENT_CAT        0x1EA   // uint8  - категория
#define ENT_COMPONENT  0x48    // указатель на компонент (тип, флаг смерти)
#define COMP_DEAD      0x273   // uint8 внутри компонента

static bool readable(const void *p, SIZE_T n) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(p, &mbi, sizeof mbi)) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    return (SIZE_T)((unsigned char *)mbi.BaseAddress + mbi.RegionSize - (unsigned char *)p) >= n;
}

struct Entity {
    unsigned char *p;
    uint32_t eid()  const { return *(uint32_t *)(p + ENT_EID); }
    uint8_t  cat()  const { return *(uint8_t  *)(p + ENT_CAT); }
    unsigned char *comp() const {
        unsigned char *c = *(unsigned char **)(p + ENT_COMPONENT);
        return (c && readable(c, COMP_DEAD + 1)) ? c : 0;
    }
    int dead() const { unsigned char *c = comp(); return c ? c[COMP_DEAD] : -1; }
};

// ------------------------------------------------------------ перехват ------
// Перенаправляем инструкцию call, а не пролог функции: так мы не мешаем
// другим модам, которые могли зацепиться за саму функцию.

static unsigned char *alloc_near(void *anchor) {
    SYSTEM_INFO si; GetSystemInfo(&si);
    uintptr_t a = (uintptr_t)anchor;
    for (uintptr_t d = si.dwAllocationGranularity; d < 0x60000000ull; d += si.dwAllocationGranularity) {
        for (int dir = 0; dir < 2; dir++) {
            uintptr_t t = dir ? a + d : a - d;
            void *m = VirtualAlloc((void *)(t & ~(uintptr_t)(si.dwAllocationGranularity - 1)),
                                   0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
            if (m) return (unsigned char *)m;
        }
    }
    return 0;
}

// -------------------------------------------------------------- обход сцены --

#define CTX_ENTITY_ARRAY 0x110   // ctx -> массив сущностей

static volatile LONG g_scanRequested;
static void *g_lastCtx;

// Наш обработчик: вызывается перед оригинальной функцией обхода.
extern "C" void __fastcall cdloot_on_area(void *ctx) {
    g_lastCtx = ctx;
    if (!InterlockedCompareExchange(&g_scanRequested, 0, 0)) return;
    InterlockedExchange(&g_scanRequested, 0);

    if (!ctx || !readable(ctx, CTX_ENTITY_ARRAY + 8)) { L("[scan] контекст нечитаем"); return; }
    unsigned char **arr = *(unsigned char ***)((unsigned char *)ctx + CTX_ENTITY_ARRAY);
    if (!arr || !readable(arr, 8)) { L("[scan] массив сущностей нечитаем"); return; }

    L("[scan] ctx=%p массив=%p", ctx, arr);
    int shown = 0, total = 0;
    for (int i = 0; i < 512; i++) {
        if (!readable(arr + i, 8)) break;
        unsigned char *e = arr[i];
        if (!e) continue;
        if (!readable(e, ENT_CAT + 1)) continue;
        total++;
        Entity ent{ e };
        if (shown < 40) {
            L("   [%3d] eid=%08X cat=0x%02X dead=%d ptr=%p",
              i, ent.eid(), ent.cat(), ent.dead(), e);
            shown++;
        }
    }
    L("[scan] всего сущностей: %d", total);
}

static bool hook_area() {
    if (!g_fn.areaFn) return false;
    // ищем инструкцию call, ведущую на функцию обхода
    unsigned char *site = 0;
    for (unsigned char *p = g_game.base; p < g_game.base + g_game.size - 5; p++) {
        if (*p != 0xE8) continue;
        int32_t rel = *(int32_t *)(p + 1);
        if (p + 5 + rel == g_fn.areaFn) { site = p; break; }
    }
    if (!site) { L("[hook] вызов функции обхода не найден"); return false; }

    unsigned char *cave = alloc_near(site);
    if (!cave) { L("[hook] не выделить память рядом"); return false; }

    unsigned char st[96]; int n = 0;
    unsigned char pro[] = { 0x48,0x83,0xEC,0x48,
                            0x48,0x89,0x4C,0x24,0x20, 0x48,0x89,0x54,0x24,0x28,
                            0x4C,0x89,0x44,0x24,0x30, 0x4C,0x89,0x4C,0x24,0x38 };
    memcpy(st + n, pro, sizeof pro); n += sizeof pro;
    st[n++] = 0x48; st[n++] = 0xB8; *(void **)(st + n) = (void *)cdloot_on_area; n += 8;
    st[n++] = 0xFF; st[n++] = 0xD0;
    unsigned char epi[] = { 0x48,0x8B,0x4C,0x24,0x20, 0x48,0x8B,0x54,0x24,0x28,
                            0x4C,0x8B,0x44,0x24,0x30, 0x4C,0x8B,0x4C,0x24,0x38,
                            0x48,0x83,0xC4,0x48 };
    memcpy(st + n, epi, sizeof epi); n += sizeof epi;
    st[n++] = 0x48; st[n++] = 0xB8; *(void **)(st + n) = (void *)g_fn.areaFn; n += 8;
    st[n++] = 0xFF; st[n++] = 0xE0;
    memcpy(cave, st, n);

    DWORD old;
    if (!VirtualProtect(site, 5, PAGE_EXECUTE_READWRITE, &old)) return false;
    *(int32_t *)(site + 1) = (int32_t)((intptr_t)cave - (intptr_t)(site + 5));
    VirtualProtect(site, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, 5);
    L("[hook] обход сцены перехвачен: вызов +0x%llX -> %p",
      (unsigned long long)(site - g_game.base), cave);
    return true;
}

// ---------------------------------------------------------------- запуск ----

static int g_scanKey = VK_F9;

static DWORD WINAPI worker(LPVOID) {
    char path[MAX_PATH];
    GetModuleFileNameA(GetModuleHandleA(0), path, MAX_PATH);
    char *slash = strrchr(path, (char)92); if (slash) slash[1] = 0;
    strcat(path, "CDLoot.log");
    InitializeCriticalSection(&g_logCs);
    g_log = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, 0, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);

    L("=== CDLoot - этап 1: разведка ===");

    HMODULE h = GetModuleHandleA("CrimsonDesert.exe");
    if (!h) { L("модуль игры не найден"); return 0; }
    g_game.base = (unsigned char *)h;
    {
        IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)h;
        IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)((unsigned char *)h + dos->e_lfanew);
        g_game.size = nt->OptionalHeader.SizeOfImage;
    }
    L("модуль игры: база=%p размер=0x%llX", g_game.base, (unsigned long long)g_game.size);

    if (!resolve_functions()) { L("останавливаюсь, дальше смысла нет"); return 0; }
    hook_area();

    L("готово. F9 - снять список сущностей вокруг.");
    int prev = 0;
    for (;;) {
        int now = (GetAsyncKeyState(g_scanKey) & 0x8000) != 0;
        if (now && !prev) { InterlockedExchange(&g_scanRequested, 1); L("[key] F9 - запрошен обзор"); }
        prev = now;
        Sleep(20);
    }
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) { DisableThreadLibraryCalls(h); CreateThread(0, 0, worker, 0, 0, 0); }
    return TRUE;
}
