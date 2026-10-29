// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once
#include "common.h"
#include <memory>

namespace wd {
// The first loader owns the prefix until every application exits. Admission and
// final export use the same file lock, including across x86/x64 loaders.
class PrefixSession {
    struct Impl;std::unique_ptr<Impl> impl;
public:
    PrefixSession(const Prefix &prefix,bool allow_join);
    ~PrefixSession();
    PrefixSession(const PrefixSession&)=delete;
    PrefixSession& operator=(const PrefixSession&)=delete;
    bool owner() const;
    void create_job(const str &media_identity);
    HANDLE job() const;
    void publish(const str &desktop_specification,HWND desktop);
    void attach_desktop(const str &explicit_specification);
    STARTUPINFOEXW &startup();
    void started();
    void release_admission();
    // Owner returns with admission held, so destruction/export cannot race a
    // new launch. A joining loader waits only for its own nested process tree.
    void wait();
};
}
