// SPDX-License-Identifier: LGPL-2.1-or-later
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <algorithm>
struct Entry {std::wstring name;ULONGLONG time;DWORD attributes;};
static int failures;
static void test(bool ok,const char *name){printf("%s %s (error=%lu)\n",ok?"PASS":"FAIL",name,GetLastError());if(!ok)++failures;}
static ULONGLONG ticks(FILETIME value){return (ULONGLONG(value.dwHighDateTime)<<32)|value.dwLowDateTime;}
static FILETIME filetime(ULONGLONG value){return {DWORD(value),DWORD(value>>32)};}
static std::wstring wide(const std::string &text){int n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),int(text.size()),nullptr,0);std::wstring out(n,0);if(n)MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),int(text.size()),out.data(),n);return out;}
static std::wstring path(const std::wstring &root,const Entry &entry){return entry.name==L"."?root:root+L"\\"+entry.name;}
static HANDLE open(const std::wstring &name,DWORD access=FILE_READ_ATTRIBUTES){return CreateFileW(name.c_str(),access,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,nullptr);}
static std::vector<Entry> manifest(const wchar_t *name){
    std::ifstream input(name,std::ios::binary);std::string line;std::vector<Entry> entries;
    while(std::getline(input,line)){
        size_t a=line.find('\t'),b=line.find('\t',a+1);if(a==line.npos||b==line.npos)continue;
        Entry entry{wide(line.substr(0,a)),_strtoui64(line.c_str()+a+1,nullptr,10),DWORD(strtoul(line.c_str()+b+1,nullptr,10))};
        for(auto &c:entry.name)if(c==L'/')c=L'\\';
        entries.push_back(entry);
    }
    return entries;
}
static bool metadata(const std::wstring &name,const Entry &entry){
    HANDLE handle=open(name);if(handle==INVALID_HANDLE_VALUE)return false;
    FILETIME c{},a{},w{};BY_HANDLE_FILE_INFORMATION info{};FILE_BASIC_INFO basic{};WIN32_FILE_ATTRIBUTE_DATA attributes{};
    // Native NTFS may update last-access time merely by opening a directory.
    // The recording-date contract is creation/last-write; access is initialized
    // identically but deliberately follows the underlying filesystem policy.
    bool good=GetFileTime(handle,&c,&a,&w)&&ticks(c)==entry.time&&ticks(w)==entry.time&&
        GetFileInformationByHandle(handle,&info)&&ticks(info.ftCreationTime)==entry.time&&ticks(info.ftLastWriteTime)==entry.time&&info.dwFileAttributes==entry.attributes&&
        GetFileInformationByHandleEx(handle,FileBasicInfo,&basic,sizeof(basic))&&ULONGLONG(basic.CreationTime.QuadPart)==entry.time&&ULONGLONG(basic.LastWriteTime.QuadPart)==entry.time&&basic.FileAttributes==entry.attributes&&
        GetFileAttributesExW(name.c_str(),GetFileExInfoStandard,&attributes)&&ticks(attributes.ftCreationTime)==entry.time&&ticks(attributes.ftLastWriteTime)==entry.time&&attributes.dwFileAttributes==entry.attributes;
    if(!good)printf("metadata expected time=%llu attr=%lu actual create=%llu access=%llu write=%llu handle-attr=%lu basic-create=%llu basic-write=%llu basic-attr=%lu path-create=%llu path-write=%llu path-attr=%lu\n",entry.time,entry.attributes,ticks(c),ticks(a),ticks(w),info.dwFileAttributes,ULONGLONG(basic.CreationTime.QuadPart),ULONGLONG(basic.LastWriteTime.QuadPart),basic.FileAttributes,ticks(attributes.ftCreationTime),ticks(attributes.ftLastWriteTime),attributes.dwFileAttributes);
    // Exact error behavior stays native because these are real cache handles.
    BYTE byte=0;SetLastError(0);bool short_buffer=!GetFileInformationByHandleEx(handle,FileBasicInfo,&byte,1)&&GetLastError()==ERROR_BAD_LENGTH;
    CloseHandle(handle);return good&&short_buffer;
}
static void baseline(const std::wstring &root,const std::vector<Entry> &entries){
    for(const auto &entry:entries){
        auto name=path(root,entry);if(entry.name==L".")continue;
        if(entry.attributes&FILE_ATTRIBUTE_DIRECTORY)CreateDirectoryW(name.c_str(),nullptr);
        else{HANDLE file=CreateFileW(name.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,0,nullptr);if(file!=INVALID_HANDLE_VALUE)CloseHandle(file);}
    }
    for(auto i=entries.rbegin();i!=entries.rend();++i){
        HANDLE handle=open(path(root,*i),FILE_WRITE_ATTRIBUTES);FILETIME time=filetime(i->time);
        bool good=handle!=INVALID_HANDLE_VALUE&&SetFileTime(handle,&time,&time,&time);
        if(handle!=INVALID_HANDLE_VALUE)CloseHandle(handle);
        if(!good||!SetFileAttributesW(path(root,*i).c_str(),i->attributes))++failures;
    }
}
int wmain(int argc,wchar_t **argv){
    if(argc!=4&&argc!=8)return 2;
    bool native=std::wstring(argv[1])==L"--native";
    if(!native&&std::wstring(argv[1])!=L"--mounted")return 2;
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX|SEM_NOOPENFILEERRORBOX);
    std::wstring root=argv[2];auto entries=manifest(argv[3]);if(entries.empty())return 3;
    if(native)baseline(root,entries);
    for(const auto &entry:entries){
        std::string title="GetFileTime/attributes/basic info preserve ";int n=WideCharToMultiByte(CP_UTF8,0,entry.name.data(),int(entry.name.size()),nullptr,0,nullptr,nullptr);size_t at=title.size();title.resize(at+n);WideCharToMultiByte(CP_UTF8,0,entry.name.data(),int(entry.name.size()),title.data()+at,n,nullptr,nullptr);
        test(metadata(path(root,entry),entry),title.c_str());
        if(entry.name!=L"."){
            WIN32_FIND_DATAW data{};HANDLE find=FindFirstFileW(path(root,entry).c_str(),&data);
            bool good=find!=INVALID_HANDLE_VALUE&&ticks(data.ftCreationTime)==entry.time&&ticks(data.ftLastWriteTime)==entry.time&&data.dwFileAttributes==entry.attributes;
            if(find!=INVALID_HANDLE_VALUE)FindClose(find);
            test(good,"directory enumeration exposes the same recording time and hidden flag");
        }
    }
    SYSTEMTIME invalid{};invalid.wYear=1900;invalid.wMonth=2;invalid.wDay=29;FILETIME value{};SetLastError(0);
    test(!SystemTimeToFileTime(&invalid,&value)&&GetLastError()==ERROR_INVALID_PARAMETER,"native calendar rejects 1900 February29 without normalization");
    invalid.wYear=2000;test(SystemTimeToFileTime(&invalid,&value)!=FALSE,"native calendar accepts 2000 leap day");
    WIN32_FILE_ATTRIBUTE_DATA missing{};SetLastError(0);test(!GetFileAttributesExW((root+L"\\not-present.xyz").c_str(),GetFileExInfoStandard,&missing)&&GetLastError()==ERROR_FILE_NOT_FOUND,"missing media path retains native FILE_NOT_FOUND");
    if(native){
        const auto &file=*std::find_if(entries.begin(),entries.end(),[](const Entry &e){return !(e.attributes&FILE_ATTRIBUTE_DIRECTORY);});
        HANDLE handle=open(path(root,file),FILE_WRITE_ATTRIBUTES|FILE_READ_ATTRIBUTES);FILETIME zero{},c{},a{},w{};
        bool good=SetFileTime(handle,&zero,&zero,&zero)&&GetFileTime(handle,&c,&a,&w)&&ticks(w)==file.time;CloseHandle(handle);
        test(good,"zero FILETIME is a native no-op, justifying explicit unspecified-date fallback");
    }else{
        const auto &file=*std::find_if(entries.begin(),entries.end(),[](const Entry &e){return !(e.attributes&FILE_ATTRIBUTE_DIRECTORY);});
        HANDLE handle=open(path(root,file),FILE_WRITE_ATTRIBUTES);test(handle==INVALID_HANDLE_VALUE&&GetLastError()==ERROR_WRITE_PROTECT,"preserved metadata stays immutable through a mounted drive");if(handle!=INVALID_HANDLE_VALUE)CloseHandle(handle);
        const auto &hidden=*std::find_if(entries.begin(),entries.end(),[](const Entry &e){return !(e.attributes&FILE_ATTRIBUTE_DIRECTORY)&&(e.attributes&FILE_ATTRIBUTE_HIDDEN);});
        std::wstring copy=L"C:\\tests\\metadata-copy-"+std::to_wstring(GetCurrentProcessId())+L".tmp";
        WIN32_FILE_ATTRIBUTE_DATA data{};test(CopyFileW(path(root,hidden).c_str(),copy.c_str(),TRUE)&&GetFileAttributesExW(copy.c_str(),GetFileExInfoStandard,&data)&&ticks(data.ftLastWriteTime)==hidden.time&&(data.dwFileAttributes&FILE_ATTRIBUTE_HIDDEN),"copying media files keeps their last-write time and hidden flag");
    }
    if(argc==8&&!native&&std::wstring(argv[4])==L"--swap"){
        std::wstring command=L"\""+std::wstring(argv[7])+L"\" drive swap D \""+argv[5]+L"\"";
        STARTUPINFOW startup{};startup.cb=sizeof(startup);PROCESS_INFORMATION child{};DWORD code=~0u;
        bool launched=CreateProcessW(argv[7],command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&child)!=FALSE;
        if(launched){CloseHandle(child.hThread);if(WaitForSingleObject(child.hProcess,10000)==WAIT_OBJECT_0)GetExitCodeProcess(child.hProcess,&code);else TerminateProcess(child.hProcess,ERROR_TIMEOUT);CloseHandle(child.hProcess);}
        test(launched&&code==0,"live controller publishes a new complete metadata-bearing medium");
        auto next=manifest(argv[6]);bool good=!next.empty();for(const auto &entry:next)good=metadata(path(root,entry),entry)&&good;
        test(good,"fresh handles after live swap expose the replacement timestamps and attributes");
    }
    printf("Media metadata probe: %d failures\n",failures);return failures?1:0;
}
