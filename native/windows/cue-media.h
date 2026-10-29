// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once
#include "media.h"

namespace wd {
struct MountedCue {
    MountedImage data;
    str audio, cue, bin;
    DWORD tracks=0;
};
// Supported subset: one local BINARY .bin file, track 01 MODE1/2352 at INDEX01
// zero, followed by AUDIO tracks. INDEX00 bytes and PREGAP silence are retained.
// Returns fully prepared private media; the caller publishes all fields atomically.
MountedCue prepare_cue(const str &prefix_root,const str &source_cue);
}
