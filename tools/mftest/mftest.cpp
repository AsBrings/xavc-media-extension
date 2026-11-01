// mftest.cpp - Media Foundation test harness for the XAVC decoder MFT.
//
//   mftest enum                      list the H.264 video decoders MFTEnumEx returns
//                                    (several flag sets, incl. the ones sandboxed hosts use)
//   mftest read  <file> [rgb|d3d]    decode through IMFSourceReader (NV12 output; "rgb" = RGB32
//                                    with video processing like the thumbnail extractor;
//                                    "d3d" = with a D3D11 device manager / DXVA like the Media Engine)
//   mftest decode <file> <NV12|P010|Y210|YUY2> [n] [first.bmp|-] [seek_seconds]
//                                    decode with that output format, print per-frame times and
//                                    luma hashes (compare with "avtest <file> hash")
//   mftest samples <file> [n]        the compressed samples the source delivers
//   mftest thumb <file> <out.bmp>    Explorer thumbnail via IShellItemImageFactory
//   mftest thumbforce <file> [bmp]   re-extract it, replacing the cached one (IThumbnailCache)
//   mftest open  <aumid> <file>      activate a packaged app for a file (like double-click);
//                                    prints the app's process id
//
// Every command prints this process's pid so results can be matched against
// C:\ProgramData\XavcDecoder\xavc.log.
//
// Build: build\build-win.ps1

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <shobjidl.h>
#include <shlobj.h>
#include <thumbcache.h>
#include <d3d11.h>
#include <d3d10.h>
#include <dxgi.h>

#define FOURCC_GUID(fcc) { fcc, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } }
static const GUID kFmtH264  = FOURCC_GUID(0x34363248);   // 'H264'
static const GUID kFmtNV12  = FOURCC_GUID(0x3231564e);   // 'NV12'
static const GUID kFmtRGB32 = FOURCC_GUID(0x00000016);   // D3DFMT_X8R8G8B8
static const GUID kCatVideoDecoder =
    { 0xd6c02d4b, 0x6833, 0x45b4, { 0x97, 0x1a, 0x05, 0xa4, 0xb0, 0x4b, 0xab, 0x91 } };
static const GUID kClsidXavc =
    { 0x0b1e6d2a, 0x7c3f, 0x4a58, { 0x9e, 0x21, 0x6f, 0x4d, 0x8c, 0x0a, 0x3b, 0x71 } };
static const GUID kClsidXavcOld =
    { 0x7f9a3c21, 0x5e44, 0x4b0a, { 0x9c, 0x1d, 0x2a, 0x6b, 0x8e, 0x3d, 0x4f, 0x10 } };
static const CLSID kClsidLocalThumbnailCache =
    { 0x50ef4544, 0xac9f, 0x4a8e, { 0xb2, 0x1b, 0x8a, 0x26, 0x18, 0x0d, 0xb1, 0x3f } };
static const CLSID kClsidAppActivationMgr =
    { 0x45ba127d, 0x10a8, 0x46ea, { 0x8a, 0xb7, 0x56, 0xea, 0x90, 0x78, 0x94, 0x3c } };
static const IID kIidAppActivationMgr =
    { 0x2e941141, 0x7f97, 0x4756, { 0xba, 0x1d, 0x9d, 0xec, 0xde, 0x89, 0x4a, 0x3d } };
static const IID kIidD3D10Multithread =
    { 0x9b7e4e00, 0x342c, 0x4106, { 0xa1, 0x9f, 0x4f, 0x27, 0x04, 0xf6, 0x89, 0xf0 } };
// QueryInterface for this succeeds only on the XAVC MFT (see src/xavc_guids.h).
static const IID kIidXavcMarker =
    { 0x7f9a3c22, 0x5e44, 0x4b0a, { 0x9c, 0x1d, 0x2a, 0x6b, 0x8e, 0x3d, 0x4f, 0x10 } };

static const char* U8(const wchar_t* w) {
    static char buf[4][1024];
    static int slot = 0;
    char* b = buf[slot++ & 3];
    if (!w) return "(null)";
    WideCharToMultiByte(CP_UTF8, 0, w, -1, b, 1024, NULL, NULL);
    return b;
}

static const char* GuidStr(const GUID& g) {
    static char buf[4][64];
    static int slot = 0;
    char* b = buf[slot++ & 3];
    snprintf(b, 64, "{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
             (unsigned long)g.Data1, g.Data2, g.Data3, g.Data4[0], g.Data4[1], g.Data4[2],
             g.Data4[3], g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
    return b;
}

static bool IsFourcc(const GUID& g, char out[5]) {
    static const BYTE tail[8] = { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 };
    if (g.Data2 != 0 || g.Data3 != 0x10 || memcmp(g.Data4, tail, 8)) return false;
    DWORD f = g.Data1;
    for (int i = 0; i < 4; i++) {
        char c = (char)((f >> (8 * i)) & 0xff);
        out[i] = (c >= 32 && c < 127) ? c : '.';
    }
    out[4] = 0;
    return true;
}

struct KeyName { const GUID* key; const char* name; };
static const KeyName kKeys[] = {
    { &MF_MT_MAJOR_TYPE, "MAJOR_TYPE" },             { &MF_MT_SUBTYPE, "SUBTYPE" },
    { &MF_MT_FRAME_SIZE, "FRAME_SIZE" },             { &MF_MT_FRAME_RATE, "FRAME_RATE" },
    { &MF_MT_PIXEL_ASPECT_RATIO, "PIXEL_ASPECT" },   { &MF_MT_INTERLACE_MODE, "INTERLACE" },
    { &MF_MT_MPEG_SEQUENCE_HEADER, "MPEG_SEQ_HDR" }, { &MF_MT_MPEG2_PROFILE, "MPEG2_PROFILE" },
    { &MF_MT_MPEG2_LEVEL, "MPEG2_LEVEL" },           { &MF_MT_AVG_BITRATE, "AVG_BITRATE" },
    { &MF_MT_USER_DATA, "USER_DATA" },               { &MF_MT_VIDEO_PRIMARIES, "PRIMARIES" },
    { &MF_MT_TRANSFER_FUNCTION, "TRANSFER" },        { &MF_MT_YUV_MATRIX, "YUV_MATRIX" },
    { &MF_MT_VIDEO_NOMINAL_RANGE, "NOMINAL_RANGE" }, { &MF_MT_DEFAULT_STRIDE, "STRIDE" },
    { &MF_MT_SAMPLE_SIZE, "SAMPLE_SIZE" },           { &MF_MT_ALL_SAMPLES_INDEPENDENT, "ALL_INDEPENDENT" },
    { &MF_MT_FIXED_SIZE_SAMPLES, "FIXED_SIZE" },     { &MF_MT_COMPRESSED, "COMPRESSED" },
};

static void DumpType(const char* label, IMFMediaType* t) {
    UINT32 n = 0;
    t->GetCount(&n);
    printf("%s (%u attributes)\n", label, n);
    for (UINT32 i = 0; i < n; i++) {
        GUID key;
        PROPVARIANT v;
        PropVariantInit(&v);
        if (FAILED(t->GetItemByIndex(i, &key, &v))) continue;
        const char* name = NULL;
        for (const KeyName& k : kKeys) if (IsEqualGUID(key, *k.key)) name = k.name;
        printf("    %-16s ", name ? name : GuidStr(key));
        char fcc[5];
        switch (v.vt) {
        case VT_UI4:  printf("%lu\n", (unsigned long)v.ulVal); break;
        case VT_UI8:  printf("%u x %u\n", (unsigned)(v.uhVal.QuadPart >> 32), (unsigned)(v.uhVal.QuadPart & 0xffffffff)); break;
        case VT_CLSID:
            if (IsFourcc(*v.puuid, fcc)) printf("'%s'\n", fcc); else printf("%s\n", GuidStr(*v.puuid));
            break;
        case VT_VECTOR | VT_UI1: {
            printf("blob %lu bytes:", (unsigned long)v.caub.cElems);
            for (ULONG b = 0; b < v.caub.cElems && b < 24; b++) printf(" %02X", v.caub.pElems[b]);
            printf("%s\n", v.caub.cElems > 24 ? " ..." : "");
            break;
        }
        case VT_LPWSTR: printf("\"%s\"\n", U8(v.pwszVal)); break;
        default: printf("(vt=%u)\n", v.vt);
        }
        PropVariantClear(&v);
    }
}

// ---------------------------------------------------------------- enum
static int CmdEnum(const wchar_t* fourcc) {
    MFT_REGISTER_TYPE_INFO in = { MFMediaType_Video, kFmtH264 };
    if (fourcc && wcslen(fourcc) == 4) {
        in.guidSubtype.Data1 = (DWORD)(fourcc[0] | (fourcc[1] << 8) | (fourcc[2] << 16) | (fourcc[3] << 24));
        char f[5]; IsFourcc(in.guidSubtype, f);
        printf("enumerating video decoders for '%s'\n", f);
    }
    struct { const char* name; UINT32 flags; } sets[] = {
        { "SYNC|ASYNC|HW|SORTANDFILTER (typical desktop)", 0x1 | 0x2 | 0x4 | 0x40 },
        { "ALL (unsorted)", 0x3f },
        { "SORTANDFILTER_APPROVED_ONLY", 0x1 | 0x2 | 0x4 | 0xC0 },
        { "SORTANDFILTER_WEB_ONLY", 0x1 | 0x2 | 0x4 | 0x140 },
        { "SORTANDFILTER_WEB_ONLY_EDGEMODE", 0x1 | 0x2 | 0x4 | 0x240 },
        { "ALL|UNTRUSTED_STOREMFT", 0x3f | 0x400 },
    };
    for (auto& s : sets) {
        IMFActivate** acts = NULL;
        UINT32 n = 0;
        HRESULT hr = MFTEnumEx(kCatVideoDecoder, s.flags, &in, NULL, &acts, &n);
        printf("[%s] flags=0x%X hr=0x%08lX count=%u\n", s.name, s.flags, (unsigned long)hr, n);
        for (UINT32 i = 0; i < n; i++) {
            WCHAR* fname = NULL;
            UINT32 len = 0;
            acts[i]->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &fname, &len);
            GUID clsid = GUID_NULL;
            acts[i]->GetGUID(MFT_TRANSFORM_CLSID_Attribute, &clsid);
            UINT32 merit = 0, tflags = 0;
            acts[i]->GetUINT32(MFT_CODEC_MERIT_Attribute, &merit);
            acts[i]->GetUINT32(MF_TRANSFORM_FLAGS_Attribute, &tflags);
            printf("   %u. %-45s %s merit=%u flags=0x%X%s\n", i, U8(fname), GuidStr(clsid), merit, tflags,
                   IsEqualGUID(clsid, kClsidXavc) ? "   <== XAVC" :
                   IsEqualGUID(clsid, kClsidXavcOld) ? "   <== XAVC (old CLSID)" : "");
            CoTaskMemFree(fname);
            acts[i]->Release();
        }
        CoTaskMemFree(acts);
    }
    return 0;
}

// ---------------------------------------------------------------- read
// d3d: give the reader a D3D11 device manager (DXVA decoding), like the Media Engine does.
static int CmdRead(const wchar_t* path, bool rgb, bool d3d) {
    IMFAttributes* attr = NULL;
    MFCreateAttributes(&attr, 3);
    attr->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
    if (rgb) attr->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);

    ID3D11Device* dev = NULL;
    IMFDXGIDeviceManager* mgr = NULL;
    if (d3d) {
        HRESULT h2 = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL,
                                       D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                       NULL, 0, D3D11_SDK_VERSION, &dev, NULL, NULL);
        printf("D3D11CreateDevice hr=0x%08lX\n", (unsigned long)h2);
        if (SUCCEEDED(h2)) {
            ID3D10Multithread* mt = NULL;
            if (SUCCEEDED(dev->QueryInterface(kIidD3D10Multithread, (void**)&mt))) {
                mt->SetMultithreadProtected(TRUE);
                mt->Release();
            }
            UINT token = 0;
            if (SUCCEEDED(MFCreateDXGIDeviceManager(&token, &mgr))) {
                mgr->ResetDevice(dev, token);
                attr->SetUnknown(MF_SOURCE_READER_D3D_MANAGER, mgr);
            }
        }
    }

    IMFSourceReader* rd = NULL;
    HRESULT hr = MFCreateSourceReaderFromURL(path, attr, &rd);
    attr->Release();
    printf("MFCreateSourceReaderFromURL hr=0x%08lX\n", (unsigned long)hr);
    if (FAILED(hr)) return 1;

    const DWORD VID = (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM;
    rd->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
    rd->SetStreamSelection(VID, TRUE);

    IMFMediaType* nt = NULL;
    if (SUCCEEDED(rd->GetNativeMediaType(VID, 0, &nt))) { DumpType("native type", nt); nt->Release(); }

    IMFMediaType* ot = NULL;
    MFCreateMediaType(&ot);
    ot->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    ot->SetGUID(MF_MT_SUBTYPE, rgb ? kFmtRGB32 : kFmtNV12);
    hr = rd->SetCurrentMediaType(VID, NULL, ot);
    ot->Release();
    printf("SetCurrentMediaType(%s) hr=0x%08lX%s\n", rgb ? "RGB32" : "NV12", (unsigned long)hr,
           hr == (HRESULT)0xC00D5212 ? "  (MF_E_TOPO_CODEC_NOT_FOUND = 'unsupported codec')" : "");
    if (FAILED(hr)) { rd->Release(); return 2; }

    UINT32 w = 0, h = 0;
    IMFMediaType* ct = NULL;
    if (SUCCEEDED(rd->GetCurrentMediaType(VID, &ct))) {
        MFGetAttributeSize(ct, MF_MT_FRAME_SIZE, &w, &h);
        ct->Release();
    }

    int frames = 0;
    for (int k = 0; k < 5; k++) {
        DWORD si = 0, fl = 0;
        LONGLONG ts = 0;
        IMFSample* s = NULL;
        hr = rd->ReadSample(VID, 0, &si, &fl, &ts, &s);
        printf("ReadSample #%d hr=0x%08lX flags=0x%lX t=%.3fs", k, (unsigned long)hr, (unsigned long)fl, ts / 1e7);
        if (s) {
            IMFMediaBuffer* b = NULL;
            if (SUCCEEDED(s->ConvertToContiguousBuffer(&b))) {
                BYTE* p = NULL;
                DWORD mx = 0, cur = 0;
                if (SUCCEEDED(b->Lock(&p, &mx, &cur))) {
                    if (!rgb && w && h && cur >= w * h * 3 / 2) {
                        BYTE y = p[(size_t)(h / 2) * w + w / 2];
                        BYTE* uv = p + (size_t)w * h + (size_t)(h / 4) * w + (w / 2 & ~1u);
                        printf("  %ux%u center Y=%u U=%u V=%u%s", w, h, y, uv[0], uv[1],
                               (y == 105 && uv[0] == 212 && uv[1] == 234) ? "  [MAGENTA = XAVC diag decoder]" : "");
                    } else if (rgb && w && h && cur >= w * h * 4) {
                        BYTE* px = p + ((size_t)(h / 2) * w + w / 2) * 4;
                        printf("  %ux%u center B=%u G=%u R=%u", w, h, px[0], px[1], px[2]);
                    } else {
                        printf("  len=%lu", (unsigned long)cur);
                    }
                    b->Unlock();
                }
                b->Release();
            }
            s->Release();
            frames++;
        }
        printf("\n");
        if (FAILED(hr) || (fl & MF_SOURCE_READERF_ENDOFSTREAM) || (fl & MF_SOURCE_READERF_ERROR)) break;
    }

    IMFSourceReaderEx* rx = NULL;
    if (SUCCEEDED(rd->QueryInterface(IID_IMFSourceReaderEx, (void**)&rx))) {
        for (DWORD i = 0;; i++) {
            GUID cat;
            IMFTransform* t = NULL;
            if (FAILED(rx->GetTransformForStream(VID, i, &cat, &t))) break;
            IUnknown* mark = NULL;
            bool ours = SUCCEEDED(t->QueryInterface(kIidXavcMarker, (void**)&mark));
            if (mark) mark->Release();
            printf("pipeline transform %lu: category %s%s -> %s\n", (unsigned long)i, GuidStr(cat),
                   IsEqualGUID(cat, kCatVideoDecoder) ? " (video decoder)" : "",
                   ours ? "XAVC decoder" : "not ours");
            t->Release();
        }
        rx->Release();
    }
    rd->Release();
    printf("decoded frames: %d\n", frames);
    return frames > 0 ? 0 : 3;
}

// ---------------------------------------------------------------- decode
static void SaveBmp(const wchar_t* out, const BYTE* px, int w, int h);

// mftest decode <file> <NV12|P010|Y210|YUY2> [n] [first.bmp]
// Decodes through IMFSourceReader with the given decoder output format and prints, per frame,
// the time and the same luma hashes as "avtest <file> hash" (h10 for 10-bit output, h8 for
// 8-bit), so the MF pipeline can be compared frame by frame with plain libavcodec.

static const GUID kFmtP010 = FOURCC_GUID(0x30313050);
static const GUID kFmtY210 = FOURCC_GUID(0x30313259);
static const GUID kFmtYUY2 = FOURCC_GUID(0x32595559);

// luma sample i of a row in each format, as a 16-bit MSB-aligned value
static inline unsigned LumaAt(const BYTE* row, int x, int fmt) {
    switch (fmt) {
    case 0: return row[x] << 8;                                   // NV12
    case 1: return ((const uint16_t*)row)[x];                     // P010
    case 2: return ((const uint16_t*)row)[(x & ~1) * 2 + (x & 1) * 2];   // Y210: Y0 U Y1 V
    default: return row[(x & ~1) * 2 + (x & 1) * 2] << 8;         // YUY2: Y0 U Y1 V
    }
}
static inline void ChromaAt(const BYTE* p, LONG pitch, int w, int h, int x, int y, int fmt, unsigned* u, unsigned* v) {
    if (fmt <= 1) {                                               // NV12 / P010: UV plane after Y
        const BYTE* uvRow = p + (size_t)pitch * h + (size_t)pitch * (y / 2);
        if (fmt == 0) { *u = uvRow[(x & ~1)] << 8; *v = uvRow[(x & ~1) + 1] << 8; }
        else { *u = ((const uint16_t*)uvRow)[x & ~1]; *v = ((const uint16_t*)uvRow)[(x & ~1) + 1]; }
    } else {
        const BYTE* row = p + (size_t)pitch * y;
        if (fmt == 2) { *u = ((const uint16_t*)row)[(x & ~1) * 2 + 1]; *v = ((const uint16_t*)row)[(x & ~1) * 2 + 3]; }
        else { *u = row[(x & ~1) * 2 + 1] << 8; *v = row[(x & ~1) * 2 + 3] << 8; }
    }
    (void)w;
}

// BT.709 limited range -> BGRX, for a quick look at the picture.
static void SaveFrameBmp(const wchar_t* out, const BYTE* p, LONG pitch, int w, int h, int fmt) {
    BYTE* px = (BYTE*)malloc((size_t)w * h * 4);
    if (!px) return;
    for (int y = 0; y < h; y++) {
        const BYTE* row = p + (size_t)pitch * y;
        for (int x = 0; x < w; x++) {
            double Y = LumaAt(row, x, fmt) / 65535.0 * 255.0;
            unsigned uu, vv;
            ChromaAt(p, pitch, w, h, x, y, fmt, &uu, &vv);
            double U = uu / 65535.0 * 255.0 - 128.0, V = vv / 65535.0 * 255.0 - 128.0;
            double c = (Y - 16.0) * 255.0 / 219.0;
            double r = c + 1.5748 * 255.0 / 224.0 * V;
            double g = c - 0.1873 * 255.0 / 224.0 * U - 0.4681 * 255.0 / 224.0 * V;
            double b = c + 1.8556 * 255.0 / 224.0 * U;
            BYTE* d = px + ((size_t)y * w + x) * 4;
            d[0] = (BYTE)(b < 0 ? 0 : b > 255 ? 255 : b);
            d[1] = (BYTE)(g < 0 ? 0 : g > 255 ? 255 : g);
            d[2] = (BYTE)(r < 0 ? 0 : r > 255 ? 255 : r);
            d[3] = 0;
        }
    }
    SaveBmp(out, px, w, h);
    free(px);
}

static int CmdDecode(const wchar_t* path, const wchar_t* fmtName, int count, const wchar_t* bmp, double seekTo) {
    int fmt = -1;
    const GUID* sub = NULL;
    if (!_wcsicmp(fmtName, L"NV12")) { fmt = 0; sub = &kFmtNV12; }
    else if (!_wcsicmp(fmtName, L"P010")) { fmt = 1; sub = &kFmtP010; }
    else if (!_wcsicmp(fmtName, L"Y210")) { fmt = 2; sub = &kFmtY210; }
    else if (!_wcsicmp(fmtName, L"YUY2")) { fmt = 3; sub = &kFmtYUY2; }
    if (!sub) { printf("format must be NV12, P010, Y210 or YUY2\n"); return 64; }

    IMFSourceReader* rd = NULL;
    HRESULT hr = MFCreateSourceReaderFromURL(path, NULL, &rd);
    if (FAILED(hr)) { printf("MFCreateSourceReaderFromURL hr=0x%08lX\n", (unsigned long)hr); return 1; }
    const DWORD VID = (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM;
    rd->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
    rd->SetStreamSelection(VID, TRUE);
    IMFMediaType* ot = NULL;
    MFCreateMediaType(&ot);
    ot->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    ot->SetGUID(MF_MT_SUBTYPE, *sub);
    hr = rd->SetCurrentMediaType(VID, NULL, ot);
    ot->Release();
    printf("SetCurrentMediaType(%ls) hr=0x%08lX\n", fmtName, (unsigned long)hr);
    if (FAILED(hr)) { rd->Release(); return 2; }

    UINT32 w = 0, h = 0, stride = 0;
    IMFMediaType* ct = NULL;
    if (SUCCEEDED(rd->GetCurrentMediaType(VID, &ct))) {
        MFGetAttributeSize(ct, MF_MT_FRAME_SIZE, &w, &h);
        ct->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride);
        ct->Release();
    }
    printf("output %ux%u default stride %u\n", w, h, stride);
    if (seekTo > 0) {
        PROPVARIANT pos;
        PropVariantInit(&pos);
        pos.vt = VT_I8;
        pos.hVal.QuadPart = (LONGLONG)(seekTo * 1e7);
        hr = rd->SetCurrentPosition(GUID_NULL, pos);
        printf("SetCurrentPosition(%.3f s) hr=0x%08lX\n", seekTo, (unsigned long)hr);
    }

    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    int frames = 0;
    bool firstTimeShown = false;
    double firstFrameAt = 0;
    while (frames < count) {
        DWORD si = 0, fl = 0;
        LONGLONG ts = 0;
        IMFSample* s = NULL;
        hr = rd->ReadSample(VID, 0, &si, &fl, &ts, &s);
        if (FAILED(hr)) { printf("ReadSample hr=0x%08lX\n", (unsigned long)hr); break; }
        if (fl & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) printf("(media type changed)\n");
        if (!s) {
            if (fl & (MF_SOURCE_READERF_ENDOFSTREAM | MF_SOURCE_READERF_ERROR)) { printf("end of stream (flags 0x%lX)\n", (unsigned long)fl); break; }
            continue;
        }
        if (!firstTimeShown) {
            QueryPerformanceCounter(&t1);
            firstFrameAt = (double)(t1.QuadPart - t0.QuadPart) / f.QuadPart;
            firstTimeShown = true;
        }
        IMFMediaBuffer* b = NULL;
        s->ConvertToContiguousBuffer(&b);
        IMF2DBuffer* b2 = NULL;
        BYTE* p = NULL;
        LONG pitch = (LONG)stride;
        DWORD mx = 0, cur = 0;
        if (b && SUCCEEDED(b->QueryInterface(IID_IMF2DBuffer, (void**)&b2))) b2->Lock2D(&p, &pitch);
        else if (b) b->Lock(&p, &mx, &cur);
        unsigned long long hh = 1469598103934665603ULL;
        const unsigned long long P = 1099511628211ULL;
        if (p) {
            for (UINT32 y = 0; y < h; y++) {
                const BYTE* row = p + (size_t)pitch * y;
                for (UINT32 x = 0; x < w; x++) {
                    unsigned v16 = LumaAt(row, (int)x, fmt);
                    if (fmt == 1 || fmt == 2) { unsigned v10 = v16 >> 6; hh = (hh ^ (v10 & 0xff)) * P; hh = (hh ^ (v10 >> 8)) * P; }
                    else hh = (hh ^ (v16 >> 8)) * P;
                }
            }
            if (frames == 0 && bmp) SaveFrameBmp(bmp, p, pitch, (int)w, (int)h, fmt);
        }
        LONGLONG dur = 0;
        s->GetSampleDuration(&dur);
        printf("%5d t=%lld dur=%lld %s=%016llx%s\n", frames, ts, dur, (fmt == 1 || fmt == 2) ? "h10" : "h8", hh,
               b2 ? " (2D)" : "");
        if (b2) { b2->Unlock2D(); b2->Release(); } else if (b && p) b->Unlock();
        if (b) b->Release();
        s->Release();
        frames++;
    }
    QueryPerformanceCounter(&t1);
    double secs = (double)(t1.QuadPart - t0.QuadPart) / f.QuadPart;
    printf("decoded %d frames in %.2f s (first after %.2f s)\n", frames, secs, firstFrameAt);

    IMFSourceReaderEx* rx = NULL;
    if (SUCCEEDED(rd->QueryInterface(IID_IMFSourceReaderEx, (void**)&rx))) {
        for (DWORD i = 0;; i++) {
            GUID cat;
            IMFTransform* t = NULL;
            if (FAILED(rx->GetTransformForStream(VID, i, &cat, &t))) break;
            IUnknown* mark = NULL;
            bool ours = SUCCEEDED(t->QueryInterface(kIidXavcMarker, (void**)&mark));
            if (mark) mark->Release();
            printf("pipeline transform %lu: %s\n", (unsigned long)i, ours ? "XAVC decoder" : "other");
            t->Release();
        }
        rx->Release();
    }
    rd->Release();
    return frames > 0 ? 0 : 3;
}

// ---------------------------------------------------------------- vpcaps
// Which decoder output formats each GPU's D3D11 video processor accepts as input (the Media
// Player renders through it, so this decides whether 4:2:2 Y210 output can be used).
static int CmdVpCaps() {
    IDXGIFactory1* fac = NULL;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&fac))) { printf("CreateDXGIFactory1 failed\n"); return 1; }
    struct { DXGI_FORMAT f; const char* n; } fmts[] = {
        { DXGI_FORMAT_NV12, "NV12" }, { DXGI_FORMAT_P010, "P010" }, { DXGI_FORMAT_YUY2, "YUY2" },
        { DXGI_FORMAT_Y210, "Y210" }, { DXGI_FORMAT_Y216, "Y216" }, { DXGI_FORMAT_P016, "P016" },
        { DXGI_FORMAT_AYUV, "AYUV" }, { DXGI_FORMAT_Y410, "Y410" } };
    IDXGIAdapter1* ad = NULL;
    for (UINT i = 0; fac->EnumAdapters1(i, &ad) != DXGI_ERROR_NOT_FOUND; i++) {
        DXGI_ADAPTER_DESC1 d;
        ad->GetDesc1(&d);
        printf("adapter %u: %s (vendor 0x%04X)\n", i, U8(d.Description), d.VendorId);
        ID3D11Device* dev = NULL;
        ID3D11DeviceContext* ctx = NULL;
        HRESULT hr = D3D11CreateDevice(ad, D3D_DRIVER_TYPE_UNKNOWN, NULL, D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
                                       NULL, 0, D3D11_SDK_VERSION, &dev, NULL, &ctx);
        ID3D11VideoDevice* vd = NULL;
        if (SUCCEEDED(hr)) hr = dev->QueryInterface(__uuidof(ID3D11VideoDevice), (void**)&vd);
        if (FAILED(hr)) { printf("   no D3D11 video device (0x%08lX)\n", (unsigned long)hr); }
        else {
            D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd = {};
            cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
            cd.InputWidth = 3840; cd.InputHeight = 2160; cd.OutputWidth = 1920; cd.OutputHeight = 1080;
            cd.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
            ID3D11VideoProcessorEnumerator* en = NULL;
            hr = vd->CreateVideoProcessorEnumerator(&cd, &en);
            if (FAILED(hr)) printf("   CreateVideoProcessorEnumerator 0x%08lX\n", (unsigned long)hr);
            else {
                printf("   video processor input:");
                for (auto& f : fmts) {
                    UINT flags = 0;
                    en->CheckVideoProcessorFormat(f.f, &flags);
                    printf(" %s=%s", f.n, (flags & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT) ? "yes" : "no");
                }
                printf("\n");
                en->Release();
            }
            vd->Release();
        }
        if (ctx) ctx->Release();
        if (dev) dev->Release();
        ad->Release();
    }
    fac->Release();
    return 0;
}

// ---------------------------------------------------------------- samples
// Print the compressed samples the media source delivers (no decoder): timestamps,
// flags and the first bytes, i.e. exactly what a decoder MFT gets in ProcessInput.
static int CmdSamples(const wchar_t* path, int count) {
    IMFSourceReader* rd = NULL;
    HRESULT hr = MFCreateSourceReaderFromURL(path, NULL, &rd);
    printf("MFCreateSourceReaderFromURL hr=0x%08lX\n", (unsigned long)hr);
    if (FAILED(hr)) return 1;
    const DWORD VID = (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM;
    rd->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
    rd->SetStreamSelection(VID, TRUE);
    IMFMediaType* nt = NULL;
    if (SUCCEEDED(rd->GetNativeMediaType(VID, 0, &nt))) {
        DumpType("native type", nt);
        rd->SetCurrentMediaType(VID, NULL, nt);   // compressed passthrough
        nt->Release();
    }
    for (int k = 0; k < count; k++) {
        DWORD si = 0, fl = 0;
        LONGLONG ts = 0;
        IMFSample* s = NULL;
        hr = rd->ReadSample(VID, 0, &si, &fl, &ts, &s);
        if (FAILED(hr) || !s) {
            printf("ReadSample #%d hr=0x%08lX flags=0x%lX\n", k, (unsigned long)hr, (unsigned long)fl);
            if (FAILED(hr) || (fl & (MF_SOURCE_READERF_ENDOFSTREAM | MF_SOURCE_READERF_ERROR))) break;
            continue;
        }
        LONGLONG dur = -1, dts = -1;
        s->GetSampleDuration(&dur);
        UINT64 d64 = 0;
        if (SUCCEEDED(s->GetUINT64(MFSampleExtension_DecodeTimestamp, &d64))) dts = (LONGLONG)d64;
        UINT32 clean = 0, disc = 0;
        s->GetUINT32(MFSampleExtension_CleanPoint, &clean);
        s->GetUINT32(MFSampleExtension_Discontinuity, &disc);
        DWORD nbuf = 0;
        s->GetBufferCount(&nbuf);
        printf("#%-3d t=%9.4f dur=%7.4f dts=%9.4f clean=%u disc=%u bufs=%lu", k, ts / 1e7, dur / 1e7,
               dts >= 0 ? dts / 1e7 : -1.0, clean, disc, (unsigned long)nbuf);
        IMFMediaBuffer* b = NULL;
        if (SUCCEEDED(s->ConvertToContiguousBuffer(&b))) {
            BYTE* p = NULL;
            DWORD mx = 0, cur = 0;
            if (SUCCEEDED(b->Lock(&p, &mx, &cur))) {
                printf(" len=%lu:", (unsigned long)cur);
                for (DWORD i = 0; i < cur && i < 12; i++) printf(" %02X", p[i]);
                // list NAL unit types if this looks like Annex B
                if (cur > 4 && p[0] == 0 && p[1] == 0 && (p[2] == 1 || (p[2] == 0 && p[3] == 1))) {
                    printf("  NALs:");
                    int shown = 0;
                    for (DWORD i = 0; i + 3 < cur && shown < 12; i++) {
                        if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1) { printf(" %u", p[i + 3] & 0x1f); shown++; i += 2; }
                    }
                }
                b->Unlock();
            }
            b->Release();
        }
        printf("\n");
        s->Release();
    }
    rd->Release();
    return 0;
}

// ---------------------------------------------------------------- resolve
// Create the media source through IMFSourceResolver directly and print the video
// stream subtypes it exposes (our byte stream handler retags 4:2:2 H.264 as 'XAVC').
static void PrintSourceStreams(IMFMediaSource* src) {
    IMFPresentationDescriptor* pd = NULL;
    if (FAILED(src->CreatePresentationDescriptor(&pd))) return;
    DWORD n = 0;
    pd->GetStreamDescriptorCount(&n);
    for (DWORD i = 0; i < n; i++) {
        BOOL sel = FALSE;
        IMFStreamDescriptor* sd = NULL;
        if (FAILED(pd->GetStreamDescriptorByIndex(i, &sel, &sd))) continue;
        IMFMediaTypeHandler* mth = NULL;
        IMFMediaType* cur = NULL;
        if (SUCCEEDED(sd->GetMediaTypeHandler(&mth)) && SUCCEEDED(mth->GetCurrentMediaType(&cur))) {
            GUID major = GUID_NULL, sub = GUID_NULL;
            cur->GetGUID(MF_MT_MAJOR_TYPE, &major);
            cur->GetGUID(MF_MT_SUBTYPE, &sub);
            char f[5] = "?";
            if (!IsFourcc(sub, f)) snprintf(f, sizeof(f), "%s", "guid");
            printf("   stream %lu: %s subtype '%s' %s\n", (unsigned long)i,
                   IsEqualGUID(major, MFMediaType_Video) ? "video" : "other", f, GuidStr(sub));
            // Would the source accept its own (possibly retagged) type back?
            IMFMediaType* closest = NULL;
            HRESULT hs = mth->IsMediaTypeSupported(cur, &closest);
            printf("      IsMediaTypeSupported(current) hr=0x%08lX\n", (unsigned long)hs);
            if (closest) closest->Release();
            hs = mth->SetCurrentMediaType(cur);
            printf("      SetCurrentMediaType(current) hr=0x%08lX\n", (unsigned long)hs);
        }
        if (cur) cur->Release();
        if (mth) mth->Release();
        sd->Release();
    }
    pd->Release();
}

static int CmdResolve(const wchar_t* path) {
    IMFSourceResolver* r = NULL;
    HRESULT hr = MFCreateSourceResolver(&r);
    if (FAILED(hr)) { printf("MFCreateSourceResolver hr=0x%08lX\n", (unsigned long)hr); return 1; }
    MF_OBJECT_TYPE type = MF_OBJECT_INVALID;
    IUnknown* obj = NULL;
    hr = r->CreateObjectFromURL(path, MF_RESOLUTION_MEDIASOURCE | MF_RESOLUTION_READ, NULL, &type, &obj);
    printf("CreateObjectFromURL hr=0x%08lX type=%d\n", (unsigned long)hr, (int)type);
    if (obj) {
        IMFMediaSource* src = NULL;
        if (SUCCEEDED(obj->QueryInterface(IID_IMFMediaSource, (void**)&src))) { PrintSourceStreams(src); src->Shutdown(); src->Release(); }
        obj->Release();
        obj = NULL;
    }
    IMFByteStream* bs = NULL;
    hr = MFCreateFile(MF_ACCESSMODE_READ, MF_OPENMODE_FAIL_IF_NOT_EXIST, MF_FILEFLAGS_NONE, path, &bs);
    if (SUCCEEDED(hr)) {
        hr = r->CreateObjectFromByteStream(bs, path, MF_RESOLUTION_MEDIASOURCE | MF_RESOLUTION_READ, NULL, &type, &obj);
        printf("CreateObjectFromByteStream hr=0x%08lX type=%d\n", (unsigned long)hr, (int)type);
        if (obj) {
            IMFMediaSource* src = NULL;
            if (SUCCEEDED(obj->QueryInterface(IID_IMFMediaSource, (void**)&src))) { PrintSourceStreams(src); src->Shutdown(); src->Release(); }
            obj->Release();
        }
        bs->Release();
    }
    r->Release();
    return 0;
}

// ---------------------------------------------------------------- thumb
static void SaveBmp(const wchar_t* out, const BYTE* px, int w, int h) {
    BITMAPFILEHEADER fh = {};
    BITMAPINFOHEADER ih = {};
    ih.biSize = sizeof(ih); ih.biWidth = w; ih.biHeight = -h; ih.biPlanes = 1; ih.biBitCount = 32;
    ih.biCompression = BI_RGB;
    fh.bfType = 0x4D42; fh.bfOffBits = sizeof(fh) + sizeof(ih); fh.bfSize = fh.bfOffBits + w * h * 4;
    HANDLE f = CreateFileW(out, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD wr;
    WriteFile(f, &fh, sizeof(fh), &wr, NULL);
    WriteFile(f, &ih, sizeof(ih), &wr, NULL);
    WriteFile(f, px, w * h * 4, &wr, NULL);
    CloseHandle(f);
}

// force: IThumbnailCache with WTS_FORCEEXTRACTION, i.e. extract again (through the thumbnail
// host process, as Explorer does) and replace whatever the cache holds for the file.
static int CmdThumb(const wchar_t* path, const wchar_t* out, bool force) {
    HRESULT hr;
    HBITMAP hbm = NULL;
    if (force) {
        IShellItem* item = NULL;
        hr = SHCreateItemFromParsingName(path, NULL, IID_PPV_ARGS(&item));
        if (FAILED(hr)) { printf("SHCreateItemFromParsingName hr=0x%08lX\n", (unsigned long)hr); return 1; }
        IThumbnailCache* tc = NULL;
        hr = CoCreateInstance(kClsidLocalThumbnailCache, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&tc));
        if (FAILED(hr)) { printf("CoCreateInstance(LocalThumbnailCache) hr=0x%08lX\n", (unsigned long)hr); item->Release(); return 1; }
        ISharedBitmap* sb = NULL;
        WTS_CACHEFLAGS cf = WTS_DEFAULT;
        hr = tc->GetThumbnail(item, 256, WTS_FORCEEXTRACTION, &sb, &cf, NULL);
        printf("IThumbnailCache::GetThumbnail(FORCEEXTRACTION) hr=0x%08lX cacheflags=0x%X\n", (unsigned long)hr, (unsigned)cf);
        if (sb) { sb->Detach(&hbm); sb->Release(); }
        tc->Release();
        item->Release();
    } else {
        IShellItemImageFactory* f = NULL;
        hr = SHCreateItemFromParsingName(path, NULL, IID_PPV_ARGS(&f));
        if (FAILED(hr)) { printf("SHCreateItemFromParsingName hr=0x%08lX\n", (unsigned long)hr); return 1; }
        SIZE sz = { 256, 256 };
        hr = f->GetImage(sz, SIIGBF_THUMBNAILONLY, &hbm);
        f->Release();
        printf("GetImage(THUMBNAILONLY) hr=0x%08lX\n", (unsigned long)hr);
    }
    if (FAILED(hr) || !hbm) return 2;
    BITMAP bm;
    GetObject(hbm, sizeof(bm), &bm);
    int w = bm.bmWidth, h = bm.bmHeight;
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader); bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    BYTE* px = (BYTE*)malloc((size_t)w * h * 4);
    HDC dc = GetDC(NULL);
    GetDIBits(dc, hbm, 0, h, px, &bi, DIB_RGB_COLORS);
    ReleaseDC(NULL, dc);
    DeleteObject(hbm);
    long magenta = 0;
    for (int i = 0; i < w * h; i++) {
        BYTE B = px[i * 4], G = px[i * 4 + 1], R = px[i * 4 + 2];
        if (R > 180 && B > 180 && G < 90) magenta++;
    }
    BYTE* c = px + ((size_t)(h / 2) * w + w / 2) * 4;
    printf("thumbnail %dx%d center B=%u G=%u R=%u, magenta pixels %.1f%%\n", w, h, c[0], c[1], c[2],
           100.0 * magenta / (w * h));
    if (out) SaveBmp(out, px, w, h);
    free(px);
    return 0;
}

// ---------------------------------------------------------------- open
static int CmdOpen(const wchar_t* aumid, const wchar_t* path) {
    IApplicationActivationManager* am = NULL;
    HRESULT hr = CoCreateInstance(kClsidAppActivationMgr, NULL, CLSCTX_LOCAL_SERVER, kIidAppActivationMgr, (void**)&am);
    if (FAILED(hr)) { printf("CoCreateInstance(ApplicationActivationManager) hr=0x%08lX\n", (unsigned long)hr); return 1; }
    IShellItem* si = NULL;
    IShellItemArray* arr = NULL;
    hr = SHCreateItemFromParsingName(path, NULL, IID_PPV_ARGS(&si));
    if (SUCCEEDED(hr)) hr = SHCreateShellItemArrayFromShellItem(si, IID_PPV_ARGS(&arr));
    if (FAILED(hr)) { printf("shell item hr=0x%08lX\n", (unsigned long)hr); return 2; }
    DWORD pid = 0;
    hr = am->ActivateForFile(aumid, arr, L"open", &pid);
    printf("ActivateForFile hr=0x%08lX pid=%lu\n", (unsigned long)hr, (unsigned long)pid);
    arr->Release();
    si->Release();
    am->Release();
    return SUCCEEDED(hr) ? 0 : 3;
}

int wmain(int argc, wchar_t** argv) {
    printf("mftest pid=%lu\n", (unsigned long)GetCurrentProcessId());
    if (argc < 2) {
        printf("usage: mftest enum [fourcc] | read <file> [rgb|d3d] | decode <file> <fmt> [n] [bmp] | samples <file> [n]\n"
               "       | resolve <file> | thumb <file> [bmp] | thumbforce <file> [bmp] | open <aumid> <file>\n");
        return 64;
    }
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    MFStartup(MF_VERSION, MFSTARTUP_FULL);
    int rc = 64;
    const wchar_t* cmd = argv[1];
    if (!wcscmp(cmd, L"enum")) rc = CmdEnum(argc >= 3 ? argv[2] : NULL);
    else if (!wcscmp(cmd, L"read") && argc >= 3) {
        const wchar_t* mode = argc >= 4 ? argv[3] : L"";
        rc = CmdRead(argv[2], !wcscmp(mode, L"rgb"), !wcscmp(mode, L"d3d"));
    }
    else if (!wcscmp(cmd, L"resolve") && argc >= 3) rc = CmdResolve(argv[2]);
    else if (!wcscmp(cmd, L"vpcaps")) rc = CmdVpCaps();
    else if (!wcscmp(cmd, L"samples") && argc >= 3) rc = CmdSamples(argv[2], argc >= 4 ? _wtoi(argv[3]) : 12);
    else if (!wcscmp(cmd, L"decode") && argc >= 4)
        rc = CmdDecode(argv[2], argv[3], argc >= 5 ? _wtoi(argv[4]) : 100,
                       argc >= 6 && wcscmp(argv[5], L"-") ? argv[5] : NULL, argc >= 7 ? _wtof(argv[6]) : 0.0);
    else if (!wcscmp(cmd, L"thumb") && argc >= 3) rc = CmdThumb(argv[2], argc >= 4 ? argv[3] : NULL, false);
    else if (!wcscmp(cmd, L"thumbforce") && argc >= 3) rc = CmdThumb(argv[2], argc >= 4 ? argv[3] : NULL, true);
    else if (!wcscmp(cmd, L"open") && argc >= 4) rc = CmdOpen(argv[2], argv[3]);
    else printf("bad arguments\n");
    MFShutdown();
    CoUninitialize();
    return rc;
}
