// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once
#include "common.h"

namespace wd {
// Initialize under the shim's Detours transaction. The identity must be the
// already validated loader-owned MediaSession name. enabled is active&&!bypass.
// This isolates ordinary application IPC, not hostile code or security tokens.
void attach_named_objects(const str &session_identity,bool (*enabled)());

// For the existing NtCreateSection/Ex hooks: construct after their enabled check
// (the shim recursion guard may already be active), then pass get() to ntdll.
// Only named BaseNamedObjects objects from application/Wine/CLR callers change.
// Native Windows backend callers and unknown origins retain the host namespace.
class NamedObjectAttributes {
    str storage;
    UNICODE_STRING name{};
    OBJECT_ATTRIBUTES attributes{};
    POBJECT_ATTRIBUTES selected;
public:
    explicit NamedObjectAttributes(POBJECT_ATTRIBUTES original);
    POBJECT_ATTRIBUTES get(){return selected;}
    bool mapped()const{return selected==&attributes;}
    NamedObjectAttributes(const NamedObjectAttributes&)=delete;
    NamedObjectAttributes&operator=(const NamedObjectAttributes&)=delete;
};
}
