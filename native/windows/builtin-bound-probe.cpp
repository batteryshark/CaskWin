// SPDX-License-Identifier: LGPL-2.1-or-later
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
struct StartupInput {UINT version=1;void *callback=nullptr;BOOL no_background=FALSE,no_codecs=FALSE;};
extern "C" __declspec(dllimport) int WINAPI GdiplusStartup(ULONG_PTR*,const StartupInput*,void*);
extern "C" __declspec(dllimport) void WINAPI GdiplusShutdown(ULONG_PTR);
static bool same_file(const wchar_t *a,const wchar_t *b){HANDLE x=CreateFileW(a,FILE_READ_ATTRIBUTES,7,nullptr,OPEN_EXISTING,0,nullptr),y=CreateFileW(b,FILE_READ_ATTRIBUTES,7,nullptr,OPEN_EXISTING,0,nullptr);BY_HANDLE_FILE_INFORMATION p{},q{};bool ok=x!=INVALID_HANDLE_VALUE&&y!=INVALID_HANDLE_VALUE&&GetFileInformationByHandle(x,&p)&&GetFileInformationByHandle(y,&q)&&p.dwVolumeSerialNumber==q.dwVolumeSerialNumber&&p.nFileIndexHigh==q.nFileIndexHigh&&p.nFileIndexLow==q.nFileIndexLow;if(x!=INVALID_HANDLE_VALUE)CloseHandle(x);if(y!=INVALID_HANDLE_VALUE)CloseHandle(y);return ok;}
int wmain(int argc,wchar_t **argv){
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);if(argc!=2)return 2;
    HMODULE owner=nullptr;if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,(LPCWSTR)&GdiplusStartup,&owner))return 1;
    wchar_t path[32768]{};if(!GetModuleFileNameW(owner,path,32768))return 1;wprintf(L"Provider=%ls\n",path);
    DWORD ignored=0;printf("VERSION_QUERY_SIZE %lu\n",GetFileVersionInfoSizeW(path,&ignored));
    wchar_t version_path[32768]{};GetModuleFileNameW(GetModuleHandleW(L"version.dll"),version_path,32768);wprintf(L"VERSION_MODULE %ls\n",version_path);
    if(!same_file(path,argv[1])){puts("FAIL actual bound-import provider identity");return 1;}
    ULONG_PTR token=0;StartupInput input;if(GdiplusStartup(&token,&input,nullptr)||!token){puts("FAIL actual imported GDI+ startup");return 1;}GdiplusShutdown(token);
    puts("PASS actual bound-import IAT and GDI+ startup");return 0;
}
