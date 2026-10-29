// SPDX-License-Identifier: LGPL-2.1-or-later
// Owned mathematical / geometry fixtures; no input, capture or host modesets.
#include <windows.h>
#include <psapi.h>
#include <GL/gl.h>
#include <GL/glu.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

#ifdef PREFER_DISCRETE
extern "C" { __declspec(dllexport) DWORD NvOptimusEnablement=1; __declspec(dllexport) int AmdPowerXpressRequestHighPerformance=1; }
#endif
using GluCallback=void (CALLBACK *)(void);
static unsigned failures,checks;
static void check(bool value,const char *name){++checks;if(!value){++failures;std::printf("FAIL %s error=%lu\n",name,GetLastError());}}
static bool close_to(double a,double b,double epsilon=1e-7){return std::isfinite(a)&&std::fabs(a-b)<=epsilon;}
#define GLU_FUNCTIONS(X) \
 X(gluProject) X(gluUnProject) X(gluGetString) X(gluErrorString) X(gluErrorUnicodeStringEXT) \
 X(gluNewTess) X(gluDeleteTess) X(gluTessCallback) X(gluTessProperty) X(gluGetTessProperty) \
 X(gluTessNormal) X(gluTessBeginPolygon) X(gluTessBeginContour) X(gluTessVertex) X(gluTessEndContour) X(gluTessEndPolygon) \
 X(gluNewQuadric) X(gluDeleteQuadric) X(gluQuadricCallback) X(gluQuadricDrawStyle) X(gluQuadricNormals) X(gluQuadricOrientation) X(gluQuadricTexture) \
 X(gluSphere) X(gluCylinder) X(gluDisk) X(gluNewNurbsRenderer) X(gluDeleteNurbsRenderer) \
 X(gluScaleImage) X(gluBuild1DMipmaps) X(gluBuild2DMipmaps) X(gluOrtho2D) X(gluPerspective) X(gluLookAt) X(gluPickMatrix)
#define DECLARE(name) static decltype(&name) p_##name;
GLU_FUNCTIONS(DECLARE)
#undef DECLARE
static std::wstring mapped_path(HMODULE module){std::vector<wchar_t> name(32768);DWORD n=GetMappedFileNameW(GetCurrentProcess(),module,name.data(),(DWORD)name.size());return n?L"\\\\?\\GLOBALROOT"+std::wstring(name.data(),n):L"";}
static bool same_file(const std::wstring &a,const std::wstring &b){
    HANDLE x=CreateFileW(a.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr);
    HANDLE y=CreateFileW(b.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr);
    BY_HANDLE_FILE_INFORMATION ix{},iy{};bool ok=x!=INVALID_HANDLE_VALUE&&y!=INVALID_HANDLE_VALUE&&GetFileInformationByHandle(x,&ix)&&GetFileInformationByHandle(y,&iy)
        &&ix.dwVolumeSerialNumber==iy.dwVolumeSerialNumber&&ix.nFileIndexHigh==iy.nFileIndexHigh&&ix.nFileIndexLow==iy.nFileIndexLow;
    if(x!=INVALID_HANDLE_VALUE)CloseHandle(x);if(y!=INVALID_HANDLE_VALUE)CloseHandle(y);return ok;
}
static HMODULE owner(const void *address){HMODULE module=nullptr;GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,(LPCWSTR)address,&module);return module;}
static const void *import_target(const void *address){
    // MinGW's GLU header omits dllimport, so a statically linked address can
    // name the executable's generated jump thunk. Inspect its actual IAT slot.
    if(owner(address)!=GetModuleHandleW(nullptr))return address;
    const BYTE *code=(const BYTE*)address;if(code[0]!=0xff||code[1]!=0x25)return address;
#ifdef _WIN64
    int32_t offset;std::memcpy(&offset,code+2,sizeof(offset));return *(const void *const *)(code+6+offset);
#else
    const void *const *slot;std::memcpy(&slot,code+2,sizeof(slot));return *slot;
#endif
}
static bool verify_gl_imports(HMODULE module,const std::wstring &native){
    auto base=(BYTE*)module;auto dos=(IMAGE_DOS_HEADER*)base;auto nt=(IMAGE_NT_HEADERS*)(base+dos->e_lfanew);
    DWORD rva=nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;unsigned count=0;bool ok=true;
    for(auto desc=(IMAGE_IMPORT_DESCRIPTOR*)(base+rva);rva&&desc->Name;++desc){
        if(_stricmp((char*)(base+desc->Name),"opengl32.dll"))continue;
        auto thunk=(IMAGE_THUNK_DATA*)(base+desc->FirstThunk);
        for(;thunk->u1.Function;++thunk){++count;ok=ok&&same_file(mapped_path(owner((const void*)thunk->u1.Function)),native);}
    }
    std::printf("OPENGL_IMPORTS count=%u native=%d\n",count,ok);return count>0&&ok;
}
static void math_tests(){
    const GLdouble identity[16]={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};GLint viewport[4]={7,11,640,480};
    GLdouble x=0,y=0,z=0,ox=0,oy=0,oz=0;
    check(p_gluProject(.25,-.5,.75,identity,identity,viewport,&x,&y,&z)==GL_TRUE,"project identity result");
    check(close_to(x,407)&&close_to(y,131)&&close_to(z,.875),"project independent expected coordinates");
    check(p_gluUnProject(x,y,z,identity,identity,viewport,&ox,&oy,&oz)==GL_TRUE&&close_to(ox,.25)&&close_to(oy,-.5)&&close_to(oz,.75),"unproject identity inverse");
    GLdouble model[16];std::memcpy(model,identity,sizeof(model));model[12]=2;model[13]=-3;model[14]=-5;
    const GLdouble perspective[16]={.5,0,0,0,0,1,0,0,0,0,-1.25,-1,0,0,-2.25,0};
    check(p_gluProject(0,0,0,model,perspective,viewport,&x,&y,&z)==GL_TRUE&&close_to(x,391)&&close_to(y,107)&&close_to(z,.9),"project translated perspective");
    check(p_gluUnProject(x,y,z,model,perspective,viewport,&ox,&oy,&oz)==GL_TRUE&&close_to(ox,0)&&close_to(oy,0)&&close_to(oz,0),"unproject translated perspective");
    GLdouble zero[16]{};check(!p_gluUnProject(0,0,0,zero,identity,viewport,&ox,&oy,&oz),"unproject singular matrix fails");
    check(!p_gluProject(0,0,0,identity,zero,viewport,&x,&y,&z),"project zero homogeneous divisor fails");
    std::printf("MATH translated=391,107,0.9 inverse=0,0,0\n");
}
using Vertex=std::array<GLdouble,3>;
struct TessResult{GLenum primitive=0;std::vector<Vertex> primitive_vertices,triangles;std::deque<Vertex> generated;unsigned errors=0,combines=0;bool valid_data=true;};
static void CALLBACK tess_begin(GLenum primitive,void *data){auto &r=*(TessResult*)data;r.primitive=primitive;r.primitive_vertices.clear();}
static void CALLBACK tess_vertex(void *vertex,void *data){auto &r=*(TessResult*)data;if(!vertex){r.valid_data=false;return;}r.primitive_vertices.push_back(*(Vertex*)vertex);}
static void CALLBACK tess_end(void *data){auto &r=*(TessResult*)data;auto &v=r.primitive_vertices;
    auto triangle=[&](size_t a,size_t b,size_t c){r.triangles.insert(r.triangles.end(),{v[a],v[b],v[c]});};
    if(r.primitive==GL_TRIANGLES){if(v.size()%3)r.valid_data=false;for(size_t i=0;i+2<v.size();i+=3)triangle(i,i+1,i+2);}
    else if(r.primitive==GL_TRIANGLE_FAN){for(size_t i=2;i<v.size();++i)triangle(0,i-1,i);}
    else if(r.primitive==GL_TRIANGLE_STRIP){for(size_t i=2;i<v.size();++i)if(i%2)triangle(i-1,i-2,i);else triangle(i-2,i-1,i);}
    else r.valid_data=false;
}
static void CALLBACK tess_error(GLenum error,void *data){if(data){++((TessResult*)data)->errors;std::printf("TESS_ERROR %u\n",error);}}
static void CALLBACK tess_combine(GLdouble coords[3],void *[4],GLfloat weights[4],void **out,void *data){auto &r=*(TessResult*)data;
    r.valid_data=r.valid_data&&close_to(weights[0]+weights[1]+weights[2]+weights[3],1,1e-5);r.generated.push_back({coords[0],coords[1],coords[2]});*out=&r.generated.back();++r.combines;
}
static TessResult tessellate(std::vector<std::vector<Vertex>> contours,const char *name,double expected,bool combine){
    TessResult result;GLUtesselator *tess=p_gluNewTess();check(!!tess,"allocate tessellator");if(!tess)return result;
    p_gluTessCallback(tess,GLU_TESS_BEGIN_DATA,(GluCallback)tess_begin);p_gluTessCallback(tess,GLU_TESS_VERTEX_DATA,(GluCallback)tess_vertex);
    p_gluTessCallback(tess,GLU_TESS_END_DATA,(GluCallback)tess_end);p_gluTessCallback(tess,GLU_TESS_ERROR_DATA,(GluCallback)tess_error);p_gluTessCallback(tess,GLU_TESS_COMBINE_DATA,(GluCallback)tess_combine);
    p_gluTessProperty(tess,GLU_TESS_WINDING_RULE,GLU_TESS_WINDING_ODD);GLdouble property=0;p_gluGetTessProperty(tess,GLU_TESS_WINDING_RULE,&property);check(property==GLU_TESS_WINDING_ODD,"tess winding roundtrip");
    p_gluTessNormal(tess,0,0,1);p_gluTessBeginPolygon(tess,&result);
    for(auto &contour:contours){p_gluTessBeginContour(tess);for(auto &v:contour)p_gluTessVertex(tess,v.data(),&v);p_gluTessEndContour(tess);}p_gluTessEndPolygon(tess);
    double area=0;for(size_t i=0;i+2<result.triangles.size();i+=3){auto a=result.triangles[i],b=result.triangles[i+1],c=result.triangles[i+2];area+=std::fabs((b[0]-a[0])*(c[1]-a[1])-(b[1]-a[1])*(c[0]-a[0]))*.5;}
    check(result.valid_data&&!result.errors&&close_to(area,expected),name);if(combine)check(result.combines>0,"self intersection invokes combine callback");
    std::printf("TESS name=%s area=%.9f triangles=%zu combines=%u errors=%u\n",name,area,result.triangles.size()/3,result.combines,result.errors);p_gluDeleteTess(tess);return result;
}
static GLenum quad_error;
static void CALLBACK quad_callback(GLenum value){quad_error=value;}
static void error_tests(bool wine){
    for(GLenum code:{GL_INVALID_ENUM,GL_INVALID_VALUE,GLU_INVALID_ENUM,GLU_INVALID_VALUE,GLU_OUT_OF_MEMORY,GLU_TESS_ERROR6}){
        auto a=p_gluErrorString(code);auto w=p_gluErrorUnicodeStringEXT(code);bool equal=a&&w&&*a;
        if(equal){size_t n=std::strlen((const char*)a);equal=std::wcslen(w)==n;for(size_t i=0;equal&&i<n;++i)equal=w[i]==a[i];}check(equal,"known ASCII / Unicode error string");
    }
    check(!p_gluErrorString(0xdeadbeef)&&!p_gluErrorUnicodeStringEXT(0xdeadbeef),"unknown error is null");
    check(p_gluGetString(GLU_VERSION)&&*p_gluGetString(GLU_VERSION),"GLU version is available");check(!p_gluGetString(0xdeadbeef),"unknown GLU string is null");
    auto q=p_gluNewQuadric();check(!!q,"allocate quadric");if(q){p_gluQuadricCallback(q,GLU_ERROR,(GluCallback)quad_callback);quad_error=0;p_gluQuadricDrawStyle(q,0xdeadbeef);check(quad_error==GLU_INVALID_ENUM,"quadric invalid enum callback");quad_error=0;p_gluSphere(q,-1,8,8);check(quad_error==GLU_INVALID_VALUE,"quadric invalid radius callback");p_gluDeleteQuadric(q);}
    auto nurbs=p_gluNewNurbsRenderer();std::printf("NURBS provider=%s supported=%d expected_upstream_gap=%d\n",wine?"wine":"native",!!nurbs,wine);
    check(wine?!nurbs:!!nurbs,"NURBS availability matches explicitly documented provider capability");if(nurbs)p_gluDeleteNurbsRenderer(nurbs);
}
static void reset_gl(){glDisable(GL_DEPTH_TEST);glDisable(GL_CULL_FACE);glDisable(GL_LIGHTING);glDisable(GL_TEXTURE_2D);glDisable(GL_DITHER);glDisable(GL_BLEND);glColor3ub(255,0,0);glClearColor(0,0,0,1);glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);glMatrixMode(GL_PROJECTION);glLoadIdentity();glOrtho(-1,1,-1,1,-10,10);glMatrixMode(GL_MODELVIEW);glLoadIdentity();}
static void tight_pack(){glPixelStorei(GL_PACK_ALIGNMENT,1);glPixelStorei(GL_PACK_ROW_LENGTH,0);glPixelStorei(GL_PACK_SKIP_ROWS,0);glPixelStorei(GL_PACK_SKIP_PIXELS,0);glPixelStorei(GL_PACK_SWAP_BYTES,GL_FALSE);glPixelStorei(GL_PACK_LSB_FIRST,GL_FALSE);}
static void check_pack(){GLint a=0,row=0,rows=0,pixels=0;glGetIntegerv(GL_PACK_ALIGNMENT,&a);glGetIntegerv(GL_PACK_ROW_LENGTH,&row);glGetIntegerv(GL_PACK_SKIP_ROWS,&rows);glGetIntegerv(GL_PACK_SKIP_PIXELS,&pixels);check(a==1&&!row&&!rows&&!pixels,"GLU pack state preserved");}
static void pixel(int x,int y,bool red,const char *name){tight_pack();BYTE p[4]{};glReadPixels(x,y,1,1,GL_RGBA,GL_UNSIGNED_BYTE,p);check(red?(p[0]>240&&p[1]<8&&p[2]<8):(p[0]<8&&p[1]<8&&p[2]<8),name);std::printf("PIXEL name=%s rgb=%u,%u,%u\n",name,p[0],p[1],p[2]);}
static void image_tests(){
    tight_pack();glPixelStorei(GL_UNPACK_ALIGNMENT,1);
    const BYTE source[16]={255,0,0,255,0,255,0,255,0,0,255,255,255,255,255,255};BYTE average[4]{};
    check(p_gluScaleImage(GL_RGBA,2,2,GL_UNSIGNED_BYTE,source,1,1,GL_UNSIGNED_BYTE,average)==0,"image downscale result");
    check(std::abs((int)average[0]-128)<=1&&std::abs((int)average[1]-128)<=1&&std::abs((int)average[2]-128)<=1&&average[3]==255,"image downscale independent average");
    std::printf("SCALE average=%u,%u,%u,%u\n",average[0],average[1],average[2],average[3]);
    check(p_gluScaleImage(GL_RGBA,-1,2,GL_UNSIGNED_BYTE,source,1,1,GL_UNSIGNED_BYTE,average)==GLU_INVALID_VALUE,"image negative size error");
    check(p_gluScaleImage(0xdeadbeef,2,2,GL_UNSIGNED_BYTE,source,1,1,GL_UNSIGNED_BYTE,average)==GLU_INVALID_ENUM,"image format error");
    GLuint texture=0;glGenTextures(1,&texture);glBindTexture(GL_TEXTURE_2D,texture);
    // Deliberately padded and offset input: verify GLU consumes and restores
    // the caller's unpack state rather than treating pixels as tightly packed.
    std::vector<BYTE> padded(6*24,0x51);const BYTE color[4]={31,79,151,255};
    for(int y=0;y<5;++y)for(int x=0;x<3;++x)std::memcpy(padded.data()+24*(y+1)+4*(x+1),color,4);
    glPixelStorei(GL_UNPACK_ALIGNMENT,8);glPixelStorei(GL_UNPACK_ROW_LENGTH,5);glPixelStorei(GL_UNPACK_SKIP_ROWS,1);glPixelStorei(GL_UNPACK_SKIP_PIXELS,1);
    check(p_gluBuild2DMipmaps(GL_TEXTURE_2D,GL_RGBA,3,5,GL_RGBA,GL_UNSIGNED_BYTE,padded.data())==0,"Demise GLU mipmap operation");
    check_pack();
    GLint state[4]{};glGetIntegerv(GL_UNPACK_ALIGNMENT,&state[0]);glGetIntegerv(GL_UNPACK_ROW_LENGTH,&state[1]);glGetIntegerv(GL_UNPACK_SKIP_ROWS,&state[2]);glGetIntegerv(GL_UNPACK_SKIP_PIXELS,&state[3]);check(state[0]==8&&state[1]==5&&state[2]==1&&state[3]==1,"mipmap unpack state preserved");
    for(int level=0;level<3;++level){GLint width=0,height=0;glGetTexLevelParameteriv(GL_TEXTURE_2D,level,GL_TEXTURE_WIDTH,&width);glGetTexLevelParameteriv(GL_TEXTURE_2D,level,GL_TEXTURE_HEIGHT,&height);int expected=4>>level;check(width==expected&&height==expected,"mipmap level dimensions");
        if(width==expected&&height==expected){tight_pack();std::vector<BYTE> pixels((size_t)width*height*4);glGetTexImage(GL_TEXTURE_2D,level,GL_RGBA,GL_UNSIGNED_BYTE,pixels.data());bool exact=true;for(size_t i=0;i<pixels.size();++i)exact&=std::abs((int)pixels[i]-color[i%4])<=1;check(exact,"mipmap all texel colors");}
        std::printf("MIPMAP level=%d size=%dx%d expectedColor=31,79,151,255\n",level,width,height);
    }
    check(p_gluBuild2DMipmaps(GL_TEXTURE_2D,GL_RGBA,-1,5,GL_RGBA,GL_UNSIGNED_BYTE,padded.data())==GLU_INVALID_VALUE,"mipmap invalid size error");
    glPixelStorei(GL_UNPACK_ALIGNMENT,1);glPixelStorei(GL_UNPACK_ROW_LENGTH,0);glPixelStorei(GL_UNPACK_SKIP_ROWS,0);glPixelStorei(GL_UNPACK_SKIP_PIXELS,0);
    glDeleteTextures(1,&texture);glGenTextures(1,&texture);glBindTexture(GL_TEXTURE_2D,texture);
    check(p_gluBuild2DMipmaps(GL_TEXTURE_2D,GL_RGBA,2,2,GL_RGBA,GL_UNSIGNED_BYTE,source)==0,"colored mipmap operation");
    check_pack();GLint widths[2]{},heights[2]{};for(int level=0;level<2;++level){glGetTexLevelParameteriv(GL_TEXTURE_2D,level,GL_TEXTURE_WIDTH,&widths[level]);glGetTexLevelParameteriv(GL_TEXTURE_2D,level,GL_TEXTURE_HEIGHT,&heights[level]);}
    bool correct_sizes=widths[0]==2&&heights[0]==2&&widths[1]==1&&heights[1]==1;check(correct_sizes,"colored mipmap bounded readback dimensions");
    if(correct_sizes){tight_pack();BYTE base[16]{},final[4]{};glGetTexImage(GL_TEXTURE_2D,0,GL_RGBA,GL_UNSIGNED_BYTE,base);glGetTexImage(GL_TEXTURE_2D,1,GL_RGBA,GL_UNSIGNED_BYTE,final);
        check(!std::memcmp(base,source,sizeof(base)),"mipmap base exact colored texels");
        check(std::abs((int)final[0]-128)<=1&&std::abs((int)final[1]-128)<=1&&std::abs((int)final[2]-128)<=1&&final[3]==255,"mipmap filtered independent average");
        std::printf("MIPMAP_AVERAGE rgba=%u,%u,%u,%u\n",final[0],final[1],final[2],final[3]);}
    glDeleteTextures(1,&texture);glGenTextures(1,&texture);glBindTexture(GL_TEXTURE_1D,texture);BYTE row[20];for(unsigned i=0;i<20;++i)row[i]=color[i%4];
    check(p_gluBuild1DMipmaps(GL_TEXTURE_1D,GL_RGBA,5,GL_RGBA,GL_UNSIGNED_BYTE,row)==0,"one dimensional mipmap operation");
    for(int level=0;level<3;++level){GLint width=0;glGetTexLevelParameteriv(GL_TEXTURE_1D,level,GL_TEXTURE_WIDTH,&width);check(width==(4>>level),"one dimensional mipmap levels");}glDeleteTextures(1,&texture);
}
static bool gpu_tests(const TessResult &concave){
    WNDCLASSW wc{};wc.style=CS_OWNDC;wc.lpfnWndProc=DefWindowProcW;wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=L"WinedowsGluFixture";RegisterClassW(&wc);
    HWND window=CreateWindowExW(WS_EX_NOACTIVATE,wc.lpszClassName,L"GLU fixture",WS_POPUP,20,20,64,64,nullptr,nullptr,wc.hInstance,nullptr);check(!!window,"owned OpenGL window");if(!window)return false;
    HDC dc=GetDC(window);PIXELFORMATDESCRIPTOR pfd{};pfd.nSize=sizeof(pfd);pfd.nVersion=1;pfd.dwFlags=PFD_DRAW_TO_WINDOW|PFD_SUPPORT_OPENGL|PFD_DOUBLEBUFFER;pfd.iPixelType=PFD_TYPE_RGBA;pfd.cColorBits=32;pfd.cDepthBits=24;
    int format=ChoosePixelFormat(dc,&pfd);bool set=format&&SetPixelFormat(dc,format,&pfd);check(set,"native pixel format");HGLRC context=set?wglCreateContext(dc):nullptr;bool current=context&&wglMakeCurrent(dc,context);check(current,"native OpenGL context");
    if(!current){if(context)wglDeleteContext(context);ReleaseDC(window,dc);DestroyWindow(window);return false;}ShowWindow(window,SW_SHOWNOACTIVATE);glViewport(0,0,64,64);glDrawBuffer(GL_BACK);glReadBuffer(GL_BACK);
    std::printf("OPENGL_RENDERER %s\n",glGetString(GL_RENDERER));std::printf("OPENGL_VERSION %s\n",glGetString(GL_VERSION));
    image_tests();GLdouble matrix[16]{};glMatrixMode(GL_PROJECTION);glLoadIdentity();p_gluPerspective(90,2,1,9);glGetDoublev(GL_PROJECTION_MATRIX,matrix);check(close_to(matrix[0],.5,1e-5)&&close_to(matrix[5],1,1e-5)&&close_to(matrix[10],-1.25,1e-5)&&close_to(matrix[11],-1,1e-5)&&close_to(matrix[14],-2.25,1e-5),"perspective OpenGL matrix");
    glMatrixMode(GL_MODELVIEW);glLoadIdentity();p_gluLookAt(0,0,5,0,0,0,0,1,0);glGetDoublev(GL_MODELVIEW_MATRIX,matrix);check(close_to(matrix[0],1)&&close_to(matrix[5],1)&&close_to(matrix[10],1)&&close_to(matrix[14],-5),"lookAt OpenGL matrix");
    GLint viewport[4]={0,0,64,64};glLoadIdentity();p_gluPickMatrix(32,32,16,32,viewport);glGetDoublev(GL_MODELVIEW_MATRIX,matrix);check(close_to(matrix[0],4)&&close_to(matrix[5],2)&&close_to(matrix[12],0)&&close_to(matrix[13],0),"pick OpenGL matrix");
    reset_gl();auto q=p_gluNewQuadric();check(!!q,"GPU quadric allocation");if(q){p_gluQuadricDrawStyle(q,GLU_FILL);p_gluQuadricNormals(q,GLU_NONE);p_gluQuadricOrientation(q,GLU_OUTSIDE);p_gluQuadricTexture(q,GL_FALSE);p_gluSphere(q,.65,24,12);pixel(32,32,true,"sphere center");pixel(2,2,false,"sphere exterior");reset_gl();p_gluDisk(q,.25,.75,32,2);pixel(32,32,false,"disk hole");pixel(48,32,true,"disk ring");pixel(61,32,false,"disk exterior");p_gluDeleteQuadric(q);}
    reset_gl();glMatrixMode(GL_PROJECTION);glLoadIdentity();p_gluOrtho2D(0,2,0,2);glMatrixMode(GL_MODELVIEW);glBegin(GL_TRIANGLES);for(auto &v:concave.triangles)glVertex3dv(v.data());glEnd();pixel(16,48,true,"tess upper arm");pixel(48,16,true,"tess lower arm");pixel(48,48,false,"tess concave cutout");
    check(glGetError()==GL_NO_ERROR,"GLU operations leave no OpenGL error");wglMakeCurrent(nullptr,nullptr);check(wglDeleteContext(context)!=FALSE,"destroy native OpenGL context");ReleaseDC(window,dc);DestroyWindow(window);return true;
}
int wmain(int argc,wchar_t **argv){
    setvbuf(stdout,nullptr,_IONBF,0);if(argc<3){std::puts("usage: glu32-probe <expected-DLL> wine|native [--cpu-only]");return 2;}
    bool wine=!std::wcscmp(argv[2],L"wine"),cpu=argc>3&&!std::wcscmp(argv[3],L"--cpu-only");
    HMODULE module=LoadLibraryW(L"glu32.dll");if(!module){std::printf("FAIL load GLU error=%lu\n",GetLastError());return 1;}
#ifdef STATIC_GLU
#define RESOLVE(name) p_##name=&name;
#else
#define RESOLVE(name) p_##name=(decltype(p_##name))GetProcAddress(module,#name);
#endif
    GLU_FUNCTIONS(RESOLVE)
#undef RESOLVE
#define VERIFY(name) if(!p_##name){std::printf("FAIL missing %s\n",#name);return 1;} check(owner(import_target((const void*)p_##name))==module,"GLU export owner / static IAT target");
    GLU_FUNCTIONS(VERIFY)
#undef VERIFY
    auto path=mapped_path(module);std::printf("GLU_MODULE %ls\n",path.c_str());check(same_file(path,argv[1]),"selected GLU backing file identity");
    wchar_t system[MAX_PATH];GetSystemDirectoryW(system,MAX_PATH);std::wstring native_gl=std::wstring(system)+L"\\opengl32.dll";
    check(same_file(mapped_path(owner((const void*)glGetString)),native_gl),"fixture OpenGL remains native");check(verify_gl_imports(module,native_gl),"GLU OpenGL IAT remains native");
    math_tests();error_tests(wine);
    auto concave=tessellate({{{0,0,0},{2,0,0},{2,1,0},{1,1,0},{1,2,0},{0,2,0}}},"concave",3,false);
    tessellate({{{0,0,0},{4,0,0},{4,4,0},{0,4,0}},{{1,1,0},{1,3,0},{3,3,0},{3,1,0}}},"hole",12,false);
    tessellate({{{0,0,0},{2,2,0},{0,2,0},{2,0,0}}},"intersection",2,true);
    if(!cpu)gpu_tests(concave);FreeLibrary(module);std::printf("SUMMARY failures=%u checks=%u cpuOnly=%d nurbsCoverage=unsupported-for-wine\n",failures,checks,cpu);return failures?1:0;
}
