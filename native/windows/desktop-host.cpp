// SPDX-License-Identifier: LGPL-2.1-or-later
#include "desktop.h"
#include <memory>
namespace wd {
// A cross-process SetParent between incompatible DPI modes resets the game's
// process awareness. A PMv2 parent with mixed hosting accepts both legacy and
// per-monitor children without changing their contexts. Set only the host
// window's creation thread; the launcher process keeps its original DPI mode.
class MixedDpiParent {
    using SetContext=HANDLE (WINAPI *)(HANDLE);
    using SetHosting=int (WINAPI *)(int);
    SetContext set_context=nullptr;
    SetHosting set_hosting=nullptr;
    HANDLE previous_context=nullptr;
    int previous_hosting=-1;
public:
    MixedDpiParent(){
        HMODULE user=GetModuleHandleW(L"user32.dll");
        set_context=reinterpret_cast<SetContext>(GetProcAddress(user,"SetThreadDpiAwarenessContext"));
        set_hosting=reinterpret_cast<SetHosting>(GetProcAddress(user,"SetThreadDpiHostingBehavior"));
        // Do not enable PMv2 alone on systems without mixed hosting: that
        // would reset unaware games in the opposite direction.
        if(!set_context||!set_hosting)return;
        previous_context=set_context(reinterpret_cast<HANDLE>(INT_PTR(-4)));
        if(!previous_context)return;
        previous_hosting=set_hosting(1); // DPI_HOSTING_BEHAVIOR_MIXED
        if(previous_hosting==-1){set_context(previous_context);previous_context=nullptr;}
    }
    ~MixedDpiParent(){
        DWORD error=GetLastError();
        if(previous_hosting!=-1)set_hosting(previous_hosting);
        if(previous_context)set_context(previous_context);
        SetLastError(error);
    }
    MixedDpiParent(const MixedDpiParent&)=delete;
    MixedDpiParent& operator=(const MixedDpiParent&)=delete;
};
struct DesktopHost::State {
    HANDLE ready=nullptr,thread=nullptr,job=nullptr;
    HWND window=nullptr;DWORD error=0;str name;unsigned width=0,height=0;
    ~State(){
        if(IsWindow(window))PostMessageW(window,WD_DESKTOP_STOP,0,0);
        if(thread){WaitForSingleObject(thread,INFINITE);CloseHandle(thread);}
        if(ready)CloseHandle(ready);
    }
    static LRESULT CALLBACK procedure(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp) {
        auto self=(State*)GetWindowLongPtrW(hwnd,GWLP_USERDATA);
        if(msg==WM_NCCREATE){self=(State*)((CREATESTRUCTW*)lp)->lpCreateParams;SetWindowLongPtrW(hwnd,GWLP_USERDATA,(LONG_PTR)self);}
        if(msg==WM_SETFOCUS||(msg==WM_ACTIVATE&&LOWORD(wp)!=WA_INACTIVE)){
            for(HWND child=GetWindow(hwnd,GW_CHILD);child;child=GetWindow(child,GW_HWNDNEXT)){
                RECT area{};GetClientRect(child,&area);
                if(IsWindowVisible(child)&&IsWindowEnabled(child)&&area.right>1&&area.bottom>1){SetFocus(child);break;}
            }
        }
        if(msg==WM_ACTIVATE||msg==WM_ACTIVATEAPP){
            for(HWND child=GetWindow(hwnd,GW_CHILD);child;child=GetWindow(child,GW_HWNDNEXT))
                // Do not synthesize host activation for native-style roots.
                // A later host inactive message can overwrite the root's
                // active state while it actually owns keyboard focus.
                if(IsWindowVisible(child)&&GetPropW(child,WD_DESKTOP_NATIVE_MENU_ROOT)!=hwnd)PostMessageW(child,msg,wp,lp);
        }
        if(msg==WD_DESKTOP_MODE){
            unsigned width=LOWORD(lp),height=HIWORD(lp),depth=(unsigned)wp;
            if(width<160||height<100||width>8192||height>8192||(depth!=16&&depth!=32))return 0;
            SetPropW(hwnd,WD_DESKTOP_WIDTH,(HANDLE)(ULONG_PTR)width);SetPropW(hwnd,WD_DESKTOP_HEIGHT,(HANDLE)(ULONG_PTR)height);SetPropW(hwnd,WD_DESKTOP_DEPTH,(HANDLE)(ULONG_PTR)depth);
            RECT rect{0,0,(LONG)width,(LONG)height};AdjustWindowRectEx(&rect,WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX,FALSE,0);
            SetWindowPos(hwnd,nullptr,0,0,rect.right-rect.left,rect.bottom-rect.top,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
            EnumChildWindows(hwnd,[](HWND child,LPARAM parent)->BOOL{
                if(GetAncestor(child,GA_PARENT)==(HWND)parent){unsigned w=(unsigned)(ULONG_PTR)GetPropW((HWND)parent,WD_DESKTOP_WIDTH),h=(unsigned)(ULONG_PTR)GetPropW((HWND)parent,WD_DESKTOP_HEIGHT),d=(unsigned)(ULONG_PTR)GetPropW((HWND)parent,WD_DESKTOP_DEPTH);PostMessageW(child,WM_DISPLAYCHANGE,d,MAKELPARAM(w,h));}return TRUE;
            },(LPARAM)hwnd);
            InvalidateRect(hwnd,nullptr,TRUE);return 1;
        }
        if(msg==WM_CLOSE){if(self&&self->job)TerminateJobObject(self->job,ERROR_CANCELLED);DestroyWindow(hwnd);return 0;}
        if(msg==WD_DESKTOP_STOP){DestroyWindow(hwnd);return 0;}
        if(msg==WM_DESTROY){PostQuitMessage(0);return 0;}
        return DefWindowProcW(hwnd,msg,wp,lp);
    }
    static DWORD WINAPI run(void *arg) {
        auto self=(State*)arg;
        WNDCLASSW cls{};cls.lpfnWndProc=procedure;cls.hInstance=GetModuleHandleW(nullptr);cls.hCursor=LoadCursorW(nullptr,IDC_ARROW);cls.hbrBackground=(HBRUSH)GetStockObject(BLACK_BRUSH);cls.lpszClassName=L"WinedowsVirtualDesktop";
        if(!RegisterClassW(&cls)&&GetLastError()!=ERROR_CLASS_ALREADY_EXISTS){self->error=GetLastError();SetEvent(self->ready);return 1;}
        {
            MixedDpiParent hosting;
            self->window=CreateWindowExW(0,cls.lpszClassName,(L"CaskWin desktop - "+self->name).c_str(),WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX|WS_CLIPCHILDREN,40,40,640,480,nullptr,nullptr,cls.hInstance,self);
        }
        if(!self->window){self->error=GetLastError();SetEvent(self->ready);return 1;}
        SetPropW(self->window,WD_DESKTOP_INITIAL_WIDTH,(HANDLE)(ULONG_PTR)self->width);SetPropW(self->window,WD_DESKTOP_INITIAL_HEIGHT,(HANDLE)(ULONG_PTR)self->height);
        SendMessageW(self->window,WD_DESKTOP_MODE,32,MAKELPARAM(self->width,self->height));
        ShowWindow(self->window,SW_SHOWNORMAL);SetEvent(self->ready);
        MSG msg;while(GetMessageW(&msg,nullptr,0,0)>0){TranslateMessage(&msg);DispatchMessageW(&msg);}return 0;
    }
};
static str reg_string(HKEY root,const str &key,const str &name) {
    wchar_t value[512]{};DWORD bytes=sizeof(value);
    LSTATUS e=RegGetValueW(root,key.c_str(),name.c_str(),RRF_RT_REG_SZ,nullptr,value,&bytes);
    if(e==ERROR_FILE_NOT_FOUND||e==ERROR_PATH_NOT_FOUND)return {};check(e,"Read Wine desktop setting");return value;
}
DesktopHost::DesktopHost(const Prefix &prefix,const str &program,const str &option,HANDLE job) {
    str specification=option,name;
    if(specification.empty()){
        HKEY hive=prefix.hive();
        try {
            str app=program.substr(program.find_last_of(L"\\/")+1);
            name=reg_string(hive,L"User\\Software\\Wine\\AppDefaults\\"+app+L"\\Explorer",L"Desktop");
            if(name.empty())name=reg_string(hive,L"User\\Software\\Wine\\Explorer",L"Desktop");
            if(!name.empty())specification=reg_string(hive,L"User\\Software\\Wine\\Explorer\\Desktops",name);
        }catch(...){RegCloseKey(hive);throw;}RegCloseKey(hive);
        if(!name.empty()&&specification.empty())throw std::runtime_error("Wine desktop has no configured size");
    }
    SetEnvironmentVariableW(L"WINEDOWS_DESKTOP_HWND",nullptr);
    if(specification.empty()||specification==L"off")return;
    size_t comma=specification.find(L',');
    if(comma!=str::npos){name=specification.substr(0,comma);specification.erase(0,comma+1);}
    if(name.empty())name=L"Default";
    unsigned w=0,h=0;wchar_t tail=0;
    if(swscanf_s(specification.c_str(),L"%ux%u%c",&w,&h,&tail,1)!=2||w<160||h<100||w>8192||h>8192)throw std::runtime_error("Desktop size must be WIDTHxHEIGHT (160x100 through 8192x8192)");
    std::unique_ptr<State> pending(new State);state=pending.get();state->name=name;state->width=w;state->height=h;state->job=job;
    state->ready=CreateEventW(nullptr,TRUE,FALSE,nullptr);if(!state->ready)check(GetLastError(),"Create desktop event");
    state->thread=CreateThread(nullptr,0,State::run,state,0,nullptr);if(!state->thread)check(GetLastError(),"Start desktop host");
    WaitForSingleObject(state->ready,INFINITE);if(state->error)check(state->error,"Create virtual desktop window");
    SetEnvironmentVariableW(L"WINEDOWS_DESKTOP_HWND",std::to_wstring((ULONG_PTR)state->window).c_str());
    printf("Virtual desktop %s: %ux%u\n",utf8(name).c_str(),w,h);fflush(stdout);
    pending.release();
}
DesktopHost::~DesktopHost(){
    delete state;
}
HWND DesktopHost::window() const {return state?state->window:nullptr;}
}
