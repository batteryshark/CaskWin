// SPDX-License-Identifier: LGPL-2.1-or-later
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <objbase.h>
#include <objidl.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>

static volatile LONG failures=0;
static void check(bool ok,const char *label,HRESULT hr=S_OK) {
    printf("%s %s (0x%08lx)\n",ok?"PASS":"FAIL",label,(ULONG)hr);
    if(!ok)InterlockedIncrement(&failures);
}
static const wchar_t *stream_iid=L"{0000000C-0000-0000-C000-000000000046}";
static const wchar_t *sequential_iid=L"{0C733A30-2A1C-11CE-ADE5-00AA0044773D}";
static const wchar_t *proxy_clsid=L"{00000320-0000-0000-C000-000000000046}";
static const wchar_t *blocked_clsid=L"{A6EB5B18-C670-4B26-B88F-A85B35F99E71}";
static std::wstring interface_path(const wchar_t *iid){return std::wstring(L"Software\\Classes\\Interface\\")+iid;}
static std::wstring class_path(){return std::wstring(L"Software\\Classes\\CLSID\\")+proxy_clsid;}
static void set(HKEY root,const std::wstring &path,const wchar_t *value) {
    HKEY key=nullptr;LSTATUS s=RegCreateKeyExW(root,path.c_str(),0,nullptr,0,KEY_ALL_ACCESS,nullptr,&key,nullptr);
    check(!s,"open private COM registration",HRESULT_FROM_WIN32(s));if(s)return;
    s=RegSetValueExW(key,nullptr,0,REG_SZ,(const BYTE*)value,(DWORD)((wcslen(value)+1)*2));
    check(!s,"write private COM override",HRESULT_FROM_WIN32(s));RegCloseKey(key);
}
static std::wstring get(HKEY root,const std::wstring &path,bool required=true) {
    HKEY key=nullptr;LSTATUS s=RegOpenKeyExW(root,path.c_str(),0,KEY_QUERY_VALUE,&key);
    if(s){if(required)check(false,"read private COM key",HRESULT_FROM_WIN32(s));return {};}
    wchar_t data[1024]{};DWORD type=0,size=sizeof(data);s=RegQueryValueExW(key,nullptr,nullptr,&type,(BYTE*)data,&size);RegCloseKey(key);
    bool valid=!s&&type==REG_SZ&&size>=2&&size<=sizeof(data)&&size%2==0&&!data[size/2-1];
    if(required)check(valid,"read bounded COM registration value",HRESULT_FROM_WIN32(s));return valid?data:L"";
}
static void absent(HKEY root,const std::wstring &path,const char *label) {
    HKEY key=nullptr;LSTATUS s=RegOpenKeyExW(root,path.c_str(),0,KEY_READ,&key);if(key)RegCloseKey(key);
    check(s==ERROR_FILE_NOT_FOUND||s==ERROR_PATH_NOT_FOUND,label,HRESULT_FROM_WIN32(s));
}
static void mapping(const wchar_t *expected) {
    CLSID actual{},wanted{};CLSIDFromString(expected,&wanted);
    HRESULT hr=CoGetPSClsid(IID_IStream,&actual);check(SUCCEEDED(hr)&&IsEqualCLSID(actual,wanted),"COM resolves requested private IStream proxy",hr);
}
// Deliberately has no IMarshal/agile interface. Standard marshaling must call
// the object in its owning apartment; direct pointer transfer cannot pass.
class TestStream final:public IStream {
    volatile LONG refs=1;
    DWORD owner=GetCurrentThreadId();bool sta;
    std::vector<BYTE> bytes;ULONGLONG position=0;
    bool apartment() {
        APTTYPE type;APTTYPEQUALIFIER q;HRESULT hr=CoGetApartmentType(&type,&q);
        bool ok=SUCCEEDED(hr)&&(sta?GetCurrentThreadId()==owner:(type==APTTYPE_MTA));
        if(!ok)check(false,"stream call stays in owning apartment",hr);return ok;
    }
public:
    volatile LONG calls=0;
    explicit TestStream(bool is_sta):sta(is_sta),bytes(4096){for(size_t i=0;i<bytes.size();i++)bytes[i]=(BYTE)(i*37+11);}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void **out) override {
        if(!out)return E_POINTER;*out=nullptr;
        if(IsEqualIID(iid,IID_IUnknown)||IsEqualIID(iid,IID_IStream)||IsEqualIID(iid,IID_ISequentialStream)){*out=static_cast<IStream*>(this);AddRef();return S_OK;}
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override{return (ULONG)InterlockedIncrement(&refs);}
    ULONG STDMETHODCALLTYPE Release() override{LONG n=InterlockedDecrement(&refs);if(!n)delete this;return (ULONG)n;}
    HRESULT STDMETHODCALLTYPE Read(void *data,ULONG size,ULONG *done) override {
        if(done)*done=0;if(!apartment())return RPC_E_WRONG_THREAD;if(size&&!data)return STG_E_INVALIDPOINTER;
        ULONG n=(ULONG)std::min<ULONGLONG>(size,position<bytes.size()?bytes.size()-position:0);
        if(n)memcpy(data,bytes.data()+(size_t)position,n);position+=n;if(done)*done=n;InterlockedIncrement(&calls);return n==size?S_OK:S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE Write(const void *data,ULONG size,ULONG *done) override {
        if(done)*done=0;if(!apartment())return RPC_E_WRONG_THREAD;if(size&&!data)return STG_E_INVALIDPOINTER;
        if(position+size>65536)return STG_E_MEDIUMFULL;if(position+size>bytes.size())bytes.resize((size_t)position+size);
        if(size)memcpy(bytes.data()+(size_t)position,data,size);position+=size;if(done)*done=size;InterlockedIncrement(&calls);return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Seek(LARGE_INTEGER move,DWORD origin,ULARGE_INTEGER *result) override {
        if(!apartment())return RPC_E_WRONG_THREAD;
        LONGLONG base=origin==STREAM_SEEK_SET?0:origin==STREAM_SEEK_CUR?(LONGLONG)position:origin==STREAM_SEEK_END?(LONGLONG)bytes.size():-1;
        if(base<0||move.QuadPart < -base||move.QuadPart>65536-base)return STG_E_INVALIDFUNCTION;
        position=(ULONGLONG)(base+move.QuadPart);if(result)result->QuadPart=position;InterlockedIncrement(&calls);return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetSize(ULARGE_INTEGER size) override{if(!apartment())return RPC_E_WRONG_THREAD;if(size.QuadPart>65536)return STG_E_MEDIUMFULL;bytes.resize((size_t)size.QuadPart);return S_OK;}
    HRESULT STDMETHODCALLTYPE CopyTo(IStream*,ULARGE_INTEGER,ULARGE_INTEGER*,ULARGE_INTEGER*) override{return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE Commit(DWORD) override{return apartment()?S_OK:RPC_E_WRONG_THREAD;}
    HRESULT STDMETHODCALLTYPE Revert() override{return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE LockRegion(ULARGE_INTEGER,ULARGE_INTEGER,DWORD) override{return STG_E_INVALIDFUNCTION;}
    HRESULT STDMETHODCALLTYPE UnlockRegion(ULARGE_INTEGER,ULARGE_INTEGER,DWORD) override{return STG_E_INVALIDFUNCTION;}
    HRESULT STDMETHODCALLTYPE Stat(STATSTG *stat,DWORD) override{if(!stat)return E_POINTER;if(!apartment())return RPC_E_WRONG_THREAD;memset(stat,0,sizeof(*stat));stat->type=STGTY_STREAM;stat->cbSize.QuadPart=bytes.size();return S_OK;}
    HRESULT STDMETHODCALLTYPE Clone(IStream **out) override{if(out)*out=nullptr;return E_NOTIMPL;}
};
static const BYTE replacement[]={0,254,17,35,53,71,89,107,125,143,161,179,197,215,233,251};
static void verify(IStream *stream,bool modified) {
    LARGE_INTEGER zero{};HRESULT hr=stream->Seek(zero,STREAM_SEEK_SET,nullptr);check(SUCCEEDED(hr),"proxy seeks source stream",hr);
    BYTE actual[4096]{};ULONG got=0;hr=stream->Read(actual,sizeof(actual),&got);bool equal=got==sizeof(actual);
    for(size_t i=0;i<sizeof(actual);i++)if(actual[i]!=(modified&&i>=1536&&i<1536+sizeof(replacement)?replacement[i-1536]:(BYTE)(i*37+11)))equal=false;
    check(hr==S_OK&&equal,"cross-apartment stream bytes are exact",hr);
    STATSTG stat{};hr=stream->Stat(&stat,STATFLAG_NONAME);check(SUCCEEDED(hr)&&stat.type==STGTY_STREAM&&stat.cbSize.QuadPart==4096,"proxy returns correct stream metadata",hr);
    hr=stream->Seek(zero,STREAM_SEEK_SET,nullptr);check(SUCCEEDED(hr),"reset before sequential interface",hr);
    ISequentialStream *sequential=nullptr;hr=stream->QueryInterface(IID_ISequentialStream,(void**)&sequential);check(SUCCEEDED(hr)&&sequential,"query standard sequential stream proxy",hr);
    if(sequential){BYTE first[16]{};got=0;hr=sequential->Read(first,sizeof(first),&got);bool good=got==sizeof(first);for(size_t i=0;i<sizeof(first);i++)good=good&&first[i]==(BYTE)(i*37+11);check(hr==S_OK&&good,"sequential proxy reads exact bytes",hr);sequential->Release();}
}
struct Work {IStream *packet;IGlobalInterfaceTable *git;DWORD cookie;bool sta;HANDLE done;};
static DWORD WINAPI worker(void *arg) {
    auto &work=*(Work*)arg;HRESULT hr=CoInitializeEx(nullptr,work.sta?COINIT_APARTMENTTHREADED:COINIT_MULTITHREADED);check(SUCCEEDED(hr),"initialize destination apartment",hr);
    if(SUCCEEDED(hr)) {
        IStream *stream=nullptr;hr=CoGetInterfaceAndReleaseStream(work.packet,IID_IStream,(void**)&stream);work.packet=nullptr;check(SUCCEEDED(hr)&&stream,"unmarshal standard IStream in destination apartment",hr);
        if(stream){verify(stream,false);LARGE_INTEGER where{};where.QuadPart=1536;hr=stream->Seek(where,STREAM_SEEK_SET,nullptr);check(SUCCEEDED(hr),"position proxy write",hr);ULONG written=0;hr=stream->Write(replacement,sizeof(replacement),&written);check(hr==S_OK&&written==sizeof(replacement),"proxy writes exact binary payload",hr);stream->Release();}
        IStream *from_git=nullptr;hr=work.git->GetInterfaceFromGlobal(work.cookie,IID_IStream,(void**)&from_git);check(SUCCEEDED(hr)&&from_git,"retrieve IStream through GIT across apartments",hr);
        if(from_git){verify(from_git,true);from_git->Release();}
        CoUninitialize();
    }
    SetEvent(work.done);return 0;
}
static void exercise(bool sta) {
    mapping(proxy_clsid);
    auto *stream=new TestStream(sta);IStream *packet=nullptr;HRESULT hr=CoMarshalInterThreadInterfaceInStream(IID_IStream,stream,&packet);
    check(SUCCEEDED(hr)&&packet,"marshal non-agile IStream through standard COM",hr);
    IGlobalInterfaceTable *git=nullptr;DWORD cookie=0;
    hr=CoCreateInstance(CLSID_StdGlobalInterfaceTable,nullptr,CLSCTX_INPROC_SERVER,IID_IGlobalInterfaceTable,(void**)&git);check(SUCCEEDED(hr)&&git,"activate process GIT",hr);
    if(git){hr=git->RegisterInterfaceInGlobal(stream,IID_IStream,&cookie);check(SUCCEEDED(hr)&&cookie,"register IStream in GIT",hr);}
    if(packet&&git&&cookie) {
        Work work{packet,git,cookie,!sta,CreateEventW(nullptr,TRUE,FALSE,nullptr)};
        HANDLE thread=CreateThread(nullptr,0,worker,&work,0,nullptr);check(thread&&work.done,"start cross-apartment worker");
        if(thread&&work.done){DWORD index=0;hr=CoWaitForMultipleHandles(COWAIT_DISPATCH_CALLS|COWAIT_DISPATCH_WINDOW_MESSAGES,12000,1,&work.done,&index);check(hr==S_OK,"source apartment dispatches calls to completion",hr);if(hr!=S_OK)TerminateProcess(GetCurrentProcess(),3);WaitForSingleObject(thread,1000);CloseHandle(thread);packet=work.packet;verify(stream,true);check(stream->calls>=8,"standard proxy invoked source methods");}
        if(work.done)CloseHandle(work.done);
    }
    if(packet){LARGE_INTEGER zero{};packet->Seek(zero,STREAM_SEEK_SET,nullptr);CoReleaseMarshalData(packet);packet->Release();}
    if(git){if(cookie){hr=git->RevokeInterfaceFromGlobal(cookie);check(SUCCEEDED(hr),"revoke GIT registration",hr);}git->Release();}
    stream->Release();
}
int wmain(int argc,wchar_t **argv) {
    setvbuf(stdout,nullptr,_IONBF,0);SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
    if(argc<2)return 2;std::wstring mode=argv[1];bool sta=argc<3||wcscmp(argv[2],L"mta");
    bool host=mode==L"--host";if(!host&&!GetEnvironmentVariableW(L"WINEDOWS_PREFIX",nullptr,0)){puts("Refusing private-registry tests outside a prefix");return 2;}
    HRESULT hr=CoInitializeEx(nullptr,sta?COINIT_APARTMENTTHREADED:COINIT_MULTITHREADED);check(SUCCEEDED(hr),"initialize source apartment",hr);if(FAILED(hr))return 1;
    if(mode==L"--exercise"||host)exercise(sta);
    else if(mode==L"--machine-block")set(HKEY_LOCAL_MACHINE,interface_path(stream_iid)+L"\\ProxyStubClsid32",blocked_clsid);
    else if(mode==L"--expect-block") {
        mapping(blocked_clsid);auto *stream=new TestStream(sta);IStream *packet=nullptr;hr=CoMarshalInterThreadInterfaceInStream(IID_IStream,stream,&packet);
        check(FAILED(hr)&&!packet,"broken private interface override blocks default marshaler",hr);if(packet){CoReleaseMarshalData(packet);packet->Release();}stream->Release();
    } else if(mode==L"--user-restore")set(HKEY_CURRENT_USER,interface_path(stream_iid)+L"\\ProxyStubClsid32",proxy_clsid);
    else if(mode==L"--verify-overrides") {
        check(!_wcsicmp(get(HKEY_LOCAL_MACHINE,interface_path(stream_iid)+L"\\ProxyStubClsid32").c_str(),blocked_clsid),"machine override survives prefix initialization");
        check(!_wcsicmp(get(HKEY_CURRENT_USER,interface_path(stream_iid)+L"\\ProxyStubClsid32").c_str(),proxy_clsid),"user override survives prefix initialization");exercise(sta);
    } else if(mode==L"--empty-sequential") {
        auto path=interface_path(sequential_iid);LSTATUS s=RegDeleteTreeW(HKEY_LOCAL_MACHINE,path.c_str());check(!s,"remove fixture sequential mapping",HRESULT_FROM_WIN32(s));HKEY key=nullptr;s=RegCreateKeyExW(HKEY_LOCAL_MACHINE,path.c_str(),0,nullptr,0,KEY_ALL_ACCESS,nullptr,&key,nullptr);check(!s,"create deliberately empty interface registration",HRESULT_FROM_WIN32(s));if(key)RegCloseKey(key);
    } else if(mode==L"--expect-empty")absent(HKEY_LOCAL_MACHINE,interface_path(sequential_iid)+L"\\ProxyStubClsid32","existing empty interface key is not repaired implicitly");
    else if(mode==L"--class-block")set(HKEY_LOCAL_MACHINE,class_path()+L"\\InprocServer32",L"C:\\tests\\missing-standard-proxy.dll");
    else if(mode==L"--expect-class-preserved") {
        check(get(HKEY_LOCAL_MACHINE,class_path()+L"\\InprocServer32")==L"C:\\tests\\missing-standard-proxy.dll","broken private proxy class is preserved");
        // Native COM provides this factory intrinsically. Preserve private class
        // data, but do not claim that data can replace the OS backend factory.
        CLSID clsid{};CLSIDFromString(proxy_clsid,&clsid);IUnknown *factory=nullptr;hr=CoGetClassObject(clsid,CLSCTX_INPROC_SERVER,nullptr,IID_IPSFactoryBuffer,(void**)&factory);check(SUCCEEDED(hr)&&factory,"native intrinsic standard proxy factory remains available",hr);if(factory)factory->Release();
    } else check(false,"known fixture mode");
    if(!host&&(mode==L"--exercise"||mode==L"--verify-overrides"))absent(HKEY_CLASSES_ROOT,std::wstring(L"CLSID\\")+proxy_clsid,"standard marshaling needs no imported host proxy class");
    if(!host){absent(HKEY_CLASSES_ROOT,L"CLSID\\{00021401-0000-0000-C000-000000000046}","unrelated host ShellLink class remains hidden");absent(HKEY_CLASSES_ROOT,L"Interface\\{00020400-0000-0000-C000-000000000046}","unrelated host IDispatch interface remains hidden");}
    CoUninitialize();printf("COM marshaling result: %ld failures\n",failures);return failures?1:0;
}
