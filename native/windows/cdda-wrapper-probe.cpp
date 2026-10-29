// SPDX-License-Identifier: LGPL-2.1-or-later
// Diagnostic for app-local OGG-WinMM replacements. Observes output API metadata
// only: it never reads/copies audio buffers, records, or accesses capture devices.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mmsystem.h>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <atomic>

static std::atomic<HWAVEOUT> output;
static std::atomic<unsigned> writes,bytes,opens;
static decltype(&waveOutOpen) original_open;
static decltype(&waveOutWrite) original_write;
static decltype(&waveOutClose) original_close;
static decltype(&waveOutGetPosition) native_position;
static unsigned failures;
static void check(bool ok,const char *what){printf("%s %s\n",ok?"PASS":"FAIL",what);if(!ok)++failures;}
static MMRESULT WINAPI observe_open(LPHWAVEOUT out,UINT id,LPCWAVEFORMATEX format,DWORD_PTR callback,DWORD_PTR instance,DWORD flags){
    MMRESULT result=original_open(out,id,format,callback,instance,flags);
    if(result==MMSYSERR_NOERROR&&!(flags&WAVE_FORMAT_QUERY)&&out){output=*out;++opens;printf("waveOutOpen: channels=%u sample_rate=%lu bits=%u flags=%08lx\n",format->nChannels,format->nSamplesPerSec,format->wBitsPerSample,flags);}
    return result;
}
static MMRESULT WINAPI observe_write(HWAVEOUT out,LPWAVEHDR header,UINT size){
    MMRESULT result=original_write(out,header,size);
    if(result==MMSYSERR_NOERROR){++writes;bytes+=header->dwBufferLength;}
    return result;
}
static MMRESULT WINAPI observe_close(HWAVEOUT out){
    if(output==out)output=nullptr;
    return original_close(out);
}
static bool replace_import(HMODULE module,const char *function,FARPROC replacement,FARPROC *original){
    BYTE *base=(BYTE*)module;auto dos=(IMAGE_DOS_HEADER*)base;auto nt=(IMAGE_NT_HEADERS*)(base+dos->e_lfanew);
    auto rva=nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;if(!rva)return false;
    for(auto descriptor=(IMAGE_IMPORT_DESCRIPTOR*)(base+rva);descriptor->Name;descriptor++){
        if(_stricmp((char*)base+descriptor->Name,"winmm.dll")||!descriptor->OriginalFirstThunk)continue;
        auto names=(IMAGE_THUNK_DATA*)(base+descriptor->OriginalFirstThunk);
        for(auto slot=(IMAGE_THUNK_DATA*)(base+descriptor->FirstThunk);slot->u1.Function;slot++,names++){
            if(IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal))continue;
            auto symbol=(IMAGE_IMPORT_BY_NAME*)(base+names->u1.AddressOfData);
            if(strcmp((char*)symbol->Name,function))continue;
            *original=(FARPROC)slot->u1.Function;
            HMODULE owner=nullptr;wchar_t path[32768]{};
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,(LPCWSTR)*original,&owner);
            if(owner)GetModuleFileNameW(owner,path,32768);
            printf("Observed import %s: target=%p owner=%ls\n",function,*original,path);
            DWORD protection=0;if(!VirtualProtect(&slot->u1.Function,sizeof(ULONG_PTR),PAGE_READWRITE,&protection))return false;
            InterlockedExchangePointer((void*volatile*)&slot->u1.Function,(void*)replacement);
            DWORD ignored;VirtualProtect(&slot->u1.Function,sizeof(ULONG_PTR),protection,&ignored);return true;
        }
    }
    return false;
}
static DWORD frame_position(){
    HWAVEOUT handle=output.load();if(!handle)return 0;
    MMTIME time{};time.wType=TIME_SAMPLES;
    MMRESULT result=native_position(handle,&time,sizeof(time));
    printf("Output position: result=%u type=%u samples=%lu writes=%u bytes=%u\n",result,time.wType,time.u.sample,writes.load(),bytes.load());
    return result==MMSYSERR_NOERROR&&time.wType==TIME_SAMPLES?time.u.sample:0;
}
static DWORD WINAPI timeout(void*){Sleep(20000);fprintf(stderr,"FAIL CDDA wrapper probe timed out\n");TerminateProcess(GetCurrentProcess(),124);return 0;}
int wmain(){
    setvbuf(stdout,nullptr,_IONBF,0);CloseHandle(CreateThread(nullptr,0,timeout,nullptr,0,nullptr));
    wchar_t self[32768];GetModuleFileNameW(nullptr,self,32768);std::wstring app=self;app.resize(app.find_last_of(L"\\/"));
    std::wstring path=app+L"\\winmm.dll";HMODULE wrapper=LoadLibraryW(path.c_str());check(wrapper!=nullptr,"load private app-local WinMM wrapper");if(!wrapper)return 1;
    wchar_t actual[32768];GetModuleFileNameW(wrapper,actual,32768);printf("Wrapper module: %ls\n",actual);check(!_wcsicmp(path.c_str(),actual),"loaded wrapper path matches staged file");
    wchar_t system[32768];GetSystemDirectoryW(system,32768);std::wstring native_path=std::wstring(system)+L"\\winmm.dll";
    HMODULE native=LoadLibraryW(native_path.c_str());check(native&&native!=wrapper,"native output backend is a distinct module");if(!native)return 1;
    original_open=(decltype(original_open))GetProcAddress(native,"waveOutOpen");original_write=(decltype(original_write))GetProcAddress(native,"waveOutWrite");
    original_close=(decltype(original_close))GetProcAddress(native,"waveOutClose");native_position=(decltype(native_position))GetProcAddress(native,"waveOutGetPosition");
    check(replace_import(wrapper,"waveOutOpen",(FARPROC)observe_open,(FARPROC*)&original_open),"observe wrapper output-open metadata");
    check(replace_import(wrapper,"waveOutWrite",(FARPROC)observe_write,(FARPROC*)&original_write),"observe wrapper output-write metadata");
    check(replace_import(wrapper,"waveOutClose",(FARPROC)observe_close,(FARPROC*)&original_close),"observe wrapper output-close metadata");
    auto command=(decltype(&mciSendCommandA))GetProcAddress(wrapper,"mciSendCommandA");
    auto string=(decltype(&mciSendStringA))GetProcAddress(wrapper,"mciSendStringA");
    check(command&&string,"wrapper exports MCI command/string interfaces");if(!command||!string)return 1;
    MCI_OPEN_PARMSA open{};open.lpstrDeviceType="cdaudio";
    MCIERROR error=command(0,MCI_OPEN,MCI_OPEN_TYPE,(DWORD_PTR)&open);printf("MCI open: error=%lu device=%u\n",error,open.wDeviceID);check(!error,"open virtual CD audio device");if(error)return 1;
    MCI_STATUS_PARMS status{};status.dwItem=MCI_STATUS_NUMBER_OF_TRACKS;
    error=command(open.wDeviceID,MCI_STATUS,MCI_STATUS_ITEM,(DWORD_PTR)&status);printf("CD tracks=%lu error=%lu\n",(DWORD)status.dwReturn,error);check(!error&&status.dwReturn==25,"24 OGG tracks plus data track 1 reported");
    MCI_SET_PARMS format{};format.dwTimeFormat=MCI_FORMAT_TMSF;
    error=command(open.wDeviceID,MCI_SET,MCI_SET_TIME_FORMAT,(DWORD_PTR)&format);check(!error,"select track-minute-second-frame time format");
    status={};status.dwItem=MCI_STATUS_LENGTH;status.dwTrack=2;
    error=command(open.wDeviceID,MCI_STATUS,MCI_STATUS_ITEM|MCI_TRACK,(DWORD_PTR)&status);printf("Track02 length=%08lx error=%lu\n",(DWORD)status.dwReturn,error);check(!error&&status.dwReturn,"decoded file track has nonzero length");
    MCI_PLAY_PARMS play{};play.dwFrom=MCI_MAKE_TMSF(2,0,0,0);play.dwTo=MCI_MAKE_TMSF(3,0,0,0);
    error=command(open.wDeviceID,MCI_PLAY,MCI_FROM|MCI_TO,(DWORD_PTR)&play);check(!error,"play decoded Track02 file");
    Sleep(150);DWORD first=frame_position();status={};status.dwItem=MCI_STATUS_POSITION;
    error=command(open.wDeviceID,MCI_STATUS,MCI_STATUS_ITEM,(DWORD_PTR)&status);DWORD mci_first=(DWORD)status.dwReturn;
    Sleep(1200);DWORD last=frame_position();
    error=command(open.wDeviceID,MCI_STATUS,MCI_STATUS_ITEM,(DWORD_PTR)&status);DWORD mci_last=(DWORD)status.dwReturn;
    printf("MCI position: %08lx -> %08lx error=%lu\n",mci_first,mci_last,error);
    check(!error&&mci_last!=mci_first,"MCI playback position progresses");
    check(opens&&writes&&bytes&&last>first,"decoded music reaches native output with advancing sample position");
    check(!command(open.wDeviceID,MCI_STOP,0,0),"stop wrapper playback");
    check(!command(open.wDeviceID,MCI_CLOSE,0,0),"close wrapper device");
    // These command forms are present literally in Nightmare Creatures NC.exe.
    char response[128]{};error=string("open cdaudio wait",nullptr,0,nullptr);check(!error,"game MCI string open/wait");
    check(!string("set cdaudio time format milliseconds",nullptr,0,nullptr),"game MCI string millisecond time format");
    error=string("status cdaudio number of tracks",response,sizeof(response),nullptr);printf("String tracks=%s error=%lu\n",response,error);check(!error&&!strcmp(response,"25"),"MCI string interface reports tracks");
    error=string("status cdaudio position track 2",response,sizeof(response),nullptr);
    unsigned long start=strtoul(response,nullptr,10);printf("String Track02 start=%s error=%lu\n",response,error);check(!error,"game queries track start position");
    error=string("status cdaudio length track 2",response,sizeof(response),nullptr);
    unsigned long length=strtoul(response,nullptr,10);printf("String Track02 length=%s error=%lu\n",response,error);check(!error&&length>2000,"game queries decoded track duration");
    char play_string[128];snprintf(play_string,sizeof(play_string),"play cdaudio from %lu to %lu ",start,start+length);
    error=string(play_string,nullptr,0,nullptr);printf("String play=%s error=%lu\n",play_string,error);check(!error,"game string starts file-backed music");
    Sleep(150);DWORD string_first=frame_position();
    error=string("status cdaudio mode",response,sizeof(response),nullptr);printf("String mode=%s error=%lu\n",response,error);check(!error&&!strcmp(response,"playing"),"game sees playing CD mode");
    Sleep(1200);DWORD string_last=frame_position();
    check(string_last>string_first,"game string playback advances native output sample position");
    check(!string("stop cdaudio",nullptr,0,nullptr),"game string stops file-backed playback");
    check(!string("close cdaudio",nullptr,0,nullptr),"MCI string close");
    printf("CDDA wrapper failures: %u\n",failures);return failures?1:0;
}
