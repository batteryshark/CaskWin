// SPDX-License-Identifier: LGPL-2.1-or-later
#include "cdda-media.h"
#include <winioctl.h>
#include <ntddcdrm.h>
#include <array>
#include <map>
#include <memory>
#include <mutex>

namespace wd {
namespace {
constexpr DWORD pcm_sector=2352, logical_sector=2048, pregap=150;
[[noreturn]] void bad(const char *reason) {throw std::runtime_error(std::string("Invalid prepared CDDA medium: ")+reason);}
struct Handle {
    HANDLE value=INVALID_HANDLE_VALUE;
    explicit Handle(HANDLE h):value(h){if(h==INVALID_HANDLE_VALUE)check(GetLastError(),"Open CDDA track");}
    ~Handle(){if(value!=INVALID_HANDLE_VALUE)CloseHandle(value);}
    Handle(const Handle&)=delete;Handle& operator=(const Handle&)=delete;
};
struct Track {
    DWORD number=0,start=0,extent=0,sectors=0;
    bool audio=false;
    std::shared_ptr<Handle> pcm;
};
struct Medium {
    CddaInfo info;
    std::vector<Track> tracks;
    DWORD cursor=0;
    DWORD generation=0;
};
str setting(const str &manifest,const wchar_t *section,const wchar_t *key) {
    wchar_t text[1024];DWORD n=GetPrivateProfileStringW(section,key,L"",text,1024,manifest.c_str());
    if(n>=1023)bad("oversized manifest field");
    return str(text,n);
}
DWORD number(const str &s,DWORD limit) {
    if(s.empty()||s.size()>10)bad("missing or oversized integer");
    ULONGLONG value=0;
    for(wchar_t c:s){if(c<L'0'||c>L'9')bad("invalid integer");value=value*10+c-L'0';}
    if(value>limit)bad("integer exceeds media bounds");
    return DWORD(value);
}
void private_path(const Prefix &prefix,const str &path,bool file) {
    str root=prefix.root+L"\\.winedows\\media";
    if(!inside(path,root)||path.size()<=root.size())bad("manifest and PCM must be inside private media storage");
    str current=prefix.root;
    str tail=path.substr(prefix.root.size()+1);size_t at=0;
    while(at<tail.size()) {
        size_t end=tail.find(L'\\',at);if(end==str::npos)end=tail.size();
        current+=L"\\"+tail.substr(at,end-at);
        DWORD a=GetFileAttributesW(current.c_str());
        if(a==INVALID_FILE_ATTRIBUTES||(a&FILE_ATTRIBUTE_REPARSE_POINT))bad("missing or redirected media path");
        if(end<tail.size()&&!(a&FILE_ATTRIBUTE_DIRECTORY))bad("media path component is not a directory");
        if(end==tail.size()&&file&&(a&FILE_ATTRIBUTE_DIRECTORY))bad("expected media file");
        at=end+1;
    }
}
std::shared_ptr<Medium> load(const Prefix &prefix,int drive,const str &manifest_arg) {
    if(drive<0||drive>=26||drive==2)bad("drive must be a non-C letter");
    str manifest=fullpath(manifest_arg);private_path(prefix,manifest,true);
    str version=setting(manifest,L"disc",L"version");
    if(version!=L"1"&&version!=L"2")bad("unknown manifest version");
    bool cue=version==L"2";
    auto m=std::make_shared<Medium>();m->info.directory=directory(manifest);
    m->info.tracks=number(setting(manifest,L"disc",L"tracks"),99);
    if(!m->info.tracks)bad("disc has no tracks");
    str origin=setting(manifest,L"disc",L"data_source");
    if(cue?origin!=L"cue":(origin!=L"none"&&origin!=L"iso"&&origin!=L"synthetic"))bad("manifest version and data source disagree");
    m->info.synthetic_data=origin==L"synthetic";
    DWORD position=0;bool have_audio=false,have_data=false;
    for(DWORD n=1;n<=m->info.tracks;n++) {
        wchar_t section[16];swprintf_s(section,L"track%02lu",n);
        Track t;t.number=n;t.start=t.extent=position;
        t.sectors=number(setting(manifest,section,L"sectors"),256*60*75-pregap);
        if(!t.sectors)bad("empty track");
        if(cue) {
            t.start=number(setting(manifest,section,L"start"),256*60*75-pregap);
            t.extent=number(setting(manifest,section,L"extent_start"),256*60*75-pregap);
            if(t.extent!=position||t.start<t.extent||ULONGLONG(t.start)>=ULONGLONG(t.extent)+t.sectors)
                bad("CUE track indexes do not match contiguous sector extents");
        }
        str type=setting(manifest,section,L"type");
        if(type==L"data") {
            if(n!=1||origin==L"none")bad("only an explicit first data track is supported");
            have_data=true;
            if(t.start!=0)bad("first data track INDEX 01 must begin at LBA zero");
            if(origin==L"iso") {
                const auto &d=prefix.drives[drive];
                if(d.image.empty()||d.sector_size!=logical_sector||d.volume_size/logical_sector!=t.sectors)
                    bad("ISO data track sectors do not match the mounted image");
            }
            if(cue) {
                const auto &d=prefix.drives[drive];
                if(d.image.empty()||d.sector_size!=logical_sector||d.image_size%logical_sector||d.image_size/logical_sector!=t.sectors)
                    bad("CUE data extent does not match the prepared data image");
            }
        } else if(type==L"audio") {
            t.audio=have_audio=true;
            wchar_t expected[32];swprintf_s(expected,L"track%02lu.pcm",n);
            str name=setting(manifest,section,L"file");
            if(_wcsicmp(name.c_str(),expected))bad("PCM filename must match its track number");
            str path=m->info.directory+L"\\"+name;private_path(prefix,path,true);
            t.pcm=std::make_shared<Handle>(CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_RANDOM_ACCESS,nullptr));
            LARGE_INTEGER size{};if(!GetFileSizeEx(t.pcm->value,&size))check(GetLastError(),"Read PCM size");
            if(size.QuadPart!=LONGLONG(t.sectors)*pcm_sector)bad("PCM length does not match declared sectors");
        } else bad("unknown track type");
        // The lead-out includes the pregap and must remain below minute 256.
        if(position>=256*60*75-pregap-t.sectors)bad("disc exceeds representable MSF duration");
        position+=t.sectors;m->tracks.push_back(std::move(t));
    }
    if(!have_audio)bad("disc has no audio tracks");
    if((origin!=L"none")!=have_data)bad("data source and first track disagree");
    m->info.sectors=position;return m;
}
void msf(DWORD sector,BYTE address[4]) {
    address[0]=0;address[1]=BYTE(sector/(60*75));address[2]=BYTE(sector/75%60);address[3]=BYTE(sector%75);
}
DWORD sector_of(BYTE minute,BYTE second,BYTE frame) {
    if(second>=60||frame>=75)bad("invalid seek MSF");
    return DWORD(minute)*60*75+DWORD(second)*75+frame;
}
const Track *track_at(const Medium &m,DWORD position) {
    for(const auto &t:m.tracks)if(position>=t.extent&&position-t.extent<t.sectors)return &t;
    return nullptr;
}
BOOL error(DWORD value) {SetLastError(value);return FALSE;}
std::mutex cache_mutex;
std::map<str,std::shared_ptr<Medium>> cache;
}
CddaInfo validate_cdda(const Prefix &prefix,int drive,const str &manifest) {return load(prefix,drive,manifest)->info;}
bool cdda_ioctl(const Prefix &prefix,int drive,DWORD code,void *input,DWORD input_size,
                void *output,DWORD output_size,DWORD *done,OVERLAPPED *overlapped,BOOL &result,DWORD generation) {
    if(drive<0||drive>=26||prefix.drives[drive].audio.empty())return false;
    switch(code) {
    case IOCTL_CDROM_READ_TOC:case IOCTL_CDROM_RAW_READ:case IOCTL_CDROM_READ_Q_CHANNEL:
    case IOCTL_CDROM_SEEK_AUDIO_MSF:case IOCTL_CDROM_PLAY_AUDIO_MSF:
    case IOCTL_CDROM_STOP_AUDIO:case IOCTL_CDROM_PAUSE_AUDIO:case IOCTL_CDROM_RESUME_AUDIO:break;
    default:return false;
    }
    if(done)*done=0;
    result=FALSE;
    if(overlapped){result=error(ERROR_NOT_SUPPORTED);return true;}
    std::lock_guard<std::mutex> guard(cache_mutex);
    try {
        str key=prefix.root+L"|"+std::to_wstring(drive)+L"|"+prefix.drives[drive].audio;
        auto it=cache.find(key);
        if(it==cache.end())it=cache.emplace(key,load(prefix,drive,prefix.drives[drive].audio)).first;
        if(it->second->generation!=generation){
            auto refreshed=load(prefix,drive,prefix.drives[drive].audio);refreshed->generation=generation;it->second=std::move(refreshed);
        }
        auto &m=*it->second;
        if(code==IOCTL_CDROM_READ_TOC) {
            DWORD size=DWORD(FIELD_OFFSET(CDROM_TOC,TrackData)+(m.tracks.size()+1)*sizeof(TRACK_DATA));
            if(!output||output_size<size){result=error(ERROR_INSUFFICIENT_BUFFER);return true;}
            memset(output,0,size);auto *toc=(CDROM_TOC*)output;
            toc->Length[0]=BYTE((size-2)>>8);toc->Length[1]=BYTE(size-2);toc->FirstTrack=1;toc->LastTrack=BYTE(m.tracks.size());
            for(size_t n=0;n<=m.tracks.size();n++) {
                auto &out=toc->TrackData[n];out.Adr=1;
                if(n==m.tracks.size()){out.TrackNumber=0xaa;msf(m.info.sectors+pregap,out.Address);}
                else {const auto &t=m.tracks[n];out.TrackNumber=BYTE(t.number);out.Control=t.audio?0:4;msf(t.start+pregap,out.Address);}
            }
            if(done)*done=size;
            result=TRUE;
        } else if(code==IOCTL_CDROM_RAW_READ) {
            if(!input||input_size<sizeof(RAW_READ_INFO)){result=error(ERROR_INVALID_PARAMETER);return true;}
            RAW_READ_INFO request;memcpy(&request,input,sizeof(request));
            ULONGLONG lba=ULONGLONG(request.DiskOffset.QuadPart)/logical_sector;
            ULONGLONG bytes=ULONGLONG(request.SectorCount)*pcm_sector;
            if(request.TrackMode!=CDDA||request.DiskOffset.QuadPart<0||request.DiskOffset.QuadPart%logical_sector||
               !request.SectorCount||lba>=m.info.sectors||request.SectorCount>m.info.sectors-lba) {
                result=error(ERROR_INVALID_PARAMETER);return true;
            }
            if(!output||bytes>output_size){result=error(ERROR_INSUFFICIENT_BUFFER);return true;}
            // Validate the complete range before returning any bytes. A data
            // extent is never fabricated as silent audio or read from a disk.
            for(DWORD n=DWORD(lba);n<lba+request.SectorCount;) {
                const Track *t=track_at(m,n);if(!t||!t->audio){result=error(ERROR_INVALID_PARAMETER);return true;}
                n=std::min(DWORD(lba+request.SectorCount),t->extent+t->sectors);
            }
            DWORD remaining=request.SectorCount,offset=0,at=DWORD(lba);
            while(remaining) {
                const Track &t=*track_at(m,at);DWORD sectors=std::min(remaining,t.extent+t.sectors-at);
                LARGE_INTEGER position;position.QuadPart=LONGLONG(at-t.extent)*pcm_sector;
                if(!SetFilePointerEx(t.pcm->value,position,nullptr,FILE_BEGIN))check(GetLastError(),"Seek CDDA track");
                DWORD count=0,n=sectors*pcm_sector;
                if(!ReadFile(t.pcm->value,(BYTE*)output+offset,n,&count,nullptr))check(GetLastError(),"Read CDDA sectors");
                if(count!=n)bad("truncated PCM during read");
                at+=sectors;remaining-=sectors;offset+=n;
            }
            // This is the reader's cursor, including possible prefetch. It is
            // not a fabricated hardware playback clock; Wine tracks its own
            // DirectSound playback mode and position separately.
            m.cursor=DWORD(lba+request.SectorCount);
            if(done)*done=DWORD(bytes);
            result=TRUE;
        } else if(code==IOCTL_CDROM_READ_Q_CHANNEL) {
            if(!input||input_size<sizeof(CDROM_SUB_Q_DATA_FORMAT)||((CDROM_SUB_Q_DATA_FORMAT*)input)->Format!=IOCTL_CDROM_CURRENT_POSITION){result=error(ERROR_NOT_SUPPORTED);return true;}
            if(!output||output_size<sizeof(SUB_Q_CURRENT_POSITION)){result=error(ERROR_INSUFFICIENT_BUFFER);return true;}
            auto *position=(SUB_Q_CURRENT_POSITION*)output;memset(position,0,sizeof(*position));
            position->Header.AudioStatus=AUDIO_STATUS_NO_STATUS;
            position->Header.DataLength[1]=sizeof(*position)-sizeof(SUB_Q_HEADER);
            position->FormatCode=IOCTL_CDROM_CURRENT_POSITION;position->ADR=1;
            const Track *t=track_at(m,std::min(m.cursor,m.info.sectors-1));
            position->TrackNumber=BYTE(t->number);position->Control=t->audio?0:4;
            position->IndexNumber=m.cursor<t->start?0:1;
            msf(m.cursor+pregap,position->AbsoluteAddress);msf(m.cursor<t->start?t->start-m.cursor:m.cursor-t->start,position->TrackRelativeAddress);
            if(done)*done=sizeof(*position);
            result=TRUE;
        } else if(code==IOCTL_CDROM_SEEK_AUDIO_MSF) {
            if(!input||input_size<sizeof(CDROM_SEEK_AUDIO_MSF)){result=error(ERROR_INVALID_PARAMETER);return true;}
            auto *seek=(CDROM_SEEK_AUDIO_MSF*)input;DWORD frame=sector_of(seek->M,seek->S,seek->F);
            if(frame<pregap||frame-pregap>=m.info.sectors){result=error(ERROR_INVALID_PARAMETER);return true;}
            m.cursor=frame-pregap;result=TRUE;
        } else if(code==IOCTL_CDROM_STOP_AUDIO) result=TRUE;
        else result=error(ERROR_NOT_SUPPORTED); // Audio rendering belongs to Wine mcicda/DirectSound.
    } catch(const std::exception &) {result=error(ERROR_INVALID_DATA);}
    return true;
}
}
