// xavc_common.h - shared GUIDs, logging, helpers for the XAVC decoder MFT.
#pragma once
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <mfobjects.h>
#include <strmif.h>
#include <stdio.h>

// Our GUIDs (CLSID + FourCC media subtypes). Declared extern here; defined once
// in guids.cpp (which includes <initguid.h>). Keeps us from re-defining MF GUIDs
// that already live in mfuuid.a.
#include "xavc_guids.h"

// ---- Logging (robust across normal + AppContainer hosts) ----
// Primary sink: OutputDebugStringA (captured by DebugView, even from AppContainer).
// Secondary: a file the installer makes world/AppContainer-writable.
static inline void XavcLog(const char* fmt, ...) {
    char buf[1024];
    va_list ap; va_start(ap, fmt);
    _vsnprintf(buf, sizeof(buf) - 2, fmt, ap);
    va_end(ap);
    buf[sizeof(buf) - 2] = 0;
    size_t n = strlen(buf);
    // tag + newline
    char line[1152];
    DWORD pid = GetCurrentProcessId();
    _snprintf(line, sizeof(line) - 1, "[XAVC pid=%lu] %s\r\n", (unsigned long)pid, buf);
    line[sizeof(line) - 1] = 0;
    OutputDebugStringA(line);
    HANDLE h = CreateFileA("C:\\ProgramData\\XavcDecoder\\xavc.log",
        FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD w = 0; SetFilePointer(h, 0, NULL, FILE_END);
        WriteFile(h, line, (DWORD)strlen(line), &w, NULL);
        CloseHandle(h);
    }
    (void)n;
}

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
