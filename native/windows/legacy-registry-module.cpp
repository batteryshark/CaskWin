// SPDX-License-Identifier: LGPL-2.1-or-later
#include <windows.h>
extern "C" __declspec(dllexport) LSTATUS __cdecl LegacySetValue(HKEY key,LPCSTR name,DWORD type,const BYTE *data,DWORD count) {
    return RegSetValueExA(key,name,0,type,data,count);
}
