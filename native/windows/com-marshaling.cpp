// SPDX-License-Identifier: LGPL-2.1-or-later
#include "com-marshaling.h"
#include "registry-views.h"
#include <string>
#include <stdexcept>

namespace wd {
namespace {
// Wine's include/objidlbase.idl defines these standard interfaces, and
// dlls/ole32/ole32_objidl.idl registers their PSFactoryBuffer coclass. Retain
// those identities while using the native COM backend's proxy implementation.
struct Interface {const wchar_t *iid,*name;};
const Interface interfaces[]={
    {L"{0000000C-0000-0000-C000-000000000046}",L"IStream"},
    {L"{0C733A30-2A1C-11CE-ADE5-00AA0044773D}",L"ISequentialStream"}
};
const wchar_t *proxy=L"{00000320-0000-0000-C000-000000000046}";
void checked(LSTATUS status,const char *what) {
    if(status)throw std::runtime_error(std::string(what)+" ("+std::to_string(status)+")");
}
struct Key {HKEY value=nullptr;~Key(){if(value)RegCloseKey(value);}};
void set(HKEY key,const wchar_t *name,const wchar_t *value) {
    checked(RegSetValueExW(key,name,0,REG_SZ,(const BYTE*)value,(DWORD)((wcslen(value)+1)*sizeof(wchar_t))),"Write private standard COM default");
}
bool create_absent(HKEY hive,const std::wstring &path,Key &key) {
    DWORD disposition=0;
    checked(RegCreateKeyExW(hive,path.c_str(),0,nullptr,0,KEY_ALL_ACCESS,nullptr,&key.value,&disposition),"Open private standard COM registration");
    return disposition==REG_CREATED_NEW_KEY;
}
}
void initialize_standard_com(HKEY hive) {
    for(REGSAM view:{KEY_WOW64_32KEY,KEY_WOW64_64KEY}) {
        const std::wstring base=L"Machine\\Software\\Classes\\";
        // Native COM supplies PSFactoryBuffer intrinsically, even without a
        // CLSID registry entry. Only the interface-to-proxy mappings are needed;
        // no host class values or server paths are imported into the prefix.
        for(const auto &entry:interfaces) {
            Key iface;auto path=registry_import_name(base+L"Interface\\"+entry.iid,view);
            if(!create_absent(hive,path,iface))continue;
            set(iface.value,nullptr,entry.name);
            Key mapping;create_absent(iface.value,L"ProxyStubClsid32",mapping);
            set(mapping.value,nullptr,proxy);
        }
    }
}
}
