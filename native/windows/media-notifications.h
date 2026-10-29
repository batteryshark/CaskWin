// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once
#include "media-state.h"

namespace wd {
// Counters encode real alternating presence transitions: a present-to-present
// swap contributes removal + arrival. An initial snapshot is only a baseline.
// A gap above the bound, or inconsistent parity, is discarded, never invented.
struct MediaNotificationBatch {DWORD transitions=0;bool gap=false;};
inline MediaNotificationBatch media_notification_batch(const MediaState &before,const MediaState &after){
    if(!before.managed||!after.managed)return {};
    DWORD count=after.changes-before.changes;
    if(count>4096||bool(before.present^(count&1))!=after.present)return {0,true};
    return {count,false};
}

// Loader-owned observer, constructed after its job/optional desktop container
// exist and before resuming the first application. The duplicated job handle
// and bounded sends make destruction independent of the launcher's handle.
// No window message is ever sent from a shim, media control hook, or IPC lock.
class MediaNotifications {
    struct Impl;std::unique_ptr<Impl> impl;
public:
    MediaNotifications(MediaSession &session,HANDLE job,HWND desktop=nullptr);
    ~MediaNotifications();
    MediaNotifications(const MediaNotifications&)=delete;
    MediaNotifications& operator=(const MediaNotifications&)=delete;
    void stop();
};
}
