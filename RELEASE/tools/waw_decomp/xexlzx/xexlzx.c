/*
 * xexlzx.c - LZX decompression for Xbox 360 XEX images, built on libmspack's
 * LZX decoder (lzxd.c, LGPL-2.1, https://github.com/kyz/libmspack), with a
 * memory-backed mspack_system. Used by tools/waw_decomp/xex2pe.py through ctypes.
 *
 * Build (Linux):  cc -O2 -shared -fPIC -I<libmspack>/libmspack/mspack \
 *                    xexlzx.c <libmspack>/libmspack/mspack/lzxd.c -o libxexlzx.so
 */
#include <stdlib.h>
#include <string.h>
#include "system.h"
#include "lzx.h"

struct mem_file {
  unsigned char *buf;
  size_t len;
  size_t pos;
};

static int mem_read(struct mspack_file *f, void *dst, int n) {
  struct mem_file *m = (struct mem_file *)f;
  size_t left = m->len - m->pos;
  size_t k = (size_t)n < left ? (size_t)n : left;
  memcpy(dst, m->buf + m->pos, k);
  m->pos += k;
  return (int)k;
}

static int mem_write(struct mspack_file *f, void *src, int n) {
  struct mem_file *m = (struct mem_file *)f;
  size_t left = m->len - m->pos;
  size_t k = (size_t)n < left ? (size_t)n : left;
  memcpy(m->buf + m->pos, src, k);
  m->pos += k;
  return (int)k; /* a short write makes lzxd stop with an error */
}

static void *mem_alloc(struct mspack_system *s, size_t n) { (void)s; return malloc(n); }
static void mem_free(void *p) { free(p); }
static void mem_copy(void *src, void *dst, size_t n) { memcpy(dst, src, n); }
static void mem_message(struct mspack_file *f, const char *fmt, ...) { (void)f; (void)fmt; }

/* Returns 0 on success, otherwise the libmspack error code (or -1). */
int xex_lzx_decompress(const unsigned char *src, size_t src_len, unsigned char *dst, size_t dst_len,
                       int window_bits) {
  struct mspack_system sys;
  memset(&sys, 0, sizeof(sys));
  sys.read = mem_read;
  sys.write = mem_write;
  sys.alloc = mem_alloc;
  sys.free = mem_free;
  sys.copy = mem_copy;
  sys.message = mem_message;
  struct mem_file in = {(unsigned char *)src, src_len, 0};
  struct mem_file out = {dst, dst_len, 0};
  struct lzxd_stream *lzx = lzxd_init(&sys, (struct mspack_file *)&in, (struct mspack_file *)&out, window_bits, 0,
                                      0x8000, (off_t)dst_len, 0);
  if (!lzx) return -1;
  int err = lzxd_decompress(lzx, (off_t)dst_len);
  lzxd_free(lzx);
  return err;
}
