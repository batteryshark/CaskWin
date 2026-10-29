// SPDX-License-Identifier: LGPL-2.1-or-later
// Owned-window DPI/geometry diagnostic. No external window enumeration, input,
// screen capture, display mode changes, or graphics device is used.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _WIN32_WINNT 0x0a00
#include <windows.h>
#include <shellscalingapi.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cwchar>

static decltype(&GetThreadDpiAwarenessContext) thread_context;
static decltype(&GetWindowDpiAwarenessContext) window_context;
static decltype(&GetAwarenessFromDpiAwarenessContext) awareness;
static decltype(&GetDpiForWindow) window_dpi;
static decltype(&GetProcessDpiAwareness) process_awareness;
static decltype(&GetDpiForMonitor) monitor_dpi;
static decltype(&AreDpiAwarenessContextsEqual) equal_context;
static HWND container;
static bool owned_hosting;
static const char *case_name="startup";
static int expected_awareness,collection_errors,regressions;
static unsigned size_events,dpi_events;
static UINT last_width,last_height;
template<class T> static void resolve(HMODULE module,const char *name,T &target){auto address=GetProcAddress(module,name);static_assert(sizeof(target)==sizeof(address),"Windows export pointer size");memcpy(&target,&address,sizeof(target));}

static int process_mode(){PROCESS_DPI_AWARENESS value=PROCESS_DPI_UNAWARE;return SUCCEEDED(process_awareness(GetCurrentProcess(),&value))?int(value):-1;}
static int thread_mode(){return int(awareness(thread_context()));}
static int context_kind(DPI_AWARENESS_CONTEXT value){for(int i=1;i<=5;++i)if(equal_context(value,reinterpret_cast<DPI_AWARENESS_CONTEXT>(INT_PTR(-i))))return -i;return 0;}
static void rect_json(const RECT &r){printf("[%ld,%ld,%ld,%ld]",r.left,r.top,r.right,r.bottom);}
static void snapshot(const char *stage,HWND window=nullptr){
    RECT rect{},client{},parent_client{};POINT origin{};
    bool rectangles=!window||(GetWindowRect(window,&rect)&&GetClientRect(window,&client)&&ClientToScreen(window,&origin));
    if(!rectangles)++collection_errors;
    HDC dc=GetDC(window);int dc_dpi=dc?GetDeviceCaps(dc,LOGPIXELSX):0;if(dc)ReleaseDC(window,dc);
    UINT mx=0,my=0;HRESULT mh=monitor_dpi(MonitorFromWindow(window,MONITOR_DEFAULTTOPRIMARY),MDT_EFFECTIVE_DPI,&mx,&my);
    if(container)GetClientRect(container,&parent_client);
    printf("{\"kind\":\"snapshot\",\"case\":\"%s\",\"stage\":\"%s\",\"processAwareness\":%d,\"threadAwareness\":%d,\"windowAwareness\":%d,\"windowDpi\":%u,\"dcDpi\":%d,\"monitorDpi\":[%u,%u],\"monitorHresult\":%ld,\"rectanglesValid\":%s,\"windowRect\":",case_name,stage,process_mode(),thread_mode(),window?int(awareness(window_context(window))):-1,window?window_dpi(window):0,dc_dpi,mx,my,long(mh),rectangles?"true":"false");
    rect_json(rect);fputs(",\"clientRect\":",stdout);rect_json(client);
    printf(",\"clientOrigin\":[%ld,%ld],\"containerAwareness\":%d,\"containerDpi\":%u,\"containerClient\":",origin.x,origin.y,container?int(awareness(window_context(container))):-1,container?window_dpi(container):0);
    rect_json(parent_client);printf(",\"threadContext\":%d,\"windowContext\":%d,\"containerContext\":%d,\"contained\":%s,\"wmSizeCount\":%u,\"lastWmSize\":[%u,%u],\"wmDpiChangedCount\":%u}\n",context_kind(thread_context()),window?context_kind(window_context(window)):0,container?context_kind(window_context(container)):0,window&&container&&IsChild(container,window)?"true":"false",size_events,last_width,last_height,dpi_events);fflush(stdout);
}
static LRESULT CALLBACK procedure(HWND window,UINT message,WPARAM wp,LPARAM lp){
    if(message==WM_SIZE){++size_events;last_width=LOWORD(lp);last_height=HIWORD(lp);printf("{\"kind\":\"wm-size\",\"case\":\"%s\",\"width\":%u,\"height\":%u,\"reason\":%llu}\n",case_name,last_width,last_height,(unsigned long long)wp);}
    if(message==WM_DPICHANGED){++dpi_events;printf("{\"kind\":\"wm-dpi-changed\",\"case\":\"%s\",\"dpi\":[%u,%u],\"suggested\":",case_name,LOWORD(wp),HIWORD(wp));rect_json(*reinterpret_cast<RECT*>(lp));fputs("}\n",stdout);}
    if(message==WM_ERASEBKGND)return 1;
    return DefWindowProcW(window,message,wp,lp);
}
static void pump(){DWORD end=GetTickCount()+80;do{MSG msg;while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)){TranslateMessage(&msg);DispatchMessageW(&msg);}Sleep(1);}while((LONG)(end-GetTickCount())>0);}
static void exercise(const char *name,bool visible,bool decorated){
    case_name=name;size_events=dpi_events=last_width=last_height=0;
    DWORD style=decorated?WS_OVERLAPPEDWINDOW:WS_POPUP,ex=WS_EX_NOACTIVATE;
    RECT request{0,0,1280,720};if(decorated&&!AdjustWindowRectEx(&request,style,FALSE,ex)){++collection_errors;return;}
    snapshot("before-create");
    printf("{\"kind\":\"request\",\"case\":\"%s\",\"client\":[1280,720],\"outer\":[%ld,%ld],\"position\":[0,0],\"visible\":%s,\"decorated\":%s}\n",case_name,request.right-request.left,request.bottom-request.top,visible?"true":"false",decorated?"true":"false");
    DWORD physical_style=style|(visible?WS_VISIBLE:0);
    if(owned_hosting&&visible)physical_style=(physical_style&~WS_POPUP)|WS_CHILD|WS_CLIPSIBLINGS;
    HWND window=CreateWindowExW(ex,L"WinedowsOwnedDpiProbe",L"Winedows owned DPI diagnostic",physical_style,0,0,request.right-request.left,request.bottom-request.top,owned_hosting&&visible?container:nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    if(!window){printf("{\"kind\":\"error\",\"operation\":\"CreateWindowEx\",\"error\":%lu}\n",GetLastError());++collection_errors;return;}
    snapshot("after-create",window);
    if(owned_hosting&&!visible){
        SetWindowLongPtrW(window,GWL_STYLE,(style&~WS_POPUP)|WS_CHILD|WS_CLIPSIBLINGS);SetLastError(0);
        HWND old=SetParent(window,container);DWORD error=GetLastError();
        printf("{\"kind\":\"native-setparent\",\"succeeded\":%s,\"error\":%lu}\n",old||!error?"true":"false",error);
        if(!old&&error)++collection_errors;
        snapshot("after-native-setparent",window);
        SetWindowPos(window,nullptr,0,0,request.right-request.left,request.bottom-request.top,SWP_NOZORDER|SWP_NOACTIVATE|SWP_FRAMECHANGED);
    }
    if(!visible)ShowWindow(window,SW_SHOWNOACTIVATE);
    pump();snapshot("after-show",window);
    RECT actual{};GetClientRect(window,&actual);
    bool geometry=actual.right==1280&&actual.bottom==720;
    bool mode=process_mode()==expected_awareness&&thread_mode()==expected_awareness&&int(awareness(window_context(window)))==expected_awareness;
    if(!geometry||!mode)++regressions;
    printf("{\"kind\":\"case-result\",\"case\":\"%s\",\"requestedClientPreserved\":%s,\"awarenessPreserved\":%s}\n",case_name,geometry?"true":"false",mode?"true":"false");
    DestroyWindow(window);pump();
}
int wmain(int argc,wchar_t **argv){
    setvbuf(stdout,nullptr,_IONBF,0);
    expected_awareness=argc>1?_wtoi(argv[1]):0;bool virtualized=argc>2&&!wcscmp(argv[2],L"virtual");
    const wchar_t *scenario=argc>3?argv[3]:L"hidden-popup";owned_hosting=argc>4&&!wcscmp(argv[2],L"hosted");
    HMODULE user=GetModuleHandleW(L"user32.dll"),scaling=LoadLibraryW(L"shcore.dll");
#define GET(module,name,var) resolve(module,name,var)
    GET(user,"GetThreadDpiAwarenessContext",thread_context);GET(user,"GetWindowDpiAwarenessContext",window_context);GET(user,"GetAwarenessFromDpiAwarenessContext",awareness);GET(user,"GetDpiForWindow",window_dpi);
    GET(user,"AreDpiAwarenessContextsEqual",equal_context);
    if(scaling){GET(scaling,"GetProcessDpiAwareness",process_awareness);GET(scaling,"GetDpiForMonitor",monitor_dpi);}
#undef GET
    if(!thread_context||!window_context||!awareness||!window_dpi||!process_awareness||!monitor_dpi||!equal_context){puts("{\"kind\":\"unsupported\",\"reason\":\"Required Windows DPI APIs missing\"}");return 77;}
    if(virtualized){container=GetDesktopWindow();wchar_t cls[64]{};GetClassNameW(container,cls,64);if(wcscmp(cls,L"WinedowsVirtualDesktop")){puts("{\"kind\":\"error\",\"reason\":\"Virtual desktop not active\"}");return 1;}}
    if(owned_hosting){container=reinterpret_cast<HWND>(UINT_PTR(_wcstoui64(argv[4],nullptr,10)));if(!IsWindow(container))return 1;}
    snapshot("entry");if(process_mode()!=expected_awareness||thread_mode()!=expected_awareness)++collection_errors;
    WNDCLASSW cls{};cls.lpfnWndProc=procedure;cls.hInstance=GetModuleHandleW(nullptr);cls.lpszClassName=L"WinedowsOwnedDpiProbe";
    if(!RegisterClassW(&cls)){++collection_errors;}else{
        // Each style runs in a fresh process: SetParent can reset process DPI,
        // which must not contaminate the next scenario's initial conditions.
        if(!wcscmp(scenario,L"hidden-popup"))exercise("hidden-popup",false,false);
        else if(!wcscmp(scenario,L"visible-popup"))exercise("visible-popup",true,false);
        else if(!wcscmp(scenario,L"hidden-overlapped"))exercise("hidden-overlapped",false,true);
        else ++collection_errors;
        UnregisterClassW(cls.lpszClassName,cls.hInstance);
    }
    printf("{\"kind\":\"summary\",\"collectionErrors\":%d,\"regressions\":%d,\"expectedAwareness\":%d,\"virtualized\":%s,\"inputSynthesized\":false,\"capturePerformed\":false,\"hostModeChanged\":false}\n",collection_errors,regressions,expected_awareness,virtualized?"true":"false");
    FreeLibrary(scaling);return collection_errors?1:regressions?2:0;
}
