// SPDX-License-Identifier: LGPL-2.1-or-later
#include "modules.h"
#include "builtin-catalog.h"
#include "pe-architecture.h"
#include <map>
#include <sstream>
#include <fstream>
#include <functional>
#include <set>

namespace wd {
using Orders=std::map<str,str>;
static str lowercase(str value){for(auto &c:value)c=(wchar_t)towlower(c);return value;}
static str trim(str value){size_t a=value.find_first_not_of(L" \t\r\n");return a==str::npos?str():value.substr(a,value.find_last_not_of(L" \t\r\n")-a+1);}
static str module_name(str value){
    value=lowercase(trim(value));
    if(value.find_first_of(L"\\/:")!=str::npos)throw std::runtime_error("Path-qualified DLL override keys are not supported; use a module basename");
    size_t start=!value.empty()&&value[0]==L'*'?1:0;
    if(value.size()>=4&&value.substr(value.size()-4)==L".dll")value.resize(value.size()-4);
    if(value.size()==start||value.find_first_of(L"*?",start)!=str::npos)throw std::runtime_error("Expected a DLL basename in override settings");
    return value;
}
static str unstar(const str &name){return !name.empty()&&name[0]==L'*'?name.substr(1):name;}
static str filename(const str &name){return name+L".dll";}
static Orders::const_iterator order_for(const Orders &orders,const str &name){auto found=orders.find(name);return found!=orders.end()?found:orders.find(L"*"+name);}
static str canonical_order(const str &value) {
    str words=value;std::replace(words.begin(),words.end(),L',',L' ');
    std::wistringstream input(words);str token,result;
    while(input>>token){
        token=lowercase(trim(token));wchar_t choice;
        if(token==L"b"||token==L"builtin")choice=L'b';
        else if(token==L"n"||token==L"native")choice=L'n';
        else throw std::runtime_error("DLL order must be builtin/native (b,n or n,b), b, n, or empty to disable");
        if(result.find(choice)==str::npos)result+=choice;
    }
    return result;
}
static void read_orders(HKEY hive,const str &key,Orders &orders) {
    HKEY h=nullptr;LSTATUS status=RegOpenKeyExW(hive,key.c_str(),0,KEY_READ,&h);
    if(status==ERROR_FILE_NOT_FOUND)return;check(status,"Read DLL override settings");
    try {
        for(DWORD index=0;;index++){
            wchar_t name[512],data[128];DWORD names=512,size=sizeof(data),type=0;
            LSTATUS e=RegEnumValueW(h,index,name,&names,nullptr,&type,(BYTE*)data,&size);
            if(e==ERROR_NO_MORE_ITEMS)break;check(e,"Read DLL override");
            if(type!=REG_SZ||size<2||size>sizeof(data)||data[size/2-1])throw std::runtime_error("Invalid DLL override registry value");
            orders[module_name(name)]=canonical_order(data);
        }
    } catch(...) {RegCloseKey(h);throw;}
    RegCloseKey(h);
}
static WORD pe_machine(const str &path) {
    std::ifstream input(path.c_str(),std::ios::binary);IMAGE_DOS_HEADER dos{};DWORD signature=0;IMAGE_FILE_HEADER file{};
    if(!input.read((char*)&dos,sizeof(dos))||dos.e_magic!=IMAGE_DOS_SIGNATURE||dos.e_lfanew<0)throw std::runtime_error("Invalid executable for DLL selection");
    input.seekg(dos.e_lfanew);
    if(!input.read((char*)&signature,4)||signature!=IMAGE_NT_SIGNATURE||!input.read((char*)&file,sizeof(file)))throw std::runtime_error("Invalid PE header for DLL selection");
    return file.Machine;
}
static bool exists(const str &file){DWORD a=GetFileAttributesW(file.c_str());return a!=INVALID_FILE_ATTRIBUTES&&!(a&FILE_ATTRIBUTE_DIRECTORY);}
static Orders read_module_orders(const Prefix &prefix,const str &program,const str &override_text) {
    Orders orders;HKEY h=prefix.hive();
    str application=program.substr(program.find_last_of(L"\\/")+1);
    try {
        read_orders(h,L"User\\Software\\Wine\\DllOverrides",orders);
        read_orders(h,L"User\\Software\\Wine\\AppDefaults\\"+application+L"\\DllOverrides",orders);
    }catch(...){RegCloseKey(h);throw;}RegCloseKey(h);
    std::wistringstream overrides(override_text);str entry;
    while(std::getline(overrides,entry,L';')){
        if(trim(entry).empty())continue;size_t equals=entry.find(L'=');
        if(equals==str::npos)throw std::runtime_error("Expected module=order in WINEDLLOVERRIDES");
        str order=canonical_order(entry.substr(equals+1)),name,words=entry.substr(0,equals);std::replace(words.begin(),words.end(),L',',L' ');std::wistringstream names(words);bool any=false;
        while(names>>name){orders[module_name(name)]=order;any=true;}
        if(!any)throw std::runtime_error("Expected a DLL basename before override '='");
    }
    // Wine swaps exact/wildcard precedence for explicit non-system paths. A
    // flat provider list cannot honor two different orders in those locations.
    // Reject that unsupported distinction instead of silently flattening it.
    for(const auto &item:orders)if(!item.first.empty()&&item.first[0]!=L'*'){
        auto wildcard=orders.find(L"*"+item.first);
        if(wildcard!=orders.end()&&wildcard->second!=item.second)throw std::runtime_error("Conflicting exact/wildcard DLL overrides require path-sensitive selection, which is unsupported: "+utf8(item.first));
    }
    return orders;
}
static std::vector<str> disabled_names(const Orders &orders,WORD machine) {
    std::set<str> names;for(const auto &item:orders)names.insert(unstar(item.first));
    std::vector<str> disabled;
    for(const auto &name:names){auto found=order_for(orders,name);if(found!=orders.end()&&found->second.empty())disabled.push_back(filename(name));}
    if(disabled.empty())return disabled;
    if(disabled.size()>256)throw std::runtime_error("Disabled DLL override list exceeds 256 modules");
    const auto known=module_known_dlls(machine);
    for(const auto &name:disabled){
        bool core=starts(name,L"api-ms-")||starts(name,L"ext-ms-");
        for(auto fixed:{L"winedows32.dll",L"winedows64.dll",L"ntdll.dll",L"kernel32.dll",L"kernelbase.dll",L"advapi32.dll",L"ole32.dll",L"combase.dll",L"shell32.dll",L"user32.dll",L"gdi32.dll",L"ucrtbase.dll",L"msvcrt.dll"})core|=name==fixed;
        if(core||std::find(known.begin(),known.end(),name)!=known.end())throw std::runtime_error("Disabling native core/KnownDLL modules is unsupported: "+utf8(name));
    }
    return disabled;
}
std::vector<str> read_disabled_modules(const Prefix &prefix,const str &program,const str &override_text) {
    Orders orders=read_module_orders(prefix,program,override_text);
    if(std::none_of(orders.begin(),orders.end(),[](const auto &item){return item.second.empty();}))return {};
    return disabled_names(orders,program_machine(program));
}
str native_custom_directory(const Prefix &prefix,const str &path,WORD machine) {
    // A child's initial search uses its own architecture, even when a parent
    // of the other architecture supplied the logical Windows System32 root.
    // Explicit physical prefix paths retain the caller's exact choice.
    if(machine==IMAGE_FILE_MACHINE_I386&&!_wcsicmp(path.c_str(),fullpath(prefix.windows+L"\\System32").c_str()))
        return fullpath(prefix.windows+L"\\SysWOW64");
    return path;
}
ModuleSelection select_module_policy(const Prefix &prefix,const str &program,const str &runtime,const str &override_text) {
    Orders orders=read_module_orders(prefix,program,override_text);
    for(const auto &item:orders){
        bool supported=false;for(const auto &module:portable_builtins)if(unstar(item.first)==module.name)supported=true;
        if(!supported&&!item.second.empty()&&item.second.find(L'n')==str::npos)
            throw std::runtime_error("No portable Wine builtin is available for override: "+utf8(item.first));
    }
    if(orders.empty())return {}; // Host-native remains the current default.
    WORD machine=program_machine(program);str arch;
    if(machine==IMAGE_FILE_MACHINE_I386)arch=L"x86";
    else if(machine==IMAGE_FILE_MACHINE_AMD64)arch=L"x64";
    else throw std::runtime_error("DLL overrides support x86 and x64 programs only");
    ModuleSelection result;result.disabled=disabled_names(orders,machine);
    str bundle=runtime+L"\\builtin\\"+arch;
    struct Choice {str name,path;bool builtin;};
    str custom_directory;
    DWORD directory_size=GetDllDirectoryW(0,nullptr);
    if(directory_size){
        std::vector<wchar_t> value(directory_size+1);
        DWORD count=GetDllDirectoryW((DWORD)value.size(),value.data());
        if(count&&count<value.size())custom_directory=fullpath(value.data());
    }
    // The startup helper replaces prefix-owned directories for child bitness.
    // An arbitrary application SetDllDirectory choice retains its precedence.
    for(auto name:{L"\\System32",L"\\SysWOW64"}){
        str owned=fullpath(prefix.translate(prefix.windows+name));
        if(!_wcsicmp(custom_directory.c_str(),owned.c_str()))custom_directory.clear();
    }
    if(!custom_directory.empty())custom_directory=native_custom_directory(prefix,custom_directory,machine);
    auto native_path=[&](const str &name){
        str filename=name+L".dll";
        str system=prefix.windows+str(machine==IMAGE_FILE_MACHINE_I386?L"\\SysWOW64\\":L"\\System32\\")+filename;
        // Match the shim's private-file-first, host-read-through view at each
        // native search location. App-local providers still precede system DLLs.
        std::vector<str> locations{directory(program)+L"\\"+filename};
        if(!custom_directory.empty())locations.push_back(custom_directory+L"\\"+filename);
        locations.push_back(system);
        for(const auto &path:locations){
            str private_path=prefix.translate(path);
            if(exists(private_path))return private_path;
            if(exists(path))return path;
        }
        return system;
    };
    std::string builtin_error;
    std::function<bool(const BuiltinDefinition&,bool,std::vector<Choice>&,unsigned)> resolve;
    resolve=[&](const BuiltinDefinition &module,bool required,std::vector<Choice> &choices,unsigned depth){
        if(depth>8)throw std::runtime_error("Cyclic Wine builtin dependency");
        auto found=order_for(orders,module.name);str order=found==orders.end()?L"b":found->second;
        if(order.empty()){if(required)builtin_error="Wine builtin dependency is disabled: "+utf8(module.name)+".dll";return false;}
        str builtin=bundle+L"\\"+module.name+L".dll",native=native_path(module.name);
        for(wchar_t mode:order){
            if(mode==L'n'&&exists(native)){choices.push_back({module.name,native,false});return true;}
            if(mode!=L'b'||!exists(builtin))continue;
            try {
                if(pe_machine(builtin)!=machine){
                    builtin_error="Wine builtin architecture does not match the program: "+utf8(builtin);
                    continue;
                }
            }catch(const std::runtime_error &error){
                builtin_error="Cannot use Wine builtin "+utf8(builtin)+": "+error.what();continue;
            }
            std::vector<Choice> dependencies;bool available=true;
            for(auto name:module.dependencies){
                if(!name)continue;
                const BuiltinDefinition *definition=nullptr;
                for(const auto &candidate:portable_builtins)if(!wcscmp(candidate.name,name)){definition=&candidate;break;}
                if(!definition||!resolve(*definition,true,dependencies,depth+1)){available=false;break;}
            }
            if(!available)continue;
            choices.insert(choices.end(),dependencies.begin(),dependencies.end());
            choices.push_back({module.name,builtin,true});return true;
        }
        // Unused native overrides remain on demand. Required dependencies must
        // exist before their parent can be considered an available Wine choice.
        return !required&&order.find(L'n')!=str::npos;
    };
    // These frontends exchange Wine-private interfaces and layer callbacks.
    // Select the complete family together; public helpers such as D3DX keep
    // their independent provider choices. Prefer the highest-level explicit
    // frontend's order when flexible orders differ, then honor every strict
    // restriction. Availability fallback switches the entire family.
    const wchar_t *graphics_family[]={L"d3d11",L"d3d10_1",L"d3d10",L"d3d10core",L"dxgi"};
    str family_order;
    for(auto name:graphics_family){auto found=order_for(orders,name);if(found!=orders.end()&&!found->second.empty()){family_order=found->second;break;}}
    if(!family_order.empty()){
        const Orders requested=orders;bool chosen=false;
        for(wchar_t mode:family_order){
            bool allowed=true;
            for(auto name:graphics_family){auto found=order_for(requested,name);if(found!=requested.end()&&!found->second.empty()&&found->second.find(mode)==str::npos)allowed=false;}
            if(!allowed)continue;
            orders=requested;for(auto name:graphics_family){auto found=order_for(requested,name);if(found==requested.end()||!found->second.empty())orders[name]=str(1,mode);}
            std::vector<Choice> candidates;bool available=true;
            for(auto name:graphics_family){
                auto disabled=order_for(orders,name);if(disabled!=orders.end()&&disabled->second.empty())continue;
                const BuiltinDefinition *definition=nullptr;
                for(const auto &module:portable_builtins)if(!wcscmp(module.name,name)){definition=&module;break;}
                if(!definition||!resolve(*definition,mode==L'b',candidates,0)){available=false;break;}
            }
            if(available){chosen=true;break;}
        }
        if(!chosen){
            if(!builtin_error.empty())throw std::runtime_error(builtin_error);
            throw std::runtime_error("DXGI and Direct3D 10/11 require one compatible available provider; conflicting strict builtin/native overrides cannot be combined");
        }
    }
    std::vector<Choice> choices;
    for(const auto &module:portable_builtins){
        auto found=order_for(orders,module.name);if(found==orders.end()||found->second.empty())continue;
        builtin_error.clear();
        if(!resolve(module,false,choices,0)){
            if(!builtin_error.empty())throw std::runtime_error(builtin_error);
            throw std::runtime_error("No available implementation for "+utf8(module.name)+".dll with DLL order "+utf8(found->second));
        }
    }
    std::vector<str> selected;std::map<str,bool> seen;
    for(const auto &choice:choices){
        if(!seen.emplace(choice.name,choice.builtin).second)continue;
        printf("DLL %s.dll: %s (%s)\n",utf8(choice.name).c_str(),choice.builtin?"Wine builtin":"native",utf8(choice.path).c_str());
        // Only Wine modules have the ordinal-1 import used by Detours. Native
        // providers keep the ordinary application/system DLL search.
        if(choice.builtin)selected.push_back(choice.path);
    }
    fflush(stdout);
    for(const auto &name:result.disabled)printf("DLL %s: disabled\n",utf8(name).c_str());
    fflush(stdout);result.builtins=std::move(selected);return result;
}
}
