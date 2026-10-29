// SPDX-License-Identifier: LGPL-2.1-or-later
#include "graphics-backend.h"
#include "module-runtime.h"

namespace wd {
namespace {
using Factory=HRESULT(WINAPI*)(REFIID,void**);
using Factory2=HRESULT(WINAPI*)(UINT,REFIID,void**);
INIT_ONCE system_once=INIT_ONCE_STATIC_INIT;
HMODULE system_dxgi;
Factory system_factory,system_factory1;
Factory2 system_factory2;
INIT_ONCE d3d11_once=INIT_ONCE_STATIC_INIT;
HMODULE system_d3d11;
decltype(&D3D11CreateDevice) system_device;
decltype(&D3D11CreateDeviceAndSwapChain) system_device_swapchain;

bool system_caller(const void *address) {
    if(!address)return false;
    HMODULE owner=nullptr;
    if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)address,&owner))return false;
    std::vector<wchar_t> filename(32768),windows(32768);
    DWORD length=GetModuleFileNameW(owner,filename.data(),(DWORD)filename.size());
    UINT count=GetWindowsDirectoryW(windows.data(),(UINT)windows.size());
    if(!length||length>=filename.size()||!count||count>=windows.size())return false;
    str path(filename.data(),length),root(windows.data(),count);
    // DriverStore ICDs and Windows graphics DLLs are host backends. A DLL in an
    // application/prefix directory is never classified by its basename alone.
    return inside(path,root+L"\\System32")||inside(path,root+L"\\SysWOW64");
}
BOOL CALLBACK load_system_factory(PINIT_ONCE,void*,void**) {
    SystemBackendScope native_scope;
    system_dxgi=load_system_dxgi();
    if(!system_dxgi)return FALSE;
    system_factory=(Factory)GetProcAddress(system_dxgi,"CreateDXGIFactory");
    system_factory1=(Factory)GetProcAddress(system_dxgi,"CreateDXGIFactory1");
    system_factory2=(Factory2)GetProcAddress(system_dxgi,"CreateDXGIFactory2");
    // Keep the backend loaded for the entire lifetime of its returned objects.
    return TRUE;
}
BOOL CALLBACK load_system_d3d11_device(PINIT_ONCE,void*,void**) {
    SystemBackendScope native_scope;
    system_d3d11=load_system_d3d11();
    if(!system_d3d11)return FALSE;
    system_device=(decltype(system_device))GetProcAddress(system_d3d11,"D3D11CreateDevice");
    system_device_swapchain=(decltype(system_device_swapchain))GetProcAddress(system_d3d11,"D3D11CreateDeviceAndSwapChain");
    // Returned COM objects retain code in this backend for their lifetime.
    return TRUE;
}
}
bool system_graphics_caller(const void *caller) noexcept {
    DWORD saved_error=GetLastError();bool result=false;
    try{result=system_caller(caller);}catch(...){}
    SetLastError(saved_error);return result;
}
BOOL create_system_dxgi_factory(const void *caller,UINT entrypoint,UINT flags,
                               REFIID iid,void **factory,HRESULT *result) {
    DWORD saved_error=GetLastError();
    if(entrypoint>2||!result||!system_caller(caller)){SetLastError(saved_error);return FALSE;}
    SystemBackendScope native_scope;
    if(!InitOnceExecuteOnce(&system_once,load_system_factory,nullptr,nullptr)){
        DWORD error=GetLastError();if(factory)*factory=nullptr;
        *result=HRESULT_FROM_WIN32(error?error:ERROR_MOD_NOT_FOUND);return TRUE;
    }
    SetLastError(saved_error);
    if(entrypoint==0&&system_factory)*result=system_factory(iid,factory);
    else if(entrypoint==1&&system_factory1)*result=system_factory1(iid,factory);
    else if(entrypoint==2&&system_factory2)*result=system_factory2(flags,iid,factory);
    else {if(factory)*factory=nullptr;*result=HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);}
    return TRUE;
}
BOOL create_system_d3d11_device(const void *caller,UINT entrypoint,IDXGIAdapter *adapter,
                               D3D_DRIVER_TYPE driver_type,HMODULE software,UINT flags,
                               const D3D_FEATURE_LEVEL *levels,UINT level_count,UINT sdk_version,
                               const DXGI_SWAP_CHAIN_DESC *description,IDXGISwapChain **swapchain,
                               ID3D11Device **device,D3D_FEATURE_LEVEL *feature_level,
                               ID3D11DeviceContext **context,HRESULT *result) {
    DWORD saved_error=GetLastError();
    if(entrypoint>1||!result||!system_caller(caller)){SetLastError(saved_error);return FALSE;}
    SystemBackendScope native_scope;
    if(!InitOnceExecuteOnce(&d3d11_once,load_system_d3d11_device,nullptr,nullptr)){
        DWORD error=GetLastError();
        if(swapchain)*swapchain=nullptr;if(device)*device=nullptr;if(context)*context=nullptr;
        if(feature_level)*feature_level=(D3D_FEATURE_LEVEL)0;
        *result=HRESULT_FROM_WIN32(error?error:ERROR_MOD_NOT_FOUND);return TRUE;
    }
    SetLastError(saved_error);
    if(entrypoint==0&&system_device)
        *result=system_device(adapter,driver_type,software,flags,levels,level_count,sdk_version,device,feature_level,context);
    else if(entrypoint==1&&system_device_swapchain)
        *result=system_device_swapchain(adapter,driver_type,software,flags,levels,level_count,sdk_version,description,swapchain,device,feature_level,context);
    else {
        if(swapchain)*swapchain=nullptr;if(device)*device=nullptr;if(context)*context=nullptr;
        if(feature_level)*feature_level=(D3D_FEATURE_LEVEL)0;
        *result=HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
    }
    // A native backend failure is final; it must not recurse through Wine.
    return TRUE;
}
}
