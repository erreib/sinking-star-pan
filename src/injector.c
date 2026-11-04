// sinking_star_pan injector: waits for sinking_star.exe and loads the mod dll into it.
// Build: x86_64-w64-mingw32-gcc -O2 -o SinkingStarPanInjector.exe injector.c
#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

static DWORD find_process(const wchar_t *exe) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe = { .dwSize = sizeof(pe) };
    DWORD pid = 0;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, exe) == 0) { pid = pe.th32ProcessID; break; }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return pid;
}

static int already_loaded(DWORD pid, const wchar_t *dll_name) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    MODULEENTRY32W me = { .dwSize = sizeof(me) };
    int found = 0;
    if (Module32FirstW(snap, &me)) {
        do {
            if (_wcsicmp(me.szModule, dll_name) == 0) { found = 1; break; }
        } while (Module32NextW(snap, &me));
    }
    CloseHandle(snap);
    return found;
}

static int inject(DWORD pid, const wchar_t *dll_path) {
    HANDLE proc = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
                              PROCESS_VM_WRITE | PROCESS_VM_READ, FALSE, pid);
    if (!proc) { printf("OpenProcess failed (%lu)\n", GetLastError()); return 0; }
    SIZE_T bytes = (wcslen(dll_path) + 1) * sizeof(wchar_t);
    void *remote = VirtualAllocEx(proc, NULL, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    int ok = 0;
    if (!remote) { printf("VirtualAllocEx failed (%lu)\n", GetLastError()); }
    else if (!WriteProcessMemory(proc, remote, dll_path, bytes, NULL)) { printf("WriteProcessMemory failed (%lu)\n", GetLastError()); }
    else {
        LPTHREAD_START_ROUTINE ll = (LPTHREAD_START_ROUTINE)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
        HANDLE th = CreateRemoteThread(proc, NULL, 0, ll, remote, 0, NULL);
        if (!th) { printf("CreateRemoteThread failed (%lu)\n", GetLastError()); }
        else {
            WaitForSingleObject(th, 20000);
            DWORD code = 0; GetExitCodeThread(th, &code); CloseHandle(th);
            ok = code != 0;
            printf("LoadLibraryW -> 0x%08lX %s\n", code, ok ? "(ok)" : "(failed: check the dll and its log)");
        }
    }
    if (remote) VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
    CloseHandle(proc);
    return ok;
}

int main(int argc, char **argv) {
    const wchar_t *exe = L"sinking_star.exe";
    static wchar_t dll_name_buf[MAX_PATH] = L"sinking_star_pan.dll";
    const wchar_t *dll_name = dll_name_buf;
    int timeout_s = 180, launch = 1;
    DWORD appid = 499170;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--no-launch")) launch = 0;
        else if (!strcmp(argv[i], "--timeout") && i + 1 < argc) timeout_s = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--dll") && i + 1 < argc) { mbstowcs(dll_name_buf, argv[++i], MAX_PATH - 1); }
        else if (!strcmp(argv[i], "--help")) { printf("usage: SinkingStarPanInjector [--no-launch] [--timeout sec] [--dll name.dll]\n"); return 0; }
    }
    wchar_t self[MAX_PATH] = {0}, dll_path[MAX_PATH] = {0};
    GetModuleFileNameW(NULL, self, MAX_PATH);
    wchar_t *slash = wcsrchr(self, L'\\');
    if (slash) slash[1] = 0;
    swprintf(dll_path, MAX_PATH, L"%ls%ls", self, dll_name);
    if (GetFileAttributesW(dll_path) == INVALID_FILE_ATTRIBUTES) { printf("dll not found: %ls\n", dll_path); return 2; }
    printf("dll: %ls\n", dll_path);

    DWORD pid = find_process(exe);
    if (!pid && launch) {
        wchar_t url[64]; swprintf(url, 64, L"steam://rungameid/%lu", appid);
        printf("game not running, opening %ls\n", url);
        ShellExecuteW(NULL, L"open", url, NULL, NULL, SW_SHOWNORMAL);
    }
    DWORD deadline = GetTickCount() + (DWORD)timeout_s * 1000;
    while (!pid && GetTickCount() < deadline) { Sleep(200); pid = find_process(exe); }
    if (!pid) { printf("gave up waiting for %ls\n", exe); return 3; }
    printf("found %ls pid=%lu\n", exe, pid);
    if (already_loaded(pid, dll_name)) { printf("mod already loaded in that process\n"); return 0; }
    // Give the engine a moment so the dll's thread does not race process start-up.
    Sleep(1500);
    if (!inject(pid, dll_path)) { printf("injection failed\n"); return 4; }
    printf("injected; log: sinking_star_pan.log next to the dll\n");
    return 0;
}
