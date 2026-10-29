// SPDX-License-Identifier: LGPL-2.1-or-later
#include "common.h"
#include "desktop.h"
#include "pe-architecture.h"
#include "startup-native.h"
#include "startup-imports.h"
#include "modules.h"
#include "media-state.h"
#include "media-controller.h"
#include "media-notifications.h"
#include "prefix-session.h"
#include "detours.h"
#include <dbghelp.h>
using namespace wd;
static void usage() {
    puts("CaskWin - native Windows backend for Wine (experimental)\n"
         "wine64 [--prefix DIR] init\n"
         "wine64 [--prefix DIR] run PROGRAM [ARG...]\n"
         "wine64 [--prefix DIR] --desktop [NAME,]WIDTHxHEIGHT run PROGRAM [ARG...]\n"
         "wine64 [--prefix DIR] PROGRAM [ARG...]\n"
         "wine64 [--prefix DIR] drive list\n"
         "wine64 [--prefix DIR] drive map LETTER DIRECTORY fixed|cdrom|removable [LABEL]\n"
         "wine64 [--prefix DIR] drive mount LETTER IMAGE.iso|IMAGE.cue [LABEL]\n"
         "wine64 [--prefix DIR] drive audio LETTER PREPARED_MANIFEST.ini\n"
         "wine64 [--prefix DIR] drive swap LETTER IMAGE.iso|IMAGE.cue [LABEL]\n"
         "wine64 [--prefix DIR] drive eject|load LETTER\n"
         "wine64 [--prefix DIR] path WINDOWS_PATH\n"
         "wine64 [--prefix DIR] reg import FILE.reg [/reg:32|/reg:64]\n"
         "wine64 [--prefix DIR] reg export\n"
         "wine64 gpu\n"
         "WINEPREFIX is honored. External drive mappings are read-only.\n"
         "This provides game-state separation, not a security sandbox.");
}
int wmain(int argc,wchar_t **argv) {
    try {
        int i=1; str p=env(L"WINEPREFIX"),desktop;
        while(i<argc&&(str(argv[i])==L"--prefix"||str(argv[i])==L"--desktop")){
            if(i+1>=argc)throw std::runtime_error("Missing loader option value");
            if(str(argv[i])==L"--prefix")p=argv[i+1];else desktop=argv[i+1];i+=2;
        }
        if(i==argc || str(argv[i])==L"--help") {usage();return 0;}
        str cmd=argv[i++];
        if(cmd==L"--version") {puts("CaskWin 0.11-alpha / native Windows backend");return 0;}
        if(cmd==L"gpu")return graphics_probe();
        if(p.empty())p=env(L"USERPROFILE")+L"\\.winedows";
        Prefix prefix(p);
        if(cmd==L"path") {if(i>=argc)throw std::runtime_error("Missing path");printf("%s\n",utf8(prefix.translate(argv[i])).c_str());return 0;}
        // Live commands go to the existing lock owner, which alone prepares and
        // publishes new media. Persistent drive configuration remains separate.
        if(cmd==L"drive"&&i<argc&&(str(argv[i])==L"swap"||str(argv[i])==L"eject"||str(argv[i])==L"load")){
            str operation=argv[i++];bool swap=operation==L"swap";
            if((swap&&(argc-i<2||argc-i>3))||(!swap&&argc-i!=1))
                throw std::runtime_error(swap?"Expected LETTER IMAGE.iso|IMAGE.cue [LABEL]":"Expected LETTER");
            str letter=argv[i++];wchar_t upper=letter.empty()?0:towupper(letter[0]);
            if(upper<L'A'||upper>L'Z'||(letter.size()!=1&&(letter.size()!=2||letter[1]!=L':')))
                throw std::runtime_error("Expected a drive letter A through Z");
            str source=swap?str(argv[i++]):str(),label=i<argc?argv[i]:L"";
            auto result=control_media(prefix.root,swap?MediaControl::swap:operation==L"eject"?MediaControl::eject:MediaControl::load,upper-L'A',source,label);
            printf("%s (change %lu)\n",utf8(result.message).c_str(),result.changes);return 0;
        }
        bool maintenance=cmd==L"init"||cmd==L"wineboot"||cmd==L"drive"||cmd==L"reg";
        PrefixSession session(prefix,!maintenance);
        // Another loader may have initialized the prefix while we waited for
        // admission. Joiners must never import text hives or reset live state.
        prefix.load();
        if(cmd==L"init" || cmd==L"wineboot") {prefix.init();printf("Prefix ready: %s\n",utf8(prefix.root).c_str());return 0;}
        if(cmd==L"drive") {
            if(i>=argc)throw std::runtime_error("Expected drive list, map, mount or audio");str op=argv[i++];
            if(op==L"map") {
                if(i+3>argc)throw std::runtime_error("Expected LETTER DIRECTORY TYPE [LABEL]");
                wchar_t letter=argv[i++][0];str path=argv[i++],kind=argv[i++];
                UINT type=kind==L"cdrom"?DRIVE_CDROM:kind==L"removable"?DRIVE_REMOVABLE:kind==L"fixed"?DRIVE_FIXED:DRIVE_UNKNOWN;
                if(!type)throw std::runtime_error("Unknown drive type");prefix.drive(letter,path,type,i<argc?argv[i]:L"Winedows",type==DRIVE_CDROM);
            } else if(op==L"mount") {
                if(i+2>argc)throw std::runtime_error("Expected LETTER IMAGE.iso|IMAGE.cue [LABEL]");
                wchar_t letter=argv[i++][0];str path=argv[i++],label=i<argc?argv[i]:L"";
                if(path.size()>=4&&!_wcsicmp(path.c_str()+path.size()-4,L".cue"))prefix.mount_cue(letter,path,label);
                else prefix.mount_image(letter,path,label);
            } else if(op==L"audio") {
                if(i+2!=argc)throw std::runtime_error("Expected LETTER PREPARED_MANIFEST.ini");
                wchar_t letter=argv[i++][0];prefix.mount_audio(letter,argv[i]);
            } else if(op!=L"list")throw std::runtime_error("Unknown drive command");
            for(int d=0;d<26;d++)if(!prefix.drives[d].path.empty()) {
                const auto &drive=prefix.drives[d];
                printf("%c: type=%u %s label=%s path=%s",'A'+d,drive.type,drive.readonly?"read-only":"writable",utf8(drive.label).c_str(),utf8(drive.path).c_str());
                if(!drive.image.empty())printf(" image=%s sectors=%llu serial=%08lx",utf8(drive.image).c_str(),drive.sector_size?drive.volume_size/drive.sector_size:0,drive.serial);
                if(!drive.audio.empty())printf(" audio=%s",utf8(drive.audio).c_str());
                puts("");
            }return 0;
        }
        if(session.owner())prefix.init();
        if(cmd==L"reg") {
            if(i>=argc)throw std::runtime_error("Expected reg import or export");str op=argv[i++];
            if(op==L"import") {
                if(i>=argc)throw std::runtime_error("Missing .reg file");str file=fullpath(argv[i++]);REGSAM view=KEY_WOW64_64KEY;
                if(i<argc){str option=argv[i++];if(!_wcsicmp(option.c_str(),L"/reg:32"))view=KEY_WOW64_32KEY;else if(_wcsicmp(option.c_str(),L"/reg:64"))throw std::runtime_error("Expected /reg:32 or /reg:64");}
                if(i!=argc)throw std::runtime_error("Unexpected registry import argument");
                HKEY h=prefix.hive();try{import_reg(h,file,view);}catch(...){RegCloseKey(h);throw;}RegCloseKey(h);
            }
            else if(op!=L"export")throw std::runtime_error("Unknown registry command");
            prefix.export_registry();return 0;
        }
        if(cmd==L"run") {if(i>=argc)throw std::runtime_error("Missing program");cmd=argv[i++];}
        if((cmd==L"explorer"||cmd==L"explorer.exe")&&i<argc&&starts(argv[i],L"/desktop=")){
            desktop=str(argv[i++]).substr(9);if(i>=argc)throw std::runtime_error("Missing virtual desktop program");cmd=argv[i++];
        }
        MediaSession media_session(prefix,session.owner()?MediaSessionMode::create:MediaSessionMode::attach);
        auto live_prefix=media_session.view();
        str exe;
        if(cmd.size()>2 && cmd[1]==L':') {
            exe=live_prefix->translate(cmd);
            int drive=live_prefix->drive_index(exe);
            if(drive>=0&&!media_session.snapshot(drive).present)throw std::runtime_error("Program is on an ejected virtual medium");
            if(GetFileAttributesW(exe.c_str())==INVALID_FILE_ATTRIBUTES)exe=fullpath(cmd);
        } else exe=fullpath(cmd);
        auto media_identity=media_session.path_identity(exe);
        auto check_start_medium=[&]{if(media_identity.drive>=0){
            auto state=media_session.snapshot(media_identity.drive);
            if(!state.present||(media_identity.medium&&media_identity.medium!=state.medium))throw std::runtime_error("Program is on an unavailable virtual medium");
        }};
        check_start_medium();
        if(GetFileAttributesW(exe.c_str())==INVALID_FILE_ATTRIBUTES)throw std::runtime_error("Program not found");
        str line=quote(exe);for(;i<argc;i++)line+=L" "+quote(argv[i]);
        prefix.environment();SetEnvironmentVariableW(L"WINEDOWS_APPDIR",directory(exe).c_str());
        std::unique_ptr<MediaController> media_controller;
        if(session.owner())media_controller=std::make_unique<MediaController>(prefix,media_session);
        WORD machine=program_machine(exe);
        const Prefix &launch_prefix=*live_prefix;
        str native_directory=launch_prefix.translate(prefix.windows+(machine==IMAGE_FILE_MACHINE_I386?L"\\SysWOW64":L"\\System32"));
        str other_directory=launch_prefix.translate(prefix.windows+(machine==IMAGE_FILE_MACHINE_I386?L"\\System32":L"\\SysWOW64"));
        if(!SetEnvironmentVariableW(startup_native_environment_name,native_directory.c_str()))check(GetLastError(),"Set private startup DLL directory");
#ifdef _WIN64
        str dll=module_dir()+L"\\winedows64.dll";
#else
        str dll=module_dir()+L"\\winedows32.dll";
#endif
        // PE import paths use the process ANSI code page, not UTF-8.
        BOOL lossy=FALSE;int n=WideCharToMultiByte(CP_ACP,WC_NO_BEST_FIT_CHARS,dll.c_str(),-1,nullptr,0,nullptr,&lossy);
        std::string dll_a(n,0);WideCharToMultiByte(CP_ACP,WC_NO_BEST_FIT_CHARS,dll.c_str(),-1,dll_a.data(),n,nullptr,&lossy);
        if(lossy)throw std::runtime_error("Runtime DLL path cannot be represented in the Windows ANSI code page; use an ASCII runtime directory");
        auto module_policy=select_module_policy(launch_prefix,exe);
        const auto &selected=module_policy.builtins;
        auto builtin_imports=plan_builtin_startup_imports(exe,machine,directory(exe),native_directory,other_directory,selected,[&](const str &path){
            str native=native_custom_directory(launch_prefix,path,machine);return StartupDirectoryPaths{launch_prefix.translate(native),native};
        });
        if(!builtin_imports.transitive_limit.empty())fprintf(stderr,"caskwin: Selected Wine transitive import inspection unavailable: %s. Direct executable imports are still bound.\n",builtin_imports.transitive_limit.c_str());
        auto native_imports=plan_native_startup_imports(exe,machine,directory(exe),native_directory,other_directory,selected,[&](const str &path){
            str native=native_custom_directory(launch_prefix,path,machine);
            return StartupDirectoryPaths{launch_prefix.translate(native),native};
        });
        if(!native_imports.unsupported_reason.empty())
            fprintf(stderr,"caskwin: Private startup DLL fallback unavailable: %s. Preserving the application's native search path.\n",native_imports.unsupported_reason.c_str());
        auto injected=selected;
        for(const auto &binding:native_imports.bindings)injected.push_back(binding.file.path);
        auto disabled_validation=validate_disabled_startup_imports(exe,machine,directory(exe),native_directory,other_directory,injected,module_policy.disabled,
            {env(L"PATH"),[&](const str &path){
                str native=native_custom_directory(launch_prefix,path,machine);
                return StartupDirectoryPaths{launch_prefix.translate(native),native};
            }});
        std::vector<std::string> dll_names{dll_a};
        for(const auto &path:injected){
            BOOL replaced=FALSE;int count=WideCharToMultiByte(CP_ACP,WC_NO_BEST_FIT_CHARS,path.c_str(),-1,nullptr,0,nullptr,&replaced);
            std::string name(count,0);WideCharToMultiByte(CP_ACP,WC_NO_BEST_FIT_CHARS,path.c_str(),-1,name.data(),count,nullptr,&replaced);
            if(replaced)throw std::runtime_error("DLL override path is not representable in the Windows ANSI code page");
            dll_names.push_back(name);
        }
        std::vector<const char*> dlls;for(const auto &name:dll_names)dlls.push_back(name.c_str());
        PROCESS_INFORMATION pi{};
        std::unique_ptr<DesktopHost> desktop_host;
        std::unique_ptr<MediaNotifications> media_notifications;
        if(session.owner()){
            session.create_job(media_session.name());
            str specification=desktop_specification(prefix,exe,desktop);
            desktop_host=std::make_unique<DesktopHost>(prefix,exe,specification,session.job());
            media_notifications=std::make_unique<MediaNotifications>(media_session,session.job(),desktop_host->window());
            session.publish(specification,desktop_host->window());
        }else session.attach_desktop(desktop.empty()?str():desktop_specification(prefix,exe,desktop));
        auto &startup=session.startup();
        bool debug=env(L"WINEDOWS_DEBUG")==L"1";
        if(!DetourCreateProcessWithDllsW(exe.c_str(),line.data(),nullptr,nullptr,FALSE,EXTENDED_STARTUPINFO_PRESENT|CREATE_SUSPENDED|(debug?DEBUG_ONLY_THIS_PROCESS:0),nullptr,directory(exe).c_str(),&startup.StartupInfo,&pi,(DWORD)dlls.size(),dlls.data(),nullptr))check(GetLastError(),"Start prefixed application in session jobs (caller job hierarchy must be compatible)");
        try {
            session.started();
            recheck_disabled_startup_imports(disabled_validation);
            bind_native_startup_imports(pi.hProcess,native_imports);
            bind_builtin_startup_imports(pi.hProcess,builtin_imports);
            configure_native_startup(pi.hProcess,machine,native_directory,other_directory);
            check_start_medium();
            if(ResumeThread(pi.hThread)==(DWORD)-1)check(GetLastError(),"Resume prefixed application");
        }catch(...){TerminateProcess(pi.hProcess,ERROR_DLL_INIT_FAILED);WaitForSingleObject(pi.hProcess,5000);CloseHandle(pi.hThread);CloseHandle(pi.hProcess);throw;}
        CloseHandle(pi.hThread);
        session.release_admission();
        if(debug) {
            bool first_breakpoint=true;
            for(;;) {
                DEBUG_EVENT event{};if(!WaitForDebugEvent(&event,INFINITE))break;DWORD action=DBG_CONTINUE;
                if(event.dwDebugEventCode==LOAD_DLL_DEBUG_EVENT && event.u.LoadDll.hFile)CloseHandle(event.u.LoadDll.hFile);
                if(event.dwDebugEventCode==CREATE_PROCESS_DEBUG_EVENT && event.u.CreateProcessInfo.hFile)CloseHandle(event.u.CreateProcessInfo.hFile);
                if(event.dwDebugEventCode==EXCEPTION_DEBUG_EVENT) {
                    auto &exception=event.u.Exception;
                    if(first_breakpoint&&exception.ExceptionRecord.ExceptionCode==EXCEPTION_BREAKPOINT)first_breakpoint=false;
                    else action=DBG_EXCEPTION_NOT_HANDLED;
                    if(!exception.dwFirstChance) {
                        fprintf(stderr,"Unhandled exception 0x%08lx at %p (thread %lu)\n",exception.ExceptionRecord.ExceptionCode,exception.ExceptionRecord.ExceptionAddress,event.dwThreadId);
                        HANDLE thread=OpenThread(THREAD_GET_CONTEXT|THREAD_QUERY_INFORMATION,FALSE,event.dwThreadId);CONTEXT context{};context.ContextFlags=CONTEXT_ALL;GetThreadContext(thread,&context);CloseHandle(thread);
                        EXCEPTION_POINTERS pointers{&exception.ExceptionRecord,&context};MINIDUMP_EXCEPTION_INFORMATION info{event.dwThreadId,&pointers,FALSE};
                        str dump=prefix.root+L"\\.winedows\\crash-"+std::to_wstring(pi.dwProcessId)+L".dmp";
                        HANDLE out=CreateFileW(dump.c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_ALWAYS,0,nullptr);
                        if(out!=INVALID_HANDLE_VALUE){BOOL saved=MiniDumpWriteDump(pi.hProcess,pi.dwProcessId,out,(MINIDUMP_TYPE)(MiniDumpWithIndirectlyReferencedMemory|MiniDumpWithThreadInfo),&info,nullptr,nullptr);CloseHandle(out);fprintf(stderr,"Crash dump %s: %s\n",saved?"saved":"failed",utf8(dump).c_str());}
                    }
                }
                ContinueDebugEvent(event.dwProcessId,event.dwThreadId,action);
                if(event.dwDebugEventCode==EXIT_PROCESS_DEBUG_EVENT)break;
            }
        }
        WaitForSingleObject(pi.hProcess,INFINITE);DWORD code=0;GetExitCodeProcess(pi.hProcess,&code);CloseHandle(pi.hProcess);
        session.wait();
        if(media_notifications)media_notifications->stop();
        if(session.owner())prefix.export_registry();return (int)code;
    } catch(const std::exception &e) {fprintf(stderr,"caskwin: %s\n",e.what());return 1;}
}
