// SPDX-License-Identifier: LGPL-2.1-or-later
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <vector>
#include <string>
#include <stdexcept>
using namespace Gdiplus;

#define FLAT_APIS(X) \
 X(GdipCreateBitmapFromScan0) X(GdipGetImageGraphicsContext) X(GdipDisposeImage) \
 X(GdipDeleteGraphics) X(GdipGraphicsClear) X(GdipSetSmoothingMode) \
 X(GdipCreatePath) X(GdipDeletePath) X(GdipAddPathRectangle) X(GdipFillPath) \
 X(GdipCreateSolidFill) X(GdipDeleteBrush) X(GdipSetClipRect) X(GdipResetClip) \
 X(GdipCreateMatrix2) X(GdipSetWorldTransform) X(GdipResetWorldTransform) X(GdipDeleteMatrix) \
 X(GdipBitmapLockBits) X(GdipBitmapUnlockBits) X(GdipCloneImage) \
 X(GdipGetImageEncodersSize) X(GdipGetImageEncoders) X(GdipSaveImageToStream) \
 X(GdipSaveImageToFile) X(GdipLoadImageFromStream) X(GdipLoadImageFromFile) \
 X(GdipGetImageWidth) X(GdipGetImageHeight) \
 X(GdipNewPrivateFontCollection) X(GdipDeletePrivateFontCollection) X(GdipPrivateAddFontFile) X(GdipPrivateAddMemoryFont) \
 X(GdipGetFontCollectionFamilyCount) X(GdipGetFontCollectionFamilyList) X(GdipDeleteFontFamily) \
 X(GdipGetFamilyName) X(GdipCreateFontFamilyFromName) X(GdipGetGenericFontFamilySansSerif) X(GdipCloneFontFamily) X(GdipCreateFont) X(GdipDeleteFont) X(GdipAddPathString) \
 X(GdipGetPathWorldBounds) X(GdipGetPointCount) X(GdipStringFormatGetGenericDefault) \
 X(GdipDeleteStringFormat) X(GdipMeasureString) X(GdipSetStringFormatMeasurableCharacterRanges) \
 X(GdipCreateRegion) X(GdipDeleteRegion) X(GdipMeasureCharacterRanges) X(GdipGetRegionBounds)

static void require(bool value,const char *message)
{if(!value)throw std::runtime_error(message);}
static void okay(GpStatus value,const char *message)
{if(value!=Ok){std::fprintf(stderr,"%s status=%d\n",message,value);throw std::runtime_error(message);}}
static std::wstring path_of(HMODULE module)
{std::vector<wchar_t> path(32768);DWORD n=GetModuleFileNameW(module,path.data(),(DWORD)path.size());require(n&&n<path.size(),"module path");return {path.data(),n};}
static bool same_file(const std::wstring &left,const std::wstring &right)
{
    HANDLE a=CreateFileW(left.c_str(),FILE_READ_ATTRIBUTES,7,nullptr,OPEN_EXISTING,0,nullptr);
    HANDLE b=CreateFileW(right.c_str(),FILE_READ_ATTRIBUTES,7,nullptr,OPEN_EXISTING,0,nullptr);
    BY_HANDLE_FILE_INFORMATION x{},y{};
    bool result=a!=INVALID_HANDLE_VALUE&&b!=INVALID_HANDLE_VALUE&&GetFileInformationByHandle(a,&x)&&GetFileInformationByHandle(b,&y)
        &&x.dwVolumeSerialNumber==y.dwVolumeSerialNumber&&x.nFileIndexHigh==y.nFileIndexHigh&&x.nFileIndexLow==y.nFileIndexLow;
    if(a!=INVALID_HANDLE_VALUE)CloseHandle(a);if(b!=INVALID_HANDLE_VALUE)CloseHandle(b);return result;
}
#ifdef WINEDOWS_STATIC_GDIPLUS
static FARPROC imported_startup()
{
    auto base=(BYTE*)GetModuleHandleW(nullptr);auto dos=(IMAGE_DOS_HEADER*)base;auto nt=(IMAGE_NT_HEADERS*)(base+dos->e_lfanew);
    auto rva=nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    for(auto desc=(IMAGE_IMPORT_DESCRIPTOR*)(base+rva);rva&&desc->Name;desc++){
        if(_stricmp((char*)base+desc->Name,"gdiplus.dll")||!desc->OriginalFirstThunk)continue;
        auto names=(IMAGE_THUNK_DATA*)(base+desc->OriginalFirstThunk),values=(IMAGE_THUNK_DATA*)(base+desc->FirstThunk);
        for(;names->u1.AddressOfData;names++,values++)if(!IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)
            &&!strcmp((char*)((IMAGE_IMPORT_BY_NAME*)(base+names->u1.AddressOfData))->Name,"GdiplusStartup"))return (FARPROC)values->u1.Function;
    }
    return nullptr;
}
#endif
struct Api {
    HMODULE module=nullptr;
    decltype(&Gdiplus::GdiplusStartup) startup;
    decltype(&Gdiplus::GdiplusShutdown) shutdown;
#define DECLARE(name) decltype(&DllExports::name) name;
    FLAT_APIS(DECLARE)
#undef DECLARE
    explicit Api(const wchar_t *expected){
#ifdef WINEDOWS_STATIC_GDIPLUS
        auto address=imported_startup();require(address!=nullptr,"static GDI+ startup IAT");
        require(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,(LPCWSTR)address,&module),"static GDI+ owner");
        startup=&Gdiplus::GdiplusStartup;shutdown=&Gdiplus::GdiplusShutdown;
#define LOAD(name) name=&DllExports::name;
#else
        module=LoadLibraryExW(expected,nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);
        require(module!=nullptr,"dynamic GDI+ load");
        startup=(decltype(startup))GetProcAddress(module,"GdiplusStartup");
        shutdown=(decltype(shutdown))GetProcAddress(module,"GdiplusShutdown");
#define LOAD(name) name=(decltype(name))GetProcAddress(module,#name);require(name!=nullptr,#name " export");
#endif
        FLAT_APIS(LOAD)
#undef LOAD
        std::wprintf(L"Provider=%ls\n",path_of(module).c_str());
        require(startup&&shutdown&&same_file(path_of(module),expected),"GDI+ provider file identity");
    }
    ~Api(){if(module)FreeLibrary(module);}
};
static GpImage *image(GpBitmap *bitmap){return reinterpret_cast<GpImage*>(bitmap);}
static std::vector<ARGB> pixels(Api &api,GpBitmap *bitmap,UINT width,UINT height)
{
    Rect rect(0,0,width,height);BitmapData data{};
    okay(api.GdipBitmapLockBits(bitmap,&rect,ImageLockModeRead,PixelFormat32bppARGB,&data),"lock bitmap");
    require(data.Width==width&&data.Height==height,"locked bitmap dimensions");
    std::vector<ARGB> result(width*height);
    for(UINT y=0;y<height;y++)std::memcpy(result.data()+y*width,(BYTE*)data.Scan0+(ptrdiff_t)y*data.Stride,width*4);
    okay(api.GdipBitmapUnlockBits(bitmap,&data),"unlock bitmap");return result;
}
static CLSID png_encoder(Api &api)
{
    UINT count=0,size=0;okay(api.GdipGetImageEncodersSize(&count,&size),"encoder size");
    require(count&&count<100&&size>=count*sizeof(ImageCodecInfo)&&size<1024*1024,"bounded encoder inventory");
    std::vector<BYTE> storage(size);auto codecs=(ImageCodecInfo*)storage.data();
    okay(api.GdipGetImageEncoders(count,size,codecs),"encoder inventory");
    for(UINT i=0;i<count;i++)if(codecs[i].MimeType&&!wcscmp(codecs[i].MimeType,L"image/png"))return codecs[i].Clsid;
    throw std::runtime_error("PNG encoder absent");
}
static void dimensions(Api &api,GpImage *value,UINT expected_width,UINT expected_height)
{
    UINT width=0,height=0;okay(api.GdipGetImageWidth(value,&width),"image width");okay(api.GdipGetImageHeight(value,&height),"image height");
    require(width==expected_width&&height==expected_height,"decoded image dimensions");
}
static void path_and_codec(Api &api,const wchar_t *output)
{
    GpBitmap *bitmap=nullptr;GpGraphics *graphics=nullptr;GpPath *path=nullptr;GpSolidFill *brush=nullptr;GpMatrix *matrix=nullptr;
    okay(api.GdipCreateBitmapFromScan0(16,16,0,PixelFormat32bppARGB,nullptr,&bitmap),"create ARGB bitmap");
    okay(api.GdipGetImageGraphicsContext(image(bitmap),&graphics),"bitmap graphics");
    okay(api.GdipGraphicsClear(graphics,0x7f102030),"clear known translucent background");
    okay(api.GdipSetSmoothingMode(graphics,SmoothingModeNone),"pixel aligned drawing");
    okay(api.GdipCreatePath(FillModeAlternate,&path),"create path");
    okay(api.GdipAddPathRectangle(path,0,0,12,12),"rectangle geometry");
    okay(api.GdipCreateMatrix2(1,0,0,1,2,2,&matrix),"translation matrix");
    okay(api.GdipSetWorldTransform(graphics,matrix),"graphics translation");
    okay(api.GdipSetClipRect(graphics,2,2,8,8,CombineModeReplace),"translated clipping");
    okay(api.GdipCreateSolidFill(0xffe13a65,&brush),"opaque brush");
    okay(api.GdipFillPath(graphics,(GpBrush*)brush,path),"fill clipped transformed path");
    okay(api.GdipResetWorldTransform(graphics),"reset transform");okay(api.GdipResetClip(graphics),"reset clip");
    okay(api.GdipDeleteGraphics(graphics),"delete graphics");graphics=nullptr;
    auto actual=pixels(api,bitmap,16,16);std::vector<ARGB> expected(256);
    for(unsigned y=0;y<16;y++)for(unsigned x=0;x<16;x++){
        expected[y*16+x]=x>=4&&x<12&&y>=4&&y<12?0xffe13a65:0x7f102030;
        if(actual[y*16+x]!=expected[y*16+x]){std::fprintf(stderr,"pixel %u,%u got=%08lx expected=%08lx\n",x,y,(ULONG)actual[y*16+x],(ULONG)expected[y*16+x]);throw std::runtime_error("independent path/clip pixels");}
    }
    GpImage *clone=nullptr;okay(api.GdipCloneImage(image(bitmap),&clone),"clone image");
    require(pixels(api,(GpBitmap*)clone,16,16)==expected,"clone preserves ARGB pixels");okay(api.GdipDisposeImage(clone),"dispose clone");
    CLSID encoder=png_encoder(api);IStream *stream=nullptr;
    require(SUCCEEDED(CreateStreamOnHGlobal(nullptr,TRUE,&stream)),"create codec stream");
    okay(api.GdipSaveImageToStream(image(bitmap),stream,&encoder,nullptr),"encode PNG stream");
    STATSTG stat{};require(SUCCEEDED(stream->Stat(&stat,STATFLAG_NONAME))&&stat.cbSize.QuadPart>32&&stat.cbSize.QuadPart<1024*1024,"encoded stream length");
    LARGE_INTEGER zero{};require(SUCCEEDED(stream->Seek(zero,STREAM_SEEK_SET,nullptr)),"rewind PNG");
    BYTE signature[8]{};ULONG read=0;require(SUCCEEDED(stream->Read(signature,8,&read))&&read==8&&!memcmp(signature,"\x89PNG\r\n\x1a\n",8),"real PNG signature");
    require(SUCCEEDED(stream->Seek(zero,STREAM_SEEK_SET,nullptr)),"rewind decoder input");
    GpImage *decoded=nullptr;okay(api.GdipLoadImageFromStream(stream,&decoded),"decode PNG stream");
    dimensions(api,decoded,16,16);require(pixels(api,(GpBitmap*)decoded,16,16)==expected,"PNG stream exact ARGB roundtrip");
    okay(api.GdipDisposeImage(decoded),"dispose stream image");stream->Release();
    okay(api.GdipSaveImageToFile(image(bitmap),output,&encoder,nullptr),"encode prefix PNG file");
    decoded=nullptr;okay(api.GdipLoadImageFromFile(output,&decoded),"decode prefix PNG file");
    dimensions(api,decoded,16,16);require(pixels(api,(GpBitmap*)decoded,16,16)==expected,"PNG file exact ARGB roundtrip");
    okay(api.GdipDisposeImage(decoded),"dispose file image");
    okay(api.GdipDeleteMatrix(matrix),"delete matrix");okay(api.GdipDeleteBrush((GpBrush*)brush),"delete brush");
    okay(api.GdipDeletePath(path),"delete path");okay(api.GdipDisposeImage(image(bitmap)),"dispose bitmap");
    bitmap=nullptr;require(api.GdipCreateBitmapFromScan0(0,16,0,PixelFormat32bppARGB,nullptr,&bitmap)==InvalidParameter,"invalid bitmap dimensions");
    std::puts("PASS transformed path / clipping / 256 exact ARGB pixels / clone / PNG stream and prefix file");
}
static void private_font(Api &api,const wchar_t *font_path)
{
    GpFontCollection *collection=nullptr;GpFontFamily *family=nullptr;GpFont *font=nullptr;
    GpStatus absent=api.GdipCreateFontFamilyFromName(L"wine_test",nullptr,&family);
    std::printf("Host test-font lookup status=%d\n",absent);
    require(absent==FontFamilyNotFound,
            "private fixture font must not exist in the host collection");family=nullptr;
    okay(api.GdipGetGenericFontFamilySansSerif(&family),"installed generic sans-serif family");
    okay(api.GdipDeleteFontFamily(family),"release installed family");family=nullptr;
    okay(api.GdipNewPrivateFontCollection(&collection),"private font collection");
    SYSTEM_INFO system{};GetSystemInfo(&system);
    BYTE *guarded=(BYTE*)VirtualAlloc(nullptr,system.dwPageSize*2,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    require(guarded!=nullptr,"bounded font parser fixture allocation");DWORD protection=0;
    require(VirtualProtect(guarded+system.dwPageSize,system.dwPageSize,PAGE_NOACCESS,&protection),"font parser guard page");
    // Valid SFNT signature, but a directory count that cannot fit the supplied
    // twelve bytes. Native reports Ok but adds no family for this malformed
    // input; Wine reports an error. Both must keep the collection empty and
    // avoid reading the guard page.
    BYTE *truncated=guarded+system.dwPageSize-12;std::memset(truncated,0,12);
    truncated[1]=1;truncated[4]=truncated[5]=0xff;
    GpStatus malformed_status=api.GdipPrivateAddMemoryFont(collection,truncated,12);INT malformed_count=-1;
    okay(api.GdipGetFontCollectionFamilyCount(collection,&malformed_count),"malformed-font collection count");
    std::printf("Truncated font status=%d families=%d\n",malformed_status,malformed_count);
    require(malformed_count==0,"truncated font must not add a family");
    VirtualFree(guarded,0,MEM_RELEASE);
    okay(api.GdipPrivateAddFontFile(collection,font_path),"load private prefix font file");
    INT count=0;okay(api.GdipGetFontCollectionFamilyCount(collection,&count),"private family count");require(count==1,"fixture has one private family");
    INT found=0;okay(api.GdipGetFontCollectionFamilyList(collection,1,&family,&found),"private font family");require(found==1&&family,"private family object");
    // Collection enumeration borrows its family. Clone before retaining an owned
    // reference; deleting the borrowed pointer would invalidate native cleanup.
    GpFontFamily *owned_family=nullptr;okay(api.GdipCloneFontFamily(family,&owned_family),"retain private family");family=owned_family;
    WCHAR name[LF_FACESIZE]{};okay(api.GdipGetFamilyName(family,name,0),"private family name");require(!wcscmp(name,L"wine_test"),"exact private fixture font name");
    std::wprintf(L"Private font=%ls\n",name);
    okay(api.GdipCreateFont(family,32,FontStyleRegular,UnitPixel,&font),"private font object");
    GpBitmap *bitmap=nullptr;GpGraphics *graphics=nullptr;GpPath *path=nullptr;GpSolidFill *brush=nullptr;GpStringFormat *format=nullptr;GpRegion *region=nullptr;
    okay(api.GdipCreateBitmapFromScan0(128,64,0,PixelFormat32bppARGB,nullptr,&bitmap),"font bitmap");
    okay(api.GdipGetImageGraphicsContext(image(bitmap),&graphics),"font graphics");
    okay(api.GdipGraphicsClear(graphics,0xff000000),"font background");
    okay(api.GdipSetSmoothingMode(graphics,SmoothingModeNone),"font path raster mode");
    okay(api.GdipStringFormatGetGenericDefault(&format),"string format");
    RectF layout(4,4,120,56),measured;INT fitted=0,lines=0;
    okay(api.GdipMeasureString(graphics,L"ADAD",4,font,&layout,format,&measured,&fitted,&lines),"measure private font");
    require(fitted==4&&lines==1&&measured.Width>20&&measured.Width<120&&measured.Height>10&&measured.Height<56,"bounded measured text");
    CharacterRange range(1,2);okay(api.GdipSetStringFormatMeasurableCharacterRanges(format,1,&range),"character range");
    okay(api.GdipCreateRegion(&region),"text region");
    okay(api.GdipMeasureCharacterRanges(graphics,L"ADAD",4,font,layout,format,1,&region),"measure character region");
    RectF region_bounds;okay(api.GdipGetRegionBounds(region,graphics,&region_bounds),"character region bounds");
    require(region_bounds.Width>5&&region_bounds.Width<measured.Width&&region_bounds.Height>10,"nonempty subrange");
    okay(api.GdipCreatePath(FillModeAlternate,&path),"font outline path");
    okay(api.GdipAddPathString(path,L"ADAD",4,family,FontStyleRegular,32,&layout,format),"private text outlines");
    INT points=0;okay(api.GdipGetPointCount(path,&points),"font outline points");require(points>20,"nontrivial glyph geometry");
    RectF bounds;okay(api.GdipGetPathWorldBounds(path,&bounds,nullptr,nullptr),"outline bounds");
    require(bounds.Width>20&&bounds.Height>10&&bounds.X>=0&&bounds.Y>=0&&bounds.X+bounds.Width<128&&bounds.Y+bounds.Height<64,"bounded text geometry");
    okay(api.GdipCreateSolidFill(0xff28e870,&brush),"font fill brush");okay(api.GdipFillPath(graphics,(GpBrush*)brush,path),"rasterize private text outlines");
    okay(api.GdipDeleteGraphics(graphics),"release text graphics");
    auto rendered=pixels(api,bitmap,128,64);unsigned ink=0;
    for(unsigned y=0;y<64;y++)for(unsigned x=0;x<128;x++)if(rendered[y*128+x]!=0xff000000){
        ink++;require(x+2>=bounds.X&&y+2>=bounds.Y&&x<=bounds.X+bounds.Width+2&&y<=bounds.Y+bounds.Height+2,"glyph pixels within independent geometry");
    }
    require(ink>20&&ink<1000,"nonempty bounded glyph rasterization");
    std::printf("PASS private-font outline points=%d ink=%u text/range metrics bounded\n",points,ink);
    okay(api.GdipDeleteRegion(region),"delete region");okay(api.GdipDeleteStringFormat(format),"delete string format");
    okay(api.GdipDeleteBrush((GpBrush*)brush),"delete text brush");okay(api.GdipDeletePath(path),"delete text path");
    okay(api.GdipDisposeImage(image(bitmap)),"dispose text bitmap");okay(api.GdipDeleteFont(font),"delete font");
    okay(api.GdipDeletePrivateFontCollection(&collection),"delete private collection");
    okay(api.GdipGetFamilyName(family,name,0),"cloned family survives collection");okay(api.GdipDeleteFontFamily(family),"delete owned family");
}
int wmain(int argc,wchar_t **argv)
{
    std::setvbuf(stdout,nullptr,_IONBF,0);std::setvbuf(stderr,nullptr,_IONBF,0);
    try{
        if(argc==2&&!wcscmp(argv[1],L"--native-path")){
            wchar_t system[MAX_PATH]{};UINT n=GetSystemDirectoryW(system,MAX_PATH);require(n&&n<MAX_PATH,"native system directory");
            auto path=std::wstring(system)+L"\\gdiplus.dll";
            HMODULE native=LoadLibraryExW(path.c_str(),nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);require(native!=nullptr,"native GDI+ discovery");
            std::wprintf(L"%ls\n",path_of(native).c_str());FreeLibrary(native);return 0;
        }
        require(argc==4,"usage: gdiplus-path font-file output-png");SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
        require(SUCCEEDED(CoInitializeEx(nullptr,COINIT_MULTITHREADED)),"initialize COM");
        {Api api(argv[1]);GdiplusStartupInput input;ULONG_PTR token=0;
        okay(api.startup(&token,&input,nullptr),"GDI+ startup");require(token!=0,"startup token");
        path_and_codec(api,argv[3]);private_font(api,argv[2]);api.shutdown(token);}
        CoUninitialize();std::puts("PASS GDI+ helper provider / paths / codecs / private fonts");return 0;
    }catch(const std::exception &error){std::fprintf(stderr,"FAIL %s winerror=%lu\n",error.what(),GetLastError());return 1;}
}
