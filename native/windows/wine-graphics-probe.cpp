// SPDX-License-Identifier: LGPL-2.1-or-later
// Exercise the built Wine implementation, and fail if Windows' DLL was used.
#define WIN32_LEAN_AND_MEAN
#define DIRECTDRAW_VERSION 0x0700
#include <windows.h>
#include <ddraw.h>
#include <d3d9.h>
#include <gl/GL.h>
#include <cstdio>
#include <initializer_list>
#include <string>

static int failures;
static void check(bool ok,const char *name,HRESULT hr=S_OK) {
    printf("%s %s (hr=%08lx)\n",ok?"PASS":"FAIL",name,(unsigned long)hr);
    if(!ok)++failures;
}
#ifdef WINEDOWS_STATIC_GRAPHICS_IMPORTS
static bool imported_from(const char *dll,const char *symbol,HMODULE provider) {
    BYTE *base=(BYTE*)GetModuleHandleW(nullptr);auto dos=(IMAGE_DOS_HEADER*)base;
    auto nt=(IMAGE_NT_HEADERS*)(base+dos->e_lfanew);
    auto imports=(IMAGE_IMPORT_DESCRIPTOR*)(base+nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress);
    for(;imports->Name;imports++)if(!_stricmp((char*)base+imports->Name,dll)){
        if(!imports->OriginalFirstThunk)return false;
        auto names=(IMAGE_THUNK_DATA*)(base+imports->OriginalFirstThunk);
        auto bound=(IMAGE_THUNK_DATA*)(base+imports->FirstThunk);
        for(;names->u1.AddressOfData;names++,bound++){
            if(IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal))continue;
            auto name=(IMAGE_IMPORT_BY_NAME*)(base+names->u1.AddressOfData);
            if(!strcmp((char*)name->Name,symbol))return (FARPROC)bound->u1.Function==GetProcAddress(provider,symbol);
        }
    }
    return false;
}
#endif
static bool run_child(const wchar_t *program,bool native) {
    std::wstring command=L"\""+std::wstring(program)+L"\""+(native?L" --native":L"");
    // Deliberately provide a custom environment without prefix/override entries.
    wchar_t environment[]=L"WINEDEBUG=-all\0\0";
    STARTUPINFOW startup{};startup.cb=sizeof(startup);PROCESS_INFORMATION process{};
    if(!CreateProcessW(program,command.data(),nullptr,nullptr,FALSE,CREATE_UNICODE_ENVIRONMENT,environment,nullptr,&startup,&process))return false;
    DWORD code=1;if(WaitForSingleObject(process.hProcess,30000)==WAIT_OBJECT_0)GetExitCodeProcess(process.hProcess,&code);else TerminateProcess(process.hProcess,99);
    CloseHandle(process.hThread);CloseHandle(process.hProcess);return code==0;
}
int wmain(int argc,wchar_t **argv) {
    bool expect_wine=!(argc>1&&!wcscmp(argv[1],L"--native"));
    bool wine_draw=expect_wine,wine_d3d9=expect_wine;
    for(int i=1;i<argc;i++){if(!wcscmp(argv[i],L"--native-ddraw"))wine_draw=false;if(!wcscmp(argv[i],L"--native-d3d9"))wine_d3d9=false;}
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
    HMODULE draw=LoadLibraryW(L"ddraw.dll"),d3d=LoadLibraryW(L"d3d9.dll");
    check(draw&&d3d,"load graphics DLLs",HRESULT_FROM_WIN32(GetLastError()));
    HMODULE wine=GetModuleHandleW(L"wined3d.dll");
    check(expect_wine?(wine&&GetProcAddress(wine,"wined3d_get_renderer")):!wine,
          expect_wine?"actual WineD3D implementation is loaded":"native Windows implementations are loaded");
    if(!draw||!d3d||(expect_wine&&!wine)||(!expect_wine&&wine))return 1;
    for(HMODULE module:{draw,d3d,wine}) {
        if(!module)continue;
        wchar_t path[MAX_PATH];GetModuleFileNameW(module,path,MAX_PATH);wprintf(L"Module: %ls\n",path);
    }
    if(expect_wine){
        wchar_t base[MAX_PATH],frontend[MAX_PATH];GetModuleFileNameW(wine,base,MAX_PATH);
        std::wstring folder(base);folder.resize(folder.find_last_of(L'\\')+1);
        GetModuleFileNameW(draw,frontend,MAX_PATH);check((!_wcsnicmp(frontend,folder.c_str(),folder.size()))==wine_draw,"DirectDraw uses its selected Wine/native provider");
        GetModuleFileNameW(d3d,frontend,MAX_PATH);check((!_wcsnicmp(frontend,folder.c_str(),folder.size()))==wine_d3d9,"Direct3D9 uses its selected Wine/native provider");
    }
    using DrawCreate=HRESULT(WINAPI*)(GUID*,void**,REFIID,IUnknown*);
#ifdef WINEDOWS_STATIC_GRAPHICS_IMPORTS
    DrawCreate create_draw=&DirectDrawCreateEx;
    check(imported_from("ddraw.dll","DirectDrawCreateEx",draw),"static DirectDraw import uses selected provider");
#else
    auto create_draw=(DrawCreate)GetProcAddress(draw,"DirectDrawCreateEx");
#endif
    IDirectDraw7 *dd=nullptr;HRESULT hr=create_draw?create_draw(nullptr,(void**)&dd,IID_IDirectDraw7,nullptr):E_NOINTERFACE;
    check(SUCCEEDED(hr),"DirectDraw7 creation",hr);
    HWND window=CreateWindowExW(0,L"STATIC",L"Winedows Wine graphics probe",WS_OVERLAPPEDWINDOW,0,0,128,128,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    if(GetEnvironmentVariableW(L"WINEDOWS_DESKTOP_HWND",nullptr,0)){ShowWindow(window,SW_SHOW);check(IsChild(GetDesktopWindow(),window)!=FALSE,"visible rendering window is contained");}
    if(expect_wine) {
        HWND gl_window=CreateWindowExW(0,L"STATIC",L"OpenGL renderer probe",0,0,0,32,32,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
        HDC dc=GetDC(gl_window);PIXELFORMATDESCRIPTOR pfd{};pfd.nSize=sizeof(pfd);pfd.nVersion=1;
        pfd.dwFlags=PFD_DRAW_TO_WINDOW|PFD_SUPPORT_OPENGL;pfd.iPixelType=PFD_TYPE_RGBA;pfd.cColorBits=24;
        int format=ChoosePixelFormat(dc,&pfd);HGLRC context=nullptr;
        if(format&&SetPixelFormat(dc,format,&pfd))context=wglCreateContext(dc);
        bool ready=context&&wglMakeCurrent(dc,context);check(ready,"native OpenGL driver available for WineD3D");
        if(ready){printf("OpenGL renderer: %s; version: %s\n",glGetString(GL_RENDERER),glGetString(GL_VERSION));wglMakeCurrent(nullptr,nullptr);}
        if(context)wglDeleteContext(context);ReleaseDC(gl_window,dc);DestroyWindow(gl_window);
    }
    if(dd) {
        hr=dd->SetCooperativeLevel(window,DDSCL_NORMAL);check(SUCCEEDED(hr),"DirectDraw cooperative level",hr);
        DDSURFACEDESC2 desc{};desc.dwSize=sizeof(desc);desc.dwFlags=DDSD_CAPS|DDSD_WIDTH|DDSD_HEIGHT|DDSD_PIXELFORMAT;
        desc.dwWidth=32;desc.dwHeight=32;desc.ddsCaps.dwCaps=DDSCAPS_OFFSCREENPLAIN|DDSCAPS_SYSTEMMEMORY;
        desc.ddpfPixelFormat.dwSize=sizeof(DDPIXELFORMAT);desc.ddpfPixelFormat.dwFlags=DDPF_RGB;desc.ddpfPixelFormat.dwRGBBitCount=32;
        desc.ddpfPixelFormat.dwRBitMask=0xff0000;desc.ddpfPixelFormat.dwGBitMask=0xff00;desc.ddpfPixelFormat.dwBBitMask=0xff;
        IDirectDrawSurface7 *surface=nullptr;hr=dd->CreateSurface(&desc,&surface,nullptr);
        check(SUCCEEDED(hr),"DirectDraw surface creation",hr);
        if(surface) {
            DDSURFACEDESC2 locked{};locked.dwSize=sizeof(locked);hr=surface->Lock(nullptr,&locked,DDLOCK_WAIT,nullptr);
            check(SUCCEEDED(hr)&&locked.lpSurface&&locked.lPitch>=128,"DirectDraw surface lock",hr);
            if(SUCCEEDED(hr)){*(DWORD*)locked.lpSurface=0x00123456;surface->Unlock(nullptr);}
            surface->Release();
        }
        dd->Release();
    }
    using D3DCreate=IDirect3D9*(WINAPI*)(UINT);
#ifdef WINEDOWS_STATIC_GRAPHICS_IMPORTS
    D3DCreate create=&Direct3DCreate9;
    check(imported_from("d3d9.dll","Direct3DCreate9",d3d),"static Direct3D9 import uses selected provider");
#else
    auto create=(D3DCreate)GetProcAddress(d3d,"Direct3DCreate9");
#endif
    IDirect3D9 *api=create?create(D3D_SDK_VERSION):nullptr;
    check(api!=nullptr,"Direct3D9 creation");
    if(api) {
        D3DADAPTER_IDENTIFIER9 id{};hr=api->GetAdapterIdentifier(D3DADAPTER_DEFAULT,0,&id);
        if(SUCCEEDED(hr))printf("Selected adapter: %s (vendor=%04lx device=%04lx)\n",id.Description,id.VendorId,id.DeviceId);
        D3DPRESENT_PARAMETERS pp{};pp.Windowed=TRUE;pp.SwapEffect=D3DSWAPEFFECT_DISCARD;pp.hDeviceWindow=window;
        pp.BackBufferWidth=32;pp.BackBufferHeight=32;pp.BackBufferFormat=D3DFMT_X8R8G8B8;pp.PresentationInterval=D3DPRESENT_INTERVAL_IMMEDIATE;
        IDirect3DDevice9 *device=nullptr;hr=api->CreateDevice(0,D3DDEVTYPE_HAL,window,D3DCREATE_SOFTWARE_VERTEXPROCESSING,&pp,&device);
        check(SUCCEEDED(hr),"D3D9 hardware rendering device",hr);
        if(device) {
            hr=device->Clear(0,nullptr,D3DCLEAR_TARGET,D3DCOLOR_XRGB(64,128,192),1.0f,0);
            check(SUCCEEDED(hr),"D3D9 render clear",hr);
            IDirect3DSurface9 *target=nullptr,*readback=nullptr;
            hr=device->GetRenderTarget(0,&target);
            if(SUCCEEDED(hr))hr=device->CreateOffscreenPlainSurface(32,32,D3DFMT_X8R8G8B8,D3DPOOL_SYSTEMMEM,&readback,nullptr);
            if(SUCCEEDED(hr))hr=device->GetRenderTargetData(target,readback);
            D3DLOCKED_RECT lock{};if(SUCCEEDED(hr))hr=readback->LockRect(&lock,nullptr,D3DLOCK_READONLY);
            bool ok=SUCCEEDED(hr)&&lock.pBits&&(*(DWORD*)lock.pBits&0xffffff)==0x4080c0;
            check(ok,"D3D9 hardware pixel readback",hr);
            if(SUCCEEDED(hr))readback->UnlockRect();
            if(readback)readback->Release();if(target)target->Release();device->Release();
        }
        api->Release();
    }
    DestroyWindow(window);
    for(int i=1;i+1<argc;i++)if(!wcscmp(argv[i],L"--child-program"))check(run_child(argv[i+1],!expect_wine),"child inherits DLL choice across architectures and custom environment");
    printf("%s graphics %zu-bit: %d failure(s)\n",expect_wine?"Wine":"Native",sizeof(void*)*8,failures);
    return failures?1:0;
}
