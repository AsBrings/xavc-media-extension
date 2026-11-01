// xavc_guids.h - our own GUIDs only (no Media Foundation headers here, so this
// can be compiled with <initguid.h> in guids.cpp without colliding with mfuuid.
#pragma once
#include <guiddef.h>

// {0B1E6D2A-7C3F-4A58-9E21-6F4D8C0A3B71}  CLSID of the XAVC H.264 decoder MFT. It is
// registered for the private 'XAVC' subtype only (see XAVC_VideoFormat_XAVC below): for
// H.264 itself mfplat.dll always tries the in-box decoder first (hardcoded priority table),
// so competing on the H264 subtype cannot work.
// (Earlier diagnostic builds used {7F9A3C21-5E44-4B0A-9C1D-2A6B8E3D4F10}.)
DEFINE_GUID(CLSID_XavcH264Decoder,
    0x0b1e6d2a, 0x7c3f, 0x4a58, 0x9e, 0x21, 0x6f, 0x4d, 0x8c, 0x0a, 0x3b, 0x71);

// {7F9A3C22-5E44-4B0A-9C1D-2A6B8E3D4F10}  Private marker IID: QueryInterface succeeds
// only on our MFT, so test tools can tell which decoder a pipeline actually inserted.
DEFINE_GUID(IID_IXavcMarker,
    0x7f9a3c22, 0x5e44, 0x4b0a, 0x9c, 0x1d, 0x2a, 0x6b, 0x8e, 0x3d, 0x4f, 0x10);

// {7F9A3C24-5E44-4B0A-9C1D-2A6B8E3D4F10}  CLSID of the XAVC byte stream handler: wraps the
// system MPEG-4 byte stream handler and retags H.264 High 4:2:2 / High 10 / 4:4:4 video
// streams with the private XAVC subtype, so only our decoder can match them.
DEFINE_GUID(CLSID_XavcByteStreamHandler,
    0x7f9a3c24, 0x5e44, 0x4b0a, 0x9c, 0x1d, 0x2a, 0x6b, 0x8e, 0x3d, 0x4f, 0x10);

// {BB420AA4-765B-4A1F-91FE-D6A8A143924C}  IID_IMFByteStreamHandler. mingw-w64 neither declares
// the interface nor ships the IID in libmfuuid.a, so we carry both (own name, no clash).
DEFINE_GUID(XAVC_IID_IMFByteStreamHandler,
    0xbb420aa4, 0x765b, 0x4a1f, 0x91, 0xfe, 0xd6, 0xa8, 0xa1, 0x43, 0x92, 0x4c);

// {EC15E2E9-E36B-4F7C-8758-77D452EF4CE7} IID_IMFQualityAdvise and
// {F3706F0D-8EA2-4886-8000-7155E9EC2EAE} IID_IMFQualityAdvise2 (missing from mingw-w64;
// values checked against msmpeg2vdec.dll / mf.dll).
DEFINE_GUID(XAVC_IID_IMFQualityAdvise,
    0xec15e2e9, 0xe36b, 0x4f7c, 0x87, 0x58, 0x77, 0xd4, 0x52, 0xef, 0x4c, 0xe7);
DEFINE_GUID(XAVC_IID_IMFQualityAdvise2,
    0xf3706f0d, 0x8ea2, 0x4886, 0x80, 0x00, 0x71, 0x55, 0xe9, 0xec, 0x2e, 0xae);

// {30D15206-ED2A-4760-BE17-EB4A9F12295C} MF_QUALITY_NOTIFY_SAMPLE_LAG: attribute of the
// MEQualityNotify events the renderer sends (how late a sample was, 100-ns units).
DEFINE_GUID(XAVC_MF_QUALITY_NOTIFY_SAMPLE_LAG,
    0x30d15206, 0xed2a, 0x4760, 0xbe, 0x17, 0xeb, 0x4a, 0x9f, 0x12, 0x29, 0x5c);

// {A6E1F733-3001-4915-8150-1558A2180EC8} MF_BYTESTREAMHANDLER_ACCEPTS_SHARE_WRITE (missing from mingw-w64).
DEFINE_GUID(XAVC_MF_BYTESTREAMHANDLER_ACCEPTS_SHARE_WRITE,
    0xa6e1f733, 0x3001, 0x4915, 0x81, 0x50, 0x15, 0x58, 0xa2, 0x18, 0x0e, 0xc8);

// {271C3902-6095-4C45-A22F-20091816EE9E}  system "MPEG4 Byte Stream Handler" (mfmp4srcsnk.dll)
DEFINE_GUID(CLSID_SystemMpeg4ByteStreamHandler,
    0x271c3902, 0x6095, 0x4c45, 0xa2, 0x2f, 0x20, 0x09, 0x18, 0x16, 0xee, 0x9e);

#ifndef XAVC_DEFINE_FOURCC_GUID
#define XAVC_DEFINE_FOURCC_GUID(name, fcc) \
    DEFINE_GUID(name, fcc, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71);
#endif
XAVC_DEFINE_FOURCC_GUID(XAVC_VideoFormat_H264, 0x34363248) // 'H264'
XAVC_DEFINE_FOURCC_GUID(XAVC_VideoFormat_NV12, 0x3231564e) // 'NV12'
XAVC_DEFINE_FOURCC_GUID(XAVC_VideoFormat_P010, 0x30313050) // 'P010'
XAVC_DEFINE_FOURCC_GUID(XAVC_VideoFormat_Y210, 0x30313259) // 'Y210'
XAVC_DEFINE_FOURCC_GUID(XAVC_VideoFormat_YUY2, 0x32595559) // 'YUY2'
// 'XAVC' - private subtype our byte stream handler gives to H.264 streams the in-box
// decoder cannot decode (it would otherwise accept them at SetInputType and then fail).
XAVC_DEFINE_FOURCC_GUID(XAVC_VideoFormat_XAVC, 0x43564158) // 'XAVC'
