// xavc_ffmpeg.cpp - run-time loading of the FFmpeg DLLs shipped next to xavc_decoder.dll.

#include "xavc_common.h"
#include "xavc_ffmpeg.h"
#include <stdarg.h>

#define XAVC_STR2(x) #x
#define XAVC_STR(x) XAVC_STR2(x)
#define XAVC_WIDEN2(s) L ## s
#define XAVC_WIDEN(s) XAVC_WIDEN2(s)
#define XAVC_WSTR(x) XAVC_WIDEN(XAVC_STR(x))   // XAVC_WSTR(61) -> L"61"

namespace {

XavcFF g_ff;
bool g_ok = false;
volatile LONG g_loaded = 0;
INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;
volatile LONG g_logBudget = 300;   // FFmpeg messages forwarded to our log per process

void FFLog(void* avcl, int level, const char* fmt, va_list vl) {
    if (level > AV_LOG_WARNING || XavcGetSettings().logLevel < 1) return;
    if (InterlockedDecrement(&g_logBudget) < 0) return;
    char line[512];
    int prefix = 1;
    va_list copy;
    va_copy(copy, vl);
    g_ff.log_format_line2(avcl, level, fmt, copy, line, sizeof(line), &prefix);
    va_end(copy);
    size_t n = strlen(line);
    while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
    XavcLog("ffmpeg: %s", line);
}

HMODULE LoadFromOurDir(const wchar_t* dir, const wchar_t* name) {
    wchar_t path[MAX_PATH];
    if (_snwprintf(path, MAX_PATH, L"%ls\\%ls", dir, name) < 0) return NULL;
    path[MAX_PATH - 1] = 0;
    // Dependencies (avcodec -> avutil, swresample) resolve from the same directory first.
    HMODULE h = LoadLibraryExW(path, NULL, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!h) XavcLog("FFmpeg: cannot load %ls (error %lu)", path, GetLastError());
    return h;
}

template <typename T>
bool Resolve(HMODULE m, const char* name, T* out) {
    *out = reinterpret_cast<T>(reinterpret_cast<void*>(GetProcAddress(m, name)));
    if (!*out) XavcLog("FFmpeg: missing export %s", name);
    return *out != NULL;
}

BOOL CALLBACK LoadOnce(PINIT_ONCE, PVOID, PVOID*) {
    wchar_t dir[MAX_PATH];
    DWORD n = GetModuleFileNameW(g_hInst, dir, MAX_PATH);
    if (!n || n >= MAX_PATH) return TRUE;
    wchar_t* slash = wcsrchr(dir, L'\\');
    if (!slash) return TRUE;
    *slash = 0;

    // Our minimal FFmpeg build (build/ffmpeg-minimal.sh) uses --build-suffix=-xavc, so its DLLs
    // cannot be confused with another FFmpeg the host process may have loaded.
#if XAVC_FFMPEG_SUFFIXED
#define XAVC_FF_SFX L"-xavc"
#else
#define XAVC_FF_SFX L""
#endif
    HMODULE u = LoadFromOurDir(dir, L"avutil" XAVC_FF_SFX L"-" XAVC_WSTR(LIBAVUTIL_VERSION_MAJOR) L".dll");
    HMODULE c = u ? LoadFromOurDir(dir, L"avcodec" XAVC_FF_SFX L"-" XAVC_WSTR(LIBAVCODEC_VERSION_MAJOR) L".dll") : NULL;
    if (!u || !c) return TRUE;

    bool ok = true;
    ok &= Resolve(u, "av_frame_alloc", &g_ff.frame_alloc);
    ok &= Resolve(u, "av_frame_free", &g_ff.frame_free);
    ok &= Resolve(u, "av_frame_unref", &g_ff.frame_unref);
    ok &= Resolve(u, "av_frame_apply_cropping", &g_ff.frame_apply_cropping);
    ok &= Resolve(u, "av_mallocz", &g_ff.mallocz);
    ok &= Resolve(u, "av_log_set_callback", &g_ff.log_set_callback);
    ok &= Resolve(u, "av_log_set_level", &g_ff.log_set_level);
    ok &= Resolve(u, "av_log_format_line2", &g_ff.log_format_line2);
    ok &= Resolve(u, "av_hwdevice_ctx_create", &g_ff.hwdevice_ctx_create);
    ok &= Resolve(u, "av_hwframe_transfer_data", &g_ff.hwframe_transfer_data);
    ok &= Resolve(u, "av_buffer_ref", &g_ff.buffer_ref);
    ok &= Resolve(u, "av_buffer_unref", &g_ff.buffer_unref);
    ok &= Resolve(u, "av_pix_fmt_desc_get", &g_ff.pix_fmt_desc_get);
    ok &= Resolve(u, "av_get_pix_fmt_name", &g_ff.get_pix_fmt_name);
    ok &= Resolve(u, "av_version_info", &g_ff.version_info);
    ok &= Resolve(u, "av_strerror", &g_ff.strerror_);
    ok &= Resolve(c, "avcodec_find_decoder", &g_ff.find_decoder);
    ok &= Resolve(c, "avcodec_alloc_context3", &g_ff.alloc_context3);
    ok &= Resolve(c, "avcodec_free_context", &g_ff.free_context);
    ok &= Resolve(c, "avcodec_open2", &g_ff.open2);
    ok &= Resolve(c, "avcodec_send_packet", &g_ff.send_packet);
    ok &= Resolve(c, "avcodec_receive_frame", &g_ff.receive_frame);
    ok &= Resolve(c, "avcodec_flush_buffers", &g_ff.flush_buffers);
    ok &= Resolve(c, "avcodec_profile_name", &g_ff.profile_name);
    ok &= Resolve(c, "av_packet_alloc", &g_ff.packet_alloc);
    ok &= Resolve(c, "av_packet_free", &g_ff.packet_free);
    ok &= Resolve(c, "av_packet_unref", &g_ff.packet_unref);
    ok &= Resolve(c, "av_new_packet", &g_ff.new_packet);
    // Never FreeLibrary: other modules' threads may still be inside them at process exit,
    // and keeping them costs nothing once a decoder has been used in this process.
    InterlockedExchange(&g_loaded, 1);
    if (!ok) return TRUE;

    g_ff.log_set_level(AV_LOG_WARNING);
    g_ff.log_set_callback(FFLog);
    g_ok = true;
    XavcLog("FFmpeg %s loaded from %ls", g_ff.version_info(), dir);
    return TRUE;
}

} // namespace

const XavcFF* XavcLoadFFmpeg() {
    InitOnceExecuteOnce(&g_once, LoadOnce, NULL, NULL);
    return g_ok ? &g_ff : NULL;
}

bool XavcFFmpegLoaded() { return g_loaded != 0; }

XavcErrText XavcAvErr(int err) {
    XavcErrText t;
    if (!g_ok || g_ff.strerror_(err, t.s, sizeof(t.s)) < 0) _snprintf(t.s, sizeof(t.s), "error %d", err);
    t.s[sizeof(t.s) - 1] = 0;
    return t;
}
