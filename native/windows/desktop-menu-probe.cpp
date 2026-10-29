// SPDX-License-Identifier: LGPL-2.1-or-later
// Menu-bearing hidden roots, matching legacy applications which show later.
// Only owned windows receive messages. No global keyboard/mouse injection.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <string>
#include <thread>
#include <atomic>
static HWND target,owner,desktop;
static bool virtual_mode,ansi;
static unsigned failures,commands,menu_init,popup_init,focus_in,focus_out,paint_count;
static DWORD create_style;
static bool create_menu,create_visible;
static int last_activation=-1,app_activation=-1;
static std::atomic<const char*> phase{"entry"};
static void check(bool value,const char *name){printf("%s %s error=%lu\n",value?"PASS":"FAIL",name,GetLastError());if(!value)++failures;}
static DWORD WINAPI watchdog(void*){Sleep(20000);fprintf(stderr,"FAIL watchdog phase=%s\n",phase.load());TerminateProcess(GetCurrentProcess(),124);return 0;}
static void pump(unsigned duration=20){
    ULONGLONG end=GetTickCount64()+duration;
    do {MSG message;unsigned count=0;while(count++<100&&PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}Sleep(1);}while(GetTickCount64()<end);
}
static LRESULT CALLBACK procedure(HWND window,UINT message,WPARAM wp,LPARAM lp){
    if(message==WM_CREATE){auto cs=reinterpret_cast<CREATESTRUCTW*>(lp);create_style=cs->style;create_menu=GetMenu(window)!=nullptr;create_visible=IsWindowVisible(window)!=FALSE;}
    if(window==target){
        if(message==WM_COMMAND)++commands;
        if(message==WM_INITMENU)++menu_init;
        if(message==WM_INITMENUPOPUP)++popup_init;
        if(message==WM_SETFOCUS)++focus_in;
        if(message==WM_KILLFOCUS)++focus_out;
        if(message==WM_ACTIVATE)last_activation=LOWORD(wp)!=WA_INACTIVE;
        if(message==WM_ACTIVATEAPP)app_activation=wp!=0;
        if(message==WM_ACTIVATE||message==WM_ACTIVATEAPP||message==WM_SETFOCUS||message==WM_KILLFOCUS)
            printf("EVENT message=%04x wp=%llu focus=%d\n",message,(unsigned long long)wp,GetFocus()==window);
        if(message==WM_TIMER&&wp==12){KillTimer(window,12);EndMenu();}
        if(message==WM_PAINT){PAINTSTRUCT ps;HDC dc=BeginPaint(window,&ps);RECT rect;GetClientRect(window,&rect);FillRect(dc,&rect,(HBRUSH)GetStockObject(GRAY_BRUSH));EndPaint(window,&ps);++paint_count;return 0;}
    }
    return ansi?DefWindowProcA(window,message,wp,lp):DefWindowProcW(window,message,wp,lp);
}
static HMENU make_menu(){HMENU menu=CreateMenu(),popup=CreatePopupMenu();AppendMenuW(popup,MF_STRING,101,L"&Command\tF2");AppendMenuW(menu,MF_POPUP,(UINT_PTR)popup,L"&Fixture");return menu;}
static void navigate(bool alt){
    unsigned before=commands,initialized=menu_init,popups=popup_init;
    SetTimer(target,12,400,nullptr);
    HWND own=target;
    std::thread input([own,alt]{Sleep(30);if(!alt){PostMessageW(own,WM_KEYDOWN,VK_DOWN,1);Sleep(20);}PostMessageW(own,WM_KEYDOWN,VK_RETURN,1);Sleep(70);PostMessageW(own,WM_CANCELMODE,0,0);});
    SendMessageW(target,WM_SYSCOMMAND,SC_KEYMENU,alt?L'f':0);
    KillTimer(target,12);input.join();pump();
    check(commands==before+1&&menu_init>initialized&&popup_init>popups,alt?"Alt menu navigation dispatches command and initialization":"F10 menu navigation dispatches command and initialization");
}
static INT_PTR CALLBACK dialog(HWND window,UINT message,WPARAM,LPARAM){
    if(message==WM_INITDIALOG){
        check(!IsWindowEnabled(target)&&IsWindowEnabled(desktop),"modal disables menu owner without disabling desktop");
        check(GetParent(window)==target&&GetWindow(window,GW_OWNER)==target,"modal retains logical menu owner");
        PostMessageW(window,WM_APP+1,0,0);return TRUE;
    }
    if(message==WM_APP+1){EndDialog(window,73);return TRUE;}
    return FALSE;
}
static void spawn_child(const wchar_t *program,const wchar_t *mode,const wchar_t *encoding){
    phase="child";
    std::wstring command=L"\""+std::wstring(program)+L"\" "+mode+L" "+encoding;
    STARTUPINFOW startup{};startup.cb=sizeof(startup);PROCESS_INFORMATION child{};
    if(!CreateProcessW(program,command.data(),nullptr,nullptr,FALSE,CREATE_SUSPENDED,nullptr,nullptr,&startup,&child)){check(false,"create cross-architecture child");return;}
    HANDLE job=CreateJobObjectW(nullptr,nullptr);JOBOBJECT_EXTENDED_LIMIT_INFORMATION limit{};limit.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    bool bounded=job&&SetInformationJobObject(job,JobObjectExtendedLimitInformation,&limit,sizeof(limit))&&AssignProcessToJobObject(job,child.hProcess);
    if(!bounded){TerminateProcess(child.hProcess,125);check(false,"bound owned child to cleanup job");}
    else {
        ResumeThread(child.hThread);ULONGLONG end=GetTickCount64()+12000;DWORD code=STILL_ACTIVE;
        while(GetTickCount64()<end){if(WaitForSingleObject(child.hProcess,0)==WAIT_OBJECT_0)break;pump(5);}
        if(WaitForSingleObject(child.hProcess,0)!=WAIT_OBJECT_0){TerminateJobObject(job,124);check(false,"child completed before deadline");}
        WaitForSingleObject(child.hProcess,1000);GetExitCodeProcess(child.hProcess,&code);check(code==0,"cross-architecture child menu regression");
    }
    if(job)CloseHandle(job);
    CloseHandle(child.hThread);CloseHandle(child.hProcess);
}
int wmain(int argc,wchar_t **argv){
    setvbuf(stdout,nullptr,_IONBF,0);CloseHandle(CreateThread(nullptr,0,watchdog,nullptr,0,nullptr));
    virtual_mode=argc>1&&!wcscmp(argv[1],L"virtual");ansi=argc>2&&!wcscmp(argv[2],L"ansi");
    HINSTANCE instance=GetModuleHandleW(nullptr);desktop=GetDesktopWindow();
    WNDCLASSW wide{};wide.lpfnWndProc=procedure;wide.hInstance=instance;wide.lpszClassName=L"WinedowsMenuRootFixture";
    WNDCLASSA narrow{};narrow.lpfnWndProc=procedure;narrow.hInstance=instance;narrow.lpszClassName="WinedowsMenuRootFixture";
    check((ansi?RegisterClassA(&narrow):RegisterClassW(&wide))!=0,"register owned fixture class");
    owner=CreateWindowExW(0,L"STATIC",L"Owned menu owner",WS_OVERLAPPEDWINDOW|WS_VISIBLE,10,10,200,100,nullptr,nullptr,instance,nullptr);
    HMENU menu=make_menu();phase="create";
    target=ansi?CreateWindowExA(0,narrow.lpszClassName,"Owned menu target",WS_OVERLAPPEDWINDOW,25,30,320,240,nullptr,menu,instance,nullptr)
               :CreateWindowExW(0,wide.lpszClassName,L"Owned menu target",WS_OVERLAPPEDWINDOW,25,30,320,240,nullptr,menu,instance,nullptr);
    check(target!=nullptr,"create hidden menu root");if(!target)return 1;
    check(create_menu&&!create_visible&&!(create_style&WS_CHILD),"native hidden WM_CREATE menu and style contract");
    RECT initial{},shown{};GetClientRect(target,&initial);phase="show";
    ShowWindow(target,SW_SHOWNORMAL);UpdateWindow(target);pump(50);GetClientRect(target,&shown);
    check(GetMenu(target)==menu,"adoption preserves original menu handle");
    check(EqualRect(&initial,&shown)!=FALSE,"adoption preserves exact client size");
    check(GetFocus()==target&&focus_in>0,"initial show establishes real keyboard focus");
    check(last_activation==1,"focused menu root keeps its active notification after pumping");
    check(paint_count>0,"menu root paints after show");
    check(GetWindow(target,GW_OWNER)==nullptr&&GetParent(target)==nullptr,"unowned application root remains unowned");
    check(GetAncestor(target,GA_ROOT)==target,"logical root retained");
    if(virtual_mode){
        check(GetAncestor(target,GA_PARENT)==desktop,"logical desktop parent retained");
        check(GetPropW(target,L"Winedows.NativeMenuRoot")==desktop,"registered native menu root matches desktop");
        check(!IsChild(desktop,target),"native menu style avoids child emulation");
        RECT rect{};GetWindowRect(target,&rect);check(rect.left==25&&rect.top==30,"adopted root keeps virtual coordinates");
        POINT origin{},roundtrip{};ClientToScreen(target,&origin);roundtrip=origin;ScreenToClient(target,&roundtrip);
        check(roundtrip.x==0&&roundtrip.y==0&&origin.x>=rect.left&&origin.y>=rect.top,"menu root coordinate round trip");
        RECT host_area{};GetClientRect(desktop,&host_area);
        SetWindowPos(target,nullptr,host_area.right-60,30,0,0,SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE);
        HDC dc=GetDC(target);RECT clip{};GetClipBox(dc,&clip);BOOL outside=PtVisible(dc,200,50);ReleaseDC(target,dc);
        check(clip.right<=60&&!outside,"native menu root paint clips to desktop");
        SetWindowPos(target,nullptr,25,30,0,0,SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE);
    }
    phase="menu";ACCEL entry{FVIRTKEY,VK_F2,101};HACCEL accelerator=CreateAcceleratorTableW(&entry,1);MSG key{};key.hwnd=target;key.message=WM_KEYDOWN;key.wParam=VK_F2;
    EnableMenuItem(menu,101,MF_BYCOMMAND|MF_GRAYED);unsigned before=commands,initialized=menu_init,popups=popup_init;TranslateAcceleratorW(target,accelerator,&key);
    check(commands==before&&menu_init>initialized&&popup_init>popups,"disabled menu accelerator initializes but does not dispatch");
    EnableMenuItem(menu,101,MF_BYCOMMAND|MF_ENABLED);TranslateAcceleratorW(target,accelerator,&key);check(commands==before+1,"enabled accelerator dispatches once");DestroyAcceleratorTable(accelerator);
    navigate(true);navigate(false);
    HMENU replacement=make_menu();check(SetMenu(target,replacement)&&GetMenu(target)==replacement,"menu replacement succeeds");
    check(SetMenu(target,nullptr)&&!GetMenu(target),"menu removal succeeds");check(SetMenu(target,menu)&&GetMenu(target)==menu,"menu reinstallation succeeds");
    phase="style";LONG_PTR original=GetWindowLongPtrW(target,GWL_STYLE);SetWindowLongPtrW(target,GWL_STYLE,original|WS_CLIPCHILDREN);
    SetWindowPos(target,nullptr,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE|SWP_FRAMECHANGED);
    check(GetMenu(target)==menu&&!(GetWindowLongPtrW(target,GWL_STYLE)&WS_CHILD),"style mutation retains native menu semantics");
    SetWindowLongPtrW(target,GWLP_HWNDPARENT,(LONG_PTR)owner);
    check(GetWindow(target,GW_OWNER)==owner&&(!virtual_mode||GetParent(target)==owner),"post-adoption logical owner assignment retained");
    if(virtual_mode)check(GetAncestor(target,GA_PARENT)==desktop,"owner change preserves physical desktop containment");
    phase="modal";
    if(virtual_mode){
        struct {DLGTEMPLATE header;WORD menu,cls,title;} resource{};
        resource.header.style=WS_POPUP|WS_CAPTION|DS_MODALFRAME;resource.header.cx=100;resource.header.cy=50;
        SetFocus(target);INT_PTR result=DialogBoxIndirectParamW(instance,&resource.header,target,dialog,0);
        check(result==73&&IsWindowEnabled(target)&&GetFocus()==target,"modal result restores menu owner and focus");
    }
    phase="bounds";
    if(virtual_mode){
        ShowWindow(target,SW_MAXIMIZE);pump();RECT area{};GetClientRect(target,&shown);GetClientRect(desktop,&area);
        check(shown.right<=area.right&&shown.bottom<=area.bottom&&!IsZoomed(desktop),"maximize remains within desktop");
        ShowWindow(target,SW_RESTORE);pump();GetClientRect(target,&shown);check(EqualRect(&initial,&shown)!=FALSE,"restore returns exact client size");
        ShowWindow(target,SW_MINIMIZE);pump();check(IsIconic(target)&&!IsIconic(desktop),"minimize affects only menu root");ShowWindow(target,SW_RESTORE);pump();
    }
    phase="destroy-target";puts("DESTROY target begin");DestroyWindow(target);target=nullptr;puts("DESTROY target end");
    phase="destroy-owner";DestroyWindow(owner);owner=nullptr;
    if(IsMenu(menu))DestroyMenu(menu);
    if(IsMenu(replacement))DestroyMenu(replacement);
    if(argc>3)spawn_child(argv[3],virtual_mode?L"virtual":L"native",ansi?L"ansi":L"unicode");
    phase="exit";printf("RESULT failures=%u focus_in=%u focus_out=%u app_activation=%d (no external activation-cycle claim)\n",failures,focus_in,focus_out,app_activation);return failures?1:0;
}
