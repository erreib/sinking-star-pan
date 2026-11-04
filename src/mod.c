// sinking_star_pan.dll — drag-to-pan for Order of the Sinking Star's local area (Q) view.
//
// How it works: the game stores the camera's target point in its game-state struct each frame
// (state+0x3d90/+0x3d94) and then builds the camera from it. We hook the three instructions that
// read that point back and add a pan offset to the two registers, so the stored value (which the
// game also uses for its own smoothing) stays untouched. The offset is driven by mouse drags
// while the overhead view is active, and decays to zero when the view closes.
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>

#define OFF_TARGET_X   0x3d90
#define OFF_CAM_POS    0x3d98
#define OFF_CAM_FOV    0x3dc0
#define OFF_VIEW_IDX   0x3dd0
#define OFF_VIEW_MODE  0x3dd4
#define OFF_BLEND      0x3dd8
#define OFF_BOUNDS     0x3e04   // x0 y0 x1 y1 of the framed local area
#define OFF_FOLLOW     0x6820   // follow point (x y z)
#define STATE_SIZE     0x7f80

static HMODULE g_self;
static volatile int g_running = 1;
static char g_dir[MAX_PATH], g_log_path[MAX_PATH], g_ini_path[MAX_PATH], g_unload_path[MAX_PATH];

// --- config -------------------------------------------------------------------------------
static int   cfg_mode_move = 1;    // 1 = view follows the mouse position (no click), 0 = click and drag
static float cfg_gain = 1.3f;      // move mode: cursor at the window edge = gain * half a screen of pan
static float cfg_smooth = 0.08f;   // move mode: easing time constant in seconds
static int   cfg_recenter = 1;     // move mode: warp the cursor to the window centre when the view opens
static int   cfg_button = VK_LBUTTON;
static int   cfg_invert_x = 0, cfg_invert_y = 0;
static float cfg_sensitivity = 1.0f;
static float cfg_max_offset = 500.0f;
static int   cfg_modes = 0x4;      // bitmask of view modes that allow panning (bit 2 = local area view)
static int   cfg_log = 1;
static int   cfg_debug = 0;

static void mlog(const char *fmt, ...) {
    if (!cfg_log) return;
    FILE *f = fopen(g_log_path, "a");
    if (!f) return;
    SYSTEMTIME st; GetLocalTime(&st);
    fprintf(f, "%02d:%02d:%02d.%03d ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fputc('\n', f); fclose(f);
}

static void load_config(void) {
    char buf[64];
    GetPrivateProfileStringA("pan", "mode", "move", buf, sizeof buf, g_ini_path);
    cfg_mode_move = _stricmp(buf, "drag") != 0;
    GetPrivateProfileStringA("pan", "gain", "1.3", buf, sizeof buf, g_ini_path);
    cfg_gain = (float)atof(buf); if (!(cfg_gain > 0.01f)) cfg_gain = 1.3f;
    GetPrivateProfileStringA("pan", "smoothing", "0.08", buf, sizeof buf, g_ini_path);
    cfg_smooth = (float)atof(buf); if (!(cfg_smooth >= 0.0f)) cfg_smooth = 0.08f;
    cfg_recenter = GetPrivateProfileIntA("pan", "recenter_cursor", 1, g_ini_path);
    GetPrivateProfileStringA("pan", "button", "left", buf, sizeof buf, g_ini_path);
    cfg_button = !_stricmp(buf, "right") ? VK_RBUTTON : !_stricmp(buf, "middle") ? VK_MBUTTON : VK_LBUTTON;
    cfg_invert_x = GetPrivateProfileIntA("pan", "invert_x", 0, g_ini_path);
    cfg_invert_y = GetPrivateProfileIntA("pan", "invert_y", 0, g_ini_path);
    GetPrivateProfileStringA("pan", "sensitivity", "1.0", buf, sizeof buf, g_ini_path);
    cfg_sensitivity = (float)atof(buf); if (!(cfg_sensitivity > 0.01f)) cfg_sensitivity = 1.0f;
    GetPrivateProfileStringA("pan", "max_offset", "500", buf, sizeof buf, g_ini_path);
    cfg_max_offset = (float)atof(buf); if (!(cfg_max_offset > 1.0f)) cfg_max_offset = 500.0f;
    GetPrivateProfileStringA("pan", "modes", "2", buf, sizeof buf, g_ini_path);
    cfg_modes = 0; for (char *p = buf; *p; p++) if (*p >= '0' && *p <= '7') cfg_modes |= 1 << (*p - '0');
    cfg_log = GetPrivateProfileIntA("log", "enabled", 1, g_ini_path);
    cfg_debug = GetPrivateProfileIntA("log", "debug", 0, g_ini_path);
}

// --- memory helpers -----------------------------------------------------------------------
static int readable(const void *p, size_t n) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!p || VirtualQuery(p, &mbi, sizeof mbi) != sizeof mbi) return 0;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return 0;
    return (uintptr_t)p + n <= (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
}

static uintptr_t find_pattern(uintptr_t base, size_t size, const uint8_t *pat, const uint8_t *msk, size_t n, int *count) {
    const uint8_t *p = (const uint8_t *)base;
    uintptr_t first = 0; *count = 0;
    for (size_t i = 0; i + n <= size; i++) {
        size_t k = 0;
        for (; k < n; k++) if (msk[k] && p[i + k] != pat[k]) break;
        if (k == n) { if (!first) first = base + i; (*count)++; }
    }
    return first;
}

// Both overhead-map toggle handlers start with:  mov rsi,[rip+disp]; test byte ptr [rsi+0x7f60],1
// Every hit must resolve to the same global.
static uintptr_t find_state_global(uintptr_t base, size_t size) {
    static const uint8_t pat[] = {0x48,0x8B,0x35,0,0,0,0, 0xF6,0x86,0x60,0x7F,0x00,0x00,0x01};
    const uint8_t *p = (const uint8_t *)base;
    uintptr_t g = 0; int hits = 0;
    for (size_t i = 0; i + sizeof pat <= size; i++) {
        if (p[i] != 0x48 || p[i+1] != 0x8B || p[i+2] != 0x35 || memcmp(p + i + 7, pat + 7, 7) != 0) continue;
        int32_t disp; memcpy(&disp, p + i + 3, 4);
        uintptr_t cand = base + i + 7 + disp;
        if (hits && cand != g) { mlog("state-global signature: conflicting hits"); return 0; }
        g = cand; hits++;
    }
    if (!hits) mlog("state-global signature: no hits");
    return g;
}

// Hook site, inside the game's camera routine (camera.jai): the local-area view centres the camera
// on the midpoint of the framed bounds, read here (r13 = the camera block at state+0x3d98):
//   F2 41 0F 10 55 6C     movsd xmm2,[r13+0x6c]     bounds x0,y0
//   F2 41 0F 10 4D 74     movsd xmm1,[r13+0x74]     bounds x1,y1
//   0F 58 CA              addps xmm1,xmm2
// 15 bytes, no branch targets inside, flags dead (the next flag use is a fresh test). We replace
// them with an absolute jmp to a trampoline that re-executes them with the pan offset added to
// both corners (so the midpoint moves by the offset) and jumps back.
static const uint8_t k_hook_pat[] = {0xF2,0x41,0x0F,0x10,0x55,0x6C, 0xF2,0x41,0x0F,0x10,0x4D,0x74, 0x0F,0x58,0xCA, 0x0F,0x59,0x0D};
#define HOOK_LEN 15   // bytes displaced; the pattern includes 3 more bytes of context for uniqueness

static uintptr_t g_hook_site = 0;
static uint8_t   g_hook_orig[HOOK_LEN];
static uint8_t  *g_tramp = NULL;
static volatile float *g_off_x = NULL, *g_off_y = NULL;   // live offsets read by the trampoline
static volatile uint32_t *g_hits = NULL;                   // how often the trampoline ran
static volatile float g_target_x = 0.0f, g_target_y = 0.0f; // where the worker thread wants the view
static volatile int   g_snap = 0;                          // 1 = jump to the target instead of easing
static volatile uintptr_t g_state = 0;                     // game state (for the trace)

// Per-frame trace for diagnosing motion quality (dumped on request when debug logging is on).
typedef struct { double t; float dt, tx, ex, raw, avg, midx; } Trace;
#define TRACE_N 1024
static Trace g_trace[TRACE_N]; static volatile unsigned g_trace_n = 0;

// Runs on the game thread, once per frame, right before the camera routine reads the bounds.
// Easing here (with the real frame time) instead of on the polling thread keeps the motion
// smooth: the thread's ticks never line up with frames, which showed up as small skips.
static void pan_tick(void) {
    static LARGE_INTEGER freq, last;
    static float ex = 0.0f, ey = 0.0f;
    LARGE_INTEGER now;
    if (!freq.QuadPart) { QueryPerformanceFrequency(&freq); QueryPerformanceCounter(&last); }
    QueryPerformanceCounter(&now);
    float raw = (float)(now.QuadPart - last.QuadPart) / (float)freq.QuadPart;
    last = now;
    if (!(raw > 0.0f)) raw = 0.0f;
    if (raw > 0.1f) raw = 0.1f;
    // Smooth the frame time: a late frame would otherwise move the view a long way in one step,
    // which reads as a skip. Use a running average and keep each frame within 30% of it.
    static float avg = 0.0f;
    float dt = raw;
    if (raw > 0.002f) {                          // ignore back-to-back calls within the same frame
        if (avg <= 0.0f) avg = raw;
        float lo = avg * 0.7f, hi = avg * 1.3f;
        if (dt < lo) dt = lo;
        if (dt > hi) dt = hi;
        avg += (raw - avg) * 0.1f;
    }
    float tx = g_target_x, ty = g_target_y;
    // Under Wine the cursor position arrives in bursts (several frames with no change, then a big
    // step), which the easing alone turns into visible speed swings. Estimate the target's velocity
    // from its changes and carry the motion forward a little between updates.
    static float ltx = 0, lty = 0, vx = 0, vy = 0; static double tchange = 0, tnow = 0;
    tnow = (double)now.QuadPart / (double)freq.QuadPart;
    if (tx != ltx || ty != lty) {
        double gap = tnow - tchange;
        if (gap > 0.004 && gap < 0.15) {
            float nvx = (tx - ltx) / (float)gap, nvy = (ty - lty) / (float)gap;
            vx += (nvx - vx) * 0.5f; vy += (nvy - vy) * 0.5f;
        } else { vx = vy = 0; }
        ltx = tx; lty = ty; tchange = tnow;
    }
    float since = (float)(tnow - tchange);
    if (since > 0.12f || g_snap) { vx = vy = 0; }
    else { vx *= 0.97f; vy *= 0.97f; }
    float lead = since < 0.06f ? since : 0.06f;        // at most ~2 frames of prediction
    float px = tx + vx * lead, py = ty + vy * lead;
    float k = (g_snap || cfg_smooth <= 0.0005f) ? 1.0f : 1.0f - expf(-dt / cfg_smooth);
    ex += (px - ex) * k; ey += (py - ey) * k;
    if (fabsf(ex) < 0.0005f) ex = 0.0f;
    if (fabsf(ey) < 0.0005f) ey = 0.0f;
    *g_off_x = ex; *g_off_y = ey;
    if (cfg_debug) {
        Trace *tr = &g_trace[g_trace_n % TRACE_N];
        tr->t = (double)now.QuadPart / (double)freq.QuadPart; tr->dt = dt; tr->tx = tx; tr->ex = ex;
        tr->raw = raw; tr->avg = avg;
        uintptr_t st = g_state;
        tr->midx = st ? (*(float *)(st + OFF_BOUNDS) + *(float *)(st + OFF_BOUNDS + 8)) * 0.5f : 0;
        g_trace_n++;
    }
}

static void dump_trace(void) {
    unsigned n = g_trace_n, from = n > TRACE_N ? n - TRACE_N : 0;
    FILE *f = fopen(g_log_path, "a"); if (!f) return;
    fprintf(f, "--- trace (%u frames): t dt tx ex raw_dt avg_dt midx\n", n - from);
    for (unsigned i = from; i < n; i++) { Trace *tr = &g_trace[i % TRACE_N];
        fprintf(f, "%.4f %.4f %.3f %.3f %.4f %.4f %.3f\n", tr->t, tr->dt, tr->tx, tr->ex, tr->raw, tr->avg, tr->midx); }
    fclose(f);
}

static int build_trampoline(uintptr_t site) {
    g_tramp = VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!g_tramp) return 0;
    uint8_t *t = g_tramp; size_t i = 0;
    const size_t data = 0x100;                                  // 16-byte aligned: (offx, offy, 0, 0), then hits
    // --- call pan_tick() with the Windows x64 volatile state preserved ---
    static const uint8_t save[] = {
        0x50, 0x51, 0x52, 0x41,0x50, 0x41,0x51, 0x41,0x52, 0x41,0x53,   // push rax rcx rdx r8 r9 r10 r11
        0x48,0x89,0xE0,                                                 // mov rax,rsp
        0x48,0x81,0xEC,0xA0,0x00,0x00,0x00,                             // sub rsp,0xA0
        0x48,0x83,0xE4,0xF0,                                            // and rsp,-16
        0x48,0x89,0x84,0x24,0x90,0x00,0x00,0x00,                        // mov [rsp+0x90],rax
        0x0F,0x11,0x44,0x24,0x20, 0x0F,0x11,0x4C,0x24,0x30,             // movups [rsp+0x20],xmm0 / +0x30,xmm1
        0x0F,0x11,0x54,0x24,0x40, 0x0F,0x11,0x5C,0x24,0x50,             // xmm2 / xmm3
        0x0F,0x11,0x64,0x24,0x60, 0x0F,0x11,0x6C,0x24,0x70,             // xmm4 / xmm5
        0x48,0xB8 };                                                    // mov rax, imm64 (follows)
    static const uint8_t restore[] = {
        0xFF,0xD0,                                                      // call rax
        0x0F,0x10,0x44,0x24,0x20, 0x0F,0x10,0x4C,0x24,0x30,
        0x0F,0x10,0x54,0x24,0x40, 0x0F,0x10,0x5C,0x24,0x50,
        0x0F,0x10,0x64,0x24,0x60, 0x0F,0x10,0x6C,0x24,0x70,             // movups xmm0..5 back
        0x48,0x8B,0xA4,0x24,0x90,0x00,0x00,0x00,                        // mov rsp,[rsp+0x90]
        0x41,0x5B, 0x41,0x5A, 0x41,0x59, 0x41,0x58, 0x5A, 0x59, 0x58 }; // pop r11 r10 r9 r8 rdx rcx rax
    memcpy(t + i, save, sizeof save); i += sizeof save;
    { uint64_t fn = (uint64_t)(uintptr_t)&pan_tick; memcpy(t + i, &fn, 8); i += 8; }
    memcpy(t + i, restore, sizeof restore); i += sizeof restore;
    // --- original: movsd xmm2,[r13+0x6c]; movsd xmm1,[r13+0x74] ---
    memcpy(t + i, k_hook_pat, 12); i += 12;
    // addps xmm2,[rip+disp]   0F 58 15 disp32
    t[i++] = 0x0F; t[i++] = 0x58; t[i++] = 0x15;
    { int32_t d = (int32_t)(data - (i + 4)); memcpy(t + i, &d, 4); i += 4; }
    // addps xmm1,[rip+disp]   0F 58 0D disp32
    t[i++] = 0x0F; t[i++] = 0x58; t[i++] = 0x0D;
    { int32_t d = (int32_t)(data - (i + 4)); memcpy(t + i, &d, 4); i += 4; }
    // addps xmm1,xmm2 (original)
    t[i++] = 0x0F; t[i++] = 0x58; t[i++] = 0xCA;
    // inc dword [rip+disp]  FF 05 disp32
    t[i++] = 0xFF; t[i++] = 0x05;
    { int32_t d = (int32_t)(data + 16 - (i + 4)); memcpy(t + i, &d, 4); i += 4; }
    // jmp [rip+0]; dq return
    t[i++] = 0xFF; t[i++] = 0x25; memset(t + i, 0, 4); i += 4;
    { uint64_t ret = site + HOOK_LEN; memcpy(t + i, &ret, 8); i += 8; }
    memset(t + data, 0, 32);
    g_off_x = (volatile float *)(t + data); g_off_y = (volatile float *)(t + data + 4);
    g_hits = (volatile uint32_t *)(t + data + 16);
    FlushInstructionCache(GetCurrentProcess(), t, 4096);
    return 1;
}

// Patch/unpatch with every other thread suspended, and refuse while any of them sits inside the site.
static int patch_site(uintptr_t site, const uint8_t *bytes, size_t n) {
    DWORD pid = GetCurrentProcessId(), tid = GetCurrentThreadId();
    HANDLE threads[256]; int nt = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        THREADENTRY32 te = { .dwSize = sizeof te };
        if (Thread32First(snap, &te)) do {
            if (te.th32OwnerProcessID != pid || te.th32ThreadID == tid) continue;
            HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, te.th32ThreadID);
            if (h && nt < 256) { SuspendThread(h); threads[nt++] = h; } else if (h) CloseHandle(h);
        } while (Thread32Next(snap, &te));
        CloseHandle(snap);
    }
    int busy = 0;
    for (int k = 0; k < nt; k++) {
        CONTEXT c; memset(&c, 0, sizeof c); c.ContextFlags = CONTEXT_CONTROL;
        if (GetThreadContext(threads[k], &c) && c.Rip >= site && c.Rip < site + n) busy = 1;
    }
    int ok = 0;
    if (!busy) {
        DWORD old;
        if (VirtualProtect((void *)site, n, PAGE_EXECUTE_READWRITE, &old)) {
            memcpy((void *)site, bytes, n);
            VirtualProtect((void *)site, n, old, &old);
            FlushInstructionCache(GetCurrentProcess(), (void *)site, n);
            ok = 1;
        }
    }
    for (int k = 0; k < nt; k++) { ResumeThread(threads[k]); CloseHandle(threads[k]); }
    return ok ? 1 : (busy ? -1 : 0);
}

static int install_hook(uintptr_t base, size_t size) {
    static const uint8_t msk[sizeof k_hook_pat] = {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1};
    int n; uintptr_t site = find_pattern(base, size, k_hook_pat, msk, sizeof k_hook_pat, &n);
    if (!site || n != 1) { mlog("hook signature: %d hits (need exactly 1) - game build not supported", n); return 0; }
    if (!build_trampoline(site)) { mlog("trampoline alloc failed"); return 0; }
    uint8_t patch[HOOK_LEN];
    patch[0] = 0xFF; patch[1] = 0x25; memset(patch + 2, 0, 4);           // jmp [rip+0]
    { uint64_t t = (uint64_t)(uintptr_t)g_tramp; memcpy(patch + 6, &t, 8); }
    memset(patch + 14, 0x90, HOOK_LEN - 14);
    memcpy(g_hook_orig, (void *)site, HOOK_LEN);
    int r = 0;
    for (int tries = 0; tries < 50 && r <= 0; tries++) { r = patch_site(site, patch, HOOK_LEN); if (r < 0) Sleep(5); }
    if (r != 1) { mlog("patching failed (%d)", r); return 0; }
    g_hook_site = site;
    mlog("hook installed at exe+0x%llx, trampoline %p", (unsigned long long)(site - base), g_tramp);
    return 1;
}

static void remove_hook(void) {
    if (!g_hook_site) return;
    int r = 0;
    for (int tries = 0; tries < 50 && r <= 0; tries++) { r = patch_site(g_hook_site, g_hook_orig, HOOK_LEN); if (r < 0) Sleep(5); }
    mlog("hook removed (%d)", r);
    g_hook_site = 0;
    // The trampoline page is leaked on purpose: a game thread could still be inside it.
}

// --- main loop ----------------------------------------------------------------------------
typedef struct { float x, y, z; } V3;

static HWND g_hwnd = NULL;
static BOOL CALLBACK find_main_window(HWND h, LPARAM lp) {
    DWORD pid; GetWindowThreadProcessId(h, &pid);
    if (pid == (DWORD)lp && IsWindowVisible(h) && !GetWindow(h, GW_OWNER)) { g_hwnd = h; return FALSE; }
    return TRUE;
}

static DWORD WINAPI worker(LPVOID arg) {
    (void)arg;
    Sleep(1500);
    load_config();
    HMODULE exe = GetModuleHandleW(NULL);
    MODULEINFO mi = {0};
    GetModuleInformation(GetCurrentProcess(), exe, &mi, sizeof mi);
    uintptr_t base = (uintptr_t)exe;
    mlog("sinking_star_pan attached: exe base 0x%llx size 0x%lx | mode=%s gain=%.2f smoothing=%.2f button=%s invert=(%d,%d) sens=%.2f modes=0x%x",
         (unsigned long long)base, mi.SizeOfImage, cfg_mode_move ? "move" : "drag", cfg_gain, cfg_smooth,
         cfg_button == VK_RBUTTON ? "right" : cfg_button == VK_MBUTTON ? "middle" : "left",
         cfg_invert_x, cfg_invert_y, cfg_sensitivity, cfg_modes);
    uintptr_t g = find_state_global(base, mi.SizeOfImage);
    if (!g) { mlog("could not find the game state pointer; giving up"); return 0; }
    mlog("state global at exe+0x%llx", (unsigned long long)(g - base));
    if (!install_hook(base, mi.SizeOfImage)) return 0;

    float off_x = 0, off_y = 0;         // accumulated pan, world units
    int dragging = 0; POINT last = {0}, origin = {0}; int was_pannable = 0;
    uint8_t last_mode = 0xFF; DWORD last_log = 0; uintptr_t last_em = 0;
    while (g_running) {
        Sleep(8);
        if (GetFileAttributesA(g_unload_path) != INVALID_FILE_ATTRIBUTES) { DeleteFileA(g_unload_path); mlog("unload requested"); break; }
        if (!readable((void *)g, 8)) continue;
        uintptr_t em = *(uintptr_t *)g;
        if (!em || !readable((void *)em, STATE_SIZE)) continue;
        if (em != last_em) { off_x = off_y = 0; last_em = em; }
        g_state = em;
        { char flag[MAX_PATH]; snprintf(flag, MAX_PATH, "%strace.flag", g_dir);
          if (cfg_debug && GetFileAttributesA(flag) != INVALID_FILE_ATTRIBUTES) { DeleteFileA(flag); dump_trace(); } }

        uint8_t mode = *(uint8_t *)(em + OFF_VIEW_MODE);
        float blend = *(float *)(em + OFF_BLEND);
        if (!(blend >= 0.0f)) blend = 0.0f;
        if (blend > 1.0f) blend = 1.0f;
        int pannable = (cfg_modes >> mode) & 1 && mode != 0;
        V3 cam = *(V3 *)(em + OFF_CAM_POS);
        float fov = *(float *)(em + OFF_CAM_FOV);
        float floor_z = *(float *)(em + OFF_FOLLOW + 8);

        // Window + mouse
        if (!g_hwnd || !IsWindow(g_hwnd)) { g_hwnd = NULL; EnumWindows(find_main_window, (LPARAM)GetCurrentProcessId()); }
        int focused = g_hwnd && GetForegroundWindow() == g_hwnd;
        RECT rc = {0}; if (g_hwnd) GetClientRect(g_hwnd, &rc);
        int client_h = rc.bottom - rc.top; if (client_h < 100) client_h = 1080;
        POINT cur; GetCursorPos(&cur);
        int down = (GetAsyncKeyState(cfg_button) & 0x8000) != 0;

        float height = cam.z - floor_z; if (!(height > 1.0f)) height = cam.z; if (!(height > 1.0f)) height = 80.0f;
        float wpp = 2.0f * tanf(fov * 0.5f) * height / (float)client_h;   // world units per pixel
        if (cfg_mode_move) {
            // The view follows the mouse from where the cursor was when the view opened (the cursor is
            // warped to the window centre at that moment, so the full range is available in every
            // direction): mouse right -> camera moves right (+x), mouse down -> south (-y).
            if (pannable && focused) {
                if (!was_pannable) {
                    POINT c; GetCursorPos(&c);
                    if (cfg_recenter && g_hwnd) {
                        c.x = (rc.left + rc.right) / 2; c.y = (rc.top + rc.bottom) / 2;
                        ClientToScreen(g_hwnd, &c); SetCursorPos(c.x, c.y); cur = c;
                    }
                    origin = c; off_x = off_y = 0;
                    if (cfg_debug) mlog("view opened: origin (%ld,%ld)", origin.x, origin.y);
                } else {
                    off_x = (float)(cur.x - origin.x) * wpp * cfg_gain; off_y = -(float)(cur.y - origin.y) * wpp * cfg_gain;
                    if (cfg_invert_x) off_x = -off_x;
                    if (cfg_invert_y) off_y = -off_y;
                }
            }
        } else if (pannable && focused && down) {
            if (!dragging) { dragging = 1; last = cur; if (cfg_debug) mlog("drag start mode=%u at (%ld,%ld)", mode, cur.x, cur.y); }
            else {
                float dx = (float)(cur.x - last.x) * wpp * cfg_sensitivity, dy = (float)(cur.y - last.y) * wpp * cfg_sensitivity;
                // content follows the cursor: drag right -> camera moves left (-x); drag down -> camera moves north (+y)
                off_x += cfg_invert_x ? dx : -dx;
                off_y += cfg_invert_y ? -dy : dy;
                last = cur;
            }
        } else if (dragging) { dragging = 0; if (cfg_debug) mlog("drag end offset=(%.2f, %.2f)", off_x, off_y); }
        { float m = sqrtf(off_x * off_x + off_y * off_y);
          if (m > cfg_max_offset) { off_x *= cfg_max_offset / m; off_y *= cfg_max_offset / m; } }

        // Leaving the overhead view: ease the offset out with the game's own transition, then forget it.
        float scale = pannable ? 1.0f : blend;
        if (!pannable && blend <= 0.001f) { off_x = off_y = 0; g_snap = 1; } else g_snap = 0;
        g_target_x = off_x * scale; g_target_y = off_y * scale;
        was_pannable = pannable && focused;

        DWORD now = GetTickCount();
        if (cfg_debug && (mode != last_mode || now - last_log > 2000)) {
            float tx = *(float *)(em + OFF_TARGET_X), ty = *(float *)(em + OFF_TARGET_X + 4);
            float *b = (float *)(em + OFF_BOUNDS);
            mlog("mode=%u blend=%.3f target=(%.2f %.2f) cam=(%.2f %.2f %.2f) fov=%.3f bounds=(%.1f %.1f %.1f %.1f) off=(%.2f %.2f) hits=%u client_h=%d focused=%d drag=%d",
                 mode, blend, tx, ty, cam.x, cam.y, cam.z, fov, b[0], b[1], b[2], b[3], off_x, off_y, *g_hits, client_h, focused, dragging);
            last_mode = mode; last_log = now;
        }
    }
    g_target_x = g_target_y = 0; g_snap = 1;
    remove_hook();
    Sleep(200);   // nothing can enter pan_tick any more; let any thread already inside it finish
    *g_off_x = 0; *g_off_y = 0;
    mlog("detached");
    if (!g_running) return 0;
    FreeLibraryAndExitThread(g_self, 0);
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = inst;
        DisableThreadLibraryCalls(inst);
        GetModuleFileNameA(inst, g_dir, MAX_PATH);
        char *s = strrchr(g_dir, '\\'); if (s) s[1] = 0;
        snprintf(g_log_path, MAX_PATH, "%ssinking_star_pan.log", g_dir);
        snprintf(g_ini_path, MAX_PATH, "%sconfig.ini", g_dir);
        snprintf(g_unload_path, MAX_PATH, "%sunload.flag", g_dir);
        CreateThread(NULL, 0, worker, NULL, 0, NULL);
    } else if (reason == DLL_PROCESS_DETACH) {
        g_running = 0;
    }
    return TRUE;
}
