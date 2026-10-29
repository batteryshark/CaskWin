// SPDX-License-Identifier: LGPL-2.1-or-later
#include "startup-native.h"
#include <cstdint>
#include <cstring>
#include <limits>

namespace wd {
namespace {
constexpr uint64_t maximum_parameters=16*1024*1024,maximum_environment=4*1024*1024;
struct RemoteString32 {USHORT length,maximum;uint32_t buffer;};
struct RemoteString64 {USHORT length,maximum;uint32_t padding;uint64_t buffer;};
struct BasicInformation64 {uint32_t exit_status,padding;uint64_t peb,affinity;uint32_t priority,padding2;uint64_t pid,parent;};
static_assert(sizeof(RemoteString32)==8&&sizeof(RemoteString64)==16&&sizeof(BasicInformation64)==48);
using QueryProcess=NTSTATUS(NTAPI*)(HANDLE,ULONG,PVOID,ULONG,PULONG);
using ReadMemory64=NTSTATUS(NTAPI*)(HANDLE,uint64_t,PVOID,uint64_t,uint64_t*);
using WriteMemory64=NTSTATUS(NTAPI*)(HANDLE,uint64_t,const void*,uint64_t,uint64_t*);

[[noreturn]] void fail(const char *message){throw std::runtime_error(std::string("Native startup DLL path: ")+message);}
uint64_t add(uint64_t base,uint64_t size){if(size>UINT64_MAX-base)fail("remote address overflow");return base+size;}

class RemoteProcess {
    HANDLE process;
    bool target32,wide_access;
    QueryProcess query,query64=nullptr;
    ReadMemory64 read64=nullptr;
    WriteMemory64 write64=nullptr;
public:
    RemoteProcess(HANDLE handle,WORD machine):process(handle),target32(machine==IMAGE_FILE_MACHINE_I386),
        wide_access(sizeof(void*)==4&&!target32){
        if(machine!=IMAGE_FILE_MACHINE_I386&&machine!=IMAGE_FILE_MACHINE_AMD64)fail("unsupported architecture");
        HMODULE ntdll=GetModuleHandleW(L"ntdll.dll");
        query=(QueryProcess)GetProcAddress(ntdll,"NtQueryInformationProcess");
        if(!query)fail("NtQueryInformationProcess unavailable");
        if(wide_access){
            query64=(QueryProcess)GetProcAddress(ntdll,"NtWow64QueryInformationProcess64");
            read64=(ReadMemory64)GetProcAddress(ntdll,"NtWow64ReadVirtualMemory64");
            write64=(WriteMemory64)GetProcAddress(ntdll,"NtWow64WriteVirtualMemory64");
            if(!query64||!read64||!write64)fail("cross-architecture process access unavailable");
        }
        using MachineQuery=BOOL(WINAPI*)(HANDLE,USHORT*,USHORT*);
        auto machines=(MachineQuery)GetProcAddress(GetModuleHandleW(L"kernel32.dll"),"IsWow64Process2");
        if(machines){
            USHORT guest=0,host=0;if(!machines(process,&guest,&host))fail("cannot query child architecture");
            WORD actual=guest?guest:host;if(actual!=machine)fail("child architecture differs from selected executable");
        }
    }
    size_t read_some(uint64_t address,void *data,size_t size) const {
        add(address,size);
        if(wide_access){uint64_t received=0;read64(process,address,data,size,&received);return received<=size?(size_t)received:0;}
        if(address>UINTPTR_MAX||size>UINTPTR_MAX-address)fail("remote address does not fit caller");
        SIZE_T received=0;ReadProcessMemory(process,(void*)(uintptr_t)address,data,size,&received);return received;
    }
    void read(uint64_t address,void *data,size_t size) const {if(read_some(address,data,size)!=size)fail("cannot read suspended child parameters");}
    template<class T>T value(uint64_t address) const {T result{};read(address,&result,sizeof(result));return result;}
    uint64_t pointer(uint64_t address) const {return target32?value<uint32_t>(address):value<uint64_t>(address);}
    void write(uint64_t address,const void *data,size_t size) const {
        add(address,size);
        if(wide_access){uint64_t written=0;NTSTATUS status=write64(process,address,data,size,&written);if(status<0||written!=size)fail("cannot write suspended child parameters");}
        else{
            if(address>UINTPTR_MAX||size>UINTPTR_MAX-address)fail("remote write does not fit caller");
            SIZE_T written=0;if(!WriteProcessMemory(process,(void*)(uintptr_t)address,data,size,&written)||written!=size)fail("cannot write suspended child parameters");
        }
    }
    uint64_t peb() const {
        if(wide_access){BasicInformation64 info{};if(query64(process,0,&info,sizeof(info),nullptr)<0||!info.peb)fail("cannot find native 64-bit PEB");return info.peb;}
        if(target32){ULONG_PTR wow=0;if(query(process,26,&wow,sizeof(wow),nullptr)>=0&&wow)return wow;}
        PROCESS_BASIC_INFORMATION info{};if(query(process,0,&info,sizeof(info),nullptr)<0||!info.PebBaseAddress)fail("cannot find child PEB");
        return (uintptr_t)info.PebBaseAddress;
    }
};

std::vector<wchar_t> read_environment(const RemoteProcess &remote,uint64_t address){
    std::vector<wchar_t> result;
    while(result.size()*sizeof(wchar_t)<maximum_environment){
        wchar_t chunk[512]{};
        size_t received=remote.read_some(add(address,result.size()*sizeof(wchar_t)),chunk,sizeof(chunk));
        if(!received||received%sizeof(wchar_t))fail("invalid environment block");
        for(size_t i=0;i<received/sizeof(wchar_t);i++){
            bool end=!chunk[i]&&!result.empty()&&!result.back();result.push_back(chunk[i]);
            if(end)return result;
        }
    }
    fail("environment exceeds bounded startup scan");
}
}

StartupNativeResult configure_native_startup(HANDLE process,WORD machine,const str &directory,const str &other_arch_directory){
    if(directory.empty()||directory.size()>32766||directory.find(L';')!=str::npos)fail("invalid private system directory");
    RemoteProcess remote(process,machine);bool target32=machine==IMAGE_FILE_MACHINE_I386;
    uint64_t peb=remote.peb(),ldr=remote.pointer(add(peb,target32?0x0c:0x18));
    if(ldr&&remote.value<BYTE>(add(ldr,4)))fail("process loader is already initialized");
    uint64_t parameters=remote.pointer(add(peb,target32?0x10:0x20));
    if(!parameters)fail("missing process parameters");
    uint32_t header[4]{};remote.read(parameters,header,sizeof(header));
    if(!(header[2]&1)||header[1]<(target32?0x4c:0x88)||header[0]<header[1]||header[0]>maximum_parameters)
        fail("unsupported initial process parameter layout");
    uint64_t parameter_end=add(parameters,header[0]);
    uint64_t descriptor=add(parameters,target32?0x30:0x50);
    RemoteString32 dll32{};RemoteString64 dll64{};
    if(target32)remote.read(descriptor,&dll32,sizeof(dll32));else remote.read(descriptor,&dll64,sizeof(dll64));
    USHORT old_length=target32?dll32.length:dll64.length,old_maximum=target32?dll32.maximum:dll64.maximum;
    uint64_t old_buffer=target32?dll32.buffer:dll64.buffer;
    if(old_length%sizeof(wchar_t)||old_length>old_maximum)fail("invalid inherited DLL directory");
    if(old_length){
        if(old_buffer<parameters||add(old_buffer,old_length)>parameter_end)fail("inherited directory lies outside initial parameters");
        str inherited(old_length/sizeof(wchar_t),0);remote.read(old_buffer,inherited.data(),old_length);
        // An application's SetDllDirectory choice retains native precedence.
        // A directory installed by this prefix may change with child bitness.
        if(_wcsicmp(inherited.c_str(),directory.c_str())&&_wcsicmp(inherited.c_str(),other_arch_directory.c_str()))
            return StartupNativeResult::inherited_directory_preserved;
    }
    uint64_t environment=remote.pointer(add(parameters,target32?0x48:0x80));
    // Windows starts with environment inside the WOW64 parameter block, or
    // immediately after the native block. Reject other layouts; never assume
    // that arbitrary nearby committed memory is unused parameter storage.
    if(environment<add(parameters,target32?0x4c:0x88)||environment>parameter_end)
        fail("environment is not contiguous with initial parameters");
    auto block=read_environment(remote,environment);
    uint64_t environment_end=add(environment,block.size()*sizeof(wchar_t));
    if(environment<parameter_end&&environment_end>parameter_end)fail("embedded environment exceeds initial parameter bounds");
    uint64_t value_address=0;str key=str(startup_native_environment_name)+L"=";
    for(size_t offset=0;offset+1<block.size();){
        size_t end=offset;while(end<block.size()&&block[end])++end;
        if(end==block.size())fail("unterminated environment entry");
        if(end-offset>=key.size()&&!_wcsnicmp(block.data()+offset,key.c_str(),key.size())){
            if(value_address)fail("duplicate private directory environment value");
            str value(block.data()+offset+key.size(),end-offset-key.size());
            if(value!=directory)fail("child directory environment value does not match selection");
            value_address=add(environment,(offset+key.size())*sizeof(wchar_t));
        }
        offset=end+1;
    }
    if(!value_address)fail("private directory environment value missing");
    uint64_t extent=std::max(parameter_end,environment_end)-parameters;
    if(extent>maximum_parameters||extent>UINT32_MAX)fail("extended parameter block is too large");
    USHORT length=(USHORT)(directory.size()*sizeof(wchar_t)),maximum=length+sizeof(wchar_t);
    if(target32){
        if(value_address>UINT32_MAX)fail("WOW64 directory address exceeds 32 bits");
        dll32={length,maximum,(uint32_t)value_address};
    }else dll64={length,maximum,0,value_address};
    // DLL initialization relocates the initial parameter block. Keep DllPath
    // backed by bytes in that block, without changing the environment itself.
    // The caller must terminate the suspended child if either write fails.
    uint32_t lengths[2]={(uint32_t)extent,(uint32_t)extent};
    remote.write(parameters,lengths,sizeof(lengths));
    if(target32)remote.write(descriptor,&dll32,sizeof(dll32));else remote.write(descriptor,&dll64,sizeof(dll64));
    return StartupNativeResult::configured;
}
}
