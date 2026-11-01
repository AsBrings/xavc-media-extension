// xavc_decoder.cpp - the XAVC decoder MFT: H.264 High 4:2:2 / High 10 / High 4:4:4 (Sony
// XAVC S, XAVC S-I, ...) decoded with libavcodec.
//
// Input:  the private 'XAVC' subtype our byte stream handler gives these H.264 streams
//         (Annex-B access units, sample time = presentation time, SPS/PPS in
//         MF_MT_MPEG_SEQUENCE_HEADER and in-band), or plain 'H264' with such a profile.
// Output: system-memory frames. 4:2:2 sources are offered as Y210 (10 bit) / YUY2 (8 bit)
//         first, so the chroma resolution is kept when the renderer accepts it; P010 / NV12
//         follow as fallbacks. HKLM\SOFTWARE\XavcDecoder\OutputFormat can force 4:2:0.
// Decoding: frame-threaded software decoding; if a GPU decoder can handle the stream
//         (NVDEC on NVIDIA Blackwell and newer is the only one with H.264 4:2:2) libavcodec
//         uses it and we copy its frames to system memory. Any failure there falls back to
//         software automatically.
//
// A synchronous MFT: the pipeline alternates ProcessInput / ProcessOutput. libavcodec keeps
// a few frames in flight (frame threads, B-frame reordering), so output starts a few
// samples after the input does; DRAIN and FLUSH map onto avcodec's drain and flush.

#include "xavc_common.h"
#include "xavc_ffmpeg.h"
#include "xavc_h264.h"
#include "xavc_convert.h"
#include <new>

namespace {

const LONGLONG kHns = 10000000;   // 100-ns units per second

// Set once the GPU path turned out to be unusable in this process (no CUDA device, or
// NVDEC rejected the stream), so later decoder instances skip the attempt.
volatile LONG g_gpuUnusable = 0;

const GUID& SubtypeOf(XavcOutFmt f) {
    switch (f) {
    case XOF_NV12: return XAVC_VideoFormat_NV12;
    case XOF_P010: return XAVC_VideoFormat_P010;
    case XOF_YUY2: return XAVC_VideoFormat_YUY2;
    default:       return XAVC_VideoFormat_Y210;
    }
}

XavcOutFmt FmtOfSubtype(const GUID& g) {
    if (IsEqualGUID(g, XAVC_VideoFormat_NV12)) return XOF_NV12;
    if (IsEqualGUID(g, XAVC_VideoFormat_P010)) return XOF_P010;
    if (IsEqualGUID(g, XAVC_VideoFormat_YUY2)) return XOF_YUY2;
    if (IsEqualGUID(g, XAVC_VideoFormat_Y210)) return XOF_Y210;
    return XOF_NONE;
}

const char* FmtName(XavcOutFmt f) {
    switch (f) {
    case XOF_NV12: return "NV12";
    case XOF_P010: return "P010";
    case XOF_YUY2: return "YUY2";
    case XOF_Y210: return "Y210";
    default:       return "none";
    }
}

// H.264 VUI code points (ISO/IEC 23091-2) -> Media Foundation enums; 0 = leave unset.
UINT32 MapPrimaries(int c) {
    switch (c) {
    case 1:  return MFVideoPrimaries_BT709;
    case 4:  return MFVideoPrimaries_BT470_2_SysM;
    case 5:  return MFVideoPrimaries_BT470_2_SysBG;
    case 6:  return MFVideoPrimaries_SMPTE170M;
    case 7:  return MFVideoPrimaries_SMPTE240M;
    case 9:  return MFVideoPrimaries_BT2020;
    case 11: case 12: return MFVideoPrimaries_DCI_P3;
    default: return 0;
    }
}
UINT32 MapTransfer(int t) {
    switch (t) {
    case 1: case 6: return MFVideoTransFunc_709;
    case 4:  return MFVideoTransFunc_22;
    case 5:  return MFVideoTransFunc_28;
    case 7:  return MFVideoTransFunc_240M;
    case 8:  return MFVideoTransFunc_10;
    case 13: return MFVideoTransFunc_sRGB;
    case 14: case 15: return MFVideoTransFunc_2020;
    case 16: return MFVideoTransFunc_2084;
    case 18: return MFVideoTransFunc_HLG;
    default: return 0;
    }
}
UINT32 MapMatrix(int m, int depth) {
    switch (m) {
    case 1:  return MFVideoTransferMatrix_BT709;
    case 5: case 6: return MFVideoTransferMatrix_BT601;
    case 7:  return MFVideoTransferMatrix_SMPTE240M;
    case 9: case 10: return depth > 10 ? MFVideoTransferMatrix_BT2020_12 : MFVideoTransferMatrix_BT2020_10;
    default: return 0;
    }
}

// Describes a decoded frame's memory layout for XavcConverter (any planar / semi-planar YUV
// layout with 8..16-bit little-endian samples). False for anything else.
bool DescribeFrame(const XavcFF* ff, const AVFrame* f, XavcSrcPlanes* s) {
    const AVPixFmtDescriptor* d = ff->pix_fmt_desc_get((enum AVPixelFormat)f->format);
    if (!d) return false;
    const uint64_t unsupported = AV_PIX_FMT_FLAG_HWACCEL | AV_PIX_FMT_FLAG_RGB | AV_PIX_FMT_FLAG_BE |
                                 AV_PIX_FMT_FLAG_PAL | AV_PIX_FMT_FLAG_BITSTREAM | AV_PIX_FMT_FLAG_FLOAT |
                                 AV_PIX_FMT_FLAG_BAYER;
    if ((d->flags & unsupported) || !(d->flags & AV_PIX_FMT_FLAG_PLANAR)) return false;
    memset(s, 0, sizeof(*s));
    const AVComponentDescriptor& y = d->comp[0];
    if (y.depth < 8 || y.depth > 16) return false;
    s->bytes = y.depth > 8 ? 2 : 1;
    if (y.step != s->bytes || y.offset != 0 || (s->bytes == 1 && y.shift)) return false;
    s->depth = y.depth;
    s->shift = y.shift;
    s->width = f->width;
    s->height = f->height;
    s->data[0] = f->data[y.plane];
    s->linesize[0] = f->linesize[y.plane];
    if (d->nb_components < 3) {
        s->gray = 1;
    } else {
        const AVComponentDescriptor& u = d->comp[1];
        const AVComponentDescriptor& v = d->comp[2];
        if (u.depth != y.depth || v.depth != y.depth || u.shift != y.shift || v.shift != y.shift) return false;
        if (d->log2_chroma_w > 1 || d->log2_chroma_h > 1) return false;
        s->log2_cw = d->log2_chroma_w;
        s->log2_ch = d->log2_chroma_h;
        if (u.plane == v.plane) {
            if (u.step != 2 * s->bytes || v.step != 2 * s->bytes) return false;
            if (u.offset == 0 && v.offset == s->bytes) s->vu = 0;
            else if (v.offset == 0 && u.offset == s->bytes) s->vu = 1;
            else return false;
            s->semi = 1;
            s->data[1] = f->data[u.plane];
            s->linesize[1] = f->linesize[u.plane];
        } else {
            if (u.step != s->bytes || v.step != s->bytes || u.offset || v.offset) return false;
            s->data[1] = f->data[u.plane];
            s->linesize[1] = f->linesize[u.plane];
            s->data[2] = f->data[v.plane];
            s->linesize[2] = f->linesize[v.plane];
        }
    }
    if (!s->data[0] || s->linesize[0] <= 0) return false;
    if (!s->gray && (!s->data[1] || s->linesize[1] <= 0)) return false;
    if (!s->gray && !s->semi && (!s->data[2] || s->linesize[2] <= 0)) return false;
    return true;
}

// Everything we derive from an input media type.
struct StreamInfo {
    UINT32 w, h;                 // output picture size (even)
    UINT32 fpsN, fpsD;           // 0/0 = unknown
    UINT32 parN, parD;
    int chroma;                  // chroma_format_idc
    int depth;                   // luma bit depth
    bool progressive;
    UINT32 range, primaries, transfer, matrix;   // MF enums, 0 = unset
    int profile;
};

class CXavcDecoder : public IMFTransform, public IMFQualityAdvise2 {
public:
    CXavcDecoder()
        : m_cRef(1), m_inType(NULL), m_outType(NULL), m_attrs(NULL), m_nOffer(0),
          m_outFmt(XOF_NONE), m_outStride(0), m_outSize(0), m_defaultDur(0),
          m_ff(NULL), m_ctx(NULL), m_pkt(NULL), m_frame(NULL), m_swFrame(NULL), m_gpuDevice(NULL),
          m_gpuTried(false), m_pktPending(false), m_frameReady(false), m_draining(false),
          m_drainSent(false), m_discPending(false), m_nextPts(0), m_frames(0), m_errors(0),
          m_convTicks(0), m_statStart(0), m_statFrames(0),
          m_dropMode(MF_DROP_MODE_NONE), m_quality(MF_QUALITY_NORMAL), m_dropUntil(AV_NOPTS_VALUE),
          m_droppedOut(0), m_autoLevel(0), m_lateRun(0), m_onTimeRun(0), m_levelChangedAt(0), m_lagEvents(0), m_eventDumps(0),
          m_catchUpUntil(AV_NOPTS_VALUE), m_catchUpEndedAt(0), m_lastDownAt(0), m_downHoldMs(10000),
          m_warmupUntil(0), m_badFrames(0), m_skippedIn(0) {
        InitializeCriticalSection(&m_cs);
        memset(&m_info, 0, sizeof(m_info));
        memset(m_offer, 0, sizeof(m_offer));
        InterlockedIncrement(&g_cObj);
        XavcTrace("decoder %p created", (void*)this);
    }

    virtual ~CXavcDecoder() {
        CloseDecoder();
        if (m_inType) m_inType->Release();
        if (m_outType) m_outType->Release();
        if (m_attrs) m_attrs->Release();
        DeleteCriticalSection(&m_cs);
        XavcTrace("decoder %p destroyed after %llu frames", (void*)this, (unsigned long long)m_frames);
        InterlockedDecrement(&g_cObj);
    }

    // ---------------------------------------------------------------- IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) {
        if (!ppv) return E_POINTER;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IMFTransform) ||
            IsEqualIID(riid, IID_IXavcMarker)) {
            *ppv = static_cast<IMFTransform*>(this);
        } else if (IsEqualIID(riid, XAVC_IID_IMFQualityAdvise) || IsEqualIID(riid, XAVC_IID_IMFQualityAdvise2)) {
            XavcTrace("decoder QueryInterface IMFQualityAdvise%s", IsEqualIID(riid, XAVC_IID_IMFQualityAdvise2) ? "2" : "");
            *ppv = static_cast<IMFQualityAdvise2*>(this);
        } else {
            if (XavcGetSettings().logLevel >= 2) {
                wchar_t g[64];
                StringFromGUID2(riid, g, 64);
                XavcTrace("decoder QueryInterface %ls -> E_NOINTERFACE", g);
            }
            *ppv = NULL;
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() { return InterlockedIncrement(&m_cRef); }
    STDMETHODIMP_(ULONG) Release() {
        LONG c = InterlockedDecrement(&m_cRef);
        if (!c) delete this;
        return c;
    }

    // ---------------------------------------------------------------- stream configuration
    STDMETHODIMP GetStreamLimits(DWORD* inMin, DWORD* inMax, DWORD* outMin, DWORD* outMax) {
        if (!inMin || !inMax || !outMin || !outMax) return E_POINTER;
        *inMin = *inMax = *outMin = *outMax = 1;
        return S_OK;
    }
    STDMETHODIMP GetStreamCount(DWORD* in, DWORD* out) {
        if (!in || !out) return E_POINTER;
        *in = *out = 1;
        return S_OK;
    }
    STDMETHODIMP GetStreamIDs(DWORD, DWORD*, DWORD, DWORD*) { return E_NOTIMPL; }

    STDMETHODIMP GetInputStreamInfo(DWORD id, MFT_INPUT_STREAM_INFO* p) {
        if (id) return MF_E_INVALIDSTREAMNUMBER;
        if (!p) return E_POINTER;
        memset(p, 0, sizeof(*p));
        p->dwFlags = MFT_INPUT_STREAM_WHOLE_SAMPLES | MFT_INPUT_STREAM_SINGLE_SAMPLE_PER_BUFFER;
        return S_OK;
    }
    STDMETHODIMP GetOutputStreamInfo(DWORD id, MFT_OUTPUT_STREAM_INFO* p) {
        if (id) return MF_E_INVALIDSTREAMNUMBER;
        if (!p) return E_POINTER;
        Lock lock(m_cs);
        memset(p, 0, sizeof(*p));
        p->dwFlags = MFT_OUTPUT_STREAM_WHOLE_SAMPLES | MFT_OUTPUT_STREAM_SINGLE_SAMPLE_PER_BUFFER |
                     MFT_OUTPUT_STREAM_FIXED_SAMPLE_SIZE;
        p->cbSize = m_outSize;   // the client allocates the output samples
        return S_OK;
    }
    STDMETHODIMP GetAttributes(IMFAttributes** pp) {
        if (!pp) return E_POINTER;
        Lock lock(m_cs);
        if (!m_attrs) {
            HRESULT hr = MFCreateAttributes(&m_attrs, 2);
            if (FAILED(hr)) return hr;
        }
        *pp = m_attrs;
        m_attrs->AddRef();
        return S_OK;
    }
    STDMETHODIMP GetInputStreamAttributes(DWORD, IMFAttributes**) { return E_NOTIMPL; }
    STDMETHODIMP GetOutputStreamAttributes(DWORD, IMFAttributes**) { return E_NOTIMPL; }
    STDMETHODIMP DeleteInputStream(DWORD) { return E_NOTIMPL; }
    STDMETHODIMP AddInputStreams(DWORD, DWORD*) { return E_NOTIMPL; }

    // ---------------------------------------------------------------- media types
    STDMETHODIMP GetInputAvailableType(DWORD id, DWORD idx, IMFMediaType** pp) {
        if (id) return MF_E_INVALIDSTREAMNUMBER;
        if (!pp) return E_POINTER;
        if (idx > 0) return MF_E_NO_MORE_TYPES;
        IMFMediaType* t = NULL;
        HRESULT hr = MFCreateMediaType(&t);
        if (FAILED(hr)) return hr;
        t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        t->SetGUID(MF_MT_SUBTYPE, XAVC_VideoFormat_XAVC);
        *pp = t;
        return S_OK;
    }

    STDMETHODIMP GetOutputAvailableType(DWORD id, DWORD idx, IMFMediaType** pp) {
        if (id) return MF_E_INVALIDSTREAMNUMBER;
        if (!pp) return E_POINTER;
        Lock lock(m_cs);
        if (!m_inType) return MF_E_TRANSFORM_TYPE_NOT_SET;
        if (idx >= (DWORD)m_nOffer) return MF_E_NO_MORE_TYPES;
        return MakeOutputType(m_offer[idx], pp);
    }

    STDMETHODIMP SetInputType(DWORD id, IMFMediaType* t, DWORD flags) {
        if (id) return MF_E_INVALIDSTREAMNUMBER;
        if (flags & ~(DWORD)MFT_SET_TYPE_TEST_ONLY) return E_INVALIDARG;
        const bool test = (flags & MFT_SET_TYPE_TEST_ONLY) != 0;
        Lock lock(m_cs);
        if (!t) {
            if (!test) {
                CloseDecoder();
                SafeRelease(&m_inType);
                SafeRelease(&m_outType);
                m_nOffer = 0;
            }
            return S_OK;
        }
        StreamInfo info;
        HRESULT hr = AnalyzeInputType(t, &info);
        if (FAILED(hr)) return hr;
        // Without FFmpeg we cannot decode: refuse now, so the failure is a clean
        // "codec not found" instead of an error in the middle of playback.
        if (!XavcLoadFFmpeg()) return MF_E_INVALIDMEDIATYPE;
        if (test) return S_OK;

        IMFMediaType* copy = NULL;
        hr = MFCreateMediaType(&copy);
        if (FAILED(hr)) return hr;
        hr = t->CopyAllItems(copy);
        if (FAILED(hr)) { copy->Release(); return hr; }

        CloseDecoder();
        SafeRelease(&m_inType);
        m_inType = copy;
        m_info = info;
        m_defaultDur = (info.fpsN && info.fpsD) ? (LONGLONG)((double)kHns * info.fpsD / info.fpsN + 0.5) : kHns / 30;
        ChooseOfferedFormats();
        // Keep a compatible output type (some clients set the input type again later).
        if (m_outType) {
            UINT32 w = 0, h = 0;
            MFGetAttributeSize(m_outType, MF_MT_FRAME_SIZE, &w, &h);
            if (w != m_info.w || h != m_info.h) SafeRelease(&m_outType);
        }
        char offers[64] = "";
        for (int i = 0; i < m_nOffer; i++) {
            strcat(offers, i ? ", " : "");
            strcat(offers, FmtName(m_offer[i]));
        }
        XavcLog("input: profile %d, %ux%u, %s %d-bit, %u/%u fps, PAR %u:%u, %s; offering %s",
                info.profile, info.w, info.h,
                info.chroma == 2 ? "4:2:2" : info.chroma == 3 ? "4:4:4" : info.chroma == 1 ? "4:2:0" : "4:0:0",
                info.depth, info.fpsN, info.fpsD, info.parN, info.parD,
                info.progressive ? "progressive" : "interlaced/mixed", offers);
        return S_OK;
    }

    STDMETHODIMP SetOutputType(DWORD id, IMFMediaType* t, DWORD flags) {
        if (id) return MF_E_INVALIDSTREAMNUMBER;
        if (flags & ~(DWORD)MFT_SET_TYPE_TEST_ONLY) return E_INVALIDARG;
        const bool test = (flags & MFT_SET_TYPE_TEST_ONLY) != 0;
        Lock lock(m_cs);
        if (!m_inType) return MF_E_TRANSFORM_TYPE_NOT_SET;
        if (!t) {
            if (!test) SafeRelease(&m_outType);
            return S_OK;
        }
        GUID major = GUID_NULL, sub = GUID_NULL;
        if (FAILED(t->GetGUID(MF_MT_MAJOR_TYPE, &major)) || !IsEqualGUID(major, MFMediaType_Video) ||
            FAILED(t->GetGUID(MF_MT_SUBTYPE, &sub)))
            return MF_E_INVALIDMEDIATYPE;
        XavcOutFmt fmt = FmtOfSubtype(sub);
        XavcTrace("SetOutputType(%s%s)", fmt == XOF_NONE ? "unsupported subtype" : FmtName(fmt), test ? ", test only" : "");
        if (fmt == XOF_NONE) return MF_E_INVALIDMEDIATYPE;
        UINT32 w = 0, h = 0;
        if (FAILED(MFGetAttributeSize(t, MF_MT_FRAME_SIZE, &w, &h)) || w != m_info.w || h != m_info.h) {
            XavcTrace("SetOutputType: frame size %ux%u rejected (picture is %ux%u)", w, h, m_info.w, m_info.h);
            return MF_E_INVALIDMEDIATYPE;
        }
        UINT32 stride = 0, size = 0;
        XavcFrameLayout(fmt, w, h, &stride, &size);
        UINT32 want = 0;
        if (SUCCEEDED(t->GetUINT32(MF_MT_DEFAULT_STRIDE, &want))) {
            INT32 s = (INT32)want;
            if (s < (INT32)stride || s > (INT32)stride * 4) return MF_E_INVALIDMEDIATYPE;   // bottom-up / odd layouts
            if ((UINT32)s != stride) {
                UINT32 rows = (fmt == XOF_NV12 || fmt == XOF_P010) ? h + h / 2 : h;
                stride = (UINT32)s;
                size = stride * rows;
            }
        }
        if (test) return S_OK;

        IMFMediaType* copy = NULL;
        HRESULT hr = MFCreateMediaType(&copy);
        if (FAILED(hr)) return hr;
        hr = t->CopyAllItems(copy);
        if (FAILED(hr)) { copy->Release(); return hr; }
        SafeRelease(&m_outType);
        m_outType = copy;
        m_outFmt = fmt;
        m_outStride = stride;
        m_outSize = size;
        XavcLog("output: %s %ux%u stride %u", FmtName(fmt), w, h, stride);
        return S_OK;
    }

    STDMETHODIMP GetInputCurrentType(DWORD id, IMFMediaType** pp) {
        if (id) return MF_E_INVALIDSTREAMNUMBER;
        if (!pp) return E_POINTER;
        Lock lock(m_cs);
        if (!m_inType) return MF_E_TRANSFORM_TYPE_NOT_SET;
        return CloneType(m_inType, pp);
    }
    STDMETHODIMP GetOutputCurrentType(DWORD id, IMFMediaType** pp) {
        if (id) return MF_E_INVALIDSTREAMNUMBER;
        if (!pp) return E_POINTER;
        Lock lock(m_cs);
        if (!m_outType) return MF_E_TRANSFORM_TYPE_NOT_SET;
        return CloneType(m_outType, pp);
    }

    STDMETHODIMP GetInputStatus(DWORD id, DWORD* f) {
        if (id) return MF_E_INVALIDSTREAMNUMBER;
        if (!f) return E_POINTER;
        Lock lock(m_cs);
        if (!m_inType || !m_outType) return MF_E_TRANSFORM_TYPE_NOT_SET;
        *f = (m_pktPending || m_draining) ? 0 : MFT_INPUT_STATUS_ACCEPT_DATA;
        return S_OK;
    }
    STDMETHODIMP GetOutputStatus(DWORD* f) {
        if (!f) return E_POINTER;
        Lock lock(m_cs);
        if (!m_inType || !m_outType) return MF_E_TRANSFORM_TYPE_NOT_SET;
        *f = m_frameReady ? MFT_OUTPUT_STATUS_SAMPLE_READY : 0;
        return S_OK;
    }
    STDMETHODIMP SetOutputBounds(LONGLONG, LONGLONG) { return E_NOTIMPL; }
    STDMETHODIMP ProcessEvent(DWORD, IMFMediaEvent*) { return E_NOTIMPL; }

    // ---------------------------------------------------------------- streaming
    STDMETHODIMP ProcessMessage(MFT_MESSAGE_TYPE m, ULONG_PTR param) {
        Lock lock(m_cs);
        XavcTrace("ProcessMessage 0x%08x", (unsigned)m);
        switch (m) {
        case MFT_MESSAGE_COMMAND_FLUSH:
            Flush();
            break;
        case MFT_MESSAGE_COMMAND_DRAIN:
            if (m_ctx) { m_draining = true; m_drainSent = false; }
            break;
        case MFT_MESSAGE_SET_D3D_MANAGER:
            // Software decoder: frames go out in system memory; the pipeline uploads them.
            return param ? E_NOTIMPL : S_OK;
        case MFT_MESSAGE_COMMAND_MARKER:
            return E_NOTIMPL;   // asynchronous MFTs only
        default:
            break;
        }
        return S_OK;
    }

    STDMETHODIMP ProcessInput(DWORD id, IMFSample* s, DWORD flags) {
        if (id) return MF_E_INVALIDSTREAMNUMBER;
        if (!s) return E_POINTER;
        if (flags) return E_INVALIDARG;
        Lock lock(m_cs);
        if (!m_inType || !m_outType) return MF_E_TRANSFORM_TYPE_NOT_SET;
        if (m_pktPending || m_draining) return MF_E_NOTACCEPTING;
        HRESULT hr = OpenDecoder();
        if (FAILED(hr)) return hr;

        UINT32 v = 0;
        if (m_catchUpUntil != AV_NOPTS_VALUE) {             // skipping ahead (see StartCatchUp)
            LONGLONG t = 0;
            const bool key = SUCCEEDED(s->GetUINT32(MFSampleExtension_CleanPoint, &v)) && v;
            if (!key || FAILED(s->GetSampleTime(&t)) || t < m_catchUpUntil) {
                if (SUCCEEDED(s->GetUINT32(MFSampleExtension_Discontinuity, &v)) && v) m_discPending = true;
                m_skippedIn++;
                return S_OK;
            }
            XavcLog("quality: resuming at key frame t=%.3fs, %llu samples skipped", t / 1e7,
                    (unsigned long long)m_skippedIn);
            m_catchUpUntil = AV_NOPTS_VALUE;
            m_catchUpEndedAt = GetTickCount64();
            m_dropUntil = t;
            ApplyQuality();
        }

        IMFMediaBuffer* mb = NULL;
        hr = s->ConvertToContiguousBuffer(&mb);
        if (FAILED(hr)) return hr;
        BYTE* p = NULL;
        DWORD maxLen = 0, len = 0;
        hr = mb->Lock(&p, &maxLen, &len);
        if (FAILED(hr)) { mb->Release(); return hr; }
        int r = 0;
        if (len > 0 && len < 0x40000000) {
            m_ff->packet_unref(m_pkt);
            r = m_ff->new_packet(m_pkt, (int)len);
            if (r == 0) memcpy(m_pkt->data, p, len);
        }
        mb->Unlock();
        mb->Release();
        if (len == 0) return S_OK;                          // empty sample: nothing to decode
        if (len >= 0x40000000 || r < 0) return E_OUTOFMEMORY;

        LONGLONG t = 0, d = 0;
        m_pkt->pts = SUCCEEDED(s->GetSampleTime(&t)) ? t : AV_NOPTS_VALUE;
        m_pkt->duration = SUCCEEDED(s->GetSampleDuration(&d)) && d > 0 ? d : 0;
        if (SUCCEEDED(s->GetUINT32(MFSampleExtension_CleanPoint, &v)) && v) m_pkt->flags |= AV_PKT_FLAG_KEY;
        if (SUCCEEDED(s->GetUINT32(MFSampleExtension_Discontinuity, &v)) && v) m_discPending = true;

        r = m_ff->send_packet(m_ctx, m_pkt);
        if (r == AVERROR(EAGAIN)) {          // decoder full: keep it until output is taken
            m_pktPending = true;
            return S_OK;
        }
        m_ff->packet_unref(m_pkt);
        if (r < 0) LogError("send_packet", r);   // corrupt data: skip the sample, keep going
        return S_OK;
    }

    STDMETHODIMP ProcessOutput(DWORD, DWORD count, MFT_OUTPUT_DATA_BUFFER* bufs, DWORD* status) {
        if (!bufs || !status) return E_POINTER;
        if (count != 1) return E_INVALIDARG;
        *status = 0;
        Lock lock(m_cs);
        bufs[0].dwStatus = 0;
        bufs[0].pEvents = NULL;
        if (!m_inType || !m_outType) return MF_E_TRANSFORM_TYPE_NOT_SET;
        if (!m_ctx) return MF_E_TRANSFORM_NEED_MORE_INPUT;

        for (int attempt = 0; attempt < 64; attempt++) {
            HRESULT hr = FetchFrame();
            if (hr != S_OK) return hr;
            UINT32 fw = (UINT32)m_frame->width & ~1u, fh = (UINT32)m_frame->height & ~1u;
            if (fw != m_info.w || fh != m_info.h) {
                // The stream's real picture size differs from what the media type said:
                // renegotiate the output type, then deliver this frame.
                XavcLog("picture is %ux%u, media type said %ux%u: output format change", fw, fh, m_info.w, m_info.h);
                if (fw < 16 || fh < 16 || fw > 8192 || fh > 8192) { DropFrame(); continue; }
                m_info.w = fw;
                m_info.h = fh;
                SafeRelease(&m_outType);
                m_outSize = 0;
                bufs[0].dwStatus = MFT_OUTPUT_DATA_BUFFER_FORMAT_CHANGE;
                return MF_E_TRANSFORM_STREAM_CHANGE;
            }
            if (m_dropUntil != AV_NOPTS_VALUE) {       // IMFQualityAdvise::DropTime window
                LONGLONG t = m_frame->pts != AV_NOPTS_VALUE ? m_frame->pts : m_frame->best_effort_timestamp;
                if (t != AV_NOPTS_VALUE && t < m_dropUntil) {
                    m_droppedOut++;
                    DropFrame();
                    continue;
                }
                m_dropUntil = AV_NOPTS_VALUE;
                ApplyQuality();
            }
            hr = DeliverFrame(bufs[0]);
            if (hr == S_FALSE) continue;     // this frame could not be used; take the next one
            return hr;
        }
        return MF_E_TRANSFORM_NEED_MORE_INPUT;
    }

    // ---------------------------------------------------------------- IMFQualityAdvise(2)
    // The playback pipeline's quality manager calls these when frames reach the renderer
    // late (e.g. 4K60 Long GOP on a CPU that cannot decode 60 fps). We trade quality for
    // speed in libavcodec instead of letting the pipeline fall back to key frames only:
    //   quality level -1: skip the deblocking filter on non-reference frames, -2: on all frames
    //   drop mode 1: don't decode non-reference frames (XAVC S Long GOP: the B frames)
    //   drop mode 2: no B frames, 3: intra frames only, 4/5: key frames only
    STDMETHODIMP SetDropMode(MF_QUALITY_DROP_MODE mode) {
        if ((int)mode < MF_DROP_MODE_NONE || (int)mode > MF_DROP_MODE_5) return MF_E_NO_MORE_DROP_MODES;
        Lock lock(m_cs);
        XavcTrace("SetDropMode(%d)", (int)mode);
        if (mode != m_dropMode) XavcLog("quality: drop mode %d -> %d", (int)m_dropMode, (int)mode);
        m_dropMode = mode;
        ApplyQuality();
        return S_OK;
    }
    STDMETHODIMP SetQualityLevel(MF_QUALITY_LEVEL level) {
        if ((int)level < MF_QUALITY_NORMAL) return E_INVALIDARG;
        if ((int)level > MF_QUALITY_NORMAL_MINUS_2) return MF_E_NO_MORE_QUALITY_LEVELS;
        Lock lock(m_cs);
        XavcTrace("SetQualityLevel(%d)", (int)level);
        if (level != m_quality) XavcLog("quality: level -%d -> -%d", (int)m_quality, (int)level);
        m_quality = level;
        ApplyQuality();
        return S_OK;
    }
    STDMETHODIMP GetDropMode(MF_QUALITY_DROP_MODE* mode) {
        if (!mode) return E_POINTER;
        XavcTrace("GetDropMode");
        Lock lock(m_cs);
        *mode = m_dropMode;
        return S_OK;
    }
    STDMETHODIMP GetQualityLevel(MF_QUALITY_LEVEL* level) {
        if (!level) return E_POINTER;
        Lock lock(m_cs);
        *level = m_quality;
        return S_OK;
    }
    // Skip the next hnsAmountToDrop of video: frames before that point are not delivered,
    // and non-reference frames are not even decoded meanwhile.
    STDMETHODIMP DropTime(LONGLONG amount) {
        Lock lock(m_cs);
        XavcTrace("DropTime(%lld)", (long long)amount);
        if (amount > 0 && m_ctx) StartCatchUp(m_nextPts + amount, "DropTime");
        return S_OK;
    }
    // The Media Engine (new Media Player) forwards every MEQualityNotify of the video renderer
    // here and leaves the reaction to us (it never calls SetDropMode / SetQualityLevel).
    STDMETHODIMP NotifyQualityEvent(IMFMediaEvent* ev, DWORD* flags) {
        if (!flags || !ev) return E_POINTER;
        *flags = 0;
        MediaEventType t = MEUnknown;
        UINT64 lag = 0;
        if (XavcGetSettings().logLevel >= 2 && (InterlockedIncrement(&m_eventDumps) % 40) == 1) {
            PROPVARIANT v;
            PropVariantInit(&v);
            ev->GetValue(&v);
            UINT32 n = 0;
            ev->GetCount(&n);
            ev->GetType(&t);
            XavcTrace("quality event type %lu, value vt=%u (%lld), %u attribute(s)", (unsigned long)t, v.vt,
                      (long long)v.hVal.QuadPart, n);
            for (UINT32 i = 0; i < n; i++) {
                GUID key;
                PROPVARIANT a;
                PropVariantInit(&a);
                if (SUCCEEDED(ev->GetItemByIndex(i, &key, &a))) {
                    wchar_t g[64];
                    StringFromGUID2(key, g, 64);
                    XavcTrace("  %ls vt=%u value=%lld", g, a.vt, (long long)a.hVal.QuadPart);
                }
                PropVariantClear(&a);
            }
            PropVariantClear(&v);
        }
        if (FAILED(ev->GetType(&t)) || t != MEQualityNotify) return S_OK;
        // The Media Engine's renderer sends the lag as the event value (VT_I8, 100 ns; its one
        // attribute is the sample time); the classic sinks use MF_QUALITY_NOTIFY_SAMPLE_LAG.
        PROPVARIANT v;
        PropVariantInit(&v);
        bool have = false;
        if (SUCCEEDED(ev->GetValue(&v)) && (v.vt == VT_I8 || v.vt == VT_UI8)) {
            lag = (UINT64)v.hVal.QuadPart;
            have = true;
        }
        PropVariantClear(&v);
        if (!have && FAILED(ev->GetUINT64(XAVC_MF_QUALITY_NOTIFY_SAMPLE_LAG, &lag))) return S_OK;
        Lock lock(m_cs);
        AdaptToLag((LONGLONG)lag, flags);
        return S_OK;
    }

private:
    // libavcodec reads these per frame (frame threads copy them from the user context).
    // Automatic levels (from renderer lateness): 1 = no deblocking on non-reference frames,
    // 2 = no deblocking at all, 3 = non-reference frames not decoded, 4 = no B frames.
    static const int kMaxAutoLevel = 4;

    void ApplyQuality() {
        if (!m_ctx) return;
        static const enum AVDiscard kSkipFrame[] = { AVDISCARD_DEFAULT, AVDISCARD_NONREF, AVDISCARD_BIDIR,
                                                     AVDISCARD_NONINTRA, AVDISCARD_NONKEY, AVDISCARD_NONKEY };
        enum AVDiscard skip = kSkipFrame[m_dropMode <= MF_DROP_MODE_5 ? m_dropMode : MF_DROP_MODE_5];
        if (m_autoLevel >= 4 && skip < AVDISCARD_BIDIR) skip = AVDISCARD_BIDIR;
        else if (m_autoLevel >= 3 && skip < AVDISCARD_NONREF) skip = AVDISCARD_NONREF;
        if (m_catchUpUntil != AV_NOPTS_VALUE && skip < AVDISCARD_NONREF) skip = AVDISCARD_NONREF;
        enum AVDiscard loop = m_quality >= MF_QUALITY_NORMAL_MINUS_2 ? AVDISCARD_ALL :
                              m_quality == MF_QUALITY_NORMAL_MINUS_1 ? AVDISCARD_NONREF : AVDISCARD_DEFAULT;
        if (m_autoLevel >= 2) loop = AVDISCARD_ALL;
        else if (m_autoLevel >= 1 && loop < AVDISCARD_NONREF) loop = AVDISCARD_NONREF;
        m_ctx->skip_frame = skip;
        m_ctx->skip_loop_filter = loop;
    }

    // Renderer feedback: lag > 0 means the frame was shown that late (100-ns units).
    // Late frames escalate the automatic level step by step; once at the maximum and still
    // far behind, we jump ahead to a key frame (StartCatchUp). Sustained on-time playback
    // steps back down.
    void AdaptToLag(LONGLONG lag, DWORD* flags) {
        const LONGLONG kLate = 40 * 10000, kOnTime = 15 * 10000, kFarBehind = 300 * 10000;
        const ULONGLONG now = GetTickCount64();
        if (++m_lagEvents % 60 == 1)
            XavcTrace("quality: lag %.1f ms (auto level %d)", lag / 1e4, m_autoLevel);
        if (m_catchUpUntil != AV_NOPTS_VALUE || now - m_catchUpEndedAt < 1500 || now < m_warmupUntil)
            return;                                   // frames from before a jump, or the pipeline filling up
        if (lag > kLate) { m_lateRun++; m_onTimeRun = 0; }
        else if (lag < kOnTime) { m_onTimeRun++; m_lateRun = 0; }
        bool changed = false;
        if (m_lateRun >= 4 && m_autoLevel < kMaxAutoLevel && now - m_levelChangedAt >= 250) {
            m_autoLevel++;
            changed = true;
            // Stepping down did not hold: wait longer before the next attempt.
            if (m_lastDownAt && now - m_lastDownAt < 20000 && m_downHoldMs < 160000) m_downHoldMs *= 2;
        } else if (m_onTimeRun >= 300 && m_autoLevel > 0 && now - m_levelChangedAt >= m_downHoldMs) {
            m_autoLevel--;
            m_lastDownAt = now;
            changed = true;
        }
        if (changed) {
            m_levelChangedAt = now;
            m_lateRun = m_onTimeRun = 0;
            XavcLog("quality: renderer lag %.0f ms -> automatic level %d", lag / 1e4, m_autoLevel);
            ApplyQuality();
        }
        if (m_autoLevel == kMaxAutoLevel && lag > kFarBehind && m_frames)
            StartCatchUp(m_nextPts + lag + 5000000, "renderer lag");
        if (m_autoLevel == kMaxAutoLevel && lag > kLate) *flags |= MF_QUALITY_CANNOT_KEEP_UP;
    }

    // Skip ahead: compressed samples are dropped at the input until a key frame at or after
    // 'until' arrives, and frames still in the decoder from before are not delivered. Decoding
    // resumes cleanly at that key frame (nothing refers to a skipped frame).
    void StartCatchUp(LONGLONG until, const char* why) {
        if (m_catchUpUntil != AV_NOPTS_VALUE) return;
        m_catchUpUntil = until;
        m_dropUntil = until;
        m_skippedIn = 0;
        XavcLog("quality: %s, skipping ahead to the first key frame after t=%.3fs", why, until / 1e7);
        ApplyQuality();
    }

    struct Lock {
        explicit Lock(CRITICAL_SECTION& cs) : m(cs) { EnterCriticalSection(&m); }
        ~Lock() { LeaveCriticalSection(&m); }
        CRITICAL_SECTION& m;
    };

    template <class T> static void SafeRelease(T** pp) {
        if (*pp) { (*pp)->Release(); *pp = NULL; }
    }

    static HRESULT CloneType(IMFMediaType* src, IMFMediaType** pp) {
        IMFMediaType* t = NULL;
        HRESULT hr = MFCreateMediaType(&t);
        if (FAILED(hr)) return hr;
        hr = src->CopyAllItems(t);
        if (FAILED(hr)) { t->Release(); return hr; }
        *pp = t;
        return S_OK;
    }

    void LogError(const char* what, int r) {
        if (++m_errors <= 20 || (m_errors % 500) == 0)
            XavcLog("%s: %s (error #%lu)", what, XavcAvErr(r).s, (unsigned long)m_errors);
    }

    // Validates an input type and derives the output picture description from it.
    HRESULT AnalyzeInputType(IMFMediaType* t, StreamInfo* info) {
        GUID major = GUID_NULL, sub = GUID_NULL;
        if (FAILED(t->GetGUID(MF_MT_MAJOR_TYPE, &major)) || !IsEqualGUID(major, MFMediaType_Video))
            return MF_E_INVALIDMEDIATYPE;
        if (FAILED(t->GetGUID(MF_MT_SUBTYPE, &sub)) ||
            (!IsEqualGUID(sub, XAVC_VideoFormat_XAVC) && !IsEqualGUID(sub, XAVC_VideoFormat_H264)))
            return MF_E_INVALIDMEDIATYPE;

        memset(info, 0, sizeof(*info));
        XavcSps sps;
        bool haveSps = false;
        BYTE* blob = NULL;
        UINT32 blobLen = 0;
        if (SUCCEEDED(t->GetAllocatedBlob(MF_MT_MPEG_SEQUENCE_HEADER, &blob, &blobLen)) && blob) {
            haveSps = XavcParseSpsFromConfig(blob, blobLen, &sps);
            CoTaskMemFree(blob);
        }
        int profile = haveSps ? sps.profile_idc : -1;
        UINT32 mtProfile = 0;
        if (profile < 0 && SUCCEEDED(t->GetUINT32(MF_MT_MPEG2_PROFILE, &mtProfile))) profile = (int)mtProfile;
        // Only the profiles the in-box decoder cannot handle; ordinary H.264 stays with it.
        if (!XavcIsHigh422Profile(profile)) {
            XavcTrace("SetInputType: profile %d is not ours", profile);
            return MF_E_INVALIDMEDIATYPE;
        }
        info->profile = profile;
        if (haveSps && sps.chroma_format_idc == 3 && sps.matrix_coefficients == 0) {
            XavcLog("SetInputType: 4:4:4 coded as GBR is not supported");
            return MF_E_INVALIDMEDIATYPE;
        }

        UINT32 mw = 0, mh = 0;
        MFGetAttributeSize(t, MF_MT_FRAME_SIZE, &mw, &mh);
        if (haveSps) {
            info->chroma = sps.chroma_format_idc;
            info->depth = sps.bit_depth_luma;
            info->w = (UINT32)sps.width;
            info->h = (UINT32)sps.height;
            info->progressive = sps.frame_mbs_only != 0;
            if (mw && mh && (mw != info->w || mh != info->h))
                XavcLog("media type says %ux%u, SPS says %ux%u: using the SPS", mw, mh, info->w, info->h);
        } else {
            info->chroma = profile == 110 ? 1 : profile == 122 ? 2 : 3;
            info->depth = 10;
            info->w = mw;
            info->h = mh;
            UINT32 il = MFVideoInterlace_Unknown;
            t->GetUINT32(MF_MT_INTERLACE_MODE, &il);
            info->progressive = il == MFVideoInterlace_Progressive;
        }
        info->w &= ~1u;
        info->h &= ~1u;
        if (info->w < 16 || info->h < 16 || info->w > 8192 || info->h > 8192) {
            XavcLog("SetInputType: unusable picture size %ux%u", info->w, info->h);
            return MF_E_INVALIDMEDIATYPE;
        }

        if (FAILED(MFGetAttributeRatio(t, MF_MT_FRAME_RATE, &info->fpsN, &info->fpsD)) || !info->fpsN || !info->fpsD) {
            info->fpsN = info->fpsD = 0;
            if (haveSps && sps.num_units_in_tick && sps.time_scale) {
                info->fpsN = sps.time_scale;
                info->fpsD = 2 * sps.num_units_in_tick;
            }
        }
        if (FAILED(MFGetAttributeRatio(t, MF_MT_PIXEL_ASPECT_RATIO, &info->parN, &info->parD)) || !info->parN || !info->parD) {
            info->parN = info->parD = 1;
            if (haveSps && sps.sar_num > 0 && sps.sar_den > 0) {
                info->parN = (UINT32)sps.sar_num;
                info->parD = (UINT32)sps.sar_den;
            }
        }
        // Colour description: the container's (if any), else the SPS VUI.
        if (FAILED(t->GetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, &info->range)))
            info->range = (haveSps && sps.video_full_range == 1) ? MFNominalRange_0_255 : MFNominalRange_16_235;
        if (FAILED(t->GetUINT32(MF_MT_VIDEO_PRIMARIES, &info->primaries)))
            info->primaries = haveSps ? MapPrimaries(sps.colour_primaries) : 0;
        if (FAILED(t->GetUINT32(MF_MT_TRANSFER_FUNCTION, &info->transfer)))
            info->transfer = haveSps ? MapTransfer(sps.transfer_characteristics) : 0;
        if (FAILED(t->GetUINT32(MF_MT_YUV_MATRIX, &info->matrix)))
            info->matrix = haveSps ? MapMatrix(sps.matrix_coefficients, info->depth) : 0;
        if (haveSps)
            XavcTrace("SPS: profile %d level %d chroma %d depth %d/%d %dx%d vui range %d prim %d trc %d matrix %d",
                      sps.profile_idc, sps.level_idc, sps.chroma_format_idc, sps.bit_depth_luma,
                      sps.bit_depth_chroma, sps.width, sps.height, sps.video_full_range,
                      sps.colour_primaries, sps.transfer_characteristics, sps.matrix_coefficients);
        return S_OK;
    }

    void ChooseOfferedFormats() {
        const bool deep = m_info.depth > 8;
        const XavcOutFmt full = deep ? XOF_Y210 : XOF_YUY2;   // keeps 4:2:2 chroma
        const XavcOutFmt half = deep ? XOF_P010 : XOF_NV12;
        m_nOffer = 0;
        switch (XavcGetSettings().outputFormat) {
        case 2:
            m_offer[m_nOffer++] = XOF_NV12;
            break;
        case 1:
            m_offer[m_nOffer++] = half;
            if (half != XOF_NV12) m_offer[m_nOffer++] = XOF_NV12;
            break;
        default:
            // Renderers take the first format their video processor supports: Y210 keeps the
            // 4:2:2 chroma at 10 bits (newer GPUs), P010 keeps the 10 bits, YUY2 keeps 4:2:2.
            if (m_info.chroma >= 2) m_offer[m_nOffer++] = full;
            m_offer[m_nOffer++] = half;
            if (m_info.chroma >= 2 && deep) m_offer[m_nOffer++] = XOF_YUY2;
            if (half != XOF_NV12) m_offer[m_nOffer++] = XOF_NV12;
            break;
        }
    }

    HRESULT MakeOutputType(XavcOutFmt f, IMFMediaType** pp) {
        IMFMediaType* t = NULL;
        HRESULT hr = MFCreateMediaType(&t);
        if (FAILED(hr)) return hr;
        UINT32 stride = 0, size = 0;
        XavcFrameLayout(f, m_info.w, m_info.h, &stride, &size);
        t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        t->SetGUID(MF_MT_SUBTYPE, SubtypeOf(f));
        MFSetAttributeSize(t, MF_MT_FRAME_SIZE, m_info.w, m_info.h);
        if (m_info.fpsN && m_info.fpsD) MFSetAttributeRatio(t, MF_MT_FRAME_RATE, m_info.fpsN, m_info.fpsD);
        MFSetAttributeRatio(t, MF_MT_PIXEL_ASPECT_RATIO, m_info.parN, m_info.parD);
        t->SetUINT32(MF_MT_INTERLACE_MODE, m_info.progressive ? MFVideoInterlace_Progressive
                                                              : MFVideoInterlace_MixedInterlaceOrProgressive);
        t->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
        t->SetUINT32(MF_MT_FIXED_SIZE_SAMPLES, TRUE);
        t->SetUINT32(MF_MT_SAMPLE_SIZE, size);
        t->SetUINT32(MF_MT_DEFAULT_STRIDE, stride);
        if (m_info.range) t->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, m_info.range);
        if (m_info.primaries) t->SetUINT32(MF_MT_VIDEO_PRIMARIES, m_info.primaries);
        if (m_info.transfer) t->SetUINT32(MF_MT_TRANSFER_FUNCTION, m_info.transfer);
        if (m_info.matrix) t->SetUINT32(MF_MT_YUV_MATRIX, m_info.matrix);
        *pp = t;
        return S_OK;
    }

    // libavcodec callback (may run on one of its threads): pick NVDEC when offered and
    // usable, otherwise the software format. If NVDEC cannot set up the stream, libavcodec
    // drops it from the list and asks again.
    static enum AVPixelFormat GetFormat(AVCodecContext* c, const enum AVPixelFormat* fmts) {
        CXavcDecoder* self = static_cast<CXavcDecoder*>(c->opaque);
        const XavcFF* ff = self->m_ff;
        enum AVPixelFormat sw = AV_PIX_FMT_NONE;
        bool cuda = false;
        for (const enum AVPixelFormat* p = fmts; *p != AV_PIX_FMT_NONE; p++) {
            if (*p == AV_PIX_FMT_CUDA) { cuda = true; continue; }
            const AVPixFmtDescriptor* d = ff->pix_fmt_desc_get(*p);
            if (sw == AV_PIX_FMT_NONE && d && !(d->flags & AV_PIX_FMT_FLAG_HWACCEL)) sw = *p;
        }
        const char* swName = sw != AV_PIX_FMT_NONE ? ff->get_pix_fmt_name(sw) : "none";
        if (cuda && c->hw_device_ctx && !g_gpuUnusable) {
            self->m_gpuTried = true;
            XavcLog("trying NVDEC (GPU) decoding, software format %s", swName);
            return AV_PIX_FMT_CUDA;
        }
        if (self->m_gpuTried && c->hw_device_ctx && !g_gpuUnusable) {
            InterlockedExchange(&g_gpuUnusable, 1);
            XavcLog("NVDEC cannot decode this stream; software decoding (%s)", swName);
        } else {
            XavcLog("software decoding (%s)", swName);
        }
        return sw;
    }

    HRESULT OpenDecoder() {
        if (m_ctx) return S_OK;
        m_ff = XavcLoadFFmpeg();
        if (!m_ff) return E_FAIL;
        const AVCodec* codec = m_ff->find_decoder(AV_CODEC_ID_H264);
        if (!codec) { XavcLog("libavcodec has no H.264 decoder"); return E_FAIL; }
        AVCodecContext* c = m_ff->alloc_context3(codec);
        if (!c) return E_OUTOFMEMORY;

        BYTE* blob = NULL;
        UINT32 len = 0;
        if (SUCCEEDED(m_inType->GetAllocatedBlob(MF_MT_MPEG_SEQUENCE_HEADER, &blob, &len)) && blob) {
            if (len && len < (1u << 20)) {
                c->extradata = (uint8_t*)m_ff->mallocz(len + AV_INPUT_BUFFER_PADDING_SIZE);
                if (c->extradata) {
                    memcpy(c->extradata, blob, len);
                    c->extradata_size = (int)len;
                }
            }
            CoTaskMemFree(blob);
        }
        const XavcSettings& st = XavcGetSettings();
        c->pkt_timebase.num = 1;
        c->pkt_timebase.den = (int)kHns;
        c->thread_count = (int)st.threads;                  // 0 = one per logical CPU (+1)
        UINT32 lowLatency = 0;
        if (m_attrs) m_attrs->GetUINT32(MF_LOW_LATENCY, &lowLatency);
        c->thread_type = lowLatency ? FF_THREAD_SLICE : (FF_THREAD_FRAME | FF_THREAD_SLICE);
        c->opaque = this;
        c->get_format = GetFormat;
        c->flags |= AV_CODEC_FLAG_UNALIGNED;               // exact cropping; the converter needs no alignment

        // GPU: only NVDEC can decode H.264 4:2:2 (NVIDIA Blackwell and newer). Creating the
        // CUDA device fails quickly where there is no NVIDIA driver.
        if (st.hwDecode && !g_gpuUnusable && !m_gpuDevice) {
            int r = m_ff->hwdevice_ctx_create(&m_gpuDevice, AV_HWDEVICE_TYPE_CUDA, NULL, NULL, 0);
            if (r < 0) {
                m_gpuDevice = NULL;
                InterlockedExchange(&g_gpuUnusable, 1);
                XavcLog("no CUDA device (%s): software decoding", XavcAvErr(r).s);
            }
        }
        if (m_gpuDevice) c->hw_device_ctx = m_ff->buffer_ref(m_gpuDevice);

        int r = m_ff->open2(c, codec, NULL);
        if (r < 0) {
            XavcLog("avcodec_open2: %s", XavcAvErr(r).s);
            m_ff->free_context(&c);
            return E_FAIL;
        }
        if (!m_pkt) m_pkt = m_ff->packet_alloc();
        if (!m_frame) m_frame = m_ff->frame_alloc();
        if (!m_swFrame) m_swFrame = m_ff->frame_alloc();
        if (!m_pkt || !m_frame || !m_swFrame) {
            m_ff->free_context(&c);
            return E_OUTOFMEMORY;
        }
        m_ctx = c;
        m_pktPending = m_frameReady = m_draining = m_drainSent = false;
        ApplyQuality();
        XavcLog("decoder opened: %d thread(s), %s threading%s", c->thread_count,
                c->active_thread_type == FF_THREAD_FRAME ? "frame" :
                c->active_thread_type == FF_THREAD_SLICE ? "slice" : "no",
                m_gpuDevice ? ", CUDA device available" : "");
        return S_OK;
    }

    void CloseDecoder() {
        if (!m_ff) return;
        if (m_ctx) m_ff->free_context(&m_ctx);
        if (m_pkt) m_ff->packet_free(&m_pkt);
        if (m_frame) m_ff->frame_free(&m_frame);
        if (m_swFrame) m_ff->frame_free(&m_swFrame);
        if (m_gpuDevice) m_ff->buffer_unref(&m_gpuDevice);
        m_pktPending = m_frameReady = m_draining = m_drainSent = false;
    }

    void Flush() {
        if (!m_ctx) return;
        XavcLog("flush (seek) after %llu frames", (unsigned long long)m_frames);
        m_ff->flush_buffers(m_ctx);
        m_ff->packet_unref(m_pkt);
        m_ff->frame_unref(m_frame);
        m_pktPending = m_frameReady = m_draining = m_drainSent = false;
        m_discPending = false;
        m_dropUntil = AV_NOPTS_VALUE;
        m_catchUpUntil = AV_NOPTS_VALUE;
        m_lateRun = m_onTimeRun = 0;
        m_warmupUntil = GetTickCount64() + 2000;
        ApplyQuality();
    }

    void DropFrame() {
        m_ff->frame_unref(m_frame);
        m_frameReady = false;
    }

    // Makes m_frame hold the next decoded frame (S_OK), or returns NEED_MORE_INPUT.
    HRESULT FetchFrame() {
        int errors = 0;
        while (!m_frameReady) {
            int r = m_ff->receive_frame(m_ctx, m_frame);
            if (r == 0) {
                m_frameReady = true;
                break;
            }
            if (r == AVERROR(EAGAIN)) {
                if (m_pktPending) {                         // all output taken: resend
                    int s = m_ff->send_packet(m_ctx, m_pkt);
                    m_ff->packet_unref(m_pkt);
                    m_pktPending = false;
                    if (s < 0) LogError("send_packet (resend)", s);
                    continue;
                }
                if (m_draining && !m_drainSent) {
                    m_ff->send_packet(m_ctx, NULL);         // enter draining mode
                    m_drainSent = true;
                    continue;
                }
                return MF_E_TRANSFORM_NEED_MORE_INPUT;
            }
            if (r == AVERROR_EOF) {                         // drain complete
                m_ff->flush_buffers(m_ctx);
                m_draining = m_drainSent = false;
                XavcTrace("drained");
                return MF_E_TRANSFORM_NEED_MORE_INPUT;
            }
            LogError("receive_frame", r);                   // a corrupt frame: skip it
            if (++errors > 32) return MF_E_TRANSFORM_NEED_MORE_INPUT;
        }
        return S_OK;
    }

    // Converts m_frame into the client's output sample. S_FALSE = frame dropped.
    HRESULT DeliverFrame(MFT_OUTPUT_DATA_BUFFER& ob) {
        AVFrame* f = m_frame;
        const AVFrame* src = f;
        if (f->hw_frames_ctx) {                             // GPU frame: copy to system memory
            m_ff->frame_unref(m_swFrame);
            int r = m_ff->hwframe_transfer_data(m_swFrame, f, 0);
            if (r < 0) { LogError("GPU frame download", r); DropFrame(); return S_FALSE; }
            // libavcodec can only crop right/bottom of GPU surfaces; do top/left on the copy.
            if (f->crop_left || f->crop_top) {
                m_swFrame->crop_left = f->crop_left;
                m_swFrame->crop_top = f->crop_top;
                m_ff->frame_apply_cropping(m_swFrame, AV_FRAME_CROP_UNALIGNED);
            }
            src = m_swFrame;
        }
        XavcSrcPlanes sp;
        if (!DescribeFrame(m_ff, src, &sp) || sp.width < (int)m_info.w || sp.height < (int)m_info.h) {
            if (++m_errors <= 5)
                XavcLog("cannot convert frame format %s %dx%d", m_ff->get_pix_fmt_name((enum AVPixelFormat)src->format),
                        src->width, src->height);
            DropFrame();
            // A stream we can never convert must fail visibly instead of playing black forever.
            return ++m_badFrames >= 30 ? E_FAIL : S_FALSE;
        }
        sp.interlaced = (f->flags & AV_FRAME_FLAG_INTERLACED) ? 1 : 0;

        IMFSample* out = ob.pSample;
        IMFSample* created = NULL;
        if (!out) {                                          // not expected, but be tolerant
            HRESULT hr = MFCreateSample(&created);
            if (FAILED(hr)) return hr;
            out = created;
        }
        IMFMediaBuffer* mb = NULL;
        DWORD nbuf = 0;
        out->GetBufferCount(&nbuf);
        HRESULT hr = S_OK;
        if (nbuf == 0) {
            hr = MFCreateAlignedMemoryBuffer(m_outSize, MF_64_BYTE_ALIGNMENT, &mb);
            if (SUCCEEDED(hr)) hr = out->AddBuffer(mb);
        } else if (nbuf == 1) {
            hr = out->GetBufferByIndex(0, &mb);
        } else {
            hr = out->ConvertToContiguousBuffer(&mb);
        }
        if (FAILED(hr)) {
            if (mb) mb->Release();
            if (created) created->Release();
            return hr;
        }

        // Lock. 2-D buffers report their own pitch (Media Foundation pads it, e.g. to a multiple of
        // 64 bytes) and, through IMF2DBuffer2, the real size of the allocation; plain buffers use
        // the negotiated stride. Rows are 'pitch' apart; the last one needs only minStride bytes.
        IMF2DBuffer* b2 = NULL;
        IMF2DBuffer2* b22 = NULL;
        BYTE* base = NULL;
        LONG pitch = 0;
        DWORD capacity = 0;
        const UINT32 rows = (m_outFmt == XOF_NV12 || m_outFmt == XOF_P010) ? m_info.h + m_info.h / 2 : m_info.h;
        UINT32 minStride = 0, minSize = 0;
        XavcFrameLayout(m_outFmt, m_info.w, m_info.h, &minStride, &minSize);
        if (SUCCEEDED(mb->QueryInterface(IID_IMF2DBuffer2, (void**)&b22))) {
            b2 = b22;                                        // IMF2DBuffer2 derives from IMF2DBuffer
            BYTE* start = NULL;
            DWORD len = 0;
            hr = b22->Lock2DSize(MF2DBuffer_LockFlags_Write, &base, &pitch, &start, &len);
            if (SUCCEEDED(hr) && (pitch < (LONG)minStride || base < start ||
                                  (UINT64)(base - start) + (UINT64)pitch * (rows - 1) + minStride > len)) {
                b22->Unlock2D();
                hr = MF_E_BUFFERTOOSMALL;
            }
        } else if (SUCCEEDED(mb->QueryInterface(IID_IMF2DBuffer, (void**)&b2))) {
            hr = b2->Lock2D(&base, &pitch);
            DWORD maxLen = 0;
            if (SUCCEEDED(hr) && (pitch < (LONG)minStride || FAILED(mb->GetMaxLength(&maxLen)) ||
                                  (UINT64)pitch * (rows - 1) + minStride > maxLen)) {
                b2->Unlock2D();
                hr = MF_E_BUFFERTOOSMALL;
            }
        } else {
            DWORD cur = 0;
            hr = mb->Lock(&base, &capacity, &cur);
            pitch = (LONG)m_outStride;
            if (SUCCEEDED(hr) && (UINT64)pitch * (rows - 1) + minStride > capacity) {
                mb->Unlock();
                hr = MF_E_BUFFERTOOSMALL;
            }
        }
        if (FAILED(hr)) {
            XavcLog("cannot use the output buffer: 0x%08lx", (unsigned long)hr);
            if (b2) b2->Release();
            mb->Release();
            if (created) created->Release();
            return hr;
        }

        XavcDstPlanes dp;
        dp.p0 = base;
        dp.pitch = pitch;
        dp.p1 = base + (ptrdiff_t)pitch * m_info.h;
        LARGE_INTEGER t0, t1;
        QueryPerformanceCounter(&t0);
        const bool converted = m_conv.Run(sp, m_outFmt, dp, (int)m_info.w, (int)m_info.h);
        QueryPerformanceCounter(&t1);
        m_convTicks += t1.QuadPart - t0.QuadPart;

        // Current length: the contiguous image size for 2-D buffers (what Lock() would return),
        // the stride-based layout for plain buffers.
        DWORD used = minSize;
        if (b2) {
            DWORD contiguous = 0;
            if (SUCCEEDED(b2->GetContiguousLength(&contiguous)) && contiguous) used = contiguous;
            b2->Unlock2D();
            b2->Release();
        } else {
            UINT64 full = (UINT64)pitch * rows;
            used = (DWORD)(full < capacity ? full : capacity);
            mb->Unlock();
        }
        if (converted) mb->SetCurrentLength(used);
        mb->Release();
        if (!converted) {                                    // out of memory: never deliver a partial picture
            XavcLog("frame conversion failed (out of memory?)");
            if (created) created->Release();
            DropFrame();
            return E_OUTOFMEMORY;
        }
        m_badFrames = 0;

        LONGLONG pts = f->pts;
        if (pts == AV_NOPTS_VALUE) pts = f->best_effort_timestamp;
        if (pts == AV_NOPTS_VALUE) pts = m_nextPts;
        const LONGLONG dur = f->duration > 0 ? f->duration : m_defaultDur;
        m_nextPts = pts + dur;
        out->SetSampleTime(pts);
        out->SetSampleDuration(dur);
        out->SetUINT32(MFSampleExtension_CleanPoint, TRUE);
        out->SetUINT32(MFSampleExtension_Discontinuity, m_discPending ? TRUE : FALSE);
        m_discPending = false;
        if (!m_info.progressive) {
            const bool il = (f->flags & AV_FRAME_FLAG_INTERLACED) != 0;
            out->SetUINT32(MFSampleExtension_Interlaced, il ? TRUE : FALSE);
            out->SetUINT32(MFSampleExtension_BottomFieldFirst,
                           (il && !(f->flags & AV_FRAME_FLAG_TOP_FIELD_FIRST)) ? TRUE : FALSE);
        }
        if (created) ob.pSample = created;                   // ownership passes to the caller

        if (m_frames++ == 0) {
            m_warmupUntil = GetTickCount64() + 2000;
            XavcLog("first frame: %s %dx%d (%s) -> %s, t=%.3fs%s", m_ff->get_pix_fmt_name((enum AVPixelFormat)src->format),
                    src->width, src->height, f->hw_frames_ctx ? "decoded on the GPU" : "decoded on the CPU",
                    FmtName(m_outFmt), pts / 1e7, b2 ? ", 2-D output buffer" : "");
            m_statStart = t1.QuadPart;
            m_statFrames = 0;
            m_convTicks = 0;
        } else if (XavcGetSettings().logLevel >= 1) {
            // Throughput every ~5 s of wall time: shows whether playback keeps up.
            m_statFrames++;
            LARGE_INTEGER fq;
            QueryPerformanceFrequency(&fq);
            double secs = (double)(t1.QuadPart - m_statStart) / fq.QuadPart;
            if (secs >= 5.0) {
                XavcLog("%llu frames so far; last %.1f s: %.1f fps output, conversion %.2f ms/frame (t=%.3fs, auto level %d, %llu skipped)",
                        (unsigned long long)m_frames, secs, m_statFrames / secs,
                        1000.0 * m_convTicks / fq.QuadPart / (m_statFrames ? m_statFrames : 1), pts / 1e7,
                        m_autoLevel, (unsigned long long)m_droppedOut);
                m_statStart = t1.QuadPart;
                m_statFrames = 0;
                m_convTicks = 0;
            }
        }
        DropFrame();

        if (m_pktPending) {                                  // room again: queue the held sample
            int r = m_ff->send_packet(m_ctx, m_pkt);
            if (r != AVERROR(EAGAIN)) {
                m_ff->packet_unref(m_pkt);
                m_pktPending = false;
                if (r < 0) LogError("send_packet (after output)", r);
            }
        }
        return S_OK;
    }

    CRITICAL_SECTION m_cs;
    LONG m_cRef;
    IMFMediaType* m_inType;
    IMFMediaType* m_outType;
    IMFAttributes* m_attrs;
    StreamInfo m_info;
    XavcOutFmt m_offer[4];
    int m_nOffer;
    XavcOutFmt m_outFmt;
    UINT32 m_outStride, m_outSize;
    LONGLONG m_defaultDur;

    const XavcFF* m_ff;
    AVCodecContext* m_ctx;
    AVPacket* m_pkt;
    AVFrame* m_frame;
    AVFrame* m_swFrame;
    AVBufferRef* m_gpuDevice;
    volatile bool m_gpuTried;
    bool m_pktPending, m_frameReady, m_draining, m_drainSent, m_discPending;
    LONGLONG m_nextPts;
    UINT64 m_frames;
    ULONG m_errors;
    LONGLONG m_convTicks;
    LONGLONG m_statStart;
    UINT64 m_statFrames;
    MF_QUALITY_DROP_MODE m_dropMode;
    MF_QUALITY_LEVEL m_quality;
    LONGLONG m_dropUntil;
    UINT64 m_droppedOut;
    int m_autoLevel;
    int m_lateRun, m_onTimeRun;
    ULONGLONG m_levelChangedAt;
    UINT64 m_lagEvents;
    volatile LONG m_eventDumps;
    LONGLONG m_catchUpUntil;
    ULONGLONG m_catchUpEndedAt;
    ULONGLONG m_lastDownAt;
    ULONGLONG m_downHoldMs;
    ULONGLONG m_warmupUntil;
    ULONG m_badFrames;
    UINT64 m_skippedIn;
    XavcConverter m_conv;
};

} // namespace

HRESULT XavcCreateDecoder(REFIID riid, void** ppv) {
    CXavcDecoder* d = new (std::nothrow) CXavcDecoder();
    if (!d) return E_OUTOFMEMORY;
    HRESULT hr = d->QueryInterface(riid, ppv);
    d->Release();
    return hr;
}
