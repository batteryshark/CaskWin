// SPDX-License-Identifier: LGPL-2.1-or-later
#define WIN32_LEAN_AND_MEAN
#define DIRECTDRAW_VERSION 0x0700
#include <windows.h>
#include <ddraw.h>
#include <d3d9.h>
#include <cstdio>
static int failures;
static void check(bool ok,const char *name,HRESULT hr=S_OK){printf("%s %s (0x%08lx)\n",ok?"PASS":"FAIL",name,hr);if(!ok)++failures;}
struct EnumMode {DWORD width;bool found;};
static HRESULT WINAPI enum_mode(DDSURFACEDESC2 *mode,void *context){auto result=(EnumMode*)context;if(mode->dwWidth==result->width&&mode->ddpfPixelFormat.dwRGBBitCount==16)result->found=true;return DDENUMRET_OK;}
int wmain(int argc,wchar_t**) {
    DEVMODEW original{};original.dmSize=sizeof(original);
    check(EnumDisplaySettingsExW(nullptr,ENUM_CURRENT_SETTINGS,&original,0)!=FALSE,"current desktop query");
    DWORD count=0,depth16=0,depth32=0;bool enumerations_agree=true;DEVMODEW mode{};mode.dmSize=sizeof(mode);
    while(count<32768&&EnumDisplaySettingsExW(nullptr,count,&mode,0)){
        if(mode.dmBitsPerPel==16)depth16++;if(mode.dmBitsPerPel==32)depth32++;
        DEVMODEA a{};a.dmSize=sizeof(a);
        if(!EnumDisplaySettingsExA(nullptr,count,&a,0)||a.dmBitsPerPel!=mode.dmBitsPerPel||a.dmPelsWidth!=mode.dmPelsWidth||a.dmPelsHeight!=mode.dmPelsHeight)enumerations_agree=false;
        ++count;
    }
    check(enumerations_agree,"ANSI/Unicode mode enumeration agree");
    printf("DISPLAY %lu %lu %lu %lu %lu %lu\n",original.dmPelsWidth,original.dmPelsHeight,original.dmBitsPerPel,count,depth16,depth32);
    if(argc<2)return failures?1:0;
    check(depth16&&depth32,"16-bit modes accompany real 32-bit resolutions");
    HMODULE draw=LoadLibraryW(L"ddraw.dll"),d3d=LoadLibraryW(L"d3d9.dll");
    check(draw&&d3d&&GetModuleHandleW(L"wined3d.dll"),"real Wine graphics providers loaded");
    if(!draw||!d3d)return 1;
    using CreateDraw=HRESULT(WINAPI*)(GUID*,void**,REFIID,IUnknown*);
    auto create_draw=(CreateDraw)GetProcAddress(draw,"DirectDrawCreateEx");IDirectDraw7 *dd=nullptr;
    HRESULT hr=create_draw(nullptr,(void**)&dd,IID_IDirectDraw7,nullptr);check(SUCCEEDED(hr),"Wine DirectDraw creation",hr);
    if(!dd)return 1;
    EnumMode selected{original.dmPelsWidth,false};hr=dd->EnumDisplayModes(0,nullptr,&selected,enum_mode);
    check(SUCCEEDED(hr)&&selected.found,"DirectDraw exposes a 16-bit mode at a supported resolution",hr);
    EnumMode legacy{640,false};dd->EnumDisplayModes(0,nullptr,&legacy,enum_mode);
    printf("Host supplies a 640-wide mode: %s\n",legacy.found?"yes":"no (virtual resolutions still needed)");
    DEVMODEW requested=original;requested.dmBitsPerPel=16;
    LONG change=ChangeDisplaySettingsExW(nullptr,&requested,nullptr,CDS_TEST,nullptr);
    check(change==DISP_CHANGE_SUCCESSFUL,"16-bit mode passes validation",change);
    EnumDisplaySettingsW(nullptr,ENUM_CURRENT_SETTINGS,&mode);
    check(mode.dmBitsPerPel==original.dmBitsPerPel,"CDS_TEST does not apply the virtual depth");
    check(ChangeDisplaySettingsW(&requested,CDS_UPDATEREGISTRY)==DISP_CHANGE_BADFLAGS,"persistent host display changes are rejected");
    change=ChangeDisplaySettingsW(&requested,0);check(change==DISP_CHANGE_SUCCESSFUL,"apply process-local 16-bit depth",change);
    EnumDisplaySettingsW(nullptr,ENUM_CURRENT_SETTINGS,&mode);check(mode.dmBitsPerPel==16,"current depth reflects selection");
    DEVMODEA current_a{};current_a.dmSize=sizeof(current_a);EnumDisplaySettingsA(nullptr,ENUM_CURRENT_SETTINGS,&current_a);
    check(current_a.dmBitsPerPel==16,"ANSI current depth agrees");
    HDC screen=GetDC(nullptr);check(GetDeviceCaps(screen,BITSPIXEL)==16,"GDI screen depth agrees");ReleaseDC(nullptr,screen);
    DDSURFACEDESC2 display{};display.dwSize=sizeof(display);hr=dd->GetDisplayMode(&display);
    check(SUCCEEDED(hr)&&display.ddpfPixelFormat.dwRGBBitCount==16,"Wine DirectDraw sees selected depth",hr);
    HWND window=CreateWindowExW(0,L"STATIC",L"Winedows 16-bit renderer test",WS_OVERLAPPEDWINDOW,50,50,160,120,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    if(GetEnvironmentVariableW(L"WINEDOWS_DESKTOP_HWND",nullptr,0)){ShowWindow(window,SW_SHOW);check(IsChild(GetDesktopWindow(),window)!=FALSE,"visible rendering window is contained");}
    hr=dd->SetCooperativeLevel(window,DDSCL_NORMAL);check(SUCCEEDED(hr),"DirectDraw cooperative level",hr);
    DDSURFACEDESC2 desc{};desc.dwSize=sizeof(desc);desc.dwFlags=DDSD_WIDTH|DDSD_HEIGHT|DDSD_CAPS|DDSD_PIXELFORMAT;
    desc.dwWidth=32;desc.dwHeight=32;desc.ddsCaps.dwCaps=DDSCAPS_OFFSCREENPLAIN|DDSCAPS_SYSTEMMEMORY;
    desc.ddpfPixelFormat={sizeof(DDPIXELFORMAT),DDPF_RGB,0,16,0xf800,0x7e0,0x1f,0};
    IDirectDrawSurface7 *surface=nullptr;hr=dd->CreateSurface(&desc,&surface,nullptr);check(SUCCEEDED(hr),"RGB565 DirectDraw surface",hr);
    if(surface){
        DDBLTFX fx{};fx.dwSize=sizeof(fx);fx.dwFillColor=0xf800;
        hr=surface->Blt(nullptr,nullptr,nullptr,DDBLT_COLORFILL|DDBLT_WAIT,&fx);DDSURFACEDESC2 locked{};locked.dwSize=sizeof(locked);
        if(SUCCEEDED(hr))hr=surface->Lock(nullptr,&locked,DDLOCK_READONLY|DDLOCK_WAIT,nullptr);
        check(SUCCEEDED(hr)&&locked.lpSurface&&*(WORD*)locked.lpSurface==0xf800,"16-bit fill and readback",hr);
        if(SUCCEEDED(hr))surface->Unlock(nullptr);surface->Release();
    }
    using CreateD3D=IDirect3D9*(WINAPI*)(UINT);auto create=(CreateD3D)GetProcAddress(d3d,"Direct3DCreate9");IDirect3D9 *api=create(D3D_SDK_VERSION);
    check(api!=nullptr,"Wine D3D9 creation");
    if(api){
        D3DPRESENT_PARAMETERS pp{};pp.Windowed=TRUE;pp.SwapEffect=D3DSWAPEFFECT_DISCARD;pp.hDeviceWindow=window;
        pp.BackBufferWidth=32;pp.BackBufferHeight=32;pp.BackBufferFormat=D3DFMT_R5G6B5;
        IDirect3DDevice9 *device=nullptr;hr=api->CreateDevice(0,D3DDEVTYPE_HAL,window,D3DCREATE_SOFTWARE_VERTEXPROCESSING,&pp,&device);
        check(SUCCEEDED(hr),"hardware WineD3D RGB565 rendering device",hr);
        if(device){
            hr=device->Clear(0,nullptr,D3DCLEAR_TARGET,D3DCOLOR_XRGB(255,0,0),1.0f,0);
            IDirect3DSurface9 *target=nullptr,*readback=nullptr;
            if(SUCCEEDED(hr))hr=device->GetRenderTarget(0,&target);
            if(SUCCEEDED(hr))hr=device->CreateOffscreenPlainSurface(32,32,D3DFMT_R5G6B5,D3DPOOL_SYSTEMMEM,&readback,nullptr);
            if(SUCCEEDED(hr))hr=device->GetRenderTargetData(target,readback);
            D3DLOCKED_RECT lock{};if(SUCCEEDED(hr))hr=readback->LockRect(&lock,nullptr,D3DLOCK_READONLY);
            check(SUCCEEDED(hr)&&lock.pBits&&*(WORD*)lock.pBits==0xf800,"hardware RGB565 pixel readback",hr);
            if(SUCCEEDED(hr))readback->UnlockRect();if(readback)readback->Release();if(target)target->Release();
            hr=device->Present(nullptr,nullptr,nullptr,nullptr);check(SUCCEEDED(hr),"16-bit render target presents on native desktop",hr);
            device->Release();
        }
        api->Release();
    }
    dd->Release();DestroyWindow(window);
    current_a.dmBitsPerPel=32;change=ChangeDisplaySettingsExA(nullptr,&current_a,nullptr,0,nullptr);
    check(change==DISP_CHANGE_SUCCESSFUL,"ANSI mode change restores 32-bit view",change);
    EnumDisplaySettingsW(nullptr,ENUM_CURRENT_SETTINGS,&mode);check(mode.dmBitsPerPel==32,"32-bit depth restored");
    ChangeDisplaySettingsW(nullptr,0);
    EnumDisplaySettingsW(nullptr,ENUM_CURRENT_SETTINGS,&mode);
    check(mode.dmPelsWidth==original.dmPelsWidth&&mode.dmPelsHeight==original.dmPelsHeight&&mode.dmBitsPerPel==original.dmBitsPerPel,"reset restores the initial desktop, independent of host saved settings");
    printf("Display compatibility %zu-bit: %d failure(s)\n",sizeof(void*)*8,failures);return failures?1:0;
}
