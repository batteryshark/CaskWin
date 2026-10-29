// SPDX-License-Identifier: LGPL-2.1-or-later
// Registry text bridge. Escaping and UTF-8 decoding use server/unicode.c from Wine.
#include "common.h"
#include "registry-views.h"
#include <fstream>
#include <sstream>
extern "C" int parse_strW(WCHAR*,unsigned int*,const char*,char);
extern "C" int dump_strW(const WCHAR*,unsigned int,FILE*,const char[2]);
namespace wd {
void copy_registry(HKEY source,HKEY destination,unsigned depth) {
    // RegCopyTree also copies security descriptors, which application hives
    // deliberately prohibit. Copy only the registry data that a prefix owns.
    if(depth>256)throw std::runtime_error("Registry copy nesting limit exceeded");
    DWORD maxname=0,maxdata=0,maxkey=0;
    check(RegQueryInfoKeyW(source,nullptr,nullptr,nullptr,nullptr,&maxkey,nullptr,nullptr,&maxname,&maxdata,nullptr,nullptr),"Read registry copy metadata");
    std::vector<wchar_t> name(std::max(maxname,maxkey)+2);std::vector<BYTE> bytes(maxdata+2);
    for(DWORD i=0;;i++) {
        DWORD n=(DWORD)name.size(),len=(DWORD)bytes.size(),type;LSTATUS e=RegEnumValueW(source,i,name.data(),&n,nullptr,&type,bytes.data(),&len);
        if(e==ERROR_NO_MORE_ITEMS)break;check(e,"Read copied registry value");check(RegSetValueExW(destination,name.data(),0,type,bytes.data(),len),"Write copied registry value");
    }
    for(DWORD i=0;;i++) {
        DWORD n=(DWORD)name.size();LSTATUS e=RegEnumKeyExW(source,i,name.data(),&n,nullptr,nullptr,nullptr,nullptr);
        if(e==ERROR_NO_MORE_ITEMS)break;check(e,"Read copied registry subkey");HKEY from=nullptr,to=nullptr;
        check(RegOpenKeyExW(source,name.data(),0,KEY_READ,&from),"Open copied registry subkey");
        e=RegCreateKeyExW(destination,name.data(),0,nullptr,0,KEY_ALL_ACCESS,nullptr,&to,nullptr);
        try{check(e,"Create copied registry subkey");copy_registry(from,to,depth+1);}catch(...){RegCloseKey(from);if(to)RegCloseKey(to);throw;}
        RegCloseKey(from);RegCloseKey(to);
    }
}
static std::string trim(std::string s) {
    auto a=s.find_first_not_of(" \t\r\n"); if(a==s.npos) return {};
    return s.substr(a,s.find_last_not_of(" \t\r\n")-a+1);
}
static str escaped(const std::string &s,char end,size_t &consumed) {
    std::vector<WCHAR> b(s.size()+2); unsigned n=(unsigned)b.size()*2;
    int r=parse_strW(b.data(),&n,s.c_str(),end);
    if(r<0) throw std::runtime_error("Malformed escaped registry string");
    consumed=r; return str(b.data(),n/2-1);
}
void import_reg(HKEY root,const str &file,REGSAM view) {
    std::ifstream f(file,std::ios::binary); if(!f) throw std::runtime_error("Cannot open registry input");
    std::string all((std::istreambuf_iterator<char>(f)),{});
    if(all.size()>=2 && (BYTE)all[0]==0xff && (BYTE)all[1]==0xfe) {
        if(all.size()%2) throw std::runtime_error("Odd UTF-16 registry input size");
        str w((all.size()-2)/2,0); memcpy(w.data(),all.data()+2,all.size()-2); all=utf8(w);
    }
    if(all.size()>=3 && all.substr(0,3)=="\xef\xbb\xbf") all.erase(0,3);
    std::istringstream input(all); std::string line; std::getline(input,line); line=trim(line);
    bool wine=line=="WINE REGISTRY Version 2";
    if(!wine && line!="Windows Registry Editor Version 5.00" && line!="REGEDIT4") throw std::runtime_error("Unrecognized registry format");
    HKEY current=nullptr;
    try {
        while(std::getline(input,line)) {
            line=trim(line); if(line.empty() || line[0]==';' || line[0]=='#') continue;
            while(!line.empty() && line.back()=='\\') {
                line.pop_back(); std::string next;
                if(!std::getline(input,next)) throw std::runtime_error("Truncated registry continuation");
                line+=trim(next);
            }
            size_t used=0;
            if(line[0]=='[') {
                if(current) { RegCloseKey(current); current=nullptr; }
                str key;
                if(wine) key=escaped(line.substr(1),']',used);
                else { size_t end=line.find(']'); if(end==line.npos) throw std::runtime_error("Missing registry key delimiter"); key=widen(line.substr(1,end-1)); }
                if(!key.empty() && key[0]==L'-') throw std::runtime_error("Registry key deletion imports are not supported");
                if(!wine) {
                    const wchar_t *roots[]={L"HKEY_LOCAL_MACHINE",L"HKEY_CURRENT_USER",L"HKEY_CLASSES_ROOT",L"HKEY_USERS\\.Default"};
                    const wchar_t *mapped[]={L"Machine",L"User",L"Machine\\Software\\Classes",L"Default"}; bool found=false;
                    for(int i=0;i<4;i++) if(inside(key,roots[i])) {key=str(mapped[i])+key.substr(wcslen(roots[i])); found=true;break;}
                    if(!found) throw std::runtime_error("Unsupported registry root");
                    key=registry_import_name(key,view);
                }
                check(RegCreateKeyExW(root,key.c_str(),0,nullptr,0,KEY_ALL_ACCESS,nullptr,&current,nullptr),"Import key"); continue;
            }
            if(!current) throw std::runtime_error("Registry value without key");
            str name; std::string value;
            if(line[0]=='@') used=1;
            else if(line[0]=='"') { name=escaped(line.substr(1),'"',used); ++used; }
            else throw std::runtime_error("Malformed registry value name");
            value=trim(line.substr(used)); if(value.empty() || value[0]!='=') throw std::runtime_error("Missing registry equals sign");
            value=trim(value.substr(1)); DWORD type=REG_NONE; std::vector<BYTE> bytes;
            if(value=="-") { LSTATUS e=RegDeleteValueW(current,name.c_str()); if(e!=ERROR_FILE_NOT_FOUND) check(e,"Delete imported value"); continue; }
            if(!value.empty() && value[0]=='"') {
                str s=escaped(value.substr(1),'"',used); type=REG_SZ;
                bytes.resize((s.size()+1)*2); memcpy(bytes.data(),s.c_str(),bytes.size());
            } else if(value.rfind("str(2):\"",0)==0 || value.rfind("str(7):\"",0)==0) {
                type=value[4]=='2'?REG_EXPAND_SZ:REG_MULTI_SZ; str s=escaped(value.substr(8),'"',used);
                bytes.resize((s.size()+1)*2); memcpy(bytes.data(),s.c_str(),bytes.size());
            } else if(value.rfind("dword:",0)==0) {
                size_t end; unsigned long v=std::stoul(value.substr(6),&end,16);
                if(end!=value.size()-6) throw std::runtime_error("Invalid registry DWORD");
                type=REG_DWORD; bytes.resize(4); memcpy(bytes.data(),&v,4);
            } else {
                size_t colon=value.find(':'); if(colon==value.npos) throw std::runtime_error("Malformed hex registry value");
                if(value.substr(0,colon)=="hex") type=REG_BINARY;
                else if(value.rfind("hex(",0)==0 && value[colon-1]==')') type=std::stoul(value.substr(4,colon-5),nullptr,16);
                else throw std::runtime_error("Unsupported registry value type");
                std::istringstream hex(value.substr(colon+1)); std::string byte;
                while(std::getline(hex,byte,',')) {
                    byte=trim(byte); if(byte.empty()) continue;
                    size_t end; unsigned long v=std::stoul(byte,&end,16);
                    if(end!=byte.size() || v>255) throw std::runtime_error("Invalid hex registry byte"); bytes.push_back((BYTE)v);
                }
            }
            check(RegSetValueExW(current,name.c_str(),0,type,bytes.data(),(DWORD)bytes.size()),"Import registry value");
        }
        if(current) RegCloseKey(current);
    } catch(...) { if(current) RegCloseKey(current); throw; }
}
static void dump_key(HKEY key,const str &path,FILE *f,unsigned depth=0) {
    if(depth>256) throw std::runtime_error("Registry nesting exceeds export limit");
    fputs("\n[",f); dump_strW(path.data(),(unsigned)path.size()*2,f,"[]"); fputs("]\n",f);
    DWORD maxname=0,maxdata=0,maxkey=0;
    check(RegQueryInfoKeyW(key,nullptr,nullptr,nullptr,nullptr,&maxkey,nullptr,nullptr,&maxname,&maxdata,nullptr,nullptr),"Read registry metadata");
    std::vector<wchar_t> name(std::max(maxname,maxkey)+2); std::vector<BYTE> data(maxdata+2);
    for(DWORD i=0;;i++) {
        DWORD n=(DWORD)name.size(),len=(DWORD)data.size(),type;
        LSTATUS e=RegEnumValueW(key,i,name.data(),&n,nullptr,&type,data.data(),&len);
        if(e==ERROR_NO_MORE_ITEMS) break; check(e,"Enumerate registry value");
        if(n) {fputc('"',f);dump_strW(name.data(),n*2,f,"\"\"");fputs("\"=",f);} else fputs("@=",f);
        if(type==REG_DWORD && len==4) { DWORD v; memcpy(&v,data.data(),4); fprintf(f,"dword:%08lx\n",v); }
        else if(type==REG_SZ && len>=2 && !(len%2) && data[len-1]==0 && data[len-2]==0) {
            fputc('"',f);dump_strW((WCHAR*)data.data(),len-2,f,"\"\"");fputs("\"\n",f);
        } else {
            fprintf(f,"hex(%lx):",type);
            for(DWORD j=0;j<len;j++) { if(j)fputc(',',f); if(j && j%24==0)fputs("\\\n  ",f);fprintf(f,"%02x",data[j]); }
            fputc('\n',f);
        }
    }
    for(DWORD i=0;;i++) {
        DWORD n=(DWORD)name.size(); LSTATUS e=RegEnumKeyExW(key,i,name.data(),&n,nullptr,nullptr,nullptr,nullptr);
        if(e==ERROR_NO_MORE_ITEMS) break; check(e,"Enumerate registry key");
        HKEY sub; check(RegOpenKeyExW(key,name.data(),0,KEY_READ,&sub),"Open registry subkey");
        try {dump_key(sub,path.empty()?str(name.data()):path+L"\\"+name.data(),f,depth+1);} catch(...) {RegCloseKey(sub);throw;}
        RegCloseKey(sub);
    }
}
void export_reg(HKEY root,const str &file) {
    str tmp=file+L".tmp."+std::to_wstring(GetCurrentProcessId()); FILE *f=nullptr;
    if(_wfopen_s(&f,tmp.c_str(),L"wb")) throw std::runtime_error("Cannot create registry export");
    try { fputs("WINE REGISTRY Version 2\n;; Generated by Winedows native backend\n#arch=win64\n",f);dump_key(root,L"",f); }
    catch(...) {fclose(f);DeleteFileW(tmp.c_str());throw;}
    bool bad=ferror(f)!=0; if(fclose(f))bad=true;
    if(bad) throw std::runtime_error("Registry export write failed");
    if(!MoveFileExW(tmp.c_str(),file.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)) check(GetLastError(),"Commit registry export");
}
}
