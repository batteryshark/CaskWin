// SPDX-License-Identifier: LGPL-2.1-or-later
// Writes only a supplied application hive or the loader's private HKCU.
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>

using ModuleSet=LSTATUS(__cdecl*)(HKEY,LPCSTR,DWORD,const BYTE*,DWORD);
static ModuleSet module_set;
static void record(HKEY key,const char *name,DWORD type,const BYTE *data,DWORD size,int backend=0) {
    std::wstring name_w(name,name+strlen(name));
    LSTATUS status=backend==1?RegSetValueExW(key,name_w.c_str(),0,type,data,size):
        backend==2?RegSetKeyValueA(key,nullptr,name,type,data,size):
        backend==3?module_set(key,name,type,data,size):RegSetValueExA(key,name,0,type,data,size);
    BYTE stored[256]{};DWORD actual_type=0,actual_size=sizeof(stored);
    LSTATUS query=RegQueryValueExW(key,name_w.c_str(),nullptr,&actual_type,stored,&actual_size);
    printf("case=%s status=%ld query=%ld type=%lu size=%lu hex=",name,status,query,actual_type,actual_size);
    if(query==ERROR_SUCCESS)for(DWORD i=0;i<actual_size;++i)printf("%02x",stored[i]);
    puts("");
}
int main(int argc,char **argv) {
    HKEY root=HKEY_CURRENT_USER,key=nullptr;bool native=argc==3&&!strcmp(argv[1],"--native");
    bool prefixed=argc==2&&!strcmp(argv[1],"--prefix");
    if(!native&&(!prefixed||(!GetModuleHandleA("winedows32.dll")&&!GetModuleHandleA("winedows64.dll")))) {
        fprintf(stderr,"Use --native PRIVATE_HIVE, or --prefix through Winedows; refusing host HKCU.\n");return 3;
    }
    char module_name[80];snprintf(module_name,sizeof(module_name),"legacy-registry-module%zu.dll",sizeof(void*)*8);
    HMODULE module=LoadLibraryA(module_name);
    if(!module||(module_set=(ModuleSet)(void*)GetProcAddress(module,"LegacySetValue"))==nullptr){fprintf(stderr,"Fixture module missing\n");return 4;}
    if(native){LSTATUS s=RegLoadAppKeyA(argv[2],&root,KEY_ALL_ACCESS,0,0);if(s){printf("load_error=%ld\n",s);return 1;}}
    const char *path=native?"User\\Software\\WinedowsLegacyRegistryFixture":"Software\\WinedowsLegacyRegistryFixture";
    LSTATUS status=RegCreateKeyExA(root,path,0,nullptr,0,KEY_ALL_ACCESS,nullptr,&key,nullptr);
    if(status){printf("create_error=%ld\n",status);return 2;}
    const char value[]="Space Cadet";const BYTE *bytes=(const BYTE*)value;
    record(key,"short_one",REG_SZ,bytes,(DWORD)strlen(value+1));
    record(key,"short_many",REG_SZ,bytes,2);
    record(key,"zero",REG_SZ,bytes,0);
    record(key,"without_nul",REG_SZ,bytes,sizeof(value)-1);
    record(key,"complete",REG_SZ,bytes,sizeof(value));
    record(key,"empty",REG_SZ,(const BYTE*)"",0);
    record(key,"null",REG_SZ,nullptr,0);
    char oversized[32];memset(oversized,'Q',sizeof(oversized));memcpy(oversized,value,sizeof(value));
    record(key,"oversized",REG_SZ,(const BYTE*)oversized,18);
    record(key,"expand_short",REG_EXPAND_SZ,bytes,sizeof(value)-2);
    record(key,"binary_short",REG_BINARY,bytes,sizeof(value)-2);
    record(key,"host_boundary",REG_SZ,bytes,sizeof(value)-2,2);
    record(key,"module_boundary",REG_SZ,bytes,sizeof(value)-2,3);
    const wchar_t wide[]=L"Space Cadet";
    record(key,"wide_short",REG_SZ,(const BYTE*)wide,sizeof(wide)-4,1);
    RegCloseKey(key);if(native)RegCloseKey(root);FreeLibrary(module);return 0;
}
