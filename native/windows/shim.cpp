// SPDX-License-Identifier: LGPL-2.1-or-later
// Native host backend. Hooks are process-local and installed before the application entry point.
#include "common.h"
#include "desktop.h"
#include "audio-backend.h"
#include "graphics-backend.h"
#include "module-runtime.h"
#include "modules.h"
#include "pe-architecture.h"
#include "startup-native.h"
#include "startup-imports.h"
#include "cdda-media.h"
#include "media-state.h"
#include "named-objects.h"
#include "registry-views.h"
#include "detours.h"
#include <sddl.h>
#include <shlobj.h>
#include <winioctl.h>
#include <ntddcdrm.h>
#include <memory>
using namespace wd;
static Prefix *configured_prefix;
static std::unique_ptr<MediaSession> media_session;
static thread_local std::shared_ptr<const Prefix> routing_view;
struct PrefixAccess {
    const Prefix *operator->() const;
    const Prefix &operator*() const {return *operator->();}
};
static PrefixAccess prefix;
static HKEY hive_root, machine_root, user_root, default_root;
static str user_sid, appdir, runtime_dir, machine_path, user_path, default_path;
static char shim_path[32768];
static bool active;
static HANDLE trace_handle=INVALID_HANDLE_VALUE;
static void trace(const char *kind,const str &name,ACCESS_MASK access,NTSTATUS status) {
    if(trace_handle==INVALID_HANDLE_VALUE)return;
    char head[120];snprintf(head,sizeof(head),"%s access=%08lx status=%08lx ",kind,access,(ULONG)status);
    std::string line=head+utf8(name)+"\r\n";DWORD n;WriteFile(trace_handle,line.data(),(DWORD)line.size(),&n,nullptr);
}
static thread_local unsigned bypass;
struct Guard {
    Guard(){++bypass;}
    ~Guard(){if(bypass==1)routing_view.reset();--bypass;}
};
const Prefix *PrefixAccess::operator->() const {
    // Hold one immutable drive view for an intercepted operation, including
    // nested backend calls. Loader configuration itself is never mutated.
    if(active&&bypass&&media_session&&!routing_view)routing_view=media_session->view();
    return routing_view?routing_view.get():configured_prefix;
}
extern "C" HMODULE WINAPI WinedowsLoadSystemXInput() {
    // This fixed host hardware backend must bypass both DLL-provider overrides
    // and private filesystem shadows. Keep the exception scoped to this call.
    Guard guard;
    try{return load_system_xinput();}catch(...){SetLastError(ERROR_NOT_ENOUGH_MEMORY);return nullptr;}
}
extern "C" HMODULE WINAPI WinedowsLoadSystemVulkan() {
    // Wine Vulkan's host loader must remain native even when vulkan-1 itself
    // is selected as a Wine provider. This accepts no application-supplied path.
    Guard guard;
    try{return load_system_vulkan();}catch(...){SetLastError(ERROR_NOT_ENOUGH_MEMORY);return nullptr;}
}
extern "C" BOOL WINAPI WinedowsCreateSystemDXGIFactory(const void *caller,UINT entrypoint,UINT flags,
                                                       REFIID iid,void **factory,HRESULT *result) {
    Guard guard;
    try{return create_system_dxgi_factory(caller,entrypoint,flags,iid,factory,result);}
    catch(...){if(factory)*factory=nullptr;if(result)*result=E_OUTOFMEMORY;return TRUE;}
}
extern "C" BOOL WINAPI WinedowsCreateSystemD3D11Device(const void *caller,UINT entrypoint,IDXGIAdapter *adapter,
        D3D_DRIVER_TYPE driver_type,HMODULE software,UINT flags,const D3D_FEATURE_LEVEL *levels,
        UINT level_count,UINT sdk_version,const DXGI_SWAP_CHAIN_DESC *description,IDXGISwapChain **swapchain,
        ID3D11Device **device,D3D_FEATURE_LEVEL *feature_level,ID3D11DeviceContext **context,HRESULT *result) {
    Guard guard;
    try{return create_system_d3d11_device(caller,entrypoint,adapter,driver_type,software,flags,levels,
            level_count,sdk_version,description,swapchain,device,feature_level,context,result);}
    catch(...){
        if(swapchain)*swapchain=nullptr;if(device)*device=nullptr;if(context)*context=nullptr;
        if(feature_level)*feature_level=(D3D_FEATURE_LEVEL)0;if(result)*result=E_OUTOFMEMORY;
        return TRUE;
    }
}
static const NTSTATUS denied=(NTSTATUS)0xc0000022L, readonly_status=(NTSTATUS)0xc00000a2L;
static const NTSTATUS missing=(NTSTATUS)0xc0000034L;
static const NTSTATUS no_media=(NTSTATUS)0xc0000013L;
static const NTSTATUS media_changed=(NTSTATUS)0x8000001cL;
using CreateFileFn=NTSTATUS(NTAPI*)(PHANDLE,ACCESS_MASK,POBJECT_ATTRIBUTES,PIO_STATUS_BLOCK,PLARGE_INTEGER,ULONG,ULONG,ULONG,ULONG,PVOID,ULONG);
using OpenFileFn=NTSTATUS(NTAPI*)(PHANDLE,ACCESS_MASK,POBJECT_ATTRIBUTES,PIO_STATUS_BLOCK,ULONG,ULONG);
using QueryAttrFn=NTSTATUS(NTAPI*)(POBJECT_ATTRIBUTES,PVOID);
using SetFileFn=NTSTATUS(NTAPI*)(HANDLE,PIO_STATUS_BLOCK,PVOID,ULONG,FILE_INFORMATION_CLASS);
using QueryVolumeFn=NTSTATUS(NTAPI*)(HANDLE,PIO_STATUS_BLOCK,PVOID,ULONG,int);
using FileIoFn=NTSTATUS(NTAPI*)(HANDLE,HANDLE,PIO_APC_ROUTINE,PVOID,PIO_STATUS_BLOCK,PVOID,ULONG,PLARGE_INTEGER,PULONG);
using QueryDirectoryFn=NTSTATUS(NTAPI*)(HANDLE,HANDLE,PIO_APC_ROUTINE,PVOID,PIO_STATUS_BLOCK,PVOID,ULONG,FILE_INFORMATION_CLASS,BOOLEAN,PUNICODE_STRING,BOOLEAN);
using QueryDirectoryExFn=NTSTATUS(NTAPI*)(HANDLE,HANDLE,PIO_APC_ROUTINE,PVOID,PIO_STATUS_BLOCK,PVOID,ULONG,FILE_INFORMATION_CLASS,ULONG,PUNICODE_STRING);
using FlushFileFn=NTSTATUS(NTAPI*)(HANDLE,PIO_STATUS_BLOCK);
using DeviceIoFn=NTSTATUS(NTAPI*)(HANDLE,HANDLE,PIO_APC_ROUTINE,PVOID,PIO_STATUS_BLOCK,ULONG,PVOID,ULONG,PVOID,ULONG);
using CreateSectionFn=NTSTATUS(NTAPI*)(PHANDLE,ACCESS_MASK,POBJECT_ATTRIBUTES,PLARGE_INTEGER,ULONG,ULONG,HANDLE);
using CreateSectionExFn=NTSTATUS(NTAPI*)(PHANDLE,ACCESS_MASK,POBJECT_ATTRIBUTES,PLARGE_INTEGER,ULONG,ULONG,HANDLE,PVOID,ULONG);
using CreateKeyFn=NTSTATUS(NTAPI*)(PHANDLE,ACCESS_MASK,POBJECT_ATTRIBUTES,ULONG,PUNICODE_STRING,ULONG,PULONG);
using OpenKeyFn=NTSTATUS(NTAPI*)(PHANDLE,ACCESS_MASK,POBJECT_ATTRIBUTES);
using OpenKeyExFn=NTSTATUS(NTAPI*)(PHANDLE,ACCESS_MASK,POBJECT_ATTRIBUTES,ULONG);
using QueryKeyFn=NTSTATUS(NTAPI*)(HANDLE,int,PVOID,ULONG,PULONG);
using SetValueFn=NTSTATUS(NTAPI*)(HANDLE,PUNICODE_STRING,ULONG,ULONG,PVOID,ULONG);
using DeleteValueFn=NTSTATUS(NTAPI*)(HANDLE,PUNICODE_STRING);
using DeleteKeyFn=NTSTATUS(NTAPI*)(HANDLE);
using CloseFn=NTSTATUS(NTAPI*)(HANDLE);
static CreateFileFn real_create_file;
static OpenFileFn real_open_file;
static QueryAttrFn real_attr,real_fullattr;
static SetFileFn real_set_file;
static QueryVolumeFn real_query_volume;
static FileIoFn real_read_file,real_write_file;
static QueryDirectoryFn real_query_directory;
static QueryDirectoryExFn real_query_directory_ex;
static SetFileFn real_query_file;
static FlushFileFn real_flush_file;
static DeviceIoFn real_device_io;
static CreateSectionFn real_create_section;
static CreateSectionExFn real_create_section_ex;
static CreateKeyFn real_create_key;
static OpenKeyFn real_open_key;
static OpenKeyExFn real_open_key_ex;
static QueryKeyFn real_query_key;
static SetValueFn real_set_value;
static DeleteValueFn real_delete_value;
static DeleteKeyFn real_delete_key;
static CloseFn real_close;
static decltype(&CreateProcessW) real_process_w=CreateProcessW;
static decltype(&CreateProcessA) real_process_a=CreateProcessA;
static decltype(&WinExec) real_win_exec=WinExec;
static decltype(&OpenFile) real_legacy_open=OpenFile;
static decltype(&GetDriveTypeW) real_drive_w=GetDriveTypeW;
static decltype(&GetDriveTypeA) real_drive_a=GetDriveTypeA;
static decltype(&GetLogicalDrives) real_logical=GetLogicalDrives;
static decltype(&GetLogicalDriveStringsW) real_strings_w=GetLogicalDriveStringsW;
static decltype(&GetLogicalDriveStringsA) real_strings_a=GetLogicalDriveStringsA;
static decltype(&GetVolumeInformationW) real_volume_w=GetVolumeInformationW;
static decltype(&GetVolumeInformationA) real_volume_a=GetVolumeInformationA;
static decltype(&GetDiskFreeSpaceExW) real_space_w=GetDiskFreeSpaceExW;
static decltype(&GetDiskFreeSpaceExA) real_space_a=GetDiskFreeSpaceExA;
static decltype(&GetDiskFreeSpaceW) real_disk_w=GetDiskFreeSpaceW;
static decltype(&GetDiskFreeSpaceA) real_disk_a=GetDiskFreeSpaceA;
static decltype(&SHGetKnownFolderPath) real_known=SHGetKnownFolderPath;
static decltype(&SHGetFolderPathW) real_folder_w=SHGetFolderPathW;
static decltype(&SHGetFolderPathA) real_folder_a=SHGetFolderPathA;
static decltype(&DeviceIoControl) real_ioctl=DeviceIoControl;

static bool hooked(){return active && !bypass;}
static str string_of(PUNICODE_STRING s){return s&&s->Buffer?str(s->Buffer,s->Length/2):str();}
static str handle_path(HANDLE h) {
    std::vector<wchar_t> b(32768);DWORD n=GetFinalPathNameByHandleW(h,b.data(),(DWORD)b.size(),FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);
    if(!n||n>=b.size())return {};str s(b.data(),n);if(starts(s,L"\\\\?\\"))s.erase(0,4);return s;
}
static bool medium_absent(int drive) {return media_session&&!media_session->snapshot(drive).present;}
static NTSTATUS path_media_status(const str &path) {
    if(path.empty()||!media_session||!media_session->needs_handle_check())return 0;
    auto identity=media_session->path_identity(path);
    auto state=media_session->snapshot(identity.drive);
    if(!state.present)return no_media;
    return identity.medium&&identity.medium!=state.medium?media_changed:0;
}
static NTSTATUS handle_media_status(HANDLE handle) {
    // Avoid resolving every ordinary handle until media has been removed or
    // replaced. Immutable cache paths identify duplicated/inherited old handles.
    if(!media_session||!media_session->needs_handle_check()||GetFileType(handle)!=FILE_TYPE_DISK)return 0;
    return path_media_status(handle_path(handle));
}
static NTSTATUS media_failure(PIO_STATUS_BLOCK io,NTSTATUS status=no_media) {
    if(io){io->Status=status;io->Information=0;}return status;
}
static str key_path(HANDLE h) {
    ULONG n=0;real_query_key(h,3,nullptr,0,&n);if(n<4 || n>1024*1024)return {};
    std::vector<BYTE> b(n);if(real_query_key(h,3,b.data(),n,&n)<0)return {};
    ULONG length=*(ULONG*)b.data();if(length>b.size()-4)return {};return str((wchar_t*)(b.data()+4),length/2);
}
struct ObjectName {
    OBJECT_ATTRIBUTES oa{};UNICODE_STRING us{};str path,original;
    bool changed=false,ro=false,host_read=false,volume=false,image=false;
    int drive=-1;
    RegistryView view=RegistryView::unknown;
    explicit ObjectName(POBJECT_ATTRIBUTES source):oa(*source){}
    void assign(const str &s,HANDLE root=nullptr) {
        path=s;us.Buffer=path.data();us.Length=(USHORT)(path.size()*2);us.MaximumLength=us.Length;
        oa.ObjectName=&us;oa.RootDirectory=root;changed=true;
    }
};
static str system_file_path(const str &path) {
    if(sizeof(void*)!=4||(!inside(path,prefix->windows+L"\\System32")&&!inside(path,prefix->windows+L"\\Sysnative")))return path;
    // The x86 loader can normalize SysWOW64 to System32 before its NT calls.
    // Redirecting that name into the prefix first would bypass WOW64's native
    // filesystem view. Resolve an existing parent with the real NT API, which
    // respects thread disable/revert state, Sysnative, and exempt subfolders.
    str parent=path;
    while(inside(parent,prefix->windows)){
        str name=L"\\??\\"+parent;
        UNICODE_STRING us{(USHORT)(name.size()*2),(USHORT)(name.size()*2),name.data()};
        OBJECT_ATTRIBUTES oa{sizeof(oa),nullptr,&us,OBJ_CASE_INSENSITIVE,nullptr,nullptr};
        IO_STATUS_BLOCK io{};HANDLE handle=nullptr;
        NTSTATUS status=real_open_file(&handle,FILE_READ_ATTRIBUTES|SYNCHRONIZE,&oa,&io,
            FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,0x21); // directory, synchronous nonalert
        if(status>=0){
            str resolved=handle_path(handle);real_close(handle);
            if(!resolved.empty())return resolved+path.substr(parent.size());
            return path;
        }
        str next=directory(parent);if(next==parent)break;parent=std::move(next);
    }
    return path;
}
static void file_name(ObjectName &n) {
    str s=string_of(n.oa.ObjectName);
    if(n.oa.RootDirectory) {str base=handle_path(n.oa.RootDirectory);if(base.empty())return;s=base+L"\\"+s;}
    if(starts(s,L"\\??\\") || starts(s,L"\\\\?\\"))s.erase(0,4);
    if(starts(s,L"\\DosDevices\\"))s.erase(0,12);
    if(s.size()<2 || s[1]!=L':' || !iswalpha(s[0]))return;
    // A bare NT drive name (Win32 \\.\D:) is a volume device open.
    // Images retain their sector stream; directory mounts use unique per-letter
    // objects so aliased filesystem roots do not lose raw device identity.
    // Neither path opens the similarly named host disk device.
    n.volume=s.size()==2;
    if(n.volume)s+=L"\\";
    // Resolve dot components before deciding which drive or backing directory is involved.
    s=system_file_path(fullpath(s)); n.original=s;n.ro=prefix->readonly(s);
    str translated=prefix->translate(s);
    int drive=n.drive=prefix->drive_index(s);
    if(n.volume&&drive>=0&&!prefix->drives[drive].path.empty()){
        if(!prefix->drives[drive].image.empty()){
            translated=prefix->drives[drive].image;n.image=true;
        } else translated=prefix->raw_device(drive);
        n.ro=true;
    }
    n.host_read=inside(s,prefix->windows)||inside(s,appdir)||inside(s,runtime_dir);
    if(translated.size()>32760)throw std::runtime_error("Path too long");
    n.assign(L"\\??\\"+translated);
}
static bool writing(ACCESS_MASK a,ULONG options=0) {
    return (a&(GENERIC_WRITE|GENERIC_ALL|FILE_WRITE_DATA|FILE_APPEND_DATA|FILE_WRITE_EA|FILE_WRITE_ATTRIBUTES|DELETE|WRITE_DAC|WRITE_OWNER)) || (options&0x1000); // FILE_DELETE_ON_CLOSE
}
static void copy_on_write(const ObjectName &n) {
    if(!n.changed || n.original.empty())return;
    str dest=n.path.substr(4);
    if(_wcsicmp(dest.c_str(),n.original.c_str())==0)return;
    // Only designated host runtime/app read-through sources can be copied.
    if(n.host_read && GetFileAttributesW(dest.c_str())==INVALID_FILE_ATTRIBUTES) {
        DWORD a=GetFileAttributesW(n.original.c_str());
        if(a!=INVALID_FILE_ATTRIBUTES && !(a&FILE_ATTRIBUTE_DIRECTORY)) {
            mkdirs(directory(dest));if(!CopyFileW(n.original.c_str(),dest.c_str(),TRUE) && GetLastError()!=ERROR_FILE_EXISTS)check(GetLastError(),"Copy on write");
        }
    }
}
static NTSTATUS NTAPI create_file(PHANDLE out,ACCESS_MASK access,POBJECT_ATTRIBUTES oa,PIO_STATUS_BLOCK io,PLARGE_INTEGER allocation,ULONG attr,ULONG sharing,ULONG disposition,ULONG options,PVOID ea,ULONG ealen) {
    if(!hooked()||!oa)return real_create_file(out,access,oa,io,allocation,attr,sharing,disposition,options,ea,ealen);
    Guard guard;
    try {
        if(oa->RootDirectory){if(auto status=handle_media_status(oa->RootDirectory))return media_failure(io,status);}
        ObjectName n(oa);file_name(n);bool write=writing(access,options)||disposition!=1; // FILE_OPEN
        if(!n.volume){if(auto status=path_media_status(n.original))return media_failure(io,status);}
        if(!n.volume&&medium_absent(n.drive))return media_failure(io);
        if(n.ro&&write)return readonly_status;
        if(n.volume){options=(options&~0x41u)|(n.image?0x40u:1u);n.host_read=false;}
        if(write)copy_on_write(n);
        NTSTATUS status=real_create_file(out,access,&n.oa,io,allocation,attr,sharing,disposition,options,ea,ealen);
        if(status<0 && !write && n.host_read && n.changed)status=real_create_file(out,access,oa,io,allocation,attr,sharing,disposition,options,ea,ealen);
        if(status>=0){NTSTATUS tracked=module_file_opened(*out,oa->ObjectName);if(tracked<0){real_close(*out);*out=nullptr;return media_failure(io,tracked);}}
        if(status<0)trace("file",n.original,access,status);return status;
    }catch(...){return denied;}
}
static NTSTATUS NTAPI open_file(PHANDLE out,ACCESS_MASK access,POBJECT_ATTRIBUTES oa,PIO_STATUS_BLOCK io,ULONG sharing,ULONG options) {
    if(!hooked()||!oa)return real_open_file(out,access,oa,io,sharing,options);Guard guard;
    try {
        if(oa->RootDirectory){if(auto status=handle_media_status(oa->RootDirectory))return media_failure(io,status);}
        ObjectName n(oa);file_name(n);bool write=writing(access,options);
        if(!n.volume){if(auto status=path_media_status(n.original))return media_failure(io,status);}
        if(!n.volume&&medium_absent(n.drive))return media_failure(io);
        if(n.ro&&write)return readonly_status;if(write)copy_on_write(n);
        if(n.volume){options=(options&~0x41u)|(n.image?0x40u:1u);n.host_read=false;}
        NTSTATUS status=real_open_file(out,access,&n.oa,io,sharing,options);
        if(status<0 && !write && n.host_read && n.changed)status=real_open_file(out,access,oa,io,sharing,options);
        if(status>=0){NTSTATUS tracked=module_file_opened(*out,oa->ObjectName);if(tracked<0){real_close(*out);*out=nullptr;return media_failure(io,tracked);}}
        if(status<0)trace("open",n.original,access,status);return status;
    }catch(...){return denied;}
}
static NTSTATUS query_attr(QueryAttrFn fn,POBJECT_ATTRIBUTES oa,PVOID out) {
    if(!hooked()||!oa)return fn(oa,out);Guard guard;
    try {
        if(oa->RootDirectory){if(auto status=handle_media_status(oa->RootDirectory))return status;}
        ObjectName n(oa);file_name(n);
        if(!n.volume){if(auto status=path_media_status(n.original))return status;if(medium_absent(n.drive))return no_media;}
        NTSTATUS s=fn(&n.oa,out);if(s<0&&n.host_read&&n.changed)s=fn(oa,out);return s;
    }catch(...){return denied;}
}
static NTSTATUS NTAPI attr(POBJECT_ATTRIBUTES oa,PVOID out){return query_attr(real_attr,oa,out);}
static NTSTATUS NTAPI fullattr(POBJECT_ATTRIBUTES oa,PVOID out){return query_attr(real_fullattr,oa,out);}
static NTSTATUS file_io(FileIoFn fn,HANDLE h,HANDLE event,PIO_APC_ROUTINE apc,PVOID context,PIO_STATUS_BLOCK io,PVOID buffer,ULONG size,PLARGE_INTEGER offset,PULONG key) {
    if(!hooked())return fn(h,event,apc,context,io,buffer,size,offset,key);Guard guard;
    try{if(auto status=handle_media_status(h))return media_failure(io,status);return fn(h,event,apc,context,io,buffer,size,offset,key);}catch(...){return denied;}
}
static NTSTATUS NTAPI read_file(HANDLE h,HANDLE event,PIO_APC_ROUTINE apc,PVOID context,PIO_STATUS_BLOCK io,PVOID buffer,ULONG size,PLARGE_INTEGER offset,PULONG key) {
    return file_io(real_read_file,h,event,apc,context,io,buffer,size,offset,key);
}
static NTSTATUS NTAPI write_file(HANDLE h,HANDLE event,PIO_APC_ROUTINE apc,PVOID context,PIO_STATUS_BLOCK io,PVOID buffer,ULONG size,PLARGE_INTEGER offset,PULONG key) {
    return file_io(real_write_file,h,event,apc,context,io,buffer,size,offset,key);
}
static NTSTATUS NTAPI query_directory(HANDLE h,HANDLE event,PIO_APC_ROUTINE apc,PVOID context,PIO_STATUS_BLOCK io,PVOID buffer,ULONG size,FILE_INFORMATION_CLASS cls,BOOLEAN single,PUNICODE_STRING name,BOOLEAN restart) {
    if(!hooked())return real_query_directory(h,event,apc,context,io,buffer,size,cls,single,name,restart);Guard guard;
    try{if(auto status=handle_media_status(h))return media_failure(io,status);return real_query_directory(h,event,apc,context,io,buffer,size,cls,single,name,restart);}catch(...){return denied;}
}
static NTSTATUS NTAPI query_directory_ex(HANDLE h,HANDLE event,PIO_APC_ROUTINE apc,PVOID context,PIO_STATUS_BLOCK io,PVOID buffer,ULONG size,FILE_INFORMATION_CLASS cls,ULONG flags,PUNICODE_STRING name) {
    if(!hooked())return real_query_directory_ex(h,event,apc,context,io,buffer,size,cls,flags,name);Guard guard;
    try{if(auto status=handle_media_status(h))return media_failure(io,status);return real_query_directory_ex(h,event,apc,context,io,buffer,size,cls,flags,name);}catch(...){return denied;}
}
static NTSTATUS NTAPI query_file(HANDLE h,PIO_STATUS_BLOCK io,PVOID data,ULONG len,FILE_INFORMATION_CLASS cls) {
    if(!hooked())return real_query_file(h,io,data,len,cls);Guard guard;
    try{if((int)cls!=14&&(int)cls!=16){if(auto status=handle_media_status(h))return media_failure(io,status);}return real_query_file(h,io,data,len,cls);}catch(...){return denied;}
}
static NTSTATUS NTAPI flush_file(HANDLE h,PIO_STATUS_BLOCK io) {
    if(!hooked())return real_flush_file(h,io);Guard guard;
    try{if(auto status=handle_media_status(h))return media_failure(io,status);return real_flush_file(h,io);}catch(...){return denied;}
}
static NTSTATUS NTAPI create_section(PHANDLE section,ACCESS_MASK access,POBJECT_ATTRIBUTES attributes,PLARGE_INTEGER size,ULONG protection,ULONG allocation,HANDLE file) {
    if(!hooked())return real_create_section(section,access,attributes,size,protection,allocation,file);Guard guard;
    try{if(file){if(auto status=handle_media_status(file))return status;if(module_image_disabled(file,allocation))return (NTSTATUS)0xc0000135;}NamedObjectAttributes name(attributes);return real_create_section(section,access,name.get(),size,protection,allocation,file);}catch(...){return denied;}
}
static NTSTATUS NTAPI create_section_ex(PHANDLE section,ACCESS_MASK access,POBJECT_ATTRIBUTES attributes,PLARGE_INTEGER size,ULONG protection,ULONG allocation,HANDLE file,PVOID parameters,ULONG count) {
    if(!hooked())return real_create_section_ex(section,access,attributes,size,protection,allocation,file,parameters,count);Guard guard;
    try{if(file){if(auto status=handle_media_status(file))return status;if(module_image_disabled(file,allocation))return (NTSTATUS)0xc0000135;}NamedObjectAttributes name(attributes);return real_create_section_ex(section,access,name.get(),size,protection,allocation,file,parameters,count);}catch(...){return denied;}
}
struct RenameInfo {DWORD flags;HANDLE root;ULONG length;WCHAR name[1];};
static NTSTATUS NTAPI set_file(HANDLE h,PIO_STATUS_BLOCK io,PVOID data,ULONG len,FILE_INFORMATION_CLASS cls) {
    if(!hooked())return real_set_file(h,io,data,len,cls);Guard guard;
    try {
        // Position, mode and completion-port association describe the open
        // file object rather than disc content. Let native Windows validate
        // them, including rejecting completion ports on synchronous handles.
        if((int)cls==14 || (int)cls==16 || (int)cls==30 || (int)cls==61)return real_set_file(h,io,data,len,cls);
        if(auto status=handle_media_status(h))return media_failure(io,status);
        str path=handle_path(h);if(!path.empty()&&prefix->readonly(path))return readonly_status;
        // Rename and hard-link destinations must be translated as well as the source handle.
        int c=(int)cls;
        if((c==10||c==11||c==65||c==72) && len>=offsetof(RenameInfo,name)) {
            auto r=(RenameInfo*)data;if(r->length>len-offsetof(RenameInfo,name))return denied;
            UNICODE_STRING us{(USHORT)r->length,(USHORT)r->length,r->name};OBJECT_ATTRIBUTES oa{sizeof(oa),r->root,&us,OBJ_CASE_INSENSITIVE,nullptr,nullptr};
            if(oa.RootDirectory){if(auto status=handle_media_status(oa.RootDirectory))return media_failure(io,status);}
            ObjectName n(&oa);file_name(n);if(auto status=path_media_status(n.original))return media_failure(io,status);
            if(medium_absent(n.drive))return media_failure(io);if(n.ro)return readonly_status;
            if(n.changed) {std::vector<BYTE> b(offsetof(RenameInfo,name)+n.path.size()*2);auto m=(RenameInfo*)b.data();m->flags=r->flags;m->root=nullptr;m->length=(ULONG)n.path.size()*2;memcpy(m->name,n.path.data(),m->length);return real_set_file(h,io,m,(ULONG)b.size(),cls);}
        }
        // Never mutate a host read-through handle. Writable opens obtain prefix handles.
        if(!path.empty() && !inside(path,prefix->root)) {
            int d=prefix->drive_index(path);if(d<0||!inside(path,prefix->drives[d].path)||prefix->drives[d].readonly)return denied;
        }
        return real_set_file(h,io,data,len,cls);
    }catch(...){return denied;}
}
static str logical_key_name(const str &path) {
    if(!machine_path.empty()&&inside(path,machine_path))return L"\\REGISTRY\\MACHINE"+path.substr(machine_path.size());
    // Windows mounts UsrClass.dat at SID_Classes and links HKCU\Software\Classes
    // to it. Native HKCR merging uses that canonical root when joining names.
    if(!user_path.empty()&&inside(path,user_path+L"\\Software\\Classes"))return L"\\REGISTRY\\USER\\"+user_sid+L"_Classes"+path.substr(user_path.size()+17);
    if(!user_path.empty()&&inside(path,user_path))return L"\\REGISTRY\\USER\\"+user_sid+path.substr(user_path.size());
    if(!default_path.empty()&&inside(path,default_path))return L"\\REGISTRY\\USER\\.DEFAULT"+path.substr(default_path.size());
    return path;
}
static NTSTATUS NTAPI query_key(HANDLE h,int cls,PVOID out,ULONG size,PULONG needed) {
    if(!hooked()||cls!=3)return real_query_key(h,cls,out,size,needed);
    Guard guard;
    try {
        str actual=key_path(h),logical=logical_key_name(actual);
        if(logical==actual)return real_query_key(h,cls,out,size,needed);
        ULONG bytes=(ULONG)(logical.size()*sizeof(wchar_t)),required=sizeof(ULONG)+bytes;
        if(!needed)return (NTSTATUS)0xc0000005L;
        *needed=required;
        if(size<sizeof(ULONG))return (NTSTATUS)0xc0000023L;
        if(!out)return (NTSTATUS)0xc0000005L;
        *(ULONG*)out=bytes;
        memcpy((BYTE*)out+sizeof(ULONG),logical.data(),std::min(bytes,size-(ULONG)sizeof(ULONG)));
        return size<required?(NTSTATUS)0x80000005L:0;
    }catch(...){return denied;}
}
static void registry_name(ObjectName &n,ACCESS_MASK access=0,bool choose_view=false) {
    str s=string_of(n.oa.ObjectName);
    str base;
    if(n.oa.RootDirectory){base=key_path(n.oa.RootDirectory);if(base.empty())return;s=base+(s.empty()?L"":L"\\"+s);}
    s=logical_key_name(s);
    if(choose_view){auto route=registry_view_name(s,access,n.oa.RootDirectory,logical_key_name(base),real_open_key,real_query_key);s=std::move(route.name);n.view=route.view;}
    n.original=s;
    const str machine=L"\\REGISTRY\\MACHINE",user=L"\\REGISTRY\\USER\\"+user_sid,def=L"\\REGISTRY\\USER\\.DEFAULT";
    HANDLE root=nullptr;size_t offset=0;
    if(inside(s,machine)){root=machine_root;offset=machine.size();}
    else if(inside(s,user)){root=user_root;offset=user.size();}
    else if(inside(s,user+L"_Classes")){root=user_root;offset=(user+L"_Classes").size();s=L"Software\\Classes"+s.substr(offset);n.assign(s,root);return;}
    else if(inside(s,def)){root=default_root;offset=def.size();}
    if(!root)return;
    str tail=s.substr(offset);if(!tail.empty()&&tail[0]==L'\\')tail.erase(0,1);
    // Native device and COM/RPC backends need their host configuration. OLE
    // supplies process authentication policy; Rpc supplies marshaling providers.
    // Application Classes registration remains private. Writes still
    // resolve to the private hive, including writes through a read-through handle.
    str backend_tail=tail;
    const str wow_software=L"SOFTWARE\\WOW6432Node";
    if(inside(backend_tail,wow_software))backend_tail=L"SOFTWARE"+backend_tail.substr(wow_software.size());
    auto backend=[&](const wchar_t *branch){return inside(backend_tail,branch);};
    n.host_read=root==machine_root && (backend(L"SYSTEM")||backend(L"HARDWARE")||backend(L"SOFTWARE\\Microsoft\\Cryptography")||backend(L"SOFTWARE\\Microsoft\\.NETFramework")||backend(L"SOFTWARE\\Microsoft\\NET Framework Setup\\NDP")||backend(L"SOFTWARE\\Microsoft\\OLE")||backend(L"SOFTWARE\\Microsoft\\Rpc")||backend(L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion")||backend(L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices")||backend(L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Audio")||backend(L"SOFTWARE\\Khronos")||backend(L"SOFTWARE\\NVIDIA Corporation")||backend(L"SOFTWARE\\AMD")||backend(L"SOFTWARE\\Intel"));
    n.assign(tail,root);
}
static NTSTATUS NTAPI create_key(PHANDLE h,ACCESS_MASK a,POBJECT_ATTRIBUTES oa,ULONG title,PUNICODE_STRING cls,ULONG opts,PULONG disp) {
    if(!hooked()||!oa)return real_create_key(h,a,oa,title,cls,opts,disp);Guard guard;
    try{ObjectName n(oa);registry_name(n,a,true);ACCESS_MASK selected=n.changed?registry_view_access(a,n.view):a;
        return real_create_key(h,selected,&n.oa,title,cls,opts,disp);}catch(...){return denied;}
}
static NTSTATUS NTAPI open_key(PHANDLE h,ACCESS_MASK a,POBJECT_ATTRIBUTES oa) {
    if(!hooked()||!oa)return real_open_key(h,a,oa);Guard guard;
    try{ObjectName n(oa);registry_name(n,a,true);ACCESS_MASK selected=n.changed?registry_view_access(a,n.view):a;
        NTSTATUS s=real_open_key(h,selected,&n.oa);if(s<0&&n.host_read){ObjectName host(oa);host.assign(n.original);s=real_open_key(h,KEY_READ|(a&(KEY_WOW64_32KEY|KEY_WOW64_64KEY)),&host.oa);}if(s<0)trace("key",n.original,a,s);return s;}catch(...){return denied;}
}
static NTSTATUS NTAPI open_key_ex(PHANDLE h,ACCESS_MASK a,POBJECT_ATTRIBUTES oa,ULONG opts) {
    if(!hooked()||!oa)return real_open_key_ex(h,a,oa,opts);Guard guard;
    try{ObjectName n(oa);registry_name(n,a,true);ACCESS_MASK selected=n.changed?registry_view_access(a,n.view):a;
        NTSTATUS s=real_open_key_ex(h,selected,&n.oa,opts);if(s<0&&n.host_read){ObjectName host(oa);host.assign(n.original);s=real_open_key_ex(h,KEY_READ|(a&(KEY_WOW64_32KEY|KEY_WOW64_64KEY)),&host.oa,opts);}if(s<0)trace("key-ex",n.original,a,s);return s;}catch(...){return denied;}
}
static bool private_write_handle(HANDLE h,HANDLE &mapped,bool create) {
    str name=key_path(h);UNICODE_STRING us{(USHORT)(name.size()*2),(USHORT)(name.size()*2),name.data()};OBJECT_ATTRIBUTES oa{sizeof(oa),nullptr,&us,OBJ_CASE_INSENSITIVE,nullptr,nullptr};
    ObjectName n(&oa);registry_name(n);mapped=h;
    if(!n.changed)return true;
    NTSTATUS s=create?real_create_key(&mapped,KEY_ALL_ACCESS,&n.oa,0,nullptr,0,nullptr):real_open_key(&mapped,KEY_ALL_ACCESS,&n.oa);
    return s>=0;
}
static NTSTATUS NTAPI set_value(HANDLE h,PUNICODE_STRING name,ULONG title,ULONG type,PVOID data,ULONG len) {
    if(!hooked())return real_set_value(h,name,title,type,data,len);Guard guard;
    try{HANDLE mapped;if(!private_write_handle(h,mapped,true))return denied;NTSTATUS s=real_set_value(mapped,name,title,type,data,len);if(mapped!=h)RegCloseKey((HKEY)mapped);return s;}catch(...){return denied;}
}
static NTSTATUS NTAPI delete_value(HANDLE h,PUNICODE_STRING name) {
    if(!hooked())return real_delete_value(h,name);Guard guard;
    try{HANDLE mapped;if(!private_write_handle(h,mapped,false))return missing;NTSTATUS s=real_delete_value(mapped,name);if(mapped!=h)RegCloseKey((HKEY)mapped);return s;}catch(...){return denied;}
}
static NTSTATUS NTAPI delete_key(HANDLE h) {
    if(!hooked())return real_delete_key(h);Guard guard;
    try{HANDLE mapped;if(!private_write_handle(h,mapped,false))return missing;NTSTATUS s=real_delete_key(mapped);if(mapped!=h)RegCloseKey((HKEY)mapped);return s;}catch(...){return denied;}
}
static str ansi(const char *s){if(!s)return {};int n=MultiByteToWideChar(CP_ACP,0,s,-1,nullptr,0);str w(n,0);MultiByteToWideChar(CP_ACP,0,s,-1,w.data(),n);w.resize(n-1);return w;}
static int drive_index(const wchar_t *path){if(!path){std::vector<wchar_t> cwd(32768);GetCurrentDirectoryW((DWORD)cwd.size(),cwd.data());return prefix->drive_index(cwd.data());}return prefix->drive_index(path);}
static UINT WINAPI drive_w(LPCWSTR s){if(!hooked())return real_drive_w(s);Guard g;int i=drive_index(s);return i<0?DRIVE_NO_ROOT_DIR:prefix->drives[i].type;}
static UINT WINAPI drive_a(LPCSTR s){if(!hooked())return real_drive_a(s);str w=ansi(s);return drive_w(s?w.c_str():nullptr);}
static DWORD WINAPI logical(){if(!hooked())return real_logical();DWORD bits=0;for(int i=0;i<26;i++)if(!prefix->drives[i].path.empty())bits|=1u<<i;return bits;}
template<class C> static DWORD drive_strings(DWORD size,C *buf){DWORD need=0;for(int i=0;i<26;i++)if(!prefix->drives[i].path.empty())need+=4;if(size<=need)return need+1;DWORD j=0;for(int i=0;i<26;i++)if(!prefix->drives[i].path.empty()){buf[j++]=C('A'+i);buf[j++]=C(':');buf[j++]=C('\\');buf[j++]=0;}buf[j]=0;return need;}
static DWORD WINAPI strings_w(DWORD n,LPWSTR b){return hooked()?drive_strings(n,b):real_strings_w(n,b);}
static DWORD WINAPI strings_a(DWORD n,LPSTR b){return hooked()?drive_strings(n,b):real_strings_a(n,b);}
static BOOL WINAPI volume_w(LPCWSTR path,LPWSTR label,DWORD lsize,LPDWORD serial,LPDWORD maxcomp,LPDWORD flags,LPWSTR fs,DWORD fsize) {
    if(!hooked())return real_volume_w(path,label,lsize,serial,maxcomp,flags,fs,fsize);Guard g;int i=drive_index(path);
    if(i<0||prefix->drives[i].path.empty()){SetLastError(ERROR_PATH_NOT_FOUND);return FALSE;}
    if(medium_absent(i)){SetLastError(ERROR_NOT_READY);return FALSE;}
    const auto &d=prefix->drives[i];str filesystem=d.type==DRIVE_CDROM?L"CDFS":L"NTFS";
    if((label&&lsize<=d.label.size())||(fs&&fsize<=filesystem.size())){SetLastError(ERROR_MORE_DATA);return FALSE;}
    if(label)wcscpy_s(label,lsize,d.label.c_str());if(fs)wcscpy_s(fs,fsize,filesystem.c_str());
    if(serial)*serial=d.serial;if(maxcomp)*maxcomp=255;if(flags)*flags=FILE_CASE_PRESERVED_NAMES|FILE_UNICODE_ON_DISK|(d.readonly?FILE_READ_ONLY_VOLUME:0);return TRUE;
}
static BOOL WINAPI volume_a(LPCSTR p,LPSTR label,DWORD ls,LPDWORD serial,LPDWORD maxc,LPDWORD flags,LPSTR fs,DWORD fsn) {
    if(!hooked())return real_volume_a(p,label,ls,serial,maxc,flags,fs,fsn);str path=ansi(p);wchar_t l[256],f[32];
    if(!volume_w(p?path.c_str():nullptr,l,256,serial,maxc,flags,f,32))return FALSE;
    if(label&&!WideCharToMultiByte(CP_ACP,0,l,-1,label,ls,nullptr,nullptr))return FALSE;
    if(fs&&!WideCharToMultiByte(CP_ACP,0,f,-1,fs,fsn,nullptr,nullptr))return FALSE;return TRUE;
}
static BOOL virtual_space(LPCWSTR path,PULARGE_INTEGER avail,PULARGE_INTEGER total,PULARGE_INTEGER freeb){
    int i=drive_index(path);
    if(i<0||prefix->drives[i].path.empty()){SetLastError(ERROR_PATH_NOT_FOUND);return FALSE;}
    if(medium_absent(i)){SetLastError(ERROR_NOT_READY);return FALSE;}
    const auto &drive=prefix->drives[i];
    if(!drive.image.empty()){if(avail)avail->QuadPart=0;if(freeb)freeb->QuadPart=0;if(total)total->QuadPart=drive.volume_size;return TRUE;}
    BOOL ok=real_space_w(drive.path.c_str(),avail,total,freeb);
    if(ok&&drive.readonly){if(avail)avail->QuadPart=0;if(freeb)freeb->QuadPart=0;}return ok;
}
static BOOL WINAPI space_w(LPCWSTR path,PULARGE_INTEGER avail,PULARGE_INTEGER total,PULARGE_INTEGER freeb){
    if(!hooked())return real_space_w(path,avail,total,freeb);Guard g;return virtual_space(path,avail,total,freeb);
}
static BOOL WINAPI space_a(LPCSTR p,PULARGE_INTEGER a,PULARGE_INTEGER b,PULARGE_INTEGER c){if(!hooked())return real_space_a(p,a,b,c);str w=ansi(p);return space_w(p?w.c_str():nullptr,a,b,c);}
static BOOL WINAPI disk_w(LPCWSTR p,LPDWORD spc,LPDWORD bps,LPDWORD freec,LPDWORD totalc){
    if(!hooked())return real_disk_w(p,spc,bps,freec,totalc);Guard guard;ULARGE_INTEGER a{},b{},c{};if(!virtual_space(p,&a,&b,&c))return FALSE;
    int i=drive_index(p);bool image=i>=0&&!prefix->drives[i].image.empty();DWORD sectors=image?1:8,bytes=image?prefix->drives[i].sector_size:512;
    if(spc)*spc=sectors;if(bps)*bps=bytes;
    if(freec)*freec=(DWORD)std::min<ULONGLONG>(c.QuadPart/(sectors*bytes),0xffffffff);
    if(totalc)*totalc=(DWORD)std::min<ULONGLONG>(b.QuadPart/(sectors*bytes),0xffffffff);return TRUE;
}
static BOOL WINAPI disk_a(LPCSTR p,LPDWORD a,LPDWORD b,LPDWORD c,LPDWORD d){if(!hooked())return real_disk_a(p,a,b,c,d);str w=ansi(p);return disk_w(p?w.c_str():nullptr,a,b,c,d);}
struct VolumeInfo {LARGE_INTEGER created;ULONG serial,length;BOOLEAN objects;WCHAR label[1];};
struct AttributeInfo {ULONG flags;LONG max_component;ULONG length;WCHAR name[1];};
static NTSTATUS volume_result(PIO_STATUS_BLOCK io,PVOID out,ULONG capacity,const void *data,ULONG size,ULONG minimum) {
    const NTSTATUS too_small=(NTSTATUS)0xc0000004L,overflow=(NTSTATUS)0x80000005L;
    if(!io||!out)return (NTSTATUS)0xc000000dL;
    io->Information=0;
    if(capacity<minimum)return io->Status=too_small;
    ULONG copied=std::min(size,capacity);memcpy(out,data,copied);io->Information=copied;
    return io->Status=capacity<size?overflow:0;
}
static NTSTATUS NTAPI query_volume(HANDLE h,PIO_STATUS_BLOCK io,PVOID out,ULONG n,int c){
    if(!hooked())return real_query_volume(h,io,out,n,c);Guard g;
    try {
    str path=handle_path(h);auto identity=media_session->path_identity(path);int i=identity.drive;
    if(i>=0 && !prefix->drives[i].path.empty()) {
        const auto &d=prefix->drives[i];
        if(c==4) {
            DWORD device[]={DWORD(d.type==DRIVE_CDROM?FILE_DEVICE_CD_ROM:FILE_DEVICE_DISK),d.type==DRIVE_FIXED?0u:1u};
            if(d.readonly)device[1]|=2;
            return volume_result(io,out,n,device,sizeof(device),sizeof(device));
        }
        if(medium_absent(i))return media_failure(io);
        if(identity.medium&&identity.medium!=media_session->snapshot(i).medium)return media_failure(io,media_changed);
        if(c==1) {
            ULONG size=(ULONG)(offsetof(VolumeInfo,label)+d.label.size()*2);
            std::vector<BYTE>b(std::max(size,(ULONG)sizeof(VolumeInfo)));auto v=(VolumeInfo*)b.data();
            v->serial=d.serial;v->length=(ULONG)d.label.size()*2;memcpy(v->label,d.label.data(),v->length);
            return volume_result(io,out,n,b.data(),size,sizeof(VolumeInfo));
        }
        if(c==5) {
            str name=d.type==DRIVE_CDROM?L"CDFS":L"NTFS";ULONG size=(ULONG)(offsetof(AttributeInfo,name)+name.size()*2);
            std::vector<BYTE>b(size);auto v=(AttributeInfo*)b.data();
            v->flags=FILE_CASE_PRESERVED_NAMES|FILE_UNICODE_ON_DISK|(d.readonly?FILE_READ_ONLY_VOLUME:0);
            v->max_component=255;v->length=(ULONG)name.size()*2;memcpy(v->name,name.data(),v->length);
            return volume_result(io,out,n,b.data(),size,sizeof(AttributeInfo));
        }
        if(!d.image.empty()&&(c==3||c==7)) {
            struct SizeInfo {LARGE_INTEGER total,available;ULONG sectors,bytes;};
            struct FullSizeInfo {LARGE_INTEGER total,caller_available,available;ULONG sectors,bytes;};
            if(c==3){SizeInfo info{};info.total.QuadPart=d.volume_size/d.sector_size;info.sectors=1;info.bytes=d.sector_size;return volume_result(io,out,n,&info,sizeof(info),sizeof(info));}
            FullSizeInfo info{};info.total.QuadPart=d.volume_size/d.sector_size;info.sectors=1;info.bytes=d.sector_size;return volume_result(io,out,n,&info,sizeof(info),sizeof(info));
        }
    }
    return real_query_volume(h,io,out,n,c);
    }catch(...){return denied;}
}
static str folder_path(REFKNOWNFOLDERID id) {
    if(id==FOLDERID_Profile)return L"C:\\users\\user";
    if(id==FOLDERID_Documents)return L"C:\\users\\user\\Documents";
    if(id==FOLDERID_Desktop)return L"C:\\users\\user\\Desktop";
    if(id==FOLDERID_Downloads)return L"C:\\users\\user\\Downloads";
    if(id==FOLDERID_SavedGames)return L"C:\\users\\user\\Saved Games";
    if(id==FOLDERID_RoamingAppData)return L"C:\\users\\user\\AppData\\Roaming";
    if(id==FOLDERID_LocalAppData)return L"C:\\users\\user\\AppData\\Local";
    if(id==FOLDERID_LocalAppDataLow)return L"C:\\users\\user\\AppData\\LocalLow";
    if(id==FOLDERID_ProgramData)return L"C:\\ProgramData";
    if(id==FOLDERID_ProgramFilesX86)return L"C:\\Program Files (x86)";
    if(id==FOLDERID_ProgramFiles)return sizeof(void*)==8?L"C:\\Program Files":L"C:\\Program Files (x86)";
    if(id==FOLDERID_Windows)return L"C:\\windows";
    if(id==FOLDERID_System)return L"C:\\windows\\system32";
    if(id==FOLDERID_SystemX86)return L"C:\\windows\\syswow64";
    return {};
}
static HRESULT WINAPI known(REFKNOWNFOLDERID id,DWORD flags,HANDLE token,PWSTR *out) {
    if(hooked() && !token) {
        Guard g;str mapped=folder_path(id);
        if(!mapped.empty()) {
            if(!out)return E_POINTER;*out=nullptr;
            if(flags&KF_FLAG_CREATE){try{mkdirs(prefix->translate(mapped));}catch(...){return E_FAIL;}}
            auto p=(PWSTR)CoTaskMemAlloc((mapped.size()+1)*2);if(!p)return E_OUTOFMEMORY;memcpy(p,mapped.c_str(),(mapped.size()+1)*2);*out=p;return S_OK;
        }
    }
    if(!hooked())return real_known(id,flags,token,out);Guard g;HRESULT hr=real_known(id,flags,token,out);
    if(SUCCEEDED(hr)&&out&&*out){str mapped=prefix->virtualize(prefix->translate(*out));if(mapped!=*out){auto p=(PWSTR)CoTaskMemAlloc((mapped.size()+1)*2);if(!p)return E_OUTOFMEMORY;memcpy(p,mapped.c_str(),(mapped.size()+1)*2);CoTaskMemFree(*out);*out=p;}}
    return hr;
}
static HRESULT WINAPI folder_w(HWND hwnd,int folder,HANDLE token,DWORD flags,LPWSTR out) {
    if(hooked() && !token) {
        const KNOWNFOLDERID *id=nullptr;
        switch(folder&0xff){case CSIDL_PROFILE:id=&FOLDERID_Profile;break;case CSIDL_PERSONAL:id=&FOLDERID_Documents;break;case CSIDL_DESKTOP:case CSIDL_DESKTOPDIRECTORY:id=&FOLDERID_Desktop;break;case CSIDL_APPDATA:id=&FOLDERID_RoamingAppData;break;case CSIDL_LOCAL_APPDATA:id=&FOLDERID_LocalAppData;break;case CSIDL_COMMON_APPDATA:id=&FOLDERID_ProgramData;break;case CSIDL_PROGRAM_FILES:id=&FOLDERID_ProgramFiles;break;case CSIDL_PROGRAM_FILESX86:id=&FOLDERID_ProgramFilesX86;break;}
        if(id){PWSTR p=nullptr;HRESULT hr=known(*id,(folder&CSIDL_FLAG_CREATE)?KF_FLAG_CREATE:0,token,&p);if(SUCCEEDED(hr)){wcscpy_s(out,MAX_PATH,p);CoTaskMemFree(p);}return hr;}
    }
    if(!hooked())return real_folder_w(hwnd,folder,token,flags,out);Guard g;HRESULT hr=real_folder_w(hwnd,folder,token,flags,out);
    if(SUCCEEDED(hr)){str mapped=prefix->virtualize(prefix->translate(out));if(mapped.size()>=MAX_PATH)return E_FAIL;wcscpy_s(out,MAX_PATH,mapped.c_str());}return hr;
}
static HRESULT WINAPI folder_a(HWND hwnd,int folder,HANDLE token,DWORD flags,LPSTR out){if(!hooked())return real_folder_a(hwnd,folder,token,flags,out);wchar_t w[MAX_PATH];HRESULT hr=folder_w(hwnd,folder,token,flags,w);if(SUCCEEDED(hr)&&!WideCharToMultiByte(CP_ACP,0,w,-1,out,MAX_PATH,nullptr,nullptr))return E_FAIL;return hr;}
// Shared synchronous emulation; unrecognized requests retain their native path.
// Call only under Guard. The NT boundary supplies its own status/access checks.
static BOOL virtual_ioctl(HANDLE h,DWORD code,LPVOID in,DWORD ilen,LPVOID out,DWORD olen,LPDWORD done,LPOVERLAPPED ov,bool &handled) {
    auto identity=media_session->path_identity(handle_path(h));int i=identity.drive;
    handled=i>=0;if(!handled)return FALSE;
    auto state=media_session->snapshot(i);
    bool stale=identity.medium&&identity.medium!=state.medium;
    if(stale&&!identity.raw){if(done)*done=0;SetLastError(state.present?ERROR_MEDIA_CHANGED:ERROR_NOT_READY);return FALSE;}
    BOOL media_result;if(media_session&&media_session->ioctl(i,code,in,ilen,out,olen,done,ov,media_result))return media_result;
    if(i>=0&&(prefix->drives[i].type==DRIVE_CDROM||prefix->drives[i].type==DRIVE_REMOVABLE)&&code==IOCTL_STORAGE_GET_DEVICE_NUMBER){
        if(done)*done=0;
        if(!out){SetLastError(ERROR_INVALID_PARAMETER);return FALSE;}
        if(olen<sizeof(STORAGE_DEVICE_NUMBER)){SetLastError(ERROR_INSUFFICIENT_BUFFER);return FALSE;}
        auto n=(STORAGE_DEVICE_NUMBER*)out;n->DeviceType=prefix->drives[i].type==DRIVE_CDROM?FILE_DEVICE_CD_ROM:FILE_DEVICE_DISK;n->DeviceNumber=i;n->PartitionNumber=(DWORD)-1;if(done)*done=sizeof(*n);return TRUE;
    }
    if(medium_absent(i)){if(done)*done=0;SetLastError(ERROR_NOT_READY);return FALSE;}
    if(stale){if(done)*done=0;SetLastError(ERROR_MEDIA_CHANGED);return FALSE;}
    DWORD generation=media_session?media_session->snapshot(i).changes:0;
    BOOL cdda_result;if(cdda_ioctl(*prefix,i,code,in,ilen,out,olen,done,ov,cdda_result,generation))return cdda_result;
    if(i>=0 && (prefix->drives[i].type==DRIVE_CDROM||prefix->drives[i].type==DRIVE_REMOVABLE)) {
        if(code==IOCTL_STORAGE_CHECK_VERIFY||code==IOCTL_STORAGE_CHECK_VERIFY2||code==IOCTL_CDROM_CHECK_VERIFY){if(done)*done=0;return TRUE;}
        if(code==IOCTL_DISK_IS_WRITABLE){if(done)*done=0;if(prefix->drives[i].readonly){SetLastError(ERROR_WRITE_PROTECT);return FALSE;}return TRUE;}
    }
    handled=false;return FALSE;
}
static BOOL WINAPI ioctl(HANDLE h,DWORD code,LPVOID in,DWORD ilen,LPVOID out,DWORD olen,LPDWORD done,LPOVERLAPPED ov) {
    if(!hooked())return real_ioctl(h,code,in,ilen,out,olen,done,ov);Guard g;
    try{bool handled=false;BOOL result=virtual_ioctl(h,code,in,ilen,out,olen,done,ov,handled);if(handled)return result;}
    catch(...){if(done)*done=0;SetLastError(ERROR_INVALID_DATA);return FALSE;}
    return real_ioctl(h,code,in,ilen,out,olen,done,ov);
}
static NTSTATUS media_error_status(DWORD error) {
    switch(error){
    case ERROR_NOT_READY:return no_media;
    case ERROR_MEDIA_CHANGED:return media_changed;
    case ERROR_INVALID_PARAMETER:return (NTSTATUS)0xc000000dL;
    case ERROR_INSUFFICIENT_BUFFER:return (NTSTATUS)0xc0000023L;
    case ERROR_NOT_SUPPORTED:return (NTSTATUS)0xc00000bbL;
    case ERROR_WRITE_PROTECT:return readonly_status;
    case ERROR_BUSY:return (NTSTATUS)0x80000011L;
    case ERROR_INVALID_DATA:return (NTSTATUS)0xc000003eL;
    case ERROR_ARITHMETIC_OVERFLOW:return (NTSTATUS)0xc0000095L;
    case ERROR_ACCESS_DENIED:return denied;
    case ERROR_INVALID_HANDLE:return (NTSTATUS)0xc0000008L;
    default:return (NTSTATUS)0xc0000001L;
    }
}
static bool nt_buffer_access(const void *buffer,size_t size,bool write) {
    if(!size)return true;
    uintptr_t at=(uintptr_t)buffer;
    if(!at||size>UINTPTR_MAX-at)return false;
    uintptr_t end=at+size;
    while(at<end){
        MEMORY_BASIC_INFORMATION region{};
        if(VirtualQuery((const void*)at,&region,sizeof(region))!=sizeof(region)||region.State!=MEM_COMMIT||
           (region.Protect&(PAGE_GUARD|PAGE_NOACCESS)))return false;
        DWORD protection=region.Protect&0xff;
        bool writable=protection==PAGE_READWRITE||protection==PAGE_WRITECOPY||
                      protection==PAGE_EXECUTE_READWRITE||protection==PAGE_EXECUTE_WRITECOPY;
        bool readable=writable||protection==PAGE_READONLY||protection==PAGE_EXECUTE_READ;
        if(write?!writable:!readable)return false;
        uintptr_t base=(uintptr_t)region.BaseAddress;
        if(region.RegionSize>UINTPTR_MAX-base)return false;
        uintptr_t next=base+region.RegionSize;if(next<=at)return false;
        at=std::min(next,end);
    }
    return true;
}
// Keep SEH in a leaf with no C++ objects; /EHsc alone does not catch bad user
// pointers. Temporary buffers keep faults out of the stateful media handlers.
static bool nt_copy_buffer(void *target,const void *source,size_t size) {
    __try {if(size)memcpy(target,source,size);return true;}
    __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
static bool virtual_ioctl_code(int drive,ULONG code) {
    // Restrict NT-specific completion/access checks to requests we implement.
    // Unknown controls, including asynchronous ones, keep the native syscall.
    switch(code){
    case IOCTL_STORAGE_CHECK_VERIFY:case IOCTL_STORAGE_CHECK_VERIFY2:case IOCTL_CDROM_CHECK_VERIFY:case IOCTL_DISK_CHECK_VERIFY:
    case IOCTL_STORAGE_EJECT_MEDIA:case IOCTL_DISK_EJECT_MEDIA:
    case IOCTL_STORAGE_LOAD_MEDIA:case IOCTL_STORAGE_LOAD_MEDIA2:case IOCTL_DISK_LOAD_MEDIA:
    case IOCTL_STORAGE_MEDIA_REMOVAL:case IOCTL_DISK_MEDIA_REMOVAL:
    case IOCTL_STORAGE_EJECTION_CONTROL:case IOCTL_STORAGE_GET_DEVICE_NUMBER:case IOCTL_DISK_IS_WRITABLE:
    case CTL_CODE(FILE_DEVICE_CD_ROM,0x0201,METHOD_BUFFERED,FILE_READ_ACCESS):
    case CTL_CODE(FILE_DEVICE_CD_ROM,0x0202,METHOD_BUFFERED,FILE_READ_ACCESS):
    case CTL_CODE(FILE_DEVICE_CD_ROM,0x0203,METHOD_BUFFERED,FILE_READ_ACCESS):return true;
    case IOCTL_CDROM_READ_TOC:case IOCTL_CDROM_RAW_READ:case IOCTL_CDROM_READ_Q_CHANNEL:
    case IOCTL_CDROM_SEEK_AUDIO_MSF:case IOCTL_CDROM_PLAY_AUDIO_MSF:
    case IOCTL_CDROM_STOP_AUDIO:case IOCTL_CDROM_PAUSE_AUDIO:case IOCTL_CDROM_RESUME_AUDIO:
        return !prefix->drives[drive].audio.empty();
    default:return false;
    }
}
static NTSTATUS NTAPI device_io(HANDLE h,HANDLE event,PIO_APC_ROUTINE apc,PVOID context,PIO_STATUS_BLOCK io,
                                ULONG code,PVOID in,ULONG ilen,PVOID out,ULONG olen) {
    if(!hooked())return real_device_io(h,event,apc,context,io,code,in,ilen,out,olen);
    // NT APIs do not expose Win32 LastError. Internal Win32 helpers must not
    // overwrite the caller's value on either success or failure.
    struct LastError {DWORD value=GetLastError();~LastError(){SetLastError(value);}} saved;
    Guard guard;
    constexpr NTSTATUS access_violation=(NTSTATUS)0xc0000005L;
    auto finish=[&](NTSTATUS status,ULONG_PTR information=0){
        IO_STATUS_BLOCK result{};result.Status=status;result.Information=information;
        return nt_copy_buffer(io,&result,sizeof(result))?status:access_violation;
    };
    try{
        auto identity=media_session->path_identity(handle_path(h));
        if(identity.drive<0||!media_session->snapshot(identity.drive).managed||!virtual_ioctl_code(identity.drive,code))
            return real_device_io(h,event,apc,context,io,code,in,ilen,out,olen);
        if(!io)return (NTSTATUS)0xc000000dL;
        if(!nt_buffer_access(io,sizeof(*io),true))return access_violation;
        if(event||apc||context)return finish((NTSTATUS)0xc00000bbL);
        ULONG mode=0,access=0;IO_STATUS_BLOCK query{};
        NTSTATUS status=real_query_file(h,&query,&mode,sizeof(mode),(FILE_INFORMATION_CLASS)16);
        if(status<0)return finish(status);
        // Async completion requires real event/APC/IOCP semantics. Reject it
        // before dispatch, including commands that mutate shared media state.
        if(!(mode&0x30))return finish((NTSTATUS)0xc00000bbL);
        status=real_query_file(h,&query,&access,sizeof(access),(FILE_INFORMATION_CLASS)8);
        if(status<0)return finish(status);
        DWORD needed=(code>>14)&3;
        if(((needed&FILE_READ_ACCESS)&&!(access&FILE_READ_DATA))||
           ((needed&FILE_WRITE_ACCESS)&&!(access&FILE_WRITE_DATA)))return finish(denied);
        if(!nt_buffer_access(in,ilen,false)||!nt_buffer_access(out,olen,true))return finish(access_violation);
        std::vector<BYTE> input(ilen),output(olen);
        if(!nt_copy_buffer(input.data(),in,ilen))return finish(access_violation);
        bool handled=false;DWORD done=0;
        BOOL result=virtual_ioctl(h,code,ilen?input.data():in,ilen,olen?output.data():out,olen,&done,nullptr,handled);
        if(!handled)return real_device_io(h,event,apc,context,io,code,in,ilen,out,olen);
        status=result?0:media_error_status(GetLastError());
        if(done>olen)return finish((NTSTATUS)0xc000003eL);
        if(!nt_copy_buffer(out,output.data(),done))return finish(access_violation);
        return finish(status,done);
    }catch(const std::bad_alloc&){return finish((NTSTATUS)0xc0000017L);}
    catch(...){return finish((NTSTATUS)0xc000003eL);}
}
static std::vector<wchar_t> child_environment(LPVOID input,DWORD flags,const str &application,const str &native_directory) {
    std::vector<str> values;
    LPWCH inherited=nullptr;
    if(!input){inherited=GetEnvironmentStringsW();if(!inherited)throw std::runtime_error("Cannot read inherited environment");input=inherited;flags|=CREATE_UNICODE_ENVIRONMENT;}
    try {
        size_t offset=0,limit=4*1024*1024/((flags&CREATE_UNICODE_ENVIRONMENT)?sizeof(wchar_t):1);
        for(;;){
            if(offset>=limit)throw std::runtime_error("Child environment exceeds bounded startup scan");
            size_t length;
            if(flags&CREATE_UNICODE_ENVIRONMENT){const wchar_t *p=(const wchar_t*)input+offset;length=wcsnlen_s(p,limit-offset);if(!length)break;if(length==limit-offset)throw std::runtime_error("Unterminated child environment");values.emplace_back(p,length);}
            else{const char *p=(const char*)input+offset;length=strnlen_s(p,limit-offset);if(!length)break;if(length==limit-offset)throw std::runtime_error("Unterminated child environment");values.emplace_back(ansi(p));}
            offset+=length+1;
        }
    }catch(...){if(inherited)FreeEnvironmentStringsW(inherited);throw;}
    if(inherited)FreeEnvironmentStringsW(inherited);
    const wchar_t *required[]={L"WINEPREFIX",L"WINEDOWS_PREFIX",L"WINEDOWS_HOST_PROFILE",L"WINEDOWS_DESKTOP_HWND",media_session_environment_name,L"USERPROFILE",L"APPDATA",L"LOCALAPPDATA",L"TEMP",L"TMP",L"ProgramData",L"ALLUSERSPROFILE",L"HOMEDRIVE",L"HOMEPATH"};
    for(auto name:required){str key=str(name)+L"=";values.erase(std::remove_if(values.begin(),values.end(),[&](const str &s){return starts(s,key);}),values.end());str value=media_session&&!_wcsicmp(name,media_session_environment_name)?media_session->name():env(name);if(!value.empty())values.push_back(key+value);}
    for(const auto &entry:{std::pair<str,str>{L"WINEDOWS_APPDIR=",wd::directory(application)},std::pair<str,str>{str(startup_native_environment_name)+L"=",native_directory}}){
        values.erase(std::remove_if(values.begin(),values.end(),[&](const str &s){return starts(s,entry.first);}),values.end());values.push_back(entry.first+entry.second);
    }
    bool has_overrides=false;for(const auto &value:values)if(starts(value,L"WINEDLLOVERRIDES="))has_overrides=true;
    if(!has_overrides&&!env(L"WINEDLLOVERRIDES").empty())values.push_back(L"WINEDLLOVERRIDES="+env(L"WINEDLLOVERRIDES"));
    // Native lookup takes the first matching environment name. Preserve that
    // order for duplicate names instead of letting the value choose a winner.
    // Drive-current-directory entries start with '=' and use the next '='.
    std::stable_sort(values.begin(),values.end(),[](const str &a,const str &b){
        size_t an=a.find(L'=',!a.empty()&&a[0]==L'='?1:0),bn=b.find(L'=',!b.empty()&&b[0]==L'='?1:0);
        if(an==str::npos)an=a.size();if(bn==str::npos)bn=b.size();
        int order=_wcsnicmp(a.c_str(),b.c_str(),(std::min)(an,bn));
        return order?order<0:an<bn;
    });
    std::vector<wchar_t> block;for(const auto &s:values){if(s.size()+block.size()+2>2*1024*1024)throw std::runtime_error("Child environment exceeds bounded startup scan");block.insert(block.end(),s.begin(),s.end());block.push_back(0);}block.push_back(0);return block;
}
static str child_executable_search_path() {
    // CreateProcess searches the parent's module directory and current directory,
    // not the requested child's current directory or child environment PATH.
    std::vector<wchar_t> current(32768),system(32768);
    DWORD current_size=GetCurrentDirectoryW((DWORD)current.size(),current.data());
    DWORD system_size=GetSystemDirectoryW(system.data(),(UINT)system.size());
    if(!current_size||current_size>=current.size()||!system_size||system_size>=system.size())
        throw std::runtime_error("Cannot resolve child executable search directories");
    str source=module_dir()+L";";
    if(NeedCurrentDirectoryForExePathW(L"winedows-child.exe"))source+=str(current.data())+L";";
    source+=str(system.data())+L";"+prefix->windows+L"\\System;"+prefix->windows+L";"+env(L"PATH");
    str search;
    for(size_t start=0;start<=source.size();) {
        size_t end=source.find(L';',start);if(end==str::npos)end=source.size();
        str path=source.substr(start,end-start);
        if(!path.empty()) {
            str translated=prefix->translate(path);
            if(!search.empty())search+=L';';search+=translated;
            if(_wcsicmp(path.c_str(),translated.c_str())){search+=L';';search+=path;}
        }
        if(end==source.size())break;start=end+1;
    }
    return search;
}
static bool find_child_executable(const str &name,const str &search,str &application,DWORD &error) {
    if(name.empty()){error=ERROR_FILE_NOT_FOUND;return false;}
    auto find=[&](const str &candidate,const wchar_t *directories) {
        std::vector<wchar_t> found(32768);
        DWORD size=SearchPathW(directories,candidate.c_str(),L".exe",(DWORD)found.size(),found.data(),nullptr);
        if(!size){error=GetLastError();return false;}
        if(size>=found.size()){error=ERROR_FILENAME_EXCED_RANGE;return false;}
        DWORD attrs=GetFileAttributesW(found.data());
        if(attrs==INVALID_FILE_ATTRIBUTES){error=GetLastError();return false;}
        if(attrs&FILE_ATTRIBUTE_DIRECTORY){error=ERROR_FILE_NOT_FOUND;return false;}
        application.assign(found.data(),size);return true;
    };
    if(name.find_first_of(L"\\/:")!=str::npos) {
        str translated=prefix->translate(name);
        if(find(translated,nullptr))return true;
        DWORD translated_error=error;
        if(_wcsicmp(name.c_str(),translated.c_str())&&find(name,nullptr))return true;
        error=translated_error;return false;
    }
    return find(name,search.c_str());
}
static bool resolve_child_command(const str &command,str &application,str &tidy,DWORD &error) {
    // Like Wine kernelbase get_file_name(), try successive whitespace boundaries.
    // A real earlier executable wins over a longer filename. Do not infer the
    // boundary from '.exe', or consume quoted arguments as part of argv[0].
    if(command.empty()||command[0]==L' '||command[0]==L'\t'){error=ERROR_INVALID_PARAMETER;return false;}
    str search=child_executable_search_path();
    if(!command.empty()&&command[0]==L'"') {
        size_t end=command.find(L'"',1);
        if(end!=str::npos){
            str name=command.substr(1,end-1);
            if(name.find_first_not_of(L" ")==str::npos){error=ERROR_INVALID_PARAMETER;return false;}
            return find_child_executable(name,search,application,error);
        }
    }
    DWORD first_error=ERROR_FILE_NOT_FOUND;bool first=true;
    for(size_t end=0;;) {
        end=command.find_first_of(L" \t",end);
        if(end==str::npos)end=command.size();
        str candidate=command.substr(0,end);
        if(find_child_executable(candidate,search,application,error)) {
            if(candidate.find_first_of(L" \t")!=str::npos)tidy=quote(candidate)+command.substr(end);
            return true;
        }
        if(first){first_error=error;first=false;}
        if(error!=ERROR_FILE_NOT_FOUND)return false;
        if(end==command.size())break;
        ++end;
    }
    error=first_error;return false;
}
static BOOL WINAPI process_w(LPCWSTR app,LPWSTR cmd,LPSECURITY_ATTRIBUTES pa,LPSECURITY_ATTRIBUTES ta,BOOL inherit,DWORD flags,LPVOID environment,LPCWSTR cwd,LPSTARTUPINFOW si,LPPROCESS_INFORMATION pi) {
    if(!hooked())return real_process_w(app,cmd,pa,ta,inherit,flags,environment,cwd,si,pi);
    Guard g;PROCESS_INFORMATION child{};try {
        if(!si||!pi){SetLastError(ERROR_INVALID_PARAMETER);return FALSE;}
        str application,directory=cwd?prefix->translate(cwd):L"",tidy;
        if(app) {
            application=prefix->translate(app);
            if(GetFileAttributesW(application.c_str())==INVALID_FILE_ATTRIBUTES)application=app;
        } else {
            DWORD error=ERROR_FILE_NOT_FOUND;
            if(!resolve_child_command(cmd?cmd:L"",application,tidy,error)){*pi={};SetLastError(error);return FALSE;}
            if(!tidy.empty())cmd=tidy.data();
        }
        WORD machine;
        try {machine=program_machine(application);}
        catch(...) {
            // File-not-found/bad-image failures retain CreateProcess error
            // categories; helper binding failures below remain DLL_INIT_FAILED.
            HANDLE file=CreateFileW(application.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr);
            DWORD error=file==INVALID_HANDLE_VALUE?GetLastError():ERROR_BAD_EXE_FORMAT;
            if(file!=INVALID_HANDLE_VALUE)CloseHandle(file);*pi={};SetLastError(error);return FALSE;
        }
        str native_directory=prefix->translate(prefix->windows+(machine==IMAGE_FILE_MACHINE_I386?L"\\SysWOW64":L"\\System32"));
        str other_directory=prefix->translate(prefix->windows+(machine==IMAGE_FILE_MACHINE_I386?L"\\System32":L"\\SysWOW64"));
        auto block=child_environment(environment,flags,application,native_directory);
        str overrides,child_path;bool have_overrides=false,have_path=false;
        for(const wchar_t *p=block.data();*p;p+=wcslen(p)+1){
            // Match GetEnvironmentVariable's first entry in the exact block
            // we pass to the child, including duplicate caller-supplied names.
            if(!have_overrides&&starts(p,L"WINEDLLOVERRIDES=")){overrides=p+17;have_overrides=true;}
            else if(!have_path&&starts(p,L"PATH=")){child_path=p+5;have_path=true;}
        }
        auto module_policy=select_module_policy(*prefix,application,runtime_dir,overrides);
        const auto &selected=module_policy.builtins;
        auto builtin_imports=plan_builtin_startup_imports(fullpath(application),machine,cwd?fullpath(directory):fullpath(L"."),native_directory,other_directory,selected,[&](const str &path){
            str native=native_custom_directory(*prefix,path,machine);return StartupDirectoryPaths{prefix->translate(native),native};
        });
        if(!builtin_imports.transitive_limit.empty())fprintf(stderr,"winedows: Selected Wine transitive import inspection unavailable: %s. Direct executable imports are still bound.\n",builtin_imports.transitive_limit.c_str());
        auto native_imports=plan_native_startup_imports(fullpath(application),machine,cwd?fullpath(directory):fullpath(L"."),native_directory,other_directory,selected,[&](const str &path){
            str native=native_custom_directory(*prefix,path,machine);
            return StartupDirectoryPaths{prefix->translate(native),native};
        });
        if(!native_imports.unsupported_reason.empty())
            fprintf(stderr,"winedows: Private startup DLL fallback unavailable: %s. Preserving the application's native search path.\n",native_imports.unsupported_reason.c_str());
        auto injected=selected;
        for(const auto &binding:native_imports.bindings)injected.push_back(binding.file.path);
        auto disabled_validation=validate_disabled_startup_imports(fullpath(application),machine,cwd?fullpath(directory):fullpath(L"."),native_directory,other_directory,injected,module_policy.disabled,
            {child_path,[&](const str &path){
                str native=native_custom_directory(*prefix,path,machine);
                return StartupDirectoryPaths{prefix->translate(native),native};
            }});
        std::vector<std::string> names{shim_path};
        for(const auto &path:injected){
            BOOL lossy=FALSE;int count=WideCharToMultiByte(CP_ACP,WC_NO_BEST_FIT_CHARS,path.c_str(),-1,nullptr,0,nullptr,&lossy);
            std::string name(count,0);WideCharToMultiByte(CP_ACP,WC_NO_BEST_FIT_CHARS,path.c_str(),-1,name.data(),count,nullptr,&lossy);
            if(lossy)throw std::runtime_error("Child DLL path cannot be represented in the ANSI code page");names.push_back(name);
        }
        std::vector<const char*> dlls;for(const auto &name:names)dlls.push_back(name.c_str());
        if(!pi){SetLastError(ERROR_INVALID_PARAMETER);return FALSE;}
        if(!DetourCreateProcessWithDllsW(application.empty()?nullptr:application.c_str(),cmd,pa,ta,inherit,flags|CREATE_UNICODE_ENVIRONMENT|CREATE_SUSPENDED,block.data(),cwd?directory.c_str():nullptr,si,&child,(DWORD)dlls.size(),dlls.data(),real_process_w))return FALSE;
        recheck_disabled_startup_imports(disabled_validation);
        bind_native_startup_imports(child.hProcess,native_imports);
        bind_builtin_startup_imports(child.hProcess,builtin_imports);
        configure_native_startup(child.hProcess,machine,native_directory,other_directory);
        if(!(flags&CREATE_SUSPENDED)&&ResumeThread(child.hThread)==(DWORD)-1)check(GetLastError(),"Resume prefixed child");
        *pi=child;return TRUE;
    }catch(const std::exception &error){
        if(child.hProcess){TerminateProcess(child.hProcess,ERROR_DLL_INIT_FAILED);CloseHandle(child.hThread);CloseHandle(child.hProcess);}
        if(pi)*pi={};fprintf(stderr,"winedows: Child startup failed: %s\n",error.what());SetLastError(ERROR_DLL_INIT_FAILED);return FALSE;
    }
}
static BOOL WINAPI process_a(LPCSTR app,LPSTR cmd,LPSECURITY_ATTRIBUTES pa,LPSECURITY_ATTRIBUTES ta,BOOL inherit,DWORD flags,LPVOID environment,LPCSTR cwd,LPSTARTUPINFOA si,LPPROCESS_INFORMATION pi) {
    if(!hooked())return real_process_a(app,cmd,pa,ta,inherit,flags,environment,cwd,si,pi);
    if(!si||!pi){SetLastError(ERROR_INVALID_PARAMETER);return FALSE;}
    str a=ansi(app),c=ansi(cmd),d=ansi(cwd),desktop=ansi(si->lpDesktop),title=ansi(si->lpTitle);
    STARTUPINFOEXW info{};memcpy(&info.StartupInfo,si,sizeof(STARTUPINFOA));info.StartupInfo.lpDesktop=si->lpDesktop?desktop.data():nullptr;info.StartupInfo.lpTitle=si->lpTitle?title.data():nullptr;
    if(flags&EXTENDED_STARTUPINFO_PRESENT)info.lpAttributeList=((STARTUPINFOEXA*)si)->lpAttributeList;
    return process_w(app?a.c_str():nullptr,cmd?c.data():nullptr,pa,ta,inherit,flags,environment,cwd?d.c_str():nullptr,&info.StartupInfo,pi);
}
static UINT WINAPI win_exec(LPCSTR command,UINT show) {
    if(!hooked())return real_win_exec(command,show);
    // Native WinExec calls an internal creation entry point which bypasses our
    // public CreateProcess hooks. Follow Wine kernel32's WinExec implementation
    // through the prefix-aware path, including the legacy <32 error convention.
    if(!command){SetLastError(ERROR_INVALID_PARAMETER);return 0;}
    try {
        std::string writable(command);STARTUPINFOA startup{};startup.cb=sizeof(startup);
        startup.dwFlags=STARTF_USESHOWWINDOW;startup.wShowWindow=(WORD)show;
        PROCESS_INFORMATION child{};
        if(!process_a(nullptr,writable.data(),nullptr,nullptr,FALSE,0,nullptr,nullptr,&startup,&child)) {
            DWORD error=GetLastError();
            // Native WinExec reports invalid command syntax as zero. Other
            // unsupported errors retain Wine's generic bad-format convention.
            return error==ERROR_INVALID_PARAMETER?0:error>=32?ERROR_BAD_FORMAT:error;
        }
        WaitForInputIdle(child.hProcess,30000);CloseHandle(child.hThread);CloseHandle(child.hProcess);return 33;
    }catch(...){SetLastError(ERROR_NOT_ENOUGH_MEMORY);return 0;}
}
static HFILE WINAPI legacy_open(LPCSTR name,LPOFSTRUCT ofs,UINT mode) {
    if(!hooked()||!ofs)return real_legacy_open(name,ofs,mode);
    DWORD incoming_error=GetLastError();std::string logical;OFSTRUCT reopened{};bool adapt=false;
    try {
        Guard guard;
        const char *source=mode&OF_REOPEN?ofs->szPathName:name;
        if(source) {
            str original=ansi(source),physical=fullpath(original);bool project=true;
            // OpenFile performs its 128-byte OFSTRUCT pathname check before
            // opening anything. A module-derived physical prefix path can be
            // too long even when the guest path is short. Preserve the native
            // API's own parser, modes, timestamps, and error behavior by passing
            // a logical name only for paths backed by a mapped prefix drive.
            if(!(mode&(OF_PARSE|OF_CREATE|OF_REOPEN))&&original.find(L':')==str::npos&&
               (original.empty()||original[0]!=L'\\')) {
                // Bare/relative names may be found beside the executable rather
                // than in CWD. Resolve that native search before projecting it.
                std::vector<wchar_t> found(32768);
                DWORD size=SearchPathW(nullptr,original.c_str(),nullptr,(DWORD)found.size(),found.data(),nullptr);
                if(size&&size<found.size())physical.assign(found.data(),size);
                else project=false; // Preserve any later hooked/native search.
            }
            // Keep image/removable-media physical identities intact across a
            // concurrent eject/swap. The fixed prefix C: tree contains installed
            // application/module paths and has no live medium generation.
            int drive=prefix->drive_index(physical);
            str projected=project&&drive>=0&&prefix->drives[drive].type==DRIVE_FIXED?prefix->virtualize(physical):physical;
            if(_wcsicmp(projected.c_str(),physical.c_str())) {
                BOOL lossy=FALSE;int size=WideCharToMultiByte(CP_ACP,WC_NO_BEST_FIT_CHARS,projected.c_str(),-1,nullptr,0,nullptr,&lossy);
                if(size>0&&!lossy) {
                    logical.resize(size);WideCharToMultiByte(CP_ACP,WC_NO_BEST_FIT_CHARS,projected.c_str(),-1,logical.data(),size,nullptr,&lossy);
                    if(!lossy) {
                        logical.resize(size-1);adapt=true;
                        if(mode&OF_REOPEN){
                            if(logical.size()>=sizeof(reopened.szPathName))adapt=false;
                            else{reopened=*ofs;memcpy(reopened.szPathName,logical.c_str(),logical.size()+1);}
                        }
                    }
                }
            }
        }
    }catch(...){adapt=false;}
    // Delegate outside Guard: the existing NT hooks must still enforce prefix
    // mapping, read-only media, and media generation checks on every operation.
    SetLastError(incoming_error);
    if(!adapt)return real_legacy_open(name,ofs,mode);
    if(mode&OF_REOPEN){HFILE file=real_legacy_open(name,&reopened,mode);*ofs=reopened;return file;}
    return real_legacy_open(logical.c_str(),ofs,mode);
}
extern "C" void WinedowsEntry(){}
template<class T> static void nt(T &p,HMODULE n,const char *name){p=(T)GetProcAddress(n,name);if(!p)throw std::runtime_error(std::string("Missing NT export: ")+name);}
template<class T,class U> static void attach(T &p,U fn){LONG e=DetourAttach((PVOID*)&p,(PVOID)fn);check(e,"Install process hook");}
BOOL WINAPI DllMain(HINSTANCE instance,DWORD reason,LPVOID) {
    if(DetourIsHelperProcess())return TRUE;
    // CRT/static destructors can close handles after user DllMain detaches.
    // Keep those calls away from process-local state that is being destroyed.
    if(reason==DLL_PROCESS_DETACH){active=false;return TRUE;}
    if(reason!=DLL_PROCESS_ATTACH)return TRUE;
    Guard g;
    try {
        str path=env(L"WINEDOWS_PREFIX");if(path.empty())return FALSE;
        DetourRestoreAfterWith();configured_prefix=new Prefix(path);configured_prefix->host_profile=env(L"WINEDOWS_HOST_PROFILE");appdir=env(L"WINEDOWS_APPDIR");
        media_session=std::make_unique<MediaSession>(*prefix,MediaSessionMode::attach);
        std::vector<wchar_t> self(32768);GetModuleFileNameW(instance,self.data(),(DWORD)self.size());runtime_dir=directory(self.data());GetModuleFileNameA(instance,shim_path,32768);
        hive_root=prefix->hive();
        if(env(L"WINEDOWS_TRACE")==L"1")trace_handle=CreateFileW((prefix->root+L"\\.winedows\\trace-"+std::to_wstring(GetCurrentProcessId())+L".log").c_str(),FILE_APPEND_DATA,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_ALWAYS,0,nullptr);
        check(RegOpenKeyExW(hive_root,L"Machine",0,KEY_ALL_ACCESS,&machine_root),"Open machine hive");
        check(RegOpenKeyExW(hive_root,L"User",0,KEY_ALL_ACCESS,&user_root),"Open user hive");
        check(RegOpenKeyExW(hive_root,L"Default",0,KEY_ALL_ACCESS,&default_root),"Open default hive");
        HANDLE token; if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token))return FALSE;
        DWORD n=0;GetTokenInformation(token,TokenUser,nullptr,0,&n);std::vector<BYTE>b(n);
        if(!GetTokenInformation(token,TokenUser,b.data(),n,&n)){CloseHandle(token);return FALSE;}CloseHandle(token);
        LPWSTR sid=nullptr;if(!ConvertSidToStringSidW(((TOKEN_USER*)b.data())->User.Sid,&sid))return FALSE;user_sid=sid;LocalFree(sid);
        HMODULE ntdll=GetModuleHandleW(L"ntdll.dll");
        nt(real_create_file,ntdll,"NtCreateFile");nt(real_open_file,ntdll,"NtOpenFile");nt(real_attr,ntdll,"NtQueryAttributesFile");nt(real_fullattr,ntdll,"NtQueryFullAttributesFile");nt(real_set_file,ntdll,"NtSetInformationFile");nt(real_query_volume,ntdll,"NtQueryVolumeInformationFile");
        nt(real_read_file,ntdll,"NtReadFile");nt(real_write_file,ntdll,"NtWriteFile");nt(real_query_file,ntdll,"NtQueryInformationFile");nt(real_flush_file,ntdll,"NtFlushBuffersFile");
        nt(real_device_io,ntdll,"NtDeviceIoControlFile");
        nt(real_query_directory,ntdll,"NtQueryDirectoryFile");nt(real_query_directory_ex,ntdll,"NtQueryDirectoryFileEx");
        nt(real_create_section,ntdll,"NtCreateSection");real_create_section_ex=(CreateSectionExFn)GetProcAddress(ntdll,"NtCreateSectionEx");
        nt(real_create_key,ntdll,"NtCreateKey");nt(real_open_key,ntdll,"NtOpenKey");nt(real_open_key_ex,ntdll,"NtOpenKeyEx");nt(real_query_key,ntdll,"NtQueryKey");nt(real_set_value,ntdll,"NtSetValueKey");nt(real_delete_value,ntdll,"NtDeleteValueKey");nt(real_delete_key,ntdll,"NtDeleteKey");
        nt(real_close,ntdll,"NtClose");
        machine_path=key_path(machine_root);user_path=key_path(user_root);default_path=key_path(default_root);
        check(DetourTransactionBegin(),"Begin hooks");check(DetourUpdateThread(GetCurrentThread()),"Update hook thread");
        attach(real_create_file,create_file);attach(real_open_file,open_file);attach(real_attr,attr);attach(real_fullattr,fullattr);attach(real_set_file,set_file);attach(real_query_volume,query_volume);
        attach(real_read_file,read_file);attach(real_write_file,write_file);attach(real_query_file,query_file);attach(real_flush_file,flush_file);
        attach(real_device_io,device_io);
        attach(real_query_directory,query_directory);attach(real_query_directory_ex,query_directory_ex);
        attach(real_create_section,create_section);if(real_create_section_ex)attach(real_create_section_ex,create_section_ex);
        attach(real_create_key,create_key);attach(real_open_key,open_key);attach(real_open_key_ex,open_key_ex);attach(real_query_key,query_key);attach(real_set_value,set_value);attach(real_delete_value,delete_value);attach(real_delete_key,delete_key);
        attach(real_drive_w,drive_w);attach(real_drive_a,drive_a);attach(real_logical,logical);attach(real_strings_w,strings_w);attach(real_strings_a,strings_a);attach(real_volume_w,volume_w);attach(real_volume_a,volume_a);
        attach(real_space_w,space_w);attach(real_space_a,space_a);attach(real_disk_w,disk_w);attach(real_disk_a,disk_a);attach(real_known,known);attach(real_folder_w,folder_w);attach(real_folder_a,folder_a);attach(real_ioctl,ioctl);
        attach(real_process_w,process_w);attach(real_process_a,process_a);attach(real_win_exec,win_exec);attach(real_legacy_open,legacy_open);
        attach_named_objects(media_session->name(),hooked);
        attach_compatibility(user_root);
        std::vector<wchar_t> executable(32768);
        DWORD executable_length=GetModuleFileNameW(nullptr,executable.data(),(DWORD)executable.size());
        if(!executable_length||executable_length>=executable.size())throw std::runtime_error("Cannot resolve executable for DLL overrides");
        attach_module_runtime(runtime_dir,read_disabled_modules(*prefix,str(executable.data(),executable_length)));
        attach_audio_backend();
        attach_desktop(prefix->root);
        attach_display();
        check(DetourTransactionCommit(),"Commit hooks");active=true;
        return TRUE;
    }catch(const std::exception &e){DetourTransactionAbort();OutputDebugStringA(e.what());return FALSE;}
}
