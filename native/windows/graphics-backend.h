// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once
#include "common.h"
#include <d3d11.h>
namespace wd {
// Classify genuine Windows graphics code without changing the caller's error.
bool system_graphics_caller(const void *caller) noexcept;
// Returns FALSE for app/Wine callers. TRUE means result is the host factory's
// HRESULT (including native failure); the caller must not retry through Wine.
BOOL create_system_dxgi_factory(const void *caller,UINT entrypoint,UINT flags,
                               REFIID iid,void **factory,HRESULT *result);
BOOL create_system_d3d11_device(const void *caller,UINT entrypoint,IDXGIAdapter *adapter,
                               D3D_DRIVER_TYPE driver_type,HMODULE software,UINT flags,
                               const D3D_FEATURE_LEVEL *levels,UINT level_count,UINT sdk_version,
                               const DXGI_SWAP_CHAIN_DESC *description,IDXGISwapChain **swapchain,
                               ID3D11Device **device,D3D_FEATURE_LEVEL *feature_level,
                               ID3D11DeviceContext **context,HRESULT *result);
}
