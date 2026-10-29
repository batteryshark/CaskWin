// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once
#include "common.h"

namespace wd {
struct MountedImage {
    str path, image, label;
    DWORD serial = 0, sector_size = 2048;
    ULONGLONG image_size = 0, volume_size = 0;
};
// Validate an ISO9660/Joliet image, retain a private immutable sector snapshot,
// and materialize its filesystem. This never mounts a Windows volume/device.
MountedImage prepare_iso(const str &prefix_root, const str &source);
}
