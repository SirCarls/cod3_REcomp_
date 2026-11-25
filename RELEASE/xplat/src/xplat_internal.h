/* xplat's own helpers shared by xplat_common.c and the two system files. Not part of the API. */
#ifndef XPLAT_INTERNAL_H
#define XPLAT_INTERNAL_H

#include <stddef.h>

#include "xplat/xplat.h"

#if defined(_WIN32)
#define XPLAT_SEPARATOR '\\'
#define XPLAT_IS_SEPARATOR(c) ((c) == '\\' || (c) == '/')
#else
#define XPLAT_SEPARATOR '/'
#define XPLAT_IS_SEPARATOR(c) ((c) == '/')
#endif

/* Copies `len` bytes of `text` and a NUL into buf; ENAMETOOLONG (and -1) when it doesn't fit. */
int xplat__copy_out(const char *text, size_t len, char *buf, size_t size);

/* buf = a + separator + b (no separator doubled); ENAMETOOLONG when it doesn't fit. */
int xplat__join(char *buf, size_t size, const char *a, const char *b);

/* How many bytes at the start of `path` are its root, never made or removed by xplat_mkdirs: "/" on POSIX; "C:\",
 * "C:", "\\server\share\", "\\?\C:\" on Windows; 0 for a relative path. */
size_t xplat__root_length(const char *path);

/* 1 when `app` is a plain folder name: not empty, no separator, not "." or "..", no character Windows refuses in a
 * name (< > : " | ? * and controls), not ending in a dot or a space. */
int xplat__plain_name(const char *app);

#if defined(_WIN32)
#include <wchar.h>
/* A UTF-8 path as the wide Windows functions should be given it, in a new string (free it); NULL with errno for
 * text that isn't UTF-8. Long paths come back full and in the \\?\ (or \\?\UNC\) form; see xplat_win32.c. */
wchar_t *xplat__system_path(const char *path);
#endif

/* The system part of xplat_user_dir: the base folder for `kind` (without the app). */
int xplat__user_base(xplat_user_dir_kind kind, char *buf, size_t size);

#endif
