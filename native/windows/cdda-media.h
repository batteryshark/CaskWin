// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once
#include "common.h"

namespace wd {
struct CddaInfo {
    str directory;
    DWORD tracks=0, sectors=0;
    bool synthetic_data=false;
};
// Prepared CDDA manifests and their sector-aligned PCM must live in the prefix's
// private .winedows/media tree. ISO data sizes are checked against the drive.
CddaInfo validate_cdda(const Prefix &prefix,int drive,const str &manifest);
// Called under the shim's recursion guard. True means this is a CDDA request
// belonging to this virtual medium; result carries normal DeviceIoControl BOOL.
bool cdda_ioctl(const Prefix &prefix,int drive,DWORD code,void *input,DWORD input_size,
                void *output,DWORD output_size,DWORD *done,OVERLAPPED *overlapped,BOOL &result,DWORD generation=0);
}
