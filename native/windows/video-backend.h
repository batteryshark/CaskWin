// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once
#include <windows.h>
namespace wd {
// Optional native Media Foundation defaults for local MP4/H.264/AAC playback
// and decoded video color conversion.
// Writes only the supplied private hive and preserves existing registrations.
void initialize_video_backend(HKEY hive);
}
