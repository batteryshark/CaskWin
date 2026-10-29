#include <windows.h>
#include <cstdio>
#include <string>
#include <vector>
#include <stdexcept>

static void require(bool condition,const char *message)
{ if(!condition)throw std::runtime_error(message); }
static std::vector<unsigned char> read_file(const std::wstring &path)
{
    HANDLE file=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
            nullptr,OPEN_EXISTING,0,nullptr);
    require(file!=INVALID_HANDLE_VALUE,"sentinel file missing");
    DWORD size=GetFileSize(file,nullptr),read=0; require(size<4096,"unexpected sentinel size");
    std::vector<unsigned char> contents(size);
    bool okay=ReadFile(file,contents.data(),size,&read,nullptr)&&read==size;
    CloseHandle(file);require(okay,"sentinel read failed");return contents;
}
int wmain(int argc,wchar_t **argv)
{
    try {
        require(argc==3,"expected winevulkan and forwarder paths");
        require(GetModuleHandleW(sizeof(void*)==8?L"winedows64.dll":L"winedows32.dll")!=nullptr,
                "registration fixture requires injected private-prefix runtime");
        for(int i=1;i<=2;i++) {
            DWORD ignored=0,size=GetFileVersionInfoSizeW(argv[i],&ignored);
            require(size>0,"version resource missing");
            std::vector<unsigned char> data(size);
            require(GetFileVersionInfoW(argv[i],0,size,data.data()),"version resource read failed");
            VS_FIXEDFILEINFO *version=nullptr; UINT length=0;
            require(VerQueryValueW(data.data(),L"\\",(void**)&version,&length)
                    &&length>=sizeof(*version)&&version->dwSignature==0xfeef04bd
                    &&version->dwFileType==VFT_DLL,"invalid version information");
            if(i==2)require(version->dwFileVersionMS==0x10000&&!version->dwFileVersionLS,
                    "forwarder must retain upstream 1.0.0.0 installer version");
            std::printf("PASS version resource file=%d version=%u.%u.%u.%u\n",i,
                    HIWORD(version->dwFileVersionMS),LOWORD(version->dwFileVersionMS),
                    HIWORD(version->dwFileVersionLS),LOWORD(version->dwFileVersionLS));
        }
        WCHAR system[MAX_PATH];require(GetSystemDirectoryW(system,MAX_PATH)>0,"system directory");
        std::wstring json=std::wstring(system)+L"\\winevulkan.json";
        auto before=read_file(json);
        const std::string marker(before.begin(),before.end());
        require(marker=="WINEDOWS_PRIVATE_REGISTRATION_SENTINEL","wrong private sentinel");
        constexpr const WCHAR *key=L"Software\\Khronos\\Vulkan\\Drivers";
        DWORD valueBefore=0,typeBefore=0,sizeBefore=sizeof(valueBefore);
        LSTATUS registryBefore=RegGetValueW(HKEY_LOCAL_MACHINE,key,json.c_str(),RRF_RT_REG_DWORD,
                &typeBefore,&valueBefore,&sizeBefore);
        HMODULE module=LoadLibraryExW(argv[1],nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);
        require(module!=nullptr,"WineVulkan load");
        require(FindResourceW(module,MAKEINTRESOURCEW(VS_VERSION_INFO),RT_VERSION)!=nullptr,
                "loaded WineVulkan is missing its version resource");
        require(!FindResourceW(module,L"winevulkan_json",RT_RCDATA),"native frontend must not advertise recursive ICD manifest");
        for(const char *name:{"DllRegisterServer","DllUnregisterServer"}) {
            auto entry=(HRESULT(WINAPI*)())GetProcAddress(module,name);require(entry!=nullptr,"registration export missing");
            HRESULT result=entry();require(result==HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED),"registration must explicitly report unsupported");
            require(read_file(json)==before,"registration changed private sentinel");
            DWORD valueAfter=0,typeAfter=0,sizeAfter=sizeof(valueAfter);
            LSTATUS registryAfter=RegGetValueW(HKEY_LOCAL_MACHINE,key,json.c_str(),RRF_RT_REG_DWORD,
                    &typeAfter,&valueAfter,&sizeAfter);
            require(registryAfter==registryBefore && (registryBefore!=ERROR_SUCCESS
                    ||(typeAfter==typeBefore &&valueAfter==valueBefore &&sizeAfter==sizeBefore)),"registration changed ICD registry value");
            std::printf("PASS %s unsupported without file/registry writes\n",name);
        }
        FreeLibrary(module);return 0;
    }catch(const std::exception &error){std::fprintf(stderr,"FAIL %s\n",error.what());return 1;}
}
