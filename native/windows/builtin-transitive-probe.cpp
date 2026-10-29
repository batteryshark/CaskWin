// SPDX-License-Identifier: LGPL-2.1-or-later
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>
#include <cstdio>
#include <cstring>
#include <vector>
#ifdef WINEDOWS_TRANSITIVE_DLL
extern "C" __declspec(dllexport) FARPROC WINAPI TransitiveGdiplusProvider(){
    // Keep a real static import and read its resolved IAT, rather than a local
    // import trampoline or a GetModuleHandle result modified by provider policy.
    HMODULE module=nullptr;GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,(LPCWSTR)&TransitiveGdiplusProvider,&module);
    auto base=(BYTE*)module;auto dos=(IMAGE_DOS_HEADER*)base;auto nt=(IMAGE_NT_HEADERS*)(base+dos->e_lfanew);auto directory=nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    for(auto d=(IMAGE_IMPORT_DESCRIPTOR*)(base+directory.VirtualAddress);d->Name;d++)if(!_stricmp((char*)base+d->Name,"gdiplus.dll")){
        auto names=(IMAGE_THUNK_DATA*)(base+d->OriginalFirstThunk),values=(IMAGE_THUNK_DATA*)(base+d->FirstThunk);
        for(;names->u1.AddressOfData;names++,values++)if(!IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)&&!strcmp((char*)((IMAGE_IMPORT_BY_NAME*)(base+names->u1.AddressOfData))->Name,"GdiplusStartup"))return (FARPROC)values->u1.Function;
    }
    return nullptr;
}
extern "C" __declspec(dllexport) Gdiplus::Status WINAPI TransitiveStart(ULONG_PTR *token,Gdiplus::GdiplusStartupInput *input){return Gdiplus::GdiplusStartup(token,input,nullptr);}
#else
extern "C" FARPROC WINAPI TransitiveGdiplusProvider();
static bool same_file(const wchar_t *a,const wchar_t *b){HANDLE x=CreateFileW(a,FILE_READ_ATTRIBUTES,7,nullptr,OPEN_EXISTING,0,nullptr),y=CreateFileW(b,FILE_READ_ATTRIBUTES,7,nullptr,OPEN_EXISTING,0,nullptr);BY_HANDLE_FILE_INFORMATION p{},q{};bool ok=x!=INVALID_HANDLE_VALUE&&y!=INVALID_HANDLE_VALUE&&GetFileInformationByHandle(x,&p)&&GetFileInformationByHandle(y,&q)&&p.dwVolumeSerialNumber==q.dwVolumeSerialNumber&&p.nFileIndexHigh==q.nFileIndexHigh&&p.nFileIndexLow==q.nFileIndexLow;if(x!=INVALID_HANDLE_VALUE)CloseHandle(x);if(y!=INVALID_HANDLE_VALUE)CloseHandle(y);return ok;}
int wmain(int argc,wchar_t **argv){
    if(argc<2)return 2;HMODULE module=nullptr;FARPROC address=TransitiveGdiplusProvider();if(!address||!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,(LPCWSTR)address,&module))return 1;
    std::vector<wchar_t> path(32768);DWORD count=GetModuleFileNameW(module,path.data(),(DWORD)path.size());if(!count||count>=path.size())return 1;wprintf(L"TRANSITIVE_PROVIDER %ls\n",path.data());
    bool ok=same_file(path.data(),argv[1]);printf("%s transitive actual IAT provider\n",ok?"PASS":"FAIL");return ok?0:1;
}
#endif
