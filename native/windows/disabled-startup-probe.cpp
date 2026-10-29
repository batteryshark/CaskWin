// SPDX-License-Identifier: LGPL-2.1-or-later
#include "startup-imports.h"
#include <cstdio>
#include <cwchar>
int wmain(int argc,wchar_t **argv){
    setvbuf(stdout,nullptr,_IONBF,0);SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX|SEM_NOOPENFILEERRORBOX);
    if(argc<11)return 2;
    bool reject=!wcscmp(argv[10],L"reject"),recheck_reject=false,run=false,validated=false;
    std::vector<wd::str> preloads,disabled;wd::DisabledStartupOptions options;options.child_path=argv[8];
    if(wcscmp(argv[9],L"-"))disabled.push_back(argv[9]);
    const wchar_t *new_directory=nullptr,*copy_source=nullptr,*copy_target=nullptr;
    for(int i=11;i<argc;i++){
        if(!wcscmp(argv[i],L"--preload")&&i+1<argc)preloads.emplace_back(argv[++i]);
        else if(!wcscmp(argv[i],L"--run"))run=true;
        else if(!wcscmp(argv[i],L"--change-directory")&&i+1<argc){new_directory=argv[++i];recheck_reject=true;}
        else if(!wcscmp(argv[i],L"--copy-candidate")&&i+2<argc){copy_source=argv[++i];copy_target=argv[++i];recheck_reject=true;}
        else return 2;
    }
    try{
        if(!SetDllDirectoryW(wcscmp(argv[6],L"-")?argv[6]:nullptr))throw std::runtime_error("fixture SetDllDirectory failed");
        auto result=wd::validate_disabled_startup_imports(argv[1],(WORD)wcstoul(argv[2],nullptr,16),argv[7],argv[3],argv[4],preloads,disabled,options);
        validated=true;printf("VALIDATED files=%zu absent=%zu\n",result.observed_files.size(),result.absent_candidates.size());
        if(reject){puts("FAIL disabled/unknown startup graph was accepted");return 1;}
        if(new_directory&&!SetDllDirectoryW(new_directory))throw std::runtime_error("fixture directory change failed");
        if(copy_source&&!CopyFileW(copy_source,copy_target,FALSE))throw std::runtime_error("fixture candidate copy failed");
        wd::recheck_disabled_startup_imports(result);
        if(recheck_reject){puts("FAIL changed startup graph was accepted");return 1;}
        if(run){
            if(!SetEnvironmentVariableW(L"PATH",argv[8]))throw std::runtime_error("fixture child PATH setup failed");
            std::wstring line=L"\""+std::wstring(argv[1])+L"\"";STARTUPINFOW start{};start.cb=sizeof(start);PROCESS_INFORMATION child{};
            if(!CreateProcessW(argv[1],line.data(),nullptr,nullptr,FALSE,0,nullptr,argv[7],&start,&child))throw std::runtime_error("native baseline child creation failed");
            DWORD code=124;if(WaitForSingleObject(child.hProcess,10000)==WAIT_OBJECT_0)GetExitCodeProcess(child.hProcess,&code);else TerminateProcess(child.hProcess,124);
            CloseHandle(child.hThread);CloseHandle(child.hProcess);printf("NATIVE_CHILD %lu\n",code);if(code)return 1;
        }
        puts("PASS disabled startup graph accepted and rechecked");return 0;
    }catch(const std::exception &e){
        bool expected=(!validated&&reject)||(validated&&recheck_reject);printf("%s %s\n",expected?"PASS expected rejection:":"FAIL",e.what());return expected?0:1;
    }
}
