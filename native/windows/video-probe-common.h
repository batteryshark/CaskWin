// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <d3d10.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <stdexcept>
#include "vendor/Detours-4.0.1/include/detours.h"
using Microsoft::WRL::ComPtr;
constexpr UINT width=160,height=96,frames=24,fps=12;
constexpr LONGLONG duration=10000000/fps;
static void checked(HRESULT hr,const char *what){printf("%s %s hr=%08lx\n",SUCCEEDED(hr)?"PASS":"FAIL",what,(ULONG)hr);if(FAILED(hr))throw std::runtime_error(what);}
static void check(bool ok,const char *what){checked(ok?S_OK:E_FAIL,what);}
static std::wstring module_path(HMODULE module){std::vector<wchar_t> path(32768);DWORD size=GetModuleFileNameW(module,path.data(),(DWORD)path.size());return size&&size<path.size()?std::wstring(path.data(),size):std::wstring();}
static void video_type(IMFMediaType *type,REFGUID subtype){
    checked(type->SetGUID(MF_MT_MAJOR_TYPE,MFMediaType_Video),"set video major type");checked(type->SetGUID(MF_MT_SUBTYPE,subtype),"set video subtype");
    checked(MFSetAttributeSize(type,MF_MT_FRAME_SIZE,width,height),"set video dimensions");checked(MFSetAttributeRatio(type,MF_MT_FRAME_RATE,fps,1),"set frame rate");
    checked(MFSetAttributeRatio(type,MF_MT_PIXEL_ASPECT_RATIO,1,1),"set pixel aspect ratio");checked(type->SetUINT32(MF_MT_INTERLACE_MODE,MFVideoInterlace_Progressive),"set progressive video");
}
using Submit=HRESULT(STDMETHODCALLTYPE*)(ID3D11VideoContext*,ID3D11VideoDecoder*,UINT,const D3D11_VIDEO_DECODER_BUFFER_DESC*);
using Begin=HRESULT(STDMETHODCALLTYPE*)(ID3D11VideoContext*,ID3D11VideoDecoder*,ID3D11VideoDecoderOutputView*,UINT,const void*);
using End=HRESULT(STDMETHODCALLTYPE*)(ID3D11VideoContext*,ID3D11VideoDecoder*);
static Submit real_submit;static Begin real_begin;static End real_end;
static volatile LONG submitted,begun,ended;
static HRESULT STDMETHODCALLTYPE submit(ID3D11VideoContext *self,ID3D11VideoDecoder *decoder,UINT count,const D3D11_VIDEO_DECODER_BUFFER_DESC *buffers){HRESULT hr=real_submit(self,decoder,count,buffers);if(SUCCEEDED(hr)&&count)InterlockedIncrement(&submitted);return hr;}
static HRESULT STDMETHODCALLTYPE begin(ID3D11VideoContext *self,ID3D11VideoDecoder *decoder,ID3D11VideoDecoderOutputView *view,UINT keysize,const void *key){HRESULT hr=real_begin(self,decoder,view,keysize,key);if(SUCCEEDED(hr))InterlockedIncrement(&begun);return hr;}
static HRESULT STDMETHODCALLTYPE end(ID3D11VideoContext *self,ID3D11VideoDecoder *decoder){HRESULT hr=real_end(self,decoder);if(SUCCEEDED(hr))InterlockedIncrement(&ended);return hr;}
struct DecodeInstrumentation {
    bool attached=false;
    explicit DecodeInstrumentation(ID3D11VideoContext *context){
        void **table=*(void***)context;real_begin=(Begin)table[9];real_end=(End)table[10];real_submit=(Submit)table[11];
        HMODULE module=nullptr;GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,(LPCWSTR)real_submit,&module);printf("DXVA submission implementation: %ls\n",module_path(module).c_str());
        check(DetourTransactionBegin()==NO_ERROR,"begin decode instrumentation");DetourUpdateThread(GetCurrentThread());
        check(DetourAttach((PVOID*)&real_begin,begin)==NO_ERROR&&DetourAttach((PVOID*)&real_end,end)==NO_ERROR&&DetourAttach((PVOID*)&real_submit,submit)==NO_ERROR,"instrument native decoder calls");
        check(DetourTransactionCommit()==NO_ERROR,"commit decode instrumentation");attached=true;
    }
    ~DecodeInstrumentation(){if(attached){DetourTransactionBegin();DetourUpdateThread(GetCurrentThread());DetourDetach((PVOID*)&real_begin,begin);DetourDetach((PVOID*)&real_end,end);DetourDetach((PVOID*)&real_submit,submit);DetourTransactionCommit();}}
};
static DWORD WINAPI watchdog(void*){Sleep(60000);TerminateProcess(GetCurrentProcess(),124);return 0;}
