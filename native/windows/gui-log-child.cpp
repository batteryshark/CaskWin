// SPDX-License-Identifier: LGPL-2.1-or-later
// GUI-subsystem diagnostic fixture. It creates no window and requests no input.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <cstdio>
#include <cstring>
#include <string>
struct StartupInput{UINT version=1;void *callback=nullptr;BOOL no_background=FALSE,no_codecs=FALSE;};
int WINAPI wWinMain(HINSTANCE,HINSTANCE,wchar_t*,int){
    int argc=0;wchar_t **argv=CommandLineToArgvW(GetCommandLineW(),&argc);if(!argv)return 2;
    fprintf(stdout,"GUI_STDOUT_MARKER\n");fprintf(stderr,"GUI_STDERR_CRT_MARKER\n");fflush(stdout);fflush(stderr);
    const char raw[]="GUI_STDERR_WRITEFILE_MARKER\n";DWORD written=0;BOOL valid=WriteFile(GetStdHandle(STD_ERROR_HANDLE),raw,sizeof(raw)-1,&written,nullptr);
    fprintf(stderr,"STD_HANDLES input=%u stdout=%u stderr=%u rawWrite=%u\n",GetFileType(GetStdHandle(STD_INPUT_HANDLE)),GetFileType(GetStdHandle(STD_OUTPUT_HANDLE)),GetFileType(GetStdHandle(STD_ERROR_HANDLE)),valid!=FALSE);
    DWORD exit_code=0;
    for(int i=1;i<argc;i++){
        if(!wcscmp(argv[i],L"--exit")&&i+1<argc)exit_code=wcstoul(argv[++i],nullptr,10);
        else if(!wcscmp(argv[i],L"--flood")){char data[32768];memset(data,'X',sizeof(data));for(int n=0;n<544;n++){DWORD done=0;if(!WriteFile(GetStdHandle(STD_ERROR_HANDLE),data,sizeof(data),&done,nullptr)||done!=sizeof(data)){LocalFree(argv);return 3;}}fprintf(stderr,"\nGUI_FLOOD_FINISHED\n");}
        else if(!wcscmp(argv[i],L"--wine-gdiplus")&&i+1<argc){HMODULE module=LoadLibraryExW(argv[++i],nullptr,LOAD_WITH_ALTERED_SEARCH_PATH);auto start=module?(int(WINAPI*)(ULONG_PTR*,const StartupInput*,void*))GetProcAddress(module,"GdiplusStartup"):nullptr;auto stop=module?(void(WINAPI*)(ULONG_PTR))GetProcAddress(module,"GdiplusShutdown"):nullptr;ULONG_PTR token=0;StartupInput input;int result=start?start(&token,&input,nullptr):-1;fprintf(stderr,"GUI_WINE_GDIPLUS status=%d token=%u\n",result,token!=0);if(stop&&token)stop(token);if(module)FreeLibrary(module);if(result)exit_code=4;}
        else if(!wcscmp(argv[i],L"--arg")&&i+1<argc){std::wstring expected=L"space and quote \" plus trailing\\";if(expected!=argv[++i])exit_code=5;else fprintf(stderr,"GUI_ARGUMENT_QUOTING_OK\n");}
    }
    fflush(stdout);fflush(stderr);LocalFree(argv);return (int)exit_code;
}
