// SPDX-License-Identifier: LGPL-2.1-or-later
// Native host endpoint adapter for portable Wine audio. Only these audio
// classes use direct factories; other COM registration remains prefix-owned.
#include "common.h"
#include "audio-backend.h"
#include "module-runtime.h"
#include "detours.h"
#include <objbase.h>
#include <mmsystem.h>
#include <dsound.h>
#include <mmdeviceapi.h>

namespace wd {
static decltype(&CoCreateInstance) real_create=CoCreateInstance;
static decltype(&CoCreateInstanceEx) real_create_ex=CoCreateInstanceEx;
using GetClassObject=HRESULT(WINAPI*)(REFCLSID,REFIID,void**);
static INIT_ONCE endpoint_once=INIT_ONCE_STATIC_INIT;
static HMODULE endpoint_module;
// Native XAudio 2.8/2.9 asks MMDevApi to create this worker when opening its
// endpoint. Like MMDeviceEnumerator it is a Windows audio backend class, not an
// application COM registration. Its factory comes from the same system DLL.
static const CLSID audio_activation_worker={0xe2f7a62a,0x862b,0x40ae,{0xbb,0xc2,0x5c,0x0c,0xa9,0xa5,0xb7,0xe1}};
// Version 2.7's coclass from Wine's include/xaudio2.idl. Later XAudio versions
// use XAudio2Create and must not acquire this legacy interface accidentally.
static const CLSID xaudio27_class={0x5a508685,0xa254,0x4fba,{0x9b,0x82,0x9a,0x24,0xb0,0x03,0x06,0xaf}};
static const CLSID xaudio27_meter={0xcac1105f,0x619b,0x4d04,{0x83,0x1a,0x44,0xe1,0xcb,0xf1,0x2d,0x57}};
static const CLSID xaudio27_reverb={0x6a93130e,0x1d53,0x41d1,{0xa9,0xcf,0xe7,0x58,0x80,0x0b,0xb1,0x79}};
// Wine's xactengine3_7/xact_classes.idl; earlier XACT versions use distinct
// classes and interfaces and must not silently receive this version.
static const CLSID xact37_class={0xbcc782bc,0x6492,0x4c22,{0x8c,0x35,0xf5,0xd7,0x2f,0xe7,0x3c,0x6e}};

static BOOL CALLBACK load_endpoint(PINIT_ONCE,void*,void**) {
    // Load the Windows backend by its system path, never an app-local substitute.
    std::vector<wchar_t> path(32768);UINT size=GetSystemDirectoryW(path.data(),(UINT)path.size());
    if(!size||size>=path.size()-14)return FALSE;
    wcscat_s(path.data(),path.size(),L"\\MMDevApi.dll");
    endpoint_module=LoadLibraryExW(path.data(),nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
    // Keep the module loaded for the lifetime of every COM object it creates.
    return endpoint_module!=nullptr;
}
static bool endpoint_class(REFCLSID clsid){return IsEqualCLSID(clsid,__uuidof(MMDeviceEnumerator))||IsEqualCLSID(clsid,audio_activation_worker);}
static bool playback_class(REFCLSID clsid){return IsEqualCLSID(clsid,CLSID_DirectSound)||IsEqualCLSID(clsid,CLSID_DirectSound8);}
static bool xaudio_class(REFCLSID clsid){return IsEqualCLSID(clsid,xaudio27_class)||IsEqualCLSID(clsid,xaudio27_meter)||IsEqualCLSID(clsid,xaudio27_reverb);}
static const wchar_t *versioned_audio_module(REFCLSID clsid){
    if(xaudio_class(clsid))return L"xaudio2_7.dll";
    if(IsEqualCLSID(clsid,xact37_class))return L"xactengine3_7.dll";
    return nullptr;
}
static bool private_class_registered(REFCLSID clsid){
    wchar_t guid[40];if(!StringFromGUID2(clsid,guid,40))return true;
    str path=L"CLSID\\"+str(guid);HKEY key=nullptr;
    // HKCR stays prefix-owned and uses the calling process's registry view.
    // Even a broken explicit registration takes precedence over native fallback.
    LSTATUS status=RegOpenKeyExW(HKEY_CLASSES_ROOT,path.c_str(),0,KEY_READ,&key);
    if(status==ERROR_SUCCESS)RegCloseKey(key);
    return status!=ERROR_FILE_NOT_FOUND&&status!=ERROR_PATH_NOT_FOUND;
}
static bool handled(REFCLSID clsid,DWORD context,COSERVERINFO *server=nullptr) {
    return (context&CLSCTX_INPROC_SERVER)&&(!server||!server->pwszName||!*server->pwszName)
        &&(endpoint_class(clsid)||playback_class(clsid)||versioned_audio_module(clsid));
}
static HRESULT audio_factory(REFCLSID clsid,IClassFactory **factory) {
    HMODULE module=nullptr;
    if(endpoint_class(clsid)){
        if(!InitOnceExecuteOnce(&endpoint_once,load_endpoint,nullptr,nullptr))return HRESULT_FROM_WIN32(GetLastError());
        module=endpoint_module;
    }else{
        // The launcher preloads the chosen Wine DLL before binding imports.
        // A native choice retains the normal application/system DLL search.
        const wchar_t *name=versioned_audio_module(clsid);if(!name)name=L"dsound.dll";
        HMODULE selected=selected_builtin_module(name);
        if(selected)GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,(LPCWSTR)selected,&module);
        else if(!GetModuleHandleExW(0,name,&module))module=LoadLibraryW(name);
        if(!module)return HRESULT_FROM_WIN32(GetLastError());
    }
    auto get_class=(GetClassObject)GetProcAddress(module,"DllGetClassObject");
    HRESULT result=get_class?get_class(clsid,IID_IClassFactory,(void**)factory):CLASS_E_CLASSNOTAVAILABLE;
    if(!endpoint_class(clsid)){
        // Factories/objects hold code pointers. Pin this selected provider
        // before balancing our temporary load reference.
        HMODULE pinned=nullptr;
        if(SUCCEEDED(result)&&!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,(LPCWSTR)get_class,&pinned)){
            (*factory)->Release();*factory=nullptr;result=HRESULT_FROM_WIN32(GetLastError());
        }
        FreeLibrary(module);
    }
    return result;
}
static HRESULT create_audio(REFCLSID clsid,IUnknown *outer,REFIID iid,void **result) {
    if(!result)return E_POINTER;
    *result=nullptr;
    ULONG_PTR token=0;HRESULT hr=CoGetContextToken(&token);
    if(FAILED(hr))return hr;
    IClassFactory *factory=nullptr;hr=audio_factory(clsid,&factory);
    if(SUCCEEDED(hr)){hr=factory->CreateInstance(outer,iid,result);factory->Release();}
    return hr;
}
static HRESULT WINAPI create(REFCLSID clsid,IUnknown *outer,DWORD context,REFIID iid,void **result) {
    if(!handled(clsid,context))return real_create(clsid,outer,context,iid,result);
    const wchar_t *versioned=versioned_audio_module(clsid);
    if(versioned&&!selected_builtin_module(versioned)){
        HRESULT hr=real_create(clsid,outer,context,iid,result);
        if(hr!=REGDB_E_CLASSNOTREG||private_class_registered(clsid))return hr;
    }
    return create_audio(clsid,outer,iid,result);
}
static HRESULT WINAPI create_ex(REFCLSID clsid,IUnknown *outer,DWORD context,COSERVERINFO *server,DWORD count,MULTI_QI *results) {
    if(!handled(clsid,context,server))return real_create_ex(clsid,outer,context,server,count,results);
    const wchar_t *versioned=versioned_audio_module(clsid);
    if(versioned&&!selected_builtin_module(versioned)){
        HRESULT hr=real_create_ex(clsid,outer,context,server,count,results);
        if(hr!=REGDB_E_CLASSNOTREG||private_class_registered(clsid))return hr;
    }
    if(!count||!results)return E_INVALIDARG;
    for(DWORD i=0;i<count;i++){results[i].pItf=nullptr;results[i].hr=E_NOINTERFACE;}
    for(DWORD i=0;i<count;i++)if(!results[i].pIID)return E_INVALIDARG;
    // CoCreateInstanceEx aggregation only requests the controlling IUnknown.
    if(outer&&(count!=1||!IsEqualIID(*results[0].pIID,IID_IUnknown)))return CLASS_E_NOAGGREGATION;
    IUnknown *object=nullptr;HRESULT hr=create_audio(clsid,outer,IID_IUnknown,(void**)&object);
    if(FAILED(hr)){for(DWORD i=0;i<count;i++)results[i].hr=hr;return hr;}
    DWORD supported=0;
    for(DWORD i=0;i<count;i++){
        results[i].hr=object->QueryInterface(*results[i].pIID,(void**)&results[i].pItf);
        if(SUCCEEDED(results[i].hr))++supported;
    }
    object->Release();
    return supported==count?S_OK:supported?CO_S_NOTALLINTERFACES:E_NOINTERFACE;
}
void attach_audio_backend() {
    check(DetourAttach((PVOID*)&real_create,(PVOID)create),"Attach audio COM activation");
    check(DetourAttach((PVOID*)&real_create_ex,(PVOID)create_ex),"Attach extended audio COM activation");
}
}
