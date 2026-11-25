/* xplat: the parts that are the same on every system (see include/xplat/xplat.h). */
#include <errno.h>
#include <string.h>

#include "xplat_internal.h"

int xplat__copy_out(const char *text, size_t len, char *buf, size_t size)
{
    if (!buf || size == 0) {
        errno = EINVAL;
        return -1;
    }
    if (len >= size) {
        buf[0] = '\0';
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(buf, text, len);
    buf[len] = '\0';
    return 0;
}

int xplat__join(char *buf, size_t size, const char *a, const char *b)
{
    size_t la = strlen(a), lb = strlen(b);
    int add = la > 0 && !XPLAT_IS_SEPARATOR(a[la - 1]);

    while (lb > 0 && XPLAT_IS_SEPARATOR(*b)) { /* "a" + "/b" is a/b, never a//b */
        b++;
        lb--;
    }
    if (!buf || la + (size_t)add + lb >= size) {
        if (buf && size)
            buf[0] = '\0';
        errno = ENAMETOOLONG;
        return -1;
    }
    memmove(buf, a, la);
    if (add)
        buf[la++] = XPLAT_SEPARATOR;
    memcpy(buf + la, b, lb);
    buf[la + lb] = '\0';
    return 0;
}

size_t xplat__root_length(const char *path)
{
#if defined(_WIN32)
    size_t n = 0;

    if (XPLAT_IS_SEPARATOR(path[0]) && XPLAT_IS_SEPARATOR(path[1])) {
        /* \\?\C:\..., \\?\UNC\server\share\..., \\server\share\... */
        if ((path[2] == '?' || path[2] == '.') && XPLAT_IS_SEPARATOR(path[3])) {
            n = 4;
            if (((path[4] | 0x20) >= 'a' && (path[4] | 0x20) <= 'z') && path[5] == ':')
                return XPLAT_IS_SEPARATOR(path[6]) ? 7 : 6;
            if ((path[4] | 0x20) == 'u' && (path[5] | 0x20) == 'n' && (path[6] | 0x20) == 'c' &&
                XPLAT_IS_SEPARATOR(path[7]))
                n = 8;
            else
                return n;
        } else {
            n = 2;
        }
        for (int part = 0; part < 2 && path[n]; part++) { /* server, then share */
            while (path[n] && !XPLAT_IS_SEPARATOR(path[n]))
                n++;
            if (XPLAT_IS_SEPARATOR(path[n]))
                n++;
        }
        return n;
    }
    if (((path[0] | 0x20) >= 'a' && (path[0] | 0x20) <= 'z') && path[1] == ':')
        return XPLAT_IS_SEPARATOR(path[2]) ? 3 : 2;
    return XPLAT_IS_SEPARATOR(path[0]) ? 1 : 0;
#else
    return path[0] == '/' ? 1 : 0;
#endif
}

int xplat__plain_name(const char *app)
{
    size_t n;

    if (!app || !app[0] || strcmp(app, ".") == 0 || strcmp(app, "..") == 0)
        return 0;
    for (n = 0; app[n]; n++) {
        unsigned char c = (unsigned char)app[n];

        if (c < 0x20 || c == '/' || c == '\\' || c == ':' || c == '<' || c == '>' || c == '"' || c == '|' ||
            c == '?' || c == '*')
            return 0;
    }
    return app[n - 1] != '.' && app[n - 1] != ' ';
}

int xplat_mkdirs(const char *path)
{
    char buf[32768];
    size_t len, i, root;

    if (!path || !path[0]) {
        errno = ENOENT;
        return -1;
    }
    len = strlen(path);
    if (xplat__copy_out(path, len, buf, sizeof buf))
        return -1;
    while (len > 1 && XPLAT_IS_SEPARATOR(buf[len - 1]) && len > xplat__root_length(buf)) /* "a/b/" is a/b */
        buf[--len] = '\0';
    root = xplat__root_length(buf);
    for (i = root; i <= len; i++) {
        if (i == len || XPLAT_IS_SEPARATOR(buf[i])) {
            char saved = buf[i];

            if (i == root) /* nothing between the root and here */
                continue;
            buf[i] = '\0';
            if (xplat_mkdir(buf) != 0) {
                xplat_stat_t st;
                int error = errno;

                /* A folder above that can't be made but is there (a drive's root, a share, a folder we may not
                 * write in but may go through) is fine; anything else stops here. */
                if (i == len || xplat_stat(buf, &st) != 0 || !st.is_dir) {
                    errno = error;
                    return -1;
                }
            }
            buf[i] = saved;
        }
    }
    return 0;
}

int xplat_user_dir(xplat_user_dir_kind kind, const char *app, char *buf, size_t size)
{
    char base[32768];

    if (!xplat__plain_name(app) || kind < XPLAT_USER_CONFIG || kind > XPLAT_USER_STATE) {
        errno = EINVAL;
        return -1;
    }
    if (xplat__user_base(kind, base, sizeof base))
        return -1;
    if (xplat__join(buf, size, base, app))
        return -1;
#if defined(_WIN32)
    if (kind == XPLAT_USER_CACHE) { /* Windows has no cache folder of its own: <LocalAppData>\<app>\cache */
        size_t used = strlen(buf);

        if (used + sizeof "\\cache" > size) {
            buf[0] = '\0';
            errno = ENAMETOOLONG;
            return -1;
        }
        memcpy(buf + used, "\\cache", sizeof "\\cache");
    }
#endif
    return 0;
}

int xplat_utf8_valid(const char *text, size_t len)
{
    const unsigned char *s = (const unsigned char *)text;
    size_t i = 0;

    while (i < len) {
        unsigned char c = s[i];
        size_t need;
        uint32_t code, low;

        if (c < 0x80) {
            i++;
            continue;
        }
        if (c >= 0xC2 && c <= 0xDF) {
            need = 1;
            code = c & 0x1F;
            low = 0x80;
        } else if (c >= 0xE0 && c <= 0xEF) {
            need = 2;
            code = c & 0x0F;
            low = 0x800;
        } else if (c >= 0xF0 && c <= 0xF4) {
            need = 3;
            code = c & 0x07;
            low = 0x10000;
        } else {
            return 0; /* a continuation byte first, 0xC0/0xC1 (overlong), or 0xF5 and up */
        }
        if (len - i <= need)
            return 0;
        for (size_t k = 1; k <= need; k++) {
            if ((s[i + k] & 0xC0) != 0x80)
                return 0;
            code = (code << 6) | (s[i + k] & 0x3F);
        }
        if (code < low || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF))
            return 0;
        i += need + 1;
    }
    return 1;
}

void xplat_sleep_ns(uint64_t ns)
{
    xplat_sleep_until_ns(xplat_monotonic_ns() + ns);
}
