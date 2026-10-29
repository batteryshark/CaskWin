// SPDX-License-Identifier: LGPL-2.1-or-later
// Native WinMM queue-depth control for the copied SDL/FAudio timing fixture.
// Synthesizes zeros; reads only completion/position metadata, never captures.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mmsystem.h>
#include <mmreg.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <stdexcept>
#include <vector>

struct Event {long long tick;int pending;};
struct State {
    HANDLE event=nullptr;
    std::atomic<int> pending{0};
    std::atomic<unsigned> used{0},dropped{0};
    std::atomic<long long> begin{0},end{0};
    Event rows[32768]{};
};
static State &state=*new State;
static long long now(){LARGE_INTEGER t;QueryPerformanceCounter(&t);return t.QuadPart;}
static void check(bool value,const char *message){if(!value)throw std::runtime_error(message);}
static void mm(MMRESULT result,const char *message){check(result==MMSYSERR_NOERROR,message);}
static void CALLBACK completed(HWAVEOUT,UINT message,DWORD_PTR,DWORD_PTR,DWORD_PTR){
    if(message!=WOM_DONE)return;
    long long tick=now();int pending=state.pending.fetch_sub(1)-1;
    if(tick>=state.begin.load()&&state.begin&&tick<state.end.load()){
        unsigned i=state.used.fetch_add(1);
        if(i<32768)state.rows[i]={tick,pending};else ++state.dropped;
    }
    SetEvent(state.event);
}
static DWORD position(HWAVEOUT device){MMTIME t{};t.wType=TIME_SAMPLES;mm(waveOutGetPosition(device,&t,sizeof(t)),"waveOutGetPosition failed");check(t.wType==TIME_SAMPLES,"Sample position unavailable");return t.u.sample;}
static int run(int argc,wchar_t **argv){
    check(argc==5,"usage: BUFFERS(2/4/8) RATE(44100/48000) SECONDS OUTPUT_JSON");
    unsigned buffers=_wtoi(argv[1]),rate=_wtoi(argv[2]),seconds=_wtoi(argv[3]);
    check((buffers==2||buffers==4||buffers==8)&&(rate==44100||rate==48000)&&seconds>=5&&seconds<=30,"Unsupported test parameters");
    check(!GetModuleHandleW(L"SDL2.dll")&&!GetModuleHandleW(L"FAudio.dll")&&!GetModuleHandleW(L"winedows64.dll"),"Native control loaded unexpected audio or shim module");
    WAVEFORMATEX format{};format.wFormatTag=WAVE_FORMAT_IEEE_FLOAT;format.nChannels=2;format.nSamplesPerSec=rate;
    format.wBitsPerSample=32;format.nBlockAlign=8;format.nAvgBytesPerSec=rate*8;
    state.event=CreateEventW(nullptr,FALSE,FALSE,nullptr);check(state.event!=nullptr,"CreateEvent failed");
    HWAVEOUT device=nullptr;mm(waveOutOpen(&device,WAVE_MAPPER,&format,(DWORD_PTR)completed,0,CALLBACK_FUNCTION),"No native WinMM endpoint");
    mm(waveOutPause(device),"Initial pause failed");
    constexpr unsigned frames=512;
    std::vector<WAVEHDR> headers(buffers);std::vector<std::vector<float>> silence(buffers,std::vector<float>(frames*2,0));
    for(unsigned i=0;i<buffers;i++){
        auto &h=headers[i];h.lpData=(LPSTR)silence[i].data();h.dwBufferLength=frames*8;
        mm(waveOutPrepareHeader(device,&h,sizeof(h)),"Prepare failed");
        ++state.pending;mm(waveOutWrite(device,&h,sizeof(h)),"Initial write failed");
    }
    LARGE_INTEGER frequency;QueryPerformanceFrequency(&frequency);auto begin=now()+2*frequency.QuadPart,end=begin+seconds*frequency.QuadPart;
    state.begin=begin;state.end=end;mm(waveOutRestart(device),"Restart failed");
    DWORD first=0,last=0;bool started=false;long long first_at=0,last_at=0;unsigned writes=0;
    while(now()<end){
        if(!started&&now()>=begin){first=position(device);first_at=now();started=true;}
        for(auto &h:headers)if(h.dwFlags&WHDR_DONE){++state.pending;mm(waveOutWrite(device,&h,sizeof(h)),"Refill failed");if(now()>=begin)++writes;}
        WaitForSingleObject(state.event,2);
    }
    last=position(device);last_at=now();state.end=1;
    mm(waveOutReset(device),"Reset failed");
    for(auto &h:headers)mm(waveOutUnprepareHeader(device,&h,sizeof(h)),"Unprepare failed");
    mm(waveOutClose(device),"Close failed");CloseHandle(state.event);
    unsigned count=std::min(state.used.load(),32768u),zero=0;double maximum=0;
    for(unsigned i=0;i<count;i++){if(!state.rows[i].pending)++zero;if(i)maximum=std::max(maximum,1000.0*(state.rows[i].tick-state.rows[i-1].tick)/frequency.QuadPart);}
    double measured=(double)(last_at-first_at)/frequency.QuadPart,ratio=(last-first)/(rate*measured);
    FILE *file=_wfopen(argv[4],L"wb");check(file!=nullptr,"Cannot create report");
    fprintf(file,"{\"buffers\":%u,\"framesPerBuffer\":%u,\"rate\":%u,\"measuredSeconds\":%.6f,\"positionSamplesAdvanced\":%lu,\"positionRateRatio\":%.6f,\"writes\":%u,\"completions\":%u,\"zeroOutstandingNotifications\":%u,\"maxCompletionGapMs\":%.6f,\"droppedEvents\":%u,\"audioOutput\":\"synthesized zeros\",\"audioCapturePerformed\":false,\"sdlLoaded\":false,\"wineLoaded\":false,\"hardwareUnderruns\":null}\n",buffers,frames,rate,measured,last-first,ratio,writes,count,zero,maximum,state.dropped.load());
    fclose(file);printf("Native waveOut buffers=%u rate=%u progress=%.4f maxGap=%.3fms zeroOutstanding=%u\n",buffers,rate,ratio,maximum,zero);
    return state.dropped?1:0;
}
int wmain(int argc,wchar_t **argv){try{return run(argc,argv);}catch(const std::exception &e){fprintf(stderr,"WaveOut control unavailable: %s\n",e.what());TerminateProcess(GetCurrentProcess(),2);return 2;}}
