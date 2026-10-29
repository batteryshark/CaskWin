// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once
#include <windows.h>
namespace wd {
// Install absent standard stream interface mappings in an already-open private
// hive. Call from Prefix::init, under its session lock, after importing user data.
// Existing registration keys (including deliberately empty keys) are untouched.
void initialize_standard_com(HKEY private_hive);
}
