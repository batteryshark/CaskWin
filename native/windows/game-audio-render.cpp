// SPDX-License-Identifier: LGPL-2.1-or-later
// Bounded, generated offscreen D3D11 workload. No game window or game inputs.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include "game-audio-render.h"
#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace game_audio { namespace {
static long long tick(){LARGE_INTEGER value;QueryPerformanceCounter(&value);return value.QuadPart;}
static void require(bool value,const char *what){if(!value)throw std::runtime_error(what);}
static void success(HRESULT result,const char *what){require(SUCCEEDED(result),what);}
template<class T>static void release(T *&value){if(value){value->Release();value=nullptr;}}
static double percentile(std::vector<double> values,double q){if(values.empty())return 0;std::sort(values.begin(),values.end());return values[size_t(q*(values.size()-1))];}
static std::string module(HMODULE value){char name[32768];DWORD length=GetModuleFileNameA(value,name,DWORD(sizeof(name)));std::string result="\"";for(DWORD i=0;i<length;i++){if(name[i]=='\\'||name[i]=='\"')result+='\\';result+=name[i];}return result+'"';}
}
struct RenderWorkload::Impl {
    bool render,readback=false;
    ID3D11Device *device=nullptr;ID3D11DeviceContext *context=nullptr;
    ID3D11Texture2D *target=nullptr,*staging=nullptr;ID3D11RenderTargetView *view=nullptr;
    ID3D11VertexShader *vs=nullptr;ID3D11PixelShader *ps=nullptr;ID3D11Query *query=nullptr;
    HMODULE d3d=nullptr,compiler=nullptr;D3D_FEATURE_LEVEL level{};
    struct Row {long long start,end;};std::vector<Row> rows;
    long long begin=0,frequency=0;
    explicit Impl(bool enabled):render(enabled){rows.reserve(8192);if(!render)return;
        d3d=LoadLibraryW(L"d3d11.dll");compiler=LoadLibraryW(L"d3dcompiler_47.dll");require(d3d&&compiler,"Native render workload DLL unavailable");
        auto create=reinterpret_cast<decltype(&D3D11CreateDevice)>(GetProcAddress(d3d,"D3D11CreateDevice"));
        auto compile=reinterpret_cast<decltype(&D3DCompile)>(GetProcAddress(compiler,"D3DCompile"));require(create&&compile,"Render workload exports missing");
        const D3D_FEATURE_LEVEL levels[]={D3D_FEATURE_LEVEL_11_0,D3D_FEATURE_LEVEL_10_1,D3D_FEATURE_LEVEL_10_0};
        success(create(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,D3D11_CREATE_DEVICE_SINGLETHREADED,levels,3,D3D11_SDK_VERSION,&device,&level,&context),"Hardware D3D11 workload unavailable");
        const char *vertex="float4 main(uint id:SV_VertexID):SV_Position{float2 p=float2((id<<1)&2,id&2);return float4(p*float2(2,-2)+float2(-1,1),0,1);}";
        const char *pixel="float4 main(float4 p:SV_Position):SV_Target{float2 x=p.xy/512.0;[unroll]for(int i=0;i<8;i++)x=frac(float2(dot(x,float2(.71,.33)),dot(x,float2(.27,.83)))+.123);return float4(x,.4,1);}";
        ID3DBlob *code=nullptr,*errors=nullptr;
        HRESULT hr=compile(vertex,strlen(vertex),"owned-audio-workload",nullptr,nullptr,"main","vs_4_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&errors);release(errors);success(hr,"Workload vertex shader compile failed");
        hr=device->CreateVertexShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&vs);release(code);success(hr,"Create workload vertex shader failed");
        hr=compile(pixel,strlen(pixel),"owned-audio-workload",nullptr,nullptr,"main","ps_4_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&errors);release(errors);success(hr,"Workload pixel shader compile failed");
        hr=device->CreatePixelShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&ps);release(code);success(hr,"Create workload pixel shader failed");
        D3D11_TEXTURE2D_DESC desc{};desc.Width=desc.Height=512;desc.MipLevels=desc.ArraySize=1;desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.SampleDesc.Count=1;desc.Usage=D3D11_USAGE_DEFAULT;desc.BindFlags=D3D11_BIND_RENDER_TARGET;
        success(device->CreateTexture2D(&desc,nullptr,&target),"Create workload render target failed");success(device->CreateRenderTargetView(target,nullptr,&view),"Create workload view failed");
        desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;success(device->CreateTexture2D(&desc,nullptr,&staging),"Create workload readback target failed");
        D3D11_QUERY_DESC q{};q.Query=D3D11_QUERY_EVENT;success(device->CreateQuery(&q,&query),"Create bounded workload GPU fence failed");
        D3D11_VIEWPORT viewport{0,0,512,512,0,1};context->RSSetViewports(1,&viewport);context->OMSetRenderTargets(1,&view,nullptr);context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);context->VSSetShader(vs,nullptr,0);context->PSSetShader(ps,nullptr,0);
    }
    ~Impl(){if(context)context->ClearState();release(query);release(vs);release(ps);release(view);release(staging);release(target);release(context);release(device);if(compiler)FreeLibrary(compiler);if(d3d)FreeLibrary(d3d);}
};
RenderWorkload::RenderWorkload(bool enabled):impl_(new Impl(enabled)){}
RenderWorkload::~RenderWorkload(){delete impl_;}
void RenderWorkload::frame(long long begin,long long finish,long long frequency){
    auto &s=*impl_;s.begin=begin;s.frequency=frequency;long long at=tick();
    if(s.render){
        const float clear[4]={0,0,0,0};s.context->ClearRenderTargetView(s.view,clear);
        for(unsigned draw=0;draw<8;draw++)s.context->Draw(3,0);
        s.context->End(s.query);s.context->Flush();BOOL complete=FALSE;
        for(;;){HRESULT result=s.context->GetData(s.query,&complete,sizeof(complete),0);if(result==S_OK&&complete)break;require(result==S_FALSE||result==S_OK,"GPU workload query failed");require(tick()-at<frequency,"GPU workload exceeded one-second fence limit");Sleep(1);}
        if(!s.readback){s.context->CopyResource(s.staging,s.target);D3D11_MAPPED_SUBRESOURCE map{};success(s.context->Map(s.staging,0,D3D11_MAP_READ,0,&map),"GPU workload readback failed");const BYTE *pixel=static_cast<BYTE*>(map.pData);bool valid=pixel[2]>=100&&pixel[2]<=104&&pixel[3]==255;s.context->Unmap(s.staging,0);require(valid,"Generated GPU pixels did not match workload shader");s.readback=true;}
    }
    if(at>=begin&&at<finish){require(s.rows.size()<8192,"Workload trace capacity exceeded");s.rows.push_back({at,tick()});}
}
void RenderWorkload::report(const std::wstring &stem,FILE *json){
    auto &s=*impl_;double scale=s.frequency?1000.0/s.frequency:0;std::vector<double> gaps,costs;unsigned slow=0;
    FILE *csv=_wfopen((stem+L"-frames.csv").c_str(),L"wb");require(csv!=nullptr,"Cannot write workload frame metadata");fputs("index,start_ms,frame_gap_ms,work_ms\n",csv);
    for(unsigned i=0;i<s.rows.size();i++){auto row=s.rows[i];double gap=i?(row.start-s.rows[i-1].start)*scale:0,cost=(row.end-row.start)*scale;if(i)gaps.push_back(gap);costs.push_back(cost);if(cost>1000.0/60)++slow;fprintf(csv,"%u,%.6f,%.6f,%.6f\n",i,(row.start-s.begin)*scale,gap,cost);}fclose(csv);
    fprintf(json,"\"workload\":{\"kind\":\"%s\",\"targetHz\":60,\"frames\":%llu,\"drawsPerFrame\":%u,\"width\":%u,\"height\":%u,\"readbackVerified\":%s,\"featureLevel\":%u,\"d3d11Path\":%s,\"compilerPath\":%s,\"frameGapP99Ms\":%.6f,\"frameGapMaxMs\":%.6f,\"workP99Ms\":%.6f,\"workMaxMs\":%.6f,\"workOverFrameBudget\":%u,\"limitations\":[\"Generated offscreen workload does not reproduce the game scene or Present pacing\",\"Only one GPU frame is allowed in flight\"]},\n",s.render?"offscreen-d3d11":"idle",(unsigned long long)s.rows.size(),s.render?8:0,s.render?512:0,s.render?512:0,s.readback?"true":"false",unsigned(s.level),s.d3d?module(s.d3d).c_str():"\"\"",s.compiler?module(s.compiler).c_str():"\"\"",percentile(gaps,.99),percentile(gaps,1),percentile(costs,.99),percentile(costs,1),slow);
}
}
