// fxtool.cpp -- tooling for the preshader->GPU relocation.
//   fxtool <shader-blob.bin>        : disassemble a raw shader blob (round-trip test)
//   fxtool --effect <effect.fxo>    : create a real D3D9 device, load the effect via
//                                     d3dx9_27, and report load result + preshader presence.
// The --effect mode is the load-verifier: nothing modified goes near the game's
// Shaders.big until D3DXCreateEffectFromFile accepts it here.
//
// Dynamically binds d3dx9_27.dll (the game's own 2005 build) and d3d9.dll. 32-bit.
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

typedef void*  (WINAPI *GetPtr_t)(void*);
typedef DWORD  (WINAPI *GetSize_t)(void*);
typedef ULONG  (WINAPI *Release_t)(void*);
static void**  vt(void* o){ return *(void***)o; }
static void*   BufPtr(void* b){ return ((GetPtr_t) vt(b)[3])(b); }
static DWORD   BufSize(void* b){ return ((GetSize_t)vt(b)[4])(b); }
static void    Rel(void* o){ if(o) ((Release_t)vt(o)[2])(o); }

typedef HRESULT (WINAPI *PFN_Disasm)(const DWORD*, BOOL, LPCSTR, void**);
typedef HRESULT (WINAPI *PFN_Asm)(LPCSTR, UINT, const void*, void*, DWORD, void**, void**);
typedef void*   (WINAPI *PFN_D3DCreate9)(UINT);
typedef HRESULT (WINAPI *PFN_CreateEffectFromFileA)(void*, LPCSTR, const void*, void*, DWORD, void*, void**, void**);
typedef HRESULT (WINAPI *PFN_DisasmEffect)(void*, BOOL, void**);

typedef struct {
    UINT  BackBufferWidth, BackBufferHeight; DWORD BackBufferFormat; UINT BackBufferCount;
    DWORD MultiSampleType, MultiSampleQuality, SwapEffect; HWND hDeviceWindow; BOOL Windowed;
    BOOL  EnableAutoDepthStencil; DWORD AutoDepthStencilFormat, Flags;
    UINT  FullScreen_RefreshRateInHz, PresentationInterval;
} PRESENT_PARAMS;

// IDirect3D9::CreateDevice is vtable slot 16
typedef HRESULT (WINAPI *PFN_CreateDevice)(void*, UINT, DWORD, HWND, DWORD, PRESENT_PARAMS*, void**);

static int roundtrip(const char* path){
    HMODULE h=LoadLibraryA("d3dx9_27.dll"); if(!h){ printf("no d3dx9_27\n"); return 3; }
    PFN_Disasm Disasm=(PFN_Disasm)GetProcAddress(h,"D3DXDisassembleShader");
    FILE* f=fopen(path,"rb"); if(!f){ printf("open %s\n",path); return 5; }
    fseek(f,0,SEEK_END); long n=ftell(f); fseek(f,0,SEEK_SET);
    BYTE* blob=(BYTE*)malloc(n); fread(blob,1,n,f); fclose(f);
    void* dis=NULL; HRESULT hr=Disasm((const DWORD*)blob,FALSE,NULL,&dis);
    if(FAILED(hr)){ printf("disasm hr=0x%08lX\n",(unsigned long)hr); return 6; }
    printf("%.*s\n",(int)BufSize(dis),(char*)BufPtr(dis)); Rel(dis); free(blob); return 0;
}

static int loadeffect(const char* path){
    HMODULE hd3d=LoadLibraryA("d3d9.dll"); if(!hd3d){ printf("no d3d9\n"); return 3; }
    HMODULE hx=LoadLibraryA("d3dx9_27.dll"); if(!hx){ printf("no d3dx9_27\n"); return 3; }
    PFN_D3DCreate9 Create9=(PFN_D3DCreate9)GetProcAddress(hd3d,"Direct3DCreate9");
    PFN_CreateEffectFromFileA CreateEffect=(PFN_CreateEffectFromFileA)GetProcAddress(hx,"D3DXCreateEffectFromFileA");
    PFN_DisasmEffect DisEff=(PFN_DisasmEffect)GetProcAddress(hx,"D3DXDisassembleEffect");
    if(!Create9||!CreateEffect){ printf("missing exports\n"); return 4; }

    WNDCLASSA wc={0}; wc.lpfnWndProc=DefWindowProcA; wc.hInstance=GetModuleHandleA(NULL); wc.lpszClassName="fxw";
    RegisterClassA(&wc);
    HWND hwnd=CreateWindowA("fxw","fx",WS_OVERLAPPED,0,0,8,8,NULL,NULL,wc.hInstance,NULL);

    void* d3d=Create9(32); if(!d3d){ printf("Direct3DCreate9 failed\n"); return 5; }
    PRESENT_PARAMS pp={0};
    pp.Windowed=TRUE; pp.SwapEffect=1 /*DISCARD*/; pp.BackBufferFormat=0 /*UNKNOWN*/;
    pp.hDeviceWindow=hwnd; pp.BackBufferWidth=8; pp.BackBufferHeight=8;
    void* dev=NULL;
    PFN_CreateDevice CreateDevice=(PFN_CreateDevice)vt(d3d)[16];
    HRESULT hr=CreateDevice(d3d,0,1/*HAL*/,hwnd,0x20/*SW vertex*/,&pp,&dev);
    if(FAILED(hr)||!dev){ printf("CreateDevice hr=0x%08lX (trying REF)\n",(unsigned long)hr);
        hr=CreateDevice(d3d,0,2/*REF*/,hwnd,0x20,&pp,&dev);
        if(FAILED(hr)||!dev){ printf("CreateDevice REF hr=0x%08lX\n",(unsigned long)hr); return 6; } }
    printf("device OK. loading effect %s\n", path);

    void* eff=NULL; void* err=NULL;
    hr=CreateEffect(dev,path,NULL,NULL,0,NULL,&eff,&err);
    if(FAILED(hr)||!eff){ printf("D3DXCreateEffectFromFileA hr=0x%08lX\n",(unsigned long)hr);
        if(err) printf("  compile errors: %.*s\n",(int)BufSize(err),(char*)BufPtr(err)); return 7; }
    printf("EFFECT LOADED OK.\n");
    if(DisEff){ void* dtxt=NULL; if(SUCCEEDED(DisEff(eff,FALSE,&dtxt))&&dtxt){
        char* t=(char*)BufPtr(dtxt); DWORD tl=BufSize(dtxt); int pc=0;
        for(DWORD i=0;i+9<=tl;i++) if(memcmp(t+i,"preshader",9)==0) pc++;
        printf("effect disassembly %lu bytes, 'preshader' occurrences = %d\n", tl, pc);
        Rel(dtxt); } }
    Rel(eff); return 0;
}

static int assemble(const char* asmpath, const char* outpath){
    HMODULE h=LoadLibraryA("d3dx9_27.dll"); if(!h){ printf("no d3dx9_27\n"); return 3; }
    PFN_Asm Asm=(PFN_Asm)GetProcAddress(h,"D3DXAssembleShader");
    FILE* f=fopen(asmpath,"rb"); if(!f){ printf("open %s\n",asmpath); return 5; }
    fseek(f,0,SEEK_END); long n=ftell(f); fseek(f,0,SEEK_SET);
    char* txt=(char*)malloc(n+1); fread(txt,1,n,f); txt[n]=0; fclose(f);
    void* out=NULL; void* err=NULL;
    HRESULT hr=Asm(txt,(UINT)n,NULL,NULL,0,&out,&err);
    if(FAILED(hr)||!out){ printf("assemble FAILED hr=0x%08lX\n",(unsigned long)hr);
        if(err) printf("  errors: %.*s\n",(int)BufSize(err),(char*)BufPtr(err)); return 7; }
    DWORD sz=BufSize(out);
    FILE* o=fopen(outpath,"wb"); fwrite(BufPtr(out),1,sz,o); fclose(o);
    printf("assembled OK: %lu bytes -> %s\n", sz, outpath);
    Rel(out); Rel(err); free(txt); return 0;
}

int main(int argc,char**argv){
    if(argc>=3 && strcmp(argv[1],"--effect")==0) return loadeffect(argv[2]);
    if(argc>=4 && strcmp(argv[1],"--asm")==0) return assemble(argv[2],argv[3]);
    if(argc>=2) return roundtrip(argv[1]);
    printf("usage: fxtool <blob.bin> | fxtool --effect <fxo> | fxtool --asm <in.asm> <out.bin>\n"); return 2;
}
