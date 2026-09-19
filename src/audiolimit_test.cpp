// Offline differential test of the audio limit index (aotr_audiolimit.inc): a fake manager with an STLport-shaped
// request list, random passes with tracked and untracked list changes, every indexed answer compared with the walk.
//   audiolimit_test.exe [mode] [distinct-infos]     mode 0: only changes the engine can make (must never differ)
//                                                    mode 1: also rewrites an event's info under the index (the proof must trip)
#include <windows.h>
#include <intrin.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void logf(const char* fmt, ...) { va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap); printf("\n"); }
static BYTE* makeTrampoline(BYTE*, int) { return NULL; }
static BOOL patchJmp(BYTE*, void*, int) { return FALSE; }
static void suspendOthers(HANDLE*, int* n, int) { *n = 0; }
static void resumeAll(HANDLE*, int) {}
static char* perStr(LONG64 a, LONG64 b, char* buf) { sprintf(buf, "%.1f", b ? (double)a / (double)b : 0.0); return buf; }
static char* msStr(LONG64 a, LONG64 b, char* buf) { sprintf(buf, "%.3f", b ? (double)a / (double)b : 0.0); return buf; }
static double g_tscPerQpc = 1.0;

struct Node { Node* next; Node* prev; void* val; };
static void  __fastcall tPushBack(void* list, void*, void* pval)  { Node* h = *(Node**)list; Node* n = (Node*)malloc(sizeof(Node)); n->val = *(void**)pval; n->next = h; n->prev = h->prev; h->prev->next = n; h->prev = n; }
static void  __fastcall tPushFront(void* list, void*, void* pval) { Node* h = *(Node**)list; Node* n = (Node*)malloc(sizeof(Node)); n->val = *(void**)pval; n->prev = h; n->next = h->next; h->next->prev = n; h->next = n; }
static void* __fastcall tErase(void* list, void*, void* res, void* node) { Node* n = (Node*)node; Node* nx = n->next; n->prev->next = nx; nx->prev = n->prev; free(n); *(Node**)res = nx; return res; }
static void  __fastcall tPopBack(void* list, void*) { Node* h = *(Node**)list; Node* r; if (h->prev != h) tErase(list, NULL, &r, h->prev); }
static void  __fastcall tClear(void* list, void*) { Node* h = *(Node**)list; Node* r; while (h->next != h) tErase(list, NULL, &r, h->next); }
#define AL_PRIM_PUSHBACK  (&tPushBack)
#define AL_PRIM_PUSHFRONT (&tPushFront)
#define AL_PRIM_ERASE     (&tErase)
#define AL_PRIM_POPBACK   (&tPopBack)
#define AL_PRIM_CLEAR     (&tClear)
#include "aotr_audiolimit.inc"

static BYTE g_mgr[0xC00];
static void** listOf() { return (void**)(g_mgr + 0x98); }
static DWORD g_infos[64];
static unsigned g_rng = 12345;
static unsigned rnd() { g_rng = g_rng * 1664525u + 1013904223u; return g_rng >> 8; }
static int g_mode = 0, g_skew = 64;
static BYTE* newEvent() { BYTE* e = (BYTE*)calloc(1, 0x80); *(DWORD*)(e + 8) = g_infos[rnd() % (unsigned)g_skew]; e[0x4B] = (rnd() % 4) ? 1 : 0; return e; }
static void* newReq() { unsigned q = rnd() % 50; if (q == 1) return NULL; DWORD* r = (DWORD*)calloc(1, 0x20); r[1] = q == 0 ? 0 : (DWORD)(ULONG_PTR)newEvent(); return r; }
static LONG g_cmp = 0, g_diff = 0, g_illegal = 0;

static Node* pick(Node* h, int span) { int k = (int)(rnd() % (unsigned)span); Node* x = h->next; while (k-- > 0 && x != h) x = x->next; return x; }
static void __fastcall passBody(void* mgr, void*) {
    Node* h = *(Node**)listOf();
    for (Node* n = h->next; n != h; ) {
        DWORD* req = (DWORD*)n->val;
        BYTE* ev = req ? (BYTE*)(ULONG_PTR)req[1] : NULL;
        if (ev) {
            LONG v = 0, a = alCount((DWORD)(ULONG_PTR)mgr, (DWORD)(ULONG_PTR)ev), w = alWalk((DWORD)(ULONG_PTR)mgr, (DWORD)(ULONG_PTR)ev, &v);
            ++g_cmp; if (a != w) { ++g_diff; if (g_diff < 5) printf("  DIFF index %d walk %d\n", (int)a, (int)w); }
        }
        unsigned r = rnd() % 100;
        Node* cur = n; n = n->next;
        Node* out;
        if (r < 12) alThErase(listOf(), NULL, &out, cur);                                                   // the pass's own erase
        else if (r < 40) { if (ev) ev[0x4B] = (BYTE)(rnd() & 1); }                                          // retry / waiting flag
        else if (r < 42) { void* q = newReq(); alThPushBack(listOf(), NULL, &q); }                         // tracked push
        else if (r < 43) { void* q = newReq(); alThPushFront(listOf(), NULL, &q); }
        else if (r < 44) { void* q = newReq(); tPushBack(listOf(), NULL, &q); }                            // UNTRACKED push at the end
        else if (r < 45) { void* q = newReq(); tPushFront(listOf(), NULL, &q); }                           // UNTRACKED push at the front
        else if (r < 47) { Node* x = pick(h, 40); if (x != h && x != n) alThErase(listOf(), NULL, &out, x); }  // tracked erase elsewhere
        else if (r < 48) { Node* x = pick(h, 40); if (x != h && x != n) tErase(listOf(), NULL, &out, x); }     // UNTRACKED erase elsewhere (node freed)
        else if (r < 50) { Node* x = pick(h, 60); if (x != h && x->val) { BYTE* e2 = (BYTE*)(ULONG_PTR)((DWORD*)x->val)[1]; if (e2) e2[0x4B] ^= 1; } }
        else if (g_mode == 1 && r < 51 && ev) { *(DWORD*)(ev + 8) = g_infos[rnd() % 64]; ++g_illegal; }     // ILLEGAL: an event's info changes under the index
    }
}
int main(int argc, char** argv) {
    g_mode = argc > 1 ? atoi(argv[1]) : 0;
    g_skew = argc > 2 ? atoi(argv[2]) : 64;
    if (g_skew < 1 || g_skew > 64) g_skew = 64;
    if (argc > 3) g_rng = (unsigned)atoi(argv[3]);
    for (int i = 0; i < 64; ++i) g_infos[i] = (DWORD)(ULONG_PTR)calloc(1, 0x100);
    Node* head = (Node*)malloc(sizeof(Node)); head->next = head->prev = head; head->val = NULL;
    *listOf() = head;
    o_alPass = &passBody; g_alLive = 1;
    LONG passes = 0, maxLen = 0;
    for (int it = 0; it < 3000; ++it) {
        int add = (int)(rnd() % 60);                                                                        // new requests between passes
        for (int i = 0; i < add; ++i) { void* q = newReq(); if (rnd() & 1) alThPushBack(listOf(), NULL, &q); else tPushBack(listOf(), NULL, &q); }
        if (!(rnd() % 400)) alThClear(listOf(), NULL);
        if (!(rnd() % 97)) alThPopBack(listOf(), NULL);
        LONG len = 0; for (Node* n = head->next; n != head; n = n->next) ++len;
        if (len > maxLen) maxLen = len;
        hkAlPass(g_mgr, NULL); ++passes;
        if (!g_alLive && g_mode == 0) break;
    }
    printf("mode %d infos %d: passes %d, longest list %d | compared %d, differences %d | checks %d indexed %d rebuilds %d stale %d erased-in-pass %d other-changes %d faults %d too-big %d | proof compared %d mismatches %d live %d | illegal changes %d\n",
           g_mode, g_skew, (int)passes, (int)maxLen, (int)g_cmp, (int)g_diff, (int)g_alCalls, (int)g_alIdx, (int)g_alRebuilds, (int)g_alStale, (int)g_alErased, (int)g_alMut, (int)g_alFaults, (int)g_alTooBig,
           (int)g_alChecked, (int)g_alMismatch, (int)g_alLive, (int)g_illegal);
    if (g_mode == 0) return (g_diff || g_alMismatch || !g_alLive) ? 1 : 0;
    return 0;
}
