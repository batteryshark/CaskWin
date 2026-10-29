// SPDX-License-Identifier: LGPL-2.1-or-later
#include "startup-imports.h"
#include "detours.h"
#include <cwchar>
#include <cstring>
#include <cstdlib>
static std::wstring quote(const std::wstring &s){return L"\""+s+L"\"";}
static std::string ansi(const std::wstring &s){BOOL lossy=FALSE;int n=WideCharToMultiByte(CP_ACP,WC_NO_BEST_FIT_CHARS,s.c_str(),-1,nullptr,0,nullptr,&lossy);if(!n||lossy)throw std::runtime_error("Cannot represent fixture path");std::string out(n,0);if(!WideCharToMultiByte(CP_ACP,WC_NO_BEST_FIT_CHARS,s.c_str(),-1,out.data(),n,nullptr,&lossy)||lossy)throw std::runtime_error("Cannot represent fixture path");out.pop_back();return out;}
static DWORD WINAPI watchdog(void*){Sleep(45000);TerminateProcess(GetCurrentProcess(),124);return 0;}
int wmain(int argc,wchar_t **argv){
    setvbuf(stdout,nullptr,_IONBF,0);SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);HANDLE timer=CreateThread(nullptr,0,watchdog,nullptr,0,nullptr);if(timer)CloseHandle(timer);
    if(argc<10){fputs("startup-imports-probe HELPER CHILD MACHINE PRIVATE OTHER CUSTOM VALUE FILE COUNT [--builtin DLL] [--reject-plan] [--omit-preload] [--tamper-directory DIR] [--custom-environment]\n",stderr);return 2;}
    PROCESS_INFORMATION process{};bool expected_plan_rejection=false,expected_bind_rejection=false;
    try{
        std::vector<wd::str> builtins;const wchar_t *tamper=nullptr,*candidate_source=nullptr,*candidate_target=nullptr,*map_from=nullptr,*map_to=nullptr;bool omit=false,custom_environment=false,unsupported_only=false,allow_unsupported=false,clear_directory=false,empty_directory=false,reject_directory=false;
        for(int i=10;i<argc;i++){
            if(!wcscmp(argv[i],L"--builtin")&&i+1<argc)builtins.emplace_back(argv[++i]);
            else if(!wcscmp(argv[i],L"--reject-plan"))expected_plan_rejection=true;
            else if(!wcscmp(argv[i],L"--omit-preload")){omit=true;expected_bind_rejection=true;}
            else if(!wcscmp(argv[i],L"--tamper-directory")&&i+1<argc){tamper=argv[++i];expected_bind_rejection=true;}
            else if(!wcscmp(argv[i],L"--custom-environment"))custom_environment=true;
            else if(!wcscmp(argv[i],L"--unsupported-plan"))unsupported_only=true;
            else if(!wcscmp(argv[i],L"--allow-unsupported"))allow_unsupported=true;
            else if(!wcscmp(argv[i],L"--clear-directory"))clear_directory=true;
            else if(!wcscmp(argv[i],L"--empty-directory"))empty_directory=true;
            else if(!wcscmp(argv[i],L"--reject-directory"))reject_directory=true;
            else if(!wcscmp(argv[i],L"--add-candidate")&&i+2<argc){candidate_source=argv[++i];candidate_target=argv[++i];expected_bind_rejection=true;}
            else if(!wcscmp(argv[i],L"--map-custom")&&i+2<argc){map_from=argv[++i];map_to=argv[++i];}
            else return 2;
        }
        BOOL directory_set=SetDllDirectoryW(clear_directory?nullptr:empty_directory?L"":argv[6]);DWORD directory_error=GetLastError();
        if(reject_directory){if(directory_set||directory_error!=ERROR_INVALID_PARAMETER)throw std::runtime_error("Expected native semicolon directory rejection");puts("PASS native SetDllDirectory rejects a semicolon path list with ERROR_INVALID_PARAMETER");return 0;}
        if(!directory_set)throw std::runtime_error("SetDllDirectory fixture failed");
        wd::str app=argv[2],cwd=app.substr(0,app.find_last_of(L"\\/"));WORD machine=(WORD)wcstoul(argv[3],nullptr,16);
        wd::StartupDirectoryMapper mapper;if(map_from)mapper=[&](const wd::str &path){return wd::StartupDirectoryPaths{!_wcsicmp(path.c_str(),map_from)?wd::str(map_to):path,path};};
        auto plan=wd::plan_native_startup_imports(app,machine,cwd,argv[4],argv[5],builtins,mapper);
        auto builtin_plan=wd::plan_builtin_startup_imports(app,machine,cwd,argv[4],argv[5],builtins,mapper);
        if(unsupported_only){if(plan.unsupported_reason.empty()||!plan.bindings.empty())throw std::runtime_error("Expected an explicit empty unsupported plan");printf("PASS native-only unsupported plan: %s\n",plan.unsupported_reason.c_str());return 0;}
        if(allow_unsupported&&plan.unsupported_reason.empty())throw std::runtime_error("Expected native-only diagnostic before unchanged native launch");
        if(!plan.unsupported_reason.empty()){printf("NATIVE-ONLY %s\n",plan.unsupported_reason.c_str());if(!allow_unsupported)throw std::runtime_error("Unexpected unsupported plan");}
        if(expected_plan_rejection){fputs("FAIL unsafe dependency plan was accepted\n",stderr);return 1;}
        size_t expected=wcstoul(argv[9],nullptr,10);printf("PLAN %zu bindings, expected %zu\n",plan.bindings.size(),expected);if(plan.bindings.size()!=expected)return 1;
        for(const auto &binding:plan.bindings)wprintf(L"NATIVE %ls ordinal=%u symbol=%hs\n",binding.file.path.c_str(),binding.ordinal,binding.symbol.c_str());
        std::vector<std::string> dlls{ansi(argv[1])};for(const auto &builtin:builtins)dlls.push_back(ansi(builtin));if(!omit)for(const auto &binding:plan.bindings)dlls.push_back(ansi(binding.file.path));std::vector<const char*> pointers;for(const auto &dll:dlls)pointers.push_back(dll.c_str());
        SetEnvironmentVariableW(L"WD_STARTUP_SENTINEL",L"prefix-env-preserved");std::wstring long_value(4097,L'x');SetEnvironmentVariableW(L"WD_STARTUP_LONG",long_value.c_str());SetEnvironmentVariableW(L"WD_STARTUP_EXPECTED_DLL_DIRECTORY",clear_directory||empty_directory?nullptr:argv[6]);SetEnvironmentVariableW(L"WD_STARTUP_EXPECT_EMPTY_DLL_DIRECTORY",clear_directory||empty_directory?L"1":nullptr);
        std::wstring env=L"WD_STARTUP_CUSTOM=custom";env.push_back(0);env+=L"WD_STARTUP_EXPECTED_DLL_DIRECTORY=";env+=argv[6];env.push_back(0);env+=L"WD_STARTUP_LONG="+long_value;env.push_back(0);env+=L"WD_STARTUP_SENTINEL=prefix-env-preserved";env.push_back(0);env.push_back(0);
        if(tamper&&!SetDllDirectoryW(tamper))throw std::runtime_error("Cannot change inherited directory for race fixture");
        if(candidate_source&&!CopyFileW(candidate_source,candidate_target,FALSE))throw std::runtime_error("Cannot create higher-priority candidate race fixture");
        bool builtin=false;for(const auto &file:builtins)if(!_wcsicmp(file.c_str(),argv[8]))builtin=true;std::wstring command=quote(app)+L" "+argv[7]+L" "+quote(argv[8])+(builtin?L" builtin":L" native");if(custom_environment)command+=L" --require-custom";
        STARTUPINFOW start{};start.cb=sizeof(start);
        if(!DetourCreateProcessWithDllsW(app.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_SUSPENDED|CREATE_UNICODE_ENVIRONMENT,custom_environment?env.data():nullptr,cwd.c_str(),&start,&process,(DWORD)pointers.size(),pointers.data(),nullptr))throw std::runtime_error("Detours child creation failed: "+std::to_string(GetLastError()));
        wd::bind_native_startup_imports(process.hProcess,plan);
        wd::bind_builtin_startup_imports(process.hProcess,builtin_plan);
        if(expected_bind_rejection)throw std::runtime_error("FAIL unsafe binding unexpectedly accepted");
        if(WaitForSingleObject(process.hProcess,0)!=WAIT_TIMEOUT||ResumeThread(process.hThread)!=1)throw std::runtime_error("Child suspension count was changed");
        DWORD code=124;if(WaitForSingleObject(process.hProcess,30000)==WAIT_OBJECT_0)GetExitCodeProcess(process.hProcess,&code);else TerminateProcess(process.hProcess,124);
        printf("CHILD %08lx\n",code);CloseHandle(process.hThread);CloseHandle(process.hProcess);return code?1:0;
    }catch(const std::exception &error){
        bool had_process=process.hProcess!=nullptr;if(process.hProcess){TerminateProcess(process.hProcess,125);CloseHandle(process.hThread);CloseHandle(process.hProcess);}
        bool expected=(expected_plan_rejection&&!had_process)||(expected_bind_rejection&&had_process&&strncmp(error.what(),"Native startup imports:",23)==0);
        printf("%s rejection: %s\n",expected?"PASS expected":"FAIL",error.what());return expected?0:1;
    }
}
