// SPDX-License-Identifier: LGPL-2.1-or-later
// Session lifetime controls; the outer-job mode itself never touches registry.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdio>
#include <string>
#include <stdexcept>

using str=std::wstring;
static void check(bool ok,const char *text){printf("%s %s (error=%lu)\n",ok?"PASS":"FAIL",text,GetLastError());if(!ok)throw std::runtime_error(text);}
static str env(const wchar_t *name){wchar_t value[32768];DWORD n=GetEnvironmentVariableW(name,value,32768);return n&&n<32768?str(value,n):str();}
static str quote(const str &value){
    str out=L"\"";unsigned slashes=0;
    for(wchar_t c:value){if(c==L'\\'){++slashes;continue;}out.append(c==L'\"'?slashes*2+1:slashes,L'\\');slashes=0;out+=c;}
    out.append(slashes*2,L'\\');return out+L"\"";
}
static int outer_job(int argc,wchar_t **argv){
    check(argc>=4,"outer-job requires explicit executable and arguments");
    HANDLE job=CreateJobObjectW(nullptr,nullptr);check(job!=nullptr,"create owned outer job");
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limit{};limit.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    check(SetInformationJobObject(job,JobObjectExtendedLimitInformation,&limit,sizeof(limit))!=FALSE,"configure owned outer job");
    str command;for(int i=2;i<argc;i++){if(i>2)command+=L" ";command+=quote(argv[i]);}
    STARTUPINFOW si{};si.cb=sizeof(si);si.dwFlags=STARTF_USESTDHANDLES;si.hStdInput=GetStdHandle(STD_INPUT_HANDLE);si.hStdOutput=GetStdHandle(STD_OUTPUT_HANDLE);si.hStdError=GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION pi{};
    check(CreateProcessW(argv[2],command.data(),nullptr,nullptr,TRUE,CREATE_SUSPENDED,nullptr,nullptr,&si,&pi)!=FALSE,"create own suspended launcher");
    if(!AssignProcessToJobObject(job,pi.hProcess)){TerminateProcess(pi.hProcess,101);CloseHandle(pi.hThread);CloseHandle(pi.hProcess);CloseHandle(job);throw std::runtime_error("assign launcher to independent outer job");}
    check(ResumeThread(pi.hThread)!=(DWORD)-1,"resume own launcher");CloseHandle(pi.hThread);
    printf("OUTER launcher_pid=%lu\n",pi.dwProcessId);
    DWORD status=WaitForSingleObject(pi.hProcess,60000),code=124;
    if(status==WAIT_OBJECT_0)GetExitCodeProcess(pi.hProcess,&code);else TerminateJobObject(job,124);
    CloseHandle(pi.hProcess);CloseHandle(job);check(status==WAIT_OBJECT_0,"outer launcher exited within deadline");return (int)code;
}
static str marker_path(const str &token,const str &role,const wchar_t *suffix){return L"C:\\tests\\"+token+L"-"+role+suffix;}
static void marker(const str &token,const str &role,DWORD child=0){
    HANDLE h=CreateFileW(marker_path(token,role,L".ready").c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_ALWAYS,0,nullptr);check(h!=INVALID_HANDLE_VALUE,"create lifetime marker");
    str media=env(L"WINEDOWS_MEDIA_SESSION");std::string text="pid="+std::to_string(GetCurrentProcessId())+"\nchild="+std::to_string(child)+"\nmedia=";
    for(wchar_t c:media){if(c<=0||c>=128)throw std::runtime_error("non-ASCII media identity");text.push_back((char)c);}text+='\n';
    DWORD n=0;BOOL ok=WriteFile(h,text.data(),(DWORD)text.size(),&n,nullptr);CloseHandle(h);check(ok&&n==text.size(),"publish lifetime marker");
}
static void value(const str &token,const wchar_t *name,DWORD data){
    HKEY h=nullptr;str key=L"Software\\CaskWin\\SessionJob\\"+token;
    check(RegCreateKeyExW(HKEY_CURRENT_USER,key.c_str(),0,nullptr,0,KEY_SET_VALUE|KEY_WOW64_64KEY,nullptr,&h,nullptr)==ERROR_SUCCESS,"create private lifecycle key");
    LSTATUS s=RegSetValueExW(h,name,0,REG_DWORD,(const BYTE*)&data,sizeof(data));RegCloseKey(h);check(s==ERROR_SUCCESS,"write private lifecycle value");
}
static void read_owner(const str &token){
    HKEY h=nullptr;str key=L"Software\\CaskWin\\SessionJob\\"+token;
    check(RegOpenKeyExW(HKEY_CURRENT_USER,key.c_str(),0,KEY_QUERY_VALUE|KEY_WOW64_64KEY,&h)==ERROR_SUCCESS,"observer opens live lifecycle registry");
    DWORD v=0,size=sizeof(v),type=0;LSTATUS s=RegQueryValueExW(h,L"owner",nullptr,&type,(BYTE*)&v,&size);RegCloseKey(h);check(s==ERROR_SUCCESS&&type==REG_DWORD&&v==1,"observer sees surviving owner state");
}
static void wait_stop(const str &token,const str &role){
    ULONGLONG deadline=GetTickCount64()+45000;
    while(GetFileAttributesW(marker_path(token,role,L".stop").c_str())==INVALID_FILE_ATTRIBUTES){if(GetTickCount64()>=deadline)throw std::runtime_error("lifecycle stop deadline");Sleep(20);}
}
int wmain(int argc,wchar_t **argv){
    setvbuf(stdout,nullptr,_IONBF,0);
    try{
        if(argc>1&&str(argv[1])==L"--outer-job")return outer_job(argc,argv);
        check(argc==5,"expected role token exitcode child-executable");str role=argv[1],token=argv[2];int code=_wtoi(argv[3]);
        check(token.size()==32&&token.find_first_not_of(L"0123456789abcdef")==str::npos,"safe lifetime token");
#ifdef _WIN64
        const wchar_t *shim=L"winedows64.dll";
#else
        const wchar_t *shim=L"winedows32.dll";
#endif
        check(GetModuleHandleW(shim)&&!env(L"WINEDOWS_PREFIX").empty()&&!env(L"WINEDOWS_MEDIA_SESSION").empty(),"injection required before registry mutation");
        if(role==L"inspect"){read_owner(token);return code;}
        check(role==L"owner"||role==L"joined"||role==L"leaf"||role==L"tree","known lifecycle role");
        value(token,role.c_str(),1);
        if(role==L"tree"){
            str command=quote(argv[4])+L" leaf "+token+L" 49 unused";STARTUPINFOW si{};si.cb=sizeof(si);PROCESS_INFORMATION pi{};
            check(CreateProcessW(argv[4],command.data(),nullptr,nullptr,FALSE,0,nullptr,nullptr,&si,&pi)!=FALSE,"joined application starts descendant");
            CloseHandle(pi.hThread);marker(token,role,pi.dwProcessId);CloseHandle(pi.hProcess);wait_stop(token,role);return code;
        }
        marker(token,role);
        wait_stop(token,role);
        value(token,(role+L"Done").c_str(),1);return code;
    }catch(const std::exception &e){fprintf(stderr,"session-job-probe: %s\n",e.what());return 100;}
}
