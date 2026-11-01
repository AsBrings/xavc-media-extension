// xavc_common.h - shared declarations for the XAVC Media Foundation components.
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <mfobjects.h>
#include <stdio.h>
#include <stdint.h>

// Our GUIDs (CLSID + FourCC media subtypes). Declared extern here; defined once
// in guids.cpp (which includes <initguid.h>). Keeps us from re-defining MF GUIDs
// that already live in mfuuid.a.
#include "xavc_guids.h"

// IMFByteStreamHandler (mfidl.h in the Windows SDK; missing from mingw-w64).
#ifndef __IMFByteStreamHandler_INTERFACE_DEFINED__
#define __IMFByteStreamHandler_INTERFACE_DEFINED__
struct IMFByteStreamHandler : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE BeginCreateObject(IMFByteStream* pByteStream, LPCWSTR pwszURL,
        DWORD dwFlags, IPropertyStore* pProps, IUnknown** ppIUnknownCancelCookie,
        IMFAsyncCallback* pCallback, IUnknown* punkState) = 0;
    virtual HRESULT STDMETHODCALLTYPE EndCreateObject(IMFAsyncResult* pResult,
        MF_OBJECT_TYPE* pObjectType, IUnknown** ppObject) = 0;
    virtual HRESULT STDMETHODCALLTYPE CancelObjectCreation(IUnknown* pIUnknownCancelCookie) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetMaxNumberOfBytesRequiredForResolution(QWORD* pqwBytes) = 0;
};
#endif

// IMFQualityAdvise / IMFQualityAdvise2 (mfidl.h in the Windows SDK; mingw-w64 only has the
// enums). The pipeline's quality manager uses them to ask a decoder to work less when
// playback falls behind.
#ifndef __IMFQualityAdvise_INTERFACE_DEFINED__
#define __IMFQualityAdvise_INTERFACE_DEFINED__
struct IMFQualityAdvise : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE SetDropMode(MF_QUALITY_DROP_MODE eDropMode) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetQualityLevel(MF_QUALITY_LEVEL eQualityLevel) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDropMode(MF_QUALITY_DROP_MODE* peDropMode) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetQualityLevel(MF_QUALITY_LEVEL* peQualityLevel) = 0;
    virtual HRESULT STDMETHODCALLTYPE DropTime(LONGLONG hnsAmountToDrop) = 0;
};
#endif
#ifndef __IMFQualityAdvise2_INTERFACE_DEFINED__
#define __IMFQualityAdvise2_INTERFACE_DEFINED__
struct IMFQualityAdvise2 : public IMFQualityAdvise {
    virtual HRESULT STDMETHODCALLTYPE NotifyQualityEvent(IMFMediaEvent* pEvent, DWORD* pdwFlags) = 0;
};
#endif

extern HMODULE g_hInst;            // this DLL (xavc_dll.cpp)
extern volatile LONG g_cObj;       // live COM objects (DllCanUnloadNow)

HRESULT XavcCreateByteStreamHandler(REFIID riid, void** ppv);   // xavc_bsh.cpp
HRESULT XavcCreateDecoder(REFIID riid, void** ppv);             // xavc_decoder.cpp

// ---- Settings (xavc_util.cpp) ----
// REG_DWORD values under HKCU\Software\XavcDecoder, then HKLM\SOFTWARE\XavcDecoder; read once
// per process. (Sandboxed hosts such as the Media Player can always read the HKLM key; the
// HKCU key only if ALL APPLICATION PACKAGES has been granted read access to it.)
struct XavcSettings {
    DWORD logLevel;      // LogLevel:       0 = off (default), 1 = info, 2 = trace every call
    DWORD hwDecode;      // HardwareDecode: 1 = use a GPU decoder when one supports the stream (default), 0 = never
    DWORD outputFormat;  // OutputFormat:   0 = auto: keep 4:2:2 (Y210/YUY2) first, then 4:2:0 fallbacks (default)
                         //                 1 = 4:2:0 only (P010 for 10-bit, NV12)   2 = NV12 only
    DWORD threads;       // DecoderThreads: 0 = automatic (default)
};
const XavcSettings& XavcGetSettings();

// ---- Logging (xavc_util.cpp) ----
// OutputDebugString + %ProgramData%\XavcDecoder\xavc.log, only when LogLevel >= level.
void XavcLogAt(int level, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
#define XavcLog(...)   XavcLogAt(1, __VA_ARGS__)
#define XavcTrace(...) XavcLogAt(2, __VA_ARGS__)

// Locate the SPS NAL in an H.264 codec-config blob (either avcC or Annex-B / raw
// SPS+PPS), and return profile_idc, or -1 if not found.
static inline int XavcH264ProfileFromConfig(const BYTE* p, DWORD len) {
    if (!p || len < 4) return -1;
    // avcC (AVCDecoderConfigurationRecord): first byte == 1
    if (p[0] == 1 && len >= 8) {
        // [0]=ver [1]=profile [2]=compat [3]=level [4]=lenSizeMinus1
        // [5]=(numSPS & 0x1f) [6..7]=spsLen [8..]=SPS NAL
        return p[1];
    }
    // Otherwise scan for a start code and an SPS (nal_unit_type == 7)
    for (DWORD i = 0; i + 4 < len; i++) {
        bool sc3 = (p[i] == 0 && p[i+1] == 0 && p[i+2] == 1);
        bool sc4 = (p[i] == 0 && p[i+1] == 0 && p[i+2] == 0 && p[i+3] == 1);
        if (sc3 || sc4) {
            DWORD nal = i + (sc4 ? 4 : 3);
            if (nal < len) {
                int type = p[nal] & 0x1f;
                if (type == 7 && nal + 1 < len) return p[nal + 1]; // profile_idc
            }
        }
    }
    return -1;
}

static inline bool XavcIsHigh422Profile(int profile_idc) {
    // Profiles the in-box H.264 decoder does NOT handle:
    // 122 High 4:2:2, 244 High 4:4:4 Predictive, 44 CAVLC 4:4:4, 110 High 10
    return profile_idc == 122 || profile_idc == 244 || profile_idc == 44 || profile_idc == 110;
}

// H.264 profile_idc of a media type: from the sequence header if present, else from
// MF_MT_MPEG2_PROFILE (the MPEG-4 source sets both). -1 if unknown.
static inline int XavcProfileOfMediaType(IMFMediaType* mt) {
    int profile = -1;
    BYTE* blob = NULL; UINT32 len = 0;
    if (SUCCEEDED(mt->GetAllocatedBlob(MF_MT_MPEG_SEQUENCE_HEADER, &blob, &len)) && blob) {
        profile = XavcH264ProfileFromConfig(blob, len);
        CoTaskMemFree(blob);
    }
    UINT32 p = 0;
    if (profile < 0 && SUCCEEDED(mt->GetUINT32(MF_MT_MPEG2_PROFILE, &p))) profile = (int)p;
    return profile;
}
