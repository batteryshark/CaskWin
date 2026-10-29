// SPDX-License-Identifier: LGPL-2.1-or-later
#include "pe-architecture.h"
static unsigned failures;
static void check(bool value,const char *name){printf("%s %s\n",value?"PASS":"FAIL",name);if(!value)++failures;}
int wmain(int argc,wchar_t **argv){
    if(argc<2)return 2;wd::str directory=argv[1];
    std::vector<wchar_t> self(32768);GetModuleFileNameW(nullptr,self.data(),(DWORD)self.size());
    auto native=wd::inspect_pe_architecture(self.data());
    WORD own_machine=sizeof(void*)==8?IMAGE_FILE_MACHINE_AMD64:IMAGE_FILE_MACHINE_I386;
    check(!native.managed&&native.machine==own_machine&&wd::program_machine(self.data())==own_machine,"native executable architecture remains unchanged");
    struct Fixture {const wchar_t *name;WORD image,effective64,effective32;bool dll;};
    Fixture fixtures[]={
        {L"anycpu.exe",IMAGE_FILE_MACHINE_I386,IMAGE_FILE_MACHINE_AMD64,IMAGE_FILE_MACHINE_I386,false},
        {L"x86.exe",IMAGE_FILE_MACHINE_I386,IMAGE_FILE_MACHINE_I386,IMAGE_FILE_MACHINE_I386,false},
        {L"preferred.exe",IMAGE_FILE_MACHINE_I386,IMAGE_FILE_MACHINE_I386,IMAGE_FILE_MACHINE_I386,false},
        {L"x64.exe",IMAGE_FILE_MACHINE_AMD64,IMAGE_FILE_MACHINE_AMD64,IMAGE_FILE_MACHINE_AMD64,false},
        {L"anycpu.dll",IMAGE_FILE_MACHINE_I386,IMAGE_FILE_MACHINE_I386,IMAGE_FILE_MACHINE_I386,true},
        {L"x64.dll",IMAGE_FILE_MACHINE_AMD64,IMAGE_FILE_MACHINE_AMD64,IMAGE_FILE_MACHINE_AMD64,true},
        {L"x64-preferred-invalid.exe",IMAGE_FILE_MACHINE_AMD64,IMAGE_FILE_MACHINE_AMD64,IMAGE_FILE_MACHINE_AMD64,false},
        {L"mixed-metadata.exe",IMAGE_FILE_MACHINE_I386,IMAGE_FILE_MACHINE_I386,IMAGE_FILE_MACHINE_I386,false}
    };
    for(const auto &fixture:fixtures){
        auto path=directory+L"\\"+fixture.name;auto arch=wd::inspect_pe_architecture(path);
        printf("FILE %ls image=%04x clr=%08lx dll=%u host-choice=%04x\n",fixture.name,arch.machine,arch.clr_flags,arch.dll,wd::program_machine(path));
        check(arch.machine==fixture.image&&arch.dll==fixture.dll,"declared image architecture preserved");
        check(arch.process_machine(IMAGE_FILE_MACHINE_AMD64)==fixture.effective64,"effective architecture on x64 Windows");
        check(arch.process_machine(IMAGE_FILE_MACHINE_I386)==fixture.effective32,"effective architecture on x86 Windows");
        check(wd::program_machine(path)==fixture.effective64,"actual x64 host detected from either probe bitness");
    }
    for(auto name:{L"truncated.exe",L"bad-clr-rva.exe"}){
        bool rejected=false;try{wd::inspect_pe_architecture(directory+L"\\"+name);}catch(const std::exception&){rejected=true;}
        check(rejected,"malformed architecture metadata rejected");
    }
    if(argc>2){auto arch=wd::inspect_pe_architecture(argv[2]);printf("GAME image=%04x clr=%08lx effective=%04x\n",arch.machine,arch.clr_flags,wd::program_machine(argv[2]));check(wd::program_machine(argv[2])==IMAGE_FILE_MACHINE_AMD64,"Blossom AnyCPU executable selects x64 middleware");}
    printf("Architecture failures: %u\n",failures);return failures?1:0;
}
