// xavc_bsh.cpp - XAVC byte stream handler.
//
// Media Foundation always offers H.264 streams to the in-box "Microsoft H264 Video
// Decoder MFT" first: mfplat.dll's SortAndFilterMFTs places it in a hardcoded priority
// tier ahead of merit and the Preferred list. That decoder accepts H.264 High 4:2:2
// (Sony XAVC S / S-I) at SetInputType and then fails at decode time, and the pipeline
// never falls back to another decoder.
//
// This handler sits in front of the system MPEG-4 byte stream handler (registered per
// user under HKCU\Software\Microsoft\Windows Media Foundation\ByteStreamHandlers, which
// mfplat.dll reads before the HKLM list). It lets the system handler create the media
// source unchanged, then retags only the video streams the in-box decoder cannot decode
// (profile_idc 110/122/244/44) with the private 'XAVC' subtype. Those streams can then
// only be matched by our decoder; everything else passes through untouched.

#include "xavc_common.h"
#include <new>

namespace {

// Retag H.264 media types the in-box decoder cannot handle. Media types are shared
// between the source's stream descriptors and the presentation descriptor copies
// (IMFPresentationDescriptor::Clone is shallow), so patching them in place is enough.
UINT32 PatchMediaTypeHandler(IMFMediaTypeHandler* mth) {
    UINT32 patched = 0;
    DWORD count = 0;
    if (FAILED(mth->GetMediaTypeCount(&count))) return 0;
    for (DWORD i = 0; i < count; i++) {
        IMFMediaType* mt = NULL;
        if (FAILED(mth->GetMediaTypeByIndex(i, &mt)) || !mt) continue;
        GUID sub = GUID_NULL;
        if (SUCCEEDED(mt->GetGUID(MF_MT_SUBTYPE, &sub)) && IsEqualGUID(sub, XAVC_VideoFormat_H264)) {
            int profile = XavcProfileOfMediaType(mt);
            if (XavcIsHigh422Profile(profile) &&
                SUCCEEDED(mt->SetGUID(MF_MT_SUBTYPE, XAVC_VideoFormat_XAVC))) {
                patched++;
                XavcLog("BSH: stream type %lu: H.264 profile %d -> XAVC", (unsigned long)i, profile);
            }
        }
        mt->Release();
    }
    IMFMediaType* cur = NULL;
    if (SUCCEEDED(mth->GetCurrentMediaType(&cur)) && cur) {
        GUID sub = GUID_NULL;
        if (SUCCEEDED(cur->GetGUID(MF_MT_SUBTYPE, &sub)) && IsEqualGUID(sub, XAVC_VideoFormat_H264) &&
            XavcIsHigh422Profile(XavcProfileOfMediaType(cur)) &&
            SUCCEEDED(cur->SetGUID(MF_MT_SUBTYPE, XAVC_VideoFormat_XAVC))) {
            patched++;
            XavcLog("BSH: current type H264 -> XAVC");
        }
        cur->Release();
    }
    return patched;
}

void PatchSource(IMFMediaSource* src) {
    IMFPresentationDescriptor* pd = NULL;
    if (FAILED(src->CreatePresentationDescriptor(&pd)) || !pd) return;
    DWORD n = 0;
    pd->GetStreamDescriptorCount(&n);
    UINT32 patched = 0;
    for (DWORD i = 0; i < n; i++) {
        BOOL selected = FALSE;
        IMFStreamDescriptor* sd = NULL;
        if (FAILED(pd->GetStreamDescriptorByIndex(i, &selected, &sd)) || !sd) continue;
        IMFMediaTypeHandler* mth = NULL;
        if (SUCCEEDED(sd->GetMediaTypeHandler(&mth)) && mth) {
            GUID major = GUID_NULL;
            if (SUCCEEDED(mth->GetMajorType(&major)) && IsEqualGUID(major, MFMediaType_Video))
                patched += PatchMediaTypeHandler(mth);
            mth->Release();
        }
        sd->Release();
    }
    pd->Release();

    // Trace: verify the change is visible through a fresh presentation descriptor.
    if (patched && XavcGetSettings().logLevel >= 2 && SUCCEEDED(src->CreatePresentationDescriptor(&pd)) && pd) {
        for (DWORD i = 0; i < n; i++) {
            BOOL selected = FALSE;
            IMFStreamDescriptor* sd = NULL;
            if (FAILED(pd->GetStreamDescriptorByIndex(i, &selected, &sd)) || !sd) continue;
            IMFMediaTypeHandler* mth = NULL;
            IMFMediaType* cur = NULL;
            if (SUCCEEDED(sd->GetMediaTypeHandler(&mth)) && mth &&
                SUCCEEDED(mth->GetCurrentMediaType(&cur)) && cur) {
                GUID sub = GUID_NULL;
                cur->GetGUID(MF_MT_SUBTYPE, &sub);
                XavcTrace("BSH: verify stream %lu current subtype %08lx", (unsigned long)i, (unsigned long)sub.Data1);
            }
            if (cur) cur->Release();
            if (mth) mth->Release();
            sd->Release();
        }
        pd->Release();
    }
    XavcTrace("BSH: patched %u media type(s)", patched);
}

// IMFAttributes: the source resolver reads MF_BYTESTREAMHANDLER_ACCEPTS_SHARE_WRITE from a
// handler before using it for a file another process has open for writing (a copy in progress,
// a camera / tethering app). The system MPEG-4 handler sets it; without it here the resolver
// would skip us for such files and the 4:2:2 stream would end up at the in-box decoder.
class CXavcByteStreamHandler : public IMFByteStreamHandler, public IMFAttributes {
public:
    CXavcByteStreamHandler() : m_cRef(1), m_inner(NULL), m_attrs(NULL) { InterlockedIncrement(&g_cObj); }
    virtual ~CXavcByteStreamHandler() {
        if (m_inner) m_inner->Release();
        if (m_attrs) m_attrs->Release();
        InterlockedDecrement(&g_cObj);
    }

    HRESULT Init() {
        HRESULT hr = MFCreateAttributes(&m_attrs, 1);
        if (SUCCEEDED(hr)) hr = m_attrs->SetUINT32(XAVC_MF_BYTESTREAMHANDLER_ACCEPTS_SHARE_WRITE, TRUE);
        return hr;
    }

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) {
        if (!ppv) return E_POINTER;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, XAVC_IID_IMFByteStreamHandler)) {
            *ppv = static_cast<IMFByteStreamHandler*>(this);
        } else if (IsEqualIID(riid, IID_IMFAttributes)) {
            *ppv = static_cast<IMFAttributes*>(this);
        } else {
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

    STDMETHODIMP BeginCreateObject(IMFByteStream* bs, LPCWSTR url, DWORD flags, IPropertyStore* props,
                                   IUnknown** cancelCookie, IMFAsyncCallback* cb, IUnknown* state) {
        char u8[260] = "";
        if (url) WideCharToMultiByte(CP_UTF8, 0, url, -1, u8, sizeof(u8) - 1, NULL, NULL);
        XavcTrace("BSH: BeginCreateObject flags=0x%lx url=%s", (unsigned long)flags, u8);
        HRESULT hr = EnsureInner();
        if (FAILED(hr)) return hr;
        // The caller's callback is handed straight to the system handler; the caller then
        // calls our EndCreateObject with that handler's async result, which we forward.
        return m_inner->BeginCreateObject(bs, url, flags, props, cancelCookie, cb, state);
    }

    STDMETHODIMP EndCreateObject(IMFAsyncResult* result, MF_OBJECT_TYPE* type, IUnknown** obj) {
        if (!m_inner) return E_UNEXPECTED;
        HRESULT hr = m_inner->EndCreateObject(result, type, obj);
        XavcTrace("BSH: EndCreateObject hr=0x%08lx type=%d", (unsigned long)hr, type ? (int)*type : -1);
        if (SUCCEEDED(hr) && type && *type == MF_OBJECT_MEDIASOURCE && obj && *obj) {
            IMFMediaSource* src = NULL;
            if (SUCCEEDED((*obj)->QueryInterface(IID_IMFMediaSource, (void**)&src)) && src) {
                PatchSource(src);
                src->Release();
            }
        }
        return hr;
    }

    STDMETHODIMP CancelObjectCreation(IUnknown* cancelCookie) {
        return m_inner ? m_inner->CancelObjectCreation(cancelCookie) : E_UNEXPECTED;
    }

    // IMFAttributes, forwarded to our attribute store
    STDMETHODIMP GetItem(REFGUID k, PROPVARIANT* v) { return m_attrs->GetItem(k, v); }
    STDMETHODIMP GetItemType(REFGUID k, MF_ATTRIBUTE_TYPE* t) { return m_attrs->GetItemType(k, t); }
    STDMETHODIMP CompareItem(REFGUID k, REFPROPVARIANT v, BOOL* r) { return m_attrs->CompareItem(k, v, r); }
    STDMETHODIMP Compare(IMFAttributes* o, MF_ATTRIBUTES_MATCH_TYPE m, BOOL* r) { return m_attrs->Compare(o, m, r); }
    STDMETHODIMP GetUINT32(REFGUID k, UINT32* v) { return m_attrs->GetUINT32(k, v); }
    STDMETHODIMP GetUINT64(REFGUID k, UINT64* v) { return m_attrs->GetUINT64(k, v); }
    STDMETHODIMP GetDouble(REFGUID k, double* v) { return m_attrs->GetDouble(k, v); }
    STDMETHODIMP GetGUID(REFGUID k, GUID* v) { return m_attrs->GetGUID(k, v); }
    STDMETHODIMP GetStringLength(REFGUID k, UINT32* n) { return m_attrs->GetStringLength(k, n); }
    STDMETHODIMP GetString(REFGUID k, LPWSTR s, UINT32 n, UINT32* len) { return m_attrs->GetString(k, s, n, len); }
    STDMETHODIMP GetAllocatedString(REFGUID k, LPWSTR* s, UINT32* n) { return m_attrs->GetAllocatedString(k, s, n); }
    STDMETHODIMP GetBlobSize(REFGUID k, UINT32* n) { return m_attrs->GetBlobSize(k, n); }
    STDMETHODIMP GetBlob(REFGUID k, UINT8* b, UINT32 n, UINT32* len) { return m_attrs->GetBlob(k, b, n, len); }
    STDMETHODIMP GetAllocatedBlob(REFGUID k, UINT8** b, UINT32* n) { return m_attrs->GetAllocatedBlob(k, b, n); }
    STDMETHODIMP GetUnknown(REFGUID k, REFIID riid, LPVOID* ppv) { return m_attrs->GetUnknown(k, riid, ppv); }
    STDMETHODIMP SetItem(REFGUID k, REFPROPVARIANT v) { return m_attrs->SetItem(k, v); }
    STDMETHODIMP DeleteItem(REFGUID k) { return m_attrs->DeleteItem(k); }
    STDMETHODIMP DeleteAllItems() { return m_attrs->DeleteAllItems(); }
    STDMETHODIMP SetUINT32(REFGUID k, UINT32 v) { return m_attrs->SetUINT32(k, v); }
    STDMETHODIMP SetUINT64(REFGUID k, UINT64 v) { return m_attrs->SetUINT64(k, v); }
    STDMETHODIMP SetDouble(REFGUID k, double v) { return m_attrs->SetDouble(k, v); }
    STDMETHODIMP SetGUID(REFGUID k, REFGUID v) { return m_attrs->SetGUID(k, v); }
    STDMETHODIMP SetString(REFGUID k, LPCWSTR v) { return m_attrs->SetString(k, v); }
    STDMETHODIMP SetBlob(REFGUID k, const UINT8* b, UINT32 n) { return m_attrs->SetBlob(k, b, n); }
    STDMETHODIMP SetUnknown(REFGUID k, IUnknown* u) { return m_attrs->SetUnknown(k, u); }
    STDMETHODIMP LockStore() { return m_attrs->LockStore(); }
    STDMETHODIMP UnlockStore() { return m_attrs->UnlockStore(); }
    STDMETHODIMP GetCount(UINT32* n) { return m_attrs->GetCount(n); }
    STDMETHODIMP GetItemByIndex(UINT32 i, GUID* k, PROPVARIANT* v) { return m_attrs->GetItemByIndex(i, k, v); }
    STDMETHODIMP CopyAllItems(IMFAttributes* d) { return m_attrs->CopyAllItems(d); }

    STDMETHODIMP GetMaxNumberOfBytesRequiredForResolution(QWORD* pqw) {
        HRESULT hr = EnsureInner();
        if (FAILED(hr)) return hr;
        return m_inner->GetMaxNumberOfBytesRequiredForResolution(pqw);
    }

private:
    // Thread-safe: a caller may use one handler object from several threads.
    HRESULT EnsureInner() {
        if (m_inner) return S_OK;
        IMFByteStreamHandler* h = NULL;
        HRESULT hr = CoCreateInstance(CLSID_SystemMpeg4ByteStreamHandler, NULL, CLSCTX_INPROC_SERVER,
                                      XAVC_IID_IMFByteStreamHandler, (void**)&h);
        if (FAILED(hr)) {
            XavcLog("BSH: cannot create system MPEG-4 handler hr=0x%08lx", (unsigned long)hr);
            return hr;
        }
        if (InterlockedCompareExchangePointer((PVOID volatile*)&m_inner, h, NULL) != NULL) h->Release();
        return S_OK;
    }

    LONG m_cRef;
    IMFByteStreamHandler* volatile m_inner;
    IMFAttributes* m_attrs;
};

} // namespace

HRESULT XavcCreateByteStreamHandler(REFIID riid, void** ppv) {
    CXavcByteStreamHandler* h = new (std::nothrow) CXavcByteStreamHandler();
    if (!h) return E_OUTOFMEMORY;
    HRESULT hr = h->Init();
    if (SUCCEEDED(hr)) hr = h->QueryInterface(riid, ppv);
    h->Release();
    return hr;
}
