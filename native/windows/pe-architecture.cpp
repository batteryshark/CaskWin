// SPDX-License-Identifier: LGPL-2.1-or-later
#include "pe-architecture.h"
#include <fstream>
#include <filesystem>
#include <limits>

namespace wd {
namespace {
constexpr DWORD clr_il_only=0x1,clr_32bit_required=0x2,clr_32bit_preferred=0x20000;
struct ClrPrefix {DWORD size;WORD major,minor;IMAGE_DATA_DIRECTORY metadata;DWORD flags;};
class PeReader {
    std::ifstream file;
    ULONGLONG length;
public:
    explicit PeReader(const str &path):file(std::filesystem::path(path),std::ios::binary) {
        if(!file)throw std::runtime_error("Cannot read executable architecture");
        file.seekg(0,std::ios::end);auto end=file.tellg();
        if(end<0)throw std::runtime_error("Cannot measure executable architecture");
        length=(ULONGLONG)end;
    }
    void read(ULONGLONG position,void *out,size_t size) {
        if(position>length||size>length-position)throw std::runtime_error("Truncated PE architecture metadata");
        file.seekg((std::streamoff)position);
        if(!file.read((char*)out,size))throw std::runtime_error("Cannot read PE architecture metadata");
    }
    template<class T> T at(ULONGLONG position){T value{};read(position,&value,sizeof(value));return value;}
};
}
WORD PeArchitecture::process_machine(WORD native_machine) const {
    if(machine!=IMAGE_FILE_MACHINE_I386||dll||!managed||!(clr_flags&clr_il_only)
       ||(clr_flags&(clr_32bit_required|clr_32bit_preferred)))return machine;
    return native_machine;
}
PeArchitecture inspect_pe_architecture(const str &path) {
    PeReader reader(path);auto dos=reader.at<IMAGE_DOS_HEADER>(0);
    if(dos.e_magic!=IMAGE_DOS_SIGNATURE||dos.e_lfanew<0)throw std::runtime_error("Invalid DOS executable header");
    ULONGLONG header=(DWORD)dos.e_lfanew;
    if(reader.at<DWORD>(header)!=IMAGE_NT_SIGNATURE)throw std::runtime_error("Invalid PE executable signature");
    auto file=reader.at<IMAGE_FILE_HEADER>(header+sizeof(DWORD));
    PeArchitecture result;result.machine=file.Machine;result.dll=!!(file.Characteristics&IMAGE_FILE_DLL);
    ULONGLONG optional=header+sizeof(DWORD)+sizeof(file);
    WORD magic=reader.at<WORD>(optional);DWORD directory_count=0,size_of_headers=0;IMAGE_DATA_DIRECTORY clr{};
    size_t directory_offset=0;
    if(magic==IMAGE_NT_OPTIONAL_HDR32_MAGIC){
        directory_offset=offsetof(IMAGE_OPTIONAL_HEADER32,DataDirectory);
        if(file.SizeOfOptionalHeader<directory_offset)throw std::runtime_error("Truncated PE32 optional header");
        directory_count=reader.at<DWORD>(optional+offsetof(IMAGE_OPTIONAL_HEADER32,NumberOfRvaAndSizes));
        size_of_headers=reader.at<DWORD>(optional+offsetof(IMAGE_OPTIONAL_HEADER32,SizeOfHeaders));
    }else if(magic==IMAGE_NT_OPTIONAL_HDR64_MAGIC){
        directory_offset=offsetof(IMAGE_OPTIONAL_HEADER64,DataDirectory);
        if(file.SizeOfOptionalHeader<directory_offset)throw std::runtime_error("Truncated PE32+ optional header");
        directory_count=reader.at<DWORD>(optional+offsetof(IMAGE_OPTIONAL_HEADER64,NumberOfRvaAndSizes));
        size_of_headers=reader.at<DWORD>(optional+offsetof(IMAGE_OPTIONAL_HEADER64,SizeOfHeaders));
    }else throw std::runtime_error("Unsupported PE optional header");
    if(directory_count<=IMAGE_DIRECTORY_ENTRY_COM_DESCRIPTOR)return result;
    size_t clr_offset=directory_offset+IMAGE_DIRECTORY_ENTRY_COM_DESCRIPTOR*sizeof(IMAGE_DATA_DIRECTORY);
    if(file.SizeOfOptionalHeader<clr_offset+sizeof(clr))throw std::runtime_error("Truncated CLR data directory");
    clr=reader.at<IMAGE_DATA_DIRECTORY>(optional+clr_offset);
    if(!clr.VirtualAddress&&!clr.Size)return result;
    if(!clr.VirtualAddress||clr.Size<sizeof(ClrPrefix))throw std::runtime_error("Invalid CLR architecture header");
    ULONGLONG location=0;bool found=false;
    if(clr.VirtualAddress<size_of_headers&&sizeof(ClrPrefix)<=size_of_headers-clr.VirtualAddress){location=clr.VirtualAddress;found=true;}
    ULONGLONG sections=optional+file.SizeOfOptionalHeader;
    for(unsigned i=0;!found&&i<file.NumberOfSections;i++){
        auto section=reader.at<IMAGE_SECTION_HEADER>(sections+i*sizeof(IMAGE_SECTION_HEADER));
        if(clr.VirtualAddress<section.VirtualAddress)continue;
        ULONGLONG displacement=(ULONGLONG)clr.VirtualAddress-section.VirtualAddress;
        if(displacement>section.SizeOfRawData||sizeof(ClrPrefix)>section.SizeOfRawData-displacement)continue;
        location=(ULONGLONG)section.PointerToRawData+displacement;found=true;
    }
    if(!found)throw std::runtime_error("CLR architecture header is outside file sections");
    auto cor=reader.at<ClrPrefix>(location);
    if(cor.size<sizeof(ClrPrefix)||cor.size>clr.Size)throw std::runtime_error("Invalid CLR architecture header size");
    result.managed=true;result.clr_flags=cor.flags;return result;
}
WORD program_machine(const str &path) {
    SYSTEM_INFO native{};GetNativeSystemInfo(&native);WORD machine;
    switch(native.wProcessorArchitecture){
    case PROCESSOR_ARCHITECTURE_AMD64:machine=IMAGE_FILE_MACHINE_AMD64;break;
    case PROCESSOR_ARCHITECTURE_ARM64:machine=IMAGE_FILE_MACHINE_ARM64;break;
    case PROCESSOR_ARCHITECTURE_IA64:machine=IMAGE_FILE_MACHINE_IA64;break;
    default:machine=IMAGE_FILE_MACHINE_I386;break;
    }
    return inspect_pe_architecture(path).process_machine(machine);
}
}
