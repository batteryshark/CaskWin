// SPDX-License-Identifier: LGPL-2.1-or-later
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <string>
int wmain(int argc,wchar_t **argv){
    if(argc==2&&!wcscmp(argv[1],L"--noop")){puts("PASS native-only executable entry point");return 0;}
    if(argc!=6)return 2;SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
    std::wstring command=L"\""+std::wstring(argv[1])+L"\" \""+argv[2]+L"\" \""+argv[3]+L"\" \""+argv[4]+L"\"";
    std::wstring environment=L"WINEDLLOVERRIDES="+std::wstring(argv[5]);environment.push_back(0);environment+=L"BUILTIN_CHILD_FIXTURE=1";environment.push_back(0);environment.push_back(0);
    STARTUPINFOW start{};start.cb=sizeof(start);PROCESS_INFORMATION child{};
    if(!CreateProcessW(argv[1],command.data(),nullptr,nullptr,FALSE,CREATE_UNICODE_ENVIRONMENT,environment.data(),nullptr,&start,&child)){printf("FAIL child creation error=%lu\n",GetLastError());return 1;}
    DWORD code=124;if(WaitForSingleObject(child.hProcess,30000)==WAIT_OBJECT_0)GetExitCodeProcess(child.hProcess,&code);else TerminateProcess(child.hProcess,124);
    CloseHandle(child.hThread);CloseHandle(child.hProcess);printf("CHILD %zu-bit parent exit=%08lx\n",sizeof(void*)*8,code);return code?1:0;
}
