// SPDX-License-Identifier: LGPL-2.1-or-later
// Console diagnostic wrapper: valid GUI startup handles, bounded saved output.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <atomic>
#include <cstdio>
#include <string>
#include <vector>
static constexpr unsigned long long log_limit=16ull*1024*1024;
struct Handle {HANDLE value=INVALID_HANDLE_VALUE;Handle()=default;explicit Handle(HANDLE h):value(h){}~Handle(){close();}void close(){if(value&&value!=INVALID_HANDLE_VALUE)CloseHandle(value);value=INVALID_HANDLE_VALUE;}Handle(const Handle&)=delete;Handle&operator=(const Handle&)=delete;};
struct Capture {
    HANDLE pipe,log;std::atomic<unsigned long long> received{0},saved{0};std::atomic<DWORD> read_error{0},write_error{0};std::atomic<bool> stop{false};
};
static DWORD WINAPI drain(void *argument){
    auto &capture=*(Capture*)argument;char bytes[32768];DWORD count=0;
    while(!capture.stop.load()){
        if(!ReadFile(capture.pipe,bytes,sizeof(bytes),&count,nullptr)){DWORD error=GetLastError();if(error!=ERROR_BROKEN_PIPE&&error!=ERROR_OPERATION_ABORTED)capture.read_error=error;break;}
        if(!count)break;capture.received.fetch_add(count);auto saved=capture.saved.load();
        DWORD keep=(DWORD)((log_limit-saved<count)?log_limit-saved:count);
        if(capture.write_error.load())continue;
        DWORD written=0;
        while(written<keep){DWORD part=0;if(!WriteFile(capture.log,bytes+written,keep-written,&part,nullptr)||!part){capture.write_error=GetLastError()?GetLastError():ERROR_WRITE_FAULT;break;}written+=part;capture.saved.fetch_add(part);}
    }
    return 0;
}
static bool absolute(const std::wstring &value){return (value.size()>2&&value[1]==L':'&&(value[2]==L'\\'||value[2]==L'/'))||(value.size()>2&&value[0]==L'\\'&&value[1]==L'\\');}
static std::wstring quote(const wchar_t *argument){
    std::wstring result=L"\"";unsigned slashes=0;
    for(const wchar_t *p=argument;*p;++p){if(*p==L'\\'){++slashes;continue;}if(*p==L'\"'){result.append(slashes*2+1,L'\\');result+=*p;}else{result.append(slashes,L'\\');result+=*p;}slashes=0;}
    result.append(slashes*2,L'\\');return result+L'\"';
}
static bool status(HANDLE file,const Capture &capture,DWORD pid,bool running,DWORD exit_code,bool incomplete,DWORD launch_error){
    // Read the monotonically increasing saved count first; bytes are counted
    // as received before writes, so the snapshot cannot underflow discarded.
    unsigned long long saved=capture.saved.load(),received=capture.received.load();
    char text[1024];int count=snprintf(text,sizeof(text),"{\n  \"childPid\": %lu,\n  \"running\": %s,\n  \"childExitCode\": %lu,\n  \"launchError\": %lu,\n  \"limitBytes\": %llu,\n  \"receivedBytes\": %llu,\n  \"savedBytes\": %llu,\n  \"discardedBytes\": %llu,\n  \"truncated\": %s,\n  \"drainIncomplete\": %s,\n  \"readError\": %lu,\n  \"writeError\": %lu\n}\n",pid,running?"true":"false",exit_code,launch_error,log_limit,received,saved,received-saved,received>log_limit?"true":"false",incomplete?"true":"false",capture.read_error.load(),capture.write_error.load());
    LARGE_INTEGER zero{};DWORD written=0;return SetFilePointerEx(file,zero,nullptr,FILE_BEGIN)&&WriteFile(file,text,(DWORD)count,&written,nullptr)&&written==(DWORD)count&&SetEndOfFile(file)&&FlushFileBuffers(file);
}
int wmain(int argc,wchar_t **argv){
    if(argc<3){fputs("Usage: gui-log-capture <absolute-log-path> <explicit-child-exe> [arguments...]\nCreates a new raw log and .status.json; captures up to16MiB, drains excess.\n",stderr);return 2;}
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);setvbuf(stdout,nullptr,_IONBF,0);
    std::wstring log_path=argv[1],application=argv[2];if(!absolute(log_path)||!absolute(application)){fputs("Both log and child paths must be explicit absolute paths.\n",stderr);return 2;}
    std::wstring command;for(int n=2;n<argc;n++){if(n!=2)command+=L' ';command+=quote(argv[n]);}
    if(command.size()>=32767){fputs("Child command line exceeds Windows limit.\n",stderr);return 2;}
    std::wstring directory=application.substr(0,application.find_last_of(L"\\/"));if(directory.size()==2&&directory[1]==L':')directory+=L'\\';
    Handle log(CreateFileW(log_path.c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));
    if(log.value==INVALID_HANDLE_VALUE){fprintf(stderr,"Cannot create new capture log: %lu\n",GetLastError());return 1;}
    Handle report(CreateFileW((log_path+L".status.json").c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));
    if(report.value==INVALID_HANDLE_VALUE){fprintf(stderr,"Cannot create new capture status: %lu\n",GetLastError());return 1;}
    SECURITY_ATTRIBUTES security{sizeof(security),nullptr,TRUE};Handle read_pipe,write_pipe,input;
    if(!CreatePipe(&read_pipe.value,&write_pipe.value,&security,65536)||!SetHandleInformation(read_pipe.value,HANDLE_FLAG_INHERIT,0)){fprintf(stderr,"Cannot create capture pipe: %lu\n",GetLastError());return 1;}
    HANDLE original_input=GetStdHandle(STD_INPUT_HANDLE);
    if(!original_input||original_input==INVALID_HANDLE_VALUE||!DuplicateHandle(GetCurrentProcess(),original_input,GetCurrentProcess(),&input.value,0,TRUE,DUPLICATE_SAME_ACCESS))input.value=CreateFileW(L"NUL",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,&security,OPEN_EXISTING,0,nullptr);
    if(input.value==INVALID_HANDLE_VALUE){fprintf(stderr,"Cannot supply valid child input: %lu\n",GetLastError());return 1;}
    Capture capture{read_pipe.value,log.value};Handle worker(CreateThread(nullptr,0,drain,&capture,0,nullptr));if(!worker.value){fprintf(stderr,"Cannot start capture reader: %lu\n",GetLastError());return 1;}
    STARTUPINFOW startup{};startup.cb=sizeof(startup);startup.dwFlags=STARTF_USESTDHANDLES;startup.hStdOutput=startup.hStdError=write_pipe.value;startup.hStdInput=input.value;PROCESS_INFORMATION child{};
    BOOL created=CreateProcessW(application.c_str(),command.data(),nullptr,nullptr,TRUE,0,nullptr,directory.c_str(),&startup,&child);DWORD launch_error=created?0:GetLastError();write_pipe.close();input.close();
    Handle process(child.hProcess),thread(child.hThread);DWORD exit_code=1;bool incomplete=false,report_ok=true;
    if(created){
        printf("CAPTURE childPid=%lu limitBytes=%llu\n",child.dwProcessId,log_limit);report_ok=status(report.value,capture,child.dwProcessId,true,STILL_ACTIVE,false,0);
        for(;;){DWORD result=WaitForSingleObject(process.value,500);if(result==WAIT_OBJECT_0)break;if(result==WAIT_FAILED){launch_error=GetLastError();break;}report_ok=status(report.value,capture,child.dwProcessId,true,STILL_ACTIVE,false,0)&&report_ok;}
        if(!GetExitCodeProcess(process.value,&exit_code))launch_error=GetLastError();
    }else fprintf(stderr,"Cannot launch explicit GUI child: %lu\n",launch_error);
    if(WaitForSingleObject(worker.value,2000)!=WAIT_OBJECT_0){incomplete=true;capture.stop=true;CancelSynchronousIo(worker.value);if(WaitForSingleObject(worker.value,5000)!=WAIT_OBJECT_0){fputs("Capture reader did not stop after cancellation.\n",stderr);ExitProcess(1);}}
    FlushFileBuffers(log.value);report_ok=status(report.value,capture,child.dwProcessId,false,exit_code,incomplete,launch_error)&&report_ok;
    printf("CAPTURE childExitCode=%lu receivedBytes=%llu savedBytes=%llu discardedBytes=%llu truncated=%u drainIncomplete=%u\n",exit_code,capture.received.load(),capture.saved.load(),capture.received.load()-capture.saved.load(),capture.received.load()>log_limit,incomplete);
    if(launch_error||capture.read_error||capture.write_error||!report_ok)return 1;return (int)exit_code;
}
