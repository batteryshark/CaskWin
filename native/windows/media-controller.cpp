// SPDX-License-Identifier: LGPL-2.1-or-later
#include "media-controller.h"
#include "media-descriptor.h"
#include <winioctl.h>
#include <process.h>

namespace wd {
namespace {
constexpr DWORD wire_magic=0x574d4350,wire_version=1,limit=256*1024,ack=0x41434b31;
constexpr wchar_t endpoint_file[]=L"\\.winedows\\media-session.control";
struct Request {DWORD magic,version,operation,drive,root_chars,source_chars,label_chars;};
struct Response {DWORD magic,version,error,changes,message_chars;};
struct Endpoint {DWORD magic,version,pid,reserved;wchar_t name[96];};
static_assert(sizeof(Request)==28&&sizeof(Response)==20&&sizeof(Endpoint)==208,"Media controller wire ABI changed");
struct Handle {
    HANDLE h=INVALID_HANDLE_VALUE;
    explicit Handle(HANDLE value):h(value){}
    ~Handle(){if(h!=INVALID_HANDLE_VALUE&&h)CloseHandle(h);}
};
str pipe_name(const str &session){
    const str head=L"Local\\Winedows.Media.";
    if(session.size()!=head.size()+32||session.compare(0,head.size(),head))throw std::runtime_error("Invalid media-controller session name");
    for(size_t n=head.size();n<session.size();n++)if(!((session[n]>=L'0'&&session[n]<=L'9')||(session[n]>=L'a'&&session[n]<=L'f')))
        throw std::runtime_error("Invalid media-controller session token");
    // LOCAL is also required for WaitNamedPipe to resolve the same endpoint
    // under an AppContainer token; it works for ordinary desktop processes.
    // https://learn.microsoft.com/windows/uwp/communication/interprocess-communication#pipes
    return L"\\\\.\\pipe\\LOCAL\\"+session.substr(6)+L".Control";
}
void storage(const str &root){
    DWORD a=GetFileAttributesW((root+L"\\.winedows").c_str());
    if(a==INVALID_FILE_ATTRIBUTES||!(a&FILE_ATTRIBUTE_DIRECTORY)||(a&FILE_ATTRIBUTE_REPARSE_POINT))
        throw std::runtime_error("Media controller requires ordinary private prefix storage");
}
Endpoint read_endpoint(const str &root){
    storage(root);str path=root+endpoint_file;
    DWORD a=GetFileAttributesW(path.c_str());
    if(a==INVALID_FILE_ATTRIBUTES||(a&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)))throw std::runtime_error("No active media controller for this prefix");
    Handle file(CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr));
    if(file.h==INVALID_HANDLE_VALUE)check(GetLastError(),"Read media-controller endpoint");
    Endpoint e{};LARGE_INTEGER size{};DWORD got=0;
    if(!GetFileSizeEx(file.h,&size)||size.QuadPart!=sizeof(e)||!ReadFile(file.h,&e,sizeof(e),&got,nullptr)||got!=sizeof(e)||
       e.magic!=wire_magic||e.version!=wire_version||wcsnlen_s(e.name,96)==96)
        throw std::runtime_error("Invalid media-controller endpoint");
    pipe_name(e.name);return e;
}
// All pipe I/O is overlapped so loader shutdown can cancel a waiting client.
// Reads of completed commands may wait while image preparation runs; a lost
// loader closes the pipe and fails the client instead of falling back to INI.
bool transfer(HANDLE pipe,HANDLE stop,bool writing,void *data,DWORD size,DWORD &done,DWORD timeout){
    Handle event(CreateEventW(nullptr,TRUE,FALSE,nullptr));if(!event.h)check(GetLastError(),"Create controller I/O event");
    OVERLAPPED overlapped{};overlapped.hEvent=event.h;done=0;
    BOOL ok=writing?WriteFile(pipe,data,size,&done,&overlapped):ReadFile(pipe,data,size,&done,&overlapped);
    if(ok)return true;
    DWORD error=GetLastError();if(error!=ERROR_IO_PENDING){SetLastError(error);return false;}
    HANDLE events[]={event.h,stop};DWORD waited=WaitForMultipleObjects(stop?2:1,events,FALSE,timeout);
    if(waited==WAIT_OBJECT_0)return GetOverlappedResult(pipe,&overlapped,&done,FALSE)!=FALSE;
    CancelIoEx(pipe,&overlapped);GetOverlappedResult(pipe,&overlapped,&done,TRUE);
    SetLastError(waited==WAIT_TIMEOUT?ERROR_TIMEOUT:ERROR_OPERATION_ABORTED);return false;
}
bool connect(HANDLE pipe,HANDLE stop){
    Handle event(CreateEventW(nullptr,TRUE,FALSE,nullptr));if(!event.h)check(GetLastError(),"Create controller connection event");
    OVERLAPPED overlapped{};overlapped.hEvent=event.h;
    if(ConnectNamedPipe(pipe,&overlapped))return true;
    DWORD error=GetLastError();if(error==ERROR_PIPE_CONNECTED)return true;if(error!=ERROR_IO_PENDING)return false;
    HANDLE events[]={event.h,stop};DWORD waited=WaitForMultipleObjects(2,events,FALSE,INFINITE),done=0;
    if(waited==WAIT_OBJECT_0)return GetOverlappedResult(pipe,&overlapped,&done,FALSE)!=FALSE;
    CancelIoEx(pipe,&overlapped);GetOverlappedResult(pipe,&overlapped,&done,TRUE);return false;
}
str consume(const std::vector<BYTE> &message,size_t &offset,DWORD chars){
    if(chars>32767||offset+size_t(chars)*2>message.size())throw std::runtime_error("Malformed media-control string");
    str value(reinterpret_cast<const wchar_t*>(message.data()+offset),chars);offset+=size_t(chars)*2;
    if(value.find(L'\0')!=str::npos)throw std::runtime_error("Media-control string contains NUL");
    return value;
}
void append(std::vector<BYTE> &message,const str &text){const BYTE *first=reinterpret_cast<const BYTE*>(text.data());message.insert(message.end(),first,first+text.size()*2);}
}
struct MediaController::Impl {
    Prefix configured;
    MediaSession &session;
    HANDLE pipe=INVALID_HANDLE_VALUE,stop=nullptr;
    HANDLE worker=nullptr;
    bool published=false;
    Impl(const Prefix &p,MediaSession &s):configured(p),session(s){}
    ~Impl(){
        if(stop)SetEvent(stop);
        if(worker){
            CancelIoEx(pipe,nullptr);CancelSynchronousIo(worker);WaitForSingleObject(worker,INFINITE);CloseHandle(worker);
        }
        if(published){try{auto e=read_endpoint(configured.root);if(session.name()==e.name)DeleteFileW((configured.root+endpoint_file).c_str());}catch(...){}
        }
        if(pipe!=INVALID_HANDLE_VALUE)CloseHandle(pipe);
        if(stop)CloseHandle(stop);
    }
    void publish(){
        storage(configured.root);Endpoint e{};e.magic=wire_magic;e.version=wire_version;e.pid=GetCurrentProcessId();
        if(session.name().size()>=96)throw std::runtime_error("Media controller name is too long");
        wcscpy_s(e.name,session.name().c_str());
        str target=configured.root+endpoint_file;
        DWORD a=GetFileAttributesW(target.c_str());if(a!=INVALID_FILE_ATTRIBUTES&&(a&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)))
            throw std::runtime_error("Media controller endpoint is redirected");
        str temp=target+L"."+std::to_wstring(GetCurrentProcessId())+L"."+std::to_wstring(GetTickCount64());
        {
            Handle file(CreateFileW(temp.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));
            if(file.h==INVALID_HANDLE_VALUE)check(GetLastError(),"Stage media controller endpoint");
            DWORD written=0;
            if(!WriteFile(file.h,&e,sizeof(e),&written,nullptr))check(GetLastError(),"Write media controller endpoint");
            if(written!=sizeof(e))throw std::runtime_error("Incomplete media controller endpoint write");
            if(!FlushFileBuffers(file.h))check(GetLastError(),"Flush media controller endpoint");
        }
        if(!MoveFileExW(temp.c_str(),target.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))check(GetLastError(),"Publish media controller endpoint");
        published=true;
    }
    std::vector<BYTE> process(std::vector<BYTE> request){
        Response response{wire_magic,wire_version,0,0,0};str message;
        try{
            if(request.size()<sizeof(Request))throw std::runtime_error("Short media-control request");
            Request header;memcpy(&header,request.data(),sizeof(header));
            if(header.magic!=wire_magic||header.version!=wire_version||header.drive>=26||header.label_chars>255||
               ULONGLONG(sizeof(header))+2ull*(ULONGLONG(header.root_chars)+header.source_chars+header.label_chars)!=request.size())
                throw std::runtime_error("Invalid media-control request header");
            size_t at=sizeof(header);str root=consume(request,at,header.root_chars),source=consume(request,at,header.source_chars),label=consume(request,at,header.label_chars);
            if(_wcsicmp(root.c_str(),configured.root.c_str()))throw std::runtime_error("Media controller belongs to another prefix");
            auto state=session.snapshot(header.drive);
            if(!state.managed||!state.ejectable)throw std::runtime_error("Drive does not support unambiguous virtual media control");
            if(header.operation==DWORD(MediaControl::swap)){
                if(source.empty()||!state.medium)throw std::runtime_error("Swap requires a private ISO/CUE CD drive and an image path");
                if(state.removal_locks){response.error=ERROR_BUSY;throw std::runtime_error("Virtual medium is locked against removal");}
                Drive next=prepare_live_medium(configured,source,label);
                if(WaitForSingleObject(stop,0)==WAIT_OBJECT_0){response.error=ERROR_OPERATION_ABORTED;throw std::runtime_error("Media controller is shutting down");}
                response.changes=session.replace(header.drive,next);message=L"Live medium: "+next.label;
            }else if(header.operation==DWORD(MediaControl::eject)||header.operation==DWORD(MediaControl::load)){
                if(!source.empty()||!label.empty())throw std::runtime_error("Eject/load do not accept media paths or labels");
                BOOL result=FALSE;DWORD done=0;
                session.ioctl(header.drive,header.operation==DWORD(MediaControl::eject)?IOCTL_STORAGE_EJECT_MEDIA:IOCTL_STORAGE_LOAD_MEDIA,nullptr,0,nullptr,0,&done,nullptr,result);
                if(!result){response.error=GetLastError();throw std::runtime_error("Virtual media operation failed");}
                response.changes=session.snapshot(header.drive).changes;message=header.operation==DWORD(MediaControl::eject)?L"Virtual medium ejected":L"Virtual medium loaded";
            }else throw std::runtime_error("Unknown media-control operation");
        }catch(const std::exception &error){if(!response.error)response.error=ERROR_INVALID_DATA;message=widen(error.what());}
        if(message.size()>2048)message.resize(2048);
        response.message_chars=DWORD(message.size());
        std::vector<BYTE> bytes(sizeof(response));memcpy(bytes.data(),&response,sizeof(response));append(bytes,message);return bytes;
    }
    void run(){
        try{
            while(WaitForSingleObject(stop,0)!=WAIT_OBJECT_0){
                if(!connect(pipe,stop))break;
                std::vector<BYTE> request(limit);DWORD got=0;
                if(transfer(pipe,stop,false,request.data(),DWORD(request.size()),got,5000)){
                    request.resize(got);auto response=process(std::move(request));DWORD sent=0;
                    if(transfer(pipe,stop,true,response.data(),DWORD(response.size()),sent,5000)&&sent==response.size()){
                        DWORD received_ack=0,n=0;transfer(pipe,stop,false,&received_ack,sizeof(received_ack),n,5000);
                    }
                }
                DisconnectNamedPipe(pipe);
            }
        }catch(const std::exception &error){fprintf(stderr,"winedows: media controller stopped: %s\n",error.what());}
    }
    static unsigned __stdcall entry(void *self){static_cast<Impl*>(self)->run();return 0;}
};
MediaController::MediaController(const Prefix &p,MediaSession &s):impl(std::make_unique<Impl>(p,s)){
    impl->stop=CreateEventW(nullptr,TRUE,FALSE,nullptr);if(!impl->stop)check(GetLastError(),"Create media controller stop event");
    str name=pipe_name(s.name());
    impl->pipe=CreateNamedPipeW(name.c_str(),PIPE_ACCESS_DUPLEX|FILE_FLAG_OVERLAPPED|FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_MESSAGE|PIPE_READMODE_MESSAGE|PIPE_WAIT|PIPE_REJECT_REMOTE_CLIENTS,1,limit,limit,5000,nullptr);
    if(impl->pipe==INVALID_HANDLE_VALUE)check(GetLastError(),"Create private media controller");
    impl->publish();impl->worker=reinterpret_cast<HANDLE>(_beginthreadex(nullptr,0,Impl::entry,impl.get(),0,nullptr));
    if(!impl->worker)throw std::runtime_error("Cannot start media controller worker");
}
MediaController::~MediaController()=default;
MediaControlResult control_media(const str &root_arg,MediaControl operation,int drive,const str &source_arg,const str &label){
    str root=fullpath(root_arg),source=source_arg.empty()?str():fullpath(source_arg);
    if(drive<0||drive>=26||root.size()>32767||source.size()>32767||label.size()>255)throw std::runtime_error("Media-control request exceeds bounded fields");
    Endpoint endpoint=read_endpoint(root);str name=pipe_name(endpoint.name);
    if(!WaitNamedPipeW(name.c_str(),5000))throw std::runtime_error("No available active media controller (stale session or busy preparation)");
    Handle pipe(CreateFileW(name.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED,nullptr));
    if(pipe.h==INVALID_HANDLE_VALUE)check(GetLastError(),"Connect to active media controller");
    DWORD mode=PIPE_READMODE_MESSAGE;if(!SetNamedPipeHandleState(pipe.h,&mode,nullptr,nullptr))check(GetLastError(),"Configure media controller connection");
    Request header{wire_magic,wire_version,DWORD(operation),DWORD(drive),DWORD(root.size()),DWORD(source.size()),DWORD(label.size())};
    std::vector<BYTE> request(sizeof(header));memcpy(request.data(),&header,sizeof(header));append(request,root);append(request,source);append(request,label);DWORD done=0;
    if(!transfer(pipe.h,nullptr,true,request.data(),DWORD(request.size()),done,5000))check(GetLastError(),"Send media-control request");
    if(done!=request.size())throw std::runtime_error("Incomplete media-control request write");
    std::vector<BYTE> response(limit);
    if(!transfer(pipe.h,nullptr,false,response.data(),DWORD(response.size()),done,INFINITE))check(GetLastError(),"Receive media-control result");
    response.resize(done);DWORD acknowledged=ack,n=0;transfer(pipe.h,nullptr,true,&acknowledged,sizeof(acknowledged),n,5000);
    if(response.size()<sizeof(Response))throw std::runtime_error("Short media-control response");
    Response reply;memcpy(&reply,response.data(),sizeof(reply));
    if(reply.magic!=wire_magic||reply.version!=wire_version||reply.message_chars>2048||sizeof(reply)+2ull*reply.message_chars!=response.size())throw std::runtime_error("Invalid media-control response");
    size_t offset=sizeof(reply);str message=consume(response,offset,reply.message_chars);
    if(reply.error)throw std::runtime_error(utf8(message)+" (error "+std::to_string(reply.error)+")");
    return {reply.changes,message};
}
}
