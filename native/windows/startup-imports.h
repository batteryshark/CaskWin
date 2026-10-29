// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once
#include "common.h"
#include <cstdint>
#include <functional>

namespace wd {
struct StartupImportFile {
    str path;
    DWORD volume=0,index_high=0,index_low=0,size_high=0,size_low=0,time_high=0,time_low=0;
};
struct StartupImportBinding {
    StartupImportFile file;
    WORD ordinal=0;
    std::string symbol;
};
struct StartupImportPlan {
    WORD machine=0;
    str application,custom_directory,current_directory;
    // Pre-creation analysis could not safely plan this native-valid layout.
    // Keep the existing native launch/search behavior; bindings is empty.
    std::string unsupported_reason;
    std::vector<StartupImportBinding> bindings;
    std::vector<StartupImportFile> observed_files;
    std::vector<str> absent_candidates;
};
struct StartupDirectoryPaths {str overlay,native;};
using StartupDirectoryMapper=std::function<StartupDirectoryPaths(const str&)>;

// Experimental, opt-in integration. Append binding.file.path values, in order,
// after the shim and selected Wine DLLs passed to Detours. The child MUST remain
// suspended until bind_native_startup_imports succeeds. No disk PE is modified.
// An empty/owned inherited directory needs the ordinary startup-native helper.
// map_custom, when supplied, maps the resolved inherited directory to its
// prefix overlay and target-architecture native view. Both paths are absolute.
// Search the overlay before its native host directory; preload overlay
// winners because the native initial loader cannot translate logical paths.
// The actual inherited DllPath is retained unchanged for binding validation.
StartupImportPlan plan_native_startup_imports(const str &application,WORD machine,
    const str &child_current_directory,const str &private_directory,
    const str &other_arch_directory,const std::vector<str> &selected_builtins,
    const StartupDirectoryMapper &map_custom={});

// Verify the actual suspended child and Detours' exact single-ordinal import
// layout, then replace only those placeholder thunk values. On ANY exception,
// the caller must terminate the suspended child, including after partial writes.
// Does not change suspension, the environment, DllPath, or PE headers.
void bind_native_startup_imports(HANDLE process,const StartupImportPlan &plan);

struct StartupBuiltinBinding {
    StartupImportFile provider;
    IMAGE_IMPORT_DESCRIPTOR descriptor{};
    std::string module;
    std::vector<uint64_t> lookup;
};
struct StartupBuiltinPlan {
    WORD machine=0;
    StartupImportFile application;
    std::vector<StartupBuiltinBinding> bindings;
    std::vector<StartupImportFile> observed_files;
    std::vector<str> absent_candidates;
    str inherited_directory;
    std::string transitive_limit;
};
// Explicit Wine selection must also bind the application's original imports:
// preloading a DLL alone does not override Windows' side-by-side resolution.
// These helpers share the bounded PE/remote reader with native preload binding.
// Only matching main-EXE descriptors in Detours' writable copied table change;
// original file bytes, native/SxS imports and import symbol tables stay intact.
StartupBuiltinPlan plan_builtin_startup_imports(const str &application,WORD machine,
    const str &child_current_directory,const str &private_directory,const str &other_arch_directory,
    const std::vector<str> &selected_builtins,const StartupDirectoryMapper &map_custom={});
void bind_builtin_startup_imports(HANDLE process,const StartupBuiltinPlan &plan);

struct DisabledStartupOptions {
    // The actual child's PATH, including a caller-supplied environment block.
    str child_path;
    StartupDirectoryMapper map_directory;
};
struct DisabledStartupValidation {
    WORD machine=0;
    str inherited_directory;
    std::vector<StartupImportFile> observed_files;
    std::vector<str> absent_candidates;
};
// Read-only startup graph validation. Run after native preload planning and
// before process creation; preloaded_paths is the exact injected list excluding
// the runtime shim. Unknown resolution layouts fail explicitly while a disable
// policy is active. An empty disabled list is an unconditional no-op.
DisabledStartupValidation validate_disabled_startup_imports(const str &application,WORD machine,
    const str &child_current_directory,const str &private_directory,const str &other_arch_directory,
    const std::vector<str> &preloaded_paths,const std::vector<str> &disabled,
    const DisabledStartupOptions &options);
// Before resuming the suspended child, reject changed files, newly present
// higher-priority candidates, or a changed inherited directory. No mutation.
void recheck_disabled_startup_imports(const DisabledStartupValidation &validation);
}
