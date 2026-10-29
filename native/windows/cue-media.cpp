// SPDX-License-Identifier: LGPL-2.1-or-later
#include "cue-media.h"
#include <array>
#include <sstream>
#include <memory>
#include <cwctype>

namespace wd {
namespace {
constexpr DWORD raw_sector=2352, data_sector=2048, msf_limit=256*60*75-150;
struct Handle {
    HANDLE h=INVALID_HANDLE_VALUE;
    explicit Handle(HANDLE value):h(value){if(h==INVALID_HANDLE_VALUE)check(GetLastError(),"Open CUE media");}
    ~Handle(){if(h!=INVALID_HANDLE_VALUE)CloseHandle(h);}
    Handle(const Handle&)=delete;Handle& operator=(const Handle&)=delete;
};
[[noreturn]] void invalid(const char *reason){throw std::runtime_error(std::string("Unsupported or invalid CUE medium: ")+reason);}
std::string upper(std::string value){for(char &c:value)if(c>='a'&&c<='z')c-=32;return value;}
void read_at(HANDLE h,ULONGLONG offset,void *out,DWORD size) {
    LARGE_INTEGER at;at.QuadPart=offset;
    if(!SetFilePointerEx(h,at,nullptr,FILE_BEGIN))check(GetLastError(),"Seek BIN");
    DWORD got=0;if(!ReadFile(h,out,size,&got,nullptr))check(GetLastError(),"Read BIN");
    if(got!=size)invalid("truncated BIN extent");
}
ULONGLONG file_size(HANDLE h) {
    BY_HANDLE_FILE_INFORMATION info{};
    if(!GetFileInformationByHandle(h,&info))check(GetLastError(),"Inspect media source");
    if((info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)||GetFileType(h)!=FILE_TYPE_DISK)invalid("source must be a regular file");
    return (ULONGLONG(info.nFileSizeHigh)<<32)|info.nFileSizeLow;
}
std::vector<std::string> words(const std::string &line) {
    std::vector<std::string> out;size_t at=0;
    while(at<line.size()) {
        while(at<line.size()&&(line[at]==' '||line[at]=='\t'||line[at]=='\r'))++at;
        if(at==line.size())break;
        std::string word;
        if(line[at]=='"') {
            size_t end=line.find('"',++at);if(end==line.npos)invalid("unterminated quoted string");
            word=line.substr(at,end-at);at=end+1;
            if(at<line.size()&&line[at]!=' '&&line[at]!='\t'&&line[at]!='\r')invalid("missing whitespace after quoted string");
        } else {
            size_t end=line.find_first_of(" \t\r",at);if(end==line.npos)end=line.size();
            word=line.substr(at,end-at);at=end;
        }
        out.push_back(std::move(word));if(out.size()>32)invalid("too many fields on CUE line");
    }
    return out;
}
DWORD decimal(const std::string &s) {
    if(s.size()!=2||s[0]<'0'||s[0]>'9'||s[1]<'0'||s[1]>'9')invalid("expected two-digit track/index number");
    return DWORD((s[0]-'0')*10+s[1]-'0');
}
DWORD msf(const std::string &s) {
    if(s.size()!=8||s[2]!=':'||s[5]!=':')invalid("expected MM:SS:FF address");
    DWORD m=decimal(s.substr(0,2)),sec=decimal(s.substr(3,2)),frame=decimal(s.substr(6,2));
    if(sec>=60||frame>=75)invalid("invalid MSF address");
    return (m*60+sec)*75+frame;
}
str bin_name(const std::string &word) {
    str name=widen(word);
    if(name.empty()||name.size()>255||name==L"."||name==L".."||name.back()==L'.'||name.back()==L' ')
        invalid("unsafe BIN filename");
    for(wchar_t c:name)if(c<32||str(L"\\/:*?\"<>|").find(c)!=str::npos)invalid("BIN must be a local basename without traversal");
    if(name.size()<5||_wcsicmp(name.c_str()+name.size()-4,L".bin"))invalid("BINARY source must have a .bin filename");
    str base=name.substr(0,name.find(L'.'));for(auto &c:base)c=wchar_t(towupper(c));
    if(base==L"CON"||base==L"PRN"||base==L"AUX"||base==L"NUL"||base==L"CONIN$"||base==L"CONOUT$"||
       (base.size()==4&&(base.substr(0,3)==L"COM"||base.substr(0,3)==L"LPT")&&base[3]>=L'1'&&base[3]<=L'9'))invalid("reserved BIN filename");
    return name;
}
struct Track {
    DWORD number=0,index0=0,index1=0,gap=0,source_begin=0,source_end=0,extent=0,start=0,sectors=0;
    bool have0=false,have1=false,have_gap=false;
};
struct Sheet {str file;std::vector<Track> tracks;};
Sheet parse(std::string text) {
    if(text.size()>=3&&(BYTE)text[0]==0xef&&(BYTE)text[1]==0xbb&&(BYTE)text[2]==0xbf)text.erase(0,3);
    for(unsigned char c:text)if(!c||(c<32&&c!='\n'&&c!='\r'&&c!='\t'))invalid("CUE must be UTF-8/ASCII text");
    Sheet sheet;std::istringstream input(text);std::string line;
    while(std::getline(input,line)) {
        if(line.size()>8192)invalid("oversized CUE line");
        size_t first=line.find_first_not_of(" \t\r");if(first==line.npos)continue;
        size_t end=line.find_first_of(" \t\r",first);
        if(upper(line.substr(first,end==line.npos?line.npos:end-first))=="REM")continue;
        auto tokens=words(line);if(tokens.empty())continue;
        std::string command=upper(tokens[0]);
        if(command=="TITLE"||command=="PERFORMER"||command=="SONGWRITER"||command=="CATALOG"||command=="ISRC") {
            if(tokens.size()!=2)invalid("invalid metadata line");
            continue; // Text metadata does not change the disc layout.
        }
        if(command=="FILE") {
            if(!sheet.file.empty()||!sheet.tracks.empty())invalid("multiple FILE sections are not supported");
            if(tokens.size()!=3||upper(tokens[2])!="BINARY")invalid("only FILE basename.bin BINARY is supported");
            sheet.file=bin_name(tokens[1]);continue;
        }
        if(command=="TRACK") {
            if(sheet.file.empty()||tokens.size()!=3)invalid("TRACK requires FILE and a mode");
            DWORD n=decimal(tokens[1]);if(n!=sheet.tracks.size()+1||n>99)invalid("tracks must be consecutive starting at 01");
            if(upper(tokens[2])!=(n==1?"MODE1/2352":"AUDIO"))invalid("only initial MODE1/2352 followed by AUDIO is supported");
            Track track;track.number=n;sheet.tracks.push_back(track);continue;
        }
        if(sheet.tracks.empty())invalid("track command without TRACK");
        Track &track=sheet.tracks.back();
        if(command=="INDEX") {
            if(tokens.size()!=3)invalid("INDEX requires number and address");
            DWORD index=decimal(tokens[1]),address=msf(tokens[2]);
            if(index==0) {
                if(track.number==1||track.have0||track.have1)invalid("unsupported or duplicate INDEX 00");
                track.index0=address;track.have0=true;
            } else if(index==1) {
                if(track.have1)invalid("duplicate INDEX 01");
                track.index1=address;track.have1=true;
            } else invalid("only INDEX 00 and INDEX 01 are supported");
        } else if(command=="PREGAP") {
            if(tokens.size()!=2||track.number==1||track.have_gap||track.have0||track.have1)invalid("unsupported or misplaced PREGAP");
            track.gap=msf(tokens[1]);track.have_gap=true;
        } else invalid("unsupported CUE command (including FLAGS/POSTGAP)");
    }
    if(sheet.tracks.size()<2)invalid("expected a data track and at least one audio track");
    for(auto &t:sheet.tracks) {
        if(!t.have1)invalid("every track requires INDEX 01");
        if(t.number==1&&t.index1!=0)invalid("first data INDEX 01 must be 00:00:00");
        if(t.have0&&t.index0>t.index1)invalid("INDEX 00 occurs after INDEX 01");
        t.source_begin=t.have0?t.index0:t.index1;
        if(t.number>1&&t.source_begin<=sheet.tracks[t.number-2].index1)invalid("overlapping or empty track extents");
    }
    return sheet;
}
void storage(const str &path) {
    DWORD attr=GetFileAttributesW(path.c_str());
    if(attr==INVALID_FILE_ATTRIBUTES){if(!CreateDirectoryW(path.c_str(),nullptr))check(GetLastError(),"Create CUE storage");attr=GetFileAttributesW(path.c_str());}
    if(attr==INVALID_FILE_ATTRIBUTES||!(attr&FILE_ATTRIBUTE_DIRECTORY)||(attr&FILE_ATTRIBUTE_REPARSE_POINT))invalid("private storage must be a real directory");
}
void write(HANDLE h,const void *data,DWORD size) {
    DWORD written=0;if(!WriteFile(h,data,size,&written,nullptr))check(GetLastError(),"Write prepared CUE medium");
    if(written!=size)invalid("incomplete private media write");
}
void copy(HANDLE source,ULONGLONG at,ULONGLONG size,HANDLE target) {
    std::vector<BYTE> bytes(1024*1024);
    while(size){DWORD n=DWORD(std::min(size,ULONGLONG(bytes.size())));read_at(source,at,bytes.data(),n);write(target,bytes.data(),n);at+=n;size-=n;}
}
void flush(HANDLE h){if(!FlushFileBuffers(h))check(GetLastError(),"Flush prepared CUE medium");}
void protect(const str &path){if(!SetFileAttributesW(path.c_str(),FILE_ATTRIBUTE_READONLY))check(GetLastError(),"Protect CUE medium");}
Handle output(const str &path){return Handle(CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));}
}
MountedCue prepare_cue(const str &prefix_root,const str &source_arg) {
    str source_path=fullpath(source_arg);
    if(starts(source_path,L"\\\\.\\")||starts(source_path,L"\\\\?\\")||starts(source_path,L"\\??\\"))invalid("source must be an ordinary CUE file");
    Handle source(CreateFileW(source_path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr));
    ULONGLONG cue_size=file_size(source.h);if(!cue_size||cue_size>1024*1024)invalid("CUE text exceeds 1 MiB limit");
    std::string text(size_t(cue_size),0);read_at(source.h,0,text.data(),DWORD(text.size()));Sheet sheet=parse(text);
    str bin_path=directory(source_path)+L"\\"+sheet.file;
    DWORD attrs=GetFileAttributesW(bin_path.c_str());if(attrs==INVALID_FILE_ATTRIBUTES||(attrs&FILE_ATTRIBUTE_REPARSE_POINT))invalid("BIN is missing or is a reparse point");
    Handle bin(CreateFileW(bin_path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_RANDOM_ACCESS,nullptr));
    ULONGLONG length=file_size(bin.h);if(!length||length%raw_sector||length/raw_sector>=msf_limit)invalid("BIN must contain bounded complete 2352-byte sectors");
    DWORD bin_sectors=DWORD(length/raw_sector),inserted=0;
    for(size_t n=0;n<sheet.tracks.size();n++) {
        auto &t=sheet.tracks[n];t.source_end=n+1<sheet.tracks.size()?sheet.tracks[n+1].source_begin:bin_sectors;
        if(t.source_begin>bin_sectors||t.index1>=t.source_end||t.source_end>bin_sectors)invalid("track index outside BIN or empty track");
        if(t.gap>=msf_limit-inserted)invalid("pregaps exceed MSF duration");
        t.extent=t.source_begin+inserted;inserted+=t.gap;t.start=t.index1+inserted;
        t.sectors=t.gap+t.source_end-t.source_begin;
        if(ULONGLONG(t.extent)+t.sectors>=msf_limit)invalid("disc exceeds representable MSF duration");
    }
    // Validate MODE1 sector headers before preparing any storage. The supported
    // adapter reads the 2048-byte data payload; it does not repair EDC/ECC.
    std::array<BYTE,raw_sector> sector{};
    for(DWORD n=0;n<sheet.tracks[0].sectors;n++) {
        read_at(bin.h,ULONGLONG(n)*raw_sector,sector.data(),raw_sector);
        if(sector[0]||sector[11]||sector[15]!=1)invalid("invalid MODE1/2352 sector header");
        for(unsigned b=1;b<11;b++)if(sector[b]!=0xff)invalid("invalid MODE1/2352 synchronization bytes");
    }
    storage(prefix_root);storage(prefix_root+L"\\.winedows");storage(prefix_root+L"\\.winedows\\media");
    str cache;
    for(unsigned attempt=0;attempt<100;attempt++) {
        cache=prefix_root+L"\\.winedows\\media\\cue-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64())+L"-"+std::to_wstring(attempt);
        if(CreateDirectoryW(cache.c_str(),nullptr))break;
        if(GetLastError()!=ERROR_ALREADY_EXISTS||attempt==99)check(GetLastError(),"Create unique CUE cache");
    }
    MountedCue result;result.tracks=DWORD(sheet.tracks.size());result.bin=cache+L"\\"+sheet.file;result.cue=cache+L"\\source.cue";result.audio=cache+L"\\disc.ini";
    {auto out=output(result.bin);copy(bin.h,0,length,out.h);flush(out.h);}protect(result.bin);
    {auto out=output(result.cue);write(out.h,text.data(),DWORD(text.size()));flush(out.h);}protect(result.cue);
    str cooked=cache+L"\\data.iso";
    {
        auto out=output(cooked);
        for(DWORD n=0;n<sheet.tracks[0].sectors;n++) {
            read_at(bin.h,ULONGLONG(n)*raw_sector,sector.data(),raw_sector);write(out.h,sector.data()+16,data_sector);
        }
        flush(out.h);
    }
    protect(cooked);result.data=prepare_iso(prefix_root,cooked);
    std::ostringstream manifest;manifest<<"[disc]\r\nversion=2\r\ndata_source=cue\r\ntracks="<<sheet.tracks.size()<<"\r\n";
    for(const auto &t:sheet.tracks) {
        wchar_t track_name[32];swprintf_s(track_name,L"track%02lu",t.number);
        manifest<<'['<<utf8(track_name)<<"]\r\ntype="<<(t.number==1?"data":"audio")<<"\r\nstart="<<t.start<<"\r\nextent_start="<<t.extent<<"\r\nsectors="<<t.sectors<<"\r\n";
        if(t.number>1) {
            str name=str(track_name)+L".pcm",path=cache+L"\\"+name;
            {auto out=output(path);std::array<BYTE,raw_sector> silence{};
                for(DWORD n=0;n<t.gap;n++)write(out.h,silence.data(),raw_sector);
                copy(bin.h,ULONGLONG(t.source_begin)*raw_sector,ULONGLONG(t.source_end-t.source_begin)*raw_sector,out.h);flush(out.h);}
            protect(path);manifest<<"file="<<utf8(name)<<"\r\n";
        }
    }
    std::string metadata=manifest.str();{auto out=output(result.audio);write(out.h,metadata.data(),DWORD(metadata.size()));flush(out.h);}protect(result.audio);
    return result;
}
}
