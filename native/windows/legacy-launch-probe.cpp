// SPDX-License-Identifier: LGPL-2.1-or-later
// Console-only reproduction of legacy OpenFile/WinExec game launcher behavior.
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static std::string quote(const std::string &s) { return "\"" + s + "\""; }
static bool injected() { return GetModuleHandleA("winedows32.dll") || GetModuleHandleA("winedows64.dll"); }
static void inspect(const char *name, const char *path) {
    SetLastError(0); DWORD attrs=GetFileAttributesA(path), attr_error=GetLastError();
    OFSTRUCT ofs{};SetLastError(0);HFILE file=OpenFile(path,&ofs,OF_EXIST);DWORD error=GetLastError();
    printf("inspect=%s length=%zu attrs=%lu attr_error=%lu open_file=%d open_error=%lu ofs_error=%u ofs_path=%s\n",
        name,strlen(path),attrs,attr_error,file,error,ofs.nErrCode,ofs.szPathName);
    SetLastError(0);HANDLE handle=CreateFileA(path,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
        nullptr,OPEN_EXISTING,0,nullptr);error=GetLastError();
    printf("create_file=%s success=%d error=%lu\n",name,handle!=INVALID_HANDLE_VALUE,error);
    if(handle!=INVALID_HANDLE_VALUE)CloseHandle(handle);
    OFSTRUCT read_ofs{};SetLastError(0);HFILE read_file=OpenFile(path,&read_ofs,OF_READ|OF_SHARE_DENY_NONE);error=GetLastError();
    char bytes[8]{};UINT count=read_file==HFILE_ERROR?0:_lread(read_file,bytes,sizeof(bytes));
    if(read_file!=HFILE_ERROR)_lclose(read_file);
    printf("open_read=%s success=%d error=%lu count=%u ofs_path=%s\n",name,read_file!=HFILE_ERROR,error,count,read_ofs.szPathName);
    OFSTRUCT parse{};SetLastError(0);HFILE parsed=OpenFile(path,&parse,OF_PARSE);error=GetLastError();
    printf("open_parse=%s result=%d error=%lu fixed=%u ofs_path=%s\n",name,parsed,error,parse.fFixedDisk,parse.szPathName);
    if(read_file!=HFILE_ERROR){SetLastError(0);HFILE reopened=OpenFile("ignored",&read_ofs,OF_REOPEN|OF_READ|OF_SHARE_DENY_NONE);error=GetLastError();
        printf("open_reopen=%s success=%d error=%lu ofs_path=%s\n",name,reopened!=HFILE_ERROR,error,read_ofs.szPathName);if(reopened!=HFILE_ERROR)_lclose(reopened);}
    SetLastError(0);HFILE old=_lopen(path,OF_READ|OF_SHARE_DENY_NONE);error=GetLastError();
    printf("lopen=%s success=%d error=%lu\n",name,old!=HFILE_ERROR,error);if(old!=HFILE_ERROR)_lclose(old);
}
static void launch(const char *api,bool quoted,const std::string &path,const std::string &directory) {
    std::string name=std::string(api)+(quoted?"-quoted":"-unquoted");
    std::string report=directory+"\\"+name+".txt";
    std::string command=(quoted?quote(path):path)+" --child "+quote(report)+" \"two words\" tail";
    DWORD result=0,error=0;
    if(std::string(api)=="WinExec") {
        SetLastError(0);result=WinExec(command.c_str(),SW_HIDE);error=GetLastError();
    } else {
        STARTUPINFOA startup{};startup.cb=sizeof(startup);startup.dwFlags=STARTF_USESHOWWINDOW;startup.wShowWindow=SW_HIDE;
        PROCESS_INFORMATION pi{};std::vector<char> buffer(command.begin(),command.end());buffer.push_back(0);
        SetLastError(0);result=CreateProcessA(nullptr,buffer.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&pi);error=GetLastError();
        if(result){WaitForSingleObject(pi.hProcess,3000);CloseHandle(pi.hThread);CloseHandle(pi.hProcess);}
    }
    std::string text;
    for(unsigned i=0;i<100;++i) {
        HANDLE h=CreateFileA(report.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr);
        if(h!=INVALID_HANDLE_VALUE){char buf[1024]{};DWORD n=0;ReadFile(h,buf,sizeof(buf)-1,&n,nullptr);CloseHandle(h);text.assign(buf,n);if(!text.empty())break;}
        Sleep(10);
    }
    printf("launch=%s result=%lu error=%lu child_report=%s\n",name.c_str(),result,error,text.empty()?"MISSING":text.c_str());
}
static void error_case(const char *name,const std::string &command) {
    STARTUPINFOA startup{};startup.cb=sizeof(startup);PROCESS_INFORMATION pi{};
    std::vector<char> buffer(command.begin(),command.end());buffer.push_back(0);
    SetLastError(0);BOOL ok=CreateProcessA(nullptr,buffer.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&pi);DWORD error=GetLastError();
    if(ok){WaitForSingleObject(pi.hProcess,3000);CloseHandle(pi.hThread);CloseHandle(pi.hProcess);}
    printf("error_case=%s api=CreateProcessA result=%d error=%lu\n",name,ok,error);
    SetLastError(0);UINT ret=WinExec(command.c_str(),SW_HIDE);error=GetLastError();
    printf("error_case=%s api=WinExec result=%u error=%lu\n",name,ret,error);
}
int main(int argc,char **argv) {
    int child=0;for(int i=1;i+1<argc;++i)if(!strcmp(argv[i],"--child")){child=i;break;}
    if(child) {
        char module[32768]{};GetModuleFileNameA(nullptr,module,sizeof(module));
        char output[32768];int size=snprintf(output,sizeof(output),"injected=%d bits=%zu argc=%d child_index=%d arg3=%s arg4=%s module=%s",injected(),sizeof(void*)*8,argc,child,argc>child+2?argv[child+2]:"",argc>child+3?argv[child+3]:"",module);
        HANDLE h=CreateFileA(argv[child+1],GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_NEW,0,nullptr);
        if(h==INVALID_HANDLE_VALUE)return 10;
        DWORD written=0;BOOL ok=WriteFile(h,output,size,&written,nullptr);CloseHandle(h);return ok?0:11;
    }
    if(argc==3&&!strcmp(argv[1],"--inspect")){inspect("requested",argv[2]);return 0;}
    if(argc==3&&!strcmp(argv[1],"--errors")){
        error_case("empty","");error_case("space"," ");error_case("leading_space"," "+std::string(argv[2]));
        error_case("leading_tab","\t"+std::string(argv[2]));error_case("quoted_empty","\"\"");error_case("quoted_space","\" \"");error_case("quoted_tab","\"\t\"");
        error_case("missing",quote(std::string(argv[2])+".missing"));error_case("missing_directory",quote(std::string(argv[2])+"\\no-such\\missing.exe"));
        error_case("bad_image",quote(argv[2]));return 0;
    }
    if(argc==5&&!strcmp(argv[1],"--openfile")){
        if(!SetCurrentDirectoryA(argv[2]))return 3;
        inspect("logical",argv[3]);inspect("physical",argv[4]);inspect("relative","fixture.dat");
        inspect("search_appdir","appdir-only.dat");inspect("missing","absent.dat");return 0;
    }
    if(argc==3&&!strcmp(argv[1],"--policy")){
        inspect("readonly",argv[2]);
        for(UINT mode:{UINT(OF_CREATE|OF_WRITE),UINT(OF_DELETE)}){
            OFSTRUCT ofs{};SetLastError(0);HFILE h=OpenFile(argv[2],&ofs,mode);DWORD error=GetLastError();
            printf("policy mode=%u success=%d error=%lu\n",mode,h!=HFILE_ERROR,error);
            if(h!=HFILE_ERROR&&mode!=OF_DELETE)_lclose(h);
        }
        return 0;
    }
    bool with_cwd=argc==6&&!strcmp(argv[1],"--parent-cwd");
    if(with_cwd&&!SetCurrentDirectoryA(argv[5]))return 4;
    if(!with_cwd&&(argc!=5||strcmp(argv[1],"--parent"))){fprintf(stderr,"--parent LOGICAL_CHILD PHYSICAL_CHILD REPORT_DIRECTORY | --inspect PATH\n");return 2;}
    printf("parent_injected=%d\n",injected());inspect("logical",argv[2]);inspect("physical",argv[3]);
    launch("CreateProcessA",false,argv[2],argv[4]);launch("CreateProcessA",true,argv[2],argv[4]);
    launch("WinExec",false,argv[2],argv[4]);launch("WinExec",true,argv[2],argv[4]);
    return 0;
}
