// SPDX-License-Identifier: LGPL-2.1-or-later
// Real native winmm MCI dispatch into Wine mcicda, backed by silent track02.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mmsystem.h>
#include <mmddk.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int failures;
static unsigned successful_notifications,aborted_notifications;
static void check(bool ok,const char *name,MCIERROR error=0){
    printf("%s %s (mci=%lu)\n",ok?"PASS":"FAIL",name,error);
    if(!ok){++failures;if(error){wchar_t text[256]{};mciGetErrorStringW(error,text,256);wprintf(L"MCI error: %ls\n",text);}}
}
static std::wstring path_of(HMODULE module){
    if(!module)return {};
    std::vector<wchar_t> text(32768);DWORD count=GetModuleFileNameW(module,text.data(),(DWORD)text.size());
    return count&&count<text.size()?std::wstring(text.data(),count):std::wstring();
}
static bool module_is(const wchar_t *name,const wchar_t *expected){
    auto path=path_of(GetModuleHandleW(name));wprintf(L"%ls: %ls\n",name,path.c_str());
    return !_wcsicmp(path.c_str(),expected);
}
static LRESULT CALLBACK window_proc(HWND window,UINT message,WPARAM wparam,LPARAM lparam){
    if(message==MM_MCINOTIFY){
        if(wparam==MCI_NOTIFY_SUCCESSFUL)++successful_notifications;
        if(wparam==MCI_NOTIFY_ABORTED)++aborted_notifications;
        return 0;
    }
    return DefWindowProcW(window,message,wparam,lparam);
}
static void pump(DWORD milliseconds){
    ULONGLONG end=GetTickCount64()+milliseconds;
    do{MSG message;while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}Sleep(5);}while(GetTickCount64()<end);
}
static DWORD_PTR status(MCIDEVICEID device,DWORD item,DWORD track=0){
    MCI_STATUS_PARMS query{};query.dwItem=item;query.dwTrack=track;
    MCIERROR error=mciSendCommandW(device,MCI_STATUS,MCI_STATUS_ITEM|(track?MCI_TRACK:0),(DWORD_PTR)&query);
    check(!error,"MCI status query succeeds",error);return error?~(DWORD_PTR)0:query.dwReturn;
}
static MCIERROR command(MCIDEVICEID device,UINT message,DWORD_PTR flags,DWORD_PTR parameters,const char *name){
    MCIERROR error=mciSendCommandW(device,message,flags,parameters);check(!error,name,error);return error;
}
static MCIERROR string_command(const wchar_t *command_text,wchar_t *result=nullptr,UINT count=0){
    MCIERROR error=mciSendStringW(command_text,result,count,nullptr);
    wprintf(L"MCI string: %ls -> %ls\n",command_text,result?result:L"");
    check(!error,"MCI string command succeeds",error);return error;
}

int wmain(int argc,wchar_t **argv){
    setvbuf(stdout,nullptr,_IONBF,0);
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
    if(argc!=3){fputs("Usage: cdda-probe EXPECTED_MCICDA_PATH EXPECTED_DSOUND_PATH_OR_native\nRequires fixture data track01=150, silent audio track02=225, audio track03=150 sectors on D:.\n",stderr);return 2;}
    std::wstring expected_sound=argv[2];
    if(expected_sound==L"native"){wchar_t system[MAX_PATH]{};GetSystemDirectoryW(system,MAX_PATH);expected_sound=std::wstring(system)+L"\\dsound.dll";}
    WNDCLASSW cls{};cls.hInstance=GetModuleHandleW(nullptr);cls.lpfnWndProc=window_proc;cls.lpszClassName=L"WinedowsCddaProbe";
    RegisterClassW(&cls);HWND window=CreateWindowExW(0,cls.lpszClassName,L"Winedows silent CDDA test",0,0,0,0,0,HWND_MESSAGE,nullptr,cls.hInstance,nullptr);
    check(window!=nullptr,"create MCI notification window");if(!window)return 1;
    wprintf(L"MCI notification HWND: %p\n",window);
    BOOL notified=mciDriverNotify(window,0,MCI_NOTIFY_SUCCESSFUL);pump(30);
    check(notified&&successful_notifications==1,"native MCI notification reaches the full window handle");
    successful_notifications=aborted_notifications=0;

    MCI_OPEN_PARMSW open{};open.dwCallback=(DWORD_PTR)window;open.lpstrDeviceType=L"cdaudio";open.lpstrElementName=L"D:";open.lpstrAlias=L"wdcommand";
    MCIERROR error=command(0,MCI_OPEN,MCI_OPEN_TYPE|MCI_OPEN_ELEMENT|MCI_OPEN_ALIAS|MCI_OPEN_SHAREABLE|MCI_NOTIFY,(DWORD_PTR)&open,"open virtual CD audio through native MCI");
    if(error){DestroyWindow(window);return 1;}
    pump(30);check(successful_notifications==1,"open completion notification reaches the full window handle");
    successful_notifications=0;
    MCIDEVICEID device=open.wDeviceID;
    check(device&&mciGetDeviceIDW(L"wdcommand")==device,"MCI alias resolves the open device");
    check(module_is(L"mcicda.dll",argv[1]),"MCI dispatch uses the actual Wine CD audio driver");
    wchar_t product[256]{};MCI_INFO_PARMSW info{};info.lpstrReturn=product;info.dwRetSize=256;
    command(device,MCI_INFO,MCI_INFO_PRODUCT,(DWORD_PTR)&info,"query CD audio product");
    check(!wcscmp(product,L"Wine's audio CD"),"Wine CD audio semantics provide the product string");
    check(status(device,MCI_STATUS_NUMBER_OF_TRACKS)==3,"mixed-mode CD reports three tracks");
    check(status(device,MCI_CDA_STATUS_TYPE_TRACK,1)==MCI_CDA_TRACK_OTHER,"track01 remains a data track");
    check(status(device,MCI_CDA_STATUS_TYPE_TRACK,2)==MCI_CDA_TRACK_AUDIO,"track02 is audio");
    check(status(device,MCI_STATUS_MEDIA_PRESENT)!=0,"virtual CD media is present");
    MCI_SET_PARMS set{};set.dwTimeFormat=MCI_FORMAT_TMSF;
    command(device,MCI_SET,MCI_SET_TIME_FORMAT,(DWORD_PTR)&set,"select track-minute-second-frame format");
    check(status(device,MCI_STATUS_TIME_FORMAT)==MCI_FORMAT_TMSF,"TMSF format round trip");
    check(status(device,MCI_STATUS_LENGTH,2)==MCI_MAKE_MSF(0,3,0),"silent track02 has exactly 225 CD frames");
    check(status(device,MCI_STATUS_POSITION,2)==MCI_MAKE_TMSF(2,0,0,0),"track02 begins at its TMSF origin");
    MCI_STATUS_PARMS invalid{};invalid.dwItem=MCI_STATUS_LENGTH;invalid.dwTrack=4;
    error=mciSendCommandW(device,MCI_STATUS,MCI_STATUS_ITEM|MCI_TRACK,(DWORD_PTR)&invalid);
    check(error==MCIERR_OUTOFRANGE,"invalid track reports an MCI range error",error);

    MCI_SEEK_PARMS seek{};seek.dwTo=MCI_MAKE_TMSF(2,0,0,0);
    command(device,MCI_SEEK,MCI_TO|MCI_WAIT,(DWORD_PTR)&seek,"seek to silent track02");
    check(status(device,MCI_STATUS_CURRENT_TRACK)==2,"seek updates the virtual subchannel track");
    MCI_PLAY_PARMS play{};play.dwCallback=(DWORD_PTR)window;play.dwFrom=MCI_MAKE_TMSF(2,0,0,0);play.dwTo=MCI_MAKE_TMSF(2,0,1,0);
    if(!command(device,MCI_PLAY,MCI_FROM|MCI_TO|MCI_NOTIFY,(DWORD_PTR)&play,"start a silent one-second CDDA segment")){
        check(module_is(L"dsound.dll",expected_sound.c_str()),"CD audio uses the selected DirectSound provider");
        pump(100);check(status(device,MCI_STATUS_MODE)==MCI_MODE_PLAY,"CDDA digital playback reports play mode");
        MCI_GENERIC_PARMS generic{};generic.dwCallback=(DWORD_PTR)window;
        command(device,MCI_PAUSE,0,(DWORD_PTR)&generic,"pause CDDA digital playback");pump(50);
        check(status(device,MCI_STATUS_MODE)==MCI_MODE_PAUSE,"paused CDDA reports pause mode");
        check(aborted_notifications>0,"pause aborts the pending play notification");
        command(device,MCI_RESUME,0,(DWORD_PTR)&generic,"resume CDDA digital playback");pump(50);
        check(status(device,MCI_STATUS_MODE)==MCI_MODE_PLAY,"resumed CDDA reports play mode");
        command(device,MCI_STOP,MCI_NOTIFY,(DWORD_PTR)&generic,"stop CDDA digital playback");pump(50);
        check(status(device,MCI_STATUS_MODE)==MCI_MODE_STOP,"stopped CDDA reports stop mode");
        check(successful_notifications>0,"stop completion notification arrives");
    }
    // Reaching STOP without an explicit stop exercises the raw-sector refill
    // loop and its real DirectSound cursor, rather than only MCI state changes.
    play.dwCallback=0;
    if(!command(device,MCI_PLAY,MCI_FROM|MCI_TO,(DWORD_PTR)&play,"play silent segment through natural completion")){
        ULONGLONG deadline=GetTickCount64()+3500;DWORD_PTR mode=MCI_MODE_PLAY;
        do{pump(50);MCI_STATUS_PARMS query{};query.dwItem=MCI_STATUS_MODE;error=mciSendCommandW(device,MCI_STATUS,MCI_STATUS_ITEM,(DWORD_PTR)&query);mode=query.dwReturn;}while(!error&&mode==MCI_MODE_PLAY&&GetTickCount64()<deadline);
        check(!error&&mode==MCI_MODE_STOP,"CDDA raw-sector playback reaches natural completion",error);
    }
    command(device,MCI_CLOSE,0,0,"close command-opened CD audio device");
    check(!mciGetDeviceIDW(L"wdcommand"),"close removes the MCI device alias");

    wchar_t result[256]{};
    if(!string_command(L"open D: type cdaudio alias wdstring shareable")){
        string_command(L"info wdstring product",result,256);check(!wcscmp(result,L"Wine's audio CD"),"string MCI dispatch reaches Wine driver");
        string_command(L"set wdstring time format tmsf");
        string_command(L"status wdstring number of tracks",result,256);check(!wcscmp(result,L"3"),"string MCI reports all tracks");
        // Upstream Wine's documented FIXME returns numeric track types to the
        // string API. Record that limitation rather than claim textual parity.
        string_command(L"status wdstring type track 2",result,256);check(!wcscmp(result,L"1088"),"inherited Wine string track type is numeric audio=1088");
        string_command(L"status wdstring type track 1",result,256);check(!wcscmp(result,L"1089"),"inherited Wine string track type is numeric other=1089");
        string_command(L"status wdstring length track 2",result,256);check(!wcscmp(result,L"00:03:00"),"string MCI formats track duration");
        string_command(L"seek wdstring to 2:0:0:0");
        string_command(L"play wdstring from 2:0:0:0 to 2:0:1:0");pump(100);
        string_command(L"pause wdstring");
        string_command(L"status wdstring mode",result,256);check(!wcscmp(result,L"paused"),"string MCI pause mode is formatted");
        string_command(L"resume wdstring");string_command(L"stop wdstring");string_command(L"close wdstring");
    }
    // Older games commonly use the ANSI APIs and the numeric device type.
    MCI_OPEN_PARMSA ansi_open{};ansi_open.lpstrDeviceType=(LPCSTR)(ULONG_PTR)MCI_DEVTYPE_CD_AUDIO;ansi_open.lpstrElementName="D:";
    error=mciSendCommandA(0,MCI_OPEN,MCI_OPEN_TYPE|MCI_OPEN_TYPE_ID|MCI_OPEN_ELEMENT,(DWORD_PTR)&ansi_open);
    check(!error,"ANSI MCI opens the numeric CD audio device type",error);
    if(!error){
        char ansi_product[256]{};MCI_INFO_PARMSA ansi_info{};ansi_info.lpstrReturn=ansi_product;ansi_info.dwRetSize=256;
        error=mciSendCommandA(ansi_open.wDeviceID,MCI_INFO,MCI_INFO_PRODUCT,(DWORD_PTR)&ansi_info);
        check(!error&&!strcmp(ansi_product,"Wine's audio CD"),"ANSI MCI product mapping reaches Wine",error);
        error=mciSendCommandA(ansi_open.wDeviceID,MCI_SET,MCI_SET_TIME_FORMAT,(DWORD_PTR)&set);check(!error,"ANSI MCI selects TMSF",error);
        error=mciSendCommandA(ansi_open.wDeviceID,MCI_PLAY,MCI_FROM|MCI_TO,(DWORD_PTR)&play);check(!error,"ANSI MCI plays the silent track segment",error);pump(100);
        error=mciSendCommandA(ansi_open.wDeviceID,MCI_STOP,0,0);check(!error,"ANSI MCI stops digital playback",error);
        error=mciSendCommandA(ansi_open.wDeviceID,MCI_CLOSE,0,0);check(!error,"ANSI MCI closes CD audio",error);
    }
    error=mciSendStringA("open D: type cdaudio alias wdascii",nullptr,0,nullptr);check(!error,"ANSI string parser opens CD audio",error);
    if(!error){
        char ansi_result[256]{};error=mciSendStringA("status wdascii type track 2",ansi_result,256,nullptr);
        check(!error&&!strcmp(ansi_result,"1088"),"ANSI string API preserves Wine's numeric audio track type",error);
        error=mciSendStringA("close all",nullptr,0,nullptr);check(!error,"MCI closes all fixture devices",error);
        check(!mciGetDeviceIDA("wdascii"),"ANSI alias is removed after close all");
    }
    DestroyWindow(window);
    puts("LIMITATION Wine returns track-type strings as 1088/1089 rather than localized audio/other; subchannel position is a reader/seek cursor, not a playback clock.");
    printf("Wine CDDA %zu-bit: %d failure(s); only silent fixture track02 was played; no capture\n",sizeof(void*)*8,failures);
    return failures?1:0;
}
