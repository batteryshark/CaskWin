// SPDX-License-Identifier: LGPL-2.1-or-later
// Independent virtual-desktop joins; never changes a host display mode.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _WIN32_WINNT 0x0a00
#define WINVER 0x0a00
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <stdexcept>
using str=std::wstring;
static void check(bool ok,const char *text){printf("%s %s (error=%lu)\n",ok?"PASS":"FAIL",text,GetLastError());if(!ok)throw std::runtime_error(text);}
static str env(const wchar_t *name){wchar_t v[32768];DWORD n=GetEnvironmentVariableW(name,v,32768);return n&&n<32768?str(v,n):str();}
static str path(const str &token,const str &phase){return L"C:\\tests\\"+token+L"-"+phase;}
static void marker(const str &token,const str &phase,HWND window,HWND desktop){
    std::string text="pid="+std::to_string(GetCurrentProcessId())+"\nwindow="+std::to_string((ULONG_PTR)window)+"\ndesktop="+std::to_string((ULONG_PTR)desktop)+"\n";
    HANDLE h=CreateFileW(path(token,phase).c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_ALWAYS,0,nullptr);check(h!=INVALID_HANDLE_VALUE,"create own window marker");
    DWORD n=0;BOOL ok=WriteFile(h,text.data(),(DWORD)text.size(),&n,nullptr);CloseHandle(h);check(ok&&n==text.size(),"publish window marker");
}
static void metrics(int width,int height){
    DEVMODEW mode{};mode.dmSize=sizeof(mode);
    check(GetSystemMetrics(SM_CXSCREEN)==width&&GetSystemMetrics(SM_CYSCREEN)==height,"logical screen metrics match live mode");
    check(EnumDisplaySettingsW(nullptr,ENUM_CURRENT_SETTINGS,&mode)&&mode.dmPelsWidth==(DWORD)width&&mode.dmPelsHeight==(DWORD)height,"current display mode matches live session");
}
static LRESULT CALLBACK procedure(HWND window,UINT message,WPARAM w,LPARAM l){return DefWindowProcW(window,message,w,l);}
int wmain(int argc,wchar_t **argv){
    setvbuf(stdout,nullptr,_IONBF,0);
    try{
        if(argc==2&&str(argv[1])==L"--host-mode"){
            DEVMODEW mode{};mode.dmSize=sizeof(mode);check(EnumDisplaySettingsW(nullptr,ENUM_CURRENT_SETTINGS,&mode)!=FALSE,"query host mode without changing it");
            printf("HOST %lu %lu %lu %lu %ld %ld\n",mode.dmPelsWidth,mode.dmPelsHeight,mode.dmBitsPerPel,mode.dmDisplayFrequency,mode.dmPosition.x,mode.dmPosition.y);return 0;
        }
        if(argc==4&&str(argv[1])==L"--host"){
            HWND window=(HWND)(ULONG_PTR)_wcstoui64(argv[2],nullptr,10),desktop=(HWND)(ULONG_PTR)_wcstoui64(argv[3],nullptr,10);
            check(!GetModuleHandleW(L"winedows32.dll")&&!GetModuleHandleW(L"winedows64.dll"),"physical ancestry observer is unhooked");
            check(SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)!=nullptr,"native observer queries physical pixels without DPI virtualization");
            check(IsWindow(window)&&IsWindow(desktop)&&window!=desktop,"owned application and desktop windows exist");
            check(GetAncestor(window,GA_PARENT)==desktop&&IsChild(desktop,window),"native USER32 confirms actual shared-container parent");
            RECT r{};check(GetClientRect(desktop,&r)&&r.right-r.left==800&&r.bottom-r.top==600,"physical desktop has current 800x600 client size");return 0;
        }
        check(argc==5,"expected role token width height");str role=argv[1],token=argv[2];int width=_wtoi(argv[3]),height=_wtoi(argv[4]);
        check(token.size()==32&&token.find_first_not_of(L"0123456789abcdef")==str::npos,"safe window fixture token");
#ifdef _WIN64
        const wchar_t *shim=L"winedows64.dll";
#else
        const wchar_t *shim=L"winedows32.dll";
#endif
        str host=env(L"WINEDOWS_DESKTOP_HWND");HWND desktop=(HWND)(ULONG_PTR)_wcstoui64(host.c_str(),nullptr,10);
        check(GetModuleHandleW(shim)&&!env(L"WINEDOWS_PREFIX").empty()&&desktop&&GetDesktopWindow()==desktop,"injected virtual desktop required before mode operations");
        metrics(width,height);
        WNDCLASSW wc{};wc.lpfnWndProc=procedure;wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=L"CaskWinSessionWindowProbe";wc.hbrBackground=(HBRUSH)(COLOR_WINDOW+1);
        check(RegisterClassW(&wc)!=0,"register owned fixture window");
        HWND window=CreateWindowExW(0,wc.lpszClassName,L"CaskWin session fixture",WS_OVERLAPPEDWINDOW|WS_VISIBLE,30,30,250,180,nullptr,nullptr,wc.hInstance,nullptr);
        check(window!=nullptr,"create own contained application window");marker(token,role+L".ready",window,desktop);
        bool resized=false;ULONGLONG deadline=GetTickCount64()+45000;
        while(GetFileAttributesW(path(token,role+L".stop").c_str())==INVALID_FILE_ATTRIBUTES){
            MSG message{};while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}
            if(role==L"owner"&&!resized&&GetFileAttributesW(path(token,L"owner.resize").c_str())!=INVALID_FILE_ATTRIBUTES){
                DEVMODEW mode{};mode.dmSize=sizeof(mode);mode.dmFields=DM_PELSWIDTH|DM_PELSHEIGHT|DM_BITSPERPEL;mode.dmPelsWidth=800;mode.dmPelsHeight=600;mode.dmBitsPerPel=32;
                check(ChangeDisplaySettingsExW(nullptr,&mode,nullptr,CDS_FULLSCREEN,nullptr)==DISP_CHANGE_SUCCESSFUL,"change only virtual session mode to800x600");metrics(800,600);resized=true;marker(token,L"owner.resized",window,desktop);
            }
            if(GetTickCount64()>=deadline)throw std::runtime_error("window fixture stop deadline");Sleep(10);
        }
        check(DestroyWindow(window)!=FALSE,"destroy own fixture window");return role==L"owner"?31:43;
    }catch(const std::exception &e){fprintf(stderr,"session-window-probe: %s\n",e.what());return 100;}
}
