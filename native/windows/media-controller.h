// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once
#include "media-state.h"
namespace wd {
enum class MediaControl : DWORD { swap=1,eject=2,load=3 };
struct MediaControlResult {DWORD changes=0;str message;};
// Construct only in the lock-owning loader, after MediaSession and before launch.
// Destruction stops the listener before the session mapping/lock are released.
class MediaController {
    struct Impl;std::unique_ptr<Impl> impl;
public:
    MediaController(const Prefix &configured,MediaSession &session);
    ~MediaController();
    MediaController(const MediaController&)=delete;
    MediaController& operator=(const MediaController&)=delete;
};
// CLI entry before SessionLock acquisition. This sends a request to the existing
// lock owner; it never prepares media or modifies drive configuration itself.
MediaControlResult control_media(const str &prefix_root,MediaControl operation,int drive,
                                const str &source=L"",const str &label=L"");
}
