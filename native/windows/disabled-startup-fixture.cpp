// SPDX-License-Identifier: LGPL-2.1-or-later
#include <windows.h>
#ifdef DISABLED_LEAF
extern "C" int ProbeValue(){return 17;}
#elif defined(DISABLED_BRIDGE)
extern "C" __declspec(dllimport) int ProbeValue();
extern "C" int BridgeValue(){return ProbeValue();}
#elif defined(DISABLED_FORWARD)
// Export forwarding is supplied by the fixture .def file.
#else
#ifdef DISABLED_EMPTY_CHILD
int main(){return 0;}
#elif defined(DISABLED_BRIDGE_CHILD)
extern "C" __declspec(dllimport) int BridgeValue();
int main(){return BridgeValue()==17?0:1;}
#else
extern "C" __declspec(dllimport) int ProbeValue();
int main(){return ProbeValue()==17?0:1;}
#endif
#endif
#if defined(DISABLED_LEAF)||defined(DISABLED_BRIDGE)||defined(DISABLED_FORWARD)
BOOL WINAPI DllMain(HINSTANCE,DWORD,LPVOID){return TRUE;}
#endif
