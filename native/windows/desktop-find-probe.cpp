// SPDX-License-Identifier: LGPL-2.1-or-later
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <algorithm>
static int failures, gettext_messages;
static const wchar_t *class_name=L"WinedowsFindFixture09";
static const wchar_t *duplicate=L"Winedows find duplicate 09";
static void check(bool value,const char *what){printf("%s %s error=%lu\n",value?"PASS":"FAIL",what,GetLastError());if(!value)++failures;}
static LRESULT CALLBACK procedure(HWND window,UINT message,WPARAM wp,LPARAM lp){
    if(message==WM_GETTEXT)++gettext_messages;
    return DefWindowProcW(window,message,wp,lp);
}
static HWND create(const wchar_t *title,DWORD style=WS_OVERLAPPEDWINDOW,HWND parent=nullptr){
    return CreateWindowExW(WS_EX_NOACTIVATE,class_name,title,style,25,30,240,140,parent,nullptr,GetModuleHandleW(nullptr),nullptr);
}
static HWND number(const wchar_t *text){return reinterpret_cast<HWND>(static_cast<ULONG_PTR>(_wcstoui64(text,nullptr,10)));}
static std::wstring numeric(HWND value){return std::to_wstring(reinterpret_cast<ULONG_PTR>(value));}
static std::wstring message_title(DWORD process){return L"Winedows message only 09 "+std::to_wstring(process);}
static void pump(){MSG message;while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}}
static bool wait_child(HANDLE process,DWORD &code){
    DWORD end=GetTickCount()+15000;
    while(static_cast<LONG>(end-GetTickCount())>0){
        if(MsgWaitForMultipleObjects(1,&process,FALSE,20,QS_ALLINPUT)==WAIT_OBJECT_0)return GetExitCodeProcess(process,&code)!=FALSE;
        pump();
    }
    TerminateProcess(process,99);WaitForSingleObject(process,2000);return false;
}
static int host(const wchar_t *ready,const wchar_t *stop){
    HWND shared=create(duplicate);
    // Hidden mock shell geometry. It belongs only to this fixture and never
    // changes or queries the user's real taskbar.
    HWND outside=CreateWindowExW(0,class_name,L"Winedows outside only 09",WS_POPUP,0,1040,1920,40,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    HWND message=create(message_title(GetCurrentProcessId()).c_str(),0,HWND_MESSAGE);
    if(!shared||!outside||!message)return 2;
    FILE *file=_wfopen(ready,L"wb");if(!file)return 3;
    fprintf(file,"%llu %llu %llu\n",static_cast<unsigned long long>(reinterpret_cast<ULONG_PTR>(shared)),static_cast<unsigned long long>(reinterpret_cast<ULONG_PTR>(outside)),static_cast<unsigned long long>(reinterpret_cast<ULONG_PTR>(message)));fclose(file);
    DWORD end=GetTickCount()+120000;
    while(static_cast<LONG>(end-GetTickCount())>0&&GetFileAttributesW(stop)==INVALID_FILE_ATTRIBUTES){pump();Sleep(10);}
    DestroyWindow(message);DestroyWindow(outside);DestroyWindow(shared);return 0;
}
int wmain(int argc,wchar_t **argv){
    WNDCLASSW cls{};cls.lpfnWndProc=procedure;cls.hInstance=GetModuleHandleW(nullptr);cls.lpszClassName=class_name;
    ATOM atom=RegisterClassW(&cls);if(!atom)return 2;
    if(argc==4&&!wcscmp(argv[1],L"--host"))return host(argv[2],argv[3]);
    if(argc==4&&!wcscmp(argv[1],L"--child")){
        HWND hidden=number(argv[2]),visible=number(argv[3]);
        check(FindWindowW(class_name,L"Winedows parent hidden 09")==hidden,"child sees hidden parent root");
        check(FindWindowExW(nullptr,nullptr,class_name,L"Winedows parent visible 09")==visible,"child sees contained parent root");
        return failures?1:0;
    }
    if(argc<5)return 2;
    bool scoped=!wcscmp(argv[1],L"--virtual");
    HWND host_duplicate=number(argv[2]),host_outside=number(argv[3]),host_message=number(argv[4]);
    DWORD message_process=0;GetWindowThreadProcessId(host_message,&message_process);
    std::wstring message_name=message_title(message_process);std::string message_ansi(message_name.begin(),message_name.end());
    HWND desktop=GetDesktopWindow();
    HWND visible=create(duplicate,WS_OVERLAPPEDWINDOW|WS_VISIBLE);
    HWND hidden1=create(duplicate);
    HWND hidden2=CreateWindowExA(WS_EX_NOACTIVATE,"WinedowsFindFixture09","Winedows find duplicate 09",WS_OVERLAPPEDWINDOW,25,30,240,140,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    HWND empty=create(L""),child=create(L"Winedows child 09",WS_CHILD,visible);
    check(visible&&hidden1&&hidden2&&empty&&child,"fixture windows created");
    if(scoped)check(IsChild(desktop,visible)&&!IsChild(desktop,hidden1),"visible and hidden logical roots exercise both lists");
    std::vector<HWND> expected{visible,hidden1,hidden2};if(!scoped)expected.push_back(host_duplicate);
    std::vector<HWND> found;
    HWND after=nullptr;
    for(unsigned i=0;i<16;++i){after=FindWindowExW(nullptr,after,class_name,duplicate);if(!after)break;found.push_back(after);}
    check(found.size()==expected.size()&&std::all_of(expected.begin(),expected.end(),[&](HWND item){return std::count(found.begin(),found.end(),item)==1;}),"duplicate enumeration covers each logical root once");
    found.clear();after=nullptr;
    for(unsigned i=0;i<16;++i){after=FindWindowExA(nullptr,after,"winedowsfindfixture09","WINEDOWS FIND DUPLICATE 09");if(!after)break;found.push_back(after);}
    check(found.size()==expected.size()&&std::all_of(expected.begin(),expected.end(),[&](HWND item){return std::count(found.begin(),found.end(),item)==1;}),"ANSI Ex enumeration preserves matching and progression");
    check(FindWindowW(class_name,L"Winedows outside only 09")== (scoped?nullptr:host_outside),"outside-only root visibility follows desktop scope");
    check(FindWindowA("winedowsfindfixture09","WINEDOWS FIND DUPLICATE 09")!=nullptr,"ANSI case-insensitive native matcher");
    check(FindWindowW(MAKEINTATOM(atom),L"")==empty,"class atom and empty title");
    check(FindWindowW(class_name,nullptr)!=nullptr,"NULL title matches any title");
    HWND any=FindWindowW(nullptr,nullptr);
    check(any&&(!scoped||IsChild(desktop,any)),"both NULL inputs search the appropriate desktop");
    check(!FindWindowW(L"",nullptr),"empty class differs from NULL class");
    check(FindWindowW(nullptr,L"Winedows outside only 09")== (scoped?nullptr:host_outside),"NULL class retains desktop scope");
    check(FindWindowExW(visible,nullptr,class_name,L"Winedows child 09")==child,"explicit child search stays native");
    check(!FindWindowW(class_name,L"Winedows child 09"),"ordinary child excluded from top-level search");
    check(FindWindowExW(HWND_MESSAGE,nullptr,class_name,message_name.c_str())==host_message,"explicit message search stays native");
    check(FindWindowExW(nullptr,nullptr,class_name,message_name.c_str())==host_message,"NULL-parent Ex search includes native message window");
    check(FindWindowW(class_name,message_name.c_str())==host_message,"FindWindowW native message-window behavior");
    check(FindWindowA("WinedowsFindFixture09",message_ansi.c_str())==host_message,"FindWindowA native message-window behavior");
    check(!FindWindowExW(desktop,nullptr,class_name,message_name.c_str()),"explicit desktop excludes message-only window");
    SetLastError(0x1234abcd);HWND from_top=FindWindowExW(nullptr,hidden1,class_name,message_name.c_str());DWORD from_top_error=GetLastError();
    SetLastError(0x1234abcd);HWND from_message=FindWindowExW(nullptr,host_message,class_name,nullptr);DWORD from_message_error=GetLastError();
    printf("CONTINUATION top_to_message=%p error=%lu after_message=%p error=%lu\n",from_top,from_top_error,from_message,from_message_error);
    check(!from_top&&!from_message&&from_top_error==0x1234abcd&&from_message_error==0x1234abcd,"non-NULL continuation does not cross message-only list");
    HWND direct_child=create(L"Winedows desktop child 09",WS_CHILD,desktop);
    printf("DIRECT_CHILD created=%p find=%p ex=%p parent=%p style=%08lx\n",direct_child,FindWindowW(class_name,L"Winedows desktop child 09"),FindWindowExW(desktop,nullptr,class_name,L"Winedows desktop child 09"),GetParent(direct_child),(DWORD)GetWindowLongPtrW(direct_child,GWL_STYLE));
    check(direct_child&&FindWindowW(class_name,L"Winedows desktop child 09")==direct_child&&FindWindowExW(desktop,nullptr,class_name,L"Winedows desktop child 09")==direct_child,"direct desktop WS_CHILD participates in native desktop search");
    DestroyWindow(direct_child);
    SetLastError(0x1234abcd);HWND missing=FindWindowExW(nullptr,nullptr,class_name,L"Winedows absent title 09");DWORD error=GetLastError();
    check(!missing&&error==0x1234abcd,"unsuccessful Ex lookup preserves LastError");
    SetLastError(0x1234abcd);HWND present=FindWindowW(class_name,L"");error=GetLastError();
    check(present==empty&&error==0x1234abcd,"successful FindWindow preserves LastError");
    SetLastError(0x1234abcd);HWND invalid=FindWindowExW(visible,hidden1,class_name,nullptr);error=GetLastError();
    printf("CONTROL invalid-child result=%p error=%lu\n",invalid,error);
    check(!invalid,"invalid explicit child relation remains rejected");
    check(gettext_messages==0,"matching did not dispatch WM_GETTEXT");
    // childAfter need not itself match the filter: empty is a logical root.
    std::vector<HWND> all;
    after=nullptr;for(unsigned i=0;i<32;++i){after=FindWindowExW(nullptr,after,class_name,nullptr);if(!after)break;all.push_back(after);}
    auto position=std::find(all.begin(),all.end(),empty);HWND next=nullptr;
    if(position!=all.end())for(++position;position!=all.end();++position)if(std::find(expected.begin(),expected.end(),*position)!=expected.end()){next=*position;break;}
    check(FindWindowExW(nullptr,empty,class_name,duplicate)==next,"nonmatching childAfter advances from its logical position");
    if(scoped){
        SetLastError(0x1234abcd);check(!FindWindowExW(nullptr,host_outside,class_name,duplicate),"outside childAfter cannot jump into logical list");
    }
    // FullTilt's generic legacy work-area calculation (CADET.EXE 0x408862):
    // a host shell rectangle used with virtual bounds turns right into 1.
    RECT work{};GetWindowRect(desktop,&work);
    HWND shell=FindWindowW(class_name,L"Winedows outside only 09");
    if(shell){RECT bar{};GetWindowRect(shell,&bar);
        if(bar.left<work.left&&bar.right>work.right){if(bar.top<work.top)work.top=bar.bottom+1;else work.bottom=bar.top-1;}
        else if(bar.left<work.left)work.left=bar.right+1;else work.right=bar.left+1;
    }
    int centered=(work.right-work.left-754)/2+work.left-2;
    printf("CADET_CENTER work=%ld,%ld,%ld,%ld x=%d\n",work.left,work.top,work.right,work.bottom,centered);
    check(scoped?centered==21:centered==-378,"owned mock proves taskbar-scope centering calculation");
    if(argc==6){
        HWND parent_hidden=create(L"Winedows parent hidden 09"),parent_visible=create(L"Winedows parent visible 09",WS_OVERLAPPEDWINDOW|WS_VISIBLE);
        std::wstring command=L"\""+std::wstring(argv[5])+L"\" --child "+numeric(parent_hidden)+L" "+numeric(parent_visible);
        STARTUPINFOW si{};si.cb=sizeof(si);PROCESS_INFORMATION pi{};
        BOOL created=CreateProcessW(argv[5],command.data(),nullptr,nullptr,FALSE,0,nullptr,nullptr,&si,&pi);check(created!=FALSE,"cross-process child created");
        if(created){DWORD code=1;bool ended=wait_child(pi.hProcess,code);check(ended&&code==0,"child shares logical desktop across architecture");CloseHandle(pi.hThread);CloseHandle(pi.hProcess);}
        DestroyWindow(parent_visible);DestroyWindow(parent_hidden);
    }
    // Record menu adoption separately. This is diagnostic, not a claim that a
    // native child menu has the same nonclient behavior as a top-level menu.
    HMENU menu=CreateMenu();AppendMenuW(menu,MF_STRING,1,L"Fixture");
    HWND menu_window=CreateWindowExW(WS_EX_NOACTIVATE,class_name,L"Winedows menu sizing 09",WS_OVERLAPPEDWINDOW,50,50,300,240,nullptr,menu,cls.hInstance,nullptr);
    RECT before{},later{};GetClientRect(menu_window,&before);ShowWindow(menu_window,SW_SHOWNOACTIVATE);GetClientRect(menu_window,&later);
    printf("MENU_CLIENT before=%ld,%ld after=%ld,%ld menu_retained=%d\n",before.right,before.bottom,later.right,later.bottom,GetMenu(menu_window)==menu);
    DestroyWindow(menu_window);
    DestroyWindow(child);DestroyWindow(empty);DestroyWindow(hidden2);DestroyWindow(hidden1);DestroyWindow(visible);
    printf("Desktop find %zu-bit: %d failure(s)\n",sizeof(void*)*8,failures);return failures?1:0;
}
