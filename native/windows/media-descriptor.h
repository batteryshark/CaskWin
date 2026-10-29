// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once
#include "common.h"
namespace wd {
// Live replacement is deliberately limited to immutable private ISO/CUE media.
// Preparation may leave an unpublished cache after failure; it never edits INI.
Drive prepare_live_medium(const Prefix &prefix,const str &source,const str &label=L"");
void validate_live_medium(const Prefix &prefix,const Drive &medium);
}
