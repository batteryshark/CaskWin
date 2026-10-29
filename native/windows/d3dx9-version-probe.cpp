// SPDX-License-Identifier: LGPL-2.1-or-later
// Versioned helper regression derived from d3dx9-probe.cpp.
// Compile once per actual upstream SDK version; never alias or rename a DLL.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d9.h>
#include <d3dx9.h>
#include <d3dcompiler.h>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#ifndef WINEDOWS_D3DX_VERSION
#error Set WINEDOWS_D3DX_VERSION to the actual D3DX SDK module version
#endif
#define WD_STRING_INNER(x) #x
#define WD_STRING(x) WD_STRING_INNER(x)
#define WD_WIDE_INNER(x) L##x
#define WD_WIDE(x) WD_WIDE_INNER(x)
#define WD_D3DX_NAME "d3dx9_" WD_STRING(WINEDOWS_D3DX_VERSION) ".dll"
static const wchar_t module_name[]=L"d3dx9_" WD_WIDE(WD_STRING(WINEDOWS_D3DX_VERSION)) L".dll";
static int failures;
static void check(bool ok,const char *message,HRESULT result=S_OK) {
    printf("%s %s (hr=%08lx)\n",ok?"PASS":"FAIL",message,(unsigned long)result);
    if(!ok)++failures;
}
static DWORD WINAPI watchdog(void *) {
    Sleep(60000);fputs("FAIL D3DX9 fixture exceeded 60 seconds\n",stderr);
    TerminateProcess(GetCurrentProcess(),124);return 124;
}
static std::wstring module_path(HMODULE module) {
    std::vector<wchar_t> path(32768);DWORD size=module?GetModuleFileNameW(module,path.data(),(DWORD)path.size()):0;
    return size&&size<path.size()?std::wstring(path.data(),size):std::wstring();
}
static bool close_float(float a,float b){return std::fabs(a-b)<0.0001f;}
static bool same_file(const std::wstring &a,const std::wstring &b) {
    HANDLE first=CreateFileW(a.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr);
    HANDLE second=CreateFileW(b.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr);
    BY_HANDLE_FILE_INFORMATION left{},right{};
    bool same=first!=INVALID_HANDLE_VALUE&&second!=INVALID_HANDLE_VALUE&&GetFileInformationByHandle(first,&left)&&GetFileInformationByHandle(second,&right)&&
        left.dwVolumeSerialNumber==right.dwVolumeSerialNumber&&left.nFileIndexHigh==right.nFileIndexHigh&&left.nFileIndexLow==right.nFileIndexLow;
    if(first!=INVALID_HANDLE_VALUE)CloseHandle(first);if(second!=INVALID_HANDLE_VALUE)CloseHandle(second);return same;
}

#define D3DX_FUNCTIONS(X) \
    X(D3DXCheckVersion) X(D3DXVec3Normalize) X(D3DXVec3TransformCoord) \
    X(D3DXMatrixScaling) X(D3DXMatrixTranslation) X(D3DXMatrixMultiply) X(D3DXMatrixInverse) \
    X(D3DXCreateBuffer) X(D3DXGetShaderConstantTable) X(D3DXGetShaderInputSemantics) X(D3DXGetShaderOutputSemantics) \
    X(D3DXCompileShader) X(D3DXGetShaderVersion) X(D3DXGetShaderSize) \
    X(D3DXGetImageInfoFromFileInMemory) X(D3DXCreateTextureFromFileInMemoryEx) X(D3DXLoadSurfaceFromMemory)
struct Functions {
#define DECLARE(name) decltype(&name) name=nullptr;
    D3DX_FUNCTIONS(DECLARE)
#undef DECLARE
    bool bind(HMODULE module) {
        bool valid=true;
#ifdef WINEDOWS_STATIC_D3DX9_IMPORT
#define BIND(name) name=&::name; if(!name)valid=false;
#else
#define BIND(name) name=(decltype(name))GetProcAddress(module,#name); if(!name)valid=false;
#endif
        D3DX_FUNCTIONS(BIND)
#undef BIND
        (void)module;return valid;
    }
};

#ifdef WINEDOWS_STATIC_D3DX9_IMPORT
static bool imports_from(HMODULE provider) {
    BYTE *base=(BYTE*)GetModuleHandleW(nullptr);auto dos=(IMAGE_DOS_HEADER*)base;
    auto pe=(IMAGE_NT_HEADERS*)(base+dos->e_lfanew);
    auto imports=(IMAGE_IMPORT_DESCRIPTOR*)(base+pe->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress);
    unsigned found=0;
    for(;imports->Name;++imports)if(!_stricmp((const char*)base+imports->Name,WD_D3DX_NAME)) {
        if(!imports->OriginalFirstThunk)return false;
        auto names=(IMAGE_THUNK_DATA*)(base+imports->OriginalFirstThunk),slots=(IMAGE_THUNK_DATA*)(base+imports->FirstThunk);
        for(;names->u1.AddressOfData;++names,++slots) {
            FARPROC expected;
            if(IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal))expected=GetProcAddress(provider,MAKEINTRESOURCEA(IMAGE_ORDINAL(names->u1.Ordinal)));
            else expected=GetProcAddress(provider,(LPCSTR)((IMAGE_IMPORT_BY_NAME*)(base+names->u1.AddressOfData))->Name);
            if(!expected||(FARPROC)slots->u1.Function!=expected)return false;
            ++found;
        }
    }
    printf("Verified D3DX startup imports: %u\n",found);return found>=10;
}
#endif

static void math(Functions &f) {
    check(f.D3DXCheckVersion(D3D_SDK_VERSION,WINEDOWS_D3DX_VERSION)!=FALSE,"exact module SDK version accepted");
    check(f.D3DXCheckVersion(D3D_SDK_VERSION,43)==FALSE,"SDK43 rejected by older versioned module");
    D3DXVECTOR3 v(3,4,0),normalized;
    check(f.D3DXVec3Normalize(&normalized,&v)==&normalized&&close_float(normalized.x,0.6f)&&close_float(normalized.y,0.8f)&&close_float(normalized.z,0),"vector normalization computes actual values");
    D3DXMATRIX scale,translation,combined,inverse,identity;
    f.D3DXMatrixScaling(&scale,2,3,4);f.D3DXMatrixTranslation(&translation,5,-4,3);f.D3DXMatrixMultiply(&combined,&scale,&translation);
    D3DXVECTOR3 point(1,2,3),transformed;
    f.D3DXVec3TransformCoord(&transformed,&point,&combined);
    check(close_float(transformed.x,7)&&close_float(transformed.y,2)&&close_float(transformed.z,15),"matrix composition and coordinate transform");
    float determinant=0;auto result=f.D3DXMatrixInverse(&inverse,&determinant,&combined);
    bool correct=result==&inverse&&close_float(determinant,24);
    if(result){f.D3DXMatrixMultiply(&identity,&combined,&inverse);for(unsigned y=0;y<4;y++)for(unsigned x=0;x<4;x++)correct=correct&&close_float(identity.m[y][x],x==y?1.0f:0.0f);}
    check(correct,"matrix inverse and determinant round trip");
    D3DXMATRIX singular;for(auto &row:singular.m)for(float &value:row)value=0;
    check(f.D3DXMatrixInverse(&inverse,nullptr,&singular)==nullptr,"singular matrix inverse fails cleanly");
}
static const char shared_source[]="float4 apply_tint(float4 value) { return value * float4(TINT,1,1,1); }\n";
struct Include : ID3DXInclude {
    unsigned opened=0,closed=0;
    HRESULT STDMETHODCALLTYPE Open(D3DXINCLUDE_TYPE type,LPCSTR name,LPCVOID,LPCVOID *data,UINT *size) override {
        if(type!=D3DXINC_LOCAL||strcmp(name,"game-shader.h"))return E_FAIL;
        *data=shared_source;*size=sizeof(shared_source)-1;++opened;return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Close(LPCVOID data) override {if(data!=shared_source)return E_FAIL;++closed;return S_OK;}
};
static void semantics(Functions &f) {
    const char source[]="struct V { float4 p:POSITION0; float2 uv:TEXCOORD1; }; V main(V i) { return i; }";
    ID3DXBuffer *code=nullptr,*errors=nullptr;
    HRESULT hr=f.D3DXCompileShader(source,sizeof(source)-1,nullptr,nullptr,"main","vs_3_0",0,&code,&errors,nullptr);
    check(SUCCEEDED(hr)&&code,"compile shader with distinct vertex input/output semantics",hr);
    if(errors){printf("Semantics diagnostics: %.*s\n",(int)errors->GetBufferSize(),(char*)errors->GetBufferPointer());errors->Release();}
    if(code){
        for(bool output:{false,true}){
            auto query=output?f.D3DXGetShaderOutputSemantics:f.D3DXGetShaderInputSemantics;
            UINT count=0,filled=0;D3DXSEMANTIC values[16]{};
            hr=query((DWORD*)code->GetBufferPointer(),nullptr,&count);
            if(SUCCEEDED(hr)&&count<=16)hr=query((DWORD*)code->GetBufferPointer(),values,&filled);
            bool position=false,uv=false;
            for(UINT i=0;i<filled&&i<16;i++){position|=values[i].Usage==D3DDECLUSAGE_POSITION&&values[i].UsageIndex==0;uv|=values[i].Usage==D3DDECLUSAGE_TEXCOORD&&values[i].UsageIndex==1;}
            check(SUCCEEDED(hr)&&count==2&&filled==2&&position&&uv,output?"actual vertex output semantics preserve POSITION0 and TEXCOORD1":"actual vertex input semantics preserve POSITION0 and TEXCOORD1",hr);
        }
        ID3DXBuffer *copy=nullptr;hr=f.D3DXCreateBuffer(code->GetBufferSize(),&copy);
        bool valid=SUCCEEDED(hr)&&copy&&copy->GetBufferSize()==code->GetBufferSize();
        if(valid){memcpy(copy->GetBufferPointer(),code->GetBufferPointer(),code->GetBufferSize());valid=f.D3DXGetShaderVersion((DWORD*)copy->GetBufferPointer())==D3DVS_VERSION(3,0);}
        check(valid,"D3DXCreateBuffer stores real shader bytecode",hr);
        if(copy)copy->Release();code->Release();
    }
}
static ID3DXBuffer *compile_shader(Functions &f,bool swapped) {
    const char source[]="#include \"game-shader.h\"\n sampler2D image : register(s0); float4 main(float2 uv : TEXCOORD0) : COLOR0 { return apply_tint(tex2D(image, COORDS)); }";
    const D3DXMACRO macros[]={{"TINT","1.0"},{"COORDS",swapped?"uv.yx":"uv"},{nullptr,nullptr}};
    printf("Shader fixture coordinates: %s\n",swapped?"uv.yx":"uv");
    ID3DXBuffer *code=nullptr,*errors=nullptr;ID3DXConstantTable *constants=nullptr;
    Include include;
    HRESULT result=f.D3DXCompileShader(source,(UINT)strlen(source),macros,&include,"main","ps_2_0",D3DXSHADER_OPTIMIZATION_LEVEL3,&code,&errors,&constants);
    if(errors){printf("Shader diagnostics: %.*s\n",(int)errors->GetBufferSize(),(const char*)errors->GetBufferPointer());errors->Release();}
    check(SUCCEEDED(result)&&code,"HLSL shader compiles with macro preprocessing",result);
    check(include.opened==1&&include.closed==1,"real include callback opens and closes shader source exactly once");
    if(code){
        const DWORD *tokens=(const DWORD*)code->GetBufferPointer();
        check(f.D3DXGetShaderVersion(tokens)==D3DPS_VERSION(2,0),"compiled bytecode identifies pixel shader 2.0");
        check(f.D3DXGetShaderSize(tokens)==code->GetBufferSize(),"compiled bytecode size is self-consistent");
    }
    D3DXHANDLE sampler=constants?constants->GetConstantByName(nullptr,"image"):nullptr;
    check(sampler&&constants->GetSamplerIndex(sampler)==0,"compiled shader constant table exposes sampler0");
    if(constants)constants->Release();
    ID3DXConstantTable *parsed=nullptr;
    result=code?f.D3DXGetShaderConstantTable((DWORD*)code->GetBufferPointer(),&parsed):E_FAIL;
    D3DXHANDLE parsedSampler=parsed?parsed->GetConstantByName(nullptr,"image"):nullptr;
    check(SUCCEEDED(result)&&parsedSampler&&parsed->GetSamplerIndex(parsedSampler)==0,"direct constant-table API parses compiled sampler binding",result);
    if(parsed)parsed->Release();
    const char invalid[]="float4 main( : COLOR { invalid shader tokens; }";
    ID3DXBuffer *bad_code=nullptr,*diagnostic=nullptr;
    result=f.D3DXCompileShader(invalid,(UINT)strlen(invalid),nullptr,nullptr,"main","ps_2_0",0,&bad_code,&diagnostic,nullptr);
    check(FAILED(result)&&!bad_code&&diagnostic&&diagnostic->GetBufferSize()>1,"invalid HLSL yields a compilation error and diagnostics",result);
    if(bad_code)bad_code->Release();if(diagnostic)diagnostic->Release();
    return code;
}
static std::vector<DWORD> image_data() {
    std::vector<DWORD> data(32+16);data[0]=0x20534444;data[1]=124;data[2]=0x100f;
    data[3]=4;data[4]=4;data[5]=16;data[7]=1;data[19]=32;data[20]=0x41;data[22]=32;
    data[23]=0x00ff0000;data[24]=0x0000ff00;data[25]=0x000000ff;data[26]=0xff000000;data[27]=0x1000;
    for(unsigned y=0;y<4;y++)for(unsigned x=0;x<4;x++)
        data[32+y*4+x]=0xff000000|((x+1)*0x20<<16)|((y+1)*0x30<<8)|((x+4*y+1)*8);
    return data;
}
static void texture_and_render(Functions &f,ID3DXBuffer *code,bool swapped) {
    auto data=image_data();D3DXIMAGE_INFO info{};
    HRESULT result=f.D3DXGetImageInfoFromFileInMemory(data.data(),(UINT)(data.size()*4),&info);
    check(SUCCEEDED(result)&&info.Width==4&&info.Height==4&&info.Format==D3DFMT_A8R8G8B8&&info.ImageFileFormat==D3DXIFF_DDS,"DDS image metadata is decoded",result);
    const BYTE invalid[]={0,1,2,3,4,5,6,7};
    result=f.D3DXGetImageInfoFromFileInMemory(invalid,sizeof(invalid),&info);
    check(FAILED(result),"invalid image data is rejected",result);
    HMODULE d3d=LoadLibraryW(L"d3d9.dll");auto create=d3d?(decltype(&Direct3DCreate9))GetProcAddress(d3d,"Direct3DCreate9"):nullptr;
    IDirect3D9 *api=create?create(D3D_SDK_VERSION):nullptr;check(api!=nullptr,"D3D9 interface is available");
    if(!api){if(d3d)FreeLibrary(d3d);return;}
    wprintf(L"D3D9 provider: %ls\n",module_path(d3d).c_str());
    HWND window=CreateWindowExW(0,L"STATIC",L"Winedows D3DX9 shader and texture test",WS_OVERLAPPEDWINDOW,40,40,192,160,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    check(window!=nullptr,"create rendering window");if(!window){api->Release();FreeLibrary(d3d);return;}
    ShowWindow(window,SW_SHOWNOACTIVATE);UpdateWindow(window);
    wchar_t desktop[64]{};GetEnvironmentVariableW(L"WINEDOWS_DESKTOP_HWND",desktop,64);
    if(*desktop)check(IsChild(GetDesktopWindow(),window)!=FALSE,"D3DX rendering window remains in virtual desktop");
    D3DPRESENT_PARAMETERS present{};present.Windowed=TRUE;present.SwapEffect=D3DSWAPEFFECT_DISCARD;present.hDeviceWindow=window;
    present.BackBufferWidth=64;present.BackBufferHeight=64;present.BackBufferFormat=D3DFMT_X8R8G8B8;present.PresentationInterval=D3DPRESENT_INTERVAL_IMMEDIATE;
    IDirect3DDevice9 *device=nullptr;result=api->CreateDevice(0,D3DDEVTYPE_HAL,window,D3DCREATE_SOFTWARE_VERTEXPROCESSING,&present,&device);
    check(SUCCEEDED(result)&&device,"D3D9 HAL device for D3DX output",result);
    if(device){
        D3DCAPS9 caps{};result=device->GetDeviceCaps(&caps);check(SUCCEEDED(result)&&caps.DeviceType==D3DDEVTYPE_HAL&&caps.PixelShaderVersion>=D3DPS_VERSION(2,0),"HAL device supports the compiled pixel shader",result);
        IDirect3DTexture9 *texture=nullptr;
        result=f.D3DXCreateTextureFromFileInMemoryEx(device,data.data(),(UINT)(data.size()*4),4,4,1,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,D3DX_FILTER_NONE,D3DX_FILTER_NONE,0,&info,nullptr,&texture);
        check(SUCCEEDED(result)&&texture,"D3DX creates a texture from encoded DDS pixels",result);
        if(texture){
            D3DLOCKED_RECT lock{};result=texture->LockRect(0,&lock,nullptr,D3DLOCK_READONLY);bool pixels=SUCCEEDED(result)&&lock.pBits&&lock.Pitch>=16;
            if(pixels)for(unsigned y=0;y<4;y++)for(unsigned x=0;x<4;x++)pixels=pixels&&*(DWORD*)((BYTE*)lock.pBits+y*lock.Pitch+x*4)==data[32+y*4+x];
            check(pixels,"decoded texture contains all 16 exact source pixels",result);if(SUCCEEDED(result))texture->UnlockRect(0);
        }
        IDirect3DSurface9 *converted=nullptr;result=device->CreateOffscreenPlainSurface(2,2,D3DFMT_R5G6B5,D3DPOOL_SYSTEMMEM,&converted,nullptr);
        if(SUCCEEDED(result)){
            DWORD argb[]={0xffff0000,0xff00ff00,0xff0000ff,0xffffffff};RECT rect{0,0,2,2};
            result=f.D3DXLoadSurfaceFromMemory(converted,nullptr,nullptr,argb,D3DFMT_A8R8G8B8,8,nullptr,&rect,D3DX_FILTER_NONE,0);
            D3DLOCKED_RECT lock{};if(SUCCEEDED(result))result=converted->LockRect(&lock,nullptr,D3DLOCK_READONLY);
            bool exact=SUCCEEDED(result)&&lock.pBits&&lock.Pitch>=4;
            const WORD expected[]={0xf800,0x07e0,0x001f,0xffff};
            if(exact)for(unsigned y=0;y<2;y++)for(unsigned x=0;x<2;x++)exact=exact&&*(WORD*)((BYTE*)lock.pBits+y*lock.Pitch+x*2)==expected[y*2+x];
            check(exact,"D3DX converts ARGB pixels to exact RGB565 values",result);
            if(SUCCEEDED(result))converted->UnlockRect();converted->Release();
        }else check(false,"create conversion surface",result);
        IDirect3DPixelShader9 *shader=nullptr;
        result=code?device->CreatePixelShader((DWORD*)code->GetBufferPointer(),&shader):E_FAIL;
        check(SUCCEEDED(result)&&shader,"D3D9 accepts D3DX-compiled shader bytecode",result);
        if(FAILED(result)&&code){
            HMODULE compiler=LoadLibraryW(L"d3dcompiler_47.dll");
            auto disassemble=compiler?(decltype(&D3DDisassemble))GetProcAddress(compiler,"D3DDisassemble"):nullptr;
            ID3DBlob *assembly=nullptr;
            HRESULT decoded=disassemble?disassemble(code->GetBufferPointer(),code->GetBufferSize(),0,nullptr,&assembly):E_FAIL;
            printf("Native disassembly result: %08lx\n",(unsigned long)decoded);
            if(assembly){printf("Rejected shader:\n%.*s\n",(int)assembly->GetBufferSize(),(const char*)assembly->GetBufferPointer());assembly->Release();}
            const DWORD *words=(const DWORD*)code->GetBufferPointer();printf("Shader words:");for(unsigned i=0;i<code->GetBufferSize()/4;i++)printf(" %08lx",(unsigned long)words[i]);puts("");
            if(compiler)FreeLibrary(compiler);
        }
        if(texture&&shader){
            struct Vertex{float x,y,z,rhw,u,v;};
            const Vertex quad[]={{7.5f,7.5f,0.5f,1,0,0},{55.5f,7.5f,0.5f,1,1,0},{7.5f,55.5f,0.5f,1,0,1},{55.5f,55.5f,0.5f,1,1,1}};
            result=device->Clear(0,nullptr,D3DCLEAR_TARGET,0xff102030,1,0);
            if(SUCCEEDED(result))result=device->SetFVF(D3DFVF_XYZRHW|D3DFVF_TEX1);
            if(SUCCEEDED(result))result=device->SetPixelShader(shader);
            if(SUCCEEDED(result))result=device->SetTexture(0,texture);
            if(SUCCEEDED(result))result=device->SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE);
            if(SUCCEEDED(result))result=device->SetRenderState(D3DRS_ZENABLE,FALSE);
            if(SUCCEEDED(result))result=device->SetSamplerState(0,D3DSAMP_MINFILTER,D3DTEXF_POINT);
            if(SUCCEEDED(result))result=device->SetSamplerState(0,D3DSAMP_MAGFILTER,D3DTEXF_POINT);
            if(SUCCEEDED(result))result=device->BeginScene();
            if(SUCCEEDED(result)){HRESULT draw=device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP,2,quad,sizeof(Vertex));HRESULT end=device->EndScene();result=FAILED(draw)?draw:end;}
            check(SUCCEEDED(result),"draw D3DX texture through the compiled shader",result);
            IDirect3DSurface9 *target=nullptr,*readback=nullptr;
            result=device->GetRenderTarget(0,&target);
            if(SUCCEEDED(result))result=device->CreateOffscreenPlainSurface(64,64,D3DFMT_X8R8G8B8,D3DPOOL_SYSTEMMEM,&readback,nullptr);
            if(SUCCEEDED(result))result=device->GetRenderTargetData(target,readback);
            D3DLOCKED_RECT lock{};if(SUCCEEDED(result))result=readback->LockRect(&lock,nullptr,D3DLOCK_READONLY);
            DWORD sample=0,corner=0;
            if(SUCCEEDED(result)&&lock.pBits&&lock.Pitch>=256){sample=*(DWORD*)((BYTE*)lock.pBits+46*lock.Pitch+22*4)&0xffffff;corner=*(DWORD*)((BYTE*)lock.pBits+lock.Pitch+4)&0xffffff;}
            // Pixel (22,46) maps to texture (1,3), or (3,1) after uv.yx.
            const DWORD expected=swapped?0x806040:0x40c070;
            printf("D3DX shader readback: sample=%06lx expected=%06lx background=%06lx\n",(unsigned long)sample,(unsigned long)expected,(unsigned long)corner);
            check(SUCCEEDED(result)&&sample==expected&&corner==0x102030,swapped?"coordinate permutation samples the correct asymmetric texel":"actual shader output and background pixels match",result);
            if(SUCCEEDED(result))readback->UnlockRect();if(readback)readback->Release();if(target)target->Release();
            result=device->Present(nullptr,nullptr,nullptr,nullptr);check(SUCCEEDED(result),"present the D3DX-rendered frame",result);
        }
        if(shader)shader->Release();if(texture)texture->Release();device->Release();
    }
    DestroyWindow(window);api->Release();FreeLibrary(d3d);
}

int wmain(int argc,wchar_t **argv) {
    setvbuf(stdout,nullptr,_IONBF,0);SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
    HANDLE timer=CreateThread(nullptr,0,watchdog,nullptr,0,nullptr);if(timer)CloseHandle(timer);
    if(argc!=3||wcscmp(argv[1],L"--expected-dll")){fputs("Usage: d3dx9-probe --expected-dll ABSOLUTE_PROVIDER_PATH\n",stderr);return 2;}
    DEVMODEW before{};before.dmSize=sizeof(before);check(EnumDisplaySettingsW(nullptr,ENUM_CURRENT_SETTINGS,&before)!=FALSE,"snapshot host display mode");
    HMODULE helper=LoadLibraryW(module_name);check(helper!=nullptr,"load exact versioned D3DX9 provider",helper?S_OK:HRESULT_FROM_WIN32(GetLastError()));if(!helper)return 1;
    auto path=module_path(helper);wprintf(L"D3DX provider: %ls\n",path.c_str());check(same_file(path,argv[2]),"selected D3DX9 module matches exact provider file identity");
    // Microsoft removed fragment-linker exports in SDK42. Their presence here
    // checks the versioned export surface without claiming the API is complete.
    bool fragmentExports=GetProcAddress(helper,"D3DXCreateFragmentLinker")&&GetProcAddress(helper,"D3DXCreateFragmentLinkerEx");
    check(fragmentExports==(WINEDOWS_D3DX_VERSION<42),"fragment-linker export presence matches the actual SDK version");
#ifdef WINEDOWS_STATIC_D3DX9_IMPORT
    check(imports_from(helper),"startup imports bind the selected D3DX9 DLL");
#endif
    Functions f;bool bound=f.bind(helper);check(bound,"all requested D3DX exports are present");
    if(bound){math(f);semantics(f);for(bool swapped:{false,true}){ID3DXBuffer *code=compile_shader(f,swapped);texture_and_render(f,code,swapped);if(code)code->Release();}}
    std::wstring expected=argv[2];bool wine=expected.find(L"\\builtin\\")!=expected.npos;
    #if WINEDOWS_D3DX_VERSION >= 42
    const wchar_t *dependencies[]={L"d3dcompiler_42.dll",L"d3dxof.dll"};
#else
    const wchar_t *dependencies[]={L"d3dcompiler_47.dll",L"wined3d.dll",L"d3dxof.dll"};
#endif
    for(const wchar_t *name:dependencies){
        auto dependency=module_path(GetModuleHandleW(name));wprintf(L"D3DX dependency %ls: %ls\n",name,dependency.c_str());
        if(wine)check(same_file(dependency,expected.substr(0,expected.find_last_of(L"\\/")+1)+name),"D3DX dependency comes from the selected Wine bundle");
    }
    FreeLibrary(helper);DEVMODEW after{};after.dmSize=sizeof(after);
    bool preserved=EnumDisplaySettingsW(nullptr,ENUM_CURRENT_SETTINGS,&after)&&before.dmPelsWidth==after.dmPelsWidth&&before.dmPelsHeight==after.dmPelsHeight&&before.dmBitsPerPel==after.dmBitsPerPel&&before.dmDisplayFrequency==after.dmDisplayFrequency;
    check(preserved,"D3DX tests leave the host display mode unchanged");printf("D3DX9 %zu-bit: %d failure(s)\n",sizeof(void*)*8,failures);return failures?1:0;
}
