// SPDX-License-Identifier: LGPL-2.1-or-later
// The ISO serial calculation is adapted from Wine mountmgr.sys/device.c,
// Copyright 2006 Alexandre Julliard (LGPL-2.1-or-later).
#include "media.h"
#include <array>
#include <set>
#include <memory>
#include <cwctype>

namespace wd {
namespace {
constexpr DWORD sector = 2048;
[[noreturn]] void invalid(const char *reason) {
    throw std::runtime_error(std::string("Unsupported or invalid ISO image: ")+reason);
}
struct Handle {
    HANDLE h = INVALID_HANDLE_VALUE;
    explicit Handle(HANDLE value):h(value) { if(h==INVALID_HANDLE_VALUE)check(GetLastError(),"Open media file"); }
    ~Handle(){if(h!=INVALID_HANDLE_VALUE)CloseHandle(h);}
    Handle(const Handle&)=delete;
    Handle& operator=(const Handle&)=delete;
};
str extended(const str &s) { return starts(s,L"\\\\")?L"\\\\?\\UNC\\"+s.substr(2):L"\\\\?\\"+s; }
void read_at(HANDLE h,ULONGLONG at,void *data,DWORD size) {
    LARGE_INTEGER p;p.QuadPart=at;
    if(!SetFilePointerEx(h,p,nullptr,FILE_BEGIN))check(GetLastError(),"Seek image");
    DWORD got=0;if(!ReadFile(h,data,size,&got,nullptr))check(GetLastError(),"Read image");
    if(got!=size)invalid("truncated extent");
}
DWORD both32(const BYTE *b) {
    DWORD le=DWORD(b[0])|(DWORD(b[1])<<8)|(DWORD(b[2])<<16)|(DWORD(b[3])<<24);
    DWORD be=DWORD(b[7])|(DWORD(b[6])<<8)|(DWORD(b[5])<<16)|(DWORD(b[4])<<24);
    if(le!=be)invalid("mismatched little/big endian field");
    return le;
}
WORD both16(const BYTE *b) {
    WORD le=WORD(b[0])|(WORD(b[1])<<8),be=WORD(b[3])|(WORD(b[2])<<8);
    if(le!=be)invalid("mismatched little/big endian field");
    return le;
}
str decode(const BYTE *p,size_t n,bool joliet) {
    str s;
    if(joliet) {
        if(n%2)invalid("odd-length Joliet name");
        for(size_t i=0;i<n;i+=2) {
            wchar_t c=wchar_t((WORD(p[i])<<8)|p[i+1]);
            if(!c || (c>=0xd800&&c<=0xdfff))invalid("invalid Joliet UCS-2 character");
            s+=c;
        }
    } else for(size_t i=0;i<n;i++) {
        if(!p[i]||p[i]>0x7f)invalid("non-ASCII primary identifier (use Joliet)");
        s+=wchar_t(p[i]);
    }
    return s;
}
str filename(const BYTE *p,size_t n,bool joliet) {
    str s=decode(p,n,joliet);
    size_t ver=s.find_last_of(L';');
    if(ver!=str::npos) {
        if(ver+1==s.size())invalid("empty file version");
        for(size_t i=ver+1;i<s.size();i++)if(s[i]<L'0'||s[i]>L'9')invalid("invalid file version");
        s.resize(ver);if(!s.empty()&&s.back()==L'.')s.pop_back();
    }
    if(s.empty()||s==L"."||s==L".."||s.size()>255||s.back()==L'.'||s.back()==L' ')invalid("unsafe filename");
    for(wchar_t c:s)if(c<32||str(L"\\/:*?\"<>|;").find(c)!=str::npos)invalid("unsafe filename character");
    str base=s.substr(0,s.find(L'.'));for(auto &c:base)c=wchar_t(towupper(c));
    if(base==L"CON"||base==L"PRN"||base==L"AUX"||base==L"NUL"||base==L"CONIN$"||base==L"CONOUT$"||
       (base.size()==4&&(base.substr(0,3)==L"COM"||base.substr(0,3)==L"LPT")&&base[3]>=L'1'&&base[3]<=L'9'))invalid("reserved Windows filename");
    return s;
}
struct NameLess {
    bool operator()(const str &a,const str &b) const {
        return CompareStringOrdinal(a.c_str(),(int)a.size(),b.c_str(),(int)b.size(),TRUE)==CSTR_LESS_THAN;
    }
};
FILETIME recording_time(const BYTE *p) {
    // ECMA-119 9.1.5: local calendar fields and a signed 15-minute UTC offset.
    // An all-zero record means unspecified. Zero FILETIME means "leave alone"
    // to SetFileTime, so use a deterministic Unix epoch for that case instead
    // of making the private copy appear newly authored at mount time.
    // https://www.ecma-international.org/wp-content/uploads/ECMA-119_4th_edition_june_2019.pdf
    bool specified=false;for(unsigned i=0;i<7;i++)specified|=p[i]!=0;
    SYSTEMTIME time{};int offset=0;
    if(!specified){time.wYear=1970;time.wMonth=1;time.wDay=1;}
    else {
        time.wYear=WORD(1900+p[0]);time.wMonth=p[1];time.wDay=p[2];
        time.wHour=p[3];time.wMinute=p[4];time.wSecond=p[5];
        offset=p[6]<128?int(p[6]):int(p[6])-256;
        if(offset < -48||offset > 52)invalid("recording time has an invalid UTC offset");
    }
    FILETIME value{};
    // Native calendar validation rejects impossible dates rather than silently
    // normalizing February 30 or out-of-range clock fields.
    if(!SystemTimeToFileTime(&time,&value))invalid("invalid recording date or time");
    ULARGE_INTEGER ticks{};ticks.LowPart=value.dwLowDateTime;ticks.HighPart=value.dwHighDateTime;
    ticks.QuadPart=ULONGLONG(LONGLONG(ticks.QuadPart)-LONGLONG(offset)*15*60*10000000);
    value.dwLowDateTime=ticks.LowPart;value.dwHighDateTime=ticks.HighPart;return value;
}
struct Entry { str name;DWORD block=0,size=0;bool dir=false,hidden=false;FILETIME time{}; };
class Iso {
    HANDLE source;
    ULONGLONG image_bytes,volume_bytes=0,extracted_bytes=0,metadata_bytes=0;
    static constexpr ULONGLONG metadata_limit=64*1024*1024;
    bool joliet=false;
    std::set<DWORD> directories;
    Entry record(const BYTE *p,size_t available) const {
        if(available<34||p[0]<34||p[0]>available||p[32]==0||33u+p[32]>p[0])invalid("invalid directory record");
        if(p[1])invalid("extended attribute records are not supported");
        if(p[25]&0x80)invalid("multi-extent files are not supported");
        if(p[26]||p[27])invalid("interleaved files are not supported");
        if(both16(p+28)!=1)invalid("multiple-volume sets are not supported");
        Entry e;e.block=both32(p+2);e.size=both32(p+10);e.dir=(p[25]&2)!=0;e.hidden=(p[25]&1)!=0;e.time=recording_time(p+18);
        ULONGLONG start=ULONGLONG(e.block)*sector;
        if(start>volume_bytes||e.size>volume_bytes-start||start>image_bytes||e.size>image_bytes-start)invalid("extent outside volume");
        if(e.dir&&(!e.size||e.size%sector))invalid("unaligned directory extent");
        return e;
    }
    void account_metadata(ULONGLONG bytes) {
        if(bytes>metadata_limit-metadata_bytes)invalid("aggregate filesystem metadata exceeds 64 MiB limit");
        metadata_bytes+=bytes;
    }
    void scan(Entry parent,unsigned depth) {
        if(depth>64||parent.size>16*1024*1024)invalid("directory exceeds parser limits");
        if(!directories.insert(parent.block).second)invalid("cyclic or aliased directory extent");
        account_metadata(parent.size);
        std::vector<size_t> children;
        {
        std::vector<BYTE> bytes(parent.size);read_at(source,ULONGLONG(parent.block)*sector,bytes.data(),parent.size);
        std::set<str,NameLess> names;
        for(size_t at=0;at<bytes.size();) {
            const BYTE *p=bytes.data()+at;
            if(!*p){at=(at/sector+1)*sector;continue;}
            size_t remaining=std::min(bytes.size()-at,size_t(sector-at%sector));
            Entry e=record(p,remaining);
            if(p[32]==1&&(p[33]==0||p[33]==1)) {
                if(!e.dir)invalid("non-directory dot entry");
                at+=p[0];continue;
            }
            str name=filename(p+33,p[32],joliet);
            account_metadata(sizeof(str)+64+(name.size()+1)*sizeof(wchar_t));
            if(!names.insert(name).second)invalid("case-insensitive filename collision");
            e.name=parent.name.empty()?name:parent.name+L"\\"+name;
            if(e.name.size()>30000||entries.size()>=100000)invalid("filesystem exceeds parser limits");
            account_metadata(sizeof(Entry)+(e.name.size()+1)*sizeof(wchar_t));
            if(!e.dir){extracted_bytes+=e.size;if(extracted_bytes>volume_bytes)invalid("extracted files exceed volume size");}
            if(e.dir)children.push_back(entries.size());
            entries.push_back(std::move(e));at+=p[0];
        }
        }
        // Release the parent directory and sibling-name index before descent.
        // Pass each child by value because recursion can reallocate entries.
        for(size_t child:children)scan(entries[child],depth+1);
    }
public:
    std::vector<Entry> entries;
    Entry root_entry;
    str label;
    DWORD serial=0;
    ULONGLONG size() const {return volume_bytes;}
    Iso(HANDLE file,ULONGLONG length):source(file),image_bytes(length) {
        if(length<18*sector||length%sector)invalid("expected 2048-byte sectors in a plain ISO file");
        std::array<BYTE,sector> primary{},selected{};bool have_primary=false,terminator=false;int level=0;
        for(DWORD block=16;block<272&&ULONGLONG(block+1)*sector<=length;block++) {
            std::array<BYTE,sector> b{};read_at(source,ULONGLONG(block)*sector,b.data(),sector);
            if(memcmp(b.data()+1,"CD001",5)||b[6]!=1)invalid("missing ISO9660 volume descriptor signature");
            if(b[0]==255){terminator=true;break;}
            if(b[0]==1&&!have_primary){primary=b;have_primary=true;}
            if(b[0]==2&&b[88]=='%'&&b[89]=='/'&&(b[90]=='@'||b[90]=='C'||b[90]=='E')) {
                int candidate=b[90]=='@'?1:b[90]=='C'?2:3;
                if(candidate>level){selected=b;level=candidate;}
            }
        }
        if(!have_primary||!terminator)invalid("primary descriptor or descriptor terminator missing");
        if(!level)selected=primary;else joliet=true;
        for(const BYTE *p:{primary.data(),selected.data()}) {
            if(both16(p+120)!=1||both16(p+124)!=1)invalid("multiple-volume sets are not supported");
            if(both16(p+128)!=sector)invalid("logical block size must be 2048 bytes");
            ULONGLONG n=ULONGLONG(both32(p+80))*sector;
            if(n>length||n<18*sector)invalid("invalid declared volume size");
            if(volume_bytes&&volume_bytes!=n)invalid("inconsistent supplementary volume size");
            volume_bytes=n;
        }
        label=decode(selected.data()+40,32,joliet);
        for(wchar_t c:label)if(c<32)invalid("invalid volume label");
        while(!label.empty()&&label.back()==L' ')label.pop_back();
        // Wine's NT-family ISO serial: four independent wrapping byte sums.
        BYTE sum[4]={};for(size_t i=0;i<sector;i++)sum[i%4]=BYTE(sum[i%4]+selected[i]);
        serial=(DWORD(sum[0])<<24)|(DWORD(sum[1])<<16)|(DWORD(sum[2])<<8)|sum[3];
        Entry root=record(selected.data()+156,sector-156);
        if(!root.dir)invalid("root record is not a directory");
        root_entry=root;
        scan(root,0);
    }
};
// Only touch newly created media directories; never follow a junction supplied
// in an existing prefix. Mapping publication happens after this function returns.
void storage_dir(const str &path) {
    DWORD a=GetFileAttributesW(extended(path).c_str());
    if(a==INVALID_FILE_ATTRIBUTES) {
        if(!CreateDirectoryW(extended(path).c_str(),nullptr))check(GetLastError(),"Create media storage");
        a=GetFileAttributesW(extended(path).c_str());
    }
    if(a==INVALID_FILE_ATTRIBUTES||!(a&FILE_ATTRIBUTE_DIRECTORY)||(a&FILE_ATTRIBUTE_REPARSE_POINT))
        throw std::runtime_error("Media storage must be a real directory, not a reparse point");
}
void copy_extent(HANDLE source,ULONGLONG at,ULONGLONG count,const str &path) {
    Handle out(CreateFileW(extended(path).c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));
    std::vector<BYTE> buffer(1024*1024);
    while(count) {
        DWORD n=DWORD(std::min(count,ULONGLONG(buffer.size())));read_at(source,at,buffer.data(),n);
        DWORD written=0;if(!WriteFile(out.h,buffer.data(),n,&written,nullptr))check(GetLastError(),"Extract media file");
        if(written!=n)throw std::runtime_error("Incomplete media file write");
        at+=n;count-=n;
    }
    if(!FlushFileBuffers(out.h))check(GetLastError(),"Flush media file");
}
void apply_metadata(const str &path,const Entry &entry) {
    {
        Handle file(CreateFileW(extended(path).c_str(),FILE_WRITE_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
                                nullptr,OPEN_EXISTING,entry.dir?FILE_FLAG_BACKUP_SEMANTICS:0,nullptr));
        // ISO has one recording timestamp; initialize the three native fields
        // from it. Read access subsequently follows the host filesystem's policy.
        if(!SetFileTime(file.h,&entry.time,&entry.time,&entry.time))check(GetLastError(),"Preserve media recording time");
    }
    DWORD attributes=(entry.dir?FILE_ATTRIBUTE_DIRECTORY:FILE_ATTRIBUTE_READONLY)|(entry.hidden?FILE_ATTRIBUTE_HIDDEN:0);
    if(!SetFileAttributesW(extended(path).c_str(),attributes))check(GetLastError(),"Preserve media attributes");
}
}
MountedImage prepare_iso(const str &prefix_root,const str &source_path) {
    str path=fullpath(source_path);
    if(starts(path,L"\\\\.\\")||starts(path,L"\\\\?\\")||starts(path,L"\\??\\"))
        throw std::runtime_error("Image source must be an ordinary file, not a device path");
    Handle source(CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_RANDOM_ACCESS,nullptr));
    BY_HANDLE_FILE_INFORMATION info{};if(!GetFileInformationByHandle(source.h,&info))check(GetLastError(),"Inspect image");
    if((info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)||GetFileType(source.h)!=FILE_TYPE_DISK)throw std::runtime_error("Image source must be a regular file");
    ULONGLONG length=(ULONGLONG(info.nFileSizeHigh)<<32)|info.nFileSizeLow;
    Iso iso(source.h,length); // Validate every name and extent before writing anything.
    storage_dir(prefix_root);storage_dir(prefix_root+L"\\.winedows");storage_dir(prefix_root+L"\\.winedows\\media");
    str staging;
    for(unsigned attempt=0;attempt<100;attempt++) {
        staging=prefix_root+L"\\.winedows\\media\\iso-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64())+L"-"+std::to_wstring(attempt);
        if(CreateDirectoryW(extended(staging).c_str(),nullptr))break;
        if(GetLastError()!=ERROR_ALREADY_EXISTS||attempt==99)check(GetLastError(),"Create unique media cache");
    }
    // No recursive removal is needed on failure: only this private, unpublished
    // directory can contain partial data. Original images are always read-only.
    MountedImage result;result.path=staging+L"\\files";result.image=staging+L"\\disc.iso";
    result.label=iso.label;result.serial=iso.serial;result.volume_size=iso.size();result.image_size=length;
    storage_dir(result.path);copy_extent(source.h,0,length,result.image);
    if(!SetFileAttributesW(extended(result.image).c_str(),FILE_ATTRIBUTE_READONLY))check(GetLastError(),"Protect image snapshot");
    for(const auto &e:iso.entries) {
        str target=result.path+L"\\"+e.name;
        if(e.dir)storage_dir(target);
        else {
            copy_extent(source.h,ULONGLONG(e.block)*sector,e.size,target);
            apply_metadata(target,e);
        }
    }
    // Creating descendants changes directory times, so restore these only after
    // all contents exist. The logical root has its own ISO directory record.
    for(auto i=iso.entries.rbegin();i!=iso.entries.rend();++i)if(i->dir)apply_metadata(result.path+L"\\"+i->name,*i);
    apply_metadata(result.path,iso.root_entry);
    return result;
}
}
