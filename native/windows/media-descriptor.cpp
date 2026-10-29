// SPDX-License-Identifier: LGPL-2.1-or-later
#include "media-descriptor.h"
#include "media.h"
#include "cue-media.h"
#include "cdda-media.h"

namespace wd {
namespace {
void private_path(const Prefix &prefix,const str &path,bool directory){
    str root=prefix.root+L"\\.winedows\\media";
    if(path.empty()||!inside(path,root)||path.size()<=root.size()||_wcsicmp(fullpath(path).c_str(),path.c_str()))
        throw std::runtime_error("Live media must use canonical private cache paths");
    str current=prefix.root;size_t at=prefix.root.size()+1;
    while(at<path.size()){
        size_t end=path.find(L'\\',at);if(end==str::npos)end=path.size();current+=L"\\"+path.substr(at,end-at);
        DWORD flags=GetFileAttributesW(current.c_str());
        if(flags==INVALID_FILE_ATTRIBUTES||(flags&FILE_ATTRIBUTE_REPARSE_POINT))throw std::runtime_error("Live media cache is missing or redirected");
        if((end<path.size()||directory)!=bool(flags&FILE_ATTRIBUTE_DIRECTORY))throw std::runtime_error("Invalid live media cache object type");
        if(end==path.size()&&!directory&&!(flags&FILE_ATTRIBUTE_READONLY))throw std::runtime_error("Live media image/manifest must be immutable");
        at=end+1;
    }
}
void label_check(const str &label){
    if(label.size()>255)throw std::runtime_error("Live media volume label exceeds 255 characters");
    for(wchar_t c:label)if(c<32)throw std::runtime_error("Invalid live media volume label");
}
}
void validate_live_medium(const Prefix &prefix,const Drive &medium){
    if(medium.type!=DRIVE_CDROM||!medium.readonly||medium.sector_size!=2048||!medium.image_size||
       medium.image_size%2048||!medium.volume_size||medium.volume_size%2048||medium.volume_size>medium.image_size)
        throw std::runtime_error("Live swap requires a prepared read-only ISO/CUE CD medium");
    label_check(medium.label);private_path(prefix,medium.path,true);private_path(prefix,medium.image,false);
    HANDLE file=CreateFileW(medium.image.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
    if(file==INVALID_HANDLE_VALUE)check(GetLastError(),"Open live media image");
    LARGE_INTEGER size{};BOOL ok=GetFileSizeEx(file,&size);DWORD error=GetLastError();CloseHandle(file);
    if(!ok)check(error,"Read live media image size");
    if(size.QuadPart<0||ULONGLONG(size.QuadPart)!=medium.image_size)throw std::runtime_error("Live media image geometry does not match its bytes");
    if(!medium.audio.empty()){
        private_path(prefix,medium.audio,false);
        Prefix staged=prefix;staged.drives[3]=medium;validate_cdda(staged,3,medium.audio);
    }
}
Drive prepare_live_medium(const Prefix &prefix,const str &source,const str &label){
    label_check(label);MountedImage image;str audio;
    if(source.size()>=4&&!_wcsicmp(source.c_str()+source.size()-4,L".cue")){
        auto cue=prepare_cue(prefix.root,source);image=std::move(cue.data);audio=std::move(cue.audio);
    }else if(source.size()>=4&&!_wcsicmp(source.c_str()+source.size()-4,L".iso"))image=prepare_iso(prefix.root,source);
    else throw std::runtime_error("Live swap accepts an ISO or supported BIN/CUE image");
    Drive result;result.path=std::move(image.path);result.image=std::move(image.image);result.audio=std::move(audio);
    result.label=label.empty()?image.label:label;result.type=DRIVE_CDROM;result.readonly=true;
    result.serial=image.serial;result.sector_size=image.sector_size;result.image_size=image.image_size;result.volume_size=image.volume_size;
    validate_live_medium(prefix,result);return result;
}
}
