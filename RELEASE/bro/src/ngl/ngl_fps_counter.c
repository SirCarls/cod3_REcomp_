/* [acct2-bro-game] Big Red One port: the frame-rate counter drawn over the picture.
 *
 * With display.show_fps on (BRO_SHOW_FPS=1), the top right corner of the screen shows the frames a second and the
 * average frame's time, updated once a second by the frame clock (ngl_frame_clock.c), and so does the window's
 * title. The text is drawn with a font of its own (3 x 5 dots a character, made here into a small texture), so it
 * doesn't depend on the game's fonts or on what the game is doing. */
#include "ngl_fps_counter.h"

#include <stdio.h>
#include <string.h>

#include "bro_settings.h"

#ifdef BRO_HAVE_VULKAN
#include "ngl_vk.h"
#endif

#define GLYPH_DOTS_X   3
#define GLYPH_DOTS_Y   5
#define DOT_TEXELS     4                    /* each dot is 4 x 4 texels in the texture */
#define CELL_DOTS_X    4                    /* a dot of space after each character */
#define CELL_DOTS_Y    6
#define DOT_SCREEN     2.0f                 /* each dot is 2 x 2 of the game's 640 x 480 screen */
#define MARGIN         6.0f
#define PADDING        3.0f
#define SCREEN_RIGHT   640.0f
#define TEXT_COLOR     0xFFFFFFFFu          /* white, opaque (0xAARRGGBB) */
#define BACK_COLOR     0xA0000000u          /* black, a little see-through */
#define MAX_TEXT       24

/* The characters the counter uses, each five rows of three dots (bit 2 the left one). */
static const char glyph_chars[] = "0123456789.FPSM ";
static const unsigned char glyph_rows[][GLYPH_DOTS_Y] = {
    {7, 5, 5, 5, 7}, {2, 6, 2, 2, 7}, {7, 1, 7, 4, 7}, {7, 1, 7, 1, 7}, {5, 5, 7, 1, 1},     /* 0 1 2 3 4 */
    {7, 4, 7, 1, 7}, {7, 4, 7, 5, 7}, {7, 1, 1, 2, 2}, {7, 5, 7, 5, 7}, {7, 5, 7, 1, 7},     /* 5 6 7 8 9 */
    {0, 0, 0, 0, 2}, {7, 4, 6, 4, 4}, {7, 5, 7, 4, 4}, {7, 4, 7, 1, 7}, {5, 7, 7, 5, 5},     /* . F P S M */
    {0, 0, 0, 0, 0},                                                                        /* space */
};
#define GLYPH_COUNT ((unsigned)(sizeof glyph_rows / sizeof glyph_rows[0]))
#define TEXTURE_WIDTH  (GLYPH_COUNT * CELL_DOTS_X * DOT_TEXELS)
#define TEXTURE_HEIGHT (CELL_DOTS_Y * DOT_TEXELS)

static int enabled = -1;
static char text[MAX_TEXT] = "";
static int title_changed;
static double shown_fps, shown_ms;

int ngl_fps_counter_on(void)
{
    if (enabled < 0)
        enabled = bro_setting_bool("display.show_fps", "BRO_SHOW_FPS", 0);
    return enabled;
}

void ngl_fps_counter_set(double frames_per_second, double frame_ms)
{
    shown_fps = frames_per_second;
    shown_ms = frame_ms;
    snprintf(text, sizeof text, "%.0f FPS %.1f MS", frames_per_second, frame_ms);
    title_changed = 1;
}

int ngl_fps_counter_title(char *out, unsigned size)
{
    if (!title_changed || !size)
        return 0;
    title_changed = 0;
    snprintf(out, size, "Call of Duty 2: Big Red One - %.0f fps (%.2f ms)", shown_fps, shown_ms);
    return 1;
}

#ifdef BRO_HAVE_VULKAN
static uint32_t make_texture(void)
{
    static uint8_t pixels[TEXTURE_HEIGHT][TEXTURE_WIDTH][4];
    unsigned g, row, column, y, x;

    memset(pixels, 0, sizeof pixels);
    for (g = 0; g < GLYPH_COUNT; g++)
        for (row = 0; row < GLYPH_DOTS_Y; row++)
            for (column = 0; column < GLYPH_DOTS_X; column++) {
                if (!(glyph_rows[g][row] & (4u >> column)))
                    continue;
                for (y = 0; y < DOT_TEXELS; y++)
                    for (x = 0; x < DOT_TEXELS; x++) {
                        uint8_t *p = pixels[row * DOT_TEXELS + y][(g * CELL_DOTS_X + column) * DOT_TEXELS + x];

                        p[0] = p[1] = p[2] = p[3] = 0xFF;   /* blue, green, red, alpha */
                    }
            }
    return ngl_vk_texture(pixels, TEXTURE_WIDTH, TEXTURE_HEIGHT);
}

/* One quad as two triangles: (left, top) to (right, bottom) on the screen, (u0, v0) to (u1, v1) in the texture. */
static uint32_t add_quad(struct ngl_vk_vertex *out, float left, float top, float right, float bottom, float u0,
                         float v0, float u1, float v1, uint32_t color, float textured, uint32_t texture)
{
    const float xs[4] = { left, right, left, right }, ys[4] = { top, top, bottom, bottom };
    const float us[4] = { u0, u1, u0, u1 }, vs[4] = { v0, v0, v1, v1 };
    static const int corners[6] = { 0, 1, 2, 2, 1, 3 };
    int i;

    for (i = 0; i < 6; i++) {
        int c = corners[i];

        out[i].x = xs[c];
        out[i].y = ys[c];
        out[i].u = us[c];
        out[i].v = vs[c];
        out[i].color = color;
        out[i].textured = textured;
        out[i].texture = texture;
    }
    return 6;
}

uint32_t ngl_fps_counter_vertices(struct ngl_vk_vertex *out, uint32_t room)
{
    static uint32_t texture;
    static int failed;
    const float cell_w = CELL_DOTS_X * DOT_SCREEN, glyph_w = GLYPH_DOTS_X * DOT_SCREEN;
    const float glyph_h = GLYPH_DOTS_Y * DOT_SCREEN;
    size_t length = strlen(text), i;
    float left, top = MARGIN + PADDING, x;
    uint32_t n = 0;

    if (!ngl_fps_counter_on() || !length || failed || room < 6 * (length + 1))
        return 0;
    if (!texture && !(texture = make_texture())) {
        failed = 1;
        fprintf(stderr, "[fps] no room in ngl_vk for the counter's texture: no counter on screen\n");
        return 0;
    }
    left = SCREEN_RIGHT - MARGIN - PADDING - (float)length * cell_w + DOT_SCREEN;
    n += add_quad(out + n, left - PADDING, top - PADDING, SCREEN_RIGHT - MARGIN, top + glyph_h + PADDING, 0.0f, 0.0f,
                  0.0f, 0.0f, BACK_COLOR, 0.0f, 0);
    for (i = 0, x = left; i < length; i++, x += cell_w) {
        const char *at = strchr(glyph_chars, text[i]);
        unsigned g = at ? (unsigned)(at - glyph_chars) : GLYPH_COUNT - 1;
        float u0 = (float)(g * CELL_DOTS_X * DOT_TEXELS) / (float)TEXTURE_WIDTH;
        float u1 = (float)((g * CELL_DOTS_X + GLYPH_DOTS_X) * DOT_TEXELS) / (float)TEXTURE_WIDTH;
        float v1 = (float)(GLYPH_DOTS_Y * DOT_TEXELS) / (float)TEXTURE_HEIGHT;

        if (text[i] == ' ')
            continue;
        n += add_quad(out + n, x, top, x + glyph_w, top + glyph_h, u0, 0.0f, u1, v1, TEXT_COLOR, 2.0f, texture);
    }
    return n;
}
#else
uint32_t ngl_fps_counter_vertices(struct ngl_vk_vertex *out, uint32_t room)
{
    (void)out;
    (void)room;
    return 0;
}
#endif
