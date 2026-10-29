// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once
#include "common.h"
namespace wd {
// Call inside the shim's Detours transaction, before attach_audio_backend().
// This discovers only modules already imported by the launcher; it never loads.
void attach_module_runtime(const str &runtime_directory,const std::vector<str> &disabled={});
// Called by the shim's image-section hooks. Data/resource mappings and trusted
// native backend loads retain native behavior. This is a compatibility policy.
bool module_image_disabled(HANDLE file,ULONG allocation);
// Remember a disabled requested basename before file reparse resolution loses
// it. Called only after a successful intercepted file open. On failure the
// caller closes the newly opened handle and reports the returned NT status.
NTSTATUS module_file_opened(HANDLE file,const UNICODE_STRING *requested) noexcept;
// Borrowed handle: callers must not FreeLibrary without acquiring a reference.
HMODULE selected_builtin_module(const wchar_t *basename);
// Owned load reference. The caller must bypass prefix file hooks while loading
// this fixed Windows controller backend; no application path is accepted.
HMODULE load_system_xinput();
HMODULE load_system_dxgi();
HMODULE load_system_d3d11();
HMODULE load_system_vulkan();
class SystemBackendScope {
public:
    SystemBackendScope();
    ~SystemBackendScope();
    SystemBackendScope(const SystemBackendScope&)=delete;
    SystemBackendScope& operator=(const SystemBackendScope&)=delete;
};
}
