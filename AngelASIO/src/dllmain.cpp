#include "common.h"
#include "panel.h"
#include "log.h"
#include <string>
#include <new>

HINSTANCE g_hInst = nullptr;
LONG g_objCount = 0;
static LONG g_lockCount = 0;

IASIO* angelCreateDriver();

class Factory : public IClassFactory {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (IsEqualIID(iid, IID_IUnknown) || IsEqualIID(iid, IID_IClassFactory)) { *ppv = static_cast<IClassFactory*>(this); AddRef(); return S_OK; }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }   // singleton estatico
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* outer, REFIID iid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (outer) return CLASS_E_NOAGGREGATION;
        IASIO* d = angelCreateDriver();
        if (!d) return E_OUTOFMEMORY;
        HRESULT hr = d->QueryInterface(iid, ppv);
        d->Release();
        return hr;
    }
    HRESULT STDMETHODCALLTYPE LockServer(BOOL lock) override {
        if (lock) InterlockedIncrement(&g_lockCount); else InterlockedDecrement(&g_lockCount);
        return S_OK;
    }
};
static Factory g_factory;

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) { g_hInst = h; DisableThreadLibraryCalls(h); }
    return TRUE;
}

STDAPI DllGetClassObject(REFCLSID clsid, REFIID iid, LPVOID* ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    if (!IsEqualCLSID(clsid, CLSID_AngelAsio)) return CLASS_E_CLASSNOTAVAILABLE;
    return g_factory.QueryInterface(iid, ppv);
}

STDAPI DllCanUnloadNow() { return (g_objCount == 0 && g_lockCount == 0) ? S_OK : S_FALSE; }

static LONG setStr(const std::wstring& path, const wchar_t* name, const std::wstring& val) {
    HKEY k;
    LONG r = RegCreateKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr);
    if (r != ERROR_SUCCESS) return r;
    r = RegSetValueExW(k, name, 0, REG_SZ, (const BYTE*)val.c_str(), (DWORD)((val.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(k);
    return r;
}

#define SELFREG_FAIL ((HRESULT)0x80040201L)

STDAPI DllRegisterServer() {
    wchar_t path[MAX_PATH];
    if (!GetModuleFileNameW(g_hInst, path, MAX_PATH)) return SELFREG_FAIL;
    std::wstring base = std::wstring(L"SOFTWARE\\Classes\\CLSID\\") + ANGEL_CLSID_STR;
    LONG r = ERROR_SUCCESS;
    if (r == ERROR_SUCCESS) r = setStr(base, nullptr, ANGEL_DRIVER_NAME_W);
    if (r == ERROR_SUCCESS) r = setStr(base + L"\\InprocServer32", nullptr, path);
    if (r == ERROR_SUCCESS) r = setStr(base + L"\\InprocServer32", L"ThreadingModel", L"Both");
    std::wstring asio = std::wstring(L"SOFTWARE\\ASIO\\") + ANGEL_DRIVER_NAME_W;
    if (r == ERROR_SUCCESS) r = setStr(asio, L"CLSID", ANGEL_CLSID_STR);
    if (r == ERROR_SUCCESS) r = setStr(asio, L"Description", ANGEL_DRIVER_NAME_W);
    return r == ERROR_SUCCESS ? S_OK : HRESULT_FROM_WIN32(r);
}

STDAPI DllUnregisterServer() {
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, (std::wstring(L"SOFTWARE\\Classes\\CLSID\\") + ANGEL_CLSID_STR).c_str());
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, (std::wstring(L"SOFTWARE\\ASIO\\") + ANGEL_DRIVER_NAME_W).c_str());
    return S_OK;
}

// rundll32 AngelASIO.dll,ShowPanel  -> abre el panel de control sin necesidad del DAW
extern "C" void CALLBACK ShowPanel(HWND hwnd, HINSTANCE, LPSTR, int) {
    Config c; c.load();
    showPanel(hwnd, "Panel independiente (el audio lo abre el DAW)", c);
}
