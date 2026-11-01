// xavc_util.cpp - settings and logging.

#include "xavc_common.h"
#include <stdarg.h>

namespace {

XavcSettings g_settings;
INIT_ONCE g_settingsOnce = INIT_ONCE_STATIC_INIT;

bool ReadDword(HKEY root, const wchar_t* sub, const wchar_t* name, DWORD* out) {
    HKEY k = NULL;
    if (RegOpenKeyExW(root, sub, 0, KEY_QUERY_VALUE, &k) != ERROR_SUCCESS) return false;
    DWORD type = 0, v = 0, cb = sizeof(v);
    LONG r = RegQueryValueExW(k, name, NULL, &type, (BYTE*)&v, &cb);
    RegCloseKey(k);
    if (r != ERROR_SUCCESS || type != REG_DWORD || cb != sizeof(v)) return false;
    *out = v;
    return true;
}

DWORD Setting(const wchar_t* name, DWORD def) {
    DWORD v = 0;
    if (ReadDword(HKEY_CURRENT_USER, L"Software\\XavcDecoder", name, &v)) return v;
    if (ReadDword(HKEY_LOCAL_MACHINE, L"SOFTWARE\\XavcDecoder", name, &v)) return v;
    return def;
}

BOOL CALLBACK InitSettings(PINIT_ONCE, PVOID, PVOID*) {
    g_settings.logLevel     = Setting(L"LogLevel", 0);
    g_settings.hwDecode     = Setting(L"HardwareDecode", 1);
    g_settings.outputFormat = Setting(L"OutputFormat", 0);
    g_settings.threads      = Setting(L"DecoderThreads", 0);
    if (g_settings.threads > 64) g_settings.threads = 64;
    return TRUE;
}

} // namespace

const XavcSettings& XavcGetSettings() {
    InitOnceExecuteOnce(&g_settingsOnce, InitSettings, NULL, NULL);
    return g_settings;
}

void XavcLogAt(int level, const char* fmt, ...) {
    if (level <= 0 || (DWORD)level > XavcGetSettings().logLevel) return;
    DWORD lastError = GetLastError();   // logging must not disturb callers' error state

    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf(msg, sizeof(msg) - 1, fmt, ap);
    va_end(ap);
    msg[sizeof(msg) - 1] = 0;

    SYSTEMTIME st;
    GetLocalTime(&st);
    char line[1200];
    _snprintf(line, sizeof(line) - 1, "%02u:%02u:%02u.%03u [XAVC pid=%lu tid=%lu] %s\r\n",
              st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
              (unsigned long)GetCurrentProcessId(), (unsigned long)GetCurrentThreadId(), msg);
    line[sizeof(line) - 1] = 0;
    OutputDebugStringA(line);

    wchar_t path[MAX_PATH];
    DWORD n = ExpandEnvironmentStringsW(L"%ProgramData%\\XavcDecoder\\xavc.log", path, MAX_PATH);
    if (n && n <= MAX_PATH) {
        HANDLE h = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                               OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD w = 0;
            WriteFile(h, line, (DWORD)strlen(line), &w, NULL);
            CloseHandle(h);
        }
    }
    SetLastError(lastError);
}
