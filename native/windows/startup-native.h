// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once
#include "common.h"

namespace wd {
inline constexpr wchar_t startup_native_environment_name[]=L"WINEDOWS_NATIVE_DLL_DIRECTORY";
enum class StartupNativeResult { configured, inherited_directory_preserved };

// The process must still be suspended before its first loader initialization.
// directory must already be present in the child's environment under the key
// above. Only the child's in-memory loader parameters are changed.
StartupNativeResult configure_native_startup(HANDLE process,WORD machine,
    const str &directory,const str &other_arch_directory);
}
