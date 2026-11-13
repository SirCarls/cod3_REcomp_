/* [acct2-bro] Big Red One port: nglLoadTexture, natively.
 *
 * nglLoadTexture(name, type, data, size) (0x000635A0) asks the texture manager for the texture called name and, if it
 * has none, makes one from the file the caller has already read into memory (nglLoadTextureFile 0x0006B7E0): a
 * 0x74-byte nglTexture, filled in from the file's DDS header by nglLoadDDS (0x0006B070), which then hands the pixels
 * to Direct3D in place (swizzling them first) and marks the file done ('M' at byte 0x7F). Direct3D and the texture
 * manager are stand-ins here, so this does that work natively instead:
 *
 *   - one nglTexture per name, made the first time and counted up (its first word) after, as the manager did;
 *   - the fields nglLoadDDS fills that the game's own code reads: size +0x10 / +0x14, levels +0x18, format +0x08,
 *     flags +0x04 (0x100: loaded) and +0x28 (bit 0: in use), the name at +0x54;
 *   - level 0 decoded on the host (blue, green, red, alpha) for ngl_vk to draw, and the file's smaller levels after
 *     it (a DDS file keeps them one after another, as many as its header's level count), so far surfaces are
 *     filtered the way the Xbox's GPU filtered them. A texture with one-bit alpha (DXT1, A1R5G5B5: it can only be cut
 *     out) keeps its level 0's share of opaque texels in every level, as CoD3's native path does, so a far fence or
 *     bush doesn't thin away.
 *
 * The files are ordinary DDS files with the pixels in rows (nglLoadDDS swizzles them for the Xbox's GPU; nothing here
 * needs that). Formats nglLoadDDS takes, by its own format numbers: DXT1 (0x0C), DXT2/3 (0x0E), DXT4/5 (0x0F),
 * A8R8G8B8 (6), R5G6B5 (5), A1R5G5B5 (2), A4R4G4B4 (4), L8 (0), A8 (0x19), A4L4 (1), P8 (0x0B). Cube maps (caps2 0xFE00)
 * are not decoded yet.
 *
 * Type 3 is an animated texture list (nglLoadAnimatedTextureList 0x00063650): a text file, a frame a line,
 * "name[.ext] [count]". Its frames are textures of their own, often loaded after the list, so each is found by name
 * when the list is first drawn. Characters use them for uniform variants: a cdChar material named
 * "c_1stinffall_multi_3a" is a list whose frames are "..._multi_3aa" to "..._multi_3ad". A mesh draw shows the frame
 * nglSetTextureAnimFrame (0x000476B0) would pick (ngl_mesh.c texture_frame, ngl_texture_list_frame); anything else
 * drawing a list shows its first frame.
 *
 * RE_RegisterShader (0x0023EC30, the HUD's and menus' pictures by path) is native too: the original looks the name
 * up in the streamed zones' texture maps (Stream_FindTexture), else falls back to ngl's white texture. Here that left
 * the crosshair's quads without a texture (drawn as white squares), though its textures (center_dot,
 * side_small_arc) are loaded. Natively the name is looked up among the loaded textures, and one not loaded yet is a
 * one-frame list found by name when it is drawn.
 *
 * BRO_TEX_DUMP_DIR=<folder> saves each decoded texture there as <n>.ppm (color only) and logs its name and format. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "recomp_types.h"
#include "xbox_memory_layout.h"     /* xbox_HeapAlloc, XBOX_PTR: guest memory */
#include "ngl_texture_load.h"

#define NGL_DEFAULT_TEXTURE   0x009026F8u   /* ngl_defaultTexture: what nglLoadTexture returns when loading fails */

#define TEXTURE_BYTES         0x74u
#define TEXTURE_FLAGS         0x04u
#define TEXTURE_FORMAT        0x08u
#define TEXTURE_TYPE          0x0Cu
#define TEXTURE_WIDTH         0x10u
#define TEXTURE_HEIGHT        0x14u
#define TEXTURE_LEVELS        0x18u
#define TEXTURE_USE           0x28u
#define TEXTURE_NAME          0x54u
#define NAME_BYTES            0x20u

#define DDS_MAGIC             0x20534444u   /* "DDS " */
#define DDS_HEADER_BYTES      0x80u
#define DDS_FOURCC(a, b, c, d) ((uint32_t)(a) | (uint32_t)(b) << 8 | (uint32_t)(c) << 16 | (uint32_t)(d) << 24)

#define MAX_TEXTURES          4096u
#define MAX_SIDE              4096u

#define TYPE_ANIMATED_LIST    3u
#define NAME_HASH_BYTES       4u            /* an NGL name: a 4-byte hash, then the name in lower case */
#define MAX_LISTS             256u
#define MAX_FRAMES            16u

static struct ngl_loaded_texture textures[MAX_TEXTURES];
static uint8_t names[MAX_TEXTURES][NAME_BYTES];
static unsigned texture_count;

/* [rexglue-native] 2026-11-11: each texture's file as it was loaded (its bytes hashed, and how many), to tell a name
 * loaded again with ANOTHER file from one loaded again with the same. The game loads every mission's loading-screen
 * map under the one name "texload" (textures\en\sicily.dds for Gela), draws it twice and releases it
 * (nglReleaseTexture, a stand-in here, so nothing is ever let go): one texture a name for good would show the first
 * mission's map on every loading screen after it. */
static uint64_t file_hashes[MAX_TEXTURES];
static uint32_t file_sizes[MAX_TEXTURES];

static uint64_t hash_bytes(const uint8_t *bytes, size_t count)
{
    uint64_t hash = 1469598103934665603ull;     /* FNV-1a */
    size_t i;

    for (i = 0; i < count; i++)
        hash = (hash ^ bytes[i]) * 1099511628211ull;
    return hash;
}

/* animated texture lists: each frame's name (lower case, without the hash), and the frame once found */
static struct {
    uint32_t texture;
    char frames[MAX_FRAMES][NAME_BYTES];
    int found[MAX_FRAMES];                  /* index into textures + 1, or 0 */
    unsigned frame_count;
} lists[MAX_LISTS];
static unsigned list_count;

static uint32_t dds_word(const uint8_t *dds, unsigned index)
{
    uint32_t value;

    memcpy(&value, dds + index * 4, sizeof value);
    return value;
}

/* 5-, 6- and 4-bit color channels widened to 8 bits. */
static uint8_t widen5(uint32_t v) { return (uint8_t)((v << 3) | (v >> 2)); }
static uint8_t widen6(uint32_t v) { return (uint8_t)((v << 2) | (v >> 4)); }
static uint8_t widen4(uint32_t v) { return (uint8_t)((v << 4) | v); }

static void put(uint8_t *out, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    out[0] = b;
    out[1] = g;
    out[2] = r;
    out[3] = a;
}

/* One DXT color block's four colors (index 3 transparent black in DXT1's three-color mode). */
static void dxt_colors(const uint8_t *block, int three_color_mode_allowed, uint8_t colors[4][4])
{
    uint32_t c0 = (uint32_t)block[0] | (uint32_t)block[1] << 8, c1 = (uint32_t)block[2] | (uint32_t)block[3] << 8;
    int i;

    for (i = 0; i < 2; i++) {
        uint32_t c = i ? c1 : c0;

        colors[i][0] = widen5(c >> 11);
        colors[i][1] = widen6((c >> 5) & 0x3F);
        colors[i][2] = widen5(c & 0x1F);
        colors[i][3] = 255;
    }
    for (i = 0; i < 3; i++) {
        if (c0 > c1 || !three_color_mode_allowed) {
            colors[2][i] = (uint8_t)((2 * colors[0][i] + colors[1][i]) / 3);
            colors[3][i] = (uint8_t)((colors[0][i] + 2 * colors[1][i]) / 3);
        } else {
            colors[2][i] = (uint8_t)((colors[0][i] + colors[1][i]) / 2);
            colors[3][i] = 0;
        }
    }
    colors[2][3] = 255;
    colors[3][3] = (c0 > c1 || !three_color_mode_allowed) ? 255 : 0;
}

/* DXT1 (alpha_kind 0), DXT3 (1, explicit 4-bit alpha) or DXT5 (2, interpolated alpha), into rows of BGRA. */
static int decode_dxt(const uint8_t *src, size_t bytes, uint32_t width, uint32_t height, int alpha_kind, uint8_t *out)
{
    uint32_t bx, by, blocks_x = (width + 3) / 4, blocks_y = (height + 3) / 4;
    size_t block_bytes = alpha_kind ? 16 : 8;

    if ((size_t)blocks_x * blocks_y * block_bytes > bytes)
        return -1;
    for (by = 0; by < blocks_y; by++) {
        for (bx = 0; bx < blocks_x; bx++) {
            const uint8_t *block = src + ((size_t)by * blocks_x + bx) * block_bytes;
            const uint8_t *color_block = block + (alpha_kind ? 8 : 0);
            uint8_t colors[4][4], alphas[8];
            uint32_t indices = (uint32_t)color_block[4] | (uint32_t)color_block[5] << 8 |
                               (uint32_t)color_block[6] << 16 | (uint32_t)color_block[7] << 24;
            uint64_t alpha_bits = 0;
            int px, py, i;

            dxt_colors(color_block, alpha_kind == 0, colors);
            if (alpha_kind == 2) {
                alphas[0] = block[0];
                alphas[1] = block[1];
                for (i = 2; i < 8; i++) {
                    if (alphas[0] > alphas[1])
                        alphas[i] = (uint8_t)(((8 - i) * alphas[0] + (i - 1) * alphas[1]) / 7);
                    else if (i < 6)
                        alphas[i] = (uint8_t)(((6 - i) * alphas[0] + (i - 1) * alphas[1]) / 5);
                    else
                        alphas[i] = i == 6 ? 0 : 255;
                }
                for (i = 0; i < 6; i++)
                    alpha_bits |= (uint64_t)block[2 + i] << (8 * i);
            }
            for (py = 0; py < 4; py++) {
                for (px = 0; px < 4; px++) {
                    uint32_t x = bx * 4 + (uint32_t)px, y = by * 4 + (uint32_t)py;
                    int n = py * 4 + px;
                    const uint8_t *c = colors[(indices >> (2 * n)) & 3];
                    uint8_t a = c[3];

                    if (x >= width || y >= height)
                        continue;
                    if (alpha_kind == 1)
                        a = widen4((block[n / 2] >> (4 * (n & 1))) & 0xF);
                    else if (alpha_kind == 2)
                        a = alphas[(alpha_bits >> (3 * n)) & 7];
                    put(out + ((size_t)y * width + x) * 4, c[0], c[1], c[2], a);
                }
            }
        }
    }
    return 0;
}

/* The uncompressed formats, one pixel at a time. palette: 256 BGRA entries for P8 (from a DDSM file), or NULL. */
static int decode_rows(const uint8_t *src, size_t bytes, uint32_t width, uint32_t height, uint32_t format,
                       const uint8_t *palette, uint8_t *out)
{
    unsigned pixel_bytes = format == 6 ? 4 : (format == 5 || format == 2 || format == 4) ? 2 : 1;
    size_t i, count = (size_t)width * height;

    if (count * pixel_bytes > bytes)
        return -1;
    for (i = 0; i < count; i++) {
        const uint8_t *p = src + i * pixel_bytes;
        uint32_t v = pixel_bytes == 4 ? (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
                                        (uint32_t)p[3] << 24
                   : pixel_bytes == 2 ? (uint32_t)p[0] | (uint32_t)p[1] << 8 : p[0];
        uint8_t *o = out + i * 4;

        switch (format) {
        case 6:     put(o, (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v, (uint8_t)(v >> 24)); break;
        case 5:     put(o, widen5(v >> 11), widen6((v >> 5) & 0x3F), widen5(v & 0x1F), 255); break;
        case 2:     put(o, widen5((v >> 10) & 0x1F), widen5((v >> 5) & 0x1F), widen5(v & 0x1F),
                        (v & 0x8000) ? 255 : 0); break;
        case 4:     put(o, widen4((v >> 8) & 0xF), widen4((v >> 4) & 0xF), widen4(v & 0xF), widen4(v >> 12)); break;
        case 0:     put(o, (uint8_t)v, (uint8_t)v, (uint8_t)v, 255); break;
        case 0x19:  put(o, 255, 255, 255, (uint8_t)v); break;
        case 1:     put(o, widen4(v & 0xF), widen4(v & 0xF), widen4(v & 0xF), widen4(v >> 4)); break;
        case 0x0B:
            if (palette)
                memcpy(o, palette + v * 4, 4);
            else
                put(o, (uint8_t)v, (uint8_t)v, (uint8_t)v, 255);
            break;
        default:
            return -1;
        }
    }
    return 0;
}

/* How many bytes one level of width x height takes in the file. */
static size_t level_bytes(uint32_t width, uint32_t height, uint32_t format)
{
    if (format == 0x0C || format == 0x0E || format == 0x0F)
        return (size_t)((width + 3) / 4) * ((height + 3) / 4) * (format == 0x0C ? 8 : 16);
    return (size_t)width * height * (format == 6 ? 4 : (format == 5 || format == 2 || format == 4) ? 2 : 1);
}

/* One level of the file, from src (bytes long), into out (width x height BGRA); 0 when it fit. */
static int decode_level(const uint8_t *src, size_t bytes, uint32_t width, uint32_t height, uint32_t format,
                        uint8_t *out)
{
    switch (format) {
    case 0x0C:  return decode_dxt(src, bytes, width, height, 0, out);
    case 0x0E:  return decode_dxt(src, bytes, width, height, 1, out);
    case 0x0F:  return decode_dxt(src, bytes, width, height, 2, out);
    default:    return decode_rows(src, bytes, width, height, format, NULL, out);
    }
}

/* A cut-out's smaller levels keep level 0's share of texels at alpha 128 or over: each level's alpha is scaled so
 * that the same share passes the cut (CoD3's KeepCoverage in ngl_side.cpp, the same rule). */
static void keep_coverage(struct ngl_loaded_texture *t)
{
    size_t count = (size_t)t->width * t->height, passing = 0, i;
    double share;
    uint32_t level;

    for (i = 0; i < count; i++)
        passing += t->pixels[i * 4 + 3] >= 128;
    if (!passing)
        return;
    share = (double)passing / (double)count;
    for (level = 1; level < t->level_count; level++) {
        uint32_t w = t->width >> level ? t->width >> level : 1, h = t->height >> level ? t->height >> level : 1;
        size_t texels = (size_t)w * h, keep, seen = 0, histogram[256] = { 0 };
        uint8_t *p = t->levels[level];
        int cut = 255;

        keep = (size_t)(share * (double)texels + 0.5);
        if (keep < 1)
            keep = 1;
        for (i = 0; i < texels; i++)
            histogram[p[i * 4 + 3]]++;
        for (; cut > 0; cut--) {            /* the keep-th largest alpha */
            seen += histogram[cut];
            if (seen >= keep)
                break;
        }
        if (cut == 0)
            continue;
        for (i = 0; i < texels; i++) {
            double a = p[i * 4 + 3] * 128.0 / cut + 0.5;

            p[i * 4 + 3] = a >= 255.0 ? 255 : (uint8_t)a;
        }
    }
}

/* The file's levels after level 0, decoded into t->levels while they fit in the file's bytes after its header. */
static void decode_smaller_levels(struct ngl_loaded_texture *t, const uint8_t *pixels, size_t bytes, uint32_t format,
                                  uint32_t file_levels)
{
    size_t at = level_bytes(t->width, t->height, format);
    uint32_t level, binary_alpha = 0;
    size_t i, count = (size_t)t->width * t->height;

    t->levels[0] = t->pixels;
    t->level_count = 1;
    for (level = 1; level < file_levels && level < NGL_MAX_LEVELS; level++) {
        uint32_t w = t->width >> level ? t->width >> level : 1, h = t->height >> level ? t->height >> level : 1;
        size_t need = level_bytes(w, h, format);
        uint8_t *out;

        if (at + need > bytes || !(out = malloc((size_t)w * h * 4)))
            break;
        if (decode_level(pixels + at, bytes - at, w, h, format, out)) {
            free(out);
            break;
        }
        t->levels[level] = out;
        t->level_count++;
        at += need;
        if (w == 1 && h == 1)
            break;
    }
    if (format == 0x0C || format == 2)      /* one-bit alpha: see-through texels mean a cut-out */
        for (i = 0; i < count && !binary_alpha; i++)
            binary_alpha = t->pixels[i * 4 + 3] == 0;
    if (binary_alpha && t->level_count > 1)
        keep_coverage(t);
}

/* nglLoadDDS's format number for the file's pixel format, or -1 for one it doesn't take. */
static int dds_format(const uint8_t *dds)
{
    uint32_t pf_flags = dds_word(dds, 0x14), fourcc = dds_word(dds, 0x15), bits = dds_word(dds, 0x16);

    if (fourcc == DDS_FOURCC('D', 'X', 'T', '1'))
        return 0x0C;
    if (fourcc == DDS_FOURCC('D', 'X', 'T', '2') || fourcc == DDS_FOURCC('D', 'X', 'T', '3'))
        return 0x0E;
    if (fourcc == DDS_FOURCC('D', 'X', 'T', '4') || fourcc == DDS_FOURCC('D', 'X', 'T', '5'))
        return 0x0F;
    if (pf_flags == 0x41 && bits == 32 && dds_word(dds, 0x1A) == 0xFF000000u)
        return 6;
    if (pf_flags == 0x40 && bits == 16 && dds_word(dds, 0x18) == 0x7E0)
        return 5;
    if (pf_flags == 0x41 && bits == 16 && dds_word(dds, 0x1A) == 0x8000)
        return 2;
    if (pf_flags == 0x41 && bits == 16 && dds_word(dds, 0x1A) == 0xF000)
        return 4;
    if (bits == 8)
        return pf_flags == 0x20000 ? 0 : pf_flags == 2 ? 0x19 : pf_flags == 0x20001 ? 1 : 0x0B;
    return -1;
}

static void dump(unsigned n, const struct ngl_loaded_texture *t, const uint8_t *name, int format)
{
    const char *folder = getenv("BRO_TEX_DUMP_DIR");
    char path[1024], printable[NAME_BYTES + 1];
    FILE *out;
    uint32_t i;

    for (i = 0; i < NAME_BYTES; i++)
        printable[i] = name[i] >= 0x20 && name[i] < 0x7F ? (char)name[i] : '.';
    printable[NAME_BYTES] = '\0';
    fprintf(stderr, "[ngl] texture %u: 0x%08X \"%s\" %ux%u format 0x%02X, %u levels\n", n, t->texture, printable,
            t->width, t->height, format, t->level_count);
    if (!folder || !t->pixels)
        return;
    snprintf(path, sizeof path, "%s/%u.ppm", folder, n);
    out = fopen(path, "wb");
    if (!out)
        return;
    fprintf(out, "P6\n%u %u\n255\n", t->width, t->height);
    for (i = 0; i < t->width * t->height; i++) {
        const uint8_t rgb[3] = { t->pixels[i * 4 + 2], t->pixels[i * 4 + 1], t->pixels[i * 4] };

        fwrite(rgb, 1, 3, out);
    }
    fclose(out);
}

static uint32_t default_texture(void)
{
    return MEM32(NGL_DEFAULT_TEXTURE);
}

/* A new nglTexture for name (counted once, in use), kept in textures; NULL if there's no room. */
static struct ngl_loaded_texture *new_texture(const uint8_t key[NAME_BYTES], uint32_t type)
{
    struct ngl_loaded_texture *t;
    uint32_t texture;

    if (texture_count == MAX_TEXTURES || !(texture = xbox_HeapAlloc(TEXTURE_BYTES, 16)))
        return NULL;
    memset((void *)XBOX_PTR(texture), 0, TEXTURE_BYTES);
    MEM32(texture) = 1;
    MEM32(texture + TEXTURE_TYPE) = type;
    memcpy((void *)XBOX_PTR(texture + TEXTURE_NAME), key, NAME_BYTES);
    MEM32(texture + TEXTURE_USE) |= 1;
    t = &textures[texture_count];
    memset(t, 0, sizeof *t);
    memcpy(names[texture_count], key, NAME_BYTES);
    t->texture = texture;
    texture_count++;
    return t;
}

/* An animated texture list (type 3): its frames' names, read from the text the way nglLoadAnimatedTextureList does. */
static uint32_t load_list(const uint8_t key[NAME_BYTES], uint32_t data, uint32_t size)
{
    const char *text = (const char *)XBOX_PTR(data), *end = text + size;
    struct ngl_loaded_texture *t;
    unsigned frames = 0;

    if (list_count == MAX_LISTS || !data)
        return default_texture();
    while (text < end && frames < MAX_FRAMES) {
        const char *line_end = memchr(text, '\n', (size_t)(end - text));
        char frame[NAME_BYTES] = "";
        unsigned length = 0, count = 1;

        if (!line_end)
            line_end = end;
        while (text < line_end && (*text == ' ' || *text == '\t' || *text == '\r'))
            text++;
        while (text < line_end && *text != ' ' && *text != '\t' && *text != '\r' && *text != '.' &&
               length < NAME_BYTES - NAME_HASH_BYTES - 1) {
            char ch = *text++;

            frame[length++] = ch >= 'A' && ch <= 'Z' ? (char)(ch - 'A' + 'a') : ch;
        }
        while (text < line_end && *text != ' ' && *text != '\t')
            text++;
        if (text < line_end) {
            int n = atoi(text + 1);

            count = n > 0 ? (unsigned)n : 1;
        }
        for (; length && count && frames < MAX_FRAMES; count--)
            memcpy(lists[list_count].frames[frames++], frame, NAME_BYTES);
        text = line_end + 1;
    }
    if (!frames || !(t = new_texture(key, TYPE_ANIMATED_LIST)))
        return default_texture();
    lists[list_count].texture = t->texture;
    lists[list_count].frame_count = frames;
    list_count++;
    fprintf(stderr, "[ngl] animated texture \"%.28s\": %u frames, the first \"%s\"\n",
            (const char *)key + NAME_HASH_BYTES, frames, lists[list_count - 1].frames[0]);
    return t->texture;
}

uint32_t ngl_texture_load(uint32_t name, uint32_t type, uint32_t data, uint32_t size)
{
    static int reported;
    uint8_t key[NAME_BYTES];
    const uint8_t *dds;
    struct ngl_loaded_texture *t;
    uint32_t texture, width, height, levels, i;
    int format;
    size_t pixel_bytes;

    if (!name)
        return default_texture();
    memcpy(key, (const void *)XBOX_PTR(name), NAME_BYTES);
    for (i = 0; i < texture_count; i++) {
        if (!memcmp(names[i], key, NAME_BYTES)) {
            /* [rexglue-native] 2026-11-11: the name again with another file (see file_hashes): the old texture
             * gives up the name (it stays as it is for whatever still draws it) and a new one is made below */
            if ((type == 0 || type == 1) && data && size >= DDS_HEADER_BYTES && file_sizes[i] &&
                (file_sizes[i] != size ||
                 file_hashes[i] != hash_bytes((const uint8_t *)XBOX_PTR(data), size))) {
                static int renamed;

                if (renamed++ < 16)
                    fprintf(stderr, "[ngl] nglLoadTexture: \"%.28s\" again with another file (%u bytes, was %u): "
                            "a new texture\n", (const char *)key + NAME_HASH_BYTES, size, file_sizes[i]);
                memset(names[i], 0, NAME_BYTES);
                break;
            }
            MEM32(textures[i].texture) += 1;
            MEM32(textures[i].texture + TEXTURE_USE) |= 1;
            return textures[i].texture;
        }
    }
    if (type == TYPE_ANIMATED_LIST)
        return load_list(key, data, size);
    if ((type != 0 && type != 1) || !data || size < DDS_HEADER_BYTES || texture_count == MAX_TEXTURES) {
        if (reported++ < 8)
            fprintf(stderr, "[ngl] nglLoadTexture: type %u, %u bytes at 0x%08X: not loaded\n", type, size, data);
        return default_texture();
    }
    dds = (const uint8_t *)XBOX_PTR(data);
    width = dds_word(dds, 4);
    height = dds_word(dds, 3);
    levels = dds_word(dds, 7);
    format = dds_format(dds);
    if (dds_word(dds, 0) != DDS_MAGIC || dds_word(dds, 1) != 0x7C || format < 0 || !width || !height ||
        width > MAX_SIDE || height > MAX_SIDE || (dds_word(dds, 0x1C) & 0xFE00u)) {
        if (reported++ < 8)
            fprintf(stderr, "[ngl] nglLoadTexture: a DDS file this can't decode (magic %08X, %ux%u, format %d, "
                    "caps2 %08X)\n", dds_word(dds, 0), width, height, format, dds_word(dds, 0x1C));
        return default_texture();
    }

    if (!(t = new_texture(key, type)))
        return default_texture();
    file_sizes[texture_count - 1] = size;       /* [rexglue-native] 2026-11-11 (see file_hashes) */
    file_hashes[texture_count - 1] = hash_bytes(dds, size);
    texture = t->texture;
    MEM32(texture + TEXTURE_WIDTH) = width;
    MEM32(texture + TEXTURE_HEIGHT) = height;
    MEM32(texture + TEXTURE_LEVELS) = levels ? levels : 1;
    MEM32(texture + TEXTURE_FORMAT) = (uint32_t)format;
    MEM32(texture + TEXTURE_FLAGS) |= 0x100;
    t->width = width;
    t->height = height;
    t->pixels = malloc((size_t)width * height * 4);
    pixel_bytes = size - DDS_HEADER_BYTES;
    if (t->pixels) {
        if (decode_level(dds + DDS_HEADER_BYTES, pixel_bytes, width, height, (uint32_t)format, t->pixels)) {
            fprintf(stderr, "[ngl] nglLoadTexture: %ux%u format 0x%02X doesn't fit in %u bytes\n", width, height,
                    format, size);
            free(t->pixels);
            t->pixels = NULL;
        } else {
            decode_smaller_levels(t, dds + DDS_HEADER_BYTES, pixel_bytes, (uint32_t)format, levels);
        }
    }
    dump(texture_count, t, key, format);
    return texture;
}

/* A list's frame, once a texture of that name is loaded. */
static const struct ngl_loaded_texture *list_frame(unsigned list, unsigned frame)
{
    if (!lists[list].found[frame]) {
        for (unsigned i = 0; i < texture_count; i++) {
            if (!strncmp((const char *)names[i] + NAME_HASH_BYTES, lists[list].frames[frame],
                         NAME_BYTES - NAME_HASH_BYTES) && textures[i].pixels) {
                lists[list].found[frame] = (int)i + 1;
                if (frame == 0) {       /* the list's size is its first frame's, as the original sets it */
                    MEM32(lists[list].texture + TEXTURE_WIDTH) = textures[i].width;
                    MEM32(lists[list].texture + TEXTURE_HEIGHT) = textures[i].height;
                }
                break;
            }
        }
    }
    return lists[list].found[frame] ? &textures[lists[list].found[frame] - 1] : NULL;
}

unsigned ngl_texture_frame_count(uint32_t texture)
{
    for (unsigned i = 0; i < list_count; i++)
        if (lists[i].texture == texture)
            return lists[i].frame_count;
    return 0;
}

uint32_t ngl_texture_list_frame(uint32_t texture, unsigned frame)
{
    for (unsigned i = 0; i < list_count; i++) {
        if (lists[i].texture == texture && lists[i].frame_count) {
            const struct ngl_loaded_texture *t = list_frame(i, frame % lists[i].frame_count);

            return t ? t->texture : 0;
        }
    }
    return 0;
}

const struct ngl_loaded_texture *ngl_texture_loaded(uint32_t texture)
{
    unsigned i;

    for (i = 0; i < list_count; i++)
        if (lists[i].texture == texture)
            return list_frame(i, 0);
    for (i = 0; i < texture_count; i++)
        if (textures[i].texture == texture)
            return textures[i].pixels ? &textures[i] : NULL;
    return NULL;
}

/* Says once for each name (the first 64) that the game asked for a texture nothing loaded. */
static void report_missing(const uint8_t *name)
{
    static uint8_t reported[64][NAME_BYTES];
    static unsigned reported_count;
    char text[NAME_BYTES - NAME_HASH_BYTES + 1];

    for (unsigned i = 0; i < reported_count; i++)
        if (!memcmp(reported[i], name, NAME_BYTES))
            return;
    if (reported_count == 64)
        return;
    memcpy(reported[reported_count++], name, NAME_BYTES);
    for (unsigned i = 0; i < NAME_BYTES - NAME_HASH_BYTES; i++) {
        char ch = (char)name[NAME_HASH_BYTES + i];

        text[i] = ch == 0 || (ch >= 32 && ch <= 126) ? ch : '.';
    }
    text[NAME_BYTES - NAME_HASH_BYTES] = 0;
    fprintf(stderr, "[ngl] texture \"%s\" asked for, but none of that name is loaded\n", text);
}

uint32_t ngl_texture_register_shader(uint32_t path)
{
    static int reported;
    const char *p = (const char *)XBOX_PTR(path);
    char name[NAME_BYTES] = "";
    unsigned length = 0, start = 0, i;
    uint8_t key[NAME_BYTES] = { 0 };
    struct ngl_loaded_texture *t;

    if (!path)
        return 0;
    for (i = 0; p[i] && p[i] != '.' && i < 63; i++)         /* the path up to its extension */
        if (p[i] == '@' || p[i] == '$' || p[i] == '/')
            start = i + 1;
    for (; start < i && length < NAME_BYTES - NAME_HASH_BYTES - 1; start++)
        name[length++] = p[start] >= 'A' && p[start] <= 'Z' ? (char)(p[start] - 'A' + 'a') : p[start];
    if (!length || !strcmp(name, "nglwhite") || !strcmp(name, "white"))     /* white: drawn as a plain color */
        return 0;
    for (i = 0; i < texture_count; i++)
        if (!strncmp((const char *)names[i] + NAME_HASH_BYTES, name, NAME_BYTES - NAME_HASH_BYTES) &&
            textures[i].pixels)
            return textures[i].texture;
    for (i = 0; i < list_count; i++)                        /* asked for before */
        if (lists[i].frame_count == 1 && !strcmp(lists[i].frames[0], name))
            return lists[i].texture;
    memcpy(key + NAME_HASH_BYTES, name, length);
    if (list_count == MAX_LISTS || !(t = new_texture(key, TYPE_ANIMATED_LIST)))
        return 0;
    memcpy(lists[list_count].frames[0], name, NAME_BYTES);
    lists[list_count].texture = t->texture;
    lists[list_count].frame_count = 1;
    list_count++;
    if (reported++ < 32)
        fprintf(stderr, "[ngl] RE_RegisterShader \"%.63s\": \"%s\" isn't loaded yet; drawn once it is\n", p, name);
    return t->texture;
}

uint32_t ngl_texture_lookup(uint32_t name)
{
    unsigned i;

    if (!name)
        return 0;
    for (i = 0; i < texture_count; i++)
        if (!memcmp(names[i], (const void *)XBOX_PTR(name), NAME_BYTES))
            return textures[i].texture;
    report_missing((const uint8_t *)XBOX_PTR(name));
    return 0;
}

int ngl_texture_named(uint32_t texture, const char *name)
{
    unsigned i;

    if (!texture || !name)
        return 0;
    for (i = 0; i < texture_count; i++)
        if (textures[i].texture == texture)
            return !strncmp((const char *)XBOX_PTR(texture + TEXTURE_NAME + NAME_HASH_BYTES), name,
                            NAME_BYTES - NAME_HASH_BYTES);
    return 0;
}

uint32_t ngl_texture_find(uint32_t name)
{
    uint32_t texture = ngl_texture_lookup(name);

    if (texture)
        MEM32(texture) += 1;
    return texture;
}
