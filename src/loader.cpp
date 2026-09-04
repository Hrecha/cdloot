// ============================================================================
//  CDLoot loader - крошечный ASI, который живёт всю сессию игры.
//
//  Сам ничего не делает: подгружает CDLoot_core.dll и умеет перезагрузить её
//  на лету. Компилятор при этом свободно перезаписывает оригинал, потому что
//  загружается всегда КОПИЯ.
//
//  F10  - перезагрузить логику сейчас
//  Либо просто пересоберите core - загрузчик заметит новую дату файла сам.
// ============================================================================

#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

typedef int  (*CoreStartFn)(const char *dir, int generation);
typedef void (*CoreStopFn)(void);

static char g_dir[MAX_PATH];
static char g_src[MAX_PATH];      // CDLoot_core.dll - его пересобирает компилятор
static char g_live[MAX_PATH];     // копия текущего поколения
// Каждое поколение грузим под своим именем. Старая копия может остаться
// заблокированной, если в ней ещё есть игровые потоки - и это нормально,
// мы просто её не трогаем.
static HMODULE g_core;
static int  g_generation;
static HANDLE g_log = INVALID_HANDLE_VALUE;

static void L(const char *fmt, ...) {
    char buf[1024]; DWORD wr; SYSTEMTIME st; GetLocalTime(&st);
    int n = sprintf(buf, "[%02u:%02u:%02u.%03u] [loader] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list ap; va_start(ap, fmt);
    n += vsnprintf(buf + n, sizeof buf - n - 3, fmt, ap);
    va_end(ap);
    buf[n++] = 13; buf[n++] = 10; buf[n] = 0;
    if (g_log != INVALID_HANDLE_VALUE) WriteFile(g_log, buf, n, &wr, 0);
}

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

static FILETIME src_time() {
    WIN32_FILE_ATTRIBUTE_DATA fa; FILETIME z = {};
    if (!GetFileAttributesExA(g_src, GetFileExInfoStandard, &fa)) return z;
    return fa.ftLastWriteTime;
}

static void unload_core() {
    if (!g_core) return;
    CoreStopFn stop = (CoreStopFn)GetProcAddress(g_core, "CoreStop");
    if (stop) stop();
    // Если выгрузка не удалась - не беда: перехваты уже сняты, а новая копия
    // поедет под другим именем.
    FreeLibrary(g_core);
    g_core = 0;
    L("логика выгружена");
}

static bool load_core() {
    sprintf(g_live, "%sCDLoot_core.live%d.dll", g_dir, g_generation + 1);
    for (int attempt = 0; attempt < 20; attempt++) {
        if (CopyFileA(g_src, g_live, FALSE)) break;
        Sleep(150);                       // компилятор ещё пишет файл
        if (attempt == 19) { L("не удалось скопировать core: ошибка %lu", GetLastError()); return false; }
    }
    g_core = LoadLibraryA(g_live);
    if (!g_core) { L("LoadLibrary не смог: ошибка %lu", GetLastError()); return false; }
    CoreStartFn start = (CoreStartFn)GetProcAddress(g_core, "CoreStart");
    if (!start) { L("в core нет CoreStart"); FreeLibrary(g_core); g_core = 0; return false; }
    int ok = start(g_dir, ++g_generation);
    L("логика загружена, поколение %d, старт вернул %d", g_generation, ok);
    return ok != 0;
}

static void reload(const char *why) {
    L("перезагрузка (%s)", why);
    unload_core();
    Sleep(80);
    load_core();
}

static DWORD WINAPI worker(LPVOID) {
    if (!host_is_game()) return 0;      // не игра - тихо уходим
    GetModuleFileNameA(GetModuleHandleA(0), g_dir, MAX_PATH);
    char *slash = strrchr(g_dir, (char)92); if (slash) slash[1] = 0;
    sprintf(g_src,  "%sCDLoot_core.dll", g_dir);
    char logPath[MAX_PATH]; sprintf(logPath, "%sCDLoot.log", g_dir);
    g_log = CreateFileA(logPath, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                        0, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);

    L("старт. core=%s", g_src);
    if (GetFileAttributesA(g_src) == INVALID_FILE_ATTRIBUTES) {
        L("CDLoot_core.dll не найден рядом - положите его в bin64");
        return 0;
    }
    FILETIME last = src_time();
    load_core();
    L("F10 - перезагрузить вручную. Пересборка core подхватывается сама.");

    int prevKey = 0;
    for (;;) {
        int now = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
        if (now && !prevKey) reload("нажата F10");
        prevKey = now;

        FILETIME t = src_time();
        if (CompareFileTime(&t, &last) != 0 && t.dwLowDateTime) {
            Sleep(400);                       // ждём, пока компилятор допишет
            FILETIME t2 = src_time();
            if (CompareFileTime(&t, &t2) == 0) { last = t2; reload("core пересобран"); }
            else last = t2;
        }
        Sleep(100);
    }
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) { DisableThreadLibraryCalls(h); CreateThread(0, 0, worker, 0, 0, 0); }
    return TRUE;
}
