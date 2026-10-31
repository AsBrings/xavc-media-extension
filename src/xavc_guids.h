// xavc_guids.h - our own GUIDs only (no Media Foundation headers here, so this
// can be compiled with <initguid.h> in guids.cpp without colliding with mfuuid.
#pragma once
#include <guiddef.h>

// {7F9A3C21-5E44-4B0A-9C1D-2A6B8E3D4F10}  CLSID of the XAVC H.264 decoder MFT.
DEFINE_GUID(CLSID_XavcH264Decoder,
    0x7f9a3c21, 0x5e44, 0x4b0a, 0x9c, 0x1d, 0x2a, 0x6b, 0x8e, 0x3d, 0x4f, 0x10);

#ifndef XAVC_DEFINE_FOURCC_GUID
#define XAVC_DEFINE_FOURCC_GUID(name, fcc) \
    DEFINE_GUID(name, fcc, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71);
#endif
XAVC_DEFINE_FOURCC_GUID(XAVC_VideoFormat_H264, 0x34363248) // 'H264'
XAVC_DEFINE_FOURCC_GUID(XAVC_VideoFormat_NV12, 0x3231564e) // 'NV12'
XAVC_DEFINE_FOURCC_GUID(XAVC_VideoFormat_P010, 0x30313050) // 'P010'
XAVC_DEFINE_FOURCC_GUID(XAVC_VideoFormat_Y210, 0x30313259) // 'Y210'
