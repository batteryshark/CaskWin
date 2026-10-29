// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once
#include <windows.h>
#include <winternl.h>
#include <string>
namespace wd {
using RegistryOpenKeyFn=NTSTATUS(NTAPI*)(PHANDLE,ACCESS_MASK,POBJECT_ATTRIBUTES);
using RegistryQueryKeyFn=NTSTATUS(NTAPI*)(HANDLE,int,PVOID,ULONG,PULONG);
enum class RegistryView { unknown, view32, view64 };
struct RegistryViewRoute {std::wstring name;RegistryView view;};
// Call only for opens/creates, after translating the private name to its logical
// NT name. Supply original parent metadata and unhooked native trampolines.
// Never call when remapping writes/deletes through an existing handle: its
// canonical physical name already identifies the intended view.
RegistryViewRoute registry_view_name(const std::wstring &logical,ACCESS_MASK access,
    HANDLE parent,const std::wstring &parent_name,RegistryOpenKeyFn,RegistryQueryKeyFn);
// Apply only to the real private open/create after selecting its physical path.
// The kernel then preserves the view on inherited and duplicated key handles.
// Explicit flags, including invalid combinations, retain native validation.
ACCESS_MASK registry_view_access(ACCESS_MASK,RegistryView);
// Windows REGEDIT/5.0 imports: resolve a Machine/User/Default private path through
// the explicitly selected Win32 registry view, irrespective of launcher bitness.
// Wine text hives already describe physical paths and must not call this helper.
std::wstring registry_import_name(const std::wstring &private_name,REGSAM view);
}
