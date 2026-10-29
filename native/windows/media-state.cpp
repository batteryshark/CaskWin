// SPDX-License-Identifier: LGPL-2.1-or-later
#include "media-state.h"
#include "media-descriptor.h"
#include <winioctl.h>
#include <ntddcdrm.h>
#include <array>
#include <cstddef>
#include <cwctype>
#include <mutex>

namespace wd {
namespace {
constexpr DWORD magic=0x4d44574d,version=2,history_limit=127,selector_shift=25,lock_limit=0x00ffffff;
constexpr wchar_t object_prefix[]=L"Local\\Winedows.Media.";
// Deprecated CDROM aliases are absent from some current SDK headers.
constexpr DWORD cd_removal=CTL_CODE(FILE_DEVICE_CD_ROM,0x0201,METHOD_BUFFERED,FILE_READ_ACCESS);
constexpr DWORD cd_eject=CTL_CODE(FILE_DEVICE_CD_ROM,0x0202,METHOD_BUFFERED,FILE_READ_ACCESS);
constexpr DWORD cd_load=CTL_CODE(FILE_DEVICE_CD_ROM,0x0203,METHOD_BUFFERED,FILE_READ_ACCESS);
// Fixed-width, explicitly aligned layout is shared between x86 and x64. The
// low word holds presence bit 0, global lock count in bits 1-24, immutable
// descriptor ID in bits 25-31; high word is the ULONG media-change counter.
struct alignas(8) Descriptor {
    volatile LONG ready;
    DWORD drive,type,serial,sector_size,readonly;
    ULONGLONG image_size,volume_size;
    wchar_t path[512],image[512],audio[512],label[256];
};
static_assert(sizeof(Descriptor)==3624,"Media descriptor ABI changed");
struct alignas(8) Shared {
    DWORD magic,version,bytes;
    volatile LONG ready;
    ULONGLONG prefix_hash,config_hash;
    DWORD managed,ejectable,swappable,initial_count;
    volatile LONG count;
    DWORD reserved,initial[26];
    alignas(8) volatile LONG64 drives[26];
    Descriptor history[128];
};
static_assert(sizeof(Shared)==464240&&offsetof(Shared,drives)==160,"Media IPC ABI changed");
alignas(8) volatile LONG64 instance_counter=0;
struct Handle {
    HANDLE h=INVALID_HANDLE_VALUE;
    explicit Handle(HANDLE value):h(value){}
    ~Handle(){if(h!=INVALID_HANDLE_VALUE&&h)CloseHandle(h);}
};
struct Fingerprint {
    ULONGLONG value=14695981039346656037ull;
    void byte(BYTE b){value^=b;value*=1099511628211ull;}
    void integer(ULONGLONG n){for(unsigned i=0;i<8;i++){byte(BYTE(n));n>>=8;}}
    void text(const str &s,bool path=false){
        for(wchar_t c:s){if(path)c=wchar_t(towlower(c));byte(BYTE(c));byte(BYTE(c>>8));}
        byte(0);byte(0);
    }
};
ULONGLONG prefix_hash(const Prefix &p){Fingerprint f;f.text(p.root,true);return f.value;}
ULONGLONG config_hash(const Prefix &p){
    Fingerprint f;
    for(const auto &d:p.drives){
        f.text(d.path,true);f.text(d.image,true);f.text(d.audio,true);f.text(d.label);
        f.integer(d.type);f.integer(d.serial);f.integer(d.sector_size);
        f.integer(d.image_size);f.integer(d.volume_size);f.integer(d.readonly);
    }
    return f.value;
}
DWORD managed_mask(const Prefix &p){
    DWORD mask=0;
    for(int i=0;i<26;i++)if(!p.drives[i].path.empty()&&(p.drives[i].type==DRIVE_CDROM||p.drives[i].type==DRIVE_REMOVABLE))mask|=1u<<i;
    return mask;
}
str resolved_directory(const str &path){
    Handle dir(CreateFileW(path.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
                          nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,nullptr));
    if(dir.h==INVALID_HANDLE_VALUE)return {};
    BY_HANDLE_FILE_INFORMATION info{};
    if(!GetFileInformationByHandle(dir.h,&info)||!(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY))return {};
    std::vector<wchar_t> buffer(32768);
    DWORD n=GetFinalPathNameByHandleW(dir.h,buffer.data(),DWORD(buffer.size()),FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);
    if(!n||n>=buffer.size())return {};
    str result(buffer.data(),n);
    if(starts(result,L"\\\\?\\UNC\\"))result=L"\\\\"+result.substr(8);
    else if(starts(result,L"\\\\?\\"))result.erase(0,4);
    while(result.size()>3&&result.back()==L'\\')result.pop_back();
    return result;
}
DWORD ejectable_mask(const Prefix &p,DWORD managed){
    std::array<str,26> roots;
    for(int i=0;i<26;i++)if(!p.drives[i].path.empty())roots[i]=resolved_directory(p.drives[i].path);
    DWORD allowed=managed;
    for(int i=0;i<26;i++)if(managed&(1u<<i)){
        if(roots[i].empty()){allowed&=~(1u<<i);continue;}
        str configured=p.drives[i].path;while(configured.size()>3&&configured.back()==L'\\')configured.pop_back();
        // Existing handle routing uses final paths. A redirected/short-name
        // mount cannot be identified after open without per-handle metadata.
        if(_wcsicmp(configured.c_str(),roots[i].c_str())){allowed&=~(1u<<i);continue;}
        for(int j=0;j<26;j++)if(i!=j&&!p.drives[j].path.empty()){
            // An unresolved other mount may conceal an alias; fail closed.
            if(roots[j].empty()||inside(roots[i],roots[j])||inside(roots[j],roots[i])){allowed&=~(1u<<i);break;}
        }
    }
    return allowed;
}
str unique_name(){
    using RandomFn=BOOLEAN(WINAPI*)(PVOID,ULONG);
    FARPROC address=GetProcAddress(GetModuleHandleW(L"advapi32.dll"),"SystemFunction036");
    RandomFn random=nullptr;static_assert(sizeof(random)==sizeof(address));memcpy(&random,&address,sizeof(random));
    std::array<BYTE,16> bytes{};
    if(!random||!random(bytes.data(),DWORD(bytes.size())))throw std::runtime_error("Cannot generate a private media session name");
    str name=object_prefix;const wchar_t *hex=L"0123456789abcdef";
    for(BYTE b:bytes){name+=hex[b>>4];name+=hex[b&15];}
    return name;
}
bool valid_name(const str &name){
    size_t prefix=std::size(object_prefix)-1;
    if(name.size()!=prefix+32||name.compare(0,prefix,object_prefix))return false;
    for(size_t i=prefix;i<name.size();i++)if(!((name[i]>=L'0'&&name[i]<=L'9')||(name[i]>=L'a'&&name[i]<=L'f')))return false;
    return true;
}
ULONGLONG read_word(volatile LONG64 *word){return ULONGLONG(InterlockedCompareExchange64(word,0,0));}
BOOL fail(DWORD error){SetLastError(error);return FALSE;}
DWORD selector(ULONGLONG word){return (DWORD(word)>>selector_shift)&127;}
DWORD locks_of(ULONGLONG word){return (DWORD(word)>>1)&lock_limit;}
template<size_t N> void text_field(wchar_t (&out)[N],const str &value){
    if(value.size()>=N)throw std::runtime_error("Live media descriptor exceeds its bounded path/label capacity");
    memcpy(out,value.c_str(),(value.size()+1)*sizeof(wchar_t));
}
str relative(const Prefix &p,const str &path){
    if(path.empty())return {};
    if(!inside(path,p.root)||path.size()<=p.root.size())throw std::runtime_error("Live media descriptor escaped prefix storage");
    return path.substr(p.root.size()+1);
}
void write_descriptor(Descriptor &out,const Prefix &p,int drive,const Drive &d){
    out.drive=drive;out.type=d.type;out.serial=d.serial;out.sector_size=d.sector_size;out.readonly=d.readonly;
    out.image_size=d.image_size;out.volume_size=d.volume_size;
    text_field(out.path,relative(p,d.path));text_field(out.image,relative(p,d.image));text_field(out.audio,relative(p,d.audio));text_field(out.label,d.label);
    InterlockedExchange(&out.ready,1);
}
template<size_t N> str field(const wchar_t (&value)[N]){
    size_t n=wcsnlen_s(value,N);if(n==N)throw std::runtime_error("Corrupted shared media descriptor string");return str(value,n);
}
Drive read_descriptor(const Prefix &p,const Descriptor &in){
    Drive out;auto path=[&](const auto &text){str value=field(text);return value.empty()?str():p.root+L"\\"+value;};
    out.path=path(in.path);out.image=path(in.image);out.audio=path(in.audio);out.label=field(in.label);
    out.type=in.type;out.serial=in.serial;out.sector_size=in.sector_size;out.readonly=in.readonly!=0;
    out.image_size=in.image_size;out.volume_size=in.volume_size;return out;
}
}
struct MediaSession::Impl {
    HANDLE mapping=nullptr;
    Shared *shared=nullptr;
    str name;
    Prefix configured;
    bool owner=false;
    ULONGLONG cookie;
    std::mutex publication;
    explicit Impl(const Prefix &p):configured(p),cookie(ULONGLONG(InterlockedIncrement64(&instance_counter))){}
    ~Impl(){if(shared)UnmapViewOfFile(shared);if(mapping)CloseHandle(mapping);}
};
MediaSession::MediaSession(const Prefix &p,MediaSessionMode mode):impl(std::make_unique<Impl>(p)){
    impl->owner=mode==MediaSessionMode::create;
    if(mode==MediaSessionMode::create){
        for(unsigned attempt=0;attempt<4;attempt++){
            impl->name=unique_name();
            impl->mapping=CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,sizeof(Shared),impl->name.c_str());
            if(!impl->mapping)check(GetLastError(),"Create virtual media session");
            if(GetLastError()!=ERROR_ALREADY_EXISTS)break;
            CloseHandle(impl->mapping);impl->mapping=nullptr;
        }
        if(!impl->mapping)throw std::runtime_error("Cannot create a unique virtual media session");
    }else{
        impl->name=env(media_session_environment_name);
        if(!valid_name(impl->name))throw std::runtime_error("Missing or invalid virtual media session identity");
        impl->mapping=OpenFileMappingW(FILE_MAP_READ|FILE_MAP_WRITE,FALSE,impl->name.c_str());
        if(!impl->mapping)check(GetLastError(),"Open virtual media session");
    }
    impl->shared=static_cast<Shared*>(MapViewOfFile(impl->mapping,FILE_MAP_READ|FILE_MAP_WRITE,0,0,sizeof(Shared)));
    if(!impl->shared)check(GetLastError(),"Map virtual media session");
    Shared &s=*impl->shared;
    if(mode==MediaSessionMode::create){
        s.magic=magic;s.version=version;s.bytes=sizeof(s);s.prefix_hash=prefix_hash(p);s.config_hash=config_hash(p);
        s.managed=managed_mask(p);s.ejectable=ejectable_mask(p,s.managed);
        for(int i=0;i<26;i++)s.drives[i]=1;
        for(int i=0;i<26;i++)if((s.ejectable&(1u<<i))&&p.drives[i].type==DRIVE_CDROM&&!p.drives[i].image.empty()){
            try{
                validate_live_medium(p,p.drives[i]);DWORD id=DWORD(s.count)+1;
                write_descriptor(s.history[id],p,i,p.drives[i]);s.count=id;s.initial[i]=id;s.swappable|=1u<<i;
                s.drives[i]=LONG64(1ull|(ULONGLONG(id)<<selector_shift));
            }catch(const std::exception &){/* Existing unsupported mounts still keep their 0.4 eject/reload behavior. */}
        }
        s.initial_count=DWORD(s.count);
        InterlockedExchange(&s.ready,1);
        if(!SetEnvironmentVariableW(media_session_environment_name,impl->name.c_str()))check(GetLastError(),"Publish virtual media session");
    }else if(InterlockedCompareExchange(&s.ready,1,1)!=1||s.magic!=magic||s.version!=version||s.bytes!=sizeof(s)||
             s.prefix_hash!=prefix_hash(p)||s.config_hash!=config_hash(p)||s.managed!=managed_mask(p)||(s.ejectable&~s.managed)||
             (s.swappable&~s.ejectable)||s.count<0||s.count>LONG(history_limit)||s.initial_count>DWORD(s.count)){
        throw std::runtime_error("Virtual media session does not match this prefix and drive configuration");
    }
}
MediaSession::~MediaSession()=default;
const str &MediaSession::name()const{return impl->name;}
MediaState MediaSession::snapshot(int drive)const{
    MediaState out;const Shared &s=*impl->shared;
    if(drive<0||drive>=26||!(s.managed&(1u<<drive)))return out;
    ULONGLONG word=read_word(&impl->shared->drives[drive]);
    out.managed=true;out.ejectable=(s.ejectable&(1u<<drive))!=0;out.present=(word&1)!=0;
    out.changes=DWORD(word>>32);out.removal_locks=locks_of(word);out.medium=selector(word);return out;
}
bool MediaSession::any_absent()const{
    DWORD mask=impl->shared->managed;
    for(int i=0;i<26;i++)if((mask&(1u<<i))&&!(read_word(&impl->shared->drives[i])&1))return true;
    return false;
}
bool MediaSession::needs_handle_check()const{
    const Shared &s=*impl->shared;
    for(int i=0;i<26;i++)if(s.managed&(1u<<i)){
        ULONGLONG word=read_word(&impl->shared->drives[i]);
        if(!(word&1)||selector(word)!=s.initial[i])return true;
    }
    return false;
}
std::shared_ptr<const Prefix> MediaSession::view()const{
    struct Local {ULONGLONG cookie=0;std::array<DWORD,26> ids{};std::shared_ptr<const Prefix> prefix;};
    thread_local Local cached;
    const Shared &s=*impl->shared;std::array<DWORD,26> ids{};bool changed=cached.cookie!=impl->cookie;
    for(int i=0;i<26;i++)if(s.swappable&(1u<<i)){
        ids[i]=selector(read_word(&impl->shared->drives[i]));if(ids[i]!=cached.ids[i])changed=true;
    }
    if(!changed&&cached.prefix)return cached.prefix;
    auto result=std::make_shared<Prefix>(impl->configured);
    for(int i=0;i<26;i++)if(ids[i]){
        const Descriptor &d=s.history[ids[i]];
        if(InterlockedCompareExchange(&impl->shared->history[ids[i]].ready,1,1)!=1||d.drive!=DWORD(i))
            throw std::runtime_error("Incomplete shared media descriptor");
        result->drives[i]=read_descriptor(impl->configured,d);
    }
    cached.cookie=impl->cookie;cached.ids=ids;cached.prefix=result;return result;
}
MediaIdentity MediaSession::path_identity(const str &path)const{
    MediaIdentity found;const Shared &s=*impl->shared;
    found.drive=impl->configured.drive_index(path);
    if(found.drive>=0&&found.drive<26){
        const Drive &d=impl->configured.drives[found.drive];
        found.raw=!_wcsicmp(path.c_str(),impl->configured.raw_device(found.drive).c_str())||(!d.image.empty()&&!_wcsicmp(path.c_str(),d.image.c_str()));
    }
    // Every swappable descriptor is confined here. Ordinary installed-game,
    // registry and save-file handles need no descriptor-history scan.
    if(!inside(path,impl->configured.root+L"\\.winedows\\media"))return found;
    DWORD count=DWORD(InterlockedCompareExchange(&impl->shared->count,0,0));
    if(count>history_limit)throw std::runtime_error("Corrupted shared media history");
    for(DWORD id=1;id<=count;id++){
        const Descriptor &entry=s.history[id];
        if(InterlockedCompareExchange(&impl->shared->history[id].ready,1,1)!=1||entry.drive>=26)continue;
        Drive d=read_descriptor(impl->configured,entry);
        if(!_wcsicmp(path.c_str(),d.image.c_str()))return {int(entry.drive),id,true};
        if(inside(path,d.path))return {int(entry.drive),id,false};
    }
    return found;
}
DWORD MediaSession::replace(int drive,const Drive &medium){
    if(!impl->owner)throw std::runtime_error("Only the active loader may publish a live medium");
    std::lock_guard<std::mutex> guard(impl->publication);auto state=snapshot(drive);
    if(!state.managed||!state.ejectable||!state.medium)throw std::runtime_error("Live swap requires an unaliased private ISO/CUE CD drive");
    if(state.removal_locks)throw std::runtime_error("Virtual medium is locked against removal");
    validate_live_medium(impl->configured,medium);
    Shared &s=*impl->shared;DWORD count=DWORD(InterlockedCompareExchange(&s.count,0,0));
    if(count>=history_limit)throw std::runtime_error("Live media history limit reached (127 descriptors per session); restart the loader");
    for(DWORD id=1;id<=count;id++){
        Drive old=read_descriptor(impl->configured,s.history[id]);
        if(inside(old.path,medium.path)||inside(medium.path,old.path)||!_wcsicmp(old.image.c_str(),medium.image.c_str()))
            throw std::runtime_error("Live replacement must use a fresh immutable cache, not a previously published path");
    }
    DWORD id=count+1;write_descriptor(s.history[id],impl->configured,drive,medium);
    InterlockedExchange(&s.count,LONG(id));
    volatile LONG64 *word=&s.drives[drive];ULONGLONG previous=read_word(word);
    for(;;){
        if(locks_of(previous))throw std::runtime_error("Virtual medium became locked before publication");
        DWORD changes=DWORD(previous>>32)+((previous&1)?2:1);
        ULONGLONG next=(ULONGLONG(changes)<<32)|(ULONGLONG(id)<<selector_shift)|1;
        ULONGLONG observed=ULONGLONG(InterlockedCompareExchange64(word,LONG64(next),LONG64(previous)));
        if(observed==previous)return changes;
        previous=observed;
    }
}
bool MediaSession::ioctl(int drive,DWORD code,void *input,DWORD input_size,void *output,DWORD output_size,
                         DWORD *done,OVERLAPPED *overlapped,BOOL &result){
    enum Action {verify,eject,load,removal,unsupported};Action action;
    if(!snapshot(drive).managed)return false;
    switch(code){
    case IOCTL_STORAGE_CHECK_VERIFY:case IOCTL_STORAGE_CHECK_VERIFY2:case IOCTL_CDROM_CHECK_VERIFY:case IOCTL_DISK_CHECK_VERIFY:action=verify;break;
    case IOCTL_STORAGE_EJECT_MEDIA:case cd_eject:case IOCTL_DISK_EJECT_MEDIA:action=eject;break;
    case IOCTL_STORAGE_LOAD_MEDIA:case IOCTL_STORAGE_LOAD_MEDIA2:case cd_load:case IOCTL_DISK_LOAD_MEDIA:action=load;break;
    case IOCTL_STORAGE_MEDIA_REMOVAL:case cd_removal:case IOCTL_DISK_MEDIA_REMOVAL:action=removal;break;
    case IOCTL_STORAGE_EJECTION_CONTROL:action=unsupported;break;
    default:return false;
    }
    if(done)*done=0;
    if(overlapped||action==unsupported){result=fail(ERROR_NOT_SUPPORTED);return true;}
    if(action==verify){
        if(input_size||(output_size&&!output)){result=fail(ERROR_INVALID_PARAMETER);return true;}
        if(output_size&&output_size<sizeof(DWORD)){result=fail(ERROR_INSUFFICIENT_BUFFER);return true;}
        auto state=snapshot(drive);
        if(!state.present){result=fail(ERROR_NOT_READY);return true;}
        if(output_size){memcpy(output,&state.changes,sizeof(DWORD));if(done)*done=sizeof(DWORD);}
        result=TRUE;return true;
    }
    if(output_size||(action==removal?(!input||input_size<sizeof(PREVENT_MEDIA_REMOVAL)):input_size)){
        result=fail(ERROR_INVALID_PARAMETER);return true;
    }
    if((action==eject||action==load)&&!snapshot(drive).ejectable){result=fail(ERROR_NOT_SUPPORTED);return true;}
    volatile LONG64 *word=&impl->shared->drives[drive];ULONGLONG previous=read_word(word);
    for(;;){
        bool present=(previous&1)!=0;DWORD locks=locks_of(previous),changes=DWORD(previous>>32),medium=selector(previous);
        ULONGLONG next=previous;
        if(action==removal){
            if(static_cast<PREVENT_MEDIA_REMOVAL*>(input)->PreventMediaRemoval){
                if(locks==lock_limit){result=fail(ERROR_ARITHMETIC_OVERFLOW);return true;}++locks;
            }else if(locks)--locks;
            next=(ULONGLONG(changes)<<32)|(ULONGLONG(medium)<<selector_shift)|(ULONGLONG(locks)<<1)|DWORD(present);
        }else if(present!=(action==load)){
            if(action==eject&&locks){result=fail(ERROR_BUSY);return true;}
            ++changes;next=(ULONGLONG(changes)<<32)|(ULONGLONG(medium)<<selector_shift)|(ULONGLONG(locks)<<1)|DWORD(action==load);
        }
        if(next==previous){result=TRUE;return true;}
        ULONGLONG observed=ULONGLONG(InterlockedCompareExchange64(word,LONG64(next),LONG64(previous)));
        if(observed==previous){result=TRUE;return true;}previous=observed;
    }
}
}
