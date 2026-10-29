// SPDX-License-Identifier: LGPL-2.1-or-later
// Only fixture-owned windows receive messages. No global input is synthesized.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <string>
static int failures;
static HWND desktop,owner,owner_edit;
static bool modeless;
static bool nested,inside_nested,destroy_in_init;
static int initialized;
static const LPARAM parameter=0x12345678;
static void check(bool ok,const char *what){printf("%s %s error=%lu\n",ok?"PASS":"FAIL",what,GetLastError());if(!ok)++failures;}
static void inspect(HWND window,bool modal){
    check(IsChild(desktop,window),"dialog physically belongs to virtual desktop");
    check(GetParent(window)==owner&&GetWindow(window,GW_OWNER)==owner,"dialog retains logical owner");
    check((HWND)GetWindowLongPtrW(window,GWLP_HWNDPARENT)==owner,"dialog owner window-long matches logical owner");
    DWORD style=(DWORD)GetWindowLongPtrW(window,GWL_STYLE);
    check((style&WS_POPUP)&&!(style&WS_CHILD),"dialog retains popup style for application");
    check(GetAncestor(window,GA_ROOT)==window&&GetAncestor(window,GA_ROOTOWNER)==GetAncestor(owner,GA_ROOTOWNER),"dialog logical root and root owner");
    check(IsWindowEnabled(owner)==!modal,"modal disables owner only; modeless preserves enabled state");
    check(IsWindowEnabled(desktop),"desktop container remains enabled during dialog");
    RECT rect{};GetWindowRect(window,&rect);
    printf("Dialog virtual rectangle %ld,%ld-%ld,%ld\n",rect.left,rect.top,rect.right,rect.bottom);
    check(rect.left>=0&&rect.top>=0&&rect.right<=GetSystemMetrics(SM_CXSCREEN)&&rect.bottom<=GetSystemMetrics(SM_CYSCREEN),"dialog rectangle stays within virtual desktop");
    POINT point{};ClientToScreen(window,&point);POINT before=point;ScreenToClient(window,&point);
    check(point.x==0&&point.y==0&&before.x>=rect.left&&before.y>=rect.top,"dialog coordinate conversion round trip");
}
static INT_PTR CALLBACK dialog_proc(HWND window,UINT msg,WPARAM,LPARAM lp){
    if(msg==WM_INITDIALOG){
        ++initialized;check(lp==parameter,"original dialog initialization parameter");
        inspect(window,!modeless);
        if(destroy_in_init){DestroyWindow(window);return FALSE;}
        if(!modeless)PostMessageW(window,WM_APP+11,0,0);
        return TRUE;
    }
    if(msg==WM_APP+11){
        check(GetFocus()==GetDlgItem(window,102),"dialog keyboard focus reaches first edit control");
        if(nested&&!inside_nested){
            HWND saved_owner=owner,saved_edit=owner_edit;owner=window;owner_edit=GetDlgItem(window,102);inside_nested=true;
            INT_PTR result=DialogBoxParamW(GetModuleHandleW(nullptr),MAKEINTRESOURCEW(101),owner,dialog_proc,parameter);
            check(result==123&&IsWindowEnabled(window),"nested modal result and outer enable restoration");
            check(GetFocus()==owner_edit,"nested modal restores outer edit focus");
            owner=saved_owner;owner_edit=saved_edit;inside_nested=false;
        }
        EndDialog(window,123);return TRUE;
    }
    return FALSE;
}
static BOOL CALLBACK message_control(HWND control,LPARAM data){
    wchar_t cls[64]{};GetClassNameW(control,cls,64);
    if(!_wcsicmp(cls,L"Button")&&IsWindowEnabled(control)&&IsWindowVisible(control))*(HWND*)data=control;
    return TRUE;
}
static VOID CALLBACK message_close_failed(HWND window,UINT,UINT_PTR timer,DWORD){
    KillTimer(window,timer);check(false,"message box button completes native modal loop");PostMessageW(window,WM_CLOSE,0,0);
}
static VOID CALLBACK close_message(HWND window,UINT,UINT_PTR timer,DWORD){
    KillTimer(window,timer);inspect(window,true);
    check(GetFocus()&&IsChild(window,GetFocus()),"message box owns keyboard focus");
    // Windows can internally use IDCANCEL for the sole OK button and translate
    // its result. Find the actual button instead of assuming its control ID.
    HWND button=nullptr;
    EnumChildWindows(window,message_control,(LPARAM)&button);
    check(button!=nullptr,"message box exposes an enabled visible button");
    SetTimer(window,32,500,message_close_failed);SendMessageW(button,BM_CLICK,0,0);
}
static LRESULT CALLBACK message_hook(int code,WPARAM wp,LPARAM lp){
    if(code==HCBT_ACTIVATE){wchar_t name[64]{};GetClassNameW((HWND)wp,name,64);if(!wcscmp(name,L"#32770"))SetTimer((HWND)wp,31,20,close_message);}
    return CallNextHookEx(nullptr,code,wp,lp);
}
static DWORD WINAPI timeout(void*){Sleep(30000);fputs("FAIL dialog fixture timeout\n",stderr);TerminateProcess(GetCurrentProcess(),124);return 0;}
static void pump(){MSG msg;while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)){TranslateMessage(&msg);DispatchMessageW(&msg);}}
static INT_PTR CALLBACK child_dialog(HWND window,UINT message,WPARAM,LPARAM lp){
    if(message==WM_INITDIALOG){
        check(lp==parameter&&GetParent(window)==owner,"control dialog retains initialization parameter and real parent");
        check(IsChild(owner,window)&&(GetWindowLongPtrW(window,GWL_STYLE)&WS_CHILD),"control dialog remains a child of application");
        check(IsWindowEnabled(owner)&&IsWindowEnabled(desktop),"control dialog preserves enable state");return TRUE;
    }return FALSE;
}
int wmain(int argc,wchar_t **argv){
    setvbuf(stdout,nullptr,_IONBF,0);CloseHandle(CreateThread(nullptr,0,timeout,nullptr,0,nullptr));
    desktop=GetDesktopWindow();wchar_t name[64]{};GetClassNameW(desktop,name,64);
    check(!wcscmp(name,L"WinedowsVirtualDesktop"),"virtual desktop active");if(failures)return 1;
    HINSTANCE instance=GetModuleHandleW(nullptr);
    owner=CreateWindowExW(0,L"STATIC",L"Winedows dialog owner",WS_OVERLAPPEDWINDOW|WS_VISIBLE,35,40,480,320,nullptr,nullptr,instance,nullptr);
    check(owner&&IsChild(desktop,owner),"fixture owner is contained");if(!owner)return 1;
    owner_edit=CreateWindowExW(0,L"EDIT",L"",WS_CHILD|WS_VISIBLE|WS_TABSTOP,12,12,160,22,owner,(HMENU)201,instance,nullptr);
    std::wstring only=argc>1?argv[1]:L"all";
    if(only==L"all"||only==L"position"){
        HWND hidden=CreateWindowExW(WS_EX_APPWINDOW|WS_EX_TOPMOST,L"STATIC",L"Initially hidden",WS_POPUP,20,30,120,80,nullptr,nullptr,instance,nullptr);
        check(hidden&&!IsChild(desktop,hidden),"hidden helper initially remains unadopted");
        SetWindowPos(hidden,HWND_TOP,23,31,125,85,SWP_SHOWWINDOW|SWP_NOACTIVATE);
        RECT rect{};GetWindowRect(hidden,&rect);
        check(IsChild(desktop,hidden)&&rect.left==23&&rect.top==31&&rect.right-rect.left==125,"SetWindowPos show adopts without coordinate drift");
        check((GetWindowLongPtrW(hidden,GWL_EXSTYLE)&(WS_EX_APPWINDOW|WS_EX_TOPMOST))==(WS_EX_APPWINDOW|WS_EX_TOPMOST),"application extended styles preserved");
        SetWindowLongPtrW(hidden,GWLP_HWNDPARENT,(LONG_PTR)owner);
        check(IsChild(desktop,hidden)&&GetParent(hidden)==owner&&(HWND)GetWindowLongPtrW(hidden,GWLP_HWNDPARENT)==owner,"changing logical owner preserves physical containment");
        SetWindowLongPtrW(hidden,GWL_EXSTYLE,WS_EX_APPWINDOW);
        check((GetWindowLongPtrW(hidden,GWL_EXSTYLE)&(WS_EX_APPWINDOW|WS_EX_TOPMOST))==WS_EX_APPWINDOW,"extended-style update preserves application value");
        DestroyWindow(hidden);
        hidden=CreateWindowExW(0,L"STATIC",L"Placement show",WS_POPUP,24,32,126,86,nullptr,nullptr,instance,nullptr);
        WINDOWPLACEMENT placement{};placement.length=sizeof(placement);GetWindowPlacement(hidden,&placement);placement.showCmd=SW_SHOWNOACTIVATE;placement.rcNormalPosition={27,35,157,125};
        check(SetWindowPlacement(hidden,&placement)&&IsChild(desktop,hidden),"SetWindowPlacement show adopts hidden top-level window");
        GetWindowRect(hidden,&rect);check(rect.left==27&&rect.top==35&&rect.right==157&&rect.bottom==125,"placement show preserves virtual coordinates");DestroyWindow(hidden);
    }
    HRSRC resource=FindResourceW(instance,MAKEINTRESOURCEW(101),RT_DIALOG);HGLOBAL loaded=LoadResource(instance,resource);
    auto data=(LPCDLGTEMPLATEW)LockResource(loaded);
    nested=only==L"nested";
    if(only==L"all"||only==L"modal"||nested)for(bool ansi:{false,true})for(bool indirect:{false,true}){
        printf("MODAL ansi=%d indirect=%d\n",ansi,indirect);modeless=false;SetFocus(owner_edit);initialized=0;
        INT_PTR result=ansi?(indirect?DialogBoxIndirectParamA(instance,data,owner,dialog_proc,parameter):DialogBoxParamA(instance,MAKEINTRESOURCEA(101),owner,dialog_proc,parameter))
                           :(indirect?DialogBoxIndirectParamW(instance,data,owner,dialog_proc,parameter):DialogBoxParamW(instance,MAKEINTRESOURCEW(101),owner,dialog_proc,parameter));
        check(result==123&&initialized==(nested?2:1),"modal result and callback count");
        check(IsWindowEnabled(owner)&&IsWindowEnabled(desktop),"modal restores owner and desktop enable state");
        check(GetFocus()==owner_edit,"modal restores original edit focus");
    }
    if(only==L"all"||only==L"modeless")for(bool ansi:{false,true})for(bool indirect:{false,true}){
        printf("MODELESS ansi=%d indirect=%d\n",ansi,indirect);modeless=true;initialized=0;
        HWND dialog=ansi?(indirect?CreateDialogIndirectParamA(instance,data,owner,dialog_proc,parameter):CreateDialogParamA(instance,MAKEINTRESOURCEA(101),owner,dialog_proc,parameter))
                        :(indirect?CreateDialogIndirectParamW(instance,data,owner,dialog_proc,parameter):CreateDialogParamW(instance,MAKEINTRESOURCEW(101),owner,dialog_proc,parameter));
        check(dialog&&initialized==1,"modeless creation and callback count");
        if(dialog){SetWindowPos(dialog,nullptr,60,65,0,0,SWP_SHOWWINDOW|SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE);pump();inspect(dialog,false);DestroyWindow(dialog);}
    }
    if(only==L"all"||only==L"message")for(bool ansi:{false,true})for(int variant=0;variant<3;variant++){
        printf("MESSAGE ansi=%d variant=%d\n",ansi,variant);SetFocus(owner_edit);HHOOK hook=SetWindowsHookExW(WH_CBT,message_hook,nullptr,GetCurrentThreadId());
        MSGBOXPARAMSW wide{sizeof(wide),owner,instance,L"Fixture closes itself.",L"Winedows message",MB_OK|MB_ICONINFORMATION,nullptr,0,nullptr,0};
        MSGBOXPARAMSA narrow{sizeof(narrow),owner,instance,"Fixture closes itself.","Winedows message",MB_OK|MB_ICONINFORMATION,nullptr,0,nullptr,0};
        int result=ansi?(variant==2?MessageBoxIndirectA(&narrow):variant==1?MessageBoxExA(owner,narrow.lpszText,narrow.lpszCaption,narrow.dwStyle,0):MessageBoxA(owner,narrow.lpszText,narrow.lpszCaption,narrow.dwStyle))
                       :(variant==2?MessageBoxIndirectW(&wide):variant==1?MessageBoxExW(owner,wide.lpszText,wide.lpszCaption,wide.dwStyle,0):MessageBoxW(owner,wide.lpszText,wide.lpszCaption,wide.dwStyle));
        UnhookWindowsHookEx(hook);check(result==IDOK,"message box result preserved");check(IsWindowEnabled(owner)&&IsWindowEnabled(desktop),"message box restores enable state");
        check(GetFocus()==owner_edit,"message box restores original edit focus");
    }
    if(only==L"lifetime"){
        modeless=true;destroy_in_init=true;
        HWND dialog=CreateDialogParamW(instance,MAKEINTRESOURCEW(101),owner,dialog_proc,parameter);
        check(!IsWindow(dialog),"modeless dialog can destroy itself during initialization");destroy_in_init=false;
        SetLastError(0);INT_PTR result=DialogBoxParamW(instance,MAKEINTRESOURCEW(999),owner,dialog_proc,parameter);
        check(result==-1&&IsWindowEnabled(owner)&&IsWindowEnabled(desktop),"missing resource failure restores modal owner state");
        modeless=false;EnableWindow(owner,FALSE);
        result=DialogBoxParamW(instance,MAKEINTRESOURCEW(101),owner,dialog_proc,parameter);
        check(result==123&&!IsWindowEnabled(owner)&&IsWindowEnabled(desktop),"previously disabled owner stays disabled after modal return");EnableWindow(owner,TRUE);
    }
    if(only==L"controls")for(bool ansi:{false,true}){
        HWND dialog=ansi?CreateDialogParamA(instance,MAKEINTRESOURCEA(102),owner,child_dialog,parameter):CreateDialogParamW(instance,MAKEINTRESOURCEW(102),owner,child_dialog,parameter);
        check(dialog&&IsChild(owner,dialog),"native child-style dialog template remains supported");if(dialog)DestroyWindow(dialog);
    }
    if(argc>2){
        std::wstring command=L"\""+std::wstring(argv[2])+L"\" all";STARTUPINFOW startup{};startup.cb=sizeof(startup);PROCESS_INFORMATION process{};
        bool created=CreateProcessW(argv[2],command.data(),nullptr,nullptr,FALSE,0,nullptr,nullptr,&startup,&process)!=FALSE;
        check(created,"cross-bitness dialog child starts");
        if(created){CloseHandle(process.hThread);for(;;){DWORD wait=MsgWaitForMultipleObjects(1,&process.hProcess,FALSE,15000,QS_ALLINPUT);if(wait==WAIT_OBJECT_0)break;if(wait==WAIT_OBJECT_0+1)pump();else{check(false,"child exits within watchdog");break;}}
            DWORD status=999;GetExitCodeProcess(process.hProcess,&status);CloseHandle(process.hProcess);check(status==0,"cross-bitness child dialogs share desktop and pass");}
    }
    DestroyWindow(owner);printf("Desktop dialogs %zu-bit failures=%d\n",8*sizeof(void*),failures);return failures?1:0;
}
