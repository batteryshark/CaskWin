// SPDX-License-Identifier: LGPL-2.1-or-later
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#if defined(WINEDOWS_CONTEXT_A)
extern "C" __declspec(dllimport) int WINAPI BMarker();
extern "C" __declspec(dllexport) int WINAPI AMarker(){return BMarker();}
extern "C" __declspec(dllexport) FARPROC WINAPI ATarget(){return reinterpret_cast<FARPROC>(&BMarker);}
#elif defined(WINEDOWS_CONTEXT_B)
extern "C" __declspec(dllexport) int WINAPI BMarker(){return 0x13572468;}
#elif defined(WINEDOWS_CONTEXT_SHADOW)
struct StartupInput{UINT version=1;void *callback=nullptr;BOOL no_background=FALSE,no_codecs=FALSE;};
extern "C" __declspec(dllimport) int WINAPI GdiplusStartup(ULONG_PTR*,const StartupInput*,void*);
extern "C" __declspec(dllimport) void WINAPI GdiplusShutdown(ULONG_PTR);
extern "C" __declspec(dllexport) int WINAPI BMarker(){ULONG_PTR token=0;StartupInput input;int status=GdiplusStartup(&token,&input,nullptr);if(!status)GdiplusShutdown(token);return 0x0bad0000+status;}
#else
extern "C" __declspec(dllimport) int WINAPI AMarker();
extern "C" __declspec(dllimport) FARPROC WINAPI ATarget();
static bool same_file(const wchar_t *a,const wchar_t *b){HANDLE x=CreateFileW(a,FILE_READ_ATTRIBUTES,7,nullptr,OPEN_EXISTING,0,nullptr),y=CreateFileW(b,FILE_READ_ATTRIBUTES,7,nullptr,OPEN_EXISTING,0,nullptr);BY_HANDLE_FILE_INFORMATION p{},q{};bool ok=x!=INVALID_HANDLE_VALUE&&y!=INVALID_HANDLE_VALUE&&GetFileInformationByHandle(x,&p)&&GetFileInformationByHandle(y,&q)&&p.dwVolumeSerialNumber==q.dwVolumeSerialNumber&&p.nFileIndexHigh==q.nFileIndexHigh&&p.nFileIndexLow==q.nFileIndexLow;if(x!=INVALID_HANDLE_VALUE)CloseHandle(x);if(y!=INVALID_HANDLE_VALUE)CloseHandle(y);return ok;}
int wmain(int argc,wchar_t **argv){
    if(argc!=2)return 2;SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);int value=AMarker();HMODULE owner=nullptr;
    if(value!=0x13572468||!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,(LPCWSTR)ATarget(),&owner)){printf("FAIL DLL-owned manifest marker value=%08x\n",value);return 1;}
    wchar_t path[32768]{};if(!GetModuleFileNameW(owner,path,32768))return 1;wprintf(L"DLL_CONTEXT_PROVIDER %ls\n",path);
    bool ok=same_file(path,argv[1]);printf("%s DLL-owned manifest selects the real dependency, not its app-local shadow\n",ok?"PASS":"FAIL");return ok?0:1;
}
#endif
