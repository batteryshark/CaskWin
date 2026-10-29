// SPDX-License-Identifier: LGPL-2.1-or-later
#define wmain native_startup_main
#define WINEDOWS_STARTUP_IAT_IDENTITY
#include "native-startup-probe.cpp"
#undef wmain
int wmain(int argc,wchar_t **argv){
    wchar_t expected[32767]{},actual[32767]{};
    DWORD size=GetEnvironmentVariableW(L"WD_STARTUP_EXPECTED_DLL_DIRECTORY",expected,32767);bool expect_empty=GetEnvironmentVariableW(L"WD_STARTUP_EXPECT_EMPTY_DLL_DIRECTORY",actual,32767)!=0;actual[0]=0;
    if((!size&&!expect_empty)||size>=32767||GetDllDirectoryW(32767,actual)!=size||wcscmp(actual,expected)){
        fwprintf(stderr,L"FAIL inherited DLL directory preserved: [%ls] expected [%ls]\n",actual,expected);return 1;
    }
    puts("PASS inherited custom DLL directory remains exact after initial loading");
    return native_startup_main(argc,argv);
}
