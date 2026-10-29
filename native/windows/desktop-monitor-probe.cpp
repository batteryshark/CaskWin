// SPDX-License-Identifier: LGPL-2.1-or-later
// Read-only monitor queries and fixture-owned windows; never changes host modes/input.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdio>
#include <string>
#include <vector>

static int failures;
static void check(bool ok,const char *name){printf("%s %s (error=%lu)\n",ok?"PASS":"FAIL",name,GetLastError());if(!ok)++failures;}
static bool same(const RECT &a,const RECT &b){return EqualRect(&a,&b)!=FALSE;}
static void describe(const char *name,const RECT &r){printf("%s %ld %ld %ld %ld\n",name,r.left,r.top,r.right,r.bottom);}
template<class T>static T export_address(HMODULE dll,const char *name){FARPROC address=GetProcAddress(dll,name);T result{};static_assert(sizeof(result)==sizeof(address),"function pointer size");memcpy(&result,&address,sizeof(result));return result;}
static LONG CALLBACK null_rect_fault(EXCEPTION_POINTERS *e){if(e->ExceptionRecord->ExceptionCode==EXCEPTION_ACCESS_VIOLATION){puts("PASS null rectangle retains native access violation");fflush(stdout);ExitProcess(77);}return EXCEPTION_CONTINUE_SEARCH;}
struct Enumeration {std::vector<HMONITOR> handles;std::vector<RECT> rectangles;HDC dc{};bool dc_matches=true;bool stop=false;};
static BOOL CALLBACK enumerate(HMONITOR monitor,HDC dc,RECT *rect,LPARAM context){auto &result=*(Enumeration*)context;result.handles.push_back(monitor);result.rectangles.push_back(*rect);result.dc_matches&=dc==result.dc;return !result.stop;}
static HMONITOR basic(bool native){
    RECT screen{0,0,GetSystemMetrics(SM_CXSCREEN),GetSystemMetrics(SM_CYSCREEN)};
    HMONITOR primary=MonitorFromPoint({0,0},MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO info{sizeof(info)};check(primary&&GetMonitorInfoW(primary,&info),"query primary monitor");
    describe("MONITOR",info.rcMonitor);describe("WORKAREA",info.rcWork);
    if(!native)check(same(info.rcMonitor,screen)&&same(info.rcWork,screen),"virtual monitor and metrics agree");
    RECT work{},work_a{};check(SystemParametersInfoW(SPI_GETWORKAREA,0,&work,0)!=FALSE,"Unicode work-area query");
    check(SystemParametersInfoA(SPI_GETWORKAREA,0,&work_a,0)&&same(work,work_a),"ANSI work-area query agrees");
    check(same(work,info.rcWork),"SPI work area agrees with primary monitor");
    using MetricsDpi=int(WINAPI*)(int,UINT);auto metrics=export_address<MetricsDpi>(GetModuleHandleW(L"user32.dll"),"GetSystemMetricsForDpi");
    if(metrics)for(UINT dpi:{96u,144u,192u})check(metrics(SM_CXSCREEN,dpi)==screen.right&&metrics(SM_CYSCREEN,dpi)==screen.bottom&&(!native?metrics(SM_CMONITORS,dpi)==1:true),"DPI metric queries retain screen geometry");
    if(metrics&&!native)check(metrics(SM_CXFULLSCREEN,144)==GetSystemMetrics(SM_CXFULLSCREEN)&&metrics(SM_CYFULLSCREEN,144)==GetSystemMetrics(SM_CYFULLSCREEN),"DPI fullscreen metrics agree with the virtual screen");
    check(MonitorFromWindow(nullptr,MONITOR_DEFAULTTONULL)==nullptr,"null window returns no monitor");
    check(MonitorFromWindow(nullptr,MONITOR_DEFAULTTOPRIMARY)==primary&&MonitorFromWindow(nullptr,MONITOR_DEFAULTTONEAREST)==primary,"null window default flags select primary");
    POINT inside{screen.right-1,screen.bottom-1};check(MonitorFromPoint(inside,MONITOR_DEFAULTTONULL)==primary,"last pixel lies inside monitor");
    RECT empty{0,0,0,0},inverted{0,0,-1,-1};
    check(MonitorFromRect(&empty,0)==primary&&MonitorFromRect(&inverted,0)==primary,"empty and inverted rectangles use their top-left point");
    Enumeration invalid_dc;check(!EnumDisplayMonitors((HDC)1,nullptr,enumerate,(LPARAM)&invalid_dc)&&invalid_dc.handles.empty(),"invalid DC fails without invoking monitor callback");
    if(native){DISPLAY_DEVICEW device{};device.cb=sizeof(device);for(DWORD index=0;EnumDisplayDevicesW(nullptr,index,&device,0);++index){DEVMODEW mode{};mode.dmSize=sizeof(mode);if((device.StateFlags&DISPLAY_DEVICE_ATTACHED_TO_DESKTOP)&&EnumDisplaySettingsW(device.DeviceName,ENUM_CURRENT_SETTINGS,&mode))printf("HOST_MODE %ls %lu %lu %lu %lu %ld %ld\n",device.DeviceName,mode.dmPelsWidth,mode.dmPelsHeight,mode.dmBitsPerPel,mode.dmDisplayFrequency,mode.dmPosition.x,mode.dmPosition.y);}}
    if(!native){
        for(POINT point:std::vector<POINT>{{screen.right,10},{10,screen.bottom},{-1,10},{10,-1}}){
            check(!MonitorFromPoint(point,MONITOR_DEFAULTTONULL),"point beyond virtual edge has no monitor");
            check(MonitorFromPoint(point,MONITOR_DEFAULTTOPRIMARY)==primary&&MonitorFromPoint(point,MONITOR_DEFAULTTONEAREST)==primary,"outside point default flags select virtual monitor");
        }
        RECT outside{screen.right,0,screen.right+20,20},overlap{screen.right-1,0,screen.right+20,20};
        RECT empty_outside{-1,0,20,-1};
        check(!MonitorFromRect(&outside,0)&&!MonitorFromRect(&empty_outside,0),"outside and empty-outside rectangles have no monitor");
        check(MonitorFromRect(&overlap,0)==primary,"partially overlapping rectangle selects virtual monitor");
        check(MonitorFromRect(&outside,1)==primary&&MonitorFromRect(&outside,2)==primary,"outside rectangle default flags select virtual monitor");
        Enumeration all;check(EnumDisplayMonitors(nullptr,nullptr,enumerate,(LPARAM)&all)&&all.handles.size()==1&&all.handles[0]==primary&&same(all.rectangles[0],screen),"monitor enumeration agrees with point and bounds queries");
        Enumeration clipped;RECT clip{20,30,80,90};check(EnumDisplayMonitors(nullptr,&clip,enumerate,(LPARAM)&clipped)&&clipped.handles.size()==1&&same(clipped.rectangles[0],clip),"monitor enumeration intersects caller clip");
        Enumeration stopped;stopped.stop=true;check(!EnumDisplayMonitors(nullptr,nullptr,enumerate,(LPARAM)&stopped)&&stopped.handles.size()==1,"monitor enumeration propagates callback stop");
    }
    UINT delay=0;check(SystemParametersInfoW(SPI_GETKEYBOARDDELAY,0,&delay,0)!=FALSE,"unrelated system-parameter queries remain native");
    return primary;
}
static void window_cases(HMONITOR primary){
    HWND desktop=GetDesktopWindow();RECT screen{};GetClientRect(desktop,&screen);
    HWND inside=CreateWindowExW(WS_EX_NOACTIVATE,L"STATIC",L"Monitor fixture",WS_POPUP|WS_VISIBLE,40,50,160,120,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    check(inside&&IsChild(desktop,inside),"fixture window is contained");if(!inside)return;
    check(MonitorFromWindow(inside,0)==primary,"contained window selects virtual monitor");
    HWND child=CreateWindowExW(0,L"STATIC",L"Child",WS_CHILD|WS_VISIBLE,5,6,20,30,inside,nullptr,GetModuleHandleW(nullptr),nullptr);
    check(child&&MonitorFromWindow(child,0)==primary,"child window selects virtual monitor");
    SetWindowPos(inside,nullptr,screen.right+20,40,0,0,SWP_NOSIZE|SWP_NOACTIVATE|SWP_NOZORDER);
    RECT actual{};GetWindowRect(inside,&actual);describe("OFFSCREEN",actual);
    check(!MonitorFromWindow(inside,MONITOR_DEFAULTTONULL),"offscreen contained window has no monitor");
    check(MonitorFromWindow(inside,MONITOR_DEFAULTTOPRIMARY)==primary&&MonitorFromWindow(inside,MONITOR_DEFAULTTONEAREST)==primary,"offscreen contained window default flags select virtual monitor");
    ShowWindow(inside,SW_SHOWMINNOACTIVE);
    check(IsIconic(inside)&&!MonitorFromWindow(inside,MONITOR_DEFAULTTONULL),"minimized window selects by its offscreen restore position");
    ShowWindow(inside,SW_SHOWNOACTIVATE);
    SetWindowPos(inside,nullptr,40,50,0,0,SWP_NOSIZE|SWP_NOACTIVATE|SWP_NOZORDER);
    HDC dc=GetDC(inside);RECT clip{};GetClipBox(dc,&clip);Enumeration from_dc;from_dc.dc=dc;
    check(EnumDisplayMonitors(dc,nullptr,enumerate,(LPARAM)&from_dc)&&from_dc.handles.size()==1&&from_dc.dc_matches&&same(from_dc.rectangles[0],clip),"monitor enumeration respects window DC clipping and coordinates");
    Enumeration clipped_dc;clipped_dc.dc=dc;RECT client_clip{5,6,30,40};
    check(EnumDisplayMonitors(dc,&client_clip,enumerate,(LPARAM)&clipped_dc)&&clipped_dc.handles.size()==1&&same(clipped_dc.rectangles[0],client_clip),"monitor enumeration intersects caller and window DC clips");
    ReleaseDC(inside,dc);DestroyWindow(inside);
}
static DWORD wait_child(PROCESS_INFORMATION &pi){DWORD result=1,start=GetTickCount();while(GetTickCount()-start<20000){DWORD state=MsgWaitForMultipleObjects(1,&pi.hProcess,FALSE,100,QS_ALLINPUT);if(state==WAIT_OBJECT_0){GetExitCodeProcess(pi.hProcess,&result);break;}MSG msg;while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)){TranslateMessage(&msg);DispatchMessageW(&msg);}}if(result)TerminateProcess(pi.hProcess,99);CloseHandle(pi.hThread);CloseHandle(pi.hProcess);return result;}
static void child_cases(const wchar_t *program){
    DEVMODEW requested{};requested.dmSize=sizeof(requested);requested.dmFields=DM_PELSWIDTH|DM_PELSHEIGHT|DM_BITSPERPEL;requested.dmPelsWidth=641;requested.dmPelsHeight=481;requested.dmBitsPerPel=32;
    check(ChangeDisplaySettingsW(&requested,CDS_FULLSCREEN)==DISP_CHANGE_SUCCESSFUL,"set only the virtual container resolution");
    wchar_t environment[]=L"DESKTOP_MONITOR_CHILD=1\0\0";std::wstring command=L"\""+std::wstring(program)+L"\" --virtual --windows";
    STARTUPINFOW si{};si.cb=sizeof(si);PROCESS_INFORMATION pi{};
    BOOL created=CreateProcessW(program,command.data(),nullptr,nullptr,FALSE,CREATE_UNICODE_ENVIRONMENT,environment,nullptr,&si,&pi);check(created!=FALSE,"launch cross-bitness monitor child with custom environment");
    if(created)check(wait_child(pi)==0,"child sees the current desktop monitor topology");
    check(ChangeDisplaySettingsW(nullptr,0)==DISP_CHANGE_SUCCESSFUL,"restore initial virtual container resolution");
}
struct SdlRect{int x,y,w,h;};
template<class T>static T sdl_export(HMODULE dll,const char *name){auto fn=export_address<T>(dll,name);if(!fn){printf("FAIL missing SDL export %s\n",name);++failures;}return fn;}
static void sdl_cases(const wchar_t *path){
    int before=failures;
    HMODULE dll=LoadLibraryW(path);check(dll!=nullptr,"load unchanged staged game SDL2");if(!dll)return;
    auto init=sdl_export<int(__cdecl*)(unsigned)>(dll,"SDL_Init");auto quit=sdl_export<void(__cdecl*)()>(dll,"SDL_Quit");
    auto displays=sdl_export<int(__cdecl*)()>(dll,"SDL_GetNumVideoDisplays");auto bounds=sdl_export<int(__cdecl*)(int,SdlRect*)>(dll,"SDL_GetDisplayBounds");
    auto usable=sdl_export<int(__cdecl*)(int,SdlRect*)>(dll,"SDL_GetDisplayUsableBounds");
    auto create=sdl_export<void*(__cdecl*)(const char*,int,int,int,int,unsigned)>(dll,"SDL_CreateWindow");auto destroy=sdl_export<void(__cdecl*)(void*)>(dll,"SDL_DestroyWindow");
    auto position=sdl_export<void(__cdecl*)(void*,int*,int*)>(dll,"SDL_GetWindowPosition");auto index=sdl_export<int(__cdecl*)(void*)>(dll,"SDL_GetWindowDisplayIndex");
    if(failures!=before){FreeLibrary(dll);return;}
    int result=init(0x20);check(result==0,"initialize actual game SDL video backend without a renderer");
    if(!result){
        SdlRect area{},work{};int width=GetSystemMetrics(SM_CXSCREEN),height=GetSystemMetrics(SM_CYSCREEN);
        printf("SDL_DISPLAYS %d\n",displays());
        check(!bounds(0,&area)&&area.x==0&&area.y==0&&area.w==width&&area.h==height,"actual SDL display bounds match virtual desktop");
        check(!usable(0,&work)&&work.x==0&&work.y==0&&work.w==width&&work.h==height,"actual SDL usable bounds match virtual desktop");
        void *window=create("Winedows staged SDL monitor test",0x2fff0000,0x2fff0000,160,120,0x18);
        check(window!=nullptr,"actual SDL creates a centered hidden borderless window");
        if(window){int x=-1,y=-1;position(window,&x,&y);printf("SDL_POSITION %d %d\n",x,y);check(x==(width-160)/2&&y==(height-120)/2&&index(window)==0,"actual SDL centers its window on the virtual display");destroy(window);}
        quit();
    }
    FreeLibrary(dll);
}
int wmain(int argc,wchar_t **argv){
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
    bool native=false,windows=false;const wchar_t *child=nullptr,*sdl=nullptr;
    for(int i=1;i<argc;i++){if(!wcscmp(argv[i],L"--native"))native=true;else if(!wcscmp(argv[i],L"--windows"))windows=true;else if(!wcscmp(argv[i],L"--child")&&i+1<argc)child=argv[++i];else if(!wcscmp(argv[i],L"--sdl")&&i+1<argc)sdl=argv[++i];else if(!wcscmp(argv[i],L"--null-rect")){AddVectoredExceptionHandler(1,null_rect_fault);MonitorFromRect(nullptr,0);puts("FAIL null rectangle did not retain native fault");return 1;}}
    if(!native){wchar_t name[64]{};GetClassNameW(GetDesktopWindow(),name,64);check(!wcscmp(name,L"WinedowsVirtualDesktop"),"fixture uses the virtual desktop");if(failures)return 1;}
    HMONITOR primary=basic(native);if(windows)window_cases(primary);if(child)child_cases(child);if(sdl)sdl_cases(sdl);
    printf("Desktop monitor %zu-bit: %d failure(s)\n",sizeof(void*)*8,failures);return failures?1:0;
}
