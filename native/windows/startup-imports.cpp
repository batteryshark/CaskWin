// SPDX-License-Identifier: LGPL-2.1-or-later
#include "startup-imports.h"
#include "modules.h"
#include <cstring>
#include <cwctype>
#include <map>
#include <set>
#include <memory>
#include <limits>

namespace wd {
namespace {
constexpr size_t max_file=256*1024*1024,max_total=512*1024*1024,max_modules=128,max_symbols=16384,max_descriptors=2048;
class UnsupportedPlan:public std::runtime_error {public:explicit UnsupportedPlan(const char *message):std::runtime_error(message){}};
[[noreturn]] void unsupported(const char *message){throw UnsupportedPlan(message);}
[[noreturn]] void bad(const char *message){throw std::runtime_error(std::string("Native startup imports: ")+message);}
template<class F>auto inspect(bool private_candidate,F operation)->decltype(operation()){
    try{return operation();}catch(const UnsupportedPlan&){throw;}catch(const std::runtime_error &error){if(private_candidate)throw;unsupported(error.what());}
}
str lower(str value){for(auto &c:value)c=(wchar_t)towlower(c);return value;}
str basename(const str &value){auto pos=value.find_last_of(L"\\/");return value.substr(pos==str::npos?0:pos+1);}
str folder(const str &value){auto pos=value.find_last_of(L"\\/");if(pos==str::npos)bad("file has no absolute directory");return value.substr(0,pos);}
bool absolute(const str &value){return value.size()>2&&((value[1]==L':'&&(value[2]==L'\\'||value[2]==L'/'))||(value[0]==L'\\'&&value[1]==L'\\'));}
str normalized(const str &value,bool file_path=false){
    if(!absolute(value)||value.size()>32766||(!file_path&&value.find(L';')!=str::npos))unsupported("unsupported directory syntax");
    std::vector<wchar_t> buffer(32767);DWORD count=GetFullPathNameW(value.c_str(),(DWORD)buffer.size(),buffer.data(),nullptr);
    if(!count||count>=buffer.size())unsupported("cannot normalize directory");str result(buffer.data(),count);
    while(result.size()>3&&result.back()==L'\\')result.pop_back();return result;
}
str from_ansi(const std::string &value){
    if(value.empty()||value.size()>32766)bad("invalid import name");
    int count=MultiByteToWideChar(CP_ACP,MB_ERR_INVALID_CHARS,value.data(),(int)value.size(),nullptr,0);if(!count)bad("invalid ANSI import name");
    str result(count,0);if(!MultiByteToWideChar(CP_ACP,MB_ERR_INVALID_CHARS,value.data(),(int)value.size(),result.data(),count))bad("cannot decode import name");return result;
}
bool file_exists(const str &path){DWORD attr=GetFileAttributesW(path.c_str());if(attr!=INVALID_FILE_ATTRIBUTES)return !(attr&FILE_ATTRIBUTE_DIRECTORY);DWORD error=GetLastError();if(error!=ERROR_FILE_NOT_FOUND&&error!=ERROR_PATH_NOT_FOUND&&error!=ERROR_INVALID_NAME)unsupported("cannot inspect candidate file");return false;}
StartupImportFile identity(HANDLE file,const str &path){
    BY_HANDLE_FILE_INFORMATION info{};if(!GetFileInformationByHandle(file,&info))bad("cannot identify dependency file");
    return {path,info.dwVolumeSerialNumber,info.nFileIndexHigh,info.nFileIndexLow,info.nFileSizeHigh,info.nFileSizeLow,info.ftLastWriteTime.dwHighDateTime,info.ftLastWriteTime.dwLowDateTime};
}
bool same_identity(const StartupImportFile &a,const StartupImportFile &b){return a.volume==b.volume&&a.index_high==b.index_high&&a.index_low==b.index_low&&a.size_high==b.size_high&&a.size_low==b.size_low&&a.time_high==b.time_high&&a.time_low==b.time_low;}
struct Handle {HANDLE value=INVALID_HANDLE_VALUE;explicit Handle(HANDLE h):value(h){}~Handle(){if(value!=INVALID_HANDLE_VALUE&&value)CloseHandle(value);}Handle(const Handle&)=delete;Handle&operator=(const Handle&)=delete;};
struct Symbol {bool ordinal=false;WORD number=0;std::string name;std::string key()const{return ordinal?"#"+std::to_string(number):name;}};
struct Import {std::string name;std::vector<Symbol> symbols;IMAGE_IMPORT_DESCRIPTOR descriptor{};std::vector<uint64_t> lookup;};
struct Export {WORD ordinal;std::string forwarder;};

class PeFile {
    std::vector<BYTE> bytes;
    std::vector<IMAGE_SECTION_HEADER> sections;
    IMAGE_DATA_DIRECTORY imports{},exports{},clr{};DWORD headers=0;
    size_t offset(DWORD rva,size_t length)const{
        if(rva<headers){if(length>headers-rva||rva>bytes.size()||length>bytes.size()-rva)bad("PE header RVA exceeds file");return rva;}
        size_t found=SIZE_MAX;
        for(const auto &s:sections)if(rva>=s.VirtualAddress&&uint64_t(rva)-s.VirtualAddress<s.SizeOfRawData){
            size_t delta=rva-s.VirtualAddress;if(length>s.SizeOfRawData-delta)bad("PE RVA crosses raw section");
            uint64_t raw=uint64_t(s.PointerToRawData)+delta;if(raw>bytes.size()||length>bytes.size()-raw||found!=SIZE_MAX)bad("ambiguous or out-of-file RVA");found=(size_t)raw;
        }
        if(found==SIZE_MAX)bad("PE RVA has no file backing");return found;
    }
    template<class T>T object(size_t at)const{if(at>bytes.size()||sizeof(T)>bytes.size()-at)bad("truncated PE structure");T value;memcpy(&value,bytes.data()+at,sizeof(value));return value;}
    template<class T>T rva(DWORD at)const{return object<T>(offset(at,sizeof(T)));}
    DWORD plus(DWORD value,uint64_t delta)const{if(delta>UINT32_MAX-value)bad("PE RVA arithmetic overflow");return value+(DWORD)delta;}
    std::string string(DWORD at,size_t limit=32766)const{std::string result;for(size_t i=0;i<=limit;i++){char c=(char)rva<BYTE>(plus(at,i));if(!c)return result;result+=c;}bad("unterminated PE string");}
public:
    StartupImportFile file;WORD machine=0;bool managed_anycpu=false;
    explicit PeFile(const str &path,size_t &total){
        Handle handle(CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr));if(handle.value==INVALID_HANDLE_VALUE)bad("cannot open dependency PE");
        file=identity(handle.value,path);uint64_t size=(uint64_t(file.size_high)<<32)|file.size_low;
        if(size<sizeof(IMAGE_DOS_HEADER))bad("truncated dependency DOS header");if(size>max_file||size>max_total-total)unsupported("dependency file budget exceeded");total+=(size_t)size;bytes.resize((size_t)size);
        DWORD received=0;if(!ReadFile(handle.value,bytes.data(),(DWORD)bytes.size(),&received,nullptr)||received!=bytes.size())bad("cannot read dependency PE");
        auto dos=object<IMAGE_DOS_HEADER>(0);if(dos.e_magic!=IMAGE_DOS_SIGNATURE||dos.e_lfanew<0)bad("invalid DOS header");size_t nt=(size_t)dos.e_lfanew;
        if(object<DWORD>(nt)!=IMAGE_NT_SIGNATURE)bad("invalid PE signature");auto fh=object<IMAGE_FILE_HEADER>(nt+4);machine=fh.Machine;
        if(!fh.NumberOfSections)bad("missing PE sections");if(fh.NumberOfSections>96)unsupported("unsupported PE section count");size_t opt=nt+4+sizeof(fh);WORD magic=object<WORD>(opt);
        if(magic==IMAGE_NT_OPTIONAL_HDR32_MAGIC){if(fh.SizeOfOptionalHeader<sizeof(IMAGE_OPTIONAL_HEADER32))unsupported("short PE32 optional header requires native-only resolution");auto h=object<IMAGE_OPTIONAL_HEADER32>(opt);headers=h.SizeOfHeaders;if(h.NumberOfRvaAndSizes>IMAGE_DIRECTORY_ENTRY_IMPORT)imports=h.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];if(h.NumberOfRvaAndSizes>IMAGE_DIRECTORY_ENTRY_EXPORT)exports=h.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];if(h.NumberOfRvaAndSizes>IMAGE_DIRECTORY_ENTRY_COM_DESCRIPTOR)clr=h.DataDirectory[IMAGE_DIRECTORY_ENTRY_COM_DESCRIPTOR];}
        else if(magic==IMAGE_NT_OPTIONAL_HDR64_MAGIC){if(fh.SizeOfOptionalHeader<sizeof(IMAGE_OPTIONAL_HEADER64))unsupported("short PE64 optional header requires native-only resolution");auto h=object<IMAGE_OPTIONAL_HEADER64>(opt);headers=h.SizeOfHeaders;if(h.NumberOfRvaAndSizes>IMAGE_DIRECTORY_ENTRY_IMPORT)imports=h.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];if(h.NumberOfRvaAndSizes>IMAGE_DIRECTORY_ENTRY_EXPORT)exports=h.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];}
        else bad("unsupported PE optional header");
        for(unsigned i=0;i<fh.NumberOfSections;i++)sections.push_back(object<IMAGE_SECTION_HEADER>(opt+fh.SizeOfOptionalHeader+i*sizeof(IMAGE_SECTION_HEADER)));
        if(machine==IMAGE_FILE_MACHINE_I386&&!(fh.Characteristics&IMAGE_FILE_DLL)&&clr.VirtualAddress){
            struct ClrHeader {DWORD size;WORD major,minor;IMAGE_DATA_DIRECTORY metadata;DWORD flags;};
            if(clr.Size<sizeof(ClrHeader))bad("truncated CLR startup header");auto h=rva<ClrHeader>(clr.VirtualAddress);
            if(h.size<sizeof(h)||h.size>clr.Size)bad("invalid CLR startup header");
            managed_anycpu=(h.flags&1)&&!(h.flags&(2|0x10|0x20000));
        }
    }
    std::vector<Import> dependencies(size_t &symbol_count,const std::set<str> *selected_names=nullptr)const{
        std::vector<Import> result;if(!imports.VirtualAddress&&!imports.Size)return result;
        if(!imports.VirtualAddress||imports.Size<sizeof(IMAGE_IMPORT_DESCRIPTOR))bad("invalid import directory bounds");if(imports.Size>1024*1024)unsupported("import directory budget exceeded");
        size_t count=std::min(max_descriptors,(size_t)imports.Size/sizeof(IMAGE_IMPORT_DESCRIPTOR));
        for(size_t i=0;i<count;i++){
            auto d=rva<IMAGE_IMPORT_DESCRIPTOR>(plus(imports.VirtualAddress,i*sizeof(IMAGE_IMPORT_DESCRIPTOR)));
            if(!d.Name&&!d.OriginalFirstThunk&&!d.FirstThunk)return result;
            if(!d.Name||!d.FirstThunk)bad("incomplete import descriptor");Import item{string(d.Name),{}};item.descriptor=d;DWORD table=d.OriginalFirstThunk?d.OriginalFirstThunk:d.FirstThunk;
            // Direct builtin binding needs symbols only for descriptors it
            // will change. Native bound imports may legitimately have no INT.
            if(selected_names){str key=lower(basename(from_ansi(item.name)));if(key.find(L'.')==str::npos)key+=L".dll";if(!selected_names->count(key))continue;}
            if(!d.OriginalFirstThunk&&d.TimeDateStamp)unsupported("bound import has no original name table");
            size_t width=machine==IMAGE_FILE_MACHINE_I386?4:8;
            for(size_t j=0;;j++){
                if(symbol_count>=max_symbols||j>=max_symbols)unsupported("import symbol budget exceeded");DWORD at=plus(table,j*width);uint64_t value=width==4?rva<DWORD>(at):rva<ULONGLONG>(at);if(!value)break;++symbol_count;
                item.lookup.push_back(value);uint64_t ordinal_flag=width==4?IMAGE_ORDINAL_FLAG32:IMAGE_ORDINAL_FLAG64;
                if(value&ordinal_flag){if(value&~(ordinal_flag|0xffffull))bad("invalid ordinal thunk");item.symbols.push_back({true,(WORD)value,{}});}
                else {if(value>UINT32_MAX)unsupported("bound import has no usable name table");rva<WORD>((DWORD)value);auto name=string(plus((DWORD)value,2));if(name.empty())bad("empty import symbol");item.symbols.push_back({false,0,name});}
            }
            // Windows skips empty import thunks; they must not cause preloads.
            if(!item.symbols.empty())result.push_back(std::move(item));
        }
        if(count==max_descriptors)unsupported("import descriptor budget exceeded");bad("unterminated import descriptor table");
    }
    Export exported(const Symbol &symbol)const{
        if(!exports.VirtualAddress||exports.Size<sizeof(IMAGE_EXPORT_DIRECTORY))bad("imported dependency has no exports");auto e=rva<IMAGE_EXPORT_DIRECTORY>(exports.VirtualAddress);
        if(!e.NumberOfFunctions)bad("empty export table");if(e.NumberOfFunctions>65536||e.NumberOfNames>65536)unsupported("unsupported export table bounds");DWORD index=UINT32_MAX;
        if(symbol.ordinal){if(symbol.number>=e.Base&&symbol.number-e.Base<e.NumberOfFunctions)index=symbol.number-e.Base;}
        else for(DWORD i=0;i<e.NumberOfNames;i++){DWORD name=rva<DWORD>(plus(e.AddressOfNames,uint64_t(i)*4));if(string(name)==symbol.name){index=rva<WORD>(plus(e.AddressOfNameOrdinals,uint64_t(i)*2));break;}}
        if(index>=e.NumberOfFunctions)bad("requested dependency export is absent");if(uint64_t(e.Base)+index>65535)unsupported("requested export is not ordinal-addressable");DWORD address=rva<DWORD>(plus(e.AddressOfFunctions,uint64_t(index)*4));if(!address)bad("requested dependency export is empty");
        std::string forward;if(address>=exports.VirtualAddress&&uint64_t(address)-exports.VirtualAddress<exports.Size)forward=string(address,1024);
        return {(WORD)(e.Base+index),forward};
    }
};

std::set<str> known_dlls(WORD machine){
    using Open=NTSTATUS(NTAPI*)(PHANDLE,ACCESS_MASK,POBJECT_ATTRIBUTES);
    using Query=NTSTATUS(NTAPI*)(HANDLE,PVOID,ULONG,BOOLEAN,BOOLEAN,PULONG,PULONG);
    auto ntdll=GetModuleHandleW(L"ntdll.dll");auto open=(Open)GetProcAddress(ntdll,"NtOpenDirectoryObject");auto query=(Query)GetProcAddress(ntdll,"NtQueryDirectoryObject");if(!open||!query)bad("KnownDLL directory APIs unavailable");
    SYSTEM_INFO system{};GetNativeSystemInfo(&system);str name=machine==IMAGE_FILE_MACHINE_I386&&system.wProcessorArchitecture==PROCESSOR_ARCHITECTURE_AMD64?L"\\KnownDlls32":L"\\KnownDlls";
    UNICODE_STRING unicode{};unicode.Buffer=name.data();unicode.Length=(USHORT)(name.size()*2);unicode.MaximumLength=unicode.Length+2;OBJECT_ATTRIBUTES attrs{};attrs.Length=sizeof(attrs);attrs.ObjectName=&unicode;attrs.Attributes=0x40;
    HANDLE raw=nullptr;if(open(&raw,1,&attrs)<0)bad("cannot open target KnownDLL directory");Handle handle(raw);std::set<str> result;ULONG context=0;
    for(unsigned i=0;i<512;i++){
        alignas(void*) BYTE buffer[4096]{};ULONG received=0;NTSTATUS status=query(handle.value,buffer,sizeof(buffer),TRUE,i==0,&context,&received);if(status==(NTSTATUS)0x8000001a)return result;if(status<0)bad("cannot enumerate target KnownDLL directory");
        auto entry=(UNICODE_STRING*)buffer;auto get=[&](const UNICODE_STRING &s){uintptr_t p=(uintptr_t)s.Buffer,b=(uintptr_t)buffer;if(s.Length%2||p<b||p>b+sizeof(buffer)||s.Length>b+sizeof(buffer)-p)bad("invalid KnownDLL object name");return str(s.Buffer,s.Length/2);};
        if(lower(get(entry[1]))==L"section")result.insert(lower(get(entry[0])));
    }
    bad("KnownDLL enumeration budget exceeded");
}

class Manifest {
    HANDLE context=INVALID_HANDLE_VALUE;
public:
    Manifest(const str &application,WORD machine,WORD resource_id=1){
        bool sidecar=file_exists(application+L".manifest");
        // CreateActCtx can fall back to an external manifest even when a
        // resource ID was requested. Inspect resources without executing code
        // first, so an ordinary sidecar is not misclassified as an ambiguity.
        HMODULE image=LoadLibraryExW(application.c_str(),nullptr,LOAD_LIBRARY_AS_DATAFILE|LOAD_LIBRARY_AS_IMAGE_RESOURCE);if(!image)unsupported("cannot inspect executable manifest resources");
        bool embedded=FindResourceW(image,MAKEINTRESOURCEW(resource_id),RT_MANIFEST)!=nullptr;DWORD error=GetLastError();FreeLibrary(image);
        if(!embedded&&error!=ERROR_RESOURCE_DATA_NOT_FOUND&&error!=ERROR_RESOURCE_TYPE_NOT_FOUND&&error!=ERROR_RESOURCE_NAME_NOT_FOUND&&error!=ERROR_RESOURCE_LANG_NOT_FOUND)unsupported("cannot inspect application manifest resource");
        if(embedded&&sidecar)unsupported("embedded and external manifests need native-only resolution");if(!embedded&&!sidecar)return;
        ACTCTXW c{};c.cbSize=sizeof(c);(void)machine;
        str source=embedded?application:application+L".manifest";c.lpSource=source.c_str();if(embedded){c.dwFlags|=ACTCTX_FLAG_RESOURCE_NAME_VALID;c.lpResourceName=MAKEINTRESOURCEW(resource_id);}
        context=CreateActCtxW(&c);if(context==INVALID_HANDLE_VALUE)unsupported(("cannot inspect application activation context (error "+std::to_string(GetLastError())+")").c_str());
    }
    ~Manifest(){if(context!=INVALID_HANDLE_VALUE)ReleaseActCtx(context);}
    bool redirects(const str &name,bool include_default=false)const{
        if(context==INVALID_HANDLE_VALUE&&!include_default)return false;ULONG_PTR cookie=0;if(context!=INVALID_HANDLE_VALUE&&!ActivateActCtx(context,&cookie))unsupported("cannot activate application manifest for inspection");ACTCTX_SECTION_KEYED_DATA data{};data.cbSize=sizeof(data);
        BOOL found=FindActCtxSectionStringW(0,nullptr,ACTIVATION_CONTEXT_SECTION_DLL_REDIRECTION,name.c_str(),&data);DWORD error=GetLastError();BOOL undone=!cookie||DeactivateActCtx(0,cookie);if(!undone)unsupported("cannot restore thread activation context");if(found)return true;if(error!=ERROR_SXS_KEY_NOT_FOUND&&error!=ERROR_SXS_SECTION_NOT_FOUND)unsupported("cannot inspect manifest DLL redirection");return false;
    }
};

struct Node {std::unique_ptr<PeFile> pe;std::unique_ptr<Manifest> manifest;bool imports_read=false,preload=false;WORD ordinal=0;std::string symbol;std::set<std::string> examined;std::set<size_t> edges;unsigned state=0;};
class Planner {
    StartupImportPlan result;str private_directory,custom_resolved,custom_overlay,app_directory;bool custom_mapped=false;
    bool dll_contexts=false;
    std::set<str> excluded;std::map<str,size_t> by_path,by_basename;std::vector<std::unique_ptr<Node>> nodes;size_t bytes=0,symbols=0;Manifest manifest;
    bool excluded_name(const str &name)const{
        str value=lower(basename(name));return excluded.count(value)||value.compare(0,4,L"api-")==0||value.compare(0,4,L"ext-")==0;
    }
    size_t node(const str &path,bool private_candidate=false){
        str key=lower(path);auto existing=by_path.find(key);if(existing!=by_path.end())return existing->second;
        if(nodes.size()>=max_modules)unsupported("dependency module budget exceeded");auto value=std::make_unique<Node>();value->pe=inspect(private_candidate,[&]{return std::make_unique<PeFile>(path,bytes);});if(value->pe->machine!=result.machine){if(private_candidate)bad("private dependency architecture differs from child");unsupported("application/native dependency disk architecture differs from child (including AnyCPU promotion)");}
        if(dll_contexts&&!nodes.empty())value->manifest=std::make_unique<Manifest>(path,result.machine,2);
        str name=lower(basename(path));auto same=by_basename.find(name);if(same!=by_basename.end()&&lower(nodes[same->second]->pe->file.path)!=key)unsupported("same DLL basename resolves to multiple native files");
        size_t index=nodes.size();by_path.emplace(key,index);by_basename.emplace(name,index);nodes.push_back(std::move(value));return index;
    }
    void dependencies(size_t index){
        auto &n=*nodes[index];if(n.imports_read)return;n.imports_read=true;
        for(const auto &entry:inspect(n.preload,[&]{return n.pe->dependencies(symbols);}))edge(index,entry.name,entry.symbols);
    }
    void edge(size_t from,const std::string &raw,const std::vector<Symbol> &requested){
        str name=from_ansi(raw);
        bool bare=name.find_first_of(L"\\/:")==str::npos;if(bare&&name.find(L'.')==str::npos)name+=L".dll";
        if(excluded_name(name))return;
        if(bare&&(manifest.redirects(name,dll_contexts)||(nodes[from]->manifest&&nodes[from]->manifest->redirects(name,true))))return;
        str chosen;bool fallback=false;
        if(!bare){if(!absolute(name)){if(name.find(L':')!=str::npos||(!name.empty()&&(name[0]==L'\\'||name[0]==L'/')))unsupported("ambiguous qualified import path");name=result.current_directory+L"\\"+name;}chosen=normalized(name);if(!file_exists(chosen))return;}
        else {
            struct Location {const str *directory;bool preload;};
            std::vector<Location> locations{{&app_directory,false}};
            if(custom_mapped)locations.push_back({&custom_overlay,true});
            locations.push_back({&custom_resolved,false});locations.push_back({&private_directory,true});
            for(const auto &location:locations){str candidate=*location.directory+L"\\"+name;if(file_exists(candidate)){chosen=candidate;fallback=location.preload;break;}result.absent_candidates.push_back(candidate);}
            if(chosen.empty())return; // Remaining host locations retain native loading.
        }
        if(fallback&&(file_exists(result.application+L".local")||GetFileAttributesW((result.application+L".local").c_str())!=INVALID_FILE_ATTRIBUTES))unsupported(".local DLL redirection requires native-only resolution");
        size_t target=node(chosen,fallback);nodes[from]->edges.insert(target);auto &n=*nodes[target];n.preload|=fallback;
        for(const auto &symbol:requested){
            if(!n.examined.insert(symbol.key()).second)continue;if(++symbols>max_symbols)unsupported("forwarded export budget exceeded");auto exported=inspect(n.preload,[&]{return n.pe->exported(symbol);});
            if(n.symbol.empty()){n.ordinal=exported.ordinal;n.symbol=symbol.key();}
            if(!exported.forwarder.empty()){
                auto split=exported.forwarder.find_last_of('.');if(split==std::string::npos||!split||split+1==exported.forwarder.size())bad("malformed forwarded export");std::string dll=exported.forwarder.substr(0,split),function=exported.forwarder.substr(split+1);Symbol next;
                if(function[0]=='#'){char *end=nullptr;unsigned long value=strtoul(function.c_str()+1,&end,10);if(!end||*end||end==function.c_str()+1||value>65535)bad("invalid forwarded ordinal");next={true,(WORD)value,{}};}else next={false,0,function};
                edge(target,dll,{next});
            }
        }
        dependencies(target);
    }
    void ordered(size_t index){auto &n=*nodes[index];if(n.state==1)unsupported("cyclic native dependency plan");if(n.state==2)return;n.state=1;for(auto next:n.edges)ordered(next);n.state=2;if(n.preload){
        if(n.symbol.empty())bad("private dependency has no genuine imported export");
        // Upstream Detours' cross-bitness helper rewrites these path substrings
        // before loading. Until that API supports exact target paths, reject a
        // native filename it would silently rename to a different provider.
        bool cross=(sizeof(void*)==4)!=(result.machine==IMAGE_FILE_MACHINE_I386);
        if(cross&&n.pe->file.path.find(sizeof(void*)==4?L"32.":L"64.")!=str::npos)unsupported("cross-architecture helper would rename native provider path");
        result.bindings.push_back({n.pe->file,n.ordinal,n.symbol});
    }}
public:
    Planner(StartupImportPlan plan,const str &private_path,const std::vector<str> &builtins,const StartupDirectoryMapper &map_custom,bool inspect_dll_contexts=false):result(std::move(plan)),private_directory(normalized(private_path)),app_directory(folder(result.application)),dll_contexts(inspect_dll_contexts),excluded(inspect(false,[&]{return known_dlls(result.machine);})),manifest(result.application,result.machine){
        static const wchar_t *core[]={L"ntdll.dll",L"kernel32.dll",L"kernelbase.dll",L"advapi32.dll",L"combase.dll",L"ole32.dll",L"oleaut32.dll",L"rpcrt4.dll",L"sechost.dll",L"user32.dll",L"gdi32.dll",L"gdi32full.dll",L"win32u.dll",L"shell32.dll",L"shlwapi.dll",L"shcore.dll",L"msvcrt.dll",L"ucrtbase.dll",L"imm32.dll",L"mscoree.dll"};
        for(auto name:core)excluded.insert(name);for(const auto &path:builtins)excluded.insert(lower(basename(path)));
        if(absolute(result.custom_directory))custom_resolved=normalized(result.custom_directory);
        else {if(result.custom_directory.find(L':')!=str::npos||result.custom_directory[0]==L'\\'||result.custom_directory[0]==L'/')unsupported("ambiguous inherited DLL directory");custom_resolved=normalized(result.current_directory+L"\\"+result.custom_directory);}
        if(map_custom){auto mapped=map_custom(custom_resolved);custom_overlay=normalized(mapped.overlay);custom_resolved=normalized(mapped.native);custom_mapped=lower(custom_overlay)!=lower(custom_resolved);}
    }
    StartupImportPlan run(){size_t root=node(result.application);dependencies(root);bool needed=false;for(const auto &n:nodes)needed|=n->preload;if(needed)ordered(root);for(const auto &n:nodes)result.observed_files.push_back(n->pe->file);return std::move(result);}
};

str inherited_directory(){
    DWORD size=GetDllDirectoryW(0,nullptr);if(!size)return {};if(size>32767)unsupported("inherited DLL directory exceeds inspection bound");
    std::vector<wchar_t> text(size+1);SetLastError(ERROR_SUCCESS);DWORD n=GetDllDirectoryW((DWORD)text.size(),text.data());
    if(!n){if(GetLastError()!=ERROR_SUCCESS)unsupported("cannot inspect inherited DLL directory");return {};}
    if(n>=text.size())unsupported("inherited DLL directory changed during inspection");return str(text.data(),n);
}
// This scope changes only the calling thread's filesystem view. A 32-bit
// launcher inspecting a 64-bit graph must read the actual System32 images.
struct NativeFileView {
    PVOID previous=nullptr;bool changed=false;
    explicit NativeFileView(WORD machine){
        if constexpr(sizeof(void*)==4){if(machine==IMAGE_FILE_MACHINE_AMD64){
            if(!Wow64DisableWow64FsRedirection(&previous))unsupported("cannot inspect native 64-bit system images");changed=true;
        }}
    }
    ~NativeFileView(){if(changed)Wow64RevertWow64FsRedirection(previous);}
};
str policy_name(str name){name=lower(basename(name));if(name.size()>4&&name.substr(name.size()-4)==L".dll")name.resize(name.size()-4);return name+L".dll";}
std::string printable(const str &value){
    int n=WideCharToMultiByte(CP_UTF8,0,value.data(),(int)value.size(),nullptr,0,nullptr,nullptr);if(!n)return "<module>";
    std::string text(n,0);WideCharToMultiByte(CP_UTF8,0,value.data(),(int)value.size(),text.data(),n,nullptr,nullptr);return text;
}
struct PolicyNode {
    std::unique_ptr<PeFile> pe;std::unique_ptr<Manifest> manifest;bool imports_read=false;
    std::set<std::string> examined,examining;
};
class NativeApiSets {
    // Version-6 structures and importer-specific host selection match Wine's
    // ntdll get_apiset_entry/get_apiset_target. Read the live native namespace;
    // the on-disk schema may omit installed schema extensions. The namespace
    // is system-wide, while the resolved host PE is checked for child bitness.
    struct Header {DWORD version,size,flags,count,entries,hashes,factor;};
    struct Entry {DWORD flags,name,length,hashed_length,values,count;};
    struct Value {DWORD flags,name,name_length,value,value_length;};
    std::vector<BYTE> data;Header header{};
    template<class T>T get(uint64_t offset)const{if(offset>data.size()||sizeof(T)>data.size()-offset)unsupported("API-set structure exceeds native namespace");T value;memcpy(&value,data.data()+(size_t)offset,sizeof(value));return value;}
    str text(DWORD offset,DWORD length)const{if(length%2||length>65532||offset>data.size()||length>data.size()-offset)unsupported("API-set string exceeds native namespace");str value(length/2,0);memcpy(value.data(),data.data()+offset,length);if(value.find(L'\0')!=str::npos)unsupported("API-set string contains embedded NUL");return lower(value);}
public:
    NativeApiSets(){
        using Query=NTSTATUS(NTAPI*)(HANDLE,ULONG,PVOID,ULONG,PULONG);
        auto query=(Query)GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"NtQueryInformationProcess");PROCESS_BASIC_INFORMATION info{};
        if(!query||query(GetCurrentProcess(),0,&info,sizeof(info),nullptr)<0||!info.PebBaseAddress)unsupported("cannot inspect native API-set namespace");
        PVOID mapping=nullptr;SIZE_T received=0;auto location=(const BYTE*)info.PebBaseAddress+(sizeof(void*)==8?0x68:0x38);
        if(!ReadProcessMemory(GetCurrentProcess(),location,&mapping,sizeof(mapping),&received)||received!=sizeof(mapping)||!mapping)unsupported("native API-set namespace pointer unavailable");
        if(!ReadProcessMemory(GetCurrentProcess(),mapping,&header,sizeof(header),&received)||received!=sizeof(header)||header.version!=6||header.size<sizeof(header)||header.size>8*1024*1024||header.count>16384)
            unsupported("unsupported native API-set namespace layout");
        data.resize(header.size);if(!ReadProcessMemory(GetCurrentProcess(),mapping,data.data(),data.size(),&received)||received!=data.size())unsupported("cannot snapshot native API-set namespace");
        auto copied=get<Header>(0);if(memcmp(&copied,&header,sizeof(header)))unsupported("native API-set namespace changed during query");
    }
    str host(const str &contract,const str &importer)const{
        str key=lower(contract);auto dot=key.find(L'.');if(dot!=str::npos)key.resize(dot);auto dash=key.find_last_of(L'-');if(dash==str::npos)unsupported("invalid API-set contract name");key.resize(dash);
        const str parent=lower(basename(importer));bool found=false;str target;
        for(DWORD i=0;i<header.count;i++){
            auto entry=get<Entry>(uint64_t(header.entries)+uint64_t(i)*sizeof(Entry));
            if(entry.hashed_length!=key.size()*2||text(entry.name,entry.hashed_length)!=key)continue;
            if(found)unsupported("ambiguous API-set contract mapping");found=true;
            if(!entry.count||entry.count>1024)unsupported("API-set contract has no bounded host mapping");
            auto selected=get<Value>(entry.values);
            for(DWORD j=1;j<entry.count;j++){auto value=get<Value>(uint64_t(entry.values)+uint64_t(j)*sizeof(Value));if(text(value.name,value.name_length)==parent){selected=value;break;}}
            if(!selected.value||!selected.value_length)unsupported("API-set contract has no host implementation");target=text(selected.value,selected.value_length);
        }
        if(!found)unsupported("unknown API-set contract under disabled startup policy");
        if(target.empty()||target.find_first_of(L"\\/:")!=str::npos)unsupported("API-set host is not a module basename");return target;
    }
};
class DisabledValidator {
    DisabledStartupValidation result;str application,cwd,app_directory,effective_directory,system_directory,windows_directory;
    const DisabledStartupOptions &options;
    std::set<str> disabled,known,absent;
    std::map<str,str> preloaded;
    std::map<str,size_t> by_path,by_basename;
    std::vector<std::unique_ptr<PolicyNode>> nodes;
    size_t bytes=0,symbols=0;
    Manifest manifest;
    std::unique_ptr<NativeApiSets> api_sets;
    str resolve_directory(const str &path)const{
        if(path.empty())return cwd;
        if(absolute(path))return normalized(path);
        if(path.find(L':')!=str::npos||path[0]==L'\\'||path[0]==L'/')unsupported("ambiguous relative startup search directory");
        return normalized(cwd+L"\\"+path);
    }
    str native_directory(const str &path)const{
        str resolved=resolve_directory(path);return options.map_directory?normalized(options.map_directory(resolved).native):resolved;
    }
    void allowed(const str &name)const{
        if(disabled.count(policy_name(name)))throw std::runtime_error("disabled DLL required by startup graph: "+printable(basename(name)));
    }
    void missing(const str &path){
        if(absent.size()>=16384)unsupported("startup search candidate budget exceeded");absent.insert(path);
    }
    void manifest_snapshot(const str &path){
        if(!file_exists(path)){missing(path);return;}
        Handle file(CreateFileW(path.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr));
        if(file.value==INVALID_HANDLE_VALUE)unsupported("cannot snapshot startup manifest");result.observed_files.push_back(identity(file.value,path));
    }
    size_t node(const str &path,bool main=false){
        str key=lower(path);auto found=by_path.find(key);if(found!=by_path.end())return found->second;
        if(nodes.size()>=max_modules)unsupported("startup dependency module budget exceeded");
        auto n=std::make_unique<PolicyNode>();n->pe=std::make_unique<PeFile>(path,bytes);
        if(n->pe->machine!=result.machine&&!(main&&result.machine==IMAGE_FILE_MACHINE_AMD64&&n->pe->managed_anycpu))
            unsupported("startup dependency architecture does not match child");
        manifest_snapshot(path+L".manifest");
        if(!main)n->manifest=std::make_unique<Manifest>(path,result.machine,(WORD)2);
        str name=lower(basename(path));auto existing=by_basename.find(name);
        if(existing!=by_basename.end()&&lower(nodes[existing->second]->pe->file.path)!=key)unsupported("startup DLL basename resolves to competing files");
        size_t index=nodes.size();by_path.emplace(key,index);by_basename.emplace(name,index);nodes.push_back(std::move(n));return index;
    }
    void dependencies(size_t index){
        auto &n=*nodes[index];if(n.imports_read)return;n.imports_read=true;
        for(const auto &entry:n.pe->dependencies(symbols))edge(index,entry.name,entry.symbols);
    }
    str candidate(const str &directory,const str &name){str path=directory+L"\\"+name;if(file_exists(path))return path;missing(path);return {};}
    str search(const str &name){
        for(const auto &directory:{app_directory,effective_directory,system_directory,windows_directory+L"\\System",windows_directory}){
            auto path=candidate(directory,name);if(!path.empty())return path;
        }
        // SetDllDirectory's nonempty path removes the implicit current folder.
        // PATH still admits explicit relative entries, resolved in the child.
        size_t at=0,entries=0;
        while(at<options.child_path.size()){
            if(++entries>512)unsupported("child PATH exceeds startup search bound");
            size_t end=options.child_path.find(L';',at);if(end==str::npos)end=options.child_path.size();str directory=options.child_path.substr(at,end-at);at=end+1;
            if(directory.empty())unsupported("empty child PATH component has ambiguous native search semantics");
            if(directory.find(L'\"')!=str::npos)unsupported("quoted child PATH requires native loader resolution");
            auto path=candidate(native_directory(directory),name);if(!path.empty())return path;
        }
        throw std::runtime_error("cannot resolve non-core startup dependency: "+printable(name));
    }
    void edge(size_t from,const std::string &raw,const std::vector<Symbol> &requested,unsigned api_depth=0){
        str name=from_ansi(raw);while(!name.empty()&&name.back()==L' ')name.pop_back();if(name.empty())bad("empty startup module name");allowed(name);
        bool bare=name.find_first_of(L"\\/:")==str::npos;
        if(bare&&name.find(L'.')==str::npos)name+=L".dll";
        str key=lower(basename(name));
        // API-set names are contracts, not proof of a native-core dependency.
        // Resolve through the native schema without loading code, then inspect
        // the host name too. Unknown contracts/platform APIs fail explicitly.
        // Other api-/ext- filenames remain ordinary files (for example an
        // app-local api-helper.dll may itself import a disabled helper).
        if(bare&&(key.compare(0,7,L"api-ms-")==0||key.compare(0,7,L"ext-ms-")==0)){
            if(api_depth>=16)unsupported("API-set resolution cycle or excessive depth");
            if(!api_sets)api_sets=std::make_unique<NativeApiSets>();
            str target=api_sets->host(name,nodes[from]->pe->file.path);
            edge(from,printable(target),requested,api_depth+1);return;
        }
        // Only actual target-architecture KnownDLLs form the native leaf boundary.
        if(bare&&known.count(key))return;
        if(bare&&(manifest.redirects(name)||(nodes[from]->manifest&&nodes[from]->manifest->redirects(name))))unsupported("side-by-side startup dependency cannot be proven under disabled policy");
        str path;auto forced=preloaded.find(key);auto loaded=by_basename.find(key);
        if(bare&&forced!=preloaded.end())path=forced->second;
        else if(bare&&loaded!=by_basename.end())path=nodes[loaded->second]->pe->file.path;
        else if(bare)path=search(name);
        else {path=resolve_directory(name);if(!file_exists(path))throw std::runtime_error("cannot resolve qualified startup dependency: "+printable(name));}
        size_t target=node(path);auto &n=*nodes[target];
        for(const auto &symbol:requested){
            auto symbol_key=symbol.key();if(n.examined.count(symbol_key))continue;
            if(!n.examining.insert(symbol_key).second)unsupported("cyclic forwarded startup export");
            if(++symbols>max_symbols)unsupported("startup forwarded export budget exceeded");
            auto exported=n.pe->exported(symbol);
            if(!exported.forwarder.empty()){
                auto split=exported.forwarder.find_last_of('.');if(split==std::string::npos||!split||split+1==exported.forwarder.size())bad("malformed forwarded startup export");
                auto dll=exported.forwarder.substr(0,split),function=exported.forwarder.substr(split+1);Symbol next;
                if(function[0]=='#'){char *end=nullptr;unsigned long number=strtoul(function.c_str()+1,&end,10);if(!end||*end||end==function.c_str()+1||number>65535)bad("invalid forwarded startup ordinal");next={true,(WORD)number,{}};}
                else next={false,0,function};
                edge(target,dll,{next});
            }
            n.examining.erase(symbol_key);n.examined.insert(symbol_key);
        }
        dependencies(target);
    }
public:
    DisabledValidator(const str &app,WORD machine,const str &current,const str &private_path,const str &other_path,
        const std::vector<str> &preloads,const std::vector<str> &disabled_names,const DisabledStartupOptions &settings):
        application(normalized(app)),cwd(normalized(current)),app_directory(folder(application)),options(settings),manifest(application,machine){
        result.machine=machine;result.inherited_directory=inherited_directory();
        for(const auto &name:disabled_names)disabled.insert(policy_name(name));
        for(const auto &name:module_known_dlls(machine))known.insert(lower(name));
        str inherited=result.inherited_directory.empty()?str():resolve_directory(result.inherited_directory);
        effective_directory=inherited.empty()||lower(inherited)==lower(normalized(private_path))||lower(inherited)==lower(normalized(other_path))?normalized(private_path):native_directory(inherited);
        std::vector<wchar_t> windows(32767);UINT n=GetSystemWindowsDirectoryW(windows.data(),(UINT)windows.size());if(!n||n>=windows.size())unsupported("cannot resolve native Windows directory");windows_directory=normalized(str(windows.data(),n));
        SYSTEM_INFO system{};GetNativeSystemInfo(&system);
        system_directory=windows_directory+(machine==IMAGE_FILE_MACHINE_I386&&system.wProcessorArchitecture==PROCESSOR_ARCHITECTURE_AMD64?L"\\SysWOW64":L"\\System32");
        for(const auto &path:preloads){allowed(path);str actual=normalized(path),name=lower(basename(actual));auto old=preloaded.find(name);if(old!=preloaded.end()&&lower(old->second)!=lower(actual))unsupported("competing startup preload basenames");preloaded[name]=actual;}
    }
    DisabledStartupValidation run(const std::vector<str> &preloads){
        str local=application+L".local";DWORD attr=GetFileAttributesW(local.c_str());
        if(attr!=INVALID_FILE_ATTRIBUTES)unsupported(".local startup redirection cannot be proven under disabled policy");
        if(GetLastError()!=ERROR_FILE_NOT_FOUND&&GetLastError()!=ERROR_PATH_NOT_FOUND)unsupported("cannot inspect .local startup redirection");missing(local);
        // Reserve all injected basenames before traversing any dependencies.
        for(const auto &path:preloads)node(normalized(path));
        for(const auto &path:preloads)dependencies(by_path.at(lower(normalized(path))));
        dependencies(node(application,true));
        for(const auto &n:nodes)result.observed_files.push_back(n->pe->file);
        result.absent_candidates.assign(absent.begin(),absent.end());return std::move(result);
    }
};

struct String32 {USHORT length,maximum;uint32_t buffer;};
struct String64 {USHORT length,maximum;uint32_t padding;uint64_t buffer;};
struct Basic64 {uint32_t status,padding;uint64_t peb,affinity;uint32_t priority,padding2;uint64_t pid,parent;};
class Remote {
    HANDLE process;bool target32,wide;using Query=NTSTATUS(NTAPI*)(HANDLE,ULONG,PVOID,ULONG,PULONG);using Read64=NTSTATUS(NTAPI*)(HANDLE,uint64_t,PVOID,uint64_t,uint64_t*);using Write64=NTSTATUS(NTAPI*)(HANDLE,uint64_t,const void*,uint64_t,uint64_t*);Query query,query64=nullptr;Read64 read64=nullptr;Write64 write64=nullptr;
public:
    Remote(HANDLE handle,WORD machine):process(handle),target32(machine==IMAGE_FILE_MACHINE_I386),wide(sizeof(void*)==4&&!target32){
        if(machine!=IMAGE_FILE_MACHINE_I386&&machine!=IMAGE_FILE_MACHINE_AMD64)bad("unsupported child machine");HMODULE n=GetModuleHandleW(L"ntdll.dll");query=(Query)GetProcAddress(n,"NtQueryInformationProcess");if(!query)bad("process query API unavailable");
        if(wide){query64=(Query)GetProcAddress(n,"NtWow64QueryInformationProcess64");read64=(Read64)GetProcAddress(n,"NtWow64ReadVirtualMemory64");write64=(Write64)GetProcAddress(n,"NtWow64WriteVirtualMemory64");if(!query64||!read64||!write64)bad("cross-architecture access unavailable");}
        using Machines=BOOL(WINAPI*)(HANDLE,USHORT*,USHORT*);auto machines=(Machines)GetProcAddress(GetModuleHandleW(L"kernel32.dll"),"IsWow64Process2");if(machines){USHORT guest=0,host=0;if(!machines(process,&guest,&host)||(guest?guest:host)!=machine)bad("actual child machine differs from plan");}
    }
    void read(uint64_t address,void *data,size_t size)const{if(address>UINT64_MAX-size)bad("remote address overflow");if(wide){uint64_t done=0;if(read64(process,address,data,size,&done)<0||done!=size)bad("cannot read child imports");}else{if(address>UINTPTR_MAX||size>UINTPTR_MAX-address)bad("remote address does not fit caller");SIZE_T done=0;if(!ReadProcessMemory(process,(void*)(uintptr_t)address,data,size,&done)||done!=size)bad("cannot read child imports");}}
    template<class T>T value(uint64_t address)const{T result{};read(address,&result,sizeof(result));return result;}
    uint64_t pointer(uint64_t address)const{return target32?value<uint32_t>(address):value<uint64_t>(address);}
    void write(uint64_t address,uint64_t value)const{size_t size=target32?4:8;if(address>UINT64_MAX-size)bad("remote write address overflow");if(wide){uint64_t done=0;if(write64(process,address,&value,size,&done)<0||done!=size)bad("cannot bind native injected thunk");}else{if(address>UINTPTR_MAX||size>UINTPTR_MAX-address)bad("remote write does not fit caller");SIZE_T done=0;if(!WriteProcessMemory(process,(void*)(uintptr_t)address,&value,size,&done)||done!=size)bad("cannot bind native injected thunk");}}
    void write_dword(uint64_t address,DWORD value)const{
        if(address>UINT64_MAX-sizeof(value))bad("remote descriptor write overflow");
        if(wide){uint64_t done=0;if(write64(process,address,&value,sizeof(value),&done)<0||done!=sizeof(value))bad("cannot bind selected builtin descriptor");}
        else {if(address>UINTPTR_MAX||sizeof(value)>UINTPTR_MAX-address)bad("remote descriptor does not fit caller");SIZE_T done=0;if(!WriteProcessMemory(process,(void*)(uintptr_t)address,&value,sizeof(value),&done)||done!=sizeof(value))bad("cannot bind selected builtin descriptor");}
    }
    uint64_t peb()const{if(wide){Basic64 info{};if(query64(process,0,&info,sizeof(info),nullptr)<0||!info.peb)bad("cannot find native child PEB");return info.peb;}if(target32){ULONG_PTR wow=0;if(query(process,26,&wow,sizeof(wow),nullptr)>=0&&wow)return wow;}PROCESS_BASIC_INFORMATION info{};if(query(process,0,&info,sizeof(info),nullptr)<0||!info.PebBaseAddress)bad("cannot find child PEB");return (uintptr_t)info.PebBaseAddress;}
    str string(uint64_t descriptor,uint64_t base,uint64_t extent)const{
        USHORT length,maximum;uint64_t address;if(target32){auto s=value<String32>(descriptor);length=s.length;maximum=s.maximum;address=s.buffer;}else{auto s=value<String64>(descriptor);length=s.length;maximum=s.maximum;address=s.buffer;}
        if(length%2||length>maximum||!length||address<base||address-base>extent||length>extent-(address-base))bad("invalid initial child parameter string");str result(length/2,0);read(address,result.data(),length);if(result.find(L'\0')!=str::npos)bad("embedded NUL in initial child string");return result;
    }
    std::string name(uint64_t address)const{std::string result;for(unsigned i=0;i<32767;i++){char c=value<char>(address+i);if(!c)return result;result+=c;}bad("unterminated remote import path");}
};
}

StartupBuiltinPlan plan_builtin_startup_imports(const str &application,WORD machine,const str &child_current_directory,const str &private_directory,const str &other_arch_directory,const std::vector<str> &selected_builtins,const StartupDirectoryMapper &map_custom){
    StartupBuiltinPlan plan;if(selected_builtins.empty())return plan;
    NativeFileView view(machine);
    size_t bytes=0,symbols=0;PeFile app(normalized(application,true),bytes);
    std::map<str,str> selected;std::map<str,std::unique_ptr<PeFile>> providers;std::set<str> selected_names;
    for(const auto &path:selected_builtins){str actual=normalized(path,true),key=lower(basename(actual));selected_names.insert(key);auto inserted=selected.emplace(key,actual);if(!inserted.second&&lower(inserted.first->second)!=lower(actual))bad("competing selected builtin files");}
    for(const auto &entry:app.dependencies(symbols,&selected_names)){
        str key=lower(basename(from_ansi(entry.name)));if(key.find(L'.')==str::npos)key+=L".dll";
        auto selected_file=selected.find(key);if(selected_file==selected.end())continue;
        if(app.machine!=machine)bad("selected static builtin import in a promoted managed image is unsupported");
        auto &provider=providers[key];if(!provider){provider=std::make_unique<PeFile>(selected_file->second,bytes);if(provider->machine!=machine)bad("selected builtin architecture differs from child");}
        for(const auto &symbol:entry.symbols)provider->exported(symbol);
        plan.bindings.push_back({provider->file,entry.descriptor,entry.name,entry.lookup});
    }
    plan.machine=machine;plan.application=app.file;plan.inherited_directory=inherited_directory();
    // Reuse the native planner's bounded app/custom/private graph. A nested
    // native import cannot be changed by rewriting the main executable. Fail
    // a proven SxS conflict instead of announcing Wine while binding native.
    // Host backend graphs and unresolved native search locations stay native.
    try{
        StartupImportPlan graph;graph.machine=machine;graph.application=app.file.path;graph.current_directory=normalized(child_current_directory);graph.custom_directory=plan.inherited_directory;
        if(graph.custom_directory.empty()||!_wcsicmp(graph.custom_directory.c_str(),other_arch_directory.c_str()))graph.custom_directory=private_directory;
        // Normal AnyCPU imports only the native CLR bootstrap. Its promoted
        // image is handled by Detours; no selected direct descriptor was found.
        if(app.machine!=machine&&app.managed_anycpu&&plan.bindings.empty())return plan;
        auto reachable=Planner(graph,private_directory,selected_builtins,map_custom,true).run();
        Manifest app_manifest(app.file.path,machine);
        for(const auto &observed:reachable.observed_files){
            if(lower(observed.path)==lower(app.file.path))continue;
            PeFile dependency(observed.path,bytes);Manifest dll_manifest(observed.path,machine,2);
            for(const auto &entry:dependency.dependencies(symbols)){
                str name=from_ansi(entry.name),key=lower(basename(name));if(key.find(L'.')==str::npos)key+=L".dll";
                if(name.find_first_of(L"\\/:")==str::npos&&selected.count(key)&&(app_manifest.redirects(key,true)||dll_manifest.redirects(key,true)))
                    throw std::runtime_error("Selected Wine startup provider conflicts with a transitive side-by-side import: "+entry.name+" in "+printable(observed.path));
            }
        }
        plan.observed_files=std::move(reachable.observed_files);plan.absent_candidates=std::move(reachable.absent_candidates);
    }catch(const UnsupportedPlan &error){plan.transitive_limit=error.what();}
    return plan;
}

void bind_builtin_startup_imports(HANDLE process,const StartupBuiltinPlan &plan){
    if(!plan.machine)return;
    NativeFileView view(plan.machine);
    auto unchanged=[](const StartupImportFile &expected){Handle file(CreateFileW(expected.path.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr));if(file.value==INVALID_HANDLE_VALUE||!same_identity(expected,identity(file.value,expected.path)))bad("selected builtin or executable changed after planning");};
    unchanged(plan.application);for(const auto &binding:plan.bindings)unchanged(binding.provider);
    if(inherited_directory()!=plan.inherited_directory)bad("DLL directory changed after selected builtin planning");
    for(const auto &observed:plan.observed_files)unchanged(observed);
    for(const auto &absent:plan.absent_candidates)if(file_exists(absent))bad("native dependency appeared after selected builtin planning");
    if(plan.bindings.empty())return;
    Remote remote(process,plan.machine);bool x86=plan.machine==IMAGE_FILE_MACHINE_I386;uint64_t peb=remote.peb();
    uint64_t ldr=remote.pointer(peb+(x86?0x0c:0x18));if(ldr&&remote.value<BYTE>(ldr+4))bad("child loader already initialized before builtin binding");
    uint64_t parameters=remote.pointer(peb+(x86?0x10:0x20));if(!parameters)bad("missing child parameters for builtin binding");uint32_t header[3];remote.read(parameters,header,sizeof(header));if(!(header[2]&1)||header[1]<(x86?0x4cu:0x88u)||header[0]<header[1]||header[0]>16*1024*1024)bad("invalid child parameters for builtin binding");
    auto application=remote.string(parameters+(x86?0x38:0x60),parameters,header[0]);if(lower(normalized(application,true))!=lower(plan.application.path))bad("actual child executable differs from builtin plan");
    uint64_t image=remote.pointer(peb+(x86?8:16));if(!image||image>UINT64_MAX-UINT32_MAX)bad("invalid child image for builtin binding");
    auto dos=remote.value<IMAGE_DOS_HEADER>(image);if(dos.e_magic!=IMAGE_DOS_SIGNATURE||dos.e_lfanew<0||dos.e_lfanew>1024*1024)bad("invalid child DOS header for builtin binding");
    uint64_t nt=image+dos.e_lfanew;IMAGE_DATA_DIRECTORY imports{};
    if(x86){auto h=remote.value<IMAGE_NT_HEADERS32>(nt);if(h.Signature!=IMAGE_NT_SIGNATURE||h.FileHeader.Machine!=plan.machine||h.OptionalHeader.Magic!=IMAGE_NT_OPTIONAL_HDR32_MAGIC)bad("child PE32 differs from builtin plan");imports=h.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];}
    else {auto h=remote.value<IMAGE_NT_HEADERS64>(nt);if(h.Signature!=IMAGE_NT_SIGNATURE||h.FileHeader.Machine!=plan.machine||h.OptionalHeader.Magic!=IMAGE_NT_OPTIONAL_HDR64_MAGIC)bad("child PE64 differs from builtin plan");imports=h.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];}
    if(!imports.VirtualAddress||imports.Size<sizeof(IMAGE_IMPORT_DESCRIPTOR)||imports.Size>1024*1024||image>UINT64_MAX-imports.VirtualAddress-imports.Size)bad("invalid copied builtin import table bounds");
    uint64_t begin=image+imports.VirtualAddress,end=begin+imports.Size,flag=x86?IMAGE_ORDINAL_FLAG32:IMAGE_ORDINAL_FLAG64;size_t width=x86?4:8;
    std::map<str,DWORD> preload_names;std::vector<std::pair<uint64_t,IMAGE_IMPORT_DESCRIPTOR>> originals;bool terminated=false;
    for(size_t i=0;i<max_descriptors&&(i+1)*sizeof(IMAGE_IMPORT_DESCRIPTOR)<=imports.Size;i++){
        uint64_t address=begin+i*sizeof(IMAGE_IMPORT_DESCRIPTOR);auto d=remote.value<IMAGE_IMPORT_DESCRIPTOR>(address);
        if(!d.Name&&!d.OriginalFirstThunk&&!d.FirstThunk){terminated=true;break;}if(!d.Name)bad("invalid copied builtin import descriptor");
        uint64_t lookup=image+d.OriginalFirstThunk,iat=image+d.FirstThunk,name=image+d.Name;
        bool injected=d.OriginalFirstThunk&&d.FirstThunk&&lookup>=begin&&lookup<end&&iat>=begin&&iat<end;
        if(!injected){originals.push_back({address,d});continue;}
        if(2*width>end-lookup||2*width>end-iat||name<begin||name>=end||d.TimeDateStamp||d.ForwarderChain||remote.pointer(lookup)!=(flag|1)||remote.pointer(iat)!=(flag|1)||remote.pointer(lookup+width)||remote.pointer(iat+width))continue;
        // Names reused by the rewrite must live entirely in Detours' writable
        // allocation. Never point a descriptor at an unrelated child string.
        std::string raw;bool ended=false;for(size_t j=0;j<32767&&j<end-name;j++){char c=remote.value<char>(name+j);if(!c){ended=true;break;}raw+=c;}if(!ended)bad("unbounded injected builtin path");
        str path=lower(from_ansi(raw));if(!preload_names.emplace(path,d.Name).second)bad("duplicate injected builtin path");
    }
    if(!terminated)bad("unterminated copied builtin import descriptors");
    struct Write {uint64_t address;DWORD value;};std::vector<Write> writes;std::set<uint64_t> matched;
    for(const auto &binding:plan.bindings){
        auto preload=preload_names.find(lower(binding.provider.path));if(preload==preload_names.end())bad("selected builtin preload missing or changed");
        uint64_t found=0;
        for(const auto &entry:originals){
            if(memcmp(&entry.second,&binding.descriptor,sizeof(binding.descriptor)))continue;
            if(found)bad("ambiguous original builtin import descriptor");found=entry.first;
        }
        if(!found||!matched.insert(found).second)bad("original builtin import descriptor missing or changed");
        const auto &d=binding.descriptor;if(remote.name(image+d.Name)!=binding.module)bad("original builtin import name changed");
        uint64_t lookup=image+(d.OriginalFirstThunk?d.OriginalFirstThunk:d.FirstThunk);
        for(size_t i=0;i<binding.lookup.size();i++)if(remote.pointer(lookup+i*width)!=binding.lookup[i])bad("original builtin import lookup changed");
        if(remote.pointer(lookup+binding.lookup.size()*width))bad("original builtin import lookup termination changed");
        writes.push_back({found+offsetof(IMAGE_IMPORT_DESCRIPTOR,Name),preload->second});
        // A bound import belongs to its original provider. Force normal symbol
        // resolution from the unchanged original lookup table for the builtin.
        if(d.TimeDateStamp)writes.push_back({found+offsetof(IMAGE_IMPORT_DESCRIPTOR,TimeDateStamp),0});
        if(d.ForwarderChain)writes.push_back({found+offsetof(IMAGE_IMPORT_DESCRIPTOR,ForwarderChain),0});
    }
    // The caller must terminate on any write failure, including partial writes.
    for(const auto &write:writes)remote.write_dword(write.address,write.value);
}

StartupImportPlan plan_native_startup_imports(const str &application,WORD machine,const str &child_current_directory,const str &private_directory,const str &other_arch_directory,const std::vector<str> &selected_builtins,const StartupDirectoryMapper &map_custom){
    StartupImportPlan plan;plan.machine=machine;plan.application=application;plan.current_directory=child_current_directory;
    try{
        DWORD size=GetDllDirectoryW(0,nullptr);if(!size)return plan;if(size>32767)unsupported("inherited DLL directory is too long");std::vector<wchar_t> directory(size+1);
        // Windows may return one (the terminator) for the required capacity
        // of an empty DLL directory, then zero when copying that empty value.
        SetLastError(ERROR_SUCCESS);DWORD count=GetDllDirectoryW((DWORD)directory.size(),directory.data());DWORD error=GetLastError();
        if(!count){if(error!=ERROR_SUCCESS)unsupported("cannot read inherited DLL directory");return plan;}
        if(count>=directory.size())unsupported("inherited DLL directory changed during query");plan.custom_directory.assign(directory.data(),count);
        if(!_wcsicmp(plan.custom_directory.c_str(),private_directory.c_str())||!_wcsicmp(plan.custom_directory.c_str(),other_arch_directory.c_str()))return plan;
        plan.application=normalized(application);plan.current_directory=normalized(child_current_directory);
        return Planner(plan,private_directory,selected_builtins,map_custom).run();
    }catch(const UnsupportedPlan &error){plan.bindings.clear();plan.observed_files.clear();plan.absent_candidates.clear();plan.unsupported_reason=error.what();return plan;}
}

DisabledStartupValidation validate_disabled_startup_imports(const str &application,WORD machine,
    const str &child_current_directory,const str &private_directory,const str &other_arch_directory,
    const std::vector<str> &preloaded_paths,const std::vector<str> &disabled,const DisabledStartupOptions &options){
    if(disabled.empty())return {};
    try{
        if(machine!=IMAGE_FILE_MACHINE_I386&&machine!=IMAGE_FILE_MACHINE_AMD64)unsupported("unsupported disabled-policy child architecture");
        NativeFileView view(machine);
        return DisabledValidator(application,machine,child_current_directory,private_directory,other_arch_directory,preloaded_paths,disabled,options).run(preloaded_paths);
    }catch(const std::exception &error){throw std::runtime_error(std::string("Disabled DLL startup validation: ")+error.what());}
}
void recheck_disabled_startup_imports(const DisabledStartupValidation &validation){
    if(!validation.machine)return;
    try{
        NativeFileView view(validation.machine);
        if(inherited_directory()!=validation.inherited_directory)unsupported("inherited DLL directory changed after disabled-policy validation");
        for(const auto &path:validation.absent_candidates){
            bool local=path.size()>=6&&lower(path.substr(path.size()-6))==L".local";
            if(local?GetFileAttributesW(path.c_str())!=INVALID_FILE_ATTRIBUTES:file_exists(path))unsupported("startup search candidate appeared after disabled-policy validation");
        }
        for(const auto &file:validation.observed_files){Handle opened(CreateFileW(file.path.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr));
            if(opened.value==INVALID_HANDLE_VALUE||!same_identity(file,identity(opened.value,file.path)))unsupported("startup dependency changed after disabled-policy validation");}
    }catch(const std::exception &error){throw std::runtime_error(std::string("Disabled DLL startup recheck: ")+error.what());}
}

void bind_native_startup_imports(HANDLE process,const StartupImportPlan &plan){
    if(plan.bindings.empty())return;Remote remote(process,plan.machine);bool x86=plan.machine==IMAGE_FILE_MACHINE_I386;uint64_t peb=remote.peb(),ldr=remote.pointer(peb+(x86?0x0c:0x18));if(ldr&&remote.value<BYTE>(ldr+4))bad("child loader is already initialized");
    uint64_t parameters=remote.pointer(peb+(x86?0x10:0x20));if(!parameters)bad("missing initial child parameters");uint32_t header[3];remote.read(parameters,header,sizeof(header));if(!(header[2]&1)||header[1]<(x86?0x4cu:0x88u)||header[0]<header[1]||header[0]>16*1024*1024)bad("unsupported child parameter layout");
    auto custom=remote.string(parameters+(x86?0x30:0x50),parameters,header[0]);if(custom!=plan.custom_directory)bad("inherited application DLL directory changed after planning");
    auto application=remote.string(parameters+(x86?0x38:0x60),parameters,header[0]);if(lower(normalized(application))!=lower(plan.application))bad("actual executable differs from plan");
    auto current=remote.string(parameters+(x86?0x24:0x38),parameters,header[0]);if(lower(normalized(current))!=lower(plan.current_directory))bad("actual child current directory differs from plan");
    uint64_t image=remote.pointer(peb+(x86?8:16));if(!image||image>UINT64_MAX-UINT32_MAX)bad("invalid suspended image address");auto dos=remote.value<IMAGE_DOS_HEADER>(image);if(dos.e_magic!=IMAGE_DOS_SIGNATURE||dos.e_lfanew<0||dos.e_lfanew>1024*1024)bad("invalid suspended image DOS header");
    uint64_t nt=image+dos.e_lfanew;IMAGE_DATA_DIRECTORY imports{};if(x86){auto h=remote.value<IMAGE_NT_HEADERS32>(nt);if(h.Signature!=IMAGE_NT_SIGNATURE||h.FileHeader.Machine!=plan.machine||h.OptionalHeader.Magic!=IMAGE_NT_OPTIONAL_HDR32_MAGIC)bad("suspended PE32 differs from plan");imports=h.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];}else{auto h=remote.value<IMAGE_NT_HEADERS64>(nt);if(h.Signature!=IMAGE_NT_SIGNATURE||h.FileHeader.Machine!=plan.machine||h.OptionalHeader.Magic!=IMAGE_NT_OPTIONAL_HDR64_MAGIC)bad("suspended PE64 differs from plan");imports=h.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];}
    if(!imports.VirtualAddress||imports.Size<sizeof(IMAGE_IMPORT_DESCRIPTOR)||imports.Size>1024*1024||image>UINT64_MAX-imports.VirtualAddress-imports.Size)bad("invalid remote import table bounds");uint64_t begin=image+imports.VirtualAddress,end=begin+imports.Size;
    for(const auto &path:plan.absent_candidates)if(file_exists(path))bad("native search candidate appeared after planning");
    for(const auto &observed:plan.observed_files){Handle file(CreateFileW(observed.path.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr));if(file.value==INVALID_HANDLE_VALUE||!same_identity(observed,identity(file.value,observed.path)))bad("native dependency changed after planning");}
    std::map<str,const StartupImportBinding*> wanted;for(const auto &binding:plan.bindings)if(!wanted.emplace(lower(binding.file.path),&binding).second)bad("duplicate planned preload");
    struct Write {uint64_t lookup,iat,value;};std::vector<Write> writes;std::set<str> found;bool terminated=false;size_t width=x86?4:8;uint64_t flag=x86?IMAGE_ORDINAL_FLAG32:IMAGE_ORDINAL_FLAG64;
    for(size_t i=0;i<max_descriptors&&(i+1)*sizeof(IMAGE_IMPORT_DESCRIPTOR)<=imports.Size;i++){
        auto d=remote.value<IMAGE_IMPORT_DESCRIPTOR>(begin+i*sizeof(IMAGE_IMPORT_DESCRIPTOR));if(!d.Name&&!d.OriginalFirstThunk&&!d.FirstThunk){terminated=true;break;}if(!d.Name)bad("invalid remote import descriptor");str name=lower(from_ansi(remote.name(image+d.Name)));auto desired=wanted.find(name);if(desired==wanted.end())continue;
        uint64_t lookup=image+d.OriginalFirstThunk,iat=image+d.FirstThunk;
        if(!d.OriginalFirstThunk||!d.FirstThunk||lookup<begin||iat<begin||lookup>end||iat>end||2*width>end-lookup||2*width>end-iat||d.TimeDateStamp||d.ForwarderChain)bad("native import is not a bounded Detours placeholder");
        if(remote.pointer(lookup)!=(flag|1)||remote.pointer(iat)!=(flag|1)||remote.pointer(lookup+width)||remote.pointer(iat+width)||!found.insert(name).second)bad("native import placeholder is ambiguous or changed");writes.push_back({lookup,iat,flag|desired->second->ordinal});
    }
    if(!terminated||found.size()!=wanted.size())bad("planned native preload missing from suspended import table");
    // Validate the whole plan before the first mutation. If a write fails,
    // termination by the caller is mandatory; the child must never be resumed.
    for(const auto &write:writes){remote.write(write.lookup,write.value);remote.write(write.iat,write.value);}
}
}
