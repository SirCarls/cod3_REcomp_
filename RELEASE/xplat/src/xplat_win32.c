/* xplat on Windows: every call through the wide (UTF-16) functions (see include/xplat/xplat.h). */
#if defined(_WIN32)

/* _wfopen, not _wfopen_s: _wfopen_s opens without sharing (another program, or this one, couldn't open the file
 * at the same time), which isn't fopen's behaviour on any other system. The MSVC CRT calls _wfopen "unsafe" only
 * because it isn't _s. */
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>  /* CommandLineToArgvW */
#include <shlobj.h>    /* SHGetKnownFolderPath */
#include <timeapi.h>   /* timeBeginPeriod */

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "xplat_internal.h"

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002 /* Windows 10 1803 and later; older SDKs lack the name */
#endif

/* Paths at least this long (in UTF-16 units, without the NUL) are handed to Windows in the \\?\ form. 248, not 260:
 * CreateDirectoryW's limit for a folder (room for an 8.3 file name inside it). */
#define LONG_PATH_FROM 248

/* ---- errors and text ------------------------------------------------------------------------------------------ */

static int errno_from(DWORD error)
{
    switch (error) {
    case ERROR_FILE_NOT_FOUND: case ERROR_PATH_NOT_FOUND: case ERROR_INVALID_DRIVE: case ERROR_BAD_NETPATH:
    case ERROR_BAD_NET_NAME: case ERROR_NO_MORE_FILES: case ERROR_ENVVAR_NOT_FOUND: case ERROR_MOD_NOT_FOUND:
        return ENOENT;
    case ERROR_ACCESS_DENIED: case ERROR_SHARING_VIOLATION: case ERROR_LOCK_VIOLATION: case ERROR_WRITE_PROTECT:
    case ERROR_NETWORK_ACCESS_DENIED: case ERROR_CURRENT_DIRECTORY:
        return EACCES;
    case ERROR_ALREADY_EXISTS: case ERROR_FILE_EXISTS:
        return EEXIST;
    case ERROR_DIR_NOT_EMPTY:
        return ENOTEMPTY;
    case ERROR_FILENAME_EXCED_RANGE: case ERROR_BUFFER_OVERFLOW: case ERROR_INSUFFICIENT_BUFFER:
        return ENAMETOOLONG;
    case ERROR_DISK_FULL: case ERROR_HANDLE_DISK_FULL:
        return ENOSPC;
    case ERROR_NOT_SAME_DEVICE:
        return EXDEV;
    case ERROR_DIRECTORY:
        return ENOTDIR;
    case ERROR_NOT_ENOUGH_MEMORY: case ERROR_OUTOFMEMORY:
        return ENOMEM;
    case ERROR_INVALID_NAME: case ERROR_BAD_PATHNAME: case ERROR_INVALID_PARAMETER: case ERROR_NO_UNICODE_TRANSLATION:
        return EINVAL;
    default:
        return EIO;
    }
}

static int fail_with(DWORD error)
{
    errno = errno_from(error);
    return -1;
}

/* UTF-8 -> a new UTF-16 string (free it), NULL with errno for bad UTF-8 or no memory. */
static wchar_t *wide_from(const char *text)
{
    int n;
    wchar_t *w;

    if (!text) {
        errno = EINVAL;
        return NULL;
    }
    n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, NULL, 0);
    if (n <= 0) {
        errno = EINVAL; /* not UTF-8 */
        return NULL;
    }
    w = (wchar_t *)malloc((size_t)n * sizeof(wchar_t));
    if (!w) {
        errno = ENOMEM;
        return NULL;
    }
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, w, n);
    return w;
}

/* UTF-16 -> UTF-8 into buf (NUL-ended). */
static int utf8_into(const wchar_t *w, int wlen, char *buf, size_t size)
{
    int n;

    if (!buf || size == 0) {
        errno = EINVAL;
        return -1;
    }
    if (wlen == 0) {
        buf[0] = '\0';
        return 0;
    }
    n = WideCharToMultiByte(CP_UTF8, 0, w, wlen, NULL, 0, NULL, NULL);
    if (n <= 0)
        return fail_with(GetLastError());
    if ((size_t)n + (wlen < 0 ? 0u : 1u) > size) {
        buf[0] = '\0';
        errno = ENAMETOOLONG;
        return -1;
    }
    WideCharToMultiByte(CP_UTF8, 0, w, wlen, buf, n, NULL, NULL);
    if (wlen >= 0)
        buf[n] = '\0';
    return 0;
}

/* A UTF-8 path as Windows should be given it (free it). Short paths go as they are (Windows itself turns "/" into
 * "\" and resolves "." and ".." for them). From LONG_PATH_FROM characters on, the path is first made full and
 * normal by GetFullPathNameW (the \\?\ form turns all of that off) and then written \\?\C:\... or, for
 * \\server\share\..., \\?\UNC\server\share\..., which Windows 10 1607+ and every Windows with the Unicode functions
 * open past 260 characters. A path already in a \\?\ or \\.\ form is left alone. */
wchar_t *xplat__system_path(const char *path)
{
    wchar_t *w = wide_from(path), *full, *out;
    size_t len;
    DWORD n, capacity;

    if (!w)
        return NULL;
    len = wcslen(w);
    if (len < LONG_PATH_FROM || (w[0] == L'\\' && w[1] == L'\\' && (w[2] == L'?' || w[2] == L'.') && w[3] == L'\\'))
        return w;
    n = GetFullPathNameW(w, 0, NULL, NULL);
    if (n == 0) {
        free(w);
        fail_with(GetLastError());
        return NULL;
    }
    capacity = n + 8; /* room for the \\?\UNC\ prefix too */
    full = (wchar_t *)malloc((size_t)capacity * sizeof(wchar_t));
    out = (wchar_t *)malloc((size_t)capacity * sizeof(wchar_t));
    if (!full || !out) {
        free(w);
        free(full);
        free(out);
        errno = ENOMEM;
        return NULL;
    }
    n = GetFullPathNameW(w, capacity - 8, full, NULL); /* now the length without the NUL */
    free(w);
    if (n == 0 || n >= capacity - 8) { /* failed, or the working folder changed in between */
        DWORD error = n == 0 ? GetLastError() : ERROR_FILENAME_EXCED_RANGE;

        free(full);
        free(out);
        fail_with(error);
        return NULL;
    }
    if (full[0] == L'\\' && full[1] == L'\\' && (full[2] == L'?' || full[2] == L'.') && full[3] == L'\\') {
        free(out); /* already a \\?\ or \\.\ path (a working folder given that way) */
        return full;
    }
    if (full[0] == L'\\' && full[1] == L'\\') { /* \\server\share\... -> \\?\UNC\server\share\... */
        memcpy(out, L"\\\\?\\UNC\\", 8 * sizeof(wchar_t));
        memcpy(out + 8, full + 2, ((size_t)n - 2 + 1) * sizeof(wchar_t)); /* n: full's length, without the NUL */
    } else {
        memcpy(out, L"\\\\?\\", 4 * sizeof(wchar_t));
        memcpy(out + 4, full, ((size_t)n + 1) * sizeof(wchar_t));
    }
    free(full);
    return out;
}

/* ---- files and folders ---------------------------------------------------------------------------------------- */

FILE *xplat_fopen(const char *path, const char *mode)
{
    wchar_t *wpath = xplat__system_path(path), *wmode = wide_from(mode);
    FILE *f = NULL;

    if (wpath && wmode)
        f = _wfopen(wpath, wmode); /* sets errno itself */
    free(wpath);
    free(wmode);
    return f;
}

static int64_t ns_from_filetime(FILETIME t)
{
    const int64_t ticks = (int64_t)(((uint64_t)t.dwHighDateTime << 32) | t.dwLowDateTime); /* 100 ns since 1601 */
    return (ticks - 116444736000000000LL) * 100;
}

int xplat_stat(const char *path, xplat_stat_t *out)
{
    wchar_t *w = xplat__system_path(path);
    HANDLE h;
    BY_HANDLE_FILE_INFORMATION info;
    WIN32_FILE_ATTRIBUTE_DATA data;
    int ok = 0;

    if (!w)
        return -1;
    memset(out, 0, sizeof *out);
    /* Through a handle, so a link reports what it points to (as POSIX stat does). FILE_FLAG_BACKUP_SEMANTICS opens
     * folders too; no access rights asked, so files another program holds open are fine. */
    h = CreateFileW(w, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                    FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        if (GetFileInformationByHandle(h, &info)) {
            out->is_dir = (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            out->is_file = !out->is_dir && !(info.dwFileAttributes & FILE_ATTRIBUTE_DEVICE);
            out->size = out->is_dir ? 0 : ((uint64_t)info.nFileSizeHigh << 32) | info.nFileSizeLow;
            out->mtime_ns = ns_from_filetime(info.ftLastWriteTime);
            ok = 1;
        }
        CloseHandle(h);
    }
    if (!ok && GetFileAttributesExW(w, GetFileExInfoStandard, &data)) { /* e.g. a file opened without sharing */
        out->is_dir = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        out->is_file = !out->is_dir;
        out->size = out->is_dir ? 0 : ((uint64_t)data.nFileSizeHigh << 32) | data.nFileSizeLow;
        out->mtime_ns = ns_from_filetime(data.ftLastWriteTime);
        ok = 1;
    }
    if (!ok) {
        DWORD error = GetLastError();

        free(w);
        return fail_with(error);
    }
    free(w);
    return 0;
}

int xplat_mkdir(const char *path)
{
    wchar_t *w = xplat__system_path(path);
    DWORD error;
    DWORD attributes;

    if (!w)
        return -1;
    if (CreateDirectoryW(w, NULL)) {
        free(w);
        return 0;
    }
    error = GetLastError();
    attributes = GetFileAttributesW(w);
    free(w);
    if (error == ERROR_ALREADY_EXISTS || (attributes != INVALID_FILE_ATTRIBUTES)) {
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY))
            return 0;
        errno = EEXIST;
        return -1;
    }
    return fail_with(error);
}

int xplat_remove(const char *path)
{
    wchar_t *w = xplat__system_path(path);
    DWORD attributes;
    BOOL done;

    if (!w)
        return -1;
    attributes = GetFileAttributesW(w);
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        DWORD error = GetLastError();

        free(w);
        return fail_with(error);
    }
    done = (attributes & FILE_ATTRIBUTE_DIRECTORY) ? RemoveDirectoryW(w) : DeleteFileW(w);
    if (!done) {
        DWORD error = GetLastError();

        free(w);
        return fail_with(error);
    }
    free(w);
    return 0;
}

int xplat_rename_replace(const char *from, const char *to)
{
    wchar_t *wfrom = xplat__system_path(from), *wto = xplat__system_path(to);
    DWORD error = ERROR_SUCCESS;
    int tries;

    if (!wfrom || !wto) {
        free(wfrom);
        free(wto);
        return -1;
    }
    /* A file another program has open (an antivirus scanning the new file, the indexer) refuses the move for a
     * moment: a few tries over ~0.2 s, then the error. Not across drives (as POSIX rename: EXDEV). */
    for (tries = 0; tries < 10; tries++) {
        if (MoveFileExW(wfrom, wto, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            error = ERROR_SUCCESS;
            break;
        }
        error = GetLastError();
        if (error != ERROR_ACCESS_DENIED && error != ERROR_SHARING_VIOLATION && error != ERROR_LOCK_VIOLATION)
            break;
        Sleep(20);
    }
    free(wfrom);
    free(wto);
    return error == ERROR_SUCCESS ? 0 : fail_with(error);
}

struct xplat_dir {
    HANDLE find;
    WIN32_FIND_DATAW data;
    int pending; /* data holds an entry not handed out yet */
    char name[MAX_PATH * 4 + 4];
};

xplat_dir *xplat_opendir(const char *path)
{
    char pattern[32768];
    wchar_t *w;
    xplat_dir *d;

    if (xplat__join(pattern, sizeof pattern, path, "*"))
        return NULL;
    w = xplat__system_path(pattern);
    if (!w)
        return NULL;
    d = (xplat_dir *)calloc(1, sizeof *d);
    if (!d) {
        free(w);
        errno = ENOMEM;
        return NULL;
    }
    d->find = FindFirstFileExW(w, FindExInfoBasic, &d->data, FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH);
    free(w);
    if (d->find == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();

        free(d);
        if (error == ERROR_FILE_NOT_FOUND) { /* an empty drive root: no entries at all, still a folder */
            errno = ENOENT;
        } else {
            errno = errno_from(error);
        }
        return NULL;
    }
    d->pending = 1;
    return d;
}

const char *xplat_readdir(xplat_dir *d, int *is_dir)
{
    for (;;) {
        if (!d->pending) {
            if (!FindNextFileW(d->find, &d->data))
                return NULL;
        }
        d->pending = 0;
        if (wcscmp(d->data.cFileName, L".") == 0 || wcscmp(d->data.cFileName, L"..") == 0)
            continue;
        if (utf8_into(d->data.cFileName, -1, d->name, sizeof d->name) != 0)
            continue; /* cannot happen for a valid name; skip rather than hand out a wrong one */
        if (is_dir)
            *is_dir = (d->data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        return d->name;
    }
}

void xplat_closedir(xplat_dir *d)
{
    if (!d)
        return;
    FindClose(d->find);
    free(d);
}

/* ---- the process ---------------------------------------------------------------------------------------------- */

int xplat_getenv(const char *name, char *buf, size_t size)
{
    wchar_t *wname = wide_from(name), first_try[1024], *value = first_try;
    DWORD n;
    int rc;

    if (!wname)
        return -1;
    SetLastError(ERROR_SUCCESS);
    n = GetEnvironmentVariableW(wname, first_try, (DWORD)(sizeof first_try / sizeof first_try[0]));
    if (n == 0 && GetLastError() == ERROR_ENVVAR_NOT_FOUND) {
        free(wname);
        errno = ENOENT;
        return -1;
    }
    if (n >= sizeof first_try / sizeof first_try[0]) { /* longer than the stack buffer: n is the size it needs */
        value = (wchar_t *)malloc((size_t)n * sizeof(wchar_t));
        if (!value) {
            free(wname);
            errno = ENOMEM;
            return -1;
        }
        n = GetEnvironmentVariableW(wname, value, n);
    }
    free(wname);
    rc = utf8_into(value, (int)n, buf, size);
    if (value != first_try)
        free(value);
    return rc;
}

int xplat_exe_path(char *buf, size_t size)
{
    DWORD capacity = MAX_PATH, n;
    wchar_t *w = NULL;
    int rc;

    for (;;) { /* GetModuleFileNameW cuts the name to the buffer without saying so but by filling it */
        wchar_t *bigger = (wchar_t *)realloc(w, (size_t)capacity * sizeof(wchar_t));

        if (!bigger) {
            free(w);
            errno = ENOMEM;
            return -1;
        }
        w = bigger;
        n = GetModuleFileNameW(NULL, w, capacity);
        if (n == 0) {
            DWORD error = GetLastError();

            free(w);
            return fail_with(error);
        }
        if (n < capacity)
            break;
        if (capacity >= 32768) {
            free(w);
            errno = ENAMETOOLONG;
            return -1;
        }
        capacity *= 2;
    }
    rc = utf8_into(w, (int)n, buf, size);
    free(w);
    return rc;
}

int xplat_utf8_args(int *argc, char ***argv)
{
    static char **made; /* once; kept for the program's life */
    static int made_count;
    int n = 0, i;
    wchar_t **wide;
    size_t total = 0;
    char *text;

    if (made) {
        *argc = made_count;
        *argv = made;
        return 0;
    }
    wide = CommandLineToArgvW(GetCommandLineW(), &n);
    if (!wide)
        return fail_with(GetLastError());
    for (i = 0; i < n; i++)
        total += (size_t)WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, NULL, 0, NULL, NULL);
    made = (char **)malloc(((size_t)n + 1) * sizeof(char *) + total);
    if (!made) {
        LocalFree(wide);
        errno = ENOMEM;
        return -1;
    }
    text = (char *)(made + n + 1);
    for (i = 0; i < n; i++) {
        int len = WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, text, (int)total, NULL, NULL);

        made[i] = text;
        text += len;
        total -= (size_t)len;
    }
    made[n] = NULL; /* as main's argv */
    LocalFree(wide);
    made_count = n;
    *argc = n;
    *argv = made;
    return 0;
}

/* ---- time ----------------------------------------------------------------------------------------------------- */

uint64_t xplat_monotonic_ns(void)
{
    static LARGE_INTEGER frequency; /* constant from boot; reading it twice in a race is harmless */
    LARGE_INTEGER now;
    uint64_t ticks, per_second;

    if (frequency.QuadPart == 0)
        QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&now);
    ticks = (uint64_t)now.QuadPart;
    per_second = (uint64_t)frequency.QuadPart;
    /* without overflow: whole seconds, then the rest */
    return (ticks / per_second) * 1000000000ull + (ticks % per_second) * 1000000000ull / per_second;
}

void xplat_sleep_until_ns(uint64_t deadline_ns)
{
    /* A high-resolution timer for this wait (a few microseconds to make; a frame limiter waits once a frame), so no
     * handle is left behind by threads that end. Windows before 10 1803 has none: Sleep() of the whole milliseconds,
     * which wakes on the next timer tick (1 ms after xplat_request_fine_timer, else up to 15.6 ms). */
    HANDLE timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                          TIMER_MODIFY_STATE | SYNCHRONIZE);
    int rounds;

    for (rounds = 0; rounds < 4; rounds++) {
        const uint64_t now = xplat_monotonic_ns();
        uint64_t left;

        if (now >= deadline_ns)
            break;
        left = deadline_ns - now;
        if (timer) {
            LARGE_INTEGER due;

            if (left < 100000) /* under 0.1 ms: not worth a wait */
                break;
            due.QuadPart = -(LONGLONG)(left / 100); /* relative, 100 ns units */
            if (!SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE) ||
                WaitForSingleObject(timer, INFINITE) != WAIT_OBJECT_0) {
                CloseHandle(timer);
                timer = NULL; /* and on with Sleep */
            }
        } else {
            if (left < 1000000)
                break;
            Sleep((DWORD)(left / 1000000));
        }
    }
    if (timer)
        CloseHandle(timer);
}

int xplat_request_fine_timer(void)
{
    static volatile LONG done = 0, accepted = 0;
    typedef BOOL(WINAPI * set_info_fn)(HANDLE, int, LPVOID, DWORD);
    struct power_throttling_state { /* processthreadsapi.h, declared here for older SDKs */
        ULONG version, control_mask, state_mask;
    } state = {1, 0x4 /* PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION */, 0};
    set_info_fn set_info;

    if (InterlockedCompareExchange(&done, 1, 0) != 0)
        return (int)accepted;
    accepted = timeBeginPeriod(1) == TIMERR_NOERROR;
    /* Windows 11 may drop the 1 ms request while the window seems hidden; this is Microsoft's documented opt-out.
     * Looked up, as Windows 7 and 8 don't have it. 4 = ProcessPowerThrottling. */
    set_info = (set_info_fn)(void (*)(void))GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetProcessInformation");
    if (set_info)
        set_info(GetCurrentProcess(), 4, &state, sizeof state);
    return (int)accepted;
}

/* ---- the player's own folders --------------------------------------------------------------------------------- */

/* FOLDERID_RoamingAppData and FOLDERID_LocalAppData (KnownFolders.h), defined here so no uuid library is needed */
static const GUID kRoamingAppData = {0x3EB685DB, 0x65F9, 0x4CF6, {0xA0, 0x3A, 0xE3, 0xEF, 0x65, 0x72, 0x9F, 0x3D}};
static const GUID kLocalAppData = {0xF1B32785, 0x6FBA, 0x4FCF, {0x9D, 0x55, 0x7B, 0x8E, 0x7F, 0x15, 0x70, 0x91}};

int xplat__user_base(xplat_user_dir_kind kind, char *buf, size_t size)
{
    const int roaming = kind == XPLAT_USER_CONFIG;
    PWSTR folder = NULL;
    int rc;

    /* The shell's known folder (follows a moved or redirected AppData); then the environment's name for it. */
    if (SUCCEEDED(SHGetKnownFolderPath(roaming ? &kRoamingAppData : &kLocalAppData, 0, NULL, &folder)) && folder) {
        rc = utf8_into(folder, -1, buf, size);
        CoTaskMemFree(folder);
        if (rc == 0)
            return 0;
        return -1;
    }
    if (folder)
        CoTaskMemFree(folder);
    return xplat_getenv(roaming ? "APPDATA" : "LOCALAPPDATA", buf, size);
}

#else
typedef int xplat_win32_unused; /* ISO C wants something in every file */
#endif
