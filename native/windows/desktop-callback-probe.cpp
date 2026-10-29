// SPDX-License-Identifier: LGPL-2.1-or-later
// Owned windows only: exercise synchronous application callbacks from USER32.
#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0a00
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <initializer_list>
static const char *phase="entry";
static bool expand,ansi,inside_nested;
static unsigned observations,mismatches,nested_calls,nested_failures,coordinate_calls,coordinate_failures;
static int expected_width,expected_height;
static RECT expected_work;
static HWND tail_window,expected_desktop;
static unsigned tail_calls,tail_mismatches;
static POINT mixed_direct,mixed_via_screen;
extern "C" __attribute__((noinline)) LRESULT CALLBACK tail_procedure(HWND window,UINT message,WPARAM wp,LPARAM lp){
    if(message==WM_APP+17)return reinterpret_cast<LRESULT>(GetDesktopWindow());
    return DefWindowProcW(window,message,wp,lp);
}
static void observe(HWND window,UINT message){
    MONITORINFO monitor{};monitor.cbSize=sizeof(monitor);RECT work{};
    BOOL info=GetMonitorInfoW(MonitorFromWindow(window,MONITOR_DEFAULTTOPRIMARY),&monitor);
    BOOL area=SystemParametersInfoW(SPI_GETWORKAREA,0,&work,0);
    int width=GetSystemMetrics(SM_CXSCREEN),height=GetSystemMetrics(SM_CYSCREEN);
    bool match=info&&area&&width==expected_width&&height==expected_height&&monitor.rcMonitor.left==0&&monitor.rcMonitor.top==0&&monitor.rcMonitor.right==width&&monitor.rcMonitor.bottom==height&&EqualRect(&work,&expected_work);
    ++observations;if(!match)++mismatches;
    std::printf("{\"kind\":\"callback\",\"phase\":\"%s\",\"message\":%u,\"metrics\":[%d,%d],\"monitor\":[%ld,%ld],\"work\":[%ld,%ld],\"matchesOutside\":%s}\n",phase,message,width,height,monitor.rcMonitor.right-monitor.rcMonitor.left,monitor.rcMonitor.bottom-monitor.rcMonitor.top,work.right-work.left,work.bottom-work.top,match?"true":"false");
}
static LRESULT CALLBACK procedure(HWND window,UINT message,WPARAM wp,LPARAM lp){
    switch(message){case WM_NCCREATE:case WM_CREATE:case WM_SHOWWINDOW:case WM_STYLECHANGING:case WM_STYLECHANGED:case WM_WINDOWPOSCHANGING:case WM_WINDOWPOSCHANGED:case WM_SIZE:observe(window,message);break;}
    if(message==WM_CREATE||message==WM_WINDOWPOSCHANGED){
        RECT outer{},client{};POINT origin{},mapped{};
        if(GetWindowRect(window,&outer)&&GetClientRect(window,&client)&&outer.right-outer.left==client.right&&outer.bottom-outer.top==client.bottom){
            ++coordinate_calls;BOOL screen=ClientToScreen(window,&origin);mapped={0,0};MapWindowPoints(window,nullptr,&mapped,1);
            bool match=screen&&origin.x==outer.left&&origin.y==outer.top&&mapped.x==origin.x&&mapped.y==origin.y;
            if(!ScreenToClient(window,&origin)||origin.x||origin.y)match=false;
            MapWindowPoints(nullptr,window,&mapped,1);if(mapped.x||mapped.y)match=false;
            if(!match)++coordinate_failures;
        }
    }
    if(expand&&!inside_nested&&message==WM_WINDOWPOSCHANGING){
        inside_nested=true;++nested_calls;
        ++tail_calls;if(reinterpret_cast<HWND>(SendMessageW(tail_window,WM_APP+17,0,0))!=expected_desktop)++tail_mismatches;
        HINSTANCE instance=GetModuleHandleW(nullptr);
        HWND nested=ansi?CreateWindowExA(WS_EX_NOACTIVATE,"WinedowsOwnedCallback","Nested owned fixture",WS_POPUP,0,0,100,70,nullptr,nullptr,instance,nullptr):CreateWindowExW(WS_EX_NOACTIVATE,L"WinedowsOwnedCallback",L"Nested owned fixture",WS_POPUP,0,0,100,70,nullptr,nullptr,instance,nullptr);
        if(nested){
            ShowWindow(nested,SW_SHOWNOACTIVATE);
            BOOL positioned=SetWindowPos(nested,nullptr,0,0,120,80,SWP_NOACTIVATE|SWP_NOZORDER);RECT client{};
            if(!positioned||!GetClientRect(nested,&client)||client.right!=120||client.bottom!=80)++nested_failures;
            DestroyWindow(nested);
        }else ++nested_failures;
        observe(window,WM_APP+1); // Exact native depth must survive nested APIs.
        auto pos=reinterpret_cast<WINDOWPOS*>(lp);if(!(pos->flags&SWP_NOSIZE)){pos->cx=GetSystemMetrics(SM_CXSCREEN);pos->cy=GetSystemMetrics(SM_CYSCREEN);}
        inside_nested=false;
    }
    return ansi?DefWindowProcA(window,message,wp,lp):DefWindowProcW(window,message,wp,lp);
}
int main(int argc,char **argv){
    ansi=argc>1&&!std::strcmp(argv[1],"ansi");bool visible=argc>2&&!std::strcmp(argv[2],"visible");
    expected_width=GetSystemMetrics(SM_CXSCREEN);expected_height=GetSystemMetrics(SM_CYSCREEN);
    if(!SystemParametersInfoW(SPI_GETWORKAREA,0,&expected_work,0))return 1;
    expected_desktop=GetDesktopWindow();
    HINSTANCE instance=GetModuleHandleW(nullptr);WNDCLASSW wc{};wc.lpfnWndProc=procedure;wc.hInstance=instance;wc.lpszClassName=L"WinedowsOwnedCallback";
    WNDCLASSW tc{};tc.lpfnWndProc=tail_procedure;tc.hInstance=instance;tc.lpszClassName=L"WinedowsOwnedTailCallback";
    if(!RegisterClassW(&tc)||!(tail_window=CreateWindowExW(0,tc.lpszClassName,L"",0,0,0,0,0,HWND_MESSAGE,nullptr,instance,nullptr)))return 1;
    WNDCLASSA ac{};ac.lpfnWndProc=procedure;ac.hInstance=instance;ac.lpszClassName="WinedowsOwnedCallback";
    if(!(ansi?RegisterClassA(&ac):RegisterClassW(&wc)))return 1;
    phase="create";DWORD style=WS_POPUP|(visible?WS_VISIBLE:0);
    HWND window=ansi?CreateWindowExA(WS_EX_NOACTIVATE,ac.lpszClassName,"Owned callback fixture",style,30,40,320,200,nullptr,nullptr,instance,nullptr):CreateWindowExW(WS_EX_NOACTIVATE,wc.lpszClassName,L"Owned callback fixture",style,30,40,320,200,nullptr,nullptr,instance,nullptr);
    if(!window)return 1;
    POINT before_show{};ClientToScreen(window,&before_show);
    phase="show";ShowWindow(window,SW_SHOWNOACTIVATE);
    POINT after_show{};ClientToScreen(window,&after_show);
    bool continuity=before_show.x==30&&before_show.y==40&&after_show.x==30&&after_show.y==40;
    bool mixed_mapping=true;
    for(DWORD extra:{DWORD(0),DWORD(WS_EX_LAYOUTRTL)}){
        HWND hidden=CreateWindowExW(extra|WS_EX_NOACTIVATE,L"STATIC",L"Owned mapping fixture",WS_POPUP,100,120,80,60,nullptr,nullptr,instance,nullptr);
        if(!hidden){mixed_mapping=false;continue;}
        if(!extra){
            MapWindowPoints(hidden,window,&mixed_direct,1);
            ClientToScreen(hidden,&mixed_via_screen);ScreenToClient(window,&mixed_via_screen);
        }
        POINT original[2]{{3,7},{35,29}},points[2];
        for(HWND target:{window,HWND(nullptr)}){
            std::memcpy(points,original,sizeof(points));MapWindowPoints(hidden,target,points,2);
            if(points[0].x>points[1].x)mixed_mapping=false;
            MapWindowPoints(target,hidden,points,2);if(std::memcmp(points,original,sizeof(points)))mixed_mapping=false;
        }
        DestroyWindow(hidden);
    }
    phase="style";SetWindowLongPtrW(window,GWL_STYLE,GetWindowLongPtrW(window,GWL_STYLE)|WS_CLIPCHILDREN);
    phase="position";expand=true;BOOL positioned=SetWindowPos(window,nullptr,0,0,640,360,SWP_NOACTIVATE|SWP_NOZORDER);expand=false;
    RECT client{};BOOL measured=GetClientRect(window,&client);bool size=measured&&client.right==expected_width&&client.bottom==expected_height;
    phase="destroy";DestroyWindow(window);DestroyWindow(tail_window);
    bool passed=positioned&&size&&observations&&mismatches==0&&nested_calls&&nested_failures==0&&coordinate_calls&&coordinate_failures==0&&continuity&&mixed_mapping;
    std::printf("{\"kind\":\"summary\",\"passed\":%s,\"outside\":[%d,%d],\"client\":[%ld,%ld],\"observations\":%u,\"mismatches\":%u,\"nestedCalls\":%u,\"nestedFailures\":%u,\"coordinateCalls\":%u,\"coordinateFailures\":%u,\"hiddenShownContinuity\":%s,\"mixedMappingRtlRoundtrips\":%s,\"mixedDirect\":[%ld,%ld],\"mixedViaScreen\":[%ld,%ld],\"tailCalls\":%u,\"tailMismatches\":%u,\"tailControlExcludedFromPass\":true,\"inputOrCapture\":false}\n",passed?"true":"false",expected_width,expected_height,client.right,client.bottom,observations,mismatches,nested_calls,nested_failures,coordinate_calls,coordinate_failures,continuity?"true":"false",mixed_mapping?"true":"false",mixed_direct.x,mixed_direct.y,mixed_via_screen.x,mixed_via_screen.y,tail_calls,tail_mismatches);
    return passed?0:2;
}
