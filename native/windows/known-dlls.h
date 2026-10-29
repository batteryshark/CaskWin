// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once
#include "common.h"
#include <cwctype>
namespace wd {
// Shared by override validation and the bounded startup dependency walker.
// Enumerate the target architecture's real object namespace, never infer this
// boundary from a filesystem basename or a broad Windows-directory allowlist.
inline std::vector<str> module_known_dlls(WORD machine) {
    using Open=NTSTATUS(NTAPI*)(PHANDLE,ACCESS_MASK,POBJECT_ATTRIBUTES);
    using Query=NTSTATUS(NTAPI*)(HANDLE,PVOID,ULONG,BOOLEAN,BOOLEAN,PULONG,PULONG);
    HMODULE ntdll=GetModuleHandleW(L"ntdll.dll");auto open=(Open)GetProcAddress(ntdll,"NtOpenDirectoryObject");auto query=(Query)GetProcAddress(ntdll,"NtQueryDirectoryObject");
    if(!open||!query)throw std::runtime_error("Cannot inspect KnownDLLs for disabled DLL overrides");
    SYSTEM_INFO system{};GetNativeSystemInfo(&system);
    str path=machine==IMAGE_FILE_MACHINE_I386&&system.wProcessorArchitecture==PROCESSOR_ARCHITECTURE_AMD64?L"\\KnownDlls32":L"\\KnownDlls";
    UNICODE_STRING name{(USHORT)(path.size()*2),(USHORT)(path.size()*2),path.data()};OBJECT_ATTRIBUTES attr{};attr.Length=sizeof(attr);attr.ObjectName=&name;attr.Attributes=OBJ_CASE_INSENSITIVE;
    HANDLE directory=nullptr;if(open(&directory,1,&attr)<0)throw std::runtime_error("Cannot open target KnownDLL directory for disabled overrides");
    std::vector<str> result;ULONG context=0;
    try {
        for(unsigned index=0;index<512;index++){
            alignas(void*) BYTE bytes[4096]{};ULONG used=0;NTSTATUS status=query(directory,bytes,sizeof(bytes),TRUE,index==0,&context,&used);
            if(status==(NTSTATUS)0x8000001a){CloseHandle(directory);return result;}
            if(status<0)throw std::runtime_error("Cannot enumerate target KnownDLL directory for disabled overrides");
            auto entry=(UNICODE_STRING*)bytes;
            auto text=[&](const UNICODE_STRING &s){uintptr_t p=(uintptr_t)s.Buffer,base=(uintptr_t)bytes;if(s.Length%2||p<base||p>base+sizeof(bytes)||s.Length>base+sizeof(bytes)-p)throw std::runtime_error("Invalid KnownDLL directory entry");str value(s.Buffer,s.Length/2);for(auto &c:value)c=(wchar_t)towlower(c);return value;};
            if(text(entry[1])==L"section")result.push_back(text(entry[0]));
        }
        throw std::runtime_error("KnownDLL directory exceeds disabled-override inspection bound");
    }catch(...){CloseHandle(directory);throw;}
}
}
