// SPDX-License-Identifier: LGPL-2.1-or-later
// Window/coordinate adapter for the native virtual desktop container.
#include "desktop.h"
#include "graphics-backend.h"
#include "detours.h"
#include <intrin.h>
namespace wd {
static HWND desktop;
static thread_local unsigned native_call;
struct Native {Native(){++native_call;}~Native(){--native_call;}};
struct NativeImage {ULONG_PTR base;size_t size;};
static NativeImage native_images[40];
static unsigned native_image_count;
static bool native_address(const void *address){
    ULONG_PTR value=reinterpret_cast<ULONG_PTR>(address);
    for(unsigned i=0;i<native_image_count;++i)if(value>=native_images[i].base&&value-native_images[i].base<native_images[i].size)return true;
    return false;
}
// A synchronous USER32 call can reenter the application's WndProc while a
// physical-coordinate operation is in progress. Only native implementation
// callers inherit that bypass. Application/JIT/Wine callers regain the virtual
// desktop for this API invocation, then restore the exact outer nesting depth.
// Evaluate _ReturnAddress at EACH hook entry, never inside this constructor.
// Tail-jumped APIs retain USER32's return address and intentionally keep the
// native fallback; this is a call-site boundary, not WndProc interception.
struct DesktopCall {
    unsigned previous;
    explicit DesktopCall(const void *caller):previous(native_call){if(previous&&!native_address(caller))native_call=0;}
    ~DesktopCall(){native_call=previous;}
};
static void remember_native_image(HMODULE module){
    if(!module||native_image_count==sizeof(native_images)/sizeof(native_images[0]))return;
    ULONG_PTR base=reinterpret_cast<ULONG_PTR>(module);
    for(unsigned i=0;i<native_image_count;++i)if(native_images[i].base==base)return;
    // Retain cached images so a later app/JIT allocation cannot reuse a stale
    // native range. Never load an absent backend or acquire modules in hooks.
    HMODULE retained=nullptr;
    if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,reinterpret_cast<LPCWSTR>(module),&retained))return;
    auto dos=reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
    auto nt=reinterpret_cast<const IMAGE_NT_HEADERS*>(base+dos->e_lfanew);
    native_images[native_image_count++]={base,nt->OptionalHeader.SizeOfImage};
}
static void initialize_native_images(){
    DWORD saved=GetLastError();HMODULE own=nullptr;
    if(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,reinterpret_cast<LPCWSTR>(&attach_desktop),&own))remember_native_image(own);
    wchar_t windows[MAX_PATH];UINT length=GetWindowsDirectoryW(windows,MAX_PATH);
    if(length&&length<MAX_PATH){
        const wchar_t *names[]={L"user32.dll",L"win32u.dll",L"gdi32.dll",L"gdi32full.dll",L"dxgi.dll",L"d3d8.dll",L"d3d9.dll",L"d3d10.dll",L"d3d10_1.dll",L"d3d10core.dll",L"d3d11.dll",L"d3d12.dll",L"d3d12core.dll"};
        for(const wchar_t *folder:{L"\\System32\\",L"\\SysWOW64\\"})for(const wchar_t *name:names){
            str path=str(windows,length)+folder+name;HMODULE module=GetModuleHandleW(path.c_str());
            wchar_t actual[MAX_PATH];DWORD count=module?GetModuleFileNameW(module,actual,MAX_PATH):0;
            if(count&&count<MAX_PATH&&!_wcsicmp(path.c_str(),actual))remember_native_image(module);
        }
    }
    SetLastError(saved);
}
static const wchar_t root_property[]=L"Winedows.VirtualRoot";
static const wchar_t style_property[]=L"Winedows.WindowStyle";
static const wchar_t exstyle_property[]=L"Winedows.WindowExStyle";
static const wchar_t owner_property[]=L"Winedows.WindowOwner";
// Hidden application roots stay physically top-level until shown. Their
// logical desktop still matters to singleton and other FindWindow searches.
static const wchar_t desktop_property[]=L"Winedows.LogicalDesktop";
#define REAL(name) static decltype(&name) real_##name=name
REAL(CreateWindowExW);REAL(CreateWindowExA);REAL(GetDesktopWindow);REAL(GetParent);REAL(GetAncestor);REAL(GetWindow);
REAL(FindWindowW);REAL(FindWindowA);REAL(FindWindowExW);REAL(FindWindowExA);
REAL(GetWindowLongPtrW);REAL(GetWindowLongPtrA);REAL(SetWindowLongPtrW);REAL(SetWindowLongPtrA);
#ifdef _WIN64
REAL(GetWindowLongW);REAL(GetWindowLongA);REAL(SetWindowLongW);REAL(SetWindowLongA);
#endif
REAL(GetWindowRect);REAL(ClientToScreen);REAL(ScreenToClient);REAL(MapWindowPoints);
REAL(GetCursorPos);REAL(SetCursorPos);REAL(ClipCursor);REAL(GetClipCursor);
REAL(GetSystemMetrics);REAL(GetMonitorInfoW);REAL(GetMonitorInfoA);REAL(EnumDisplayMonitors);
REAL(MonitorFromPoint);REAL(MonitorFromRect);REAL(MonitorFromWindow);
REAL(SystemParametersInfoW);REAL(SystemParametersInfoA);
REAL(GetDC);REAL(ReleaseDC);REAL(GetDCEx);
REAL(GetForegroundWindow);REAL(GetActiveWindow);REAL(SetForegroundWindow);
REAL(ShowWindow);REAL(ShowWindowAsync);
REAL(SetWindowPos);REAL(SetWindowPlacement);
REAL(DialogBoxParamW);REAL(DialogBoxParamA);REAL(DialogBoxIndirectParamW);REAL(DialogBoxIndirectParamA);
REAL(CreateDialogParamW);REAL(CreateDialogParamA);REAL(CreateDialogIndirectParamW);REAL(CreateDialogIndirectParamA);
REAL(MessageBoxW);REAL(MessageBoxA);REAL(MessageBoxExW);REAL(MessageBoxExA);REAL(MessageBoxIndirectW);REAL(MessageBoxIndirectA);
#undef REAL
static decltype(&GetSystemMetricsForDpi) real_GetSystemMetricsForDpi;
static bool geometry_trace;
static LONG geometry_trace_count;
// Opened before the hook transaction commits, then owned for process lifetime.
// GUI applications often have no inherited stderr handle.
static HANDLE geometry_trace_file=INVALID_HANDLE_VALUE;
static HANDLE (WINAPI *trace_thread_context)();
static HANDLE (WINAPI *trace_window_context)(HWND);
static int (WINAPI *trace_awareness)(HANDLE);
static UINT (WINAPI *trace_window_dpi)(HWND);
static void trace_geometry(const char *operation,HWND window,int x,int y,int width,int height,DWORD flags,DWORD style=0){
    if(!geometry_trace)return;
    DWORD saved=GetLastError();
    if(InterlockedCompareExchange(&geometry_trace_count,0,0)>=128||InterlockedIncrement(&geometry_trace_count)>128){SetLastError(saved);return;}
    {
        Native native;RECT outer{},client{},container_client{};DWORD pid=0;
        if(window)GetWindowThreadProcessId(window,&pid);
        // Only the current application's windows and its own container may
        // appear in this trace. Never read titles, input, pixels or other apps.
        if(!window||pid==GetCurrentProcessId()||window==desktop){
            BOOL rectangle=window&&real_GetWindowRect(window,&outer),area=window&&GetClientRect(window,&client);
            GetClientRect(desktop,&container_client);
            int thread_mode=trace_thread_context&&trace_awareness?trace_awareness(trace_thread_context()):-1;
            int window_mode=window&&trace_window_context&&trace_awareness?trace_awareness(trace_window_context(window)):-1;
            UINT dpi=window&&trace_window_dpi?trace_window_dpi(window):0;
            char line[1024];int length=snprintf(line,sizeof(line),"WINEDOWS_DESKTOP pid=%lu tid=%lu op=%s hwnd=%p request=%d,%d,%d,%d flags=%08lx style=%08lx outer=%ld,%ld,%ld,%ld client=%ld,%ld rect_ok=%d client_ok=%d dpi=%u thread_awareness=%d window_awareness=%d container=%ld,%ld\r\n",GetCurrentProcessId(),GetCurrentThreadId(),operation,window,x,y,width,height,flags,style,outer.left,outer.top,outer.right,outer.bottom,client.right,client.bottom,rectangle,area,dpi,thread_mode,window_mode,container_client.right,container_client.bottom);
            if(length>0&&size_t(length)<sizeof(line)){
                DWORD written;HANDLE error=GetStdHandle(STD_ERROR_HANDLE);
                if(geometry_trace_file!=INVALID_HANDLE_VALUE)WriteFile(geometry_trace_file,line,DWORD(length),&written,nullptr);
                if(error&&error!=INVALID_HANDLE_VALUE)WriteFile(error,line,DWORD(length),&written,nullptr);
            }
        }
    }
    SetLastError(saved);
}
HWND virtual_desktop(){return desktop;}
static unsigned property(const wchar_t *name){return (unsigned)(ULONG_PTR)GetPropW(desktop,name);}
static RECT desktop_rect(){return {0,0,(LONG)property(WD_DESKTOP_WIDTH),(LONG)property(WD_DESKTOP_HEIGHT)};}
static HMONITOR desktop_monitor(){Native native;return real_MonitorFromWindow(desktop,MONITOR_DEFAULTTOPRIMARY);}
bool desktop_mode(DEVMODEW *mode,bool initial) {
    if(!desktop)return false;
    mode->dmSize=sizeof(*mode);mode->dmDriverExtra=0;
    mode->dmFields=DM_POSITION|DM_DISPLAYORIENTATION|DM_BITSPERPEL|DM_PELSWIDTH|DM_PELSHEIGHT|DM_DISPLAYFREQUENCY|DM_DISPLAYFLAGS;
    mode->dmPosition={0,0};mode->dmDisplayOrientation=DMDO_DEFAULT;mode->dmDisplayFlags=0;mode->dmDisplayFrequency=60;
    mode->dmPelsWidth=property(initial?WD_DESKTOP_INITIAL_WIDTH:WD_DESKTOP_WIDTH);
    mode->dmPelsHeight=property(initial?WD_DESKTOP_INITIAL_HEIGHT:WD_DESKTOP_HEIGHT);
    mode->dmBitsPerPel=initial?32:property(WD_DESKTOP_DEPTH);
    return true;
}
LONG desktop_change(DEVMODEW *mode,DWORD flags) {
    if(flags&~(CDS_TEST|CDS_FULLSCREEN|CDS_RESET))return DISP_CHANGE_BADFLAGS;
    DEVMODEW current{};desktop_mode(&current,!mode);
    if(mode){
        if(mode->dmSize<offsetof(DEVMODEW,dmICMMethod)||mode->dmDriverExtra)return DISP_CHANGE_BADPARAM;
        if((mode->dmFields&DM_POSITION)&&(mode->dmPosition.x||mode->dmPosition.y))return DISP_CHANGE_BADMODE;
        if((mode->dmFields&DM_DISPLAYORIENTATION)&&mode->dmDisplayOrientation!=DMDO_DEFAULT)return DISP_CHANGE_BADMODE;
        if((mode->dmFields&DM_DISPLAYFREQUENCY)&&mode->dmDisplayFrequency>1&&mode->dmDisplayFrequency!=60)return DISP_CHANGE_BADMODE;
        if((mode->dmFields&DM_PELSWIDTH)&&mode->dmPelsWidth)current.dmPelsWidth=mode->dmPelsWidth;
        if((mode->dmFields&DM_PELSHEIGHT)&&mode->dmPelsHeight)current.dmPelsHeight=mode->dmPelsHeight;
        if((mode->dmFields&DM_BITSPERPEL)&&mode->dmBitsPerPel)current.dmBitsPerPel=mode->dmBitsPerPel;
    }
    if(current.dmPelsWidth<160||current.dmPelsHeight<100||current.dmPelsWidth>8192||current.dmPelsHeight>8192||(current.dmBitsPerPel!=16&&current.dmBitsPerPel!=32))return DISP_CHANGE_BADMODE;
    if(flags&CDS_TEST)return DISP_CHANGE_SUCCESSFUL;
    DWORD_PTR result=0;
    Native native;
    if(!SendMessageTimeoutW(desktop,WD_DESKTOP_MODE,current.dmBitsPerPel,MAKELPARAM(current.dmPelsWidth,current.dmPelsHeight),SMTO_ABORTIFHUNG,5000,&result)||!result)return DISP_CHANGE_FAILED;
    return DISP_CHANGE_SUCCESSFUL;
}
static bool root(HWND hwnd){return GetPropW(hwnd,root_property)!=nullptr;}
static bool native_menu_root(HWND hwnd){return GetPropW(hwnd,WD_DESKTOP_NATIVE_MENU_ROOT)==desktop;}
static bool contained(HWND hwnd){
    if(hwnd==desktop)return true;
    // IsChild/GetParent follow WS_CHILD/owner semantics. A menu-bearing root
    // deliberately keeps its original style, so follow physical parents.
    // Require our exact registration for this new kind of direct child;
    // genuine WS_CHILD roots and their ordinary descendants remain supported.
    for(unsigned i=0;hwnd&&i<64;++i){
        HWND physical=real_GetAncestor(hwnd,GA_PARENT);
        if(physical==desktop)return ((DWORD)real_GetWindowLongPtrW(hwnd,GWL_STYLE)&WS_CHILD)
            ||(root(hwnd)&&native_menu_root(hwnd)&&GetPropW(hwnd,desktop_property)==desktop);
        if(!physical||physical==hwnd)break;
        hwnd=physical;
    }
    return false;
}
static HWND WINAPI get_desktop(){DesktopCall desktop_call(_ReturnAddress());return native_call?real_GetDesktopWindow():desktop;}
static DWORD child_style(DWORD style){return (style&~WS_POPUP)|WS_CHILD|WS_CLIPSIBLINGS;}
static void remember_desktop(HWND window){if(window){DWORD saved=GetLastError();SetPropW(window,desktop_property,desktop);SetLastError(saved);}}
static void remember(HWND window,DWORD style,DWORD ex,HWND owner){if(window){remember_desktop(window);SetPropW(window,root_property,(HANDLE)1);SetPropW(window,style_property,(HANDLE)(ULONG_PTR)style);SetPropW(window,exstyle_property,(HANDLE)(ULONG_PTR)ex);if(owner)SetPropW(window,owner_property,owner);}}
static HWND WINAPI create_w(DWORD ex,LPCWSTR cls,LPCWSTR name,DWORD style,int x,int y,int w,int h,HWND parent,HMENU menu,HINSTANCE instance,LPVOID param){DesktopCall desktop_call(_ReturnAddress());
    if(native_call)return real_CreateWindowExW(ex,cls,name,style,x,y,w,h,parent,menu,instance,param);
    trace_geometry("create-w-request",nullptr,x,y,w,h,ex,style);
    if((style&WS_CHILD)||parent==HWND_MESSAGE||!(style&WS_VISIBLE)){Native native;HWND result=real_CreateWindowExW(ex,cls,name,style,x,y,w,h,parent,menu,instance,param);if(!(style&WS_CHILD)&&parent!=HWND_MESSAGE)remember_desktop(result);trace_geometry("create-w-native-result",result,x,y,w,h,ex,style);return result;}
    Native native;HWND owner=parent;
    if(x==CW_USEDEFAULT)x=0;if(y==CW_USEDEFAULT)y=0;
    if(w==CW_USEDEFAULT)w=(int)property(WD_DESKTOP_WIDTH);if(h==CW_USEDEFAULT)h=(int)property(WD_DESKTOP_HEIGHT);
    HWND result=real_CreateWindowExW(ex&~(WS_EX_APPWINDOW|WS_EX_TOPMOST),cls,name,child_style(style),x,y,w,h,desktop,nullptr,instance,param);
    remember(result,style,ex,owner);if(result&&menu)SetMenu(result,menu);trace_geometry("create-w-contained-result",result,x,y,w,h,ex,style);return result;
}
static HWND WINAPI create_a(DWORD ex,LPCSTR cls,LPCSTR name,DWORD style,int x,int y,int w,int h,HWND parent,HMENU menu,HINSTANCE instance,LPVOID param){DesktopCall desktop_call(_ReturnAddress());
    if(native_call)return real_CreateWindowExA(ex,cls,name,style,x,y,w,h,parent,menu,instance,param);
    trace_geometry("create-a-request",nullptr,x,y,w,h,ex,style);
    if((style&WS_CHILD)||parent==HWND_MESSAGE||!(style&WS_VISIBLE)){Native native;HWND result=real_CreateWindowExA(ex,cls,name,style,x,y,w,h,parent,menu,instance,param);if(!(style&WS_CHILD)&&parent!=HWND_MESSAGE)remember_desktop(result);trace_geometry("create-a-native-result",result,x,y,w,h,ex,style);return result;}
    Native native;HWND owner=parent;
    if(x==CW_USEDEFAULT)x=0;if(y==CW_USEDEFAULT)y=0;
    if(w==CW_USEDEFAULT)w=(int)property(WD_DESKTOP_WIDTH);if(h==CW_USEDEFAULT)h=(int)property(WD_DESKTOP_HEIGHT);
    HWND result=real_CreateWindowExA(ex&~(WS_EX_APPWINDOW|WS_EX_TOPMOST),cls,name,child_style(style),x,y,w,h,desktop,nullptr,instance,param);
    remember(result,style,ex,owner);if(result&&menu)SetMenu(result,menu);trace_geometry("create-a-contained-result",result,x,y,w,h,ex,style);return result;
}
static void adopt(HWND window,int show){
    if(native_call||show==SW_HIDE||root(window)||window==desktop)return;
    DWORD pid=0;GetWindowThreadProcessId(window,&pid);if(pid!=GetCurrentProcessId())return;
    DWORD style=(DWORD)real_GetWindowLongPtrW(window,GWL_STYLE);
    if(style&WS_CHILD)return;
    Native native;if(real_GetParent(window)==HWND_MESSAGE)return;
    HWND owner=real_GetWindow(window,GW_OWNER);
    RECT rect{};if(!real_GetWindowRect(window,&rect))return;
    trace_geometry("adopt-before",window,rect.left,rect.top,rect.right-rect.left,rect.bottom-rect.top,0,style);
    // Pure WS_CHILD conversion destroys the native menu slot and also skips
    // disabled-menu accelerator checks. Keep real top-level menu semantics
    // for existing menu-bearing windows; non-menu/dialog paths stay unchanged.
    bool native_menu=GetMenu(window)!=nullptr;
    HANDLE previous_desktop=GetPropW(window,desktop_property);
    LONG_PTR ex=real_GetWindowLongPtrW(window,GWL_EXSTYLE);
    if(native_menu){
        // SetParent can synchronously activate/reenter the host. Publish the
        // marker before it can enqueue child-emulation activation messages.
        if(!SetPropW(window,WD_DESKTOP_NATIVE_MENU_ROOT,desktop))return;
        remember(window,style,(DWORD)ex,owner);
    }else real_SetWindowLongPtrW(window,GWL_STYLE,child_style(style));
    real_SetWindowLongPtrW(window,GWL_EXSTYLE,ex&~(WS_EX_APPWINDOW|WS_EX_TOPMOST));
    SetLastError(0);HWND previous=SetParent(window,desktop);
    if(!previous&&GetLastError()){
        DWORD error=GetLastError();
        if(native_menu){
            RemovePropW(window,WD_DESKTOP_NATIVE_MENU_ROOT);RemovePropW(window,root_property);
            RemovePropW(window,style_property);RemovePropW(window,exstyle_property);RemovePropW(window,owner_property);
            if(previous_desktop)SetPropW(window,desktop_property,previous_desktop);else RemovePropW(window,desktop_property);
        }else real_SetWindowLongPtrW(window,GWL_STYLE,style);
        real_SetWindowLongPtrW(window,GWL_EXSTYLE,ex);SetLastError(error);return;
    }
    if(!native_menu)remember(window,style,(DWORD)ex,owner);
    real_SetWindowPos(window,nullptr,rect.left,rect.top,rect.right-rect.left,rect.bottom-rect.top,SWP_NOZORDER|SWP_NOACTIVATE|SWP_FRAMECHANGED);
    trace_geometry("adopt-after",window,rect.left,rect.top,rect.right-rect.left,rect.bottom-rect.top,0,style);
}
static BOOL WINAPI show_window(HWND window,int show){DesktopCall desktop_call(_ReturnAddress());adopt(window,show);Native native;return real_ShowWindow(window,show);}
static BOOL WINAPI show_window_async(HWND window,int show){DesktopCall desktop_call(_ReturnAddress());adopt(window,show);Native native;return real_ShowWindowAsync(window,show);}
static BOOL WINAPI position_window(HWND window,HWND after,int x,int y,int cx,int cy,UINT flags){DesktopCall desktop_call(_ReturnAddress());
    bool traced=!native_call;if(traced)trace_geometry("position-request",window,x,y,cx,cy,flags);
    if(!native_call&&(flags&SWP_SHOWWINDOW))adopt(window,SW_SHOW);
    Native native;BOOL result=real_SetWindowPos(window,after,x,y,cx,cy,flags);if(traced)trace_geometry("position-result",window,x,y,cx,cy,flags);return result;
}
static BOOL WINAPI place_window(HWND window,const WINDOWPLACEMENT *placement){DesktopCall desktop_call(_ReturnAddress());
    if(placement&&placement->length==sizeof(*placement))adopt(window,placement->showCmd);
    Native native;return real_SetWindowPlacement(window,placement);
}
static HWND WINAPI parent(HWND hwnd){DesktopCall desktop_call(_ReturnAddress());if(!native_call&&root(hwnd))return (HWND)GetPropW(hwnd,owner_property);return real_GetParent(hwnd);}
static HWND WINAPI ancestor(HWND hwnd,UINT flags){DesktopCall desktop_call(_ReturnAddress());
    if(native_call||!contained(hwnd))return real_GetAncestor(hwnd,flags);
    if(flags==GA_PARENT)return hwnd==desktop?nullptr:real_GetAncestor(hwnd,GA_PARENT);
    if(flags!=GA_ROOT&&flags!=GA_ROOTOWNER)return real_GetAncestor(hwnd,flags);
    for(unsigned i=0;hwnd!=desktop&&!root(hwnd)&&i<64;++i){HWND p=real_GetAncestor(hwnd,GA_PARENT);if(!p||p==desktop||p==hwnd)break;hwnd=p;}
    if(flags==GA_ROOTOWNER)for(unsigned i=0;i<64;i++){HWND owner=(HWND)GetPropW(hwnd,owner_property);if(!owner)break;hwnd=owner;}
    return hwnd;
}
static HWND WINAPI get_window(HWND hwnd,UINT cmd){DesktopCall desktop_call(_ReturnAddress());if(!native_call&&root(hwnd)&&cmd==GW_OWNER)return (HWND)GetPropW(hwnd,owner_property);return real_GetWindow(hwnd,cmd);}
// USER32 performs the matching, including atoms, ANSI conversion, empty versus
// NULL strings, and its cached title lookup (no application WM_GETTEXT calls).
// The logical desktop list consists of its direct children (including actual
// WS_CHILD desktop children), followed by hidden roots which have not joined
// the container yet. A NULL-parent search starting at NULL also checks native
// message-only windows. Native continuation from any non-NULL childAfter does
// not cross to that message-only list. Explicit child/IPC searches stay native.
template<class Find,class Text>static HWND find_logical(Find find,HWND requested,HWND after,Text cls,Text title){
    DWORD saved=GetLastError();Native native;
    HWND physical=real_GetDesktopWindow();
    if(requested&&requested!=desktop&&requested!=physical){SetLastError(saved);return find(requested,after,cls,title);}
    bool messages=!requested&&!after;
    unsigned segment=0;
    if(after){
        HWND actual_parent=real_GetAncestor(after,GA_PARENT);
        if(actual_parent==desktop)segment=0;
        else if(actual_parent==physical&&GetPropW(after,desktop_property)==desktop)segment=1;
        else {
            // Let USER32 produce its normal invalid-child/parent result.
            SetLastError(saved);return find(desktop,after,cls,title);
        }
    }
    for(;segment<3;++segment,after=nullptr){
        if(segment==2&&!messages)break;
        HWND parent=segment==0?desktop:(segment==1?physical:HWND_MESSAGE);
        for(;;){
            SetLastError(saved);HWND found=find(parent,after,cls,title);DWORD error=GetLastError();
            if(!found){saved=error;break;}
            bool matches=segment!=1||GetPropW(found,desktop_property)==desktop;
            if(matches){SetLastError(error);return found;}
            after=found;
        }
    }
    SetLastError(saved);return nullptr;
}
static HWND WINAPI find_window_w(LPCWSTR cls,LPCWSTR title){const void *caller=_ReturnAddress();DesktopCall call(caller);
    if(native_call||native_address(caller)||system_graphics_caller(caller))return real_FindWindowW(cls,title);
    return find_logical(real_FindWindowExW,nullptr,nullptr,cls,title);
}
static HWND WINAPI find_window_a(LPCSTR cls,LPCSTR title){const void *caller=_ReturnAddress();DesktopCall call(caller);
    if(native_call||native_address(caller)||system_graphics_caller(caller))return real_FindWindowA(cls,title);
    return find_logical(real_FindWindowExA,nullptr,nullptr,cls,title);
}
static HWND WINAPI find_window_ex_w(HWND parent,HWND after,LPCWSTR cls,LPCWSTR title){const void *caller=_ReturnAddress();DesktopCall call(caller);
    if(native_call||native_address(caller)||parent==HWND_MESSAGE||system_graphics_caller(caller))return real_FindWindowExW(parent,after,cls,title);
    return find_logical(real_FindWindowExW,parent,after,cls,title);
}
static HWND WINAPI find_window_ex_a(HWND parent,HWND after,LPCSTR cls,LPCSTR title){const void *caller=_ReturnAddress();DesktopCall call(caller);
    if(native_call||native_address(caller)||parent==HWND_MESSAGE||system_graphics_caller(caller))return real_FindWindowExA(parent,after,cls,title);
    return find_logical(real_FindWindowExA,parent,after,cls,title);
}
static HWND desktop_focus(){
    GUITHREADINFO gui{};gui.cbSize=sizeof(gui);
    if(GetGUIThreadInfo(0,&gui)&&gui.hwndFocus&&gui.hwndFocus!=desktop&&contained(gui.hwndFocus))return ancestor(gui.hwndFocus,GA_ROOT);
    return desktop;
}
static HWND WINAPI foreground(){DesktopCall desktop_call(_ReturnAddress());if(native_call)return real_GetForegroundWindow();HWND window=real_GetForegroundWindow();return window==desktop?desktop_focus():window;}
static HWND WINAPI active_window(){DesktopCall desktop_call(_ReturnAddress());if(native_call)return real_GetActiveWindow();HWND window=real_GetActiveWindow();if(!window||window==desktop){HWND focus=GetFocus();if(focus&&contained(focus))return ancestor(focus,GA_ROOT);}return window;}
static BOOL WINAPI set_foreground(HWND window){DesktopCall desktop_call(_ReturnAddress());
    if(native_call||!contained(window))return real_SetForegroundWindow(window);
    Native native;
    BOOL ok=real_SetForegroundWindow(desktop);if(ok&&window!=desktop)SetFocus(window);return ok;
}
static LONG_PTR logical_style(HWND hwnd){
    const DWORD dynamic=WS_VISIBLE|WS_DISABLED|WS_MINIMIZE|WS_MAXIMIZE;
    return ((DWORD)(ULONG_PTR)GetPropW(hwnd,style_property)&~dynamic)|((DWORD)real_GetWindowLongPtrW(hwnd,GWL_STYLE)&dynamic);
}
static LONG_PTR logical_exstyle(HWND hwnd){
    const DWORD removed=WS_EX_APPWINDOW|WS_EX_TOPMOST;
    return ((DWORD)(ULONG_PTR)GetPropW(hwnd,exstyle_property)&removed)|((DWORD)real_GetWindowLongPtrW(hwnd,GWL_EXSTYLE)&~removed);
}
static LONG_PTR WINAPI get_long_w(HWND hwnd,int index){DesktopCall desktop_call(_ReturnAddress());
    if(!native_call&&root(hwnd)){if(index==GWL_STYLE)return logical_style(hwnd);if(index==GWL_EXSTYLE)return logical_exstyle(hwnd);if(index==GWLP_HWNDPARENT)return (LONG_PTR)GetPropW(hwnd,owner_property);}
    return real_GetWindowLongPtrW(hwnd,index);
}
static LONG_PTR WINAPI get_long_a(HWND hwnd,int index){DesktopCall desktop_call(_ReturnAddress());
    if(!native_call&&root(hwnd)){if(index==GWL_STYLE)return logical_style(hwnd);if(index==GWL_EXSTYLE)return logical_exstyle(hwnd);if(index==GWLP_HWNDPARENT)return (LONG_PTR)GetPropW(hwnd,owner_property);}
    return real_GetWindowLongPtrA(hwnd,index);
}
static LONG_PTR set_owner(HWND hwnd,LONG_PTR value){
    if(value&&!IsWindow((HWND)value)){SetLastError(ERROR_INVALID_WINDOW_HANDLE);return 0;}
    LONG_PTR previous=(LONG_PTR)GetPropW(hwnd,owner_property);
    if(value){if(!SetPropW(hwnd,owner_property,(HANDLE)value))return 0;}else RemovePropW(hwnd,owner_property);
    SetLastError(0);return previous;
}
static LONG_PTR WINAPI set_long_w(HWND hwnd,int index,LONG_PTR value){DesktopCall desktop_call(_ReturnAddress());
    if(native_call||!root(hwnd))return real_SetWindowLongPtrW(hwnd,index,value);
    if(index==GWLP_HWNDPARENT)return set_owner(hwnd,value);
    if(index!=GWL_STYLE&&index!=GWL_EXSTYLE)return real_SetWindowLongPtrW(hwnd,index,value);
    LONG_PTR previous=index==GWL_STYLE?logical_style(hwnd):logical_exstyle(hwnd);Native native;SetLastError(0);
    LONG_PTR physical=index==GWL_STYLE?(native_menu_root(hwnd)?value:child_style((DWORD)value)):value&~(WS_EX_APPWINDOW|WS_EX_TOPMOST);
    LONG_PTR result=real_SetWindowLongPtrW(hwnd,index,physical);if(!result&&GetLastError())return 0;
    SetPropW(hwnd,index==GWL_STYLE?style_property:exstyle_property,(HANDLE)value);return previous;
}
static LONG_PTR WINAPI set_long_a(HWND hwnd,int index,LONG_PTR value){DesktopCall desktop_call(_ReturnAddress());
    if(!native_call&&root(hwnd)&&(index==GWL_STYLE||index==GWL_EXSTYLE||index==GWLP_HWNDPARENT))return set_long_w(hwnd,index,value);
    return real_SetWindowLongPtrA(hwnd,index,value);
}
#ifdef _WIN64
static LONG WINAPI get_long32_w(HWND w,int i){DesktopCall desktop_call(_ReturnAddress());return (i==GWL_STYLE||i==GWL_EXSTYLE)&&!native_call&&root(w)?(LONG)get_long_w(w,i):real_GetWindowLongW(w,i);}
static LONG WINAPI get_long32_a(HWND w,int i){DesktopCall desktop_call(_ReturnAddress());return (i==GWL_STYLE||i==GWL_EXSTYLE)&&!native_call&&root(w)?(LONG)get_long_a(w,i):real_GetWindowLongA(w,i);}
static LONG WINAPI set_long32_w(HWND w,int i,LONG v){DesktopCall desktop_call(_ReturnAddress());return (i==GWL_STYLE||i==GWL_EXSTYLE)&&!native_call&&root(w)?(LONG)set_long_w(w,i,v):real_SetWindowLongW(w,i,v);}
static LONG WINAPI set_long32_a(HWND w,int i,LONG v){DesktopCall desktop_call(_ReturnAddress());return (i==GWL_STYLE||i==GWL_EXSTYLE)&&!native_call&&root(w)?(LONG)set_long_a(w,i,v):real_SetWindowLongA(w,i,v);}
#endif
static BOOL WINAPI window_rect(HWND hwnd,RECT *rect){DesktopCall desktop_call(_ReturnAddress());
    if(native_call)return real_GetWindowRect(hwnd,rect);
    Native native;if(hwnd==desktop){*rect={0,0,(LONG)property(WD_DESKTOP_WIDTH),(LONG)property(WD_DESKTOP_HEIGHT)};return TRUE;}
    BOOL ok=real_GetWindowRect(hwnd,rect);if(ok&&contained(hwnd))real_MapWindowPoints(nullptr,desktop,(POINT*)rect,2);return ok;
}
static int WINAPI map_points(HWND from,HWND to,POINT *points,UINT count){DesktopCall desktop_call(_ReturnAddress());
    if(native_call)return real_MapWindowPoints(from,to,points,count);
    // Hidden windows retain their requested native placement until adoption,
    // just as GetWindowRect does. Only contained endpoints use the container
    // for screen coordinates. HWND-to-HWND mapping stays native, including
    // mixed hidden/contained windows and USER32's two-point RTL rectangle rule.
    HWND source=!from&&contained(to)?desktop:from,target=!to&&contained(from)?desktop:to;
    Native native;return real_MapWindowPoints(source,target,points,count);
}
static BOOL WINAPI client_screen(HWND hwnd,POINT *point){DesktopCall desktop_call(_ReturnAddress());if(native_call||!contained(hwnd))return real_ClientToScreen(hwnd,point);Native native;SetLastError(0);return real_MapWindowPoints(hwnd,desktop,point,1)!=0||GetLastError()==0;}
static BOOL WINAPI screen_client(HWND hwnd,POINT *point){DesktopCall desktop_call(_ReturnAddress());if(native_call||!contained(hwnd))return real_ScreenToClient(hwnd,point);Native native;SetLastError(0);return real_MapWindowPoints(desktop,hwnd,point,1)!=0||GetLastError()==0;}
static BOOL WINAPI cursor(POINT *point){DesktopCall desktop_call(_ReturnAddress());if(native_call)return real_GetCursorPos(point);Native native;BOOL ok=real_GetCursorPos(point);if(ok)real_MapWindowPoints(nullptr,desktop,point,1);return ok;}
static BOOL WINAPI set_cursor(int x,int y){DesktopCall desktop_call(_ReturnAddress());if(native_call)return real_SetCursorPos(x,y);Native native;POINT point{x,y};real_MapWindowPoints(desktop,nullptr,&point,1);return real_SetCursorPos(point.x,point.y);}
static BOOL WINAPI clip(const RECT *rect){DesktopCall desktop_call(_ReturnAddress());if(native_call)return real_ClipCursor(rect);Native native;if(!rect)return real_ClipCursor(nullptr);RECT mapped=*rect;real_MapWindowPoints(desktop,nullptr,(POINT*)&mapped,2);return real_ClipCursor(&mapped);}
static BOOL WINAPI get_clip(RECT *rect){DesktopCall desktop_call(_ReturnAddress());if(native_call)return real_GetClipCursor(rect);Native native;BOOL ok=real_GetClipCursor(rect);if(ok)real_MapWindowPoints(nullptr,desktop,(POINT*)rect,2);return ok;}
static int WINAPI metrics(int index){DesktopCall desktop_call(_ReturnAddress());
    if(native_call)return real_GetSystemMetrics(index);
    switch(index){case SM_CXSCREEN:case SM_CXVIRTUALSCREEN:case SM_CXFULLSCREEN:return property(WD_DESKTOP_WIDTH);
        case SM_CYSCREEN:case SM_CYVIRTUALSCREEN:case SM_CYFULLSCREEN:return property(WD_DESKTOP_HEIGHT);
        case SM_XVIRTUALSCREEN:case SM_YVIRTUALSCREEN:return 0;case SM_CMONITORS:return 1;default:return real_GetSystemMetrics(index);}
}
static int WINAPI metrics_dpi(int index,UINT dpi){DesktopCall desktop_call(_ReturnAddress());
    if(!native_call)switch(index){
        case SM_CXSCREEN:case SM_CYSCREEN:case SM_CXVIRTUALSCREEN:case SM_CYVIRTUALSCREEN:case SM_CXFULLSCREEN:case SM_CYFULLSCREEN:
        case SM_XVIRTUALSCREEN:case SM_YVIRTUALSCREEN:case SM_CMONITORS:return metrics(index);
        default:break;
    }
    Native native;return real_GetSystemMetricsForDpi(index,dpi);
}
static BOOL WINAPI parameters_w(UINT action,UINT param,void *value,UINT flags){DesktopCall desktop_call(_ReturnAddress());
    if(native_call)return real_SystemParametersInfoW(action,param,value,flags);
    Native native;BOOL ok=real_SystemParametersInfoW(action,param,value,flags);
    if(ok&&action==SPI_GETWORKAREA&&value)*(RECT*)value=desktop_rect();
    return ok;
}
static BOOL WINAPI parameters_a(UINT action,UINT param,void *value,UINT flags){DesktopCall desktop_call(_ReturnAddress());
    if(native_call)return real_SystemParametersInfoA(action,param,value,flags);
    Native native;BOOL ok=real_SystemParametersInfoA(action,param,value,flags);
    if(ok&&action==SPI_GETWORKAREA&&value)*(RECT*)value=desktop_rect();
    return ok;
}
// As in Wine win32u/sysparams.c, empty rectangles select by their top-left
// point. Keeping that case separate also avoids signed overflow at LONG_MAX.
static HMONITOR select_monitor(const RECT &rect,DWORD flags){
    RECT bounds=desktop_rect(),intersection{};
    bool intersects=IsRectEmpty(&rect)?PtInRect(&bounds,{rect.left,rect.top})!=FALSE:IntersectRect(&intersection,&bounds,&rect)!=FALSE;
    return intersects||(flags&(MONITOR_DEFAULTTOPRIMARY|MONITOR_DEFAULTTONEAREST))?desktop_monitor():nullptr;
}
static HMONITOR WINAPI monitor_point(POINT point,DWORD flags){DesktopCall desktop_call(_ReturnAddress());
    if(native_call)return real_MonitorFromPoint(point,flags);
    RECT point_rect{point.x,point.y,point.x,point.y};return select_monitor(point_rect,flags);
}
static HMONITOR WINAPI monitor_rect(const RECT *rect,DWORD flags){DesktopCall desktop_call(_ReturnAddress());
    // NULL is invalid input and faults on native Windows; leave that contract
    // to USER32 instead of converting an invalid pointer into a valid monitor.
    if(native_call||!rect){Native native;return real_MonitorFromRect(rect,flags);}
    return select_monitor(*rect,flags);
}
static HMONITOR WINAPI monitor_window(HWND window,DWORD flags){DesktopCall desktop_call(_ReturnAddress());
    if(native_call)return real_MonitorFromWindow(window,flags);
    if(window==desktop)return desktop_monitor();
    if(!IsWindow(window))return flags&(MONITOR_DEFAULTTOPRIMARY|MONITOR_DEFAULTTONEAREST)?desktop_monitor():nullptr;
    // Hidden native driver/helper windows deliberately remain outside the
    // container. Only contained application windows have virtual coordinates.
    if(!contained(window)){Native native;return real_MonitorFromWindow(window,flags);}
    RECT rect{};WINDOWPLACEMENT placement{};placement.length=sizeof(placement);
    if(IsIconic(window)&&GetWindowPlacement(window,&placement))rect=placement.rcNormalPosition;
    else if(!window_rect(window,&rect))return flags&(MONITOR_DEFAULTTOPRIMARY|MONITOR_DEFAULTTONEAREST)?desktop_monitor():nullptr;
    return select_monitor(rect,flags);
}
static BOOL WINAPI monitor_w(HMONITOR monitor,MONITORINFO *info){DesktopCall desktop_call(_ReturnAddress());if(native_call)return real_GetMonitorInfoW(monitor,info);Native native;BOOL ok=real_GetMonitorInfoW(monitor,info);if(ok){info->rcMonitor=info->rcWork={0,0,(LONG)property(WD_DESKTOP_WIDTH),(LONG)property(WD_DESKTOP_HEIGHT)};info->dwFlags=MONITORINFOF_PRIMARY;}return ok;}
static BOOL WINAPI monitor_a(HMONITOR monitor,MONITORINFO *info){DesktopCall desktop_call(_ReturnAddress());if(native_call)return real_GetMonitorInfoA(monitor,info);Native native;BOOL ok=real_GetMonitorInfoA(monitor,info);if(ok){info->rcMonitor=info->rcWork={0,0,(LONG)property(WD_DESKTOP_WIDTH),(LONG)property(WD_DESKTOP_HEIGHT)};info->dwFlags=MONITORINFOF_PRIMARY;}return ok;}
static BOOL WINAPI monitors(HDC dc,const RECT *clip,MONITORENUMPROC callback,LPARAM data){DesktopCall desktop_call(_ReturnAddress());
    if(native_call)return real_EnumDisplayMonitors(dc,clip,callback,data);
    RECT rect=desktop_rect(),intersection;
    if(dc){
        Native native;RECT visible{};POINT origin{},desktop_origin{};
        if(GetClipBox(dc,&visible)==ERROR||!GetDCOrgEx(dc,&origin)||!real_ClientToScreen(desktop,&desktop_origin))return FALSE;
        OffsetRect(&rect,desktop_origin.x-origin.x,desktop_origin.y-origin.y);
        if(!IntersectRect(&intersection,&rect,&visible))return TRUE;rect=intersection;
    }
    if(clip){if(!IntersectRect(&intersection,&rect,clip))return TRUE;rect=intersection;}
    // Invoke application code outside the native guard so callbacks see the
    // same virtual geometry as the query that enumerated this monitor.
    return callback(desktop_monitor(),dc,&rect,data);
}
static HDC WINAPI get_dc(HWND hwnd){DesktopCall desktop_call(_ReturnAddress());if(native_call)return real_GetDC(hwnd);Native native;return real_GetDC(hwnd?hwnd:desktop);}
static HDC WINAPI get_dc_ex(HWND hwnd,HRGN region,DWORD flags){DesktopCall desktop_call(_ReturnAddress());if(native_call)return real_GetDCEx(hwnd,region,flags);Native native;return real_GetDCEx(hwnd?hwnd:desktop,region,flags);}
static int WINAPI release_dc(HWND hwnd,HDC dc){DesktopCall desktop_call(_ReturnAddress());if(native_call)return real_ReleaseDC(hwnd,dc);Native native;return real_ReleaseDC(hwnd?hwnd:desktop,dc);}

// Native USER32 promotes a child owner to its physical top-level ancestor. For
// contained applications that ancestor is the shared desktop, so native modal
// handling would disable every application in it. Keep modality on the logical
// application root and let USER32 run its normal dialog loop without an owner.
struct DialogOwner {
    HWND window{},focus{};bool disabled{};
    explicit DialogOwner(HWND requested,bool modal){
        window=requested;
        if(window&&contained(window)&&window!=desktop)window=ancestor(window,GA_ROOT);
        if(modal&&window&&window!=desktop&&contained(window)&&IsWindowEnabled(window)){
            focus=GetFocus();disabled=true;EnableWindow(window,FALSE);
        }
    }
    ~DialogOwner(){
        if(disabled&&IsWindow(window)){
            EnableWindow(window,TRUE);
            if(focus&&IsWindow(focus)&&contained(focus)&&IsWindowEnabled(focus))SetFocus(focus);
        }
    }
};
static void adopt_dialog(HWND window,HWND owner,bool center){
    DWORD style=(DWORD)real_GetWindowLongPtrW(window,GWL_STYLE);
    if((style&WS_CHILD)&&!root(window))return;
    adopt(window,SW_SHOW);
    if(!root(window))return;
    if(owner)SetPropW(window,owner_property,owner);else RemovePropW(window,owner_property);
    Native native;RECT rect{};real_GetWindowRect(window,&rect);real_MapWindowPoints(nullptr,desktop,(POINT*)&rect,2);
    int width=rect.right-rect.left,height=rect.bottom-rect.top;
    int screen_width=(int)property(WD_DESKTOP_WIDTH),screen_height=(int)property(WD_DESKTOP_HEIGHT);
    if(center||(style&DS_CENTER)){
        RECT target{0,0,screen_width,screen_height};
        if(owner&&contained(owner)){real_GetWindowRect(owner,&target);real_MapWindowPoints(nullptr,desktop,(POINT*)&target,2);}
        rect.left=target.left+(target.right-target.left-width)/2;rect.top=target.top+(target.bottom-target.top-height)/2;
    }else if(owner&&contained(owner)&&!(style&DS_ABSALIGN)){
        POINT origin{};real_MapWindowPoints(owner,desktop,&origin,1);rect.left+=origin.x;rect.top+=origin.y;
    }
    rect.left=std::max(0,std::min((int)rect.left,std::max(0,screen_width-width)));
    rect.top=std::max(0,std::min((int)rect.top,std::max(0,screen_height-height)));
    real_SetWindowPos(window,nullptr,rect.left,rect.top,0,0,SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE);
}
static const wchar_t dialog_property[]=L"Winedows.DialogContext";
struct DialogContext {
    DLGPROC proc;LPARAM parameter;HWND owner;unsigned references=1;
    DialogContext *previous;bool bound=false;
    void release(){if(!--references)delete this;}
};
static thread_local DialogContext *pending_dialog;
static INT_PTR CALLBACK dialog_bridge(HWND window,UINT message,WPARAM wp,LPARAM lp){
    auto context=(DialogContext*)GetPropW(window,dialog_property);
    if(!context&&pending_dialog&&!pending_dialog->bound){
        context=pending_dialog;
        if(!SetPropW(window,dialog_property,(HANDLE)context))return FALSE;
        context->bound=true;++context->references;
    }
    if(!context)return FALSE;
    if(message==WM_INITDIALOG){adopt_dialog(window,context->owner,false);lp=context->parameter;}
    // The callback may destroy this dialog or create another one. Keep its
    // context alive across both actions, and never suppress API virtualization
    // while running application code or the native modal message loop.
    ++context->references;
    INT_PTR result=context->proc(window,message,wp,lp);
    if(message==WM_NCDESTROY){RemovePropW(window,dialog_property);context->release();}
    context->release();return result;
}
struct DialogCall {
    DialogOwner owner;DialogContext *context;
    DialogCall(HWND parent,DLGPROC proc,LPARAM parameter,bool modal):owner(parent,modal){
        context=new DialogContext{proc,parameter,owner.window,1,pending_dialog,false};pending_dialog=context;
    }
    ~DialogCall(){pending_dialog=context->previous;context->release();}
};
static bool child_dialog_template(HINSTANCE,LPCDLGTEMPLATEW resource){
    if(!resource)return false;
    auto words=(const WORD*)resource;DWORD style;
    memcpy(&style,(const BYTE*)resource+(words[0]==1&&words[1]==0xffff?12:0),sizeof(style));
    return (style&WS_CHILD)!=0;
}
static bool child_dialog_template(HINSTANCE instance,LPCWSTR name){
    HRSRC resource=FindResourceW(instance,name,RT_DIALOG);
    return resource&&child_dialog_template(instance,(LPCDLGTEMPLATEW)LockResource(LoadResource(instance,resource)));
}
static bool child_dialog_template(HINSTANCE instance,LPCSTR name){
    HRSRC resource=FindResourceA(instance,name,MAKEINTRESOURCEA(5));
    return resource&&child_dialog_template(instance,(LPCDLGTEMPLATEW)LockResource(LoadResource(instance,resource)));
}
#define DIALOG_HOOK(suffix,Name,Template,Modal,Result) \
static Result WINAPI dialog_##Name##suffix(HINSTANCE instance,Template resource,HWND owner,DLGPROC proc,LPARAM parameter){DesktopCall desktop_call(_ReturnAddress()); \
    if(native_call||!proc||proc==dialog_bridge||(owner&&!IsWindow(owner))||child_dialog_template(instance,resource))return real_##Name##suffix(instance,resource,owner,proc,parameter); \
    DialogCall call(owner,proc,parameter,Modal); \
    return real_##Name##suffix(instance,resource,nullptr,dialog_bridge,parameter); \
}
DIALOG_HOOK(W,DialogBoxParam,LPCWSTR,true,INT_PTR)
DIALOG_HOOK(A,DialogBoxParam,LPCSTR,true,INT_PTR)
DIALOG_HOOK(W,DialogBoxIndirectParam,LPCDLGTEMPLATEW,true,INT_PTR)
DIALOG_HOOK(A,DialogBoxIndirectParam,LPCDLGTEMPLATEA,true,INT_PTR)
DIALOG_HOOK(W,CreateDialogParam,LPCWSTR,false,HWND)
DIALOG_HOOK(A,CreateDialogParam,LPCSTR,false,HWND)
DIALOG_HOOK(W,CreateDialogIndirectParam,LPCDLGTEMPLATEW,false,HWND)
DIALOG_HOOK(A,CreateDialogIndirectParam,LPCDLGTEMPLATEA,false,HWND)
#undef DIALOG_HOOK
struct MessageContext {HWND owner,window{};MessageContext *previous;};
static thread_local MessageContext *pending_message;
static LRESULT CALLBACK message_cbt(int code,WPARAM wp,LPARAM lp){
    if(code==HCBT_ACTIVATE&&pending_message&&!pending_message->window){
        HWND window=(HWND)wp;wchar_t name[32]{};GetClassNameW(window,name,32);
        if(!wcscmp(name,L"#32770")){pending_message->window=window;adopt_dialog(window,pending_message->owner,true);}
    }
    return CallNextHookEx(nullptr,code,wp,lp);
}
struct MessageCall {
    DialogOwner owner;MessageContext context;HHOOK hook;
    explicit MessageCall(HWND parent):owner(parent,true),context{owner.window,nullptr,pending_message}{
        pending_message=&context;hook=SetWindowsHookExW(WH_CBT,message_cbt,nullptr,GetCurrentThreadId());
    }
    ~MessageCall(){if(hook)UnhookWindowsHookEx(hook);pending_message=context.previous;}
};
static int WINAPI message_indirect_w(const MSGBOXPARAMSW *parameters){DesktopCall desktop_call(_ReturnAddress());
    if(native_call||!parameters||parameters->cbSize!=sizeof(*parameters)||(parameters->hwndOwner&&!IsWindow(parameters->hwndOwner)))return real_MessageBoxIndirectW(parameters);
    MessageCall call(parameters->hwndOwner);MSGBOXPARAMSW copy=*parameters;copy.hwndOwner=nullptr;return real_MessageBoxIndirectW(&copy);
}
static int WINAPI message_indirect_a(const MSGBOXPARAMSA *parameters){DesktopCall desktop_call(_ReturnAddress());
    if(native_call||!parameters||parameters->cbSize!=sizeof(*parameters)||(parameters->hwndOwner&&!IsWindow(parameters->hwndOwner)))return real_MessageBoxIndirectA(parameters);
    MessageCall call(parameters->hwndOwner);MSGBOXPARAMSA copy=*parameters;copy.hwndOwner=nullptr;return real_MessageBoxIndirectA(&copy);
}
static int WINAPI message_ex_w(HWND owner,LPCWSTR text,LPCWSTR caption,UINT type,WORD language){DesktopCall desktop_call(_ReturnAddress());
    if(native_call)return real_MessageBoxExW(owner,text,caption,type,language);
    MSGBOXPARAMSW parameters{sizeof(parameters),owner,nullptr,text,caption,type,nullptr,0,nullptr,language};return message_indirect_w(&parameters);
}
static int WINAPI message_ex_a(HWND owner,LPCSTR text,LPCSTR caption,UINT type,WORD language){DesktopCall desktop_call(_ReturnAddress());
    if(native_call)return real_MessageBoxExA(owner,text,caption,type,language);
    MSGBOXPARAMSA parameters{sizeof(parameters),owner,nullptr,text,caption,type,nullptr,0,nullptr,language};return message_indirect_a(&parameters);
}
static int WINAPI message_w(HWND owner,LPCWSTR text,LPCWSTR caption,UINT type){DesktopCall desktop_call(_ReturnAddress());return native_call?real_MessageBoxW(owner,text,caption,type):message_ex_w(owner,text,caption,type,0);}
static int WINAPI message_a(HWND owner,LPCSTR text,LPCSTR caption,UINT type){DesktopCall desktop_call(_ReturnAddress());return native_call?real_MessageBoxA(owner,text,caption,type):message_ex_a(owner,text,caption,type,0);}
template<class T,class U>static void hook(T &real,U fn){check(DetourAttach((PVOID*)&real,(PVOID)fn),"Attach virtual desktop API");}
void attach_desktop(const str &prefix_root){
    str value=env(L"WINEDOWS_DESKTOP_HWND");if(value.empty())return;
    desktop=(HWND)(ULONG_PTR)_wcstoui64(value.c_str(),nullptr,10);
    wchar_t name[64]{};GetClassNameW(desktop,name,64);
    if(!IsWindow(desktop)||wcscmp(name,L"WinedowsVirtualDesktop")||!property(WD_DESKTOP_WIDTH))throw std::runtime_error("Invalid virtual desktop host");
    initialize_native_images();
    geometry_trace=env(L"WINEDOWS_TRACE_DESKTOP")==L"1";
    if(geometry_trace){
        DWORD saved=GetLastError();
        str path=prefix_root+L"\\.winedows\\desktop-"+std::to_wstring(GetCurrentProcessId())+L".log";
        geometry_trace_file=CreateFileW(path.c_str(),FILE_APPEND_DATA,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
        HMODULE user=GetModuleHandleW(L"user32.dll");
        trace_thread_context=reinterpret_cast<decltype(trace_thread_context)>(GetProcAddress(user,"GetThreadDpiAwarenessContext"));
        trace_window_context=reinterpret_cast<decltype(trace_window_context)>(GetProcAddress(user,"GetWindowDpiAwarenessContext"));
        trace_awareness=reinterpret_cast<decltype(trace_awareness)>(GetProcAddress(user,"GetAwarenessFromDpiAwarenessContext"));
        trace_window_dpi=reinterpret_cast<decltype(trace_window_dpi)>(GetProcAddress(user,"GetDpiForWindow"));
        SetLastError(saved);
    }
    hook(real_CreateWindowExW,create_w);hook(real_CreateWindowExA,create_a);hook(real_GetDesktopWindow,get_desktop);
    hook(real_GetParent,parent);hook(real_GetAncestor,ancestor);hook(real_GetWindow,get_window);
    hook(real_FindWindowW,find_window_w);hook(real_FindWindowA,find_window_a);hook(real_FindWindowExW,find_window_ex_w);hook(real_FindWindowExA,find_window_ex_a);
    hook(real_GetWindowLongPtrW,get_long_w);hook(real_GetWindowLongPtrA,get_long_a);hook(real_SetWindowLongPtrW,set_long_w);hook(real_SetWindowLongPtrA,set_long_a);
#ifdef _WIN64
    hook(real_GetWindowLongW,get_long32_w);hook(real_GetWindowLongA,get_long32_a);hook(real_SetWindowLongW,set_long32_w);hook(real_SetWindowLongA,set_long32_a);
#endif
    hook(real_GetWindowRect,window_rect);hook(real_ClientToScreen,client_screen);hook(real_ScreenToClient,screen_client);hook(real_MapWindowPoints,map_points);
    hook(real_GetCursorPos,cursor);hook(real_SetCursorPos,set_cursor);hook(real_ClipCursor,clip);hook(real_GetClipCursor,get_clip);
    hook(real_GetSystemMetrics,metrics);hook(real_GetMonitorInfoW,monitor_w);hook(real_GetMonitorInfoA,monitor_a);hook(real_EnumDisplayMonitors,monitors);
    hook(real_MonitorFromPoint,monitor_point);hook(real_MonitorFromRect,monitor_rect);hook(real_MonitorFromWindow,monitor_window);
    hook(real_SystemParametersInfoW,parameters_w);hook(real_SystemParametersInfoA,parameters_a);
    real_GetSystemMetricsForDpi=(decltype(real_GetSystemMetricsForDpi))GetProcAddress(GetModuleHandleW(L"user32.dll"),"GetSystemMetricsForDpi");
    if(real_GetSystemMetricsForDpi)hook(real_GetSystemMetricsForDpi,metrics_dpi);
    hook(real_GetDC,get_dc);hook(real_GetDCEx,get_dc_ex);hook(real_ReleaseDC,release_dc);
    hook(real_GetForegroundWindow,foreground);hook(real_GetActiveWindow,active_window);hook(real_SetForegroundWindow,set_foreground);
    hook(real_ShowWindow,show_window);hook(real_ShowWindowAsync,show_window_async);
    hook(real_SetWindowPos,position_window);hook(real_SetWindowPlacement,place_window);
    hook(real_DialogBoxParamW,dialog_DialogBoxParamW);hook(real_DialogBoxParamA,dialog_DialogBoxParamA);
    hook(real_DialogBoxIndirectParamW,dialog_DialogBoxIndirectParamW);hook(real_DialogBoxIndirectParamA,dialog_DialogBoxIndirectParamA);
    hook(real_CreateDialogParamW,dialog_CreateDialogParamW);hook(real_CreateDialogParamA,dialog_CreateDialogParamA);
    hook(real_CreateDialogIndirectParamW,dialog_CreateDialogIndirectParamW);hook(real_CreateDialogIndirectParamA,dialog_CreateDialogIndirectParamA);
    hook(real_MessageBoxW,message_w);hook(real_MessageBoxA,message_a);hook(real_MessageBoxExW,message_ex_w);hook(real_MessageBoxExA,message_ex_a);
    hook(real_MessageBoxIndirectW,message_indirect_w);hook(real_MessageBoxIndirectA,message_indirect_a);
}
}
