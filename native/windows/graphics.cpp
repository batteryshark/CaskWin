// SPDX-License-Identifier: LGPL-2.1-or-later
#include "common.h"
#include <d3d11.h>
#include <dxgi.h>
namespace wd {
int graphics_probe() {
    ID3D11Device *device=nullptr; ID3D11DeviceContext *context=nullptr; D3D_FEATURE_LEVEL level;
    HRESULT hr=D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT|D3D11_CREATE_DEVICE_VIDEO_SUPPORT,nullptr,0,D3D11_SDK_VERSION,&device,&level,&context);
    if(FAILED(hr)) {printf("Hardware D3D11 creation failed: 0x%08lx\n",hr);return 1;}
    IDXGIDevice *dxgi=nullptr; IDXGIAdapter *adapter=nullptr;
    if(SUCCEEDED(device->QueryInterface(__uuidof(IDXGIDevice),(void**)&dxgi)) && SUCCEEDED(dxgi->GetAdapter(&adapter))) {
        DXGI_ADAPTER_DESC d{}; adapter->GetDesc(&d); printf("Adapter: %s\n",utf8(d.Description).c_str()); adapter->Release();
    }
    if(dxgi)dxgi->Release();
    printf("Hardware D3D11 feature level: 0x%x\n",level);
    ID3D11VideoDevice *video=nullptr;
    if(SUCCEEDED(device->QueryInterface(__uuidof(ID3D11VideoDevice),(void**)&video))) {
        printf("D3D11 video decode profiles: %u\n",video->GetVideoDecoderProfileCount());video->Release();
    } else printf("D3D11 video interface unavailable\n");
    D3D11_TEXTURE2D_DESC desc{}; desc.Width=64;desc.Height=64;desc.MipLevels=1;desc.ArraySize=1;
    desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.SampleDesc.Count=1;desc.Usage=D3D11_USAGE_DEFAULT;desc.BindFlags=D3D11_BIND_RENDER_TARGET;
    ID3D11Texture2D *target=nullptr,*staging=nullptr; ID3D11RenderTargetView *rtv=nullptr;
    hr=device->CreateTexture2D(&desc,nullptr,&target);
    if(SUCCEEDED(hr))hr=device->CreateRenderTargetView(target,nullptr,&rtv);
    if(SUCCEEDED(hr)) {
        const float color[]={0.25f,0.5f,0.75f,1.0f};context->ClearRenderTargetView(rtv,color);
        desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        hr=device->CreateTexture2D(&desc,nullptr,&staging);
        if(SUCCEEDED(hr)) {
            context->CopyResource(staging,target);D3D11_MAPPED_SUBRESOURCE map{};
            hr=context->Map(staging,0,D3D11_MAP_READ,0,&map);
            if(SUCCEEDED(hr)) {
                const BYTE *p=(const BYTE*)map.pData;
                bool ok=(p[0]>=63&&p[0]<=64)&&(p[1]>=127&&p[1]<=128)&&(p[2]>=191&&p[2]<=192)&&p[3]==255;
                printf("Hardware render/readback: %s (%u,%u,%u,%u)\n",ok?"PASS":"FAIL",p[0],p[1],p[2],p[3]);
                context->Unmap(staging,0);if(!ok)hr=E_FAIL;
            }
        }
    }
    if(staging)staging->Release();if(rtv)rtv->Release();if(target)target->Release();context->Release();device->Release();
    return FAILED(hr)?1:0;
}
}
