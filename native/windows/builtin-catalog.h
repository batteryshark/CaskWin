// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once
namespace wd {
struct BuiltinDefinition {const wchar_t *name;const wchar_t *dependencies[2];};
// Dependencies are Wine implementation details. Public interfaces passed into a
// helper (for example native IDirect3DDevice9 into Wine D3DX) retain their provider.
inline constexpr BuiltinDefinition portable_builtins[]={
    {L"wined3d",{}},
    {L"winevulkan",{}}, {L"vulkan-1",{L"winevulkan"}},
    {L"ddraw",{L"wined3d"}}, {L"d3d8",{L"wined3d"}}, {L"d3d9",{L"wined3d"}},
    {L"dxgi",{L"wined3d"}}, {L"d3d11",{L"dxgi"}},
    {L"d3d10core",{L"d3d11"}}, {L"d3d10_1",{L"d3d10core"}},
    {L"d3d10",{L"d3d10core",L"d3dcompiler_39"}},
    {L"d3dcompiler_39",{L"wined3d"}},
    {L"d3dcompiler_42",{L"wined3d"}},
    {L"d3dcompiler_43",{L"wined3d"}}, {L"d3dxof",{}},
    {L"d3dcompiler_47",{L"wined3d"}},
    {L"d3dx9_36",{L"d3dcompiler_47",L"d3dxof"}},
    {L"d3dx9_37",{L"d3dcompiler_47",L"d3dxof"}},
    {L"d3dx9_41",{L"d3dcompiler_47",L"d3dxof"}},
    {L"d3dx9_42",{L"d3dcompiler_42",L"d3dxof"}},
    {L"d3dx9_43",{L"d3dcompiler_43",L"d3dxof"}},
    {L"d3dx11_43",{L"d3dcompiler_47"}},
    {L"d3dx10_43",{L"d3dcompiler_47",L"d3dx9_36"}},
    {L"gdiplus",{}}, {L"glu32",{}},
    {L"dinput",{}}, {L"dinput8",{}}, {L"dsound",{}}, {L"mcicda",{}},
    {L"xinput1_1",{}}, {L"xinput1_2",{}}, {L"xinput1_3",{}},
    {L"xinput1_4",{}}, {L"xinput9_1_0",{L"xinput1_4"}},
    {L"xaudio2_7",{}}, {L"xaudio2_8",{}}, {L"xaudio2_9",{}},
    {L"x3daudio1_7",{}}, {L"xapofx1_5",{}},
    {L"xactengine3_7",{}},
    // Public MF interfaces bridge to native platform, source and codec modules.
    {L"mfreadwrite",{}}
};
}
