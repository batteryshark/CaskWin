// SPDX-License-Identifier: LGPL-2.1-or-later
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <string>
static int failures;
static void check(bool ok,const char *name){printf("%s %s (error=%lu)\n",ok?"PASS":"FAIL",name,GetLastError());if(!ok)++failures;}
static LRESULT CALLBACK procedure(HWND window,UINT msg,WPARAM wp,LPARAM lp){
    if(msg==WM_PAINT){PAINTSTRUCT ps;HDC dc=BeginPaint(window,&ps);RECT rect;GetClientRect(window,&rect);HBRUSH brush=CreateSolidBrush(RGB(24,90,135));FillRect(dc,&rect,brush);DeleteObject(brush);SetTextColor(dc,RGB(255,255,255));SetBkMode(dc,TRANSPARENT);DrawTextW(dc,L"Winedows virtual desktop\n640 x 480, 16-bit virtual mode\nHost display unchanged",-1,&rect,DT_CENTER|DT_WORDBREAK);EndPaint(window,&ps);return 0;}
    return DefWindowProcW(window,msg,wp,lp);
}
int wmain(int argc,wchar_t **argv){
    HWND desktop=GetDesktopWindow();wchar_t name[64]{};GetClassNameW(desktop,name,64);
    check(!wcscmp(name,L"WinedowsVirtualDesktop"),"virtual desktop window");
    if(failures)return 1;
    DEVMODEW initial{};initial.dmSize=sizeof(initial);EnumDisplaySettingsW(nullptr,ENUM_CURRENT_SETTINGS,&initial);
    check(GetSystemMetrics(SM_CXSCREEN)==(int)initial.dmPelsWidth&&GetSystemMetrics(SM_CYSCREEN)==(int)initial.dmPelsHeight,"desktop metrics match display mode");
    WNDCLASSW cls{};cls.lpfnWndProc=procedure;cls.hInstance=GetModuleHandleW(nullptr);cls.hCursor=LoadCursorW(nullptr,IDC_ARROW);cls.lpszClassName=L"WinedowsDesktopProbe";RegisterClassW(&cls);
    DWORD style=WS_OVERLAPPEDWINDOW|WS_VISIBLE;
    HWND window=CreateWindowExW(0,cls.lpszClassName,L"Contained game window",style,20,25,300,200,nullptr,nullptr,cls.hInstance,nullptr);
    check(window&&IsChild(desktop,window),"top-level game window is contained");
    check(GetParent(window)==nullptr&&GetAncestor(window,GA_PARENT)==desktop&&GetAncestor(window,GA_ROOT)==window,"logical top-level relationships");
    check((DWORD)GetWindowLongPtrW(window,GWL_STYLE)==style,"logical game window style");
    HWND hidden=CreateWindowExW(0,cls.lpszClassName,L"Hidden helper",WS_POPUP,0,0,100,100,nullptr,nullptr,cls.hInstance,nullptr);
    check(hidden&&!IsChild(desktop,hidden),"hidden helpers do not join the desktop input queue");
    ShowWindow(hidden,SW_SHOWNOACTIVATE);
    check(IsChild(desktop,hidden)&&(GetWindowLongPtrW(hidden,GWL_STYLE)&WS_VISIBLE),"hidden application window joins when shown");
    ShowWindow(hidden,SW_HIDE);check(!(GetWindowLongPtrW(hidden,GWL_STYLE)&WS_VISIBLE),"logical visibility follows show/hide");DestroyWindow(hidden);
    if(SetForegroundWindow(window)){
        check(GetForegroundWindow()==window&&GetFocus()==window,"foreground and keyboard focus reach the contained game");
        check(GetActiveWindow()==window,"game sees its logical active window");
    }else puts("Foreground request declined by Windows; focus assertions skipped");
    RECT rect{};GetWindowRect(window,&rect);check(rect.left==20&&rect.top==25&&rect.right-rect.left==300,"window rectangle uses virtual coordinates");
    POINT point{};ClientToScreen(window,&point);POINT start=point;ScreenToClient(window,&point);check(point.x==0&&point.y==0&&start.x>=20&&start.y>=25,"screen/client coordinate round trip");
    POINT mapped{};MapWindowPoints(window,nullptr,&mapped,1);check(mapped.x==start.x&&mapped.y==start.y,"MapWindowPoints uses the virtual origin");
    SetWindowLongPtrW(window,GWL_STYLE,WS_POPUP|WS_VISIBLE);SetWindowPos(window,nullptr,0,0,640,480,SWP_NOZORDER|SWP_FRAMECHANGED);
    check(IsChild(desktop,window)&&(DWORD)GetWindowLongW(window,GWL_STYLE)==(WS_POPUP|WS_VISIBLE),"fullscreen style remains contained");
    DEVMODEW mode{};mode.dmSize=sizeof(mode);mode.dmFields=DM_PELSWIDTH|DM_PELSHEIGHT|DM_BITSPERPEL;mode.dmPelsWidth=640;mode.dmPelsHeight=480;mode.dmBitsPerPel=16;
    check(ChangeDisplaySettingsExW(nullptr,&mode,nullptr,CDS_FULLSCREEN,nullptr)==DISP_CHANGE_SUCCESSFUL,"virtual 640x480x16 fullscreen switch");
    DEVMODEW current{};current.dmSize=sizeof(current);EnumDisplaySettingsW(nullptr,ENUM_CURRENT_SETTINGS,&current);
    check(current.dmPelsWidth==640&&current.dmPelsHeight==480&&current.dmBitsPerPel==16,"virtual mode is current");
    GetClientRect(desktop,&rect);check(rect.right==640&&rect.bottom==480,"host container resizes to virtual mode");
    check(GetSystemMetrics(SM_CXSCREEN)==640&&GetSystemMetrics(SM_CYSCREEN)==480,"metrics follow virtual mode");
    MONITORINFO monitor{};monitor.cbSize=sizeof(monitor);GetMonitorInfoW(MonitorFromWindow(window,MONITOR_DEFAULTTOPRIMARY),&monitor);
    check(monitor.rcMonitor.left==0&&monitor.rcMonitor.top==0&&monitor.rcMonitor.right==640&&monitor.rcMonitor.bottom==480,"monitor bounds follow virtual mode");
    for(int i=1;i+1<argc;i++)if(!wcscmp(argv[i],L"--child")){
        std::wstring command=L"\""+std::wstring(argv[i+1])+L"\"";wchar_t environment[]=L"DESKTOP_CHILD_FIXTURE=1\0\0";
        STARTUPINFOW si{};si.cb=sizeof(si);PROCESS_INFORMATION pi{};
        BOOL created=CreateProcessW(argv[i+1],command.data(),nullptr,nullptr,FALSE,CREATE_UNICODE_ENVIRONMENT,environment,nullptr,&si,&pi);
        check(created!=FALSE,"child launches with custom environment");
        if(created){
            // A GUI thread must keep dispatching while another GUI process
            // uses its parent container/input queue.
            DWORD code=1,end=GetTickCount()+20000;MSG message;
            while((LONG)(end-GetTickCount())>0){
                if(MsgWaitForMultipleObjects(1,&pi.hProcess,FALSE,100,QS_ALLINPUT)==WAIT_OBJECT_0){GetExitCodeProcess(pi.hProcess,&code);break;}
                while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}
            }
            if(code)TerminateProcess(pi.hProcess,99);check(code==0,"other-bitness child shares virtual desktop");CloseHandle(pi.hProcess);CloseHandle(pi.hThread);
        }
    }
    for(int i=1;i<argc;i++)if(!wcscmp(argv[i],L"--show")){
        DWORD end=GetTickCount()+15000;MSG msg;
        while((LONG)(end-GetTickCount())>0){while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)){TranslateMessage(&msg);DispatchMessageW(&msg);}Sleep(10);}
    }
    ChangeDisplaySettingsW(nullptr,0);EnumDisplaySettingsW(nullptr,ENUM_CURRENT_SETTINGS,&current);
    check(current.dmPelsWidth==(DWORD)(ULONG_PTR)GetPropW(desktop,L"Winedows.Desktop.InitialWidth")&&current.dmBitsPerPel==32,"reset returns to configured virtual desktop");
    DestroyWindow(window);printf("Virtual desktop %zu-bit: %d failure(s)\n",sizeof(void*)*8,failures);return failures?1:0;
}
