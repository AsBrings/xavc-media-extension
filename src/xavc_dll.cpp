// xavc_dll.cpp - DLL entry points: class factory and (un)registration.
//
// Two COM classes live in xavc_decoder.dll:
//   CLSID_XavcH264Decoder       the decoder MFT, registered with Media Foundation for the
//                               private 'XAVC' subtype only (no merit: see below)
//   CLSID_XavcByteStreamHandler the MP4 byte stream handler that retags 4:2:2 / 10-bit H.264
// regsvr32 registers both COM classes and the MFT (machine-wide, needs Administrator).
// Making Media Foundation actually use them is a per-user setting written by the installer.

#include "xavc_common.h"
#include "xavc_ffmpeg.h"
#include <new>

HMODULE g_hInst = NULL;
volatile LONG g_cObj = 0;           // live objects (decoders + byte stream handlers)
static volatile LONG g_cLock = 0;   // IClassFactory::LockServer

// Global new/delete on malloc/free. libstdc++'s versions bring in its exception machinery,
// which needs libwinpthread-1.dll at run time - a DLL host processes cannot find. All our
// allocations use new (std::nothrow) and check for NULL.
void* operator new(size_t n) { return malloc(n ? n : 1); }
void* operator new[](size_t n) { return malloc(n ? n : 1); }
void* operator new(size_t n, const std::nothrow_t&) noexcept { return malloc(n ? n : 1); }
void* operator new[](size_t n, const std::nothrow_t&) noexcept { return malloc(n ? n : 1); }
void operator delete(void* p) noexcept { free(p); }
void operator delete[](void* p) noexcept { free(p); }
void operator delete(void* p, size_t) noexcept { free(p); }
void operator delete[](void* p, size_t) noexcept { free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { free(p); }

namespace {

typedef HRESULT (*CreateFn)(REFIID riid, void** ppv);

class CFactory : public IClassFactory {
public:
    explicit CFactory(CreateFn fn) : m_cRef(1), m_fn(fn) {}
    virtual ~CFactory() {}
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) {
        if (!ppv) return E_POINTER;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IClassFactory)) {
            *ppv = static_cast<IClassFactory*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = NULL;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() { return InterlockedIncrement(&m_cRef); }
    STDMETHODIMP_(ULONG) Release() {
        LONG c = InterlockedDecrement(&m_cRef);
        if (!c) delete this;
        return c;
    }
    STDMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** ppv) {
        if (!ppv) return E_POINTER;
        *ppv = NULL;
        if (outer) return CLASS_E_NOAGGREGATION;
        return m_fn(riid, ppv);
    }
    STDMETHODIMP LockServer(BOOL lock) {
        if (lock) InterlockedIncrement(&g_cLock); else InterlockedDecrement(&g_cLock);
        return S_OK;
    }
private:
    LONG m_cRef;
    CreateFn m_fn;
};

const wchar_t* kDecoderName = L"XAVC H.264 4:2:2 Decoder";
const wchar_t* kHandlerName = L"XAVC Byte Stream Handler";

// MFT_CATEGORY_VIDEO_DECODER
const GUID kCatVideoDecoder = { 0xd6c02d4b, 0x6833, 0x45b4, { 0x97, 0x1a, 0x05, 0xa4, 0xb0, 0x4b, 0xab, 0x91 } };

// An early build wrote "{GUID}"-style MFT keys by hand; MFTRegister uses brace-less GUID
// strings. Remove the leftovers.
void RemoveLegacyBraceKeys() {
    wchar_t clsid[64], cat[64], path[256];
    StringFromGUID2(CLSID_XavcH264Decoder, clsid, 64);
    StringFromGUID2(kCatVideoDecoder, cat, 64);
    _snwprintf(path, 256, L"SOFTWARE\\Classes\\MediaFoundation\\Transforms\\Categories\\%ls\\%ls", cat, clsid);
    RegDeleteKeyW(HKEY_LOCAL_MACHINE, path);
    _snwprintf(path, 256, L"SOFTWARE\\Classes\\MediaFoundation\\Transforms\\Categories\\%ls", cat);
    RegDeleteKeyW(HKEY_LOCAL_MACHINE, path);       // fails harmlessly unless empty
    _snwprintf(path, 256, L"SOFTWARE\\Classes\\MediaFoundation\\Transforms\\%ls", clsid);
    RegDeleteKeyW(HKEY_LOCAL_MACHINE, path);
}

HRESULT RegisterMFT() {
    // Only the private 'XAVC' subtype: the decoder is never in the H.264 decoder list, so
    // ordinary H.264 playback cannot be affected. No MFT_CODEC_MERIT_Attribute: mfplat
    // verifies a registered merit against a certificate (MFGetMFTMerit) and drops MFTs that
    // fail the check.
    MFT_REGISTER_TYPE_INFO in[1] = { { MFMediaType_Video, XAVC_VideoFormat_XAVC } };
    MFT_REGISTER_TYPE_INFO out[4] = { { MFMediaType_Video, XAVC_VideoFormat_Y210 },
                                      { MFMediaType_Video, XAVC_VideoFormat_YUY2 },
                                      { MFMediaType_Video, XAVC_VideoFormat_P010 },
                                      { MFMediaType_Video, XAVC_VideoFormat_NV12 } };
    MFTUnregister(CLSID_XavcH264Decoder);   // MFTRegister keeps stale values otherwise
    HRESULT hr = MFTRegister(CLSID_XavcH264Decoder, kCatVideoDecoder, (LPWSTR)kDecoderName,
                             MFT_ENUM_FLAG_SYNCMFT, 1, in, 4, out, NULL);
    XavcLog("MFTRegister hr=0x%08lx", (unsigned long)hr);
    return hr;
}

// HKCR\CLSID\{clsid}\InprocServer32 -> this DLL, ThreadingModel Both.
HRESULT WriteClsidKey(REFCLSID clsid, const wchar_t* friendly) {
    wchar_t guid[64], keyClsid[96], keyInproc[160], path[MAX_PATH];
    StringFromGUID2(clsid, guid, 64);
    _snwprintf(keyClsid, 96, L"CLSID\\%ls", guid);
    _snwprintf(keyInproc, 160, L"CLSID\\%ls\\InprocServer32", guid);
    DWORD n = GetModuleFileNameW(g_hInst, path, MAX_PATH);
    if (!n || n >= MAX_PATH) return E_FAIL;

    HKEY k;
    if (RegCreateKeyExW(HKEY_CLASSES_ROOT, keyClsid, 0, NULL, 0, KEY_WRITE, NULL, &k, NULL) != ERROR_SUCCESS)
        return E_ACCESSDENIED;
    RegSetValueExW(k, NULL, 0, REG_SZ, (const BYTE*)friendly, (DWORD)((wcslen(friendly) + 1) * 2));
    RegCloseKey(k);
    if (RegCreateKeyExW(HKEY_CLASSES_ROOT, keyInproc, 0, NULL, 0, KEY_WRITE, NULL, &k, NULL) != ERROR_SUCCESS)
        return E_ACCESSDENIED;
    RegSetValueExW(k, NULL, 0, REG_SZ, (const BYTE*)path, (DWORD)((wcslen(path) + 1) * 2));
    const wchar_t* both = L"Both";
    RegSetValueExW(k, L"ThreadingModel", 0, REG_SZ, (const BYTE*)both, (DWORD)((wcslen(both) + 1) * 2));
    RegCloseKey(k);
    return S_OK;
}

void DeleteClsidKey(REFCLSID clsid) {
    wchar_t guid[64], keyClsid[96], sub[160];
    StringFromGUID2(clsid, guid, 64);
    _snwprintf(keyClsid, 96, L"CLSID\\%ls", guid);
    _snwprintf(sub, 160, L"%ls\\InprocServer32", keyClsid);
    RegDeleteKeyW(HKEY_CLASSES_ROOT, sub);
    RegDeleteKeyW(HKEY_CLASSES_ROOT, keyClsid);
}

} // namespace

STDAPI DllRegisterServer() {
    XavcLog("DllRegisterServer");
    HRESULT hr = WriteClsidKey(CLSID_XavcH264Decoder, kDecoderName);
    if (SUCCEEDED(hr)) hr = WriteClsidKey(CLSID_XavcByteStreamHandler, kHandlerName);
    if (FAILED(hr)) return hr;
    RemoveLegacyBraceKeys();
    MFStartup(MF_VERSION, MFSTARTUP_LITE);
    hr = RegisterMFT();
    MFShutdown();
    return hr;
}

STDAPI DllUnregisterServer() {
    XavcLog("DllUnregisterServer");
    MFStartup(MF_VERSION, MFSTARTUP_LITE);
    MFTUnregister(CLSID_XavcH264Decoder);
    MFShutdown();
    RemoveLegacyBraceKeys();
    DeleteClsidKey(CLSID_XavcH264Decoder);
    DeleteClsidKey(CLSID_XavcByteStreamHandler);
    return S_OK;
}

STDAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    *ppv = NULL;
    CreateFn fn = NULL;
    if (IsEqualCLSID(clsid, CLSID_XavcH264Decoder)) fn = XavcCreateDecoder;
    else if (IsEqualCLSID(clsid, CLSID_XavcByteStreamHandler)) fn = XavcCreateByteStreamHandler;
    else return CLASS_E_CLASSNOTAVAILABLE;
    CFactory* f = new (std::nothrow) CFactory(fn);
    if (!f) return E_OUTOFMEMORY;
    HRESULT hr = f->QueryInterface(riid, ppv);
    f->Release();
    return hr;
}

STDAPI DllCanUnloadNow() {
    // Once FFmpeg is loaded we stay: its log callback points into this DLL.
    return (g_cObj == 0 && g_cLock == 0 && !XavcFFmpegLoaded()) ? S_OK : S_FALSE;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_hInst = h;
        DisableThreadLibraryCalls(h);
    }
    return TRUE;
}
