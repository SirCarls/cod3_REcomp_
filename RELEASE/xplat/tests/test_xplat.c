/* xplat's tests (C). [rexglue-native-platform-fix] 2026-11-23
 *
 * Run from a folder the test may write in (ctest runs it in the build folder). It makes and removes
 * "xplat_tmp_<name>" there and next to its own program. Arguments:
 *   --expect-arg <text>    xplat_utf8_args must hand back <text> as argv[2] (ctest passes a name outside ASCII)
 *   --expect-utf8-acp      Windows: the manifest's UTF-8 code page must be in force (GetACP() == 65001)
 * Exit 0 when every check passes; each failed check prints its line. */
#if defined(_WIN32) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS /* _wfopen (see xplat_win32.c) */
#endif
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <wchar.h>
#else
#include <unistd.h>
#endif

#include "xplat/xplat.h"
#include "xplat_internal.h" /* root_length, join, plain_name, and on Windows system_path */

static int failures = 0, checks = 0;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        checks++;                                                                                                      \
        if (!(cond)) {                                                                                                 \
            failures++;                                                                                                \
            printf("FAIL %s:%d: %s (errno %d)\n", __FILE__, __LINE__, #cond, errno);                                  \
        }                                                                                                              \
    } while (0)

/* "Łódź-日本-ü-🎮": two-byte, three-byte and four-byte UTF-8 (the last is a surrogate pair in UTF-16). Written as
 * bytes so the source file's encoding can't change it; split so no \x swallows the letter after it. */
#define NAME "\xC5\x81\xC3\xB3" "d" "\xC5\xBA" "-\xE6\x97\xA5\xE6\x9C\xAC-\xC3\xBC-\xF0\x9F\x8E\xAE"
#if defined(_WIN32)
#define WNAME L"\u0141\u00F3d\u017A-\u65E5\u672C-\u00FC-\U0001F3AE"
#endif

/* ---- helpers -------------------------------------------------------------------------------------------------- */

static char *join(const char *a, const char *b)
{
    size_t size = strlen(a) + strlen(b) + 2;
    char *out = (char *)malloc(size);

    if (!out || xplat__join(out, size, a, b) != 0) {
        printf("FAIL join(%s, %s)\n", a, b);
        exit(2);
    }
    return out;
}

static int write_text(const char *path, const char *text)
{
    FILE *f = xplat_fopen(path, "wb");

    if (!f)
        return -1;
    fputs(text, f);
    return fclose(f);
}

/* the file's first line (up to 255 bytes) into a static buffer, "" when it can't be read */
static const char *read_text(const char *path)
{
    static char buf[256];
    FILE *f = xplat_fopen(path, "rb");
    size_t n = 0;

    buf[0] = '\0';
    if (f) {
        n = fread(buf, 1, sizeof buf - 1, f);
        fclose(f);
    }
    buf[n] = '\0';
    return buf;
}

/* removes a folder and everything in it, through xplat only (so it tests opendir/readdir/remove too) */
static void remove_tree(const char *path)
{
    xplat_stat_t st;
    xplat_dir *d;
    const char *name;
    int is_dir = 0;

    if (xplat_stat(path, &st) != 0)
        return;
    if (!st.is_dir) {
        xplat_remove(path);
        return;
    }
    d = xplat_opendir(path);
    if (d) {
        while ((name = xplat_readdir(d, &is_dir)) != NULL) {
            char *child = join(path, name);

            /* the name is only good until the next readdir: the recursion opens other folders, not this one */
            remove_tree(child);
            free(child);
        }
        xplat_closedir(d);
    }
    xplat_remove(path);
}

/* ---- text and path pieces ------------------------------------------------------------------------------------- */

static void test_utf8_valid(void)
{
    CHECK(xplat_utf8_valid("", 0));
    CHECK(xplat_utf8_valid("plain", 5));
    CHECK(xplat_utf8_valid(NAME, strlen(NAME)));
    CHECK(xplat_utf8_valid("\xF4\x8F\xBF\xBF", 4));    /* U+10FFFF */
    CHECK(xplat_utf8_valid("\xEF\xBF\xBF", 3));        /* U+FFFF */
    CHECK(!xplat_utf8_valid("\xC0\x80", 2));           /* overlong NUL */
    CHECK(!xplat_utf8_valid("\xE0\x80\xAF", 3));       /* overlong "/" */
    CHECK(!xplat_utf8_valid("\xF0\x80\x80\xAF", 4));   /* overlong "/" */
    CHECK(!xplat_utf8_valid("\xED\xA0\x80", 3));       /* U+D800, a surrogate */
    CHECK(!xplat_utf8_valid("\xF4\x90\x80\x80", 4));   /* past U+10FFFF */
    CHECK(!xplat_utf8_valid("\xC5", 1));               /* cut short */
    CHECK(!xplat_utf8_valid("\xE6\x97", 2));           /* cut short */
    CHECK(!xplat_utf8_valid("\x81", 1));               /* a continuation byte first */
    CHECK(!xplat_utf8_valid("\xC3\x28", 2));           /* not a continuation */
    CHECK(!xplat_utf8_valid("\xFF", 1));
}

static void test_path_pieces(void)
{
    char buf[64];

    CHECK(xplat__root_length("") == 0);
    CHECK(xplat__root_length("relative/x") == 0);
    CHECK(xplat__root_length("/abs") == 1);
#if defined(_WIN32)
    CHECK(xplat__root_length("C:\\x") == 3);
    CHECK(xplat__root_length("c:/x") == 3);
    CHECK(xplat__root_length("C:x") == 2);
    CHECK(xplat__root_length("\\x") == 1);
    CHECK(xplat__root_length("\\\\server\\share\\x") == strlen("\\\\server\\share\\"));
    CHECK(xplat__root_length("//server/share/x") == strlen("//server/share/"));
    CHECK(xplat__root_length("\\\\server\\share") == strlen("\\\\server\\share"));
    CHECK(xplat__root_length("\\\\?\\C:\\x") == 7);
    CHECK(xplat__root_length("\\\\?\\UNC\\server\\share\\x") == strlen("\\\\?\\UNC\\server\\share\\"));
    CHECK(xplat__root_length("\\\\.\\pipe\\x") == 4);
#else
    CHECK(xplat__root_length("C:\\x") == 0); /* an ordinary name on POSIX */
    CHECK(xplat__root_length("//x") == 1);
#endif

    CHECK(xplat__join(buf, sizeof buf, "a", "b") == 0);
#if defined(_WIN32)
    CHECK(strcmp(buf, "a\\b") == 0);
    CHECK(xplat__join(buf, sizeof buf, "a\\", "\\b") == 0 && strcmp(buf, "a\\b") == 0);
#else
    CHECK(strcmp(buf, "a/b") == 0);
#endif
    CHECK(xplat__join(buf, sizeof buf, "a/", "/b") == 0 && strcmp(buf, "a/b") == 0);
    CHECK(xplat__join(buf, sizeof buf, "", "b") == 0 && strcmp(buf, "b") == 0);
    errno = 0;
    CHECK(xplat__join(buf, 4, "abc", "def") == -1 && errno == ENAMETOOLONG && buf[0] == '\0');

    CHECK(xplat__plain_name("CoD3-recomp"));
    CHECK(xplat__plain_name(NAME));
    CHECK(xplat__plain_name(".hidden"));
    CHECK(!xplat__plain_name(""));
    CHECK(!xplat__plain_name("."));
    CHECK(!xplat__plain_name(".."));
    CHECK(!xplat__plain_name("a/b"));
    CHECK(!xplat__plain_name("a\\b"));
    CHECK(!xplat__plain_name("a:b"));
    CHECK(!xplat__plain_name("a?"));
    CHECK(!xplat__plain_name("trailing."));
    CHECK(!xplat__plain_name("trailing "));
    CHECK(!xplat__plain_name("tab\tname"));
    CHECK(!xplat__plain_name(NULL));
}

#if defined(_WIN32)
/* The form long paths are handed to Windows in (condition 2: normal first, \\?\UNC\ for shares). */
static const wchar_t *widen(const char *fmt, const char *arg)
{
    static wchar_t out[1024];
    char text[1024];

    snprintf(text, sizeof text, fmt, arg);
    if (MultiByteToWideChar(CP_UTF8, 0, text, -1, out, 1024) <= 0)
        out[0] = L'\0';
    return out;
}

static void test_system_path(void)
{
    char long_part[400], text[600];
    wchar_t *w;
    size_t i;

    for (i = 0; i < 300; i++)
        long_part[i] = (char)('a' + i % 26);
    long_part[300] = '\0';

    w = xplat__system_path("short/x/../y"); /* short: unchanged (Windows resolves it itself) */
    CHECK(w && wcscmp(w, L"short/x/../y") == 0);
    free(w);

    snprintf(text, sizeof text, "C:/base/x/../%s/./end", long_part); /* forward slashes, "..", "." */
    w = xplat__system_path(text);
    CHECK(w && wcscmp(w, widen("\\\\?\\C:\\base\\%s\\end", long_part)) == 0);
    free(w);

    snprintf(text, sizeof text, "//server/share/x/../%s", long_part); /* a share, forward slashes */
    w = xplat__system_path(text);
    CHECK(w && wcscmp(w, widen("\\\\?\\UNC\\server\\share\\%s", long_part)) == 0);
    free(w);

    snprintf(text, sizeof text, "\\\\server\\share\\%s", long_part);
    w = xplat__system_path(text);
    CHECK(w && wcscmp(w, widen("\\\\?\\UNC\\server\\share\\%s", long_part)) == 0);
    free(w);

    snprintf(text, sizeof text, "\\\\?\\C:\\%s", long_part); /* already \\?\: untouched */
    w = xplat__system_path(text);
    CHECK(w && wcscmp(w, widen("\\\\?\\C:\\%s", long_part)) == 0);
    free(w);

    snprintf(text, sizeof text, "rel/%s", long_part); /* relative: made full against the working folder */
    w = xplat__system_path(text);
    CHECK(w && wcsncmp(w, L"\\\\?\\", 4) == 0 && w[5] == L':' && wcsstr(w, L"\\rel\\abc") != NULL);
    free(w);

    errno = 0;
    w = xplat__system_path("bad-\xC3\x28-utf8");
    CHECK(w == NULL && errno == EINVAL);
    errno = 0;
    CHECK(xplat_fopen("bad-\xC3\x28-utf8", "rb") == NULL);
}
#endif

/* ---- files and folders ---------------------------------------------------------------------------------------- */

static void test_files_in(const char *base)
{
    char *root = join(base, "xplat_tmp_" NAME);
    char *sub = join(root, "sub/deeper/" NAME);
    char *file = join(root, NAME ".txt");
    char *other = join(root, "ziel.txt");
    char *missing = join(root, "missing");
    char *file_as_parent = join(file, "child");
    xplat_stat_t st;
    xplat_dir *d;
    const char *name;
    int is_dir = -1, saw_file = 0, saw_sub = 0, saw_dots = 0, entries = 0;

    remove_tree(root);
    CHECK(xplat_mkdirs(sub) == 0);
    CHECK(xplat_stat(sub, &st) == 0 && st.is_dir && !st.is_file && st.size == 0);
    CHECK(xplat_mkdirs(sub) == 0); /* again: already there */
    CHECK(xplat_mkdir(sub) == 0);
    {
        char *trailing = join(root, "trail/ing/");
        CHECK(xplat_mkdirs(trailing) == 0);
        free(trailing);
    }

    CHECK(write_text(file, "hello") == 0);
    CHECK(xplat_stat(file, &st) == 0 && st.is_file && !st.is_dir && st.size == 5);
    CHECK(st.mtime_ns > 1600000000LL * 1000000000LL); /* after 2020: the epoch conversion is right */
    CHECK(strcmp(read_text(file), "hello") == 0);

#if defined(_WIN32)
    { /* the file really has the name, as Windows' own wide functions see it */
        wchar_t wide[1024];
        FILE *f;

        CHECK(MultiByteToWideChar(CP_UTF8, 0, file, -1, wide, 1024) > 0);
        CHECK(wcsstr(wide, L"xplat_tmp_" WNAME) != NULL);
        f = _wfopen(wide, L"rb");
        CHECK(f != NULL);
        if (f)
            fclose(f);
    }
#endif

    errno = 0;
    CHECK(xplat_mkdir(file) == -1 && errno == EEXIST);    /* a file of that name */
    errno = 0;
    CHECK(xplat_mkdirs(file_as_parent) == -1);            /* a file in the way */
    errno = 0;
    CHECK(xplat_stat(missing, &st) == -1 && errno == ENOENT);
    errno = 0;
    CHECK(xplat_fopen(missing, "rb") == NULL && errno == ENOENT);
    errno = 0;
    CHECK(xplat_remove(missing) == -1 && errno == ENOENT);
    errno = 0;
    CHECK(xplat_mkdirs("") == -1 && errno == ENOENT);

    d = xplat_opendir(root);
    CHECK(d != NULL);
    while (d && (name = xplat_readdir(d, &is_dir)) != NULL) {
        entries++;
        if (strcmp(name, NAME ".txt") == 0)
            saw_file = !is_dir;
        else if (strcmp(name, "sub") == 0)
            saw_sub = is_dir;
        else if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
            saw_dots = 1;
    }
    xplat_closedir(d);
    CHECK(saw_file && saw_sub && !saw_dots && entries == 3); /* the file, sub, trail */
    errno = 0;
    CHECK(xplat_opendir(missing) == NULL && errno == ENOENT);

    CHECK(write_text(other, "old") == 0);
    CHECK(xplat_rename_replace(file, other) == 0); /* replaces "old" */
    CHECK(strcmp(read_text(other), "hello") == 0);
    CHECK(xplat_stat(file, &st) == -1 && errno == ENOENT);
    CHECK(xplat_rename_replace(other, file) == 0); /* onto nothing */
    CHECK(strcmp(read_text(file), "hello") == 0);

    {
        char *root_sub = join(root, "sub");
        errno = 0;
        CHECK(xplat_remove(root_sub) == -1); /* not empty */
        free(root_sub);
    }

    remove_tree(root);
    CHECK(xplat_stat(root, &st) == -1 && errno == ENOENT);
    free(root);
    free(sub);
    free(file);
    free(other);
    free(missing);
    free(file_as_parent);
}

/* Paths past 260 characters (condition 2): forward slashes, "..", relative and full. */
static void test_long_paths_in(const char *base)
{
    char *root = join(base, "xplat_tmp_long_" NAME);
    char deep[2048], *file, *moved;
    xplat_stat_t st;
    xplat_dir *d;
    const char *name;
    int i, is_dir = -1, found = 0;

    remove_tree(root);
    /* root/x/../<7 parts of ~50 characters> with "/" throughout: over 260 UTF-16 units past the root alone */
    snprintf(deep, sizeof deep, "%s/x/..", root);
    for (i = 0; i < 7; i++) {
        size_t n = strlen(deep);
        snprintf(deep + n, sizeof deep - n, "/%d-%s-part-of-a-long-path-%s", i, NAME, "abcdefghij");
    }
    CHECK(strlen(deep) > 300);
    CHECK(xplat_mkdirs(deep) == 0);
    CHECK(xplat_stat(deep, &st) == 0 && st.is_dir);

    file = join(deep, NAME "-file-with-a-long-name-too.txt");
    moved = join(deep, "moved.txt");
    CHECK(write_text(file, "long") == 0);
    CHECK(xplat_stat(file, &st) == 0 && st.is_file && st.size == 4);
    CHECK(strcmp(read_text(file), "long") == 0);
    CHECK(xplat_rename_replace(file, moved) == 0);
    d = xplat_opendir(deep);
    CHECK(d != NULL);
    while (d && (name = xplat_readdir(d, &is_dir)) != NULL)
        found += strcmp(name, "moved.txt") == 0 && !is_dir;
    xplat_closedir(d);
    CHECK(found == 1);
    CHECK(xplat_remove(moved) == 0);

    remove_tree(root);
    CHECK(xplat_stat(root, &st) == -1);
    free(file);
    free(moved);
    free(root);
}

/* ---- the process ---------------------------------------------------------------------------------------------- */

static void set_env(const char *name, const char *value)
{
#if defined(_WIN32)
    wchar_t wname[256], wvalue[8192];

    MultiByteToWideChar(CP_UTF8, 0, name, -1, wname, 256);
    if (value)
        MultiByteToWideChar(CP_UTF8, 0, value, -1, wvalue, 8192);
    SetEnvironmentVariableW(wname, value ? wvalue : NULL);
#else
    if (value)
        setenv(name, value, 1);
    else
        unsetenv(name);
#endif
}

static void test_process(char *exe_dir_out_buf, size_t exe_dir_size)
{
    char buf[8192], big[4000];
    xplat_stat_t st;
    size_t i;

    set_env("XPLAT_TEST_VAR", NAME);
    CHECK(xplat_getenv("XPLAT_TEST_VAR", buf, sizeof buf) == 0 && strcmp(buf, NAME) == 0);
    errno = 0;
    CHECK(xplat_getenv("XPLAT_TEST_VAR", buf, 4) == -1 && errno == ENAMETOOLONG);
    for (i = 0; i < sizeof big - 1; i++) /* past getenv's 1024-character first try */
        big[i] = (char)('A' + i % 26);
    big[sizeof big - 1] = '\0';
    set_env("XPLAT_TEST_VAR", big);
    CHECK(xplat_getenv("XPLAT_TEST_VAR", buf, sizeof buf) == 0 && strcmp(buf, big) == 0);
    set_env("XPLAT_TEST_VAR", NULL);
    errno = 0;
    CHECK(xplat_getenv("XPLAT_TEST_VAR", buf, sizeof buf) == -1 && errno == ENOENT);

    CHECK(xplat_exe_path(buf, sizeof buf) == 0);
    CHECK(xplat__root_length(buf) > 0);                     /* full */
    CHECK(strstr(buf, "xplat_test") != NULL);
    CHECK(xplat_stat(buf, &st) == 0 && st.is_file);
    errno = 0;
    {
        char small[4];
        CHECK(xplat_exe_path(small, sizeof small) == -1 && errno == ENAMETOOLONG);
    }
    /* the program's folder, for the full-path tests */
    {
        char *cut = buf + strlen(buf);

        while (cut > buf && !XPLAT_IS_SEPARATOR(cut[-1]))
            cut--;
        if (cut > buf)
            cut[-1] = '\0';
        xplat__copy_out(buf, strlen(buf), exe_dir_out_buf, exe_dir_size);
    }
}

static void test_user_dirs(void)
{
    char buf[8192], saved_home[8192];
    int had_home;

    errno = 0;
    CHECK(xplat_user_dir(XPLAT_USER_CONFIG, "", buf, sizeof buf) == -1 && errno == EINVAL);
    CHECK(xplat_user_dir(XPLAT_USER_CONFIG, "../x", buf, sizeof buf) == -1 && errno == EINVAL);
    CHECK(xplat_user_dir((xplat_user_dir_kind)7, "App", buf, sizeof buf) == -1 && errno == EINVAL);
#if defined(_WIN32)
    (void)saved_home;
    (void)had_home;
    {
        char appdata[8192];

        CHECK(xplat_user_dir(XPLAT_USER_CONFIG, NAME, buf, sizeof buf) == 0);
        printf("  config: %s\n", buf);
        CHECK(xplat__root_length(buf) > 0);
        CHECK(strlen(buf) > strlen("\\" NAME) && strcmp(buf + strlen(buf) - strlen("\\" NAME), "\\" NAME) == 0);
        if (xplat_getenv("APPDATA", appdata, sizeof appdata) == 0) /* the known folder is normally APPDATA */
            printf("  APPDATA: %s\n", appdata);
        CHECK(xplat_user_dir(XPLAT_USER_CACHE, "App", buf, sizeof buf) == 0);
        CHECK(strcmp(buf + strlen(buf) - strlen("\\App\\cache"), "\\App\\cache") == 0);
        CHECK(xplat_user_dir(XPLAT_USER_DATA, "App", buf, sizeof buf) == 0);
        CHECK(strcmp(buf + strlen(buf) - strlen("\\App"), "\\App") == 0);
        CHECK(xplat_user_dir(XPLAT_USER_STATE, "App", buf, sizeof buf) == 0);
        errno = 0;
        CHECK(xplat_user_dir(XPLAT_USER_CACHE, "App", buf, 8) == -1 && errno == ENAMETOOLONG);
    }
#else
    had_home = xplat_getenv("HOME", saved_home, sizeof saved_home) == 0;
    set_env("HOME", "/home/user-" NAME);
    set_env("XDG_CONFIG_HOME", NULL);
    set_env("XDG_DATA_HOME", "");
    set_env("XDG_CACHE_HOME", "relative/ignored");
    set_env("XDG_STATE_HOME", "/xdg/state");
    CHECK(xplat_user_dir(XPLAT_USER_CONFIG, "App", buf, sizeof buf) == 0 &&
          strcmp(buf, "/home/user-" NAME "/.config/App") == 0);
    CHECK(xplat_user_dir(XPLAT_USER_DATA, "App", buf, sizeof buf) == 0 &&
          strcmp(buf, "/home/user-" NAME "/.local/share/App") == 0);
    CHECK(xplat_user_dir(XPLAT_USER_CACHE, "App", buf, sizeof buf) == 0 &&
          strcmp(buf, "/home/user-" NAME "/.cache/App") == 0);
    CHECK(xplat_user_dir(XPLAT_USER_STATE, NAME, buf, sizeof buf) == 0 && strcmp(buf, "/xdg/state/" NAME) == 0);
    set_env("XDG_CONFIG_HOME", "/xdg/config/");
    CHECK(xplat_user_dir(XPLAT_USER_CONFIG, "App", buf, sizeof buf) == 0 && strcmp(buf, "/xdg/config/App") == 0);
    set_env("HOME", "relative-home");
    errno = 0;
    CHECK(xplat_user_dir(XPLAT_USER_DATA, "App", buf, sizeof buf) == -1 && errno == ENOENT);
    set_env("HOME", had_home ? saved_home : NULL);
    set_env("XDG_CONFIG_HOME", NULL);
    set_env("XDG_DATA_HOME", NULL);
    set_env("XDG_CACHE_HOME", NULL);
    set_env("XDG_STATE_HOME", NULL);
#endif
}

/* ---- time ----------------------------------------------------------------------------------------------------- */

static void test_time(void)
{
    uint64_t last = xplat_monotonic_ns(), t0, t1, worst = 0, total = 0;
    int i, backwards = 0;

    for (i = 0; i < 100000; i++) {
        const uint64_t now = xplat_monotonic_ns();

        backwards += now < last;
        last = now;
    }
    CHECK(backwards == 0);
    CHECK(xplat_request_fine_timer() == 1);
    CHECK(xplat_request_fine_timer() == 1); /* once; the same answer */

    t0 = xplat_monotonic_ns();
    xplat_sleep_ns(5000000);
    t1 = xplat_monotonic_ns();
    CHECK(t1 - t0 >= 5000000);
    CHECK(t1 - t0 < 250000000); /* generous: a loaded machine, Wine */

    t0 = xplat_monotonic_ns();
    xplat_sleep_until_ns(t0 - 1000000); /* in the past: back at once */
    xplat_sleep_until_ns(0);
    CHECK(xplat_monotonic_ns() - t0 < 5000000);

    for (i = 0; i < 20; i++) { /* a frame limiter's waits: how late does a 2 ms sleep end? */
        const uint64_t deadline = xplat_monotonic_ns() + 2000000;
        uint64_t late;

        xplat_sleep_until_ns(deadline);
        late = xplat_monotonic_ns() - deadline;
        CHECK(xplat_monotonic_ns() >= deadline);
        total += late;
        if (late > worst)
            worst = late;
    }
    printf("  2 ms sleeps: %.3f ms late on average, %.3f ms at worst\n", (double)total / 20 / 1e6, (double)worst / 1e6);
    CHECK(total / 20 < 20000000); /* informative; only a broken sleep is 20 ms late on average */
}

/* ---- main ----------------------------------------------------------------------------------------------------- */

int main(int argc, char **argv)
{
    char exe_dir[8192] = "";
    int i;

    CHECK(xplat_utf8_args(&argc, &argv) == 0);
    CHECK(argc >= 1 && argv[argc] == NULL);
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--expect-arg") == 0 && i + 1 < argc) {
            i++;
            printf("  argument: %s\n", argv[i]);
            CHECK(xplat_utf8_valid(argv[i], strlen(argv[i])));
            CHECK(strcmp(argv[i], NAME) == 0);
        } else if (strcmp(argv[i], "--expect-utf8-acp") == 0) {
#if defined(_WIN32)
            printf("  GetACP() = %u\n", GetACP());
            CHECK(GetACP() == 65001);
#endif
        }
    }
#if defined(_WIN32)
    printf("  code page %u, console output %u\n", GetACP(), GetConsoleOutputCP());
#endif

    test_utf8_valid();
    test_path_pieces();
#if defined(_WIN32)
    test_system_path();
#endif
    test_process(exe_dir, sizeof exe_dir);
    test_files_in(".");
    test_long_paths_in(".");
    if (exe_dir[0]) { /* the same with a full path */
        test_files_in(exe_dir);
        test_long_paths_in(exe_dir);
    }
    test_user_dirs();
    test_time();

    printf("%s: %d checks, %d failed\n", failures ? "FAILED" : "ok", checks, failures);
    return failures ? 1 : 0;
}
