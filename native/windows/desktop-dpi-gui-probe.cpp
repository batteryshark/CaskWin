// SPDX-License-Identifier: LGPL-2.1-or-later
// True GUI-subsystem fixture: deliberately has no stderr handle and reports
// only its own window geometry and Win32 error results to the requested file.
#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0a00
#include <windows.h>
#include <shellapi.h>
#include <cstdio>
#include <cwchar>
static void pump(){DWORD end=GetTickCount()+50;do{MSG msg;while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)){TranslateMessage(&msg);DispatchMessageW(&msg);}Sleep(1);}while(LONG(end-GetTickCount())>0);}
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,PWSTR,int){
    int argc;LPWSTR *argv=CommandLineToArgvW(GetCommandLineW(),&argc);if(!argv||argc!=2){if(argv)LocalFree(argv);return 1;}
    SetStdHandle(STD_ERROR_HANDLE,INVALID_HANDLE_VALUE);
    HWND window=CreateWindowExW(WS_EX_NOACTIVATE,L"STATIC",L"Owned GUI trace fixture",WS_POPUP,0,0,320,200,nullptr,nullptr,instance,nullptr);
    if(!window){LocalFree(argv);return 1;}
    ShowWindow(window,SW_SHOWNOACTIVATE);pump();
    RECT client{};BOOL measured=GetClientRect(window,&client);
    SetLastError(0x12345678);BOOL invalid=SetWindowPos(nullptr,nullptr,0,0,0,0,SWP_NOACTIVATE|SWP_NOZORDER);DWORD invalid_error=GetLastError();
    SetLastError(0x12345678);BOOL valid=SetWindowPos(window,nullptr,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE|SWP_NOZORDER);DWORD valid_error=GetLastError();
    bool moves=true;for(unsigned i=0;i<100;++i)moves=SetWindowPos(window,nullptr,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE|SWP_NOZORDER)&&moves;
    DestroyWindow(window);pump();
    bool passed=measured&&client.right==320&&client.bottom==200&&!invalid&&invalid_error==ERROR_INVALID_WINDOW_HANDLE&&valid&&moves;
    char json[512];int length=snprintf(json,sizeof(json),"{\"passed\":%s,\"pid\":%lu,\"guiSubsystem\":true,\"stderrInvalid\":true,\"client\":[%ld,%ld],\"invalidResult\":%d,\"invalidError\":%lu,\"validResult\":%d,\"validError\":%lu,\"positionCalls\":102,\"inputOrCapture\":false}\n",passed?"true":"false",GetCurrentProcessId(),client.right,client.bottom,invalid,invalid_error,valid,valid_error);
    HANDLE file=CreateFileW(argv[1],GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);LocalFree(argv);
    if(file==INVALID_HANDLE_VALUE)return 1;
    DWORD written;BOOL saved=WriteFile(file,json,DWORD(length),&written,nullptr);CloseHandle(file);return passed&&saved&&written==DWORD(length)?0:1;
}
