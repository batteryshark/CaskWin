// SPDX-License-Identifier: LGPL-2.1-or-later
// WineD3D owns the 16-bit surfaces and conversion. This Windows display adapter
// supplies the emulated mode enumeration/depth that its Unix drivers provide.
// Host scan-out stays at 32 bits. Enabled only when WineD3D is loaded at startup.
#include "common.h"
#include "desktop.h"
#include "graphics-backend.h"
#include "detours.h"
#include <intrin.h>
#include <map>
#include <cstddef>

namespace wd {
static decltype(&EnumDisplaySettingsExW) real_enum_w=EnumDisplaySettingsExW;
static decltype(&EnumDisplaySettingsExA) real_enum_a=EnumDisplaySettingsExA;
static decltype(&EnumDisplaySettingsW) real_simple_w=EnumDisplaySettingsW;
static decltype(&EnumDisplaySettingsA) real_simple_a=EnumDisplaySettingsA;
static decltype(&ChangeDisplaySettingsExW) real_change_w=ChangeDisplaySettingsExW;
static decltype(&ChangeDisplaySettingsExA) real_change_a=ChangeDisplaySettingsExA;
static decltype(&ChangeDisplaySettingsW) real_change_simple_w=ChangeDisplaySettingsW;
static decltype(&ChangeDisplaySettingsA) real_change_simple_a=ChangeDisplaySettingsA;
static decltype(&GetDeviceCaps) real_caps=GetDeviceCaps;
static SRWLOCK depth_lock=SRWLOCK_INIT;
static std::map<str,DWORD> depths;
static std::map<str,DEVMODEW> initial_modes;
static thread_local unsigned passthrough;
struct NativeCall {NativeCall(){++passthrough;}~NativeCall(){--passthrough;}};
struct DisplayQuery {
    unsigned previous;
    bool native;
    explicit DisplayQuery(const void *caller):previous(passthrough),native(system_graphics_caller(caller)){
        // An application callback invoked during a native query still sees
        // guest modes. USER32 / driver callbacks keep the physical backend.
        if(previous&&!native)passthrough=0;
        native=native||passthrough;
    }
    ~DisplayQuery(){passthrough=previous;}
};
template<class Mode> static BOOL copy_mode(Mode *destination,const Mode &source) {
    if(!destination||destination->dmSize<offsetof(Mode,dmICMMethod)){
        SetLastError(ERROR_INVALID_PARAMETER);return FALSE;
    }
    WORD size=destination->dmSize;Mode result=source;
    result.dmSize=size;result.dmDriverExtra=0;
    memcpy(destination,&result,(std::min)((size_t)size,sizeof(result)));
    return TRUE;
}
static DEVMODEA ansi_mode(const DEVMODEW &source) {
    DEVMODEA result{};result.dmSize=sizeof(result);
    result.dmSpecVersion=source.dmSpecVersion;result.dmDriverVersion=source.dmDriverVersion;
    WideCharToMultiByte(CP_ACP,0,source.dmDeviceName,CCHDEVICENAME,(LPSTR)result.dmDeviceName,CCHDEVICENAME,nullptr,nullptr);
    WideCharToMultiByte(CP_ACP,0,source.dmFormName,CCHFORMNAME,(LPSTR)result.dmFormName,CCHFORMNAME,nullptr,nullptr);
    memcpy(&result.dmFields,&source.dmFields,offsetof(DEVMODEA,dmFormName)-offsetof(DEVMODEA,dmFields));
    memcpy(&result.dmLogPixels,&source.dmLogPixels,sizeof(result)-offsetof(DEVMODEA,dmLogPixels));
    return result;
}
static str device_key(LPCWSTR device) {
    str key=device?device:L"";
    if(key.empty()){
        MONITORINFOEXW monitor{};monitor.cbSize=sizeof(monitor);
        if(GetMonitorInfoW(MonitorFromPoint({0,0},MONITOR_DEFAULTTOPRIMARY),&monitor))key=monitor.szDevice;
    }
    for(auto &c:key)c=(wchar_t)towupper(c);return key;
}
static str ansi_name(LPCSTR name) {
    if(!name)return {};
    int n=MultiByteToWideChar(CP_ACP,0,name,-1,nullptr,0);
    str result(n,0);if(n)MultiByteToWideChar(CP_ACP,0,name,-1,result.data(),n);
    if(!result.empty())result.pop_back();return result;
}
static DWORD virtual_depth(LPCWSTR name) {
    str key=device_key(name);AcquireSRWLockShared(&depth_lock);
    auto it=depths.find(key);DWORD depth=it==depths.end()?0:it->second;
    ReleaseSRWLockShared(&depth_lock);return depth;
}
struct ModeIndex {DWORD index,depth;};
static bool virtual_mode(DWORD index,DEVMODEW *mode){
    desktop_mode(mode,index==ENUM_REGISTRY_SETTINGS);
    if(index==ENUM_CURRENT_SETTINGS||index==ENUM_REGISTRY_SETTINGS)return true;
    static const SIZE sizes[]={{320,240},{512,384},{640,400},{640,480},{800,600},{1024,768},{1280,720},{1280,960},{1280,1024},{1600,900},{1600,1200},{1920,1080},{2560,1440},{3840,2160}};
    const DWORD count=(DWORD)(sizeof(sizes)/sizeof(sizes[0]));
    if(index/2>count)return false;
    if(index/2<count){mode->dmPelsWidth=sizes[index/2].cx;mode->dmPelsHeight=sizes[index/2].cy;}
    else {DEVMODEW initial{};desktop_mode(&initial,true);mode->dmPelsWidth=initial.dmPelsWidth;mode->dmPelsHeight=initial.dmPelsHeight;}
    mode->dmBitsPerPel=(index%2)?16:32;return true;
}
struct ModeCache {str device;DWORD flags=0;std::vector<ModeIndex> modes;};
static thread_local ModeCache cache;
static bool mode_index(LPCWSTR device,DWORD index,DWORD flags,ModeIndex &result) {
    str key=device_key(device);
    if(index==0||cache.modes.empty()||key!=cache.device||flags!=cache.flags){
        cache.device=key;cache.flags=flags;cache.modes.clear();
        DEVMODEW mode{};mode.dmSize=sizeof(mode);
        NativeCall native;
        for(DWORD i=0;i<16384&&real_enum_w(device,i,&mode,flags);++i){
            cache.modes.push_back({i,mode.dmBitsPerPel});
            if(mode.dmBitsPerPel==32)cache.modes.push_back({i,16});
        }
    }
    if(index>=cache.modes.size())return false;
    result=cache.modes[index];return true;
}
static BOOL WINAPI enum_w(LPCWSTR device,DWORD index,DEVMODEW *mode,DWORD flags) {
    DisplayQuery query(_ReturnAddress());
    if(query.native){NativeCall native;return real_enum_w(device,index,mode,flags);}
    if(virtual_desktop()){
        DEVMODEW value{};return virtual_mode(index,&value)&&copy_mode(mode,value);
    }
    if(index==ENUM_CURRENT_SETTINGS||index==ENUM_REGISTRY_SETTINGS){
        NativeCall native;BOOL ok=real_enum_w(device,index,mode,flags);
        auto initial=initial_modes.find(device_key(device));
        if(ok&&index==ENUM_REGISTRY_SETTINGS&&initial!=initial_modes.end())ok=copy_mode(mode,initial->second);
        if(ok&&index==ENUM_CURRENT_SETTINGS){DWORD depth=virtual_depth(device);if(depth)mode->dmBitsPerPel=depth;}
        return ok;
    }
    ModeIndex mapped{};if(!mode_index(device,index,flags,mapped))return FALSE;
    NativeCall native;BOOL ok=real_enum_w(device,mapped.index,mode,flags);if(ok)mode->dmBitsPerPel=mapped.depth;return ok;
}
static BOOL WINAPI enum_a(LPCSTR device,DWORD index,DEVMODEA *mode,DWORD flags) {
    DisplayQuery query(_ReturnAddress());
    if(query.native){NativeCall native;return real_enum_a(device,index,mode,flags);}
    if(virtual_desktop()){
        DEVMODEW w{};if(!virtual_mode(index,&w))return FALSE;
        return copy_mode(mode,ansi_mode(w));
    }
    str name=ansi_name(device);LPCWSTR w=device?name.c_str():nullptr;
    if(index==ENUM_CURRENT_SETTINGS||index==ENUM_REGISTRY_SETTINGS){
        NativeCall native;BOOL ok=real_enum_a(device,index,mode,flags);
        auto initial=initial_modes.find(device_key(w));
        if(ok&&index==ENUM_REGISTRY_SETTINGS&&initial!=initial_modes.end()){
            ok=copy_mode(mode,ansi_mode(initial->second));
        }
        if(ok&&index==ENUM_CURRENT_SETTINGS){DWORD depth=virtual_depth(w);if(depth)mode->dmBitsPerPel=depth;}
        return ok;
    }
    ModeIndex mapped{};if(!mode_index(w,index,flags,mapped))return FALSE;
    NativeCall native;BOOL ok=real_enum_a(device,mapped.index,mode,flags);if(ok)mode->dmBitsPerPel=mapped.depth;return ok;
}
static BOOL WINAPI simple_w(LPCWSTR device,DWORD index,DEVMODEW *mode){
    DisplayQuery query(_ReturnAddress());
    if(query.native||!mode){NativeCall native;return real_simple_w(device,index,mode);}
    // USER32's simple APIs initialize the legacy DEVMODE size themselves.
    // Preserve that contract before passing through the extended-mode adapter.
    mode->dmSize=offsetof(DEVMODEW,dmICMMethod);mode->dmDriverExtra=0;
    return enum_w(device,index,mode,0);
}
static BOOL WINAPI simple_a(LPCSTR device,DWORD index,DEVMODEA *mode){
    DisplayQuery query(_ReturnAddress());
    if(query.native||!mode){NativeCall native;return real_simple_a(device,index,mode);}
    mode->dmSize=offsetof(DEVMODEA,dmICMMethod);mode->dmDriverExtra=0;
    return enum_a(device,index,mode,0);
}
static LONG WINAPI change_w(LPCWSTR device,DEVMODEW *mode,HWND window,DWORD flags,LPVOID param) {
    if(passthrough)return real_change_w(device,mode,window,flags,param);
    // Persistent monitor layout changes are not prefix-local on Windows.
    // Do not claim success for settings we cannot isolate yet.
    if(flags&~(CDS_TEST|CDS_FULLSCREEN|CDS_RESET))return DISP_CHANGE_BADFLAGS;
    if(window||param)return DISP_CHANGE_BADPARAM;
    if(virtual_desktop())return desktop_change(mode,flags);
    // Wine's display registry view is the session's initial desktop. A reset
    // must not switch to an unrelated host mode saved before the game began.
    if(!mode){
        LONG result=DISP_CHANGE_SUCCESSFUL;str key=device_key(device);
        for(auto &entry:initial_modes){
            if(device&&entry.first!=key)continue;
            LONG changed=change_w(entry.first.c_str(),&entry.second,nullptr,flags,nullptr);
            if(changed!=DISP_CHANGE_SUCCESSFUL)result=changed;
        }
        return result;
    }
    DEVMODEW adjusted{},current{};current.dmSize=sizeof(current);
    DWORD depth=0;
    if(mode){
        if(mode->dmSize<offsetof(DEVMODEW,dmICMMethod)||mode->dmDriverExtra)return DISP_CHANGE_BADPARAM;
        memcpy(&adjusted,mode,(std::min)((size_t)mode->dmSize,sizeof(adjusted)));adjusted.dmSize=sizeof(adjusted);
        if((adjusted.dmFields&DM_BITSPERPEL)&&adjusted.dmBitsPerPel==16){
            depth=16;adjusted.dmBitsPerPel=32;
        }
    }
    LONG result;
    {
        NativeCall native;
        // Avoid a physical modeset when only the emulated bit depth changed.
        bool same=real_enum_w(device,ENUM_CURRENT_SETTINGS,&current,0)!=FALSE;
#define SAME_FIELD(flag,field) if((adjusted.dmFields&(flag))&&adjusted.field!=current.field)same=false
        SAME_FIELD(DM_BITSPERPEL,dmBitsPerPel);SAME_FIELD(DM_PELSWIDTH,dmPelsWidth);SAME_FIELD(DM_PELSHEIGHT,dmPelsHeight);
        SAME_FIELD(DM_DISPLAYFREQUENCY,dmDisplayFrequency);SAME_FIELD(DM_DISPLAYFLAGS,dmDisplayFlags);
        SAME_FIELD(DM_DISPLAYORIENTATION,dmDisplayOrientation);SAME_FIELD(DM_DISPLAYFIXEDOUTPUT,dmDisplayFixedOutput);
        SAME_FIELD(DM_POSITION,dmPosition.x);SAME_FIELD(DM_POSITION,dmPosition.y);
#undef SAME_FIELD
        result=same?DISP_CHANGE_SUCCESSFUL:real_change_w(device,&adjusted,window,flags,param);
    }
    if(result==DISP_CHANGE_SUCCESSFUL&&!(flags&CDS_TEST)){
        str key=device_key(device);AcquireSRWLockExclusive(&depth_lock);
        if(!device&&!mode)depths.clear();
        else if(depth)depths[key]=depth;
        else if(!mode||(mode->dmFields&DM_BITSPERPEL))depths.erase(key);
        ReleaseSRWLockExclusive(&depth_lock);
        cache.modes.clear();
    }
    return result;
}
static LONG WINAPI change_a(LPCSTR device,DEVMODEA *mode,HWND window,DWORD flags,LPVOID param) {
    if(passthrough)return real_change_a(device,mode,window,flags,param);
    DEVMODEW w{};str name=ansi_name(device);
    if(mode){
        if(mode->dmSize<offsetof(DEVMODEA,dmICMMethod)||mode->dmDriverExtra)return DISP_CHANGE_BADPARAM;
        // The numeric fields before and after dmFormName have identical layout.
        w.dmSize=sizeof(w);w.dmSpecVersion=mode->dmSpecVersion;w.dmDriverVersion=mode->dmDriverVersion;
        memcpy(&w.dmFields,&mode->dmFields,offsetof(DEVMODEA,dmFormName)-offsetof(DEVMODEA,dmFields));
        size_t count=(std::min)((size_t)mode->dmSize,sizeof(*mode))-offsetof(DEVMODEA,dmLogPixels);
        memcpy(&w.dmLogPixels,&mode->dmLogPixels,count);
        MultiByteToWideChar(CP_ACP,0,(LPCSTR)mode->dmDeviceName,CCHDEVICENAME,w.dmDeviceName,CCHDEVICENAME);
        MultiByteToWideChar(CP_ACP,0,(LPCSTR)mode->dmFormName,CCHFORMNAME,w.dmFormName,CCHFORMNAME);
    }
    return change_w(device?name.c_str():nullptr,mode?&w:nullptr,window,flags,param);
}
static LONG WINAPI change_simple_w(DEVMODEW *mode,DWORD flags){return change_w(nullptr,mode,nullptr,flags,nullptr);}
static LONG WINAPI change_simple_a(DEVMODEA *mode,DWORD flags){return change_a(nullptr,mode,nullptr,flags,nullptr);}
static int WINAPI caps(HDC dc,int index) {
    DisplayQuery query(_ReturnAddress());
    if(query.native){NativeCall native;return real_caps(dc,index);}
    if(virtual_desktop()&&real_caps(dc,TECHNOLOGY)==DT_RASDISPLAY){
        DEVMODEW mode{};desktop_mode(&mode);
        switch(index){case BITSPIXEL:return mode.dmBitsPerPel;case HORZRES:case DESKTOPHORZRES:return mode.dmPelsWidth;case VERTRES:case DESKTOPVERTRES:return mode.dmPelsHeight;default:break;}
    }
    if(index==BITSPIXEL&&real_caps(dc,TECHNOLOGY)==DT_RASDISPLAY){
        MONITORINFOEXW monitor{};monitor.cbSize=sizeof(monitor);
        HWND window=WindowFromDC(dc);LPCWSTR name=nullptr;
        if(GetMonitorInfoW(MonitorFromWindow(window,MONITOR_DEFAULTTOPRIMARY),&monitor))name=monitor.szDevice;
        DWORD depth=virtual_depth(name);if(depth)return (int)depth;
    }
    return real_caps(dc,index);
}
template<class T,class U> static void hook(T &real,U fn){check(DetourAttach((PVOID*)&real,(PVOID)fn),"Attach Wine display adapter");}
void attach_display() {
    if(!GetModuleHandleW(L"wined3d.dll")&&!virtual_desktop())return;
    DISPLAY_DEVICEW device{};device.cb=sizeof(device);
    for(DWORD index=0;EnumDisplayDevicesW(nullptr,index,&device,0);++index){
        if(!(device.StateFlags&DISPLAY_DEVICE_ATTACHED_TO_DESKTOP))continue;
        DEVMODEW mode{};mode.dmSize=sizeof(mode);
        if(real_enum_w(device.DeviceName,ENUM_CURRENT_SETTINGS,&mode,0))initial_modes[device_key(device.DeviceName)]=mode;
    }
    hook(real_enum_w,enum_w);hook(real_enum_a,enum_a);hook(real_simple_w,simple_w);hook(real_simple_a,simple_a);
    hook(real_change_w,change_w);hook(real_change_a,change_a);hook(real_change_simple_w,change_simple_w);hook(real_change_simple_a,change_simple_a);
    hook(real_caps,caps);
}
}
