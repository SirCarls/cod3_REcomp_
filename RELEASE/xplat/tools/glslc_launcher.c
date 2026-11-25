/* glslc_launcher: the glslc of xplat_find_glslc (cmake/XplatNglVk.cmake) on a Windows build PC that has glslang but
 * no glslc. [rexglue-native-platform-fix] 2026-11-23
 *
 * Runs the command written in <this program's name>.args beside it (UTF-8, one argument per line: XplatNglVk.cmake
 * writes Python, -I, glslc_glslang.py, --glslang and glslang there) with this program's own arguments after it, and
 * exits with that command's exit code. Wide functions only, so every folder involved may have any name.
 *
 * It replaces a glslc.cmd: cmd reads a batch file in the console's code page, so a Python or glslang in a folder
 * outside ASCII needed "chcp 65001" first, which changes the code page of the build's whole console, and still failed
 * where cmd goes on reading the file in the old code page (Wine's cmd, the 11-23 test) or where there is no console.
 * C that also builds as C++ (XplatNglVk.cmake compiles it with whichever of the two the project has). */
#if defined(_WIN32)
#if !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS /* _wfopen (MSVC's runtime calls it unsafe; _wfopen_s adds nothing here) */
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

typedef struct {
    wchar_t *p;
    size_t n, cap;
} wbuf;

static int put(wbuf *b, const wchar_t *s, size_t n)
{
    if (b->n + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 256;
        wchar_t *p;
        while (cap < b->n + n + 1)
            cap *= 2;
        p = (wchar_t *)realloc(b->p, cap * sizeof *p);
        if (!p)
            return -1;
        b->p = p;
        b->cap = cap;
    }
    memcpy(b->p + b->n, s, n * sizeof *s);
    b->n += n;
    b->p[b->n] = 0;
    return 0;
}

static int put_repeat(wbuf *b, wchar_t c, size_t count)
{
    size_t i;
    for (i = 0; i < count; i++)
        if (put(b, &c, 1))
            return -1;
    return 0;
}

/* One argument, quoted so that CommandLineToArgvW and the C runtimes read it back as it is. */
static int put_arg(wbuf *b, const wchar_t *a)
{
    size_t i, slashes = 0;
    if (b->n && put(b, L" ", 1))
        return -1;
    if (*a && !wcspbrk(a, L" \t\n\v\""))
        return put(b, a, wcslen(a));
    if (put(b, L"\"", 1))
        return -1;
    for (i = 0;; i++) {
        if (a[i] == L'\\') {
            slashes++;
            continue;
        }
        if (a[i] == 0) { /* the backslashes before the closing quote are doubled */
            if (put_repeat(b, L'\\', slashes * 2))
                return -1;
            break;
        }
        if (a[i] == L'"') { /* and those before a quote, which itself gets one */
            if (put_repeat(b, L'\\', slashes * 2 + 1) || put(b, L"\"", 1))
                return -1;
        } else if (put_repeat(b, L'\\', slashes) || put(b, a + i, 1)) {
            return -1;
        }
        slashes = 0;
    }
    return put(b, L"\"", 1);
}

static wchar_t *widen(const char *s, int n)
{
    int count = MultiByteToWideChar(CP_UTF8, 0, s, n, NULL, 0);
    wchar_t *w = (wchar_t *)malloc(((size_t)(count > 0 ? count : 0) + 1) * sizeof *w);
    if (!w)
        return NULL;
    if (count > 0)
        MultiByteToWideChar(CP_UTF8, 0, s, n, w, count);
    w[count > 0 ? count : 0] = 0;
    return w;
}

static void say(const char *what, const wchar_t *name, DWORD error)
{
    char text[4096] = "";
    if (name)
        WideCharToMultiByte(CP_UTF8, 0, name, -1, text, (int)sizeof text - 1, NULL, NULL);
    fprintf(stderr, "glslc launcher: %s %s (Windows error %lu)\n", what, text, (unsigned long)error);
}

/* A standard handle the child can inherit (NULL when there is none). */
static HANDLE inheritable(DWORD which)
{
    HANDLE h = GetStdHandle(which), copy = NULL;
    if (h == NULL || h == INVALID_HANDLE_VALUE)
        return NULL;
    if (!DuplicateHandle(GetCurrentProcess(), h, GetCurrentProcess(), &copy, 0, TRUE, DUPLICATE_SAME_ACCESS))
        return h;
    return copy;
}

static BOOL WINAPI on_ctrl(DWORD type)
{
    (void)type;
    return TRUE; /* Ctrl+C reaches the command too (same console); this waits for it to end */
}

int main(void)
{
    wchar_t *self = NULL, *args_path, *dot, *sep, *app = NULL, **argv;
    DWORD cap = 512, len = 0, code = 1;
    size_t stem, size = 0, room = 0, start, i;
    char *text = NULL;
    FILE *f;
    int argc = 0, k;
    wbuf cmd = {NULL, 0, 0};
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;

    for (;;) { /* this program's path, however long */
        wchar_t *p = (wchar_t *)realloc(self, cap * sizeof *self);
        if (!p)
            return 127;
        self = p;
        len = GetModuleFileNameW(NULL, self, cap);
        if (len == 0) {
            say("can't find its own path", NULL, GetLastError());
            return 127;
        }
        if (len < cap)
            break;
        cap *= 2;
    }
    dot = wcsrchr(self, L'.');
    sep = wcsrchr(self, L'\\');
    stem = (dot && (!sep || dot > sep)) ? (size_t)(dot - self) : (size_t)len;
    args_path = (wchar_t *)malloc((stem + 6) * sizeof *args_path);
    if (!args_path)
        return 127;
    wmemcpy(args_path, self, stem);
    wmemcpy(args_path + stem, L".args", 6); /* with its terminating 0 */

    f = _wfopen(args_path, L"rb");
    if (!f) {
        say("can't read", args_path, GetLastError());
        return 127;
    }
    for (;;) {
        size_t got;
        if (size + 4096 + 1 > room) {
            char *p = (char *)realloc(text, room = (room ? room * 2 : 8192));
            if (!p)
                return 127;
            text = p;
        }
        got = fread(text + size, 1, room - size - 1, f);
        size += got;
        if (got == 0)
            break;
    }
    fclose(f);
    if (!text)
        return 127;
    text[size] = 0;

    start = (size >= 3 && memcmp(text, "\xEF\xBB\xBF", 3) == 0) ? 3 : 0; /* a byte order mark */
    for (i = start; i < size; i++) { /* one argument a line; a last line without "\n" counts too */
        if (text[i] == '\n' || i + 1 == size) {
            size_t end = text[i] == '\n' ? i : i + 1;
            wchar_t *arg;
            if (end > start && text[end - 1] == '\r')
                end--;
            arg = widen(text + start, (int)(end - start));
            if (!arg)
                return 127;
            if (!app) {
                wchar_t *c;
                for (c = arg; *c; c++)
                    if (*c == L'/')
                        *c = L'\\';
                app = arg; /* the program itself, started by its path (no search) */
            }
            if (put_arg(&cmd, arg))
                return 127;
            if (arg != app)
                free(arg);
            start = i + 1;
        }
    }
    if (!app || !*app) {
        say("has no command in", args_path, 0);
        return 127;
    }
    argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (k = 1; argv && k < argc; k++)
        if (put_arg(&cmd, argv[k]))
            return 127;

    ZeroMemory(&si, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = inheritable(STD_INPUT_HANDLE);
    si.hStdOutput = inheritable(STD_OUTPUT_HANDLE);
    si.hStdError = inheritable(STD_ERROR_HANDLE);
    SetConsoleCtrlHandler(on_ctrl, TRUE);
    if (!CreateProcessW(app, cmd.p, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
        say("can't start", app, GetLastError());
        return 127;
    }
    CloseHandle(pi.hThread);
    WaitForSingleObject(pi.hProcess, INFINITE);
    if (!GetExitCodeProcess(pi.hProcess, &code))
        code = 1;
    CloseHandle(pi.hProcess);
    return (int)code;
}

#else /* a Windows program: XplatNglVk.cmake builds it only on a Windows build PC */
#include <stdio.h>

int main(void)
{
    fputs("glslc launcher: a Windows program (elsewhere xplat_find_glslc writes a shell script)\n", stderr);
    return 127;
}
#endif
