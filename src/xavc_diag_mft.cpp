// xavc_diag_mft.cpp
//
// DIAGNOSTIC build of the XAVC H.264 4:2:2 decoder MFT.
//
// Purpose: answer the make-or-break question -- will the Windows 11 Media Player
// (and other hosts) load and select a third-party, system-registered Media
// Foundation video decoder for H.264 High 4:2:2 streams that the in-box decoder
// rejects?  This build does NOT really decode: once selected it emits a solid
// magenta frame.  So:
//   * magenta video in the player  => it loaded + selected us (green light).
//   * still "unsupported" / black  => it does not use third-party system MFTs.
//
// SAFETY: SetInputType accepts ONLY H.264 High 4:2:2 / High 10 / 4:4:4 (parsed
// from the SPS).  Ordinary 8-bit 4:2:0 H.264 is rejected, so normal video keeps
// using the in-box decoder and system playback is unaffected.  Uninstall script
// provided.
//
// Every entry point logs via OutputDebugString + C:\ProgramData\XavcDecoder\xavc.log.

#include "xavc_common.h"
#include <mfreadwrite.h>
#include <stdlib.h>

// Minimal global new/delete (malloc/free) so we need no C++ runtime library.
void* operator new(size_t n) { void* p = malloc(n ? n : 1); return p; }
void  operator delete(void* p) noexcept { free(p); }
void  operator delete(void* p, size_t) noexcept { free(p); }

static HMODULE g_hInst = NULL;
static volatile LONG g_cObj = 0;   // live objects
static volatile LONG g_cLock = 0;  // class-factory locks

static void FormatInfo(const GUID& sub, UINT32 w, UINT32 h, UINT32* stride, UINT32* size) {
    if (IsEqualGUID(sub, XAVC_VideoFormat_NV12))      { *stride = w;     *size = w * h * 3 / 2; }
    else if (IsEqualGUID(sub, XAVC_VideoFormat_P010)) { *stride = w * 2; *size = w * h * 3; }
    else /* Y210 */                                   { *stride = w * 4; *size = w * h * 4; }
}

// Fill a contiguous output buffer with solid magenta in the chosen format.
static void FillMagenta(const GUID& sub, BYTE* p, UINT32 w, UINT32 h) {
    // BT.709 limited-range magenta-ish: Y=105, Cb=212, Cr=234
    const BYTE Y8 = 105, U8 = 212, V8 = 234;
    if (IsEqualGUID(sub, XAVC_VideoFormat_NV12)) {
        memset(p, Y8, (size_t)w * h);
        BYTE* uv = p + (size_t)w * h;
        for (UINT32 i = 0; i < (w / 2) * (h / 2); i++) { uv[2*i] = U8; uv[2*i+1] = V8; }
    } else if (IsEqualGUID(sub, XAVC_VideoFormat_P010)) {
        unsigned short y = (unsigned short)((Y8 << 2) << 6);
        unsigned short u = (unsigned short)((U8 << 2) << 6);
        unsigned short v = (unsigned short)((V8 << 2) << 6);
        unsigned short* yp = (unsigned short*)p;
        for (UINT32 i = 0; i < w * h; i++) yp[i] = y;
        unsigned short* uv = (unsigned short*)(p + (size_t)w * h * 2);
        for (UINT32 i = 0; i < (w / 2) * (h / 2); i++) { uv[2*i] = u; uv[2*i+1] = v; }
    } else { // Y210: packed Y0 U Y1 V, 16-bit top-aligned 10-bit
        unsigned short y = (unsigned short)((Y8 << 2) << 6);
        unsigned short u = (unsigned short)((U8 << 2) << 6);
        unsigned short v = (unsigned short)((V8 << 2) << 6);
        unsigned short* q = (unsigned short*)p;
        UINT32 pairs = (w / 2) * h;
        for (UINT32 i = 0; i < pairs; i++) { q[4*i+0]=y; q[4*i+1]=u; q[4*i+2]=y; q[4*i+3]=v; }
    }
}

class CXavcDecoder : public IMFTransform {
public:
    CXavcDecoder() : m_cRef(1), m_haveInput(FALSE), m_inSet(FALSE), m_outSet(FALSE),
                     m_w(0), m_h(0), m_fpsN(30000), m_fpsD(1001),
                     m_outStride(0), m_outSize(0), m_pending(NULL),
                     m_time(0), m_dur(0) {
        memset(&m_outSub, 0, sizeof(m_outSub));
        InterlockedIncrement(&g_cObj);
        XavcLog("CXavcDecoder created");
    }
    virtual ~CXavcDecoder() {
        if (m_pending) m_pending->Release();
        InterlockedDecrement(&g_cObj);
    }

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) {
        if (!ppv) return E_POINTER;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IMFTransform) ||
            IsEqualIID(riid, IID_IXavcMarker)) {
            *ppv = static_cast<IMFTransform*>(this); AddRef(); return S_OK;
        }
        *ppv = NULL; return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() { return InterlockedIncrement(&m_cRef); }
    STDMETHODIMP_(ULONG) Release() {
        LONG c = InterlockedDecrement(&m_cRef); if (!c) delete this; return c;
    }

    // IMFTransform - stream config
    STDMETHODIMP GetStreamLimits(DWORD* inMin, DWORD* inMax, DWORD* outMin, DWORD* outMax) {
        if (!inMin||!inMax||!outMin||!outMax) return E_POINTER;
        *inMin=*inMax=*outMin=*outMax=1; return S_OK;
    }
    STDMETHODIMP GetStreamCount(DWORD* in, DWORD* out) {
        if (!in||!out) return E_POINTER; *in=1; *out=1; return S_OK;
    }
    STDMETHODIMP GetStreamIDs(DWORD, DWORD*, DWORD, DWORD*) { return E_NOTIMPL; }

    STDMETHODIMP GetInputStreamInfo(DWORD id, MFT_INPUT_STREAM_INFO* p) {
        if (id) return MF_E_INVALIDSTREAMNUMBER; if (!p) return E_POINTER;
        memset(p, 0, sizeof(*p));
        p->dwFlags = MFT_INPUT_STREAM_WHOLE_SAMPLES | MFT_INPUT_STREAM_SINGLE_SAMPLE_PER_BUFFER;
        return S_OK;
    }
    STDMETHODIMP GetOutputStreamInfo(DWORD id, MFT_OUTPUT_STREAM_INFO* p) {
        if (id) return MF_E_INVALIDSTREAMNUMBER; if (!p) return E_POINTER;
        memset(p, 0, sizeof(*p));
        p->dwFlags = MFT_OUTPUT_STREAM_WHOLE_SAMPLES | MFT_OUTPUT_STREAM_FIXED_SAMPLE_SIZE;
        p->cbSize = m_outSize;  // client allocates this
        return S_OK;
    }
    STDMETHODIMP GetAttributes(IMFAttributes** pp) { (void)pp; return E_NOTIMPL; }
    STDMETHODIMP GetInputStreamAttributes(DWORD, IMFAttributes**) { return E_NOTIMPL; }
    STDMETHODIMP GetOutputStreamAttributes(DWORD, IMFAttributes**) { return E_NOTIMPL; }
    STDMETHODIMP DeleteInputStream(DWORD) { return E_NOTIMPL; }
    STDMETHODIMP AddInputStreams(DWORD, DWORD*) { return E_NOTIMPL; }

    // Available types
    STDMETHODIMP GetInputAvailableType(DWORD id, DWORD idx, IMFMediaType** pp) {
        if (id) return MF_E_INVALIDSTREAMNUMBER; if (!pp) return E_POINTER;
        if (idx > 0) return MF_E_NO_MORE_TYPES;
        IMFMediaType* t = NULL; HRESULT hr = MFCreateMediaType(&t);
        if (FAILED(hr)) return hr;
        t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        t->SetGUID(MF_MT_SUBTYPE, XAVC_VideoFormat_H264);
        *pp = t; return S_OK;
    }
    STDMETHODIMP GetOutputAvailableType(DWORD id, DWORD idx, IMFMediaType** pp) {
        if (id) return MF_E_INVALIDSTREAMNUMBER; if (!pp) return E_POINTER;
        if (!m_inSet) return MF_E_TRANSFORM_TYPE_NOT_SET;
        GUID sub;
        if (idx == 0) sub = XAVC_VideoFormat_NV12;
        else if (idx == 1) sub = XAVC_VideoFormat_P010;
        else if (idx == 2) sub = XAVC_VideoFormat_Y210;
        else return MF_E_NO_MORE_TYPES;
        return MakeVideoType(sub, pp);
    }

    STDMETHODIMP SetInputType(DWORD id, IMFMediaType* t, DWORD flags) {
        if (id) return MF_E_INVALIDSTREAMNUMBER;
        if (!t) { if (!(flags & MFT_SET_TYPE_TEST_ONLY)) { m_inSet=FALSE; } return S_OK; }
        GUID maj, sub;
        if (FAILED(t->GetGUID(MF_MT_MAJOR_TYPE, &maj)) || !IsEqualGUID(maj, MFMediaType_Video))
            return MF_E_INVALIDMEDIATYPE;
        if (FAILED(t->GetGUID(MF_MT_SUBTYPE, &sub)) || !IsEqualGUID(sub, XAVC_VideoFormat_H264))
            return MF_E_INVALIDMEDIATYPE;

        // Confirm High 4:2:2 (etc.) from the codec private data. If we cannot
        // positively confirm it, REJECT -- so we never steal ordinary 8-bit video.
        int profile = -1;
        BYTE* blob = NULL; UINT32 blobLen = 0;
        if (SUCCEEDED(t->GetAllocatedBlob(MF_MT_MPEG_SEQUENCE_HEADER, &blob, &blobLen)) && blob) {
            profile = XavcH264ProfileFromConfig(blob, blobLen);
            CoTaskMemFree(blob);
        }
        UINT32 mtProfile = 0;
        if (profile < 0 && SUCCEEDED(t->GetUINT32(MF_MT_MPEG2_PROFILE, &mtProfile)))
            profile = (int)mtProfile;   // the MPEG-4 source also states the profile here
        XavcLog("SetInputType: H264, seqhdr=%s profile_idc=%d flags=%lu",
                blobLen ? "yes" : "no", profile, (unsigned long)flags);
        if (!XavcIsHigh422Profile(profile)) {
            XavcLog("SetInputType: not High 4:2:2 -> reject (let in-box decode)");
            return MF_E_INVALIDMEDIATYPE;
        }
        UINT32 w = 0, h = 0;
        MFGetAttributeSize(t, MF_MT_FRAME_SIZE, &w, &h);
        UINT32 fn = 30000, fd = 1001;
        MFGetAttributeRatio(t, MF_MT_FRAME_RATE, &fn, &fd);
        if (!w || !h) { XavcLog("SetInputType: missing frame size -> reject"); return MF_E_INVALIDMEDIATYPE; }
        if (flags & MFT_SET_TYPE_TEST_ONLY) return S_OK;
        m_w = w; m_h = h; m_fpsN = fn; m_fpsD = fd; m_inSet = TRUE; m_outSet = FALSE;
        XavcLog("SetInputType ACCEPTED: %ux%u @ %u/%u (profile %d)", w, h, fn, fd, profile);
        return S_OK;
    }

    STDMETHODIMP SetOutputType(DWORD id, IMFMediaType* t, DWORD flags) {
        if (id) return MF_E_INVALIDSTREAMNUMBER;
        if (!m_inSet) return MF_E_TRANSFORM_TYPE_NOT_SET;
        if (!t) { if (!(flags & MFT_SET_TYPE_TEST_ONLY)) m_outSet=FALSE; return S_OK; }
        GUID maj, sub;
        if (FAILED(t->GetGUID(MF_MT_MAJOR_TYPE,&maj)) || !IsEqualGUID(maj, MFMediaType_Video))
            return MF_E_INVALIDMEDIATYPE;
        if (FAILED(t->GetGUID(MF_MT_SUBTYPE,&sub))) return MF_E_INVALIDMEDIATYPE;
        if (!IsEqualGUID(sub, XAVC_VideoFormat_NV12) && !IsEqualGUID(sub, XAVC_VideoFormat_P010)
            && !IsEqualGUID(sub, XAVC_VideoFormat_Y210)) {
            XavcLog("SetOutputType: reject subtype %08lx", (unsigned long)sub.Data1);
            return MF_E_INVALIDMEDIATYPE;
        }
        UINT32 w=0,h=0; MFGetAttributeSize(t, MF_MT_FRAME_SIZE, &w,&h);
        if (w != m_w || h != m_h) {
            XavcLog("SetOutputType: reject size %ux%u (want %ux%u)", w, h, m_w, m_h);
            return MF_E_INVALIDMEDIATYPE;
        }
        if (flags & MFT_SET_TYPE_TEST_ONLY) return S_OK;
        m_outSub = sub; FormatInfo(sub, m_w, m_h, &m_outStride, &m_outSize); m_outSet = TRUE;
        XavcLog("SetOutputType ACCEPTED: stride=%u size=%u", m_outStride, m_outSize);
        return S_OK;
    }

    STDMETHODIMP GetInputCurrentType(DWORD id, IMFMediaType** pp) {
        if (id) return MF_E_INVALIDSTREAMNUMBER; if (!pp) return E_POINTER;
        if (!m_inSet) return MF_E_TRANSFORM_TYPE_NOT_SET;
        IMFMediaType* t=NULL; HRESULT hr=MFCreateMediaType(&t); if (FAILED(hr)) return hr;
        t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        t->SetGUID(MF_MT_SUBTYPE, XAVC_VideoFormat_H264);
        MFSetAttributeSize(t, MF_MT_FRAME_SIZE, m_w, m_h);
        *pp=t; return S_OK;
    }
    STDMETHODIMP GetOutputCurrentType(DWORD id, IMFMediaType** pp) {
        if (id) return MF_E_INVALIDSTREAMNUMBER; if (!pp) return E_POINTER;
        if (!m_outSet) return MF_E_TRANSFORM_TYPE_NOT_SET;
        return MakeVideoType(m_outSub, pp);
    }
    STDMETHODIMP GetInputStatus(DWORD id, DWORD* f) {
        if (id) return MF_E_INVALIDSTREAMNUMBER; if (!f) return E_POINTER;
        *f = m_haveInput ? 0 : MFT_INPUT_STATUS_ACCEPT_DATA; return S_OK;
    }
    STDMETHODIMP GetOutputStatus(DWORD* f) {
        if (!f) return E_POINTER;
        *f = m_haveInput ? MFT_OUTPUT_STATUS_SAMPLE_READY : 0; return S_OK;
    }
    STDMETHODIMP SetOutputBounds(LONGLONG, LONGLONG) { return S_OK; }
    STDMETHODIMP ProcessEvent(DWORD, IMFMediaEvent*) { return E_NOTIMPL; }

    STDMETHODIMP ProcessMessage(MFT_MESSAGE_TYPE m, ULONG_PTR param) {
        XavcLog("ProcessMessage 0x%08x param=%p", (unsigned)m, (void*)param);
        if (m == MFT_MESSAGE_SET_D3D_MANAGER && param)
            return E_NOTIMPL;   // software MFT: decline the D3D manager, output system memory
        if (m == MFT_MESSAGE_COMMAND_FLUSH) {
            if (m_pending) { m_pending->Release(); m_pending = NULL; } m_haveInput = FALSE;
        }
        return S_OK;
    }

    STDMETHODIMP ProcessInput(DWORD id, IMFSample* s, DWORD) {
        if (id) return MF_E_INVALIDSTREAMNUMBER; if (!s) return E_POINTER;
        if (!m_inSet || !m_outSet) return MF_E_TRANSFORM_TYPE_NOT_SET;
        if (m_haveInput) return MF_E_NOTACCEPTING;
        static LONG firstIn = 0;
        if (InterlockedExchange(&firstIn, 1) == 0) XavcLog("ProcessInput: first compressed sample");
        s->AddRef(); m_pending = s; m_haveInput = TRUE;
        if (FAILED(s->GetSampleTime(&m_time))) m_time = 0;
        if (FAILED(s->GetSampleDuration(&m_dur))) m_dur = (LONGLONG)(10000000.0 * m_fpsD / (m_fpsN ? m_fpsN : 30000));
        return S_OK;
    }

    STDMETHODIMP ProcessOutput(DWORD, DWORD count, MFT_OUTPUT_DATA_BUFFER* bufs, DWORD* status) {
        if (status) *status = 0;
        if (count < 1 || !bufs) return E_INVALIDARG;
        if (!m_haveInput) return MF_E_TRANSFORM_NEED_MORE_INPUT;
        IMFSample* outS = bufs[0].pSample;
        if (!outS) return E_INVALIDARG; // we don't provide samples
        IMFMediaBuffer* mb = NULL;
        HRESULT hr = outS->ConvertToContiguousBuffer(&mb);
        if (FAILED(hr) || !mb) return FAILED(hr) ? hr : E_FAIL;
        BYTE* p = NULL; DWORD maxLen = 0, curLen = 0;
        hr = mb->Lock(&p, &maxLen, &curLen);
        if (SUCCEEDED(hr)) {
            if (maxLen >= m_outSize) { FillMagenta(m_outSub, p, m_w, m_h); mb->SetCurrentLength(m_outSize); }
            else hr = E_OUTOFMEMORY;
            mb->Unlock();
        }
        mb->Release();
        if (FAILED(hr)) return hr;
        outS->SetSampleTime(m_time);
        outS->SetSampleDuration(m_dur);
        bufs[0].dwStatus = 0;
        m_pending->Release(); m_pending = NULL; m_haveInput = FALSE;
        static LONG once = 0;
        if (InterlockedExchange(&once, 1) == 0) XavcLog("ProcessOutput: emitted first magenta frame");
        return S_OK;
    }

private:
    HRESULT MakeVideoType(const GUID& sub, IMFMediaType** pp) {
        IMFMediaType* t=NULL; HRESULT hr=MFCreateMediaType(&t); if (FAILED(hr)) return hr;
        UINT32 stride=0,size=0; FormatInfo(sub, m_w, m_h, &stride, &size);
        t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        t->SetGUID(MF_MT_SUBTYPE, sub);
        t->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        t->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
        t->SetUINT32(MF_MT_FIXED_SIZE_SAMPLES, TRUE);
        t->SetUINT32(MF_MT_SAMPLE_SIZE, size);
        t->SetUINT32(MF_MT_DEFAULT_STRIDE, stride);
        MFSetAttributeSize(t, MF_MT_FRAME_SIZE, m_w, m_h);
        MFSetAttributeRatio(t, MF_MT_FRAME_RATE, m_fpsN, m_fpsD);
        MFSetAttributeRatio(t, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        *pp = t; return S_OK;
    }

    LONG m_cRef;
    BOOL m_haveInput, m_inSet, m_outSet;
    UINT32 m_w, m_h, m_fpsN, m_fpsD, m_outStride, m_outSize;
    GUID m_outSub;
    IMFSample* m_pending;
    LONGLONG m_time, m_dur;
};

// ----------------------- class factory -----------------------
class CFactory : public IClassFactory {
public:
    CFactory() : m_cRef(1) {}
    virtual ~CFactory() {}
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) {
        if (!ppv) return E_POINTER;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IClassFactory)) {
            *ppv = static_cast<IClassFactory*>(this); AddRef(); return S_OK;
        }
        *ppv=NULL; return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() { return InterlockedIncrement(&m_cRef); }
    STDMETHODIMP_(ULONG) Release() { LONG c=InterlockedDecrement(&m_cRef); if(!c) delete this; return c; }
    STDMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** ppv) {
        if (outer) return CLASS_E_NOAGGREGATION;
        XavcLog("Factory::CreateInstance");
        CXavcDecoder* d = new CXavcDecoder();
        if (!d) return E_OUTOFMEMORY;
        HRESULT hr = d->QueryInterface(riid, ppv);
        d->Release(); return hr;
    }
    STDMETHODIMP LockServer(BOOL lock) {
        if (lock) InterlockedIncrement(&g_cLock); else InterlockedDecrement(&g_cLock);
        return S_OK;
    }
private:
    LONG m_cRef;
};

// ----------------------- registration -----------------------
static const wchar_t* kFriendly = L"XAVC H.264 4:2:2 Decoder (diagnostic)";

// MFT_CATEGORY_VIDEO_DECODER
static const GUID CAT_VIDEO_DECODER =
    { 0xd6c02d4b, 0x6833, 0x45b4, { 0x97, 0x1a, 0x05, 0xa4, 0xb0, 0x4b, 0xab, 0x91 } };

// Merit to register with (0 = none). Set through DllInstall("merit=N") for the
// ordering experiments; MFTEnumEx sorts MFTs that have a merit ahead of the
// Preferred list, if Media Foundation honours it for this MFT.
static UINT32 g_regMerit = 0;

// An earlier build wrote "{GUID}"-style keys by hand. MFTRegister (and Windows'
// own MFTs) use brace-less GUID strings, so drop the leftovers.
static void RemoveLegacyBraceKeys() {
    wchar_t clsid[64]; StringFromGUID2(CLSID_XavcH264Decoder, clsid, 64);
    wchar_t cat[64];   StringFromGUID2(CAT_VIDEO_DECODER, cat, 64);
    wchar_t path[256];
    _snwprintf(path, 256, L"SOFTWARE\\Classes\\MediaFoundation\\Transforms\\Categories\\%ls\\%ls", cat, clsid);
    RegDeleteKeyW(HKEY_LOCAL_MACHINE, path);
    _snwprintf(path, 256, L"SOFTWARE\\Classes\\MediaFoundation\\Transforms\\Categories\\%ls", cat);
    RegDeleteKeyW(HKEY_LOCAL_MACHINE, path);   // fails harmlessly unless now empty
    _snwprintf(path, 256, L"SOFTWARE\\Classes\\MediaFoundation\\Transforms\\%ls", clsid);
    RegDeleteKeyW(HKEY_LOCAL_MACHINE, path);
}

static HRESULT RegisterMFT() {
    MFT_REGISTER_TYPE_INFO in[1]  = { { MFMediaType_Video, XAVC_VideoFormat_H264 } };
    MFT_REGISTER_TYPE_INFO out[3] = { { MFMediaType_Video, XAVC_VideoFormat_NV12 },
                                      { MFMediaType_Video, XAVC_VideoFormat_P010 },
                                      { MFMediaType_Video, XAVC_VideoFormat_Y210 } };
    IMFAttributes* attrs = NULL;
    if (g_regMerit && SUCCEEDED(MFCreateAttributes(&attrs, 1)))
        attrs->SetUINT32(MFT_CODEC_MERIT_Attribute, g_regMerit);
    // MFTRegister does not remove values from a previous registration (e.g. an old
    // "Attributes" blob), so start from a clean key.
    MFTUnregister(CLSID_XavcH264Decoder);
    HRESULT hr = MFTRegister(CLSID_XavcH264Decoder, CAT_VIDEO_DECODER, (LPWSTR)kFriendly,
                             MFT_ENUM_FLAG_SYNCMFT, 1, in, 3, out, attrs);
    if (attrs) attrs->Release();
    XavcLog("MFTRegister merit=%u hr=0x%08lx", g_regMerit, (unsigned long)hr);
    return hr;
}

static void UnregisterMFT() {
    MFTUnregister(CLSID_XavcH264Decoder);
    RemoveLegacyBraceKeys();
}

static HRESULT WriteClsidKeys() {
    wchar_t guid[64]; StringFromGUID2(CLSID_XavcH264Decoder, guid, 64);
    wchar_t keyInproc[160];
    _snwprintf(keyInproc, 160, L"CLSID\\%ls\\InprocServer32", guid);
    wchar_t path[MAX_PATH]; GetModuleFileNameW(g_hInst, path, MAX_PATH);

    HKEY k;
    wchar_t keyClsid[96]; _snwprintf(keyClsid, 96, L"CLSID\\%ls", guid);
    if (RegCreateKeyExW(HKEY_CLASSES_ROOT, keyClsid, 0, NULL, 0, KEY_WRITE, NULL, &k, NULL) == ERROR_SUCCESS) {
        RegSetValueExW(k, NULL, 0, REG_SZ, (const BYTE*)kFriendly, (DWORD)((wcslen(kFriendly)+1)*2));
        RegCloseKey(k);
    }
    if (RegCreateKeyExW(HKEY_CLASSES_ROOT, keyInproc, 0, NULL, 0, KEY_WRITE, NULL, &k, NULL) != ERROR_SUCCESS)
        return E_ACCESSDENIED;
    RegSetValueExW(k, NULL, 0, REG_SZ, (const BYTE*)path, (DWORD)((wcslen(path)+1)*2));
    const wchar_t* both = L"Both";
    RegSetValueExW(k, L"ThreadingModel", 0, REG_SZ, (const BYTE*)both, (DWORD)((wcslen(both)+1)*2));
    RegCloseKey(k);
    return S_OK;
}

STDAPI DllRegisterServer() {
    XavcLog("DllRegisterServer");
    HRESULT hr = WriteClsidKeys();
    if (FAILED(hr)) { XavcLog("WriteClsidKeys failed 0x%08lx", (unsigned long)hr); return hr; }
    RemoveLegacyBraceKeys();
    MFStartup(MF_VERSION, MFSTARTUP_LITE);
    hr = RegisterMFT();
    MFShutdown();
    return hr;
}

STDAPI DllUnregisterServer();

// regsvr32 /n /i:"merit=8" xavc_decoder.dll registers with a merit attribute.
STDAPI DllInstall(BOOL install, PCWSTR cmd) {
    if (!install) return DllUnregisterServer();
    g_regMerit = 0;
    const wchar_t* m = cmd ? wcsstr(cmd, L"merit=") : NULL;
    if (m) g_regMerit = (UINT32)wcstoul(m + 6, NULL, 0);
    return DllRegisterServer();
}

STDAPI DllUnregisterServer() {
    XavcLog("DllUnregisterServer");
    MFStartup(MF_VERSION, MFSTARTUP_LITE);
    UnregisterMFT();
    MFShutdown();
    wchar_t guid[64]; StringFromGUID2(CLSID_XavcH264Decoder, guid, 64);
    wchar_t keyClsid[96]; _snwprintf(keyClsid, 96, L"CLSID\\%ls", guid);
    wchar_t sub[160];
    _snwprintf(sub, 160, L"%ls\\InprocServer32", keyClsid);
    RegDeleteKeyW(HKEY_CLASSES_ROOT, sub);
    RegDeleteKeyW(HKEY_CLASSES_ROOT, keyClsid);
    return S_OK;
}

STDAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    if (!IsEqualCLSID(clsid, CLSID_XavcH264Decoder)) return CLASS_E_CLASSNOTAVAILABLE;
    XavcLog("DllGetClassObject");
    CFactory* f = new CFactory();
    if (!f) return E_OUTOFMEMORY;
    HRESULT hr = f->QueryInterface(riid, ppv);
    f->Release(); return hr;
}

STDAPI DllCanUnloadNow() {
    return (g_cObj == 0 && g_cLock == 0) ? S_OK : S_FALSE;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) { g_hInst = h; DisableThreadLibraryCalls(h); }
    return TRUE;
}
