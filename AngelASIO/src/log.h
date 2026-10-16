#pragma once
#include <cstdarg>
#include <cstdio>
#include <string>
#include <windows.h>

inline std::wstring angelDataDir() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH);
    std::wstring d = (n > 0 && n < MAX_PATH) ? std::wstring(buf) : std::wstring(L".");
    d += L"\\AngelASIO";
    CreateDirectoryW(d.c_str(), nullptr);
    return d;
}

inline void angelLog(const char* fmt, ...) {
    static CRITICAL_SECTION cs;
    static bool init = false;
    if (!init) { InitializeCriticalSection(&cs); init = true; }
    char msg[512];
    va_list ap; va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    EnterCriticalSection(&cs);
    std::wstring p = angelDataDir() + L"\\angelasio.log";
    FILE* f = _wfopen(p.c_str(), L"ab");
    if (f) {
        fprintf(f, "[%lu] %s\n", (unsigned long)GetTickCount(), msg);
        fclose(f);
    }
    LeaveCriticalSection(&cs);
}
