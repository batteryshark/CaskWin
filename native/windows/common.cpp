// SPDX-License-Identifier: LGPL-2.1-or-later
#include "common.h"
#include "media.h"
#include "cdda-media.h"
#include "cue-media.h"
#include "com-marshaling.h"
#include "imaging-defaults.h"
#include "video-backend.h"
#include <fstream>
#include <sstream>

namespace wd {
str env(const wchar_t *name) {
    DWORD n = GetEnvironmentVariableW(name, nullptr, 0);
    if (!n) return {};
    str s(n, 0); GetEnvironmentVariableW(name, s.data(), n); s.resize(n-1); return s;
}
str fullpath(const str &path) {
    DWORD n=GetFullPathNameW(path.c_str(),0,nullptr,nullptr);
    if (!n) throw std::runtime_error("Invalid path");
    str s(n,0); n=GetFullPathNameW(path.c_str(),n,s.data(),nullptr); s.resize(n);
    while(s.size()>3 && s.back()==L'\\') s.pop_back();
    return s;
}
str directory(const str &s) { return s.substr(0,s.find_last_of(L"\\/")); }
str module_dir() { wchar_t s[32768]; GetModuleFileNameW(nullptr,s,32768); return directory(s); }
bool starts(const str &s,const str &base) { return s.size()>=base.size() && !_wcsnicmp(s.c_str(),base.c_str(),base.size()); }
bool inside(const str &s,const str &base) { return starts(s,base) && (s.size()==base.size() || s[base.size()]==L'\\'); }
std::string utf8(const str &s) {
    int n=WideCharToMultiByte(CP_UTF8,0,s.data(),(int)s.size(),nullptr,0,nullptr,nullptr);
    std::string r(n,0); WideCharToMultiByte(CP_UTF8,0,s.data(),(int)s.size(),r.data(),n,nullptr,nullptr); return r;
}
str widen(const std::string &s) {
    int n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,s.data(),(int)s.size(),nullptr,0);
    if (!n && !s.empty()) throw std::runtime_error("Invalid UTF-8");
    str r(n,0); MultiByteToWideChar(CP_UTF8,0,s.data(),(int)s.size(),r.data(),n); return r;
}
str quote(const str &s) {
    str out=L"\""; unsigned slash=0;
    for(wchar_t c:s) {
        if(c==L'\\') { ++slash; continue; }
        out.append(c==L'"'?slash*2+1:slash,L'\\'); slash=0; out+=c;
    }
    out.append(slash*2,L'\\'); return out+L'"';
}
void check(LSTATUS e,const char *op) { if(e) throw std::runtime_error(std::string(op)+": Windows error "+std::to_string(e)); }
void mkdirs(const str &s) {
    if(s.empty() || GetFileAttributesW(s.c_str())!=INVALID_FILE_ATTRIBUTES) return;
    str parent=directory(s); if(parent!=s) mkdirs(parent);
    if(!CreateDirectoryW(s.c_str(),nullptr) && GetLastError()!=ERROR_ALREADY_EXISTS) check(GetLastError(),"CreateDirectory");
}
void set_string(HKEY root,const wchar_t *path,const wchar_t *name,const str &value) {
    HKEY k; check(RegCreateKeyExW(root,path,0,nullptr,0,KEY_ALL_ACCESS,nullptr,&k,nullptr),"Create prefix registry key");
    LSTATUS e=RegSetValueExW(k,name,0,REG_SZ,(const BYTE*)value.c_str(),(DWORD)((value.size()+1)*2)); RegCloseKey(k); check(e,"Set prefix registry value");
}
Prefix::Prefix(const str &p):root(fullpath(p)) {
    wchar_t b[32768]; GetWindowsDirectoryW(b,32768); windows=b;
    host_profile=env(L"USERPROFILE");
    load();
}
void Prefix::load() {
    str ini=root+L"\\dosdevices\\drives.ini";
    for(int i=0;i<26;i++) {
        wchar_t section[]={wchar_t(L'a'+i),0}, b[32768];
        drives[i]=Drive{};
        GetPrivateProfileStringW(section,L"path",L"",b,32768,ini.c_str());
        drives[i].path=b;
        if(!drives[i].path.empty()) {
            if(drives[i].path.size()<2 || drives[i].path[1]!=L':') drives[i].path=fullpath(root+L"\\"+drives[i].path);
            drives[i].type=GetPrivateProfileIntW(section,L"type",DRIVE_FIXED,ini.c_str());
            drives[i].readonly=GetPrivateProfileIntW(section,L"readonly",0,ini.c_str())!=0;
            drives[i].serial=GetPrivateProfileIntW(section,L"serial",0x57494e45+i,ini.c_str());
            GetPrivateProfileStringW(section,L"label",L"Winedows",b,32768,ini.c_str()); drives[i].label=b;
            GetPrivateProfileStringW(section,L"audio",L"",b,32768,ini.c_str());drives[i].audio=b;
            if(!drives[i].audio.empty()) {
                if(drives[i].audio.size()<2||drives[i].audio[1]!=L':')drives[i].audio=fullpath(root+L"\\"+drives[i].audio);
                drives[i].readonly=true;
            }
            GetPrivateProfileStringW(section,L"image",L"",b,32768,ini.c_str());drives[i].image=b;
            if(!drives[i].image.empty()) {
                if(drives[i].image.size()<2||drives[i].image[1]!=L':')drives[i].image=fullpath(root+L"\\"+drives[i].image);
                GetPrivateProfileStringW(section,L"image_size",L"0",b,32768,ini.c_str());drives[i].image_size=_wcstoui64(b,nullptr,10);
                GetPrivateProfileStringW(section,L"volume_size",L"0",b,32768,ini.c_str());drives[i].volume_size=_wcstoui64(b,nullptr,10);
                drives[i].sector_size=GetPrivateProfileIntW(section,L"sector_size",2048,ini.c_str());
                drives[i].readonly=true;
            }
        }
    }
    if(drives[2].path.empty()) { drives[2].path=root+L"\\drive_c"; drives[2].type=DRIVE_FIXED; drives[2].label=L"Winedows"; drives[2].serial=0x57494e45; }
}
void Prefix::drive(wchar_t letter,const str &p,UINT type,const str &label,bool ro) {
    int i=towlower(letter)-L'a'; if(i<0 || i>=26) throw std::runtime_error("Drive letter must be A-Z");
    if(i==2) throw std::runtime_error("The prefix C: drive cannot be remapped");
    str target=fullpath(p); DWORD a=GetFileAttributesW(target.c_str());
    if(a==INVALID_FILE_ATTRIBUTES || !(a&FILE_ATTRIBUTE_DIRECTORY)) throw std::runtime_error("Drive source must be an existing directory");
    // Writable media stays in the prefix; external media is always read-only.
    if(!inside(target,root)) ro=true;
    mkdirs(root+L"\\dosdevices"); str ini=root+L"\\dosdevices\\drives.ini";
    wchar_t sec[]={wchar_t(L'a'+i),0};
    if(inside(target,root)) target=target.substr(root.size()+1);
    auto put=[&](const wchar_t *key,const str &v){ if(!WritePrivateProfileStringW(sec,key,v.c_str(),ini.c_str())) check(GetLastError(),"Save drive mapping"); };
    put(L"path",target); put(L"type",std::to_wstring(type)); put(L"label",label); put(L"readonly",(ro||type==DRIVE_CDROM)?L"1":L"0");
    for(const wchar_t *key:{L"image",L"image_size",L"volume_size",L"sector_size",L"serial",L"audio"})
        if(!WritePrivateProfileStringW(sec,key,nullptr,ini.c_str()))check(GetLastError(),"Clear image mapping");
    load();
}
static void publish_mounted_image(Prefix &prefix,int i,const MountedImage &media,const str &label,const str &audio) {
    const str &root=prefix.root;
    mkdirs(root+L"\\dosdevices");str ini=root+L"\\dosdevices\\drives.ini";
    // Publish a complete mapping in one replacement. A failed parse or extraction
    // leaves the previously mounted disc usable, including its label and serial.
    str temp=directory(media.image)+L"\\drives.ini";
    // INI APIs otherwise create ANSI files, which lose Joliet volume labels.
    // Preserve all existing text while converting old ANSI mappings to UTF-16.
    std::ifstream previous(ini,std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(previous)),{});
    previous.close(); // Windows must be able to replace the old INI below.
    std::ofstream staged(temp,std::ios::binary|std::ios::trunc);
    if(bytes.size()>=2&&(BYTE)bytes[0]==0xff&&(BYTE)bytes[1]==0xfe)staged.write(bytes.data(),bytes.size());
    else {
        staged.put(char(0xff));staged.put(char(0xfe));
        int n=MultiByteToWideChar(CP_ACP,0,bytes.data(),(int)bytes.size(),nullptr,0);
        str text(n,0);if(n)MultiByteToWideChar(CP_ACP,0,bytes.data(),(int)bytes.size(),text.data(),n);
        staged.write((const char*)text.data(),text.size()*sizeof(wchar_t));
    }
    staged.close();if(!staged)throw std::runtime_error("Cannot stage drive mappings");
    wchar_t sec[]={wchar_t(L'a'+i),0};
    auto put=[&](const wchar_t *key,const str &v){if(!WritePrivateProfileStringW(sec,key,v.c_str(),temp.c_str()))check(GetLastError(),"Stage image mapping");};
    put(L"path",media.path.substr(root.size()+1));put(L"image",media.image.substr(root.size()+1));
    if(audio.empty()){
        if(!WritePrivateProfileStringW(sec,L"audio",nullptr,temp.c_str()))check(GetLastError(),"Clear previous CDDA medium");
    }else put(L"audio",audio.substr(root.size()+1));
    put(L"type",std::to_wstring(DRIVE_CDROM));put(L"readonly",L"1");put(L"label",label.empty()?media.label:label);
    put(L"serial",std::to_wstring(media.serial));put(L"sector_size",std::to_wstring(media.sector_size));
    put(L"image_size",std::to_wstring(media.image_size));put(L"volume_size",std::to_wstring(media.volume_size));
    // The all-null cache flush is allowed to return zero; flush the staged file
    // itself before publishing instead of interpreting that value as an error.
    WritePrivateProfileStringW(nullptr,nullptr,nullptr,temp.c_str());
    HANDLE metadata=CreateFileW(temp.c_str(),GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
    if(metadata==INVALID_HANDLE_VALUE)check(GetLastError(),"Open staged image mapping");
    BOOL flushed=FlushFileBuffers(metadata);DWORD flush_error=GetLastError();CloseHandle(metadata);
    if(!flushed)check(flush_error,"Flush image mapping");
    if(!MoveFileExW(temp.c_str(),ini.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))check(GetLastError(),"Publish image mapping");
    WritePrivateProfileStringW(nullptr,nullptr,nullptr,ini.c_str());prefix.load();
}
void Prefix::mount_image(wchar_t letter,const str &path,const str &label) {
    int i=towlower(letter)-L'a';if(i<0||i>=26)throw std::runtime_error("Drive letter must be A-Z");
    if(i==2)throw std::runtime_error("The prefix C: drive cannot be remapped");
    for(wchar_t c:label)if(c<32)throw std::runtime_error("Invalid volume label");
    MountedImage media=prepare_iso(root,path);
    publish_mounted_image(*this,i,media,label,{});
}
void Prefix::mount_cue(wchar_t letter,const str &path,const str &label) {
    int i=towlower(letter)-L'a';if(i<0||i>=26)throw std::runtime_error("Drive letter must be A-Z");
    if(i==2)throw std::runtime_error("The prefix C: drive cannot be remapped");
    for(wchar_t c:label)if(c<32)throw std::runtime_error("Invalid volume label");
    MountedCue media=prepare_cue(root,path);
    // Validate against the new data extent without publishing a partially
    // updated data/audio pair. The physical extent can exceed the ISO filesystem.
    Prefix staged=*this;auto &drive=staged.drives[i];drive=Drive{};
    drive.path=media.data.path;drive.image=media.data.image;drive.audio=media.audio;
    drive.image_size=media.data.image_size;drive.volume_size=media.data.volume_size;
    drive.sector_size=media.data.sector_size;drive.type=DRIVE_CDROM;drive.readonly=true;
    validate_cdda(staged,i,media.audio);
    publish_mounted_image(*this,i,media.data,label,media.audio);
}
void Prefix::mount_audio(wchar_t letter,const str &manifest_arg) {
    int i=towlower(letter)-L'a';if(i<0||i>=26||i==2)throw std::runtime_error("CDDA drive must be a non-C letter");
    str manifest=fullpath(manifest_arg);CddaInfo media=validate_cdda(*this,i,manifest);
    mkdirs(root+L"\\dosdevices");str ini=root+L"\\dosdevices\\drives.ini";
    str temp=media.directory+L"\\mapping-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64())+L".ini";
    std::ifstream previous(ini,std::ios::binary);std::string bytes((std::istreambuf_iterator<char>(previous)),{});previous.close();
    std::ofstream staged(temp,std::ios::binary|std::ios::trunc);
    if(bytes.size()>=2&&(BYTE)bytes[0]==0xff&&(BYTE)bytes[1]==0xfe)staged.write(bytes.data(),bytes.size());
    else {
        staged.put(char(0xff));staged.put(char(0xfe));
        int n=MultiByteToWideChar(CP_ACP,0,bytes.data(),(int)bytes.size(),nullptr,0);
        str value(n,0);if(n)MultiByteToWideChar(CP_ACP,0,bytes.data(),(int)bytes.size(),value.data(),n);
        staged.write((const char*)value.data(),value.size()*sizeof(wchar_t));
    }
    staged.close();if(!staged)throw std::runtime_error("Cannot stage CDDA mapping");
    wchar_t sec[]={wchar_t(L'a'+i),0};
    auto put=[&](const wchar_t *key,const str &v){if(!WritePrivateProfileStringW(sec,key,v.c_str(),temp.c_str()))check(GetLastError(),"Stage CDDA mapping");};
    if(drives[i].path.empty()) {
        str files=media.directory+L"\\files";mkdirs(files);
        if(GetFileAttributesW(files.c_str())&FILE_ATTRIBUTE_REPARSE_POINT)throw std::runtime_error("CDDA file view must not be a reparse point");
        put(L"path",files.substr(root.size()+1));put(L"label",L"CDDA");
    }
    put(L"audio",manifest.substr(root.size()+1));put(L"type",std::to_wstring(DRIVE_CDROM));put(L"readonly",L"1");
    WritePrivateProfileStringW(nullptr,nullptr,nullptr,temp.c_str());
    HANDLE metadata=CreateFileW(temp.c_str(),GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
    if(metadata==INVALID_HANDLE_VALUE)check(GetLastError(),"Open staged CDDA mapping");
    BOOL flushed=FlushFileBuffers(metadata);DWORD flush_error=GetLastError();CloseHandle(metadata);
    if(!flushed)check(flush_error,"Flush CDDA mapping");
    if(!MoveFileExW(temp.c_str(),ini.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))check(GetLastError(),"Publish CDDA mapping");
    WritePrivateProfileStringW(nullptr,nullptr,nullptr,ini.c_str());load();
}
HKEY Prefix::hive() const {
    HKEY k; check(RegLoadAppKeyW((root+L"\\.winedows\\registry.dat").c_str(),&k,KEY_ALL_ACCESS,0,0),"Load private registry hive"); return k;
}
static std::string registry_fingerprint(const str &root) {
    std::ostringstream result;
    for(const wchar_t *name:{L"system.reg",L"user.reg",L"userdef.reg"}) {
        std::ifstream f(root+L"\\"+name,std::ios::binary);unsigned long long hash=14695981039346656037ull;
        char b[4096];while(f){f.read(b,sizeof(b));for(std::streamsize i=0;i<f.gcount();i++){hash^=(unsigned char)b[i];hash*=1099511628211ull;}}
        result<<std::hex<<hash<<'\n';
    }
    return result.str();
}
void Prefix::init() {
    mkdirs(root+L"\\.winedows"); mkdirs(root+L"\\dosdevices");
    // CLI ISO mounts create distinct snapshots. Reject hand-edited aliases
    // before launching rather than guessing which drive owns a raw image handle.
    for(int i=0;i<26;i++)if(!drives[i].image.empty())for(int j=0;j<i;j++)
        if(!drives[j].image.empty()&&!_wcsicmp(drives[i].image.c_str(),drives[j].image.c_str()))
            throw std::runtime_error("Two drives share one ISO snapshot. Remount the source image separately for each letter with drive mount.");
    auto device_dir=[&](const str &path) {
        mkdirs(path);DWORD a=GetFileAttributesW(path.c_str());
        if(a==INVALID_FILE_ATTRIBUTES||!(a&FILE_ATTRIBUTE_DIRECTORY)||(a&FILE_ATTRIBUTE_REPARSE_POINT))
            throw std::runtime_error("Private raw device storage must be a real directory");
    };
    device_dir(root+L"\\.winedows");device_dir(root+L"\\.winedows\\devices");
    for(int i=0;i<26;i++)if(!drives[i].path.empty()&&drives[i].image.empty())device_dir(raw_device(i));
    const wchar_t *dirs[]={L"windows\\system32",L"windows\\syswow64",L"windows\\temp",L"Program Files",L"Program Files (x86)",L"ProgramData",L"users\\user\\AppData\\Local\\Temp",L"users\\user\\AppData\\Roaming",L"users\\user\\Documents",L"users\\user\\Saved Games",L"users\\user\\Desktop"};
    for(auto d:dirs) mkdirs(root+L"\\drive_c\\"+d);
    bool fresh=GetFileAttributesW((root+L"\\.winedows\\registry.dat").c_str())==INVALID_FILE_ATTRIBUTES;
    HKEY h=hive();
    const wchar_t *keys[]={L"Machine",L"User",L"Default"}, *files[]={L"system.reg",L"user.reg",L"userdef.reg"};
    if(!fresh) {
        std::ifstream stamp(root+L"\\.winedows\\registry.state",std::ios::binary);
        std::string previous((std::istreambuf_iterator<char>(stamp)),{});
        if(!previous.empty() && previous!=registry_fingerprint(root)) {
            // Validate every text hive before changing any live root. The launcher
            // holds the prefix session lock, so no game can see partial imports.
            try {
                for(int i=0;i<3;i++) {
                    str temp=str(L"Import")+keys[i];RegDeleteTreeW(h,temp.c_str());HKEY k;
                    check(RegCreateKeyExW(h,temp.c_str(),0,nullptr,0,KEY_ALL_ACCESS,nullptr,&k,nullptr),"Stage registry sync");
                    try{import_reg(k,root+L"\\"+files[i]);}catch(...){RegCloseKey(k);throw;}RegCloseKey(k);
                }
                auto copy=[&](const str &source,const str &dest,bool clear) {
                    HKEY from=nullptr,to=nullptr;check(RegOpenKeyExW(h,source.c_str(),0,KEY_READ,&from),"Open sync source");
                    LSTATUS e=RegCreateKeyExW(h,dest.c_str(),0,nullptr,0,KEY_ALL_ACCESS,nullptr,&to,nullptr);
                    if(!e && clear)e=RegDeleteTreeW(to,nullptr);
                    try{check(e,"Prepare synchronized registry root");copy_registry(from,to);}
                    catch(...){RegCloseKey(from);if(to)RegCloseKey(to);throw;}
                    RegCloseKey(from);RegCloseKey(to);
                };
                for(auto name:keys)copy(name,str(L"Backup")+name,true);
                try {for(auto name:keys)copy(str(L"Import")+name,name,true);}
                catch(...) {for(auto name:keys)copy(str(L"Backup")+name,name,true);throw;}
                for(auto name:keys){RegDeleteTreeW(h,(str(L"Import")+name).c_str());RegDeleteTreeW(h,(str(L"Backup")+name).c_str());}
            }catch(...){for(auto name:keys)RegDeleteTreeW(h,(str(L"Import")+name).c_str());RegCloseKey(h);throw;}
        }
    }
    try {
    for(int i=0;i<3;i++) {
        HKEY k; check(RegCreateKeyExW(h,keys[i],0,nullptr,0,KEY_ALL_ACCESS,nullptr,&k,nullptr),"Create registry root");
        try {
            if(fresh && GetFileAttributesW((root+L"\\"+files[i]).c_str())!=INVALID_FILE_ATTRIBUTES) import_reg(k,root+L"\\"+files[i]);
        } catch(...) {RegCloseKey(k);throw;}
        RegCloseKey(k);
    }
    // Native HKCR merging opens both class roots before searching a child.
    // Keep empty private roots available even before an installer registers COM.
    for(const wchar_t *classes:{L"Machine\\Software\\Classes",L"User\\Software\\Classes",L"Machine\\Software\\WOW6432Node",L"Machine\\Software\\Classes\\WOW6432Node",L"User\\Software\\Classes\\WOW6432Node"}){
        HKEY k;check(RegCreateKeyExW(h,classes,0,nullptr,0,KEY_ALL_ACCESS,nullptr,&k,nullptr),"Create private class root");RegCloseKey(k);
    }
    initialize_standard_com(h);
    initialize_imaging_com(h);
    initialize_video_backend(h);
    if(fresh) {
        set_string(h,L"User\\Software\\Wine",L"Version",L"win10");
        set_string(h,L"Machine\\Software\\Microsoft\\Windows\\CurrentVersion",L"ProgramFilesDir",L"C:\\Program Files");
        set_string(h,L"Machine\\Software\\Microsoft\\Windows\\CurrentVersion",L"ProgramFilesDir (x86)",L"C:\\Program Files (x86)");
        set_string(h,L"Machine\\Software\\Microsoft\\Windows\\CurrentVersion",L"CommonFilesDir",L"C:\\Program Files\\Common Files");
        set_string(h,L"Machine\\Software\\Microsoft\\Windows NT\\CurrentVersion",L"SystemRoot",L"C:\\windows");
        const wchar_t *names[]={L"AppData",L"Local AppData",L"Personal",L"Desktop",L"{4C5C32FF-BB9D-43B0-BF0B-CCF11923E02B}"};
        const wchar_t *values[]={L"AppData\\Roaming",L"AppData\\Local",L"Documents",L"Desktop",L"Saved Games"};
        for(int i=0;i<5;i++) {
            set_string(h,L"User\\Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Shell Folders",names[i],str(L"C:\\users\\user\\")+values[i]);
            set_string(h,L"User\\Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\User Shell Folders",names[i],str(L"C:\\users\\user\\")+values[i]);
        }
    }
    } catch(...) {
        RegCloseKey(h);
        // An invalid initial text hive must not become a partially initialized
        // binary prefix which silently overwrites that input on the next run.
        if(fresh)DeleteFileW((root+L"\\.winedows\\registry.dat").c_str());
        throw;
    }
    RegCloseKey(h); export_registry();
}
void Prefix::export_registry() const {
    HKEY h=hive(); const wchar_t *keys[]={L"Machine",L"User",L"Default"}, *files[]={L"system.reg",L"user.reg",L"userdef.reg"};
    for(int i=0;i<3;i++) { HKEY k; if(!RegOpenKeyExW(h,keys[i],0,KEY_READ,&k)) { export_reg(k,root+L"\\"+files[i]); RegCloseKey(k); } }
    RegCloseKey(h);
    std::ofstream state(root+L"\\.winedows\\registry.state",std::ios::binary|std::ios::trunc);
    state<<registry_fingerprint(root);state.close();if(!state)throw std::runtime_error("Cannot save registry synchronization state");
}
str Prefix::raw_device(int drive) const {
    if(drive<0||drive>=26)throw std::runtime_error("Invalid raw drive index");
    return root+L"\\.winedows\\devices\\"+str(1,wchar_t(L'a'+drive));
}
int Prefix::drive_index(const str &s) const {
    str devices=root+L"\\.winedows\\devices\\";
    if(s.size()==devices.size()+1&&starts(s,devices)) {
        int i=towlower(s.back())-L'a';if(i>=0&&i<26&&!drives[i].path.empty())return i;
    }
    for(int i=0;i<26;i++)if(!drives[i].image.empty()&&!_wcsicmp(s.c_str(),drives[i].image.c_str()))return i;
    int best=-1;size_t length=0;
    for(int i=0;i<26;i++)if(!drives[i].path.empty()&&inside(s,drives[i].path)&&drives[i].path.size()>length){best=i;length=drives[i].path.size();}
    if(best>=0)return best;
    if(s.size()>=2 && s[1]==L':' && iswalpha(s[0])) return towlower(s[0])-L'a';
    return -1;
}
bool Prefix::readonly(const str &s) const {
    // Internal media remains immutable even when addressed by its host path or
    // when an older snapshot is no longer the active drive mapping.
    if(inside(s,root+L"\\.winedows\\media")||inside(s,root+L"\\.winedows\\devices"))return true;
    int i=drive_index(s);return i>=0&&drives[i].readonly;
}
str Prefix::translate(const str &input) const {
    str s=input; std::replace(s.begin(),s.end(),L'/',L'\\');
    if(starts(s,L"\\??\\") || starts(s,L"\\\\?\\")) s=s.substr(4);
    if(inside(s,root)) return s;
    for(const auto &d:drives) if(!d.path.empty() && inside(s,d.path)) return s;
    if(!host_profile.empty() && inside(s,host_profile)) return root+L"\\drive_c\\users\\user"+s.substr(host_profile.size());
    if(inside(s,windows)) return root+L"\\drive_c\\windows"+s.substr(windows.size());
    if(s.size()>=2 && s[1]==L':' && iswalpha(s[0])) {
        int i=towlower(s[0])-L'a';
        str tail=s.substr(2); if(tail.empty() || tail[0]!=L'\\') tail=L"\\"+tail;
        if(drives[i].path.empty()) return root+L"\\.unmapped\\"+str(1,wchar_t(L'a'+i))+tail;
        return drives[i].path+tail;
    }
    return s;
}
str Prefix::virtualize(const str &s) const {
    int i=drive_index(s);
    if(i>=0&&!drives[i].path.empty()&&inside(s,drives[i].path))return str(1,wchar_t(L'A'+i))+L":"+s.substr(drives[i].path.size());
    return s;
}
void Prefix::environment() const {
    SetEnvironmentVariableW(L"WINEDOWS_PREFIX",root.c_str()); SetEnvironmentVariableW(L"WINEPREFIX",root.c_str());
    SetEnvironmentVariableW(L"WINEDOWS_HOST_PROFILE",host_profile.c_str());
    const wchar_t *names[]={L"USERPROFILE",L"APPDATA",L"LOCALAPPDATA",L"TEMP",L"TMP",L"ProgramData",L"ALLUSERSPROFILE",L"ProgramFiles",L"ProgramFiles(x86)",L"HOMEDRIVE",L"HOMEPATH"};
    const wchar_t *values[]={L"C:\\users\\user",L"C:\\users\\user\\AppData\\Roaming",L"C:\\users\\user\\AppData\\Local",L"C:\\users\\user\\AppData\\Local\\Temp",L"C:\\users\\user\\AppData\\Local\\Temp",L"C:\\ProgramData",L"C:\\ProgramData",L"C:\\Program Files",L"C:\\Program Files (x86)",L"C:",L"\\users\\user"};
    for(int i=0;i<11;i++) SetEnvironmentVariableW(names[i],values[i]);
}
}
