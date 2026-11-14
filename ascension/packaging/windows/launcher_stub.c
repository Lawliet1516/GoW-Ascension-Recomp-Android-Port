/* "God of War Ascension Launcher.exe": starts the launcher with the Python
 * runtime shipped beside it, so players need no Python of their own.
 *
 *   <here>\runtime\python\pythonw.exe <here>\ascension-recomp\ascension\launcher\ascension_launcher.py [args]
 *
 * in <here>\ascension-recomp\ascension. Arguments are passed on (--play).
 * Build: clang -O2 launcher_stub.c launcher_stub.res -o "God of War Ascension Launcher.exe"
 *        -Wl,/subsystem:windows -luser32 -lshell32
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <wchar.h>

static void fail(const wchar_t* what, const wchar_t* path)
{
    wchar_t msg[2048];
    swprintf(msg, 2048, L"%ls\n\n%ls\n\nExtract the whole zip and start the launcher from its folder.", what, path);
    MessageBoxW(NULL, msg, L"God of War: Ascension", MB_ICONERROR | MB_OK);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmd, int show)
{
    (void)inst; (void)prev; (void)show;
    wchar_t here[MAX_PATH], py[MAX_PATH + 64], script[MAX_PATH + 96], cwd[MAX_PATH + 64];
    DWORD n = GetModuleFileNameW(NULL, here, MAX_PATH);
    if (!n || n >= MAX_PATH) return 1;
    wchar_t* slash = wcsrchr(here, L'\\');
    if (slash) *slash = 0;

    swprintf(py, MAX_PATH + 64, L"%ls\\runtime\\python\\pythonw.exe", here);
    swprintf(script, MAX_PATH + 96, L"%ls\\ascension-recomp\\ascension\\launcher\\ascension_launcher.py", here);
    swprintf(cwd, MAX_PATH + 64, L"%ls\\ascension-recomp\\ascension", here);
    if (GetFileAttributesW(py) == INVALID_FILE_ATTRIBUTES) { fail(L"The launcher's Python runtime is missing:", py); return 1; }
    if (GetFileAttributesW(script) == INVALID_FILE_ATTRIBUTES) { fail(L"The launcher is missing:", script); return 1; }

    static wchar_t line[32768];
    swprintf(line, 32768, L"\"%ls\" \"%ls\"%ls%ls", py, script, (cmd && *cmd) ? L" " : L"", cmd ? cmd : L"");

    STARTUPINFOW si = { sizeof si };
    PROCESS_INFORMATION pi;
    if (!CreateProcessW(py, line, NULL, NULL, FALSE, 0, NULL, cwd, &si, &pi)) {
        fail(L"Could not start the launcher:", py);
        return 1;
    }
    /* --play: wait, so the exit code is the game's (shortcuts, Steam). */
    DWORD code = 0;
    if (cmd && wcsstr(cmd, L"--play")) {
        WaitForSingleObject(pi.hProcess, INFINITE);
        GetExitCodeProcess(pi.hProcess, &code);
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return (int)code;
}
