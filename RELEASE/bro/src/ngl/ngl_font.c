/* [acct2-bro] Big Red One port: NGL's fonts and text, natively.
 *
 * The game draws all its text through NGL: nglLoadFont (0x000658F0) makes a font from a small text file and the
 * texture of the same name, nglListAddText / nglListAddTextf (0x000661C0 / 0x00066490) queue a string for the frame,
 * and nglGetTextExtent* (0x00065F00, 0x000662C0, 0x00066310) measure one, which the menus use to place their text. On
 * the Xbox the string becomes a render node that nglRenderTextNode (0x00079FC0) turns into one textured quad a glyph
 * (laid out by nglBuildTextGlyphs 0x00065B60). Those are stand-ins here, so this does the same natively: it parses the
 * font file the way nglParseFontFile (0x00065590) does and lays the glyphs out the way nglBuildTextGlyphs does, as
 * quads for ngl_vk.
 *
 * The font file: "version" (hex), "cellheight", "ascent", "firstglyph", "numglyphs", then for each glyph its code
 * and "x", "y", "w" (the advance), "gx", "gy" (offsets), "gw", "gh" (size). nglFontParseKeyInt finds the next
 * occurrence of the key and reads the number right after it (strtol), which this copies exactly.
 *
 * The font object the game gets (0x54 bytes, as nglLoadFontFile makes it): the name +0x00 (32 bytes), use count
 * +0x20, texture +0x24, glyph table +0x28 (0x1C bytes a glyph: x, y, gw, gh, gx, gy, w), version +0x2C, cell height
 * +0x30, ascent +0x34, first glyph +0x38, glyph count +0x3C, flags +0x40, blend +0x44 / +0x48 (2, 2), and the
 * glyphs' texture coordinates +0x4C (u0, v0, u1, v1 each).
 *
 * In a string, byte 1 starts a color ("\1[RRGGBBAA]"-style: one byte, hex, one byte), 2 a scale (one number), 3 a
 * separate x and y scale (two numbers); 9 is a tab (four spaces), 10 a new line (the cell height times the line's
 * largest y scale down, back to the start x), 32 a space (the space glyph's advance). */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "recomp_types.h"
#include "xbox_memory_layout.h"     /* xbox_HeapAlloc, XBOX_PTR: guest memory */
#include "ngl_font.h"
#include "ngl_quad.h"
#include "ngl_texture_load.h"
#include "bro_pad.h"                /* bro_pad_text_drawn: the pad script's waits for a screen */

#define FONT_BYTES          0x54u
#define FONT_NAME_BYTES     0x20u
#define FONT_USE            0x20u
#define FONT_TEXTURE        0x24u
#define FONT_GLYPHS         0x28u
#define FONT_VERSION        0x2Cu
#define FONT_CELL_HEIGHT    0x30u
#define FONT_ASCENT         0x34u
#define FONT_FIRST          0x38u
#define FONT_COUNT          0x3Cu
#define FONT_FLAGS          0x40u
#define FONT_BLEND          0x44u
#define FONT_BLEND_ARG      0x48u
#define FONT_UVS            0x4Cu
#define GLYPH_BYTES         0x1Cu

#define TEXTURE_WIDTH       0x10u
#define TEXTURE_HEIGHT      0x14u

#define MAX_FONTS           64
#define MAX_FONT_FILE       (256u * 1024u)
#define MAX_TEXT            1024        /* the game's own vsprintf buffers are this size */
#define TEXTS_SHOWN         64          /* different strings written to the log, the first ones queued */

#define CODE_COLOR          1
#define CODE_SCALE          2
#define CODE_SCALE_XY       3

struct glyph {
    int x, y, w, h;             /* in the texture, pixels */
    int x_offset, y_offset;     /* from the pen */
    int advance;
    float u0, v0, u1, v1;
};

struct font {
    uint32_t guest;             /* the font object the game holds */
    uint8_t name[FONT_NAME_BYTES];
    int cell_height, ascent, first, count;
    struct glyph *glyphs;
    int uvs_done;               /* the texture coordinates need the texture's size */
};

static struct font fonts[MAX_FONTS];
static int texts_shown;
static char last_shown[MAX_TEXT];      /* a string repeated each frame is shown once */
static int font_count;

static float as_float(uint32_t bits)
{
    float value;

    memcpy(&value, &bits, sizeof value);
    return value;
}

static uint32_t float_bits(float value)
{
    uint32_t bits;

    memcpy(&bits, &value, sizeof bits);
    return bits;
}

/* nglFontParseKeyInt: past the next occurrence of key, the number there (0 when there is none). */
static long parse_key_int(const char **at, const char *key, int base)
{
    size_t length = strlen(key);
    const char *p = *at;

    while (*p) {
        if (!strncmp(p, key, length)) {
            char *end;
            long value = strtol(p + length, &end, base);

            *at = end;
            return value;
        }
        p++;
    }
    *at = p;
    return 0;
}

static struct font *font_for(uint32_t guest)
{
    int i;

    for (i = 0; i < font_count; i++)
        if (fonts[i].guest == guest)
            return &fonts[i];
    return NULL;
}

/* The font's texture (nglFindTextureAddRef by the font's name), and the glyphs' texture coordinates once its size is
 * known. The texture can be loaded after the font, so this is tried again when the font is drawn. */
static uint32_t font_texture(struct font *f)
{
    uint32_t texture = MEM32(f->guest + FONT_TEXTURE);
    float width, height;
    int i;

    if (!texture) {
        texture = ngl_texture_find(f->guest);
        if (!texture)
            return 0;
        MEM32(f->guest + FONT_TEXTURE) = texture;
    }
    if (!f->uvs_done) {
        uint32_t uvs = MEM32(f->guest + FONT_UVS);

        width = (float)MEM32(texture + TEXTURE_WIDTH);
        height = (float)MEM32(texture + TEXTURE_HEIGHT);
        if (width <= 0 || height <= 0)
            return 0;
        for (i = 0; i < f->count; i++) {
            struct glyph *g = &f->glyphs[i];

            g->u0 = (float)g->x / width;
            g->v0 = (float)g->y / height;
            g->u1 = (float)g->w / width + g->u0;
            g->v1 = (float)g->h / height + g->v0;
            if (uvs) {
                MEM32(uvs + (uint32_t)i * 16) = float_bits(g->u0);
                MEM32(uvs + (uint32_t)i * 16 + 4) = float_bits(g->v0);
                MEM32(uvs + (uint32_t)i * 16 + 8) = float_bits(g->u1);
                MEM32(uvs + (uint32_t)i * 16 + 12) = float_bits(g->v1);
            }
        }
        f->uvs_done = 1;
    }
    return texture;
}

uint32_t ngl_font_find(uint32_t name)
{
    int i;

    if (!name)
        return 0;
    for (i = 0; i < font_count; i++) {
        if (!memcmp(fonts[i].name, (const void *)XBOX_PTR(name), FONT_NAME_BYTES)) {
            MEM32(fonts[i].guest + FONT_USE) += 1;
            return fonts[i].guest;
        }
    }
    return 0;
}

uint32_t ngl_font_load(uint32_t name, uint32_t data)
{
    struct font *f;
    const char *text, *at;
    char *copy;
    size_t length;
    uint32_t guest, table, found;
    int i;

    found = ngl_font_find(name);
    if (found)
        return found;
    if (!name || !data || font_count == MAX_FONTS)
        return 0;
    text = (const char *)XBOX_PTR(data);
    length = strnlen(text, MAX_FONT_FILE);
    copy = malloc(length + 1);
    guest = xbox_HeapAlloc(FONT_BYTES, 16);
    if (!copy || !guest) {
        free(copy);
        return 0;
    }
    memcpy(copy, text, length);
    copy[length] = '\0';

    f = &fonts[font_count];
    memset(f, 0, sizeof *f);
    memcpy(f->name, (const void *)XBOX_PTR(name), FONT_NAME_BYTES);
    f->guest = guest;
    memset((void *)XBOX_PTR(guest), 0, FONT_BYTES);
    memcpy((void *)XBOX_PTR(guest), f->name, FONT_NAME_BYTES);
    MEM32(guest + FONT_USE) = 1;
    MEM32(guest + FONT_BLEND) = 2;
    MEM32(guest + FONT_BLEND_ARG) = 2;

    at = copy;
    MEM32(guest + FONT_VERSION) = (uint32_t)parse_key_int(&at, "version", 16);
    f->cell_height = (int)parse_key_int(&at, "cellheight", 10);
    f->ascent = (int)parse_key_int(&at, "ascent", 10);
    f->first = (int)parse_key_int(&at, "firstglyph", 10);
    f->count = (int)parse_key_int(&at, "numglyphs", 10);
    if (f->count <= 0 || f->count > 0x10000) {
        fprintf(stderr, "[ngl] nglLoadFont: a font file with %d glyphs; not loaded\n", f->count);
        free(copy);
        return 0;
    }
    f->glyphs = calloc((size_t)f->count, sizeof *f->glyphs);
    table = xbox_HeapAlloc((uint32_t)f->count * GLYPH_BYTES, 16);
    MEM32(guest + FONT_UVS) = xbox_HeapAlloc((uint32_t)f->count * 16, 16);
    if (!f->glyphs || !table) {
        free(f->glyphs);
        free(copy);
        return 0;
    }
    for (i = 0; i < f->count; i++) {
        struct glyph g;
        long code;
        char *end;
        uint32_t entry;

        while (*at && (*at < '0' || *at > '9'))
            at++;
        code = strtol(at, &end, 10);
        at = end;
        g.x = (int)parse_key_int(&at, "x", 10);
        g.y = (int)parse_key_int(&at, "y", 10);
        g.advance = (int)parse_key_int(&at, "w", 10);
        g.x_offset = (int)parse_key_int(&at, "gx", 10);
        g.y_offset = (int)parse_key_int(&at, "gy", 10);
        g.w = (int)parse_key_int(&at, "gw", 10);
        g.h = (int)parse_key_int(&at, "gh", 10);
        g.u0 = g.v0 = g.u1 = g.v1 = 0;
        if (code - f->first < 0 || code - f->first >= f->count)
            continue;
        f->glyphs[code - f->first] = g;
        entry = table + (uint32_t)(code - f->first) * GLYPH_BYTES;
        MEM32(entry) = (uint32_t)g.x;
        MEM32(entry + 4) = (uint32_t)g.y;
        MEM32(entry + 8) = (uint32_t)g.w;
        MEM32(entry + 12) = (uint32_t)g.h;
        MEM32(entry + 16) = (uint32_t)g.x_offset;
        MEM32(entry + 20) = (uint32_t)g.y_offset;
        MEM32(entry + 24) = (uint32_t)g.advance;
    }
    free(copy);
    MEM32(guest + FONT_GLYPHS) = table;
    MEM32(guest + FONT_CELL_HEIGHT) = (uint32_t)f->cell_height;
    MEM32(guest + FONT_ASCENT) = (uint32_t)f->ascent;
    MEM32(guest + FONT_FIRST) = (uint32_t)f->first;
    MEM32(guest + FONT_COUNT) = (uint32_t)f->count;
    font_count++;
    font_texture(f);
    fprintf(stderr, "[ngl] font %d: 0x%08X \"%.24s\" %d glyphs from %d, cell height %d, texture 0x%08X\n",
            font_count, guest, (const char *)f->name + 4, f->count, f->first, f->cell_height,
            MEM32(guest + FONT_TEXTURE));
    return guest;
}

static const struct glyph *glyph_for(const struct font *f, unsigned char c)
{
    int index = (int)c - f->first;

    return index >= 0 && index < f->count ? &f->glyphs[index] : NULL;
}

static float space_advance(const struct font *f)
{
    const struct glyph *g = glyph_for(f, ' ');

    return g ? (float)g->advance : 0.0f;
}

/* Reads a control code's argument the way nglBuildTextGlyphs does: one byte, the value, one byte. */
static const char *skip_open(const char *p)
{
    return *p ? p + 1 : p;
}

static const char *skip_close(const char *p)
{
    return *p ? p + 1 : p;
}

/* Lays out text as nglBuildTextGlyphs does; quads when draw, else only the extent. */
static void lay_out(struct font *f, const char *text, float x, float y, float z, float scale_x, float scale_y,
                    uint32_t color, int draw, float *width, float *height)
{
    float start_x = x, line_scale = scale_y, widest = 0, top = y, line_end = x;
    uint32_t texture = draw ? font_texture(f) : 0;
    const char *p = text;

    if (draw && !texture)
        return;
    while (*p) {
        unsigned char c = (unsigned char)*p++;
        char *end;

        switch (c) {
        case CODE_COLOR: {
            unsigned long rgba;

            p = skip_open(p);
            rgba = strtoul(p, &end, 16);
            p = skip_close(end);
            color = (uint32_t)(rgba << 24 | rgba >> 8);
            break;
        }
        case CODE_SCALE:
            p = skip_open(p);
            scale_x = scale_y = (float)strtod(p, &end);
            p = skip_close(end);
            if (line_scale < scale_y)
                line_scale = scale_y;
            break;
        case CODE_SCALE_XY:
            p = skip_open(p);
            scale_x = (float)strtod(p, &end);
            p = skip_close(end);
            scale_y = (float)strtod(p, &end);
            p = skip_close(end);
            if (line_scale < scale_y)
                line_scale = scale_y;
            break;
        case '\t':
            x += space_advance(f) * scale_x * 4.0f;
            break;
        case '\n':
            if (line_end - start_x > widest)
                widest = line_end - start_x;
            y += (float)f->cell_height * line_scale;
            x = line_end = start_x;
            line_scale = scale_y;
            break;
        case ' ':
            x += space_advance(f) * scale_x;
            break;
        default: {
            const struct glyph *g = glyph_for(f, c);

            if (!g)
                break;
            if (draw && g->w > 0 && g->h > 0) {
                struct ngl_quad q;
                float x0 = (float)g->x_offset * scale_x + x, y0 = (float)g->y_offset * scale_y + y;
                float x1 = (float)g->w * scale_x + x0, y1 = (float)g->h * scale_y + y0;
                int i;

                memset(&q, 0, sizeof q);
                q.x[0] = x0; q.y[0] = y0; q.u[0] = g->u0; q.v[0] = g->v0;
                q.x[1] = x1; q.y[1] = y0; q.u[1] = g->u1; q.v[1] = g->v0;
                q.x[2] = x0; q.y[2] = y1; q.u[2] = g->u0; q.v[2] = g->v1;
                q.x[3] = x1; q.y[3] = y1; q.u[3] = g->u1; q.v[3] = g->v1;
                for (i = 0; i < 4; i++)
                    q.color[i] = color;
                q.z = z;
                q.blend = MEM32(f->guest + FONT_BLEND);
                q.blend_arg = MEM32(f->guest + FONT_BLEND_ARG);
                q.texture = texture;
                ngl_quad_add_host(&q);
            }
            x += (float)g->advance * scale_x;
            /* nglGetTextExtent measures a line to its last glyph's right edge, not to the pen */
            line_end = (float)(g->x_offset - g->advance + g->w) * scale_x + x;
            break;
        }
        }
    }
    if (line_end - start_x > widest)
        widest = line_end - start_x;
    if (width)
        *width = widest;
    if (height)
        *height = y - top + (float)f->cell_height * line_scale;
}

/* The game's vsprintf, for the *f functions: format and arguments in guest memory, cdecl (doubles take 8 bytes). */
static void guest_format(char *out, size_t size, uint32_t format, uint32_t args)
{
    const char *f = (const char *)XBOX_PTR(format);
    size_t used = 0;

    if (!format) {
        out[0] = '\0';
        return;
    }
    while (*f && used + 1 < size) {
        char spec[32], piece[MAX_TEXT];
        size_t n = 0;
        int written = 0;

        if (*f != '%') {
            out[used++] = *f++;
            continue;
        }
        spec[n++] = *f++;
        while (*f && strchr("-+ #0123456789.", *f) && n < sizeof spec - 4)
            spec[n++] = *f++;
        while (*f == 'l' || *f == 'h' || *f == 'I' || *f == '6' || *f == '4')
            f++;                                /* sizes: all 32-bit here but %f */
        if (!*f)
            break;
        spec[n++] = *f;
        spec[n] = '\0';
        switch (*f++) {
        case 'd': case 'i': case 'c':
            written = snprintf(piece, sizeof piece, spec, (int)MEM32(args));
            args += 4;
            break;
        case 'u': case 'x': case 'X': case 'o':
            written = snprintf(piece, sizeof piece, spec, (unsigned)MEM32(args));
            args += 4;
            break;
        case 's': {
            uint32_t s = MEM32(args);

            written = snprintf(piece, sizeof piece, spec, s ? (const char *)XBOX_PTR(s) : "(null)");
            args += 4;
            break;
        }
        case 'f': case 'F': case 'g': case 'G': case 'e': case 'E': {
            uint64_t bits = (uint64_t)MEM32(args) | (uint64_t)MEM32(args + 4) << 32;
            double value;

            memcpy(&value, &bits, sizeof value);
            written = snprintf(piece, sizeof piece, spec, value);
            args += 8;
            break;
        }
        case '%':
            written = snprintf(piece, sizeof piece, "%%");
            break;
        default:
            written = snprintf(piece, sizeof piece, "%s", spec);
            break;
        }
        if (written > 0) {
            size_t take = (size_t)written < sizeof piece ? (size_t)written : sizeof piece - 1;

            if (take > size - 1 - used)
                take = size - 1 - used;
            memcpy(out + used, piece, take);
            used += take;
        }
    }
    out[used] = '\0';
}

/* [acct2-bro-game] BRO_TEXT_LOG=<file>: the text on the screen, for documenting the menus (dev/agents/acct2-bro-game/
 * MENUS.md) from scripted runs. Each second's strings, without their control codes, top to bottom (then left to
 * right), go into the file as one line ("<seconds>  <y>: <text> #<color> | ..."), when they differ from the second
 * before. The color (the string's own first color code, else the one it is drawn with) shows which item a menu has
 * picked out. The seconds count from the first text drawn. */
#define SCREEN_TEXTS        96
#define SCREEN_TEXT_CHARS   96

struct screen_text {
    float x, y;
    uint32_t color;
    char text[SCREEN_TEXT_CHARS];
};

static struct {
    int state;                  /* 0: BRO_TEXT_LOG not read yet, 1: logging, -1: off */
    FILE *file;
    struct timespec start;
    long second;
    int count, shown_count;
    struct screen_text now[SCREEN_TEXTS], shown[SCREEN_TEXTS];
} screen_log;

/* The text as it shows: the color and scale codes and other control bytes left out. The first color code's color
 * goes into *color; *color is left alone when there is none. */
static void plain_text(const char *text, char *out, size_t size, uint32_t *color)
{
    const char *p = text;
    size_t used = 0;
    int colored = 0;

    while (*p && used + 1 < size) {
        unsigned char c = (unsigned char)*p++;
        char *end;

        if (c == CODE_COLOR) {
            unsigned long code;

            p = skip_open(p);
            code = strtoul(p, &end, 16);
            p = skip_close(end);
            if (!colored) {
                *color = (uint32_t)code;
                colored = 1;
            }
        } else if (c == CODE_SCALE || c == CODE_SCALE_XY) {
            p = skip_open(p);
            strtod(p, &end);
            p = skip_close(end);
            if (c == CODE_SCALE_XY) {
                strtod(p, &end);
                p = skip_close(end);
            }
        } else if (c >= 0x20) {
            out[used++] = (char)c;
        } else if (c == '\n' || c == '\t') {
            out[used++] = ' ';
        }
    }
    out[used] = '\0';
}

static int by_place(const void *a, const void *b)
{
    const struct screen_text *s = a, *t = b;

    if (s->y != t->y)
        return s->y < t->y ? -1 : 1;
    return s->x < t->x ? -1 : s->x > t->x;
}

static void screen_log_second(void)
{
    int i, same = screen_log.count == screen_log.shown_count;

    qsort(screen_log.now, (size_t)screen_log.count, sizeof screen_log.now[0], by_place);
    for (i = 0; same && i < screen_log.count; i++)
        same = !strcmp(screen_log.now[i].text, screen_log.shown[i].text)
               && screen_log.now[i].color == screen_log.shown[i].color;
    if (!same && screen_log.count) {
        fprintf(screen_log.file, "%4ld ", screen_log.second);
        for (i = 0; i < screen_log.count; i++)
            fprintf(screen_log.file, "%s %.0f: %s #%08X", i ? " |" : "", screen_log.now[i].y, screen_log.now[i].text,
                    screen_log.now[i].color);
        fprintf(screen_log.file, "\n");
        fflush(screen_log.file);
    }
    memcpy(screen_log.shown, screen_log.now, sizeof screen_log.now);
    screen_log.shown_count = screen_log.count;
    screen_log.count = 0;
}

static void screen_log_text(const char *text, float x, float y, uint32_t color)
{
    struct timespec now;
    long second;
    char plain[SCREEN_TEXT_CHARS];
    int i;

    if (!screen_log.state) {
        const char *path = getenv("BRO_TEXT_LOG");

        screen_log.file = path && *path ? fopen(path, "w") : NULL;
        screen_log.state = screen_log.file ? 1 : -1;
        clock_gettime(CLOCK_MONOTONIC, &screen_log.start);
        screen_log.second = 0;
    }
    if (screen_log.state < 0)
        return;
    clock_gettime(CLOCK_MONOTONIC, &now);
    second = (long)(now.tv_sec - screen_log.start.tv_sec);
    if (second != screen_log.second) {
        screen_log_second();
        screen_log.second = second;
    }
    plain_text(text, plain, sizeof plain, &color);
    if (!plain[0] || screen_log.count == SCREEN_TEXTS)
        return;
    for (i = 0; i < screen_log.count; i++)
        if (!strcmp(screen_log.now[i].text, plain))
            return;
    screen_log.now[screen_log.count].x = x;
    screen_log.now[screen_log.count].y = y;
    screen_log.now[screen_log.count].color = color;
    memcpy(screen_log.now[screen_log.count].text, plain, sizeof plain);
    screen_log.count++;
}

static void add_text(uint32_t font, const char *text, uint32_t at)
{
    struct font *f = font_for(font);

    /* at: x, y, z, color, scale x, scale y, as the guest passed them */
    if (!f || !text || !*text)
        return;
    screen_log_text(text, as_float(MEM32(at)), as_float(MEM32(at + 4)), MEM32(at + 12));
    if (bro_pad_waiting_for_text()) {
        char plain[SCREEN_TEXT_CHARS];
        uint32_t color = 0;

        plain_text(text, plain, sizeof plain, &color);
        bro_pad_text_drawn(plain);
    }
    if (texts_shown < TEXTS_SHOWN && strcmp(text, last_shown)) {
        const struct ngl_loaded_texture *t = ngl_texture_loaded(font_texture(f));

        texts_shown++;
        snprintf(last_shown, sizeof last_shown, "%s", text);
        fprintf(stderr, "[ngl] text \"%.40s\" font %08X at (%.1f, %.1f) z %.3f color %08X scale %.2f x %.2f texture "
                "%08X %s\n", text, font, as_float(MEM32(at)), as_float(MEM32(at + 4)), as_float(MEM32(at + 8)),
                MEM32(at + 12),
                as_float(MEM32(at + 16)), as_float(MEM32(at + 20)), MEM32(font + FONT_TEXTURE),
                t ? "decoded" : "not decoded");
    }
    lay_out(f, text, as_float(MEM32(at)), as_float(MEM32(at + 4)), as_float(MEM32(at + 8)),
            as_float(MEM32(at + 16)), as_float(MEM32(at + 20)), MEM32(at + 12), 1, NULL, NULL);
}

void ngl_text_add(uint32_t font, uint32_t text, uint32_t args)
{
    if (text)
        add_text(font, (const char *)XBOX_PTR(text), args);
}

void ngl_text_addf(uint32_t args)
{
    char text[MAX_TEXT];

    guest_format(text, sizeof text, MEM32(args + 28), args + 32);
    add_text(MEM32(args), text, args + 4);
}

static void extent(uint32_t font, const char *text, uint32_t width_out, uint32_t height_out, float scale_x,
                   float scale_y)
{
    struct font *f = font_for(font);
    float width = 0, height = 0;

    if (f && text)
        lay_out(f, text, 0, 0, 0, scale_x, scale_y, 0xFFFFFFFFu, 0, &width, &height);
    /* integers, truncated, as the original's _ftol stores them (its callers read them as unsigned ints) */
    if (width_out)
        MEM32(width_out) = (uint32_t)(int32_t)width;
    if (height_out)
        MEM32(height_out) = (uint32_t)(int32_t)height;
}

void ngl_text_extent(uint32_t args)
{
    uint32_t text = MEM32(args + 4);

    extent(MEM32(args), text ? (const char *)XBOX_PTR(text) : NULL, MEM32(args + 8), MEM32(args + 12),
           as_float(MEM32(args + 16)), as_float(MEM32(args + 20)));
}

void ngl_text_extentf(uint32_t args)
{
    char text[MAX_TEXT];

    guest_format(text, sizeof text, MEM32(args + 12), args + 16);
    extent(MEM32(args), text, MEM32(args + 4), MEM32(args + 8), 1.0f, 1.0f);
}

void ngl_text_sizef(uint32_t args)
{
    char text[MAX_TEXT];

    guest_format(text, sizeof text, MEM32(args + 20), args + 24);
    extent(MEM32(args), text, MEM32(args + 4), MEM32(args + 8), as_float(MEM32(args + 12)),
           as_float(MEM32(args + 16)));
}
