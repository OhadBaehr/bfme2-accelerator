#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
int main(int argc,char**argv){
  const char* p = argc>1?argv[1]:"aotr_accel.dll";
  HMODULE h = LoadLibraryA(p);
  if(!h){ printf("LoadLibraryA failed, GetLastError=%lu\n", GetLastError()); return 1; }
  printf("loaded OK at %08lX\n",(unsigned long)(ULONG_PTR)h);
  Sleep(3000);
  return 0;
}
