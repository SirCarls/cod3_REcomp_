/* [waw-decomp-fh2 r19] 2026-11-24 The game's XFONT text drawn at the picture's own size.
 *
 * Owner 11-24: "Please make sure we are getting all these native renderers to actually render the rquired resolutions
 * without upscaling then downscaling or other things similar." And earlier the same day: "The intros aren't playing
 * right, same with the subtitles."
 *
 * What the game does (read from its code, 2026-11-24):
 *   - It opens d:\media\arialuni.ttf once with the XDK's XFONT_OpenTrueTypeFont (0x001397F4, called at 0x0012A967
 *     with L"d:\\media\\arialuni.ttf" at 0x002EB510 and a 64 KB glyph cache) and keeps the font in a small class
 *     (vtable 0x002EB4FC) whose slots are SetTextHeight (0x0012A910 -> 0x00138E27), SetTextColor (0x0012A8F0 ->
 *     0x00138B22, the color at font+0x28), TextOut (0x0012A890) and GetTextExtent (0x0012A8C0 -> 0x00138E59). Those
 *     are the only XFONT setters linked into the game: alignment (+0x24), background color (+0x2C), background mode
 *     (+0x30) and the clip (+0x34..+0x44) keep the values the open gave them.
 *   - The subtitles ("Tanya Pavelovna: Un'esplosione", "%s: %s" next to the font's path) go through that class's
 *     TextOut, 0x0012A890, and a centered message through 0x0012C664: both call XFONT_TextOut (0x00138F9A, stdcall:
 *     font, surface, text, length, x, y) with the back buffer (GetBackBuffer2(0) at 0x0012BC55, kept at 0x003EAFEC).
 *     XFONT_TextOut does GetDesc and LockRect on the surface, then XFONT_TextOutToMemory (0x00138ECB: font, bits,
 *     pitch, width, height, format, text, length, x, y), which draws the glyphs into the 640 x 480 pixels by the CPU.
 *
 * What the port did with that: the render target's pixels (640 x 480) are the "CPU picture", put over the frame when
 * the game locked them that frame (fh_present.c, fh_draw.c draw_picture_quad). So the text was drawn at 640 x 480 and
 * stretched to the picture's size, and in a frame of the game's own draws (a subtitle over a level) the whole CPU
 * picture, the last film frame with every earlier line of text on it, went over the 4:3 part of the frame, opaque.
 *
 * What this does: the port's sub_00138F9A (fh/config/wrapped_functions.tsv; the recompiled XFONT_TextOut is renamed
 * fh_lifted_00138F9A) draws a call aimed at the back buffer with the game's own XFONT code, into the picture's own
 * pixels:
 *   - a second font, opened once from the same file as the game opens its own, set to the game font's height times
 *     the scale of the screen's 4:3 part (fh_draw_screen_area: the part's height / 480), with the game font's color,
 *     alignment and background copied in before each call;
 *   - the line drawn by XFONT_TextOutToMemory into a box of guest memory twice, once over black and once over white.
 *     XFONT blends each glyph pixel over what is there, out = under x (1 - a) + color x a, so over black the result is
 *     the color already times a, and white - black = 255 x (1 - a) gives a. Both are exact for any color, the
 *     background mode included (an opaque background gives a = 1);
 *   - the box put into a layer the size of ngl_vk's picture at the line's place in the 4:3 part (x, y times the scale,
 *     the part's corner added), the earlier text of the frame kept under it (premultiplied "over").
 * The layer goes on top of the frame at Swap (fh_draw.c draw_text_quad), one texel to a pixel, and is emptied after.
 * The render target is not locked for it any more, so the CPU picture is the films' alone again.
 *
 * Positions: the game works out x and y in its 640 x 480 screen (a centered line from GetTextExtent with its own
 * 640-scale font); they are kept and scaled, so the line starts where it did. The larger font's width can differ
 * from 640-scale width x scale by its hinting, at most a pixel or two at the picture's size.
 *
 * FH_NATIVE_TEXT=0 is the game's own TextOut, into the 640 x 480 pixels, as before. FH_TEXT_LOG=1 prints the first
 * lines drawn. Ours: the other Finest Hour recompilation draws XFONT's text through its own font code. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "recomp_types.h"           /* MEM32, PUSH32, g_esp, recomp_lookup: guest memory, stack and functions */
#include "xbox_memory_layout.h"     /* XBOX_PTR, xbox_HeapAlloc */
#include "fh_d3d.h"                 /* fh_d3d_render_target, fh_d3d_render_target_picture */
#include "fh_draw.h"                /* fh_draw_screen_area, fh_draw_size */
#include "fh_text.h"

#define XFONT_OPEN              0x001397F4u     /* XFONT_OpenTrueTypeFont(path, cache bytes, &font) */
#define XFONT_OPEN_RETURN       0x0012A96Cu
#define XFONT_SET_HEIGHT        0x00138E27u     /* XFONT_SetTextHeight(font, height) */
#define XFONT_SET_HEIGHT_RETURN 0x0012A97Eu
#define XFONT_EXTENT            0x00138E59u     /* XFONT_GetTextExtent(font, text, length, &width) */
#define XFONT_EXTENT_RETURN     0x0012A8E9u
#define XFONT_TO_MEMORY         0x00138ECBu     /* XFONT_TextOutToMemory(font, bits, pitch, w, h, format, text, n, x, y) */
#define XFONT_TO_MEMORY_RETURN  0x00138FDFu
#define FONT_PATH               0x002EB510u     /* L"d:\\media\\arialuni.ttf" */
#define GAME_CACHE              0x10000u        /* the game's own glyph cache */

#define FONT_HEIGHT             0x04u
#define FONT_ALIGN              0x24u
#define FONT_COLOR              0x28u
#define FONT_BK_COLOR           0x2Cu
#define FONT_BK_MODE            0x30u

#define TEXT_ARGS               24u             /* XFONT_TextOut's six arguments, taken off by its ret 0x18 */

void fh_lifted_00138F9A(void);
void sub_00138F9A(void);

static struct {
    int checked, on, log, failed;
    uint32_t font;                  /* the larger font (guest), 0 before it is opened */
    uint32_t box;                   /* guest memory: 16 bytes for answers, then the box twice */
    size_t box_bytes;
    uint8_t *layer;                 /* host: width x height, blue, green, red, alpha (premultiplied) */
    uint32_t width, height;
    uint32_t top, bottom;           /* rows holding text: top..bottom-1 (bottom 0: none) */
    unsigned long lines;
} text;

static void check(void)
{
    const char *on = getenv("FH_NATIVE_TEXT"), *log = getenv("FH_TEXT_LOG");

    if (text.checked)
        return;
    text.checked = 1;
    text.on = !(on && strcmp(on, "0") == 0);
    text.log = log && strcmp(log, "0") != 0;
    if (!text.on)
        fprintf(stderr, "[fh] text: FH_NATIVE_TEXT=0: the game's text is drawn into its 640 x 480 pixels\n");
}

/* Calls one of XFONT's stdcall functions as the game's code does: the arguments pushed last to first, then the
 * return address of a real call of it. Every register is put back after. The function's eax, or ~0 when it isn't in
 * the port. */
static uint32_t call_game(uint32_t function, uint32_t return_address, int count, const uint32_t *args)
{
    uint32_t eax_was = g_eax, ebx_was = g_ebx, ecx_was = g_ecx, edx_was = g_edx;
    uint32_t esi_was = g_esi, edi_was = g_edi, ebp_was = g_ebp, esp_was = g_esp, result;
    recomp_func_t fn = recomp_lookup_manual(function);
    int i;

    if (!fn)
        fn = recomp_lookup(function);
    if (!fn)
        return ~0u;
    for (i = count - 1; i >= 0; i--)
        PUSH32(g_esp, args[i]);
    PUSH32(g_esp, return_address);
    fn();
    result = g_eax;
    g_eax = eax_was;
    g_ebx = ebx_was;
    g_ecx = ecx_was;
    g_edx = edx_was;
    g_esi = esi_was;
    g_edi = edi_was;
    g_ebp = ebp_was;
    g_esp = esp_was;
    return result;
}

/* Guest memory for answers and two boxes of bytes each; grown when a line needs more (the old block is kept: the
 * guest heap has no free here, and it grows only a few times). */
static int box_room(size_t bytes)
{
    const size_t need = 16 + 2 * bytes;
    uint32_t p;

    if (text.box && text.box_bytes >= need)
        return 1;
    p = xbox_HeapAlloc((uint32_t)(need + need / 2), 16);
    if (!p) {
        fprintf(stderr, "[fh] text: no guest memory for a line of %zu bytes: the game's own text from here on\n",
                bytes);
        text.failed = 1;
        return 0;
    }
    text.box = p;
    text.box_bytes = need + need / 2;
    return 1;
}

static int open_font(float scale)
{
    uint32_t cache = GAME_CACHE, args[3], hr;
    float area = scale * scale;

    if (text.font)
        return 1;
    if (!box_room(64))
        return 0;
    if (area > 1.0f)
        cache = GAME_CACHE * (uint32_t)(area > 32.0f ? 32.0f : ceilf(area));
    MEM32(text.box) = 0;
    args[0] = FONT_PATH;
    args[1] = cache;
    args[2] = text.box;
    hr = call_game(XFONT_OPEN, XFONT_OPEN_RETURN, 3, args);
    if ((int32_t)hr < 0 || !MEM32(text.box)) {
        fprintf(stderr, "[fh] text: the font couldn't be opened again (0x%08X): the game's own text from here on\n",
                hr);
        text.failed = 1;
        return 0;
    }
    text.font = MEM32(text.box);
    fprintf(stderr, "[fh] text: the game's font opened again for the picture's size (%u KB of glyph cache)\n",
            cache / 1024u);
    return 1;
}

static int layer_size(uint32_t width, uint32_t height)
{
    if (text.layer && text.width == width && text.height == height)
        return 1;
    free(text.layer);
    text.layer = calloc((size_t)width * height, 4);
    text.width = text.layer ? width : 0;
    text.height = text.layer ? height : 0;
    text.top = text.bottom = 0;
    if (!text.layer) {
        fprintf(stderr, "[fh] text: no memory for a %u x %u layer: the game's own text from here on\n", width, height);
        text.failed = 1;
        return 0;
    }
    return 1;
}

static void fill(uint32_t bits, size_t count, uint32_t value)
{
    uint32_t *p = (uint32_t *)XBOX_PTR(bits);

    for (size_t i = 0; i < count; i++)
        p[i] = value;
}

/* The box over black and over white into the layer at (x0, y0): a from white - black, the color from black. */
static void put_box(const uint8_t *black, const uint8_t *white, uint32_t box_width, uint32_t box_height, int x0,
                    int y0)
{
    for (uint32_t r = 0; r < box_height; r++) {
        const int y = y0 + (int)r;

        if (y < 0 || y >= (int)text.height)
            continue;
        for (uint32_t c = 0; c < box_width; c++) {
            const int x = x0 + (int)c;
            const uint8_t *b = black + ((size_t)r * box_width + c) * 4, *w = white + ((size_t)r * box_width + c) * 4;
            int a = 255 - ((int)w[1] - (int)b[1]);
            uint8_t *d;

            if (x < 0 || x >= (int)text.width)
                continue;
            if (a <= 0)
                continue;
            if (a > 255)
                a = 255;
            d = text.layer + ((size_t)y * text.width + (size_t)x) * 4;
            for (int i = 0; i < 3; i++) {
                const int s = b[i] < a ? b[i] : a;
                d[i] = (uint8_t)(s + (d[i] * (255 - a) + 127) / 255);
            }
            d[3] = (uint8_t)(a + (d[3] * (255 - a) + 127) / 255);
            if (text.bottom == 0 || (uint32_t)y < text.top)
                text.top = (uint32_t)y;
            if ((uint32_t)y + 1 > text.bottom)
                text.bottom = (uint32_t)y + 1;
        }
    }
}

static void log_line(uint32_t string, uint32_t length, int x, int y, uint32_t game_height, uint32_t height,
                     float scale, int x0, int y0, uint32_t box_width, uint32_t box_height)
{
    char ascii[96];
    size_t n = 0;

    for (uint32_t i = 0; n + 1 < sizeof ascii && (length == 0xFFFFFFFFu || i < length); i++) {
        const uint16_t ch = *(const uint16_t *)XBOX_PTR(string + 2 * i);

        if (!ch)
            break;
        ascii[n++] = ch < 0x80 && ch >= 0x20 ? (char)ch : '?';
    }
    ascii[n] = '\0';
    fprintf(stderr, "[fh] text: \"%s\" at (%d, %d) height %u: drawn %u high at scale %.3f, a %u x %u box at (%d, %d)\n",
            ascii, x, y, game_height, height, scale, box_width, box_height, x0, y0);
}

/* XFONT_TextOut (stdcall: font, surface, text, length, x, y): S_OK */
void sub_00138F9A(void)
{
    const uint32_t font = MEM32(g_esp + 4), surface = MEM32(g_esp + 8), string = MEM32(g_esp + 12);
    const uint32_t length = MEM32(g_esp + 16);
    const int x = (int32_t)MEM32(g_esp + 20), y = (int32_t)MEM32(g_esp + 24);
    const uint8_t *pixels;
    uint32_t game_width, game_height, pitch, format, target_width, target_height, height, args[10];
    float part_x, part_y, part_width, part_height, scale;
    uint32_t extent, pad, box_width, box_height, black, white;
    size_t box_bytes;
    int x0, y0;

    check();
    if (!text.on || text.failed || !font || !string || surface != fh_d3d_render_target() ||
        !fh_d3d_render_target_picture(&pixels, &game_width, &game_height, &pitch, &format) || !game_height ||
        !fh_draw_size(&target_width, &target_height) ||
        !fh_draw_screen_area(&part_x, &part_y, &part_width, &part_height) || !(part_height > 0.0f)) {
        fh_lifted_00138F9A();
        return;
    }
    scale = part_height / (float)game_height;
    height = (uint32_t)lroundf((float)MEM32(font + FONT_HEIGHT) * scale);
    if (height < 1 || height > 4096 || !layer_size(target_width, target_height) || !open_font(scale)) {
        fh_lifted_00138F9A();
        return;
    }
    if (MEM32(text.font + FONT_HEIGHT) != height) {
        args[0] = text.font;
        args[1] = height;
        call_game(XFONT_SET_HEIGHT, XFONT_SET_HEIGHT_RETURN, 2, args);
    }
    MEM32(text.font + FONT_ALIGN) = MEM32(font + FONT_ALIGN);
    MEM32(text.font + FONT_COLOR) = MEM32(font + FONT_COLOR);
    MEM32(text.font + FONT_BK_COLOR) = MEM32(font + FONT_BK_COLOR);
    MEM32(text.font + FONT_BK_MODE) = MEM32(font + FONT_BK_MODE);

    /* the line's width at this size, then a box with room around it (a glyph can reach past its advance) */
    MEM32(text.box) = 0;
    args[0] = text.font;
    args[1] = string;
    args[2] = length;
    args[3] = text.box;
    if ((int32_t)call_game(XFONT_EXTENT, XFONT_EXTENT_RETURN, 4, args) < 0) {
        fh_lifted_00138F9A();
        return;
    }
    extent = MEM32(text.box);
    if (extent > 4u * target_width)
        extent = 4u * target_width;
    pad = height / 4u + 2u;
    box_width = extent + 2u * pad + height;
    box_height = 2u * height + 2u * pad;
    box_bytes = (size_t)box_width * box_height * 4;
    if (!box_room(box_bytes)) {
        fh_lifted_00138F9A();
        return;
    }
    black = text.box + 16;
    white = black + (uint32_t)box_bytes;
    for (int pass = 0; pass < 2; pass++) {
        const uint32_t bits = pass ? white : black;

        fill(bits, (size_t)box_width * box_height, pass ? 0x00FFFFFFu : 0x00000000u);
        args[0] = text.font;
        args[1] = bits;
        args[2] = box_width * 4;
        args[3] = box_width;
        args[4] = box_height;
        args[5] = format;
        args[6] = string;
        args[7] = length;
        args[8] = pad;
        args[9] = pad;
        call_game(XFONT_TO_MEMORY, XFONT_TO_MEMORY_RETURN, 10, args);
    }
    x0 = (int)lroundf(part_x + (float)x * scale) - (int)pad;
    y0 = (int)lroundf(part_y + (float)y * scale) - (int)pad;
    put_box((const uint8_t *)XBOX_PTR(black), (const uint8_t *)XBOX_PTR(white), box_width, box_height, x0, y0);
    if (text.log && text.lines < 40)
        log_line(string, length, x, y, MEM32(font + FONT_HEIGHT), height, scale, x0, y0, box_width, box_height);
    text.lines++;

    g_eax = 0;                              /* S_OK */
    g_esp += 4u + TEXT_ARGS;                /* stdcall: the return address and the six arguments */
}

int fh_text_layer(const uint8_t **pixels, uint32_t *width, uint32_t *height, uint32_t *top, uint32_t *rows)
{
    if (!text.layer || text.bottom <= text.top)
        return 0;
    *pixels = text.layer;
    *width = text.width;
    *height = text.height;
    *top = text.top;
    *rows = text.bottom - text.top;
    return 1;
}

int fh_text_waiting(void)
{
    return text.layer && text.bottom > text.top;
}

void fh_text_swap(void)
{
    if (!text.layer || text.bottom <= text.top)
        return;
    memset(text.layer + (size_t)text.top * text.width * 4, 0, (size_t)(text.bottom - text.top) * text.width * 4);
    text.top = text.bottom = 0;
}
