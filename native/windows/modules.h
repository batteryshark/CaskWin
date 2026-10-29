// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once
#include "common.h"
#include "known-dlls.h"
namespace wd {
struct ModuleSelection {
    std::vector<str> builtins;
    // Canonical lowercase basenames including the DLL extension. Executable
    // loads must fail; resource-only loads and fixed native backends are separate.
    std::vector<str> disabled;
};
ModuleSelection select_module_policy(const Prefix&,const str &program,
    const str &runtime=module_dir(),const str &overrides=env(L"WINEDLLOVERRIDES"));
std::vector<str> read_disabled_modules(const Prefix&,const str &program,
    const str &overrides=env(L"WINEDLLOVERRIDES"));
}
