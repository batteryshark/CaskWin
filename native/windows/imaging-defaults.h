// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once
#include <windows.h>
namespace wd {
// Call from Prefix::init while holding its session lock, after importing user
// data. Only absent standard WIC class keys are added to this private hive.
void initialize_imaging_com(HKEY private_hive);
}
