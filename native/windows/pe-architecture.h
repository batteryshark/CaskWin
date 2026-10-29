// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once
#include "common.h"
namespace wd {
struct PeArchitecture {
    WORD machine=0;
    bool dll=false,managed=false;
    DWORD clr_flags=0;
    // Only IL-only I386 executables can request the host CLR architecture.
    // DLL validation must retain the image's declared machine.
    WORD process_machine(WORD native_machine) const;
};
PeArchitecture inspect_pe_architecture(const str &path);
// Architecture Windows will choose for an executable on this host. For native
// DLL compatibility checks use inspect_pe_architecture(path).machine instead.
WORD program_machine(const str &path);
}
