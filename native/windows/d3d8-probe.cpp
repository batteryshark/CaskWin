// SPDX-License-Identifier: LGPL-2.1-or-later
// Render and read real D3D8 pixels through the selected Wine/native provider.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d8.h>
#include <GL/gl.h>
#include <cstdio>
#include <cstring>
#include <cctype>
#include <string>
#include <algorithm>

static int failures;
static DWORD WINAPI timeout_thread(void *) {
    Sleep(45000);
    fputs("FAIL D3D8 fixture exceeded its 45-second deadline\n",stderr);
    TerminateProcess(GetCurrentProcess(),124);
    return 124;
}
static void check(bool ok,const char *name,HRESULT hr=S_OK) {
    printf("%s %s (hr=%08lx)\n",ok?"PASS":"FAIL",name,(unsigned long)hr);
    if(!ok)++failures;
}
static std::wstring module_path(HMODULE module) {
    wchar_t path[32768]{};DWORD n=GetModuleFileNameW(module,path,32768);
    return n&&n<32768?std::wstring(path,n):std::wstring();
}
#ifdef WINEDOWS_STATIC_D3D8_IMPORT
static bool imported_from(HMODULE provider) {
    BYTE *base=(BYTE*)GetModuleHandleW(nullptr);
    auto dos=(IMAGE_DOS_HEADER*)base;auto pe=(IMAGE_NT_HEADERS*)(base+dos->e_lfanew);
    auto imports=(IMAGE_IMPORT_DESCRIPTOR*)(base+pe->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress);
    for(;imports->Name;++imports)if(!_stricmp((char*)base+imports->Name,"d3d8.dll")) {
        if(!imports->OriginalFirstThunk)return false;
        auto names=(IMAGE_THUNK_DATA*)(base+imports->OriginalFirstThunk);
        auto bindings=(IMAGE_THUNK_DATA*)(base+imports->FirstThunk);
        for(;names->u1.AddressOfData;++names,++bindings) {
            if(IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal))continue;
            auto entry=(IMAGE_IMPORT_BY_NAME*)(base+names->u1.AddressOfData);
            if(!strcmp((char*)entry->Name,"Direct3DCreate8"))return (FARPROC)bindings->u1.Function==GetProcAddress(provider,"Direct3DCreate8");
        }
    }
    return false;
}
#endif
static bool native_gl_hardware() {
    HWND window=CreateWindowExW(0,L"STATIC",L"D3D8 OpenGL driver evidence",0,0,0,32,32,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    HDC dc=window?GetDC(window):nullptr;HGLRC context=nullptr;bool hardware=false;
    if(dc) {
        PIXELFORMATDESCRIPTOR format{};format.nSize=sizeof(format);format.nVersion=1;
        format.dwFlags=PFD_DRAW_TO_WINDOW|PFD_SUPPORT_OPENGL;format.iPixelType=PFD_TYPE_RGBA;format.cColorBits=24;
        int pixel=ChoosePixelFormat(dc,&format);
        if(pixel&&SetPixelFormat(dc,pixel,&format))context=wglCreateContext(dc);
        if(context&&wglMakeCurrent(dc,context)) {
            const char *renderer=(const char*)glGetString(GL_RENDERER),*version=(const char*)glGetString(GL_VERSION);
            printf("OpenGL renderer: %s; version: %s\n",renderer?renderer:"unavailable",version?version:"unavailable");
            if(renderer) {
                std::string name=renderer;std::transform(name.begin(),name.end(),name.begin(),[](unsigned char c){return (char)tolower(c);});
                hardware=name.find("gdi generic")==name.npos&&name.find("llvmpipe")==name.npos&&name.find("softpipe")==name.npos&&name.find("software rasterizer")==name.npos&&name.find("basic render")==name.npos;
            }
            wglMakeCurrent(nullptr,nullptr);
        }
        if(context)wglDeleteContext(context);
        ReleaseDC(window,dc);
    }
    if(window)DestroyWindow(window);
    return hardware;
}
static void render_and_read(IDirect3DDevice8 *device) {
    HRESULT hr=device->Clear(0,nullptr,D3DCLEAR_TARGET,D3DCOLOR_XRGB(16,32,48),1.0f,0);
    check(SUCCEEDED(hr),"D3D8 render-target clear",hr);
    struct Vertex {float x,y,z,rhw;DWORD color;};
    const Vertex triangle[]={{4.0f,4.0f,0.5f,1.0f,0xffff0000},{60.0f,4.0f,0.5f,1.0f,0xffff0000},{32.0f,60.0f,0.5f,1.0f,0xffff0000}};
    hr=device->SetVertexShader(D3DFVF_XYZRHW|D3DFVF_DIFFUSE);
    if(SUCCEEDED(hr))hr=device->SetPixelShader(0);
    if(SUCCEEDED(hr))hr=device->SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE);
    if(SUCCEEDED(hr))hr=device->SetRenderState(D3DRS_LIGHTING,FALSE);
    if(SUCCEEDED(hr))hr=device->SetRenderState(D3DRS_ZENABLE,FALSE);
    if(SUCCEEDED(hr))hr=device->SetTextureStageState(0,D3DTSS_COLOROP,D3DTOP_SELECTARG1);
    if(SUCCEEDED(hr))hr=device->SetTextureStageState(0,D3DTSS_COLORARG1,D3DTA_DIFFUSE);
    if(SUCCEEDED(hr))hr=device->SetTextureStageState(0,D3DTSS_ALPHAOP,D3DTOP_DISABLE);
    check(SUCCEEDED(hr),"D3D8 fixed-function pipeline setup",hr);
    if(SUCCEEDED(hr))hr=device->BeginScene();
    if(SUCCEEDED(hr)) {
        HRESULT drawn=device->DrawPrimitiveUP(D3DPT_TRIANGLELIST,1,triangle,sizeof(Vertex));
        HRESULT ended=device->EndScene();hr=FAILED(drawn)?drawn:ended;
    }
    check(SUCCEEDED(hr),"D3D8 triangle draw",hr);
    IDirect3DSurface8 *target=nullptr,*readback=nullptr;
    hr=device->GetRenderTarget(&target);
    if(SUCCEEDED(hr))hr=device->CreateImageSurface(64,64,D3DFMT_X8R8G8B8,&readback);
    if(SUCCEEDED(hr))hr=device->CopyRects(target,nullptr,0,readback,nullptr);
    D3DLOCKED_RECT locked{};if(SUCCEEDED(hr))hr=readback->LockRect(&locked,nullptr,D3DLOCK_READONLY);
    bool triangle_ok=false,background_ok=false;
    if(SUCCEEDED(hr)&&locked.pBits&&locked.Pitch>=64*4) {
        const auto pixel=[&](unsigned x,unsigned y){return *(const DWORD*)((const BYTE*)locked.pBits+y*locked.Pitch+x*4)&0xffffff;};
        DWORD center=pixel(32,24),corner=pixel(1,1);
        printf("Readback pixels: triangle=%06lx background=%06lx\n",(unsigned long)center,(unsigned long)corner);
        triangle_ok=center==0xff0000;background_ok=corner==0x102030;
    }
    check(triangle_ok,"D3D8 hardware triangle pixel readback",hr);
    check(background_ok,"D3D8 clear survives outside triangle",hr);
    if(SUCCEEDED(hr))readback->UnlockRect();
    if(readback)readback->Release();
    if(target)target->Release();
    hr=device->Present(nullptr,nullptr,nullptr,nullptr);
    check(SUCCEEDED(hr),"D3D8 rendered frame presentation",hr);
}
int wmain(int argc,wchar_t **argv) {
    setvbuf(stdout,nullptr,_IONBF,0);
    HANDLE watchdog=CreateThread(nullptr,0,timeout_thread,nullptr,0,nullptr);
    if(watchdog)CloseHandle(watchdog);
    bool native=false;for(int i=1;i<argc;++i)if(!wcscmp(argv[i],L"--native"))native=true;
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
    DEVMODEW before{};before.dmSize=sizeof(before);
    check(EnumDisplaySettingsW(nullptr,ENUM_CURRENT_SETTINGS,&before)!=FALSE,"starting display mode");
    HMODULE module=LoadLibraryW(L"d3d8.dll"),wine=GetModuleHandleW(L"wined3d.dll");
    check(module!=nullptr,"load Direct3D8 provider",module?S_OK:HRESULT_FROM_WIN32(GetLastError()));
    if(!module)return 1;
    std::wstring path=module_path(module);wprintf(L"Module: %ls\n",path.c_str());
    if(native)check(!wine,"native D3D8 fallback does not load WineD3D");
    else {
        std::wstring backend=wine?module_path(wine):L"";wprintf(L"WineD3D: %ls\n",backend.c_str());
        size_t slash=backend.find_last_of(L"\\/");
        std::wstring directory=slash==backend.npos?L"":backend.substr(0,slash+1);
        check(wine&&GetProcAddress(wine,"wined3d_get_renderer")&&!directory.empty()&&!_wcsnicmp(path.c_str(),directory.c_str(),directory.size()),"actual selected Wine D3D8 and WineD3D providers");
        check(native_gl_hardware(),"native hardware OpenGL driver for WineD3D");
    }
    using Create=IDirect3D8*(WINAPI*)(UINT);
#ifdef WINEDOWS_STATIC_D3D8_IMPORT
    Create create=&Direct3DCreate8;check(imported_from(module),"startup D3D8 import binds selected provider");
#else
    auto create=(Create)GetProcAddress(module,"Direct3DCreate8");
#endif
    IDirect3D8 *api=create?create(D3D_SDK_VERSION):nullptr;check(api!=nullptr,"Direct3D8 interface creation");
    if(!api){FreeLibrary(module);return 1;}
    D3DADAPTER_IDENTIFIER8 id{};HRESULT hr=api->GetAdapterIdentifier(D3DADAPTER_DEFAULT,0,&id);
    check(SUCCEEDED(hr),"D3D8 adapter identification",hr);
    if(SUCCEEDED(hr))printf("D3D8 adapter: %s; driver=%s; vendor=%04lx device=%04lx\n",id.Description,id.Driver,(unsigned long)id.VendorId,(unsigned long)id.DeviceId);
    HWND window=CreateWindowExW(0,L"STATIC",L"Winedows D3D8 rendering probe",WS_OVERLAPPEDWINDOW,40,40,192,160,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    check(window!=nullptr,"rendering window creation");
    if(!window){api->Release();FreeLibrary(module);return 1;}
    ShowWindow(window,SW_SHOWNOACTIVATE);UpdateWindow(window);
    bool desktop=GetEnvironmentVariableW(L"WINEDOWS_DESKTOP_HWND",nullptr,0)!=0;
    if(desktop)check(IsChild(GetDesktopWindow(),window)!=FALSE,"visible D3D8 window contained in virtual desktop");
    D3DPRESENT_PARAMETERS present{};present.Windowed=TRUE;present.SwapEffect=D3DSWAPEFFECT_DISCARD;present.hDeviceWindow=window;
    present.BackBufferWidth=64;present.BackBufferHeight=64;present.BackBufferFormat=D3DFMT_X8R8G8B8;present.BackBufferCount=1;
    IDirect3DDevice8 *device=nullptr;hr=api->CreateDevice(0,D3DDEVTYPE_HAL,window,D3DCREATE_SOFTWARE_VERTEXPROCESSING,&present,&device);
    check(SUCCEEDED(hr),"D3D8 HAL rendering device",hr);
    if(device) {
        D3DCAPS8 caps{};hr=device->GetDeviceCaps(&caps);check(SUCCEEDED(hr)&&caps.DeviceType==D3DDEVTYPE_HAL,"D3D8 device reports HAL rasterization",hr);
        render_and_read(device);device->Release();
    }
    DestroyWindow(window);api->Release();FreeLibrary(module);
    DEVMODEW after{};after.dmSize=sizeof(after);
    bool unchanged=EnumDisplaySettingsW(nullptr,ENUM_CURRENT_SETTINGS,&after)&&before.dmPelsWidth==after.dmPelsWidth&&before.dmPelsHeight==after.dmPelsHeight&&before.dmBitsPerPel==after.dmBitsPerPel&&before.dmDisplayFrequency==after.dmDisplayFrequency;
    check(unchanged,"windowed D3D8 leaves display mode unchanged");
    printf("%s D3D8 %zu-bit: %d failure(s)\n",native?"Native":"Wine",sizeof(void*)*8,failures);
    return failures?1:0;
}
