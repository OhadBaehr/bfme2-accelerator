// bfme2_accel_loader.exe - the launcher for the BFME2 Accelerator.
//
// One window: the games it can find on this machine, as their own covers. Click one and it is gone - it starts the
// game, loads the accelerator into game.dat as soon as it appears, keeps covering the rest of the session (drop to
// the menu, start another match, that one gets it too) and exits by itself once the game is closed. No console,
// nothing to close afterwards.
//
// Nothing is ever written into a game folder and game.dat on disk is never modified, so mod launchers that check
// their files and their checksums still pass. Needs administrator (manifest, embedded at link time) because the
// game runs elevated - one prompt, which the game asks for anyway.
//
// Ships as two files: bfme2_accel_loader.exe + bfme2_accel.dll. The cover art is embedded (launcher.rc) and the
// injector is compiled in - a helper .exe was one more file for antivirus to quarantine, and a quarantined helper
// looked exactly like "the accelerator silently did nothing".

#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <tlhelp32.h>
#include <objidl.h>
#include <gdiplus.h>
#include <commdlg.h>
#include <stdio.h>
#include <string.h>
using namespace Gdiplus;

// ---------------------------------------------------------------- layout
#define TILE      280
#define GAP        30
#define MARGIN     42
#define TOP       112
#define CLIENT_W  (MARGIN * 2 + TILE * 3 + GAP * 2)
#define CLIENT_H  (TOP + TILE + 118)
#define NSLOT       3

struct Slot {
    const char* key;       // how it is written down in games.txt
    const char* name;      // what it is called on the plate
    const char* sub;       // the one-line description under it
    const char* wanted;    // the program this normally is, shown when asking where it went
    int         resId;     // the cover
    char        exe[MAX_PATH];
    bool        have;
    const char* managed;   // a mod launcher owns this install, and is what actually starts it
    RECT        rc;
    Image*      img;
};
static Slot g_slot[NSLOT] = {
    { "bfme2", "The Battle for Middle-earth II", "partial - untested", "lotrbfme2.exe",    101, "", false, NULL, {0,0,0,0}, NULL },
    { "rotwk", "The Rise of the Witch-king",     "2.02 - everything",  "lotrbfme2ep1.exe", 102, "", false, NULL, {0,0,0,0}, NULL },
    { "aotr",  "Age of the Ring",                "2.02 - everything",  "AotR_Launcher.exe",103, "", false, NULL, {0,0,0,0}, NULL },
};
static char  g_dll[MAX_PATH];
static int   g_hover = -1;
static HWND  g_hwnd = NULL;
static ULONG_PTR g_gdip = 0;
static WCHAR g_serif[64] = L"Palatino Linotype";
static Image* g_logo = NULL;                // the mark, shown in the header

// ---------------------------------------------------------------- the two pages
#define PAGE_PICK      0
#define PAGE_SETTINGS  1
#define ID_EDIT        200      // +slot
#define ID_BROWSE      210      // +slot
#define ID_SAVE        230
#define ID_BACK        231
#define ROW_Y(i)      (130 + (i) * 88)

static int   g_page = PAGE_PICK;
static HWND  g_edit[NSLOT], g_browse[NSLOT], g_save = NULL, g_back = NULL;
static HFONT g_uiFont = NULL;
static HBRUSH g_editBg = NULL;
static RECT  g_gearRc = { 0, 0, 0, 0 };     // the "Paths" hotspot on the pick page
static bool  g_gearHot = false;

static bool exists(const char* p) { return p && *p && GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES; }

static void siblingPath(const char* leaf, char* out, DWORD n) {
    char self[MAX_PATH]; GetModuleFileNameA(NULL, self, MAX_PATH);
    char* slash = strrchr(self, '\\'); if (slash) *(slash + 1) = 0;
    _snprintf(out, n, "%s%s", self, leaf); out[n - 1] = 0;
}
static void folderOf(const char* path, char* out, DWORD n) {
    lstrcpynA(out, path, n);
    char* slash = strrchr(out, '\\'); if (slash) *slash = 0;
}

// ---------------------------------------------------------------- finding what is installed
static void offer(int slot, const char* exe) {
    if (slot < 0 || slot >= NSLOT || g_slot[slot].have || !exists(exe)) return;
    lstrcpynA(g_slot[slot].exe, exe, MAX_PATH);
    g_slot[slot].have = true;
}
// an install root can hold several of these: the AotR launcher at the top, RotWK in rotwk\, BFME2 in bfme2\, or
// the root can itself be one plain game folder.
//
// A mod launcher stages its own files INTO the RotWK folder - an Age of the Ring install leaves #aotr_patch202.big
// and friends sitting in rotwk\ - so starting rotwk\lotrbfme2ep1.exe directly runs the stock executable against
// half-wired mod data and dies with "Could not open include in Data\INI\...". Where a mod launcher owns the
// folder, that launcher is the way in, and this says so rather than offering a path that cannot work.
static void scanRoot(const char* root) {
    if (!root || !*root) return;
    char p[MAX_PATH], aotr[MAX_PATH];
    _snprintf(p, sizeof(p), "%s\\bfme2\\lotrbfme2.exe", root);    offer(0, p);
    _snprintf(p, sizeof(p), "%s\\lotrbfme2.exe", root);           offer(0, p);

    _snprintf(aotr, sizeof(aotr), "%s\\AotR_Launcher.exe", root);
    const bool modded = exists(aotr);
    _snprintf(p, sizeof(p), "%s\\rotwk\\lotrbfme2ep1.exe", root);
    if (exists(p) && modded) { offer(1, aotr); g_slot[1].managed = "the Age of the Ring launcher"; }
    else offer(1, p);
    _snprintf(p, sizeof(p), "%s\\lotrbfme2ep1.exe", root);        offer(1, p);

    offer(2, aotr);
}
static bool regPath(HKEY root, const char* subkey, const char* value, char* out, DWORD n) {
    HKEY k;
    if (RegOpenKeyExA(root, subkey, 0, KEY_READ | KEY_WOW64_32KEY, &k) != ERROR_SUCCESS) return false;
    char buf[MAX_PATH]; DWORD sz = sizeof(buf) - 1, type = 0;
    bool ok = RegQueryValueExA(k, value, NULL, &type, (BYTE*)buf, &sz) == ERROR_SUCCESS &&
              (type == REG_SZ || type == REG_EXPAND_SZ) && sz > 0;
    RegCloseKey(k);
    if (!ok) return false;
    buf[sz < sizeof(buf) ? sz : sizeof(buf) - 1] = 0;
    for (char* q = buf + strlen(buf); q > buf && q[-1] == '\\'; --q) q[-1] = 0;
    lstrcpynA(out, buf, n);
    return true;
}
// bfme2_accel.ini, [Games]: where each game is, when it is not somewhere this can work out for itself. Written
// whenever a game is located by hand or the paths page is saved, and read before anything is searched for.
static void iniPath(char* out, DWORD n) { siblingPath("bfme2_accel.ini", out, n); }

static void loadGames() {
    char ini[MAX_PATH]; iniPath(ini, sizeof(ini));
    if (!exists(ini)) return;
    for (int i = 0; i < NSLOT; ++i) {
        char p[MAX_PATH]; p[0] = 0;
        GetPrivateProfileStringA("Games", g_slot[i].key, "", p, MAX_PATH, ini);
        if (p[0]) offer(i, p);
    }
}
static void saveGames() {
    char ini[MAX_PATH]; iniPath(ini, sizeof(ini));
    if (!exists(ini)) {                                // write the header once, so the file explains itself
        FILE* f = fopen(ini, "w");
        if (f) {
            fprintf(f, "; BFME2 Accelerator\n"
                       "; Where each game is. Edit here or on the loader's Paths page - either works.\n"
                       "; Remove a line, or leave it empty, and that game is looked for the usual way instead.\n\n"
                       "[Games]\n");
            fclose(f);
        }
    }
    for (int i = 0; i < NSLOT; ++i)
        WritePrivateProfileStringA("Games", g_slot[i].key, g_slot[i].have ? g_slot[i].exe : NULL, ini);
}

static void findInstalls() {
    loadGames();                                      // 0) bfme2_accel.ini: what the player has set
    // 1) an older hand-written list, still honoured: one path per line, matched to a slot by its filename
    char txt[MAX_PATH]; siblingPath("launcher.txt", txt, sizeof(txt));
    FILE* f = fopen(txt, "r");
    if (f) {
        char line[MAX_PATH];
        while (fgets(line, sizeof(line), f)) {
            for (char* q = line + strlen(line); q > line && (q[-1] == '\n' || q[-1] == '\r' || q[-1] == ' '); --q) q[-1] = 0;
            if (!exists(line)) continue;
            const char* leaf = strrchr(line, '\\'); leaf = leaf ? leaf + 1 : line;
            if      (!_stricmp(leaf, "AotR_Launcher.exe"))  offer(2, line);
            else if (!_stricmp(leaf, "lotrbfme2ep1.exe"))   offer(1, line);
            else if (!_stricmp(leaf, "lotrbfme2.exe"))      offer(0, line);
        }
        fclose(f);
    }
    // 2) this loader's own folder and the folders above it: dropping it into a game folder just works
    char dir[MAX_PATH]; siblingPath("", dir, sizeof(dir));
    for (char* q = dir + strlen(dir); q > dir && q[-1] == '\\'; --q) q[-1] = 0;
    for (int up = 0; up < 5 && dir[0]; ++up) {
        scanRoot(dir);
        char parent[MAX_PATH]; folderOf(dir, parent, sizeof(parent));
        if (!lstrcmpiA(parent, dir)) break;
        lstrcpynA(dir, parent, sizeof(dir));
    }
    // 3) where the installers put things
    static const struct { const char* key; const char* val; } kKeys[] = {
        { "SOFTWARE\\Electronic Arts\\Electronic Arts\\The Lord of the Rings, The Rise of the Witch-king", "InstallPath" },
        { "SOFTWARE\\Electronic Arts\\Electronic Arts\\The Battle for Middle-earth II", "InstallPath" },
    };
    for (int i = 0; i < (int)(sizeof(kKeys) / sizeof(kKeys[0])); ++i) {
        char v[MAX_PATH];
        if (regPath(HKEY_LOCAL_MACHINE, kKeys[i].key, kKeys[i].val, v, sizeof(v))) scanRoot(v);
        if (regPath(HKEY_CURRENT_USER,  kKeys[i].key, kKeys[i].val, v, sizeof(v))) scanRoot(v);
    }
    scanRoot("C:\\AgeoftheRing");
    scanRoot("C:\\Program Files (x86)\\Electronic Arts\\The Lord of the Rings, The Rise of the Witch-king");
    scanRoot("C:\\Program Files (x86)\\Electronic Arts\\The Battle for Middle-earth II");
}

// ---------------------------------------------------------------- loading the DLL into the game
static DWORD findPid(const char* name) {
    DWORD self = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32 pe; pe.dwSize = sizeof(pe);
    DWORD pid = 0;
    if (Process32First(snap, &pe)) {
        do { if (pe.th32ProcessID != self && _stricmp(pe.szExeFile, name) == 0) { pid = pe.th32ProcessID; break; } }
        while (Process32Next(snap, &pe));
    }
    CloseHandle(snap);
    return pid;
}
static void enableDebugPrivilege() {
    HANDLE tok;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES, &tok)) return;
    TOKEN_PRIVILEGES tp; tp.PrivilegeCount = 1; tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    if (LookupPrivilegeValueA(NULL, "SeDebugPrivilege", &tp.Privileges[0].Luid))
        AdjustTokenPrivileges(tok, FALSE, &tp, sizeof(tp), NULL, NULL);
    CloseHandle(tok);
}
// This loader is 32-bit and so is game.dat, so kernel32's LoadLibraryA sits at the same address in both processes.
static bool injectInto(DWORD pid, const char* dllFullPath) {
    HANDLE hp = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
                            PROCESS_VM_WRITE | PROCESS_VM_READ, FALSE, pid);
    if (!hp) return false;
    // A game that died during start-up is still here for a moment, and loading into its corpse used to succeed
    // and write a cheerful "loaded" line - which read exactly like the accelerator having broken the game.
    DWORD code = 0;
    if (GetExitCodeProcess(hp, &code) && code != STILL_ACTIVE) { CloseHandle(hp); return false; }
    SIZE_T len = strlen(dllFullPath) + 1;
    void* remote = VirtualAllocEx(hp, NULL, len, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) { CloseHandle(hp); return false; }
    SIZE_T wrote = 0;
    if (!WriteProcessMemory(hp, remote, dllFullPath, len, &wrote)) {
        VirtualFreeEx(hp, remote, 0, MEM_RELEASE); CloseHandle(hp); return false;
    }
    FARPROC loadLib = GetProcAddress(GetModuleHandleA("kernel32.dll"), "LoadLibraryA");
    HANDLE th = CreateRemoteThread(hp, NULL, 0, (LPTHREAD_START_ROUTINE)loadLib, remote, 0, NULL);
    if (!th) { VirtualFreeEx(hp, remote, 0, MEM_RELEASE); CloseHandle(hp); return false; }
    WaitForSingleObject(th, 15000);
    DWORD mod = 0; GetExitCodeThread(th, &mod);
    VirtualFreeEx(hp, remote, 0, MEM_RELEASE);
    CloseHandle(th); CloseHandle(hp);
    return mod != 0;
}
// From here on there is no window: start the game, load into every game.dat of the session, leave when it is over.
static void runHeadless(const char* exe) {
    const char* leaf = strrchr(exe, '\\'); leaf = leaf ? leaf + 1 : exe;
    char launcherLeaf[MAX_PATH]; lstrcpynA(launcherLeaf, leaf, MAX_PATH);
    if (!findPid("game.dat") && !findPid(launcherLeaf)) {
        char dir[MAX_PATH]; folderOf(exe, dir, sizeof(dir));
        char cmd[MAX_PATH + 2]; _snprintf(cmd, sizeof(cmd), "\"%s\"", exe);
        STARTUPINFOA si; ZeroMemory(&si, sizeof(si)); si.cb = sizeof(si);
        PROCESS_INFORMATION pi; ZeroMemory(&pi, sizeof(pi));
        if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, dir, &si, &pi)) {
            char msg[MAX_PATH + 160];
            _snprintf(msg, sizeof(msg), "Could not start:\n\n%s\n\n(error %lu)", exe, GetLastError());
            MessageBoxA(NULL, msg, "BFME2 Accelerator", MB_ICONERROR | MB_OK);
            return;
        }
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    }
    DWORD injectedPid = 0, lastAlive = GetTickCount();
    bool warned = false;
    for (;;) {
        DWORD pid = findPid("game.dat");
        if (pid && pid != injectedPid) {
            Sleep(800);                              // let the game finish its own initial module loading
            if (!injectInto(pid, g_dll) && !warned) {
                warned = true;
                MessageBoxA(NULL, "The accelerator could not be loaded into the game.\n\n"
                                  "The game keeps running normally, just without it.\n"
                                  "bfme2_accel.log next to the loader says why.",
                            "BFME2 Accelerator", MB_ICONWARNING | MB_OK);
            }
            injectedPid = pid;                       // one attempt per instance
        }
        if (pid || findPid(launcherLeaf)) lastAlive = GetTickCount();
        else if (GetTickCount() - lastAlive > 20000) break;      // both gone for 20s: the session is over
        Sleep(500);
    }
}

// ---------------------------------------------------------------- the window
static Image* loadCover(HINSTANCE inst, int id) {
    HRSRC r = FindResourceA(inst, MAKEINTRESOURCEA(id), (LPCSTR)RT_RCDATA);
    if (!r) return NULL;
    DWORD n = SizeofResource(inst, r);
    HGLOBAL res = LoadResource(inst, r);
    const void* p = LockResource(res);
    if (!p || !n) return NULL;
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, n);
    if (!mem) return NULL;
    void* q = GlobalLock(mem);
    memcpy(q, p, n);
    GlobalUnlock(mem);
    IStream* st = NULL;
    if (CreateStreamOnHGlobal(mem, TRUE, &st) != S_OK) { GlobalFree(mem); return NULL; }
    Image* img = Image::FromStream(st);
    st->Release();                                   // the stream owns the memory now (fDeleteOnRelease)
    if (img && img->GetLastStatus() != Ok) { delete img; img = NULL; }
    return img;
}

static void drawText(Graphics& g, const WCHAR* s, const WCHAR* face, REAL size, FontStyle style,
                     Color col, REAL cx, REAL y, REAL width, StringAlignment align = StringAlignmentCenter) {
    FontFamily ff(face);
    if (!ff.IsAvailable()) { FontFamily f2(L"Georgia"); if (f2.IsAvailable()) return drawText(g, s, L"Georgia", size, style, col, cx, y, width, align);
                             return drawText(g, s, L"Times New Roman", size, style, col, cx, y, width, align); }
    Font font(&ff, size, style, UnitPixel);
    StringFormat fmt;
    fmt.SetAlignment(align);
    fmt.SetTrimming(StringTrimmingEllipsisCharacter);
    fmt.SetFormatFlags(StringFormatFlagsNoWrap);
    SolidBrush br(col);
    RectF box(cx - width / 2, y, width, size * 2.2f);
    g.DrawString(s, -1, &font, box, &fmt, &br);
}

static void paint(HWND hwnd, HDC dc, const RECT& cl) {
    Graphics g(dc);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
    g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);

    // a dark, warm background with a slow gradient, like lamplit parchment gone to shadow
    { LinearGradientBrush bg(Rect(0, 0, cl.right, cl.bottom), Color(255, 22, 18, 13), Color(255, 8, 7, 6), 90.0f);
      g.FillRectangle(&bg, 0, 0, cl.right, cl.bottom); }
    { Pen outer(Color(70, 196, 158, 86), 1.0f);  g.DrawRectangle(&outer, 10, 10, cl.right - 21, cl.bottom - 21); }
    { Pen inner(Color(36, 196, 158, 86), 1.0f);  g.DrawRectangle(&inner, 14, 14, cl.right - 29, cl.bottom - 29); }

    REAL cx = (REAL)cl.right / 2;

    // mark and name as one centred lockup, rather than a title with something floating beside it
    {
        const WCHAR* title = (g_page == PAGE_SETTINGS) ? L"WHERE THE GAMES ARE" : L"BFME2  ACCELERATOR";
        const REAL pt = (g_page == PAGE_SETTINGS) ? 26.0f : 30.0f;
        const int  ms = (g_page == PAGE_SETTINGS) ? 42 : 52;
        FontFamily ff(g_serif);
        Font font(&ff, pt, FontStyleRegular, UnitPixel);
        // Typographic formatting: the default adds a leading space of its own, which widened the gap again.
        StringFormat fmt(StringFormat::GenericTypographic());
        fmt.SetFormatFlags(fmt.GetFormatFlags() | StringFormatFlagsNoClip);
        RectF bb;
        g.MeasureString(title, -1, &font, RectF(0, 0, 4000, 200), &fmt, &bb);
        // The mark's own PNG carries about a tenth of transparent margin on each side, so the gap that shows is
        // wider than the gap asked for.
        const REAL gap = ms * 0.10f;
        REAL x = cx - ((g_logo ? ms + gap : 0) + bb.Width) / 2;
        const REAL y = (g_page == PAGE_SETTINGS) ? 30.0f : 28.0f;
        if (g_logo) {
            // Centre the mark on the CAPITALS, not on the line box: the title is all caps, so the descender
            // space below it is empty and centring on the line would sit the mark visibly low.
            const REAL em   = (REAL)ff.GetEmHeight(FontStyleRegular);
            const REAL asc  = (REAL)ff.GetCellAscent(FontStyleRegular) / em * pt;   // baseline, from the top
            const REAL caps = pt * 0.70f;                                           // cap height, near enough
            const REAL mid  = y + asc - caps / 2.0f;
            g.DrawImage(g_logo, Rect((INT)x, (INT)(mid - ms / 2.0f), ms, ms));
            x += ms + gap;
        }
        SolidBrush br(Color(255, 226, 190, 116));
        g.DrawString(title, -1, &font, PointF(x, y), &fmt, &br);
    }
    if (g_page == PAGE_SETTINGS) {
        drawText(g, L"the program each cover starts - leave one empty to forget it", g_serif, 12.5f, FontStyleItalic,
                 Color(190, 150, 132, 96), cx, 66.0f, (REAL)cl.right);
        { Pen div(Color(90, 196, 158, 86), 1.0f);
          g.DrawLine(&div, cx - 150, 92.0f, cx - 18, 92.0f);
          g.DrawLine(&div, cx + 18, 92.0f, cx + 150, 92.0f);
          SolidBrush dot(Color(150, 216, 180, 110));
          g.FillEllipse(&dot, cx - 3.0f, 89.0f, 6.0f, 6.0f); }
        for (int i = 0; i < NSLOT; ++i) {
            WCHAR w[160];
            char line[160];
            _snprintf(line, sizeof(line), "%s   (normally %s)", g_slot[i].name, g_slot[i].wanted);
            MultiByteToWideChar(CP_ACP, 0, line, -1, w, 160);
            drawText(g, w, g_serif, 14.0f, FontStyleRegular, Color(255, 206, 172, 104),
                     460.0f, (REAL)ROW_Y(i), 800.0f, StringAlignmentNear);
        }
        for (int i = 0; i < NSLOT; ++i) {           // the boxes are borderless: this is their frame
            Rect b(60, ROW_Y(i) + 26, 760, 26);
            SolidBrush fill(Color(255, 26, 22, 17));
            g.FillRectangle(&fill, b);
            Pen edge(Color(255, 120, 94, 48), 1.0f);
            g.DrawRectangle(&edge, b);
        }
#ifdef BFME2_PREVIEW
        // the boxes and buttons are real child windows, invisible to this renderer - outline where they land
        for (int i = 0; i < NSLOT; ++i) {
            Rect bb(832, ROW_Y(i) + 26, 92, 26);
            LinearGradientBrush face(Rect(bb.X, bb.Y, bb.Width, bb.Height + 1), Color(255, 56, 46, 33), Color(255, 34, 28, 20), 90.0f);
            g.FillRectangle(&face, bb);
            Pen e(Color(255, 146, 114, 56), 1.0f);
            g.DrawRectangle(&e, bb);
            WCHAR w[MAX_PATH];
            MultiByteToWideChar(CP_ACP, 0, g_slot[i].have ? g_slot[i].exe : "", -1, w, MAX_PATH);
            drawText(g, w, L"Segoe UI", 11.5f, FontStyleRegular, Color(220, 226, 200, 150), 440.0f, (REAL)ROW_Y(i) + 30, 740.0f, StringAlignmentNear);
            drawText(g, L"Browse...", g_serif, 13.0f, FontStyleRegular, Color(255, 226, 196, 132), 878.0f, (REAL)ROW_Y(i) + 31, 90.0f);
        }
        for (int k = 0; k < 2; ++k) {
            Rect bb(k ? 832 : 732, CLIENT_H - 54, 92, 28);
            LinearGradientBrush face(Rect(bb.X, bb.Y, bb.Width, bb.Height + 1), Color(255, 56, 46, 33), Color(255, 34, 28, 20), 90.0f);
            g.FillRectangle(&face, bb);
            Pen e(Color(255, k ? 146 : 226, k ? 114 : 190, k ? 56 : 116), 1.0f);
            g.DrawRectangle(&e, bb);
            drawText(g, k ? L"Back" : L"Save", g_serif, 13.0f, FontStyleRegular, Color(255, 226, 196, 132),
                     (REAL)bb.X + 46.0f, (REAL)CLIENT_H - 48.0f, 90.0f);
        }
#endif
        { Pen rule(Color(60, 196, 158, 86), 1.0f);
          g.DrawLine(&rule, 70, cl.bottom - 74, cl.right - 70, cl.bottom - 74); }
        drawText(g, L"Saved to bfme2_accel.ini beside this program, and read back the next time it starts.",
                 L"Segoe UI", 11.0f, FontStyleRegular, Color(160, 142, 124, 92), cx, (REAL)cl.bottom - 66, (REAL)cl.right - 90);
        return;
    }
    drawText(g, L"the same game, on more than one core", g_serif, 13.0f, FontStyleItalic, Color(190, 150, 132, 96), cx, 68.0f, (REAL)cl.right);
    { Pen div(Color(90, 196, 158, 86), 1.0f);
      g.DrawLine(&div, cx - 150, 96.0f, cx - 18, 96.0f);
      g.DrawLine(&div, cx + 18, 96.0f, cx + 150, 96.0f);
      SolidBrush dot(Color(150, 216, 180, 110));
      g.FillEllipse(&dot, cx - 3.0f, 93.0f, 6.0f, 6.0f); }

    // A tile whose install is owned by a mod launcher is a signpost, not a door: hovering it also lights the tile
    // it leads to, so pressing it cannot be a surprise.
    int leadsTo = -1;
    if (g_hover >= 0 && g_slot[g_hover].have && g_slot[g_hover].managed)
        for (int j = 0; j < NSLOT; ++j)
            if (j != g_hover && g_slot[j].have && !lstrcmpiA(g_slot[j].exe, g_slot[g_hover].exe)) { leadsTo = j; break; }

    for (int i = 0; i < NSLOT; ++i) {
        const Slot& s = g_slot[i];
        const bool hot = (g_hover == i);              // a game that is missing is still worth hovering: it can be found
        const bool lit = (leadsTo == i);
        Rect box(s.rc.left, s.rc.top, TILE, TILE);

        if (hot || lit) {                             // a soft glow instead of a highlight rectangle
            for (int k = 6; k >= 1; --k) {
                BYTE a = (BYTE)((lit && !hot ? 6 : (s.have ? 12 : 8)) + (lit && !hot ? 4 : (s.have ? 9 : 5)) * (7 - k));
                Pen glow(s.have ? Color(a, 236, 202, 126) : Color(a, 170, 170, 176), (REAL)k * 2.0f);
                g.DrawRectangle(&glow, box.X - k, box.Y - k, box.Width + 2 * k, box.Height + 2 * k);
            }
        }
        if (s.img && !s.have) {                       // a game that is not here is drawn drained of its colour
            static const ColorMatrix gray = {
                { { 0.30f, 0.30f, 0.30f, 0.0f, 0.0f },
                  { 0.59f, 0.59f, 0.59f, 0.0f, 0.0f },
                  { 0.11f, 0.11f, 0.11f, 0.0f, 0.0f },
                  { 0.0f,  0.0f,  0.0f,  1.0f, 0.0f },
                  { 0.0f,  0.0f,  0.0f,  0.0f, 1.0f } } };
            ImageAttributes ia;
            ia.SetColorMatrix(&gray, ColorMatrixFlagsDefault, ColorAdjustTypeBitmap);
            g.DrawImage(s.img, box, 0, 0, (INT)s.img->GetWidth(), (INT)s.img->GetHeight(), UnitPixel, &ia);
        } else if (s.img) g.DrawImage(s.img, box);
        else { SolidBrush b(Color(255, 30, 26, 20)); g.FillRectangle(&b, box); }

        if (!s.have) {                                // installed or not, it is always shown - drained when not
            SolidBrush veil(Color(hot ? 120 : 160, 6, 5, 4));
            g.FillRectangle(&veil, box);
        } else if (s.managed) {
            SolidBrush veil(Color(hot ? 96 : 120, 8, 6, 4));   // owned by another launcher: a signpost, kept quieter
            g.FillRectangle(&veil, box);
        } else if (!hot && !lit) {
            SolidBrush veil(Color(56, 8, 6, 4));      // the one under the cursor is the bright one
            g.FillRectangle(&veil, box);
        }
        Pen frame(s.have ? (hot ? (s.managed ? Color(255, 190, 164, 112) : Color(255, 240, 208, 132))
                                : Color(255, 138, 106, 47))
                         : (hot ? Color(255, 150, 148, 152) : Color(255, 66, 62, 58)), hot ? 2.0f : 1.0f);
        g.DrawRectangle(&frame, box);

        WCHAR wname[128], wsub[160];
        MultiByteToWideChar(CP_ACP, 0, s.name, -1, wname, 128);
        char sub[160];
        if (!s.have) lstrcpynA(sub, "not found  -  click to locate", sizeof(sub));
        else if (s.managed) _snprintf(sub, sizeof(sub), "opens %s", s.managed);
        else lstrcpynA(sub, s.sub, sizeof(sub));
        MultiByteToWideChar(CP_ACP, 0, sub, -1, wsub, 160);
        REAL tcx = (REAL)(s.rc.left + TILE / 2);
        drawText(g, wname, g_serif, 15.0f, FontStyleRegular,
                 s.have ? (hot ? Color(255, 244, 218, 152) : Color(255, 198, 164, 98))
                        : (hot ? Color(255, 166, 162, 158) : Color(255, 104, 96, 86)),
                 tcx, (REAL)(s.rc.bottom + 12), (REAL)TILE + 20);
        drawText(g, wsub, g_serif, 11.5f, FontStyleItalic,
                 s.have ? Color(215, 136, 118, 84) : (hot ? Color(240, 150, 146, 142) : Color(255, 92, 84, 74)),
                 tcx, (REAL)(s.rc.bottom + 33), (REAL)TILE + 20);
    }

    { Pen rule(Color(60, 196, 158, 86), 1.0f);
      g.DrawLine(&rule, 70, cl.bottom - 44, cl.right - 70, cl.bottom - 44); }
    // the way into the settings page, drawn rather than a native button so it belongs to the rest of it
    {
        const WCHAR* gear = L"Paths...";
        g_gearRc.left = 60; g_gearRc.top = cl.bottom - 36; g_gearRc.right = 150; g_gearRc.bottom = cl.bottom - 16;
        drawText(g, gear, g_serif, 12.5f, FontStyleRegular,
                 g_gearHot ? Color(255, 244, 218, 152) : Color(255, 170, 142, 88),
                 (REAL)(g_gearRc.left + g_gearRc.right) / 2, (REAL)g_gearRc.top - 2, 120.0f);
    }
    const WCHAR* foot = L"Pick one and this window is gone. It loads itself when the game starts, and closes when you are done.";
    WCHAR w[MAX_PATH + 160];
    if (g_hover >= 0 && g_slot[g_hover].have && g_slot[g_hover].managed) {
        char line[MAX_PATH + 160];
        _snprintf(line, sizeof(line), "This install is wired up by %s, so that is what opens - pick %s there.",
                  g_slot[g_hover].managed, g_slot[g_hover].name);
        MultiByteToWideChar(CP_ACP, 0, line, -1, w, MAX_PATH + 160);
        drawText(g, w, L"Segoe UI", 11.0f, FontStyleRegular, Color(200, 170, 140, 100), cx, (REAL)cl.bottom - 32, (REAL)cl.right - 90);
    } else if (g_hover >= 0 && g_slot[g_hover].have) {
        MultiByteToWideChar(CP_ACP, 0, g_slot[g_hover].exe, -1, w, MAX_PATH);
        drawText(g, w, L"Segoe UI", 11.0f, FontStyleRegular, Color(190, 150, 132, 96), cx, (REAL)cl.bottom - 32, (REAL)cl.right - 90);
    } else if (g_hover >= 0) {
        char line[MAX_PATH + 96];
        _snprintf(line, sizeof(line), "Not where this could look for it. Click to point at %s yourself.", g_slot[g_hover].wanted);
        MultiByteToWideChar(CP_ACP, 0, line, -1, w, MAX_PATH + 96);
        drawText(g, w, L"Segoe UI", 11.0f, FontStyleRegular, Color(200, 158, 154, 150), cx, (REAL)cl.bottom - 32, (REAL)cl.right - 90);
    } else {
        drawText(g, foot, L"Segoe UI", 11.0f, FontStyleRegular, Color(160, 142, 124, 92), cx, (REAL)cl.bottom - 32, (REAL)cl.right - 90);
    }
}

// A game somewhere this could not look. The player points at it once and it is written down for next time.
static bool locateSlot(HWND owner, int i, bool persist) {
    char file[MAX_PATH]; file[0] = 0;
    char title[160];
    _snprintf(title, sizeof(title), "Where is %s?", g_slot[i].name);
    // OPENFILENAME wants description and pattern as NUL-separated pairs, ended by a second NUL
    char filter[256]; int n = 0;
    { char one[96]; int k = _snprintf(one, sizeof(one), "The game program (%s)", g_slot[i].wanted);
      memcpy(filter + n, one, k + 1); n += k + 1;
      k = (int)strlen(g_slot[i].wanted); memcpy(filter + n, g_slot[i].wanted, k + 1); n += k + 1;
      memcpy(filter + n, "Any program", 12); n += 12;
      memcpy(filter + n, "*.exe", 6); n += 6;
      filter[n] = 0; }

    OPENFILENAMEA ofn; ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = title;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
    if (!GetOpenFileNameA(&ofn) || !exists(file)) return false;

    const char* leaf = strrchr(file, '\\'); leaf = leaf ? leaf + 1 : file;
    if (_stricmp(leaf, g_slot[i].wanted)) {            // accepted anyway - a renamed or repackaged launcher is normal
        char msg[MAX_PATH + 220];
        _snprintf(msg, sizeof(msg),
                  "That is %s, not %s.\n\nIt will be used for %s anyway - press OK if that is what you meant.",
                  leaf, g_slot[i].wanted, g_slot[i].name);
        if (MessageBoxA(owner, msg, "BFME2 Accelerator", MB_ICONQUESTION | MB_OKCANCEL) != IDOK) return false;
    }
    lstrcpynA(g_slot[i].exe, file, MAX_PATH);
    g_slot[i].have = true;
    if (persist) saveGames();
    return true;
}

// ---------------------------------------------------------------- the settings page
static void showPage(HWND hwnd, int page) {
    g_page = page;
    const int show = (page == PAGE_SETTINGS) ? SW_SHOW : SW_HIDE;
    for (int i = 0; i < NSLOT; ++i) {
        if (page == PAGE_SETTINGS) SetWindowTextA(g_edit[i], g_slot[i].have ? g_slot[i].exe : "");
        ShowWindow(g_edit[i], show);
        ShowWindow(g_browse[i], show);
    }
    ShowWindow(g_save, show);
    ShowWindow(g_back, show);
    g_hover = -1; g_gearHot = false;
    InvalidateRect(hwnd, NULL, FALSE);
}

// Take what is in the boxes. A path that is empty clears that game; a path that does not exist is refused by
// name so a typo cannot quietly leave a game unstartable.
static bool applySettings(HWND hwnd) {
    char bad[MAX_PATH + 160]; bad[0] = 0;
    for (int i = 0; i < NSLOT; ++i) {
        char p[MAX_PATH]; p[0] = 0;
        GetWindowTextA(g_edit[i], p, MAX_PATH);
        for (char* q = p + strlen(p); q > p && (q[-1] == ' ' || q[-1] == '"'); --q) q[-1] = 0;
        char* start = p; while (*start == ' ' || *start == '"') ++start;
        if (*start && !exists(start)) {
            _snprintf(bad, sizeof(bad), "This is not a file that exists:\n\n%s\n\nFix it, clear the box, or press Back.", start);
            MessageBoxA(hwnd, bad, "BFME2 Accelerator", MB_ICONWARNING | MB_OK);
            SetFocus(g_edit[i]);
            return false;
        }
        if (*start) {
            if (lstrcmpiA(start, g_slot[i].exe)) g_slot[i].managed = NULL;   // changed by hand: no longer a signpost
            lstrcpynA(g_slot[i].exe, start, MAX_PATH);
            g_slot[i].have = true;
        }
        else { g_slot[i].exe[0] = 0; g_slot[i].have = false; g_slot[i].managed = NULL; }
    }
    saveGames();
    return true;
}

static int hitTest(int x, int y) {
    for (int i = 0; i < NSLOT; ++i) {
        const RECT& r = g_slot[i].rc;
        if (x >= r.left && x < r.right && y >= r.top && y < r.bottom) return i;
    }
    return -1;
}

static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        for (int i = 0; i < NSLOT; ++i) {
            g_slot[i].rc.left   = MARGIN + i * (TILE + GAP);
            g_slot[i].rc.top    = TOP;
            g_slot[i].rc.right  = g_slot[i].rc.left + TILE;
            g_slot[i].rc.bottom = TOP + TILE;
        }
        g_uiFont = CreateFontA(-12, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                               CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, "Segoe UI");
        g_editBg = CreateSolidBrush(RGB(26, 22, 17));
        HINSTANCE inst = (HINSTANCE)GetWindowLongPtrA(hwnd, GWLP_HINSTANCE);
        for (int i = 0; i < NSLOT; ++i) {
            g_edit[i] = CreateWindowExA(0, "EDIT", "", WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL,
                                        62, ROW_Y(i) + 28, 756, 22, hwnd, (HMENU)(ID_EDIT + i), inst, NULL);
            g_browse[i] = CreateWindowA("BUTTON", "Browse...", WS_CHILD | WS_TABSTOP | BS_OWNERDRAW,
                                        832, ROW_Y(i) + 26, 92, 26, hwnd, (HMENU)(ID_BROWSE + i), inst, NULL);
            SendMessageA(g_edit[i], EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(8, 8));
            SendMessageA(g_edit[i], WM_SETFONT, (WPARAM)g_uiFont, TRUE);
            SendMessageA(g_browse[i], WM_SETFONT, (WPARAM)g_uiFont, TRUE);
            SendMessageA(g_edit[i], EM_SETLIMITTEXT, MAX_PATH - 1, 0);
        }
        g_save = CreateWindowA("BUTTON", "Save", WS_CHILD | WS_TABSTOP | BS_OWNERDRAW,
                               732, CLIENT_H - 54, 92, 28, hwnd, (HMENU)ID_SAVE, inst, NULL);
        g_back = CreateWindowA("BUTTON", "Back", WS_CHILD | WS_TABSTOP | BS_OWNERDRAW,
                               832, CLIENT_H - 54, 92, 28, hwnd, (HMENU)ID_BACK, inst, NULL);
        SendMessageA(g_save, WM_SETFONT, (WPARAM)g_uiFont, TRUE);
        SendMessageA(g_back, WM_SETFONT, (WPARAM)g_uiFont, TRUE);
        return 0;
    }
    case WM_DRAWITEM: {
        DRAWITEMSTRUCT* di = (DRAWITEMSTRUCT*)lp;
        const bool down = (di->itemState & ODS_SELECTED) != 0;
        const bool focus = (di->itemState & ODS_FOCUS) != 0;
        Graphics g(di->hDC);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
        Rect r(di->rcItem.left, di->rcItem.top, di->rcItem.right - di->rcItem.left - 1,
               di->rcItem.bottom - di->rcItem.top - 1);
        LinearGradientBrush face(Rect(r.X, r.Y, r.Width, r.Height + 1),
                                 down ? Color(255, 44, 36, 26) : Color(255, 56, 46, 33),
                                 down ? Color(255, 30, 25, 18) : Color(255, 34, 28, 20), 90.0f);
        g.FillRectangle(&face, r);
        Pen edge(focus || down ? Color(255, 226, 190, 116) : Color(255, 146, 114, 56), 1.0f);
        g.DrawRectangle(&edge, r);
        char txt[64]; txt[0] = 0;
        GetWindowTextA(di->hwndItem, txt, sizeof(txt));
        WCHAR w[64]; MultiByteToWideChar(CP_ACP, 0, txt, -1, w, 64);
        drawText(g, w, g_serif, 13.0f, FontStyleRegular,
                 down ? Color(255, 246, 224, 168) : Color(255, 226, 196, 132),
                 (REAL)(r.X + r.Width / 2.0f), (REAL)(r.Y + (r.Height - 15) / 2.0f), (REAL)r.Width);
        return TRUE;
    }
    case WM_CTLCOLOREDIT: {                               // the boxes belong to the same dark page
        HDC dc = (HDC)wp;
        SetTextColor(dc, RGB(226, 200, 150));
        SetBkColor(dc, RGB(26, 22, 17));
        return (LRESULT)g_editBg;
    }
    case WM_COMMAND: {
        int id = LOWORD(wp);
        if (id >= ID_BROWSE && id < ID_BROWSE + NSLOT) {
            int i = id - ID_BROWSE;
            char before[MAX_PATH]; GetWindowTextA(g_edit[i], before, MAX_PATH);
            bool had = g_slot[i].have;
            char keep[MAX_PATH]; lstrcpynA(keep, g_slot[i].exe, MAX_PATH);
            g_slot[i].have = false;                       // locateSlot fills exe/have; the box is the real answer
            if (locateSlot(hwnd, i, false)) SetWindowTextA(g_edit[i], g_slot[i].exe);
            else { g_slot[i].have = had; lstrcpynA(g_slot[i].exe, keep, MAX_PATH); }
            return 0;
        }
        if (id == ID_SAVE) { if (applySettings(hwnd)) showPage(hwnd, PAGE_PICK); return 0; }
        if (id == ID_BACK) { showPage(hwnd, PAGE_PICK); return 0; }
        return 0;
    }
    case WM_ERASEBKGND: return 1;                     // everything is painted into a back buffer below
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(hwnd, &ps);
        RECT cl; GetClientRect(hwnd, &cl);
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, cl.right, cl.bottom);
        HGDIOBJ old = SelectObject(mem, bmp);
        paint(hwnd, mem, cl);
        BitBlt(dc, 0, 0, cl.right, cl.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old); DeleteObject(bmp); DeleteDC(mem);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (g_page != PAGE_PICK) return 0;
        int mx = LOWORD(lp), my = HIWORD(lp);
        bool gear = (mx >= g_gearRc.left && mx < g_gearRc.right && my >= g_gearRc.top && my < g_gearRc.bottom);
        if (gear != g_gearHot) { g_gearHot = gear; InvalidateRect(hwnd, NULL, FALSE); }
        int h = hitTest(mx, my);
        if (h != g_hover) {
            g_hover = h;
            SetCursor(LoadCursorA(NULL, h >= 0 ? IDC_HAND : IDC_ARROW));   // a missing game is clickable too
            InvalidateRect(hwnd, NULL, FALSE);
        }
        TRACKMOUSEEVENT tme; tme.cbSize = sizeof(tme); tme.dwFlags = TME_LEAVE; tme.hwndTrack = hwnd; tme.dwHoverTime = 0;
        TrackMouseEvent(&tme);
        return 0;
    }
    case WM_SETCURSOR:
        if (g_page == PAGE_PICK && (g_hover >= 0 || g_gearHot)) { SetCursor(LoadCursorA(NULL, IDC_HAND)); return TRUE; }
        break;
    case WM_MOUSELEAVE:
        if (g_hover != -1 || g_gearHot) { g_hover = -1; g_gearHot = false; InvalidateRect(hwnd, NULL, FALSE); }
        return 0;
    case WM_LBUTTONUP: {
        if (g_page != PAGE_PICK) return 0;
        int mx = LOWORD(lp), my = HIWORD(lp);
        if (mx >= g_gearRc.left && mx < g_gearRc.right && my >= g_gearRc.top && my < g_gearRc.bottom) {
            showPage(hwnd, PAGE_SETTINGS);
            return 0;
        }
        int h = hitTest(mx, my);
        if (h < 0) return 0;
        if (!g_slot[h].have) {                        // missing: ask where it is, and stay on the window
            if (locateSlot(hwnd, h, true)) InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        char exe[MAX_PATH]; lstrcpynA(exe, g_slot[h].exe, MAX_PATH);
        ShowWindow(hwnd, SW_HIDE);                    // from here on it is headless
        runHeadless(exe);
        PostQuitMessage(0);
        return 0;
    }
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) { if (g_page == PAGE_SETTINGS) showPage(hwnd, PAGE_PICK); else PostQuitMessage(0); }
        return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE, LPSTR cmdLine, int) {
    siblingPath("bfme2_accel.dll", g_dll, sizeof(g_dll));
    if (!exists(g_dll)) {
        char msg[MAX_PATH + 220];
        _snprintf(msg, sizeof(msg),
                  "bfme2_accel.dll is not next to this program.\n\nLooked for:\n%s\n\n"
                  "If it was there a moment ago, your antivirus has probably quarantined it.", g_dll);
        MessageBoxA(NULL, msg, "BFME2 Accelerator", MB_ICONERROR | MB_OK);
        return 1;
    }
    enableDebugPrivilege();
    findInstalls();

    if (cmdLine && *cmdLine) {                        // an explicit path skips the window entirely
        char one[MAX_PATH]; lstrcpynA(one, cmdLine, MAX_PATH);
        if (one[0] == '"') { memmove(one, one + 1, strlen(one)); char* e = strchr(one, '"'); if (e) *e = 0; }
        if (exists(one)) { runHeadless(one); return 0; }
    }
    // Nothing found is not an error any more: the window opens with every game greyed out and each one can be
    // pointed at by hand.
    GdiplusStartupInput gsi;
    GdiplusStartup(&g_gdip, &gsi, NULL);
    { FontFamily ff(g_serif); if (!ff.IsAvailable()) lstrcpynW(g_serif, L"Georgia", 64); }
    for (int i = 0; i < NSLOT; ++i) g_slot[i].img = loadCover(inst, g_slot[i].resId);
    g_logo = loadCover(inst, 104);

    // Both sizes come out of the icon resource rather than one being scaled from the other: the title bar and
    // the task bar ask for different sizes, and a 32-pixel icon squeezed to 16 looks like mud.
    HICON iconBig = (HICON)LoadImageA(inst, MAKEINTRESOURCEA(1), IMAGE_ICON,
                                      GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR);
    HICON iconSmall = (HICON)LoadImageA(inst, MAKEINTRESOURCEA(1), IMAGE_ICON,
                                        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
    WNDCLASSEXA wc; ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = wndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorA(NULL, IDC_ARROW);
    wc.hIcon = iconBig;
    wc.hIconSm = iconSmall;
    wc.hbrBackground = NULL;
    wc.lpszClassName = "Bfme2AccelLoader";
    RegisterClassExA(&wc);

    RECT want = { 0, 0, CLIENT_W, CLIENT_H };
    DWORD style = (WS_OVERLAPPEDWINDOW & ~(WS_MAXIMIZEBOX | WS_THICKFRAME));
    AdjustWindowRect(&want, style, FALSE);
    int w = want.right - want.left, h = want.bottom - want.top;
    int x = (GetSystemMetrics(SM_CXSCREEN) - w) / 2, y = (GetSystemMetrics(SM_CYSCREEN) - h) / 2;
    g_hwnd = CreateWindowExA(0, wc.lpszClassName, "BFME2 Accelerator", style | WS_VISIBLE, x, y, w, h, NULL, NULL, inst, NULL);
    if (!g_hwnd) return 3;
    SendMessageA(g_hwnd, WM_SETICON, ICON_BIG, (LPARAM)iconBig);      // what the window keeps, not just starts with
    SendMessageA(g_hwnd, WM_SETICON, ICON_SMALL, (LPARAM)iconSmall);

    MSG msg;
    while (GetMessageA(&msg, NULL, 0, 0) > 0) {
        if (g_page == PAGE_SETTINGS && IsDialogMessageA(g_hwnd, &msg)) continue;
        TranslateMessage(&msg); DispatchMessageA(&msg);
    }
    for (int i = 0; i < NSLOT; ++i) delete g_slot[i].img;
    GdiplusShutdown(g_gdip);
    return 0;
}

#ifdef BFME2_DETECT_TEST
// A console build of just the detection, so what the window will show can be checked without a UAC prompt.
int main() {
    findInstalls();
    for (int i = 0; i < NSLOT; ++i)
        printf("  [%d] %-32s %s\n", i, g_slot[i].name, g_slot[i].have ? g_slot[i].exe : "(not installed)");
    char d[MAX_PATH]; siblingPath("bfme2_accel.dll", d, sizeof(d));
    printf("dll beside the loader: %s (%s)\n", d, exists(d) ? "present" : "MISSING");
    return 0;
}
#endif

#ifdef BFME2_PREVIEW
// Renders exactly what the window paints into a PNG, so the design can be looked at and changed without
// running the loader (which asks for administrator and starts a game).
#include <stdlib.h>
static int encoderClsid(const WCHAR* mime, CLSID* out) {
    UINT n = 0, sz = 0;
    GetImageEncodersSize(&n, &sz);
    if (!sz) return -1;
    ImageCodecInfo* info = (ImageCodecInfo*)malloc(sz);
    GetImageEncoders(n, sz, info);
    for (UINT i = 0; i < n; ++i) if (!wcscmp(info[i].MimeType, mime)) { *out = info[i].Clsid; free(info); return 0; }
    free(info); return -1;
}
int main(int argc, char** argv) {
    GdiplusStartupInput gsi; GdiplusStartup(&g_gdip, &gsi, NULL);
    { FontFamily ff(g_serif); if (!ff.IsAvailable()) lstrcpynW(g_serif, L"Georgia", 64); }
    HINSTANCE inst = GetModuleHandleA(NULL);
    for (int i = 0; i < NSLOT; ++i) g_slot[i].img = loadCover(inst, g_slot[i].resId);
    g_logo = loadCover(inst, 104);
    findInstalls();
    if (argc > 1 && !strcmp(argv[1], "allInstalled")) for (int i = 0; i < NSLOT; ++i) g_slot[i].have = true;
    if (argc > 1 && !strcmp(argv[1], "missing")) { for (int i = 0; i < NSLOT; ++i) g_slot[i].have = true; g_slot[0].have = false; }
    if (argc > 2) g_hover = atoi(argv[2]);
    if (argc > 1 && !strcmp(argv[1], "settings")) g_page = PAGE_SETTINGS;
    for (int i = 0; i < NSLOT; ++i) {
        g_slot[i].rc.left = MARGIN + i * (TILE + GAP); g_slot[i].rc.top = TOP;
        g_slot[i].rc.right = g_slot[i].rc.left + TILE; g_slot[i].rc.bottom = TOP + TILE;
    }
    RECT cl = { 0, 0, CLIENT_W, CLIENT_H };
    HDC screen = GetDC(NULL);
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateCompatibleBitmap(screen, CLIENT_W, CLIENT_H);
    HGDIOBJ old = SelectObject(mem, bmp);
    paint(NULL, mem, cl);
    Bitmap out(bmp, NULL);
    CLSID png; encoderClsid(L"image/png", &png);
    out.Save(L"preview.png", &png, NULL);
    SelectObject(mem, old); DeleteObject(bmp); DeleteDC(mem); ReleaseDC(NULL, screen);
    printf("wrote preview.png (%dx%d)\n", CLIENT_W, CLIENT_H);
    return 0;
}
#endif
