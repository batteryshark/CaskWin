// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once
#include "common.h"
#include <memory>

namespace wd {
inline constexpr wchar_t media_session_environment_name[]=L"WINEDOWS_MEDIA_SESSION";
enum class MediaSessionMode { create, attach };
struct MediaState {
    bool managed=false,present=true,ejectable=false;
    DWORD changes=0,removal_locks=0,medium=0;
};
struct MediaIdentity {int drive=-1;DWORD medium=0;bool raw=false;};
// A loader owns this pagefile mapping for the existing job/session-lock lifetime.
// create publishes its name in the environment; attach opens that exact mapping.
// Child environment construction must preserve the key even with custom blocks.
// Presence is session-scoped: a new loader starts each configured medium present.
class MediaSession {
    struct Impl;
    std::unique_ptr<Impl> impl;
public:
    MediaSession(const Prefix &prefix,MediaSessionMode mode);
    ~MediaSession();
    MediaSession(const MediaSession&)=delete;
    MediaSession& operator=(const MediaSession&)=delete;
    const str &name() const;
    MediaState snapshot(int drive) const;
    // Fast all-present path for hooks before resolving a file handle's drive.
    bool any_absent() const;
    // Includes replaced media: old handles still need identity checks when the
    // new disc is present. Unique immutable paths retain cross-process identity.
    bool needs_handle_check() const;
    MediaIdentity path_identity(const str &path) const;
    // Per-thread cached immutable views: no file access or hot-path mutex.
    std::shared_ptr<const Prefix> view() const;
    // Only the loader-owned instance may replace a fully prepared medium.
    // Returns the new change counter. Never modifies persistent configuration.
    DWORD replace(int drive,const Drive &medium);
    // Call under the shim recursion guard, before CDDA dispatch. This handles
    // only virtual-media control requests; it never calls host DeviceIoControl.
    bool ioctl(int drive,DWORD code,void *input,DWORD input_size,void *output,
               DWORD output_size,DWORD *done,OVERLAPPED *overlapped,BOOL &result);
};
}
