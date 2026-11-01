// xavc_userreg.cpp - per-user registration (regsvr32 /n /i:user xavc_decoder.dll; no admin rights).
//
// The machine-wide part (DllRegisterServer: COM classes + the MFT for the 'XAVC' subtype) only
// makes our components available. Whether Media Foundation uses them is decided per user, by
// settings that mfplat.dll reads from HKCU before HKLM:
//   HKCU\Software\Microsoft\Windows Media Foundation\ByteStreamHandlers\{.mp4, video/mp4}
//       our byte stream handler becomes a candidate for MP4 files,
//   HKCU\Software\Classes\MediaFoundation\MediaSources\Preferred\{.mp4, video/mp4}
//       ... and the preferred one (HKCR merges HKCU over HKLM value by value),
//   HKCU\Software\Classes\MediaFoundation\Transforms\Preferred\{XAVC subtype}
//       our decoder is "approved" for the private subtype (sandboxed hosts only load approved
//       plug-ins), and
//   read access for ALL APPLICATION PACKAGES on the per-user Media Foundation key, so the
//   Media Player (an AppContainer) can read the handler list.
// Values that were there before are saved under HKCU\Software\XavcDecoder\Saved and put back
// on removal. If the machine part is uninstalled first, leftover per-user entries are
// harmless: Media Foundation skips a handler whose class is not registered.

#include "xavc_common.h"
#include <aclapi.h>
#include <sddl.h>

namespace {

const wchar_t* kWmfKey      = L"Software\\Microsoft\\Windows Media Foundation";
const wchar_t* kBshKey      = L"Software\\Microsoft\\Windows Media Foundation\\ByteStreamHandlers";
const wchar_t* kSrcPrefKey  = L"Software\\Classes\\MediaFoundation\\MediaSources\\Preferred";
const wchar_t* kMftPrefKey  = L"Software\\Classes\\MediaFoundation\\Transforms\\Preferred";
const wchar_t* kOurKey      = L"Software\\XavcDecoder";
const wchar_t* kSavedKey    = L"Software\\XavcDecoder\\Saved";
const wchar_t* kSelectors[] = { L".mp4", L"video/mp4" };
const wchar_t* kAllAppPackagesSid = L"S-1-15-2-1";
// Per-user marker Active Setup writes once it ran our StubPath for this user (installer).
const wchar_t* kActiveSetupKey = L"Software\\Microsoft\\Active Setup\\Installed Components\\{7F9A3C2F-5E44-4B0A-9C1D-2A6B8E3D4F10}";

void GuidString(REFGUID g, wchar_t* out) { StringFromGUID2(g, out, 64); }

LONG SetString(HKEY root, const wchar_t* sub, const wchar_t* name, const wchar_t* value) {
    HKEY k;
    LONG r = RegCreateKeyExW(root, sub, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL);
    if (r != ERROR_SUCCESS) return r;
    r = RegSetValueExW(k, name, 0, REG_SZ, (const BYTE*)value, (DWORD)((wcslen(value) + 1) * sizeof(wchar_t)));
    RegCloseKey(k);
    return r;
}

bool GetString(HKEY root, const wchar_t* sub, const wchar_t* name, wchar_t* out, DWORD chars) {
    DWORD cb = chars * sizeof(wchar_t);
    DWORD type = 0;
    LONG r = RegGetValueW(root, sub, name, RRF_RT_REG_SZ, &type, out, &cb);
    return r == ERROR_SUCCESS;
}

void DeleteValue(HKEY root, const wchar_t* sub, const wchar_t* name) {
    HKEY k;
    if (RegOpenKeyExW(root, sub, 0, KEY_SET_VALUE, &k) == ERROR_SUCCESS) {
        RegDeleteValueW(k, name);
        RegCloseKey(k);
    }
}

// Deletes root\parent\child if it has neither values nor subkeys.
void DeleteKeyIfEmpty(HKEY root, const wchar_t* parent, const wchar_t* child) {
    wchar_t path[512];
    _snwprintf(path, 512, L"%ls\\%ls", parent, child);
    path[511] = 0;
    HKEY k;
    if (RegOpenKeyExW(root, path, 0, KEY_READ, &k) != ERROR_SUCCESS) return;
    DWORD subkeys = 1, values = 1;
    RegQueryInfoKeyW(k, NULL, NULL, NULL, &subkeys, NULL, NULL, &values, NULL, NULL, NULL, NULL);
    RegCloseKey(k);
    if (subkeys == 0 && values == 0) RegDeleteKeyW(root, path);
}

// Sets root\sub:name = ours, saving a different previous value under the Saved key first.
LONG SetPreferred(const wchar_t* sub, const wchar_t* name, const wchar_t* ours, const wchar_t* savedName) {
    wchar_t prev[128];
    if (GetString(HKEY_CURRENT_USER, sub, name, prev, 128) && _wcsicmp(prev, ours) != 0)
        SetString(HKEY_CURRENT_USER, kSavedKey, savedName, prev);
    return SetString(HKEY_CURRENT_USER, sub, name, ours);
}

// Removes root\sub:name if it is ours, restoring a saved previous value.
void RestorePreferred(const wchar_t* sub, const wchar_t* name, const wchar_t* ours, const wchar_t* savedName) {
    wchar_t cur[128], saved[128];
    if (GetString(HKEY_CURRENT_USER, sub, name, cur, 128) && _wcsicmp(cur, ours) == 0) {
        if (GetString(HKEY_CURRENT_USER, kSavedKey, savedName, saved, 128))
            SetString(HKEY_CURRENT_USER, sub, name, saved);
        else
            DeleteValue(HKEY_CURRENT_USER, sub, name);
    }
}

// Does the DACL already give ALL APPLICATION PACKAGES inheritable read access?
bool HasAppContainerRead(PACL dacl, PSID sid) {
    if (!dacl) return true;                       // NULL DACL: everyone has access
    ACL_SIZE_INFORMATION info;
    if (!GetAclInformation(dacl, &info, sizeof(info), AclSizeInformation)) return false;
    for (DWORD i = 0; i < info.AceCount; i++) {
        ACCESS_ALLOWED_ACE* ace = NULL;
        if (!GetAce(dacl, i, (void**)&ace) || ace->Header.AceType != ACCESS_ALLOWED_ACE_TYPE) continue;
        if (EqualSid((PSID)&ace->SidStart, sid) && (ace->Mask & KEY_READ) == KEY_READ &&
            (ace->Header.AceFlags & CONTAINER_INHERIT_ACE))
            return true;
    }
    return false;
}

// Grants ALL APPLICATION PACKAGES read access (inherited by subkeys) on HKCU\<sub>.
// Returns S_OK if added, S_FALSE if it was already there.
HRESULT GrantAppContainerRead(const wchar_t* sub) {
    HKEY k;
    LONG r = RegCreateKeyExW(HKEY_CURRENT_USER, sub, 0, NULL, 0, READ_CONTROL | WRITE_DAC, NULL, &k, NULL);
    if (r != ERROR_SUCCESS) return HRESULT_FROM_WIN32(r);
    PSID sid = NULL;
    PACL dacl = NULL, newDacl = NULL;
    PSECURITY_DESCRIPTOR sd = NULL;
    HRESULT hr = S_OK;
    if (!ConvertStringSidToSidW(kAllAppPackagesSid, &sid)) {
        hr = HRESULT_FROM_WIN32(GetLastError());
    } else if ((r = GetSecurityInfo(k, SE_REGISTRY_KEY, DACL_SECURITY_INFORMATION, NULL, NULL, &dacl, NULL, &sd)) != ERROR_SUCCESS) {
        hr = HRESULT_FROM_WIN32(r);
    } else if (HasAppContainerRead(dacl, sid)) {
        hr = S_FALSE;
    } else {
        EXPLICIT_ACCESSW ea;
        memset(&ea, 0, sizeof(ea));
        ea.grfAccessPermissions = KEY_READ;
        ea.grfAccessMode = GRANT_ACCESS;
        ea.grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
        ea.Trustee.TrusteeForm = TRUSTEE_IS_SID;
        ea.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
        ea.Trustee.ptstrName = (LPWSTR)sid;
        r = SetEntriesInAclW(1, &ea, dacl, &newDacl);
        if (r == ERROR_SUCCESS)
            r = SetSecurityInfo(k, SE_REGISTRY_KEY, DACL_SECURITY_INFORMATION, NULL, NULL, newDacl, NULL);
        hr = HRESULT_FROM_WIN32(r);
    }
    if (newDacl) LocalFree(newDacl);
    if (sd) LocalFree(sd);
    if (sid) LocalFree(sid);
    RegCloseKey(k);
    return hr;
}

// Removes the ALL APPLICATION PACKAGES grant from HKCU\<sub> (only called if we added it).
void RevokeAppContainerRead(const wchar_t* sub) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, sub, 0, READ_CONTROL | WRITE_DAC, &k) != ERROR_SUCCESS) return;
    PSID sid = NULL;
    PACL dacl = NULL, newDacl = NULL;
    PSECURITY_DESCRIPTOR sd = NULL;
    if (ConvertStringSidToSidW(kAllAppPackagesSid, &sid) &&
        GetSecurityInfo(k, SE_REGISTRY_KEY, DACL_SECURITY_INFORMATION, NULL, NULL, &dacl, NULL, &sd) == ERROR_SUCCESS) {
        EXPLICIT_ACCESSW ea;
        memset(&ea, 0, sizeof(ea));
        ea.grfAccessMode = REVOKE_ACCESS;
        ea.Trustee.TrusteeForm = TRUSTEE_IS_SID;
        ea.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
        ea.Trustee.ptstrName = (LPWSTR)sid;
        if (SetEntriesInAclW(1, &ea, dacl, &newDacl) == ERROR_SUCCESS)
            SetSecurityInfo(k, SE_REGISTRY_KEY, DACL_SECURITY_INFORMATION, NULL, NULL, newDacl, NULL);
    }
    if (newDacl) LocalFree(newDacl);
    if (sd) LocalFree(sd);
    if (sid) LocalFree(sid);
    RegCloseKey(k);
}

} // namespace

HRESULT XavcRegisterUser() {
    wchar_t bsh[64], dec[64], sub[64];
    GuidString(CLSID_XavcByteStreamHandler, bsh);
    GuidString(CLSID_XavcH264Decoder, dec);
    GuidString(XAVC_VideoFormat_XAVC, sub);
    // The Media Player runs in an AppContainer: let it read the handler list and our settings.
    // Granted before the keys below are created, so they inherit it.
    HRESULT hr = GrantAppContainerRead(kWmfKey);
    if (hr == S_OK) SetString(HKEY_CURRENT_USER, kOurKey, L"GrantedWmfRead", L"1");
    HRESULT hr2 = GrantAppContainerRead(kOurKey);
    if (SUCCEEDED(hr)) hr = hr2;
    LONG r = ERROR_SUCCESS;
    for (const wchar_t* sel : kSelectors) {
        wchar_t path[256], saved[64];
        _snwprintf(path, 256, L"%ls\\%ls", kBshKey, sel);
        path[255] = 0;
        if (r == ERROR_SUCCESS) r = SetString(HKEY_CURRENT_USER, path, bsh, L"XAVC Byte Stream Handler");
        _snwprintf(saved, 64, L"MediaSources %ls", sel);
        saved[63] = 0;
        if (r == ERROR_SUCCESS) r = SetPreferred(kSrcPrefKey, sel, bsh, saved);
    }
    if (r == ERROR_SUCCESS) r = SetPreferred(kMftPrefKey, sub, dec, L"Transforms XAVC");
    if (r != ERROR_SUCCESS) {
        XavcLog("per-user registration failed: %ld", r);
        return HRESULT_FROM_WIN32(r);
    }
    XavcLog("per-user registration done (ACL 0x%08lx)", (unsigned long)hr);
    return FAILED(hr) ? hr : S_OK;
}

HRESULT XavcUnregisterUser() {
    wchar_t bsh[64], dec[64], sub[64];
    GuidString(CLSID_XavcByteStreamHandler, bsh);
    GuidString(CLSID_XavcH264Decoder, dec);
    GuidString(XAVC_VideoFormat_XAVC, sub);
    for (const wchar_t* sel : kSelectors) {
        wchar_t path[256], saved[64];
        _snwprintf(path, 256, L"%ls\\%ls", kBshKey, sel);
        path[255] = 0;
        DeleteValue(HKEY_CURRENT_USER, path, bsh);
        DeleteKeyIfEmpty(HKEY_CURRENT_USER, kBshKey, sel);
        _snwprintf(saved, 64, L"MediaSources %ls", sel);
        saved[63] = 0;
        RestorePreferred(kSrcPrefKey, sel, bsh, saved);
    }
    RestorePreferred(kMftPrefKey, sub, dec, L"Transforms XAVC");
    wchar_t granted[8];
    if (GetString(HKEY_CURRENT_USER, kOurKey, L"GrantedWmfRead", granted, 8)) RevokeAppContainerRead(kWmfKey);
    RegDeleteTreeW(HKEY_CURRENT_USER, kOurKey);
    // Forget that Active Setup ran for this user, so a later (re)install enables the user again.
    RegDeleteTreeW(HKEY_CURRENT_USER, kActiveSetupKey);
    XavcLog("per-user registration removed");
    return S_OK;
}
