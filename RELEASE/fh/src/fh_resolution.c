/* [acct2-fh] 2026-11-13 Finest Hour port: what size the game is drawn at (fh_resolution.h).
 *
 * Call of Duty 3's port has the same choices (recomp/src/game/render_resolution.h: cod3_render_resolution,
 * cod3_render_percent, cod3_aspect_ratio, present_letterbox), but its limits come from the Xbox 360 it emulates: what
 * fits the console's render target memory, multiplied by a whole number. Finest Hour emulates no GPU: fh_draw.c draws
 * the game's draws at whatever size the target is, so here a size is only limited by the GPU (FH_MAX_SIZE, and
 * fh_present.c falls back when the GPU can't make it), and a size bigger than the window is drawn at that size and
 * shrunk to fit, which CoD3 doesn't do (it draws such a size at the window's size).
 *
 * The plan, as CoD3's PlanForView:
 *   1. The size: "auto" is the view's (the window's drawable size, the screen's in fullscreen; the game's own 640 x 480
 *      without a window), a named size or "WxH" as given, "original" the game's 640 x 480.
 *   2. The shape: with fit = bars the target takes the picture's shape (fh_resolution_picture_shape: the largest size
 *      of that shape inside the size; within 1% of it, the size stays as it is); with fit = stretch, the view's shape,
 *      so the picture fills the window.
 *   3. The scale: display.render_percent of each side (not for "original", as CoD3's "Xbox 360 original").
 *
 * The settings (fh.toml's [display], each with its variable): render_resolution (FH_RENDER_RESOLUTION), render_percent
 * (FH_RENDER_PERCENT), fit (FH_FIT), live_resolution (FH_LIVE_RESOLUTION). FH_SIZE=<w>x<h>, the port's setting before
 * these, still means what it meant: the target exactly that size, whatever the shape and the scale. */
#include "fh_resolution.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fh_settings.h"

/* The named sizes, in the order F2 lists them (as CoD3's kResolutionChoices, with the game's own first). */
static const struct {
    const char *value, *other;  /* the setting's value, and another name it is known by */
    uint32_t width, height;
} named_sizes[] = {
    {"original", "xbox", FH_GAME_WIDTH, FH_GAME_HEIGHT},
    {"480p", "sd", 854, 480},
    {"720p", "hd", 1280, 720},
    {"1080p", "fullhd", 1920, 1080},
    {"1440p", "qhd", 2560, 1440},
    {"2160p", "4k", 3840, 2160},
    {"2880p", "5k", 5120, 2880},
    {"4320p", "8k", 7680, 4320},
};

int fh_resolution_parse_size(const char *text, uint32_t min_width, uint32_t min_height, uint32_t max_side,
                             uint32_t *width, uint32_t *height)
{
    char *end;
    unsigned long w, h;

    if (!text)
        return 0;
    while (isspace((unsigned char)*text))
        text++;
    w = strtoul(text, &end, 10);
    if (end == text)
        return 0;
    while (isspace((unsigned char)*end))
        end++;
    if (*end != 'x' && *end != 'X' && *end != '*')
        return 0;
    text = end + 1;
    h = strtoul(text, &end, 10);
    if (end == text)
        return 0;
    while (isspace((unsigned char)*end))
        end++;
    if (*end || w < min_width || h < min_height || w > max_side || h > max_side)
        return 0;
    *width = (uint32_t)w;
    *height = (uint32_t)h;
    return 1;
}

int fh_resolution_live(void)
{
    return fh_setting_bool("display.live_resolution", "FH_LIVE_RESOLUTION", 1);
}

enum fh_fit fh_resolution_fit(void)
{
    const char *fit = fh_setting_text("display.fit", "FH_FIT");

    if (!fit || !fit[0] || !strcmp(fit, "bars"))
        return FH_FIT_BARS;
    if (!strcmp(fit, "stretch"))
        return FH_FIT_STRETCH;
    {
        static int said;

        if (!said++)
            fprintf(stderr, "[fh] display.fit = \"%s\" isn't bars or stretch: bars\n", fit);
    }
    return FH_FIT_BARS;
}

int fh_resolution_parse_shape(const char *text, float *shape)
{
    char *end;
    double w, h = 1.0;

    if (!text || !text[0])
        return 0;
    w = strtod(text, &end);
    if (end == text || !(w > 0.0))
        return 0;
    if (*end == ':' || *end == 'x' || *end == 'X' || *end == '/') {
        const char *rest = end + 1;

        h = strtod(rest, &end);
        if (end == rest || !(h > 0.0))
            return 0;
    }
    while (*end == ' ')
        end++;
    if (*end)
        return 0;
    *shape = (float)(w / h);
    return 1;
}

float fh_resolution_picture_shape(uint32_t view_width, uint32_t view_height, uint32_t size_width,
                                  uint32_t size_height)
{
    const char *aspect = fh_setting_text("display.aspect_ratio", "FH_ASPECT_RATIO");
    const float game = (float)FH_GAME_WIDTH / (float)FH_GAME_HEIGHT;
    float shape = game;
    static char said[48];

    if (!strcmp(aspect ? aspect : "", "game") || !strcmp(aspect ? aspect : "", "4:3"))
        return game;
    /* [acct2-fh] 21:50, after boot44 showed Hor+ right: "auto" when not set, as CoD3's cod3_aspect_ratio */
    if (!aspect || !aspect[0] || !strcmp(aspect, "auto"))
        shape = view_width && view_height ? (float)view_width / (float)view_height : game;
    else if (!strcmp(aspect, "resolution"))
        shape = size_width && size_height ? (float)size_width / (float)size_height : game;
    else if (!fh_resolution_parse_shape(aspect, &shape)) {
        if (strncmp(said, aspect, sizeof said - 1)) {
            snprintf(said, sizeof said, "%s", aspect);
            fprintf(stderr, "[fh] display.aspect_ratio = \"%s\" isn't game, auto, resolution, W:H or a number: the "
                    "game's 4:3\n", aspect);
        }
        return game;
    }
    if (shape < FH_MIN_SHAPE)
        shape = FH_MIN_SHAPE;
    if (shape > FH_MAX_SHAPE)
        shape = FH_MAX_SHAPE;
    return shape;
}

/* The largest size of shape (width / height) inside *width x *height; a size within 1% of the shape stays. */
static void fit_shape(uint32_t *width, uint32_t *height, float shape)
{
    float have = (float)*width / (float)*height;

    if (fabsf(have - shape) <= shape * 0.01f)
        return;
    if (have > shape)
        *width = (uint32_t)lroundf((float)*height * shape);
    else
        *height = (uint32_t)lroundf((float)*width / shape);
    if (*width < 1)
        *width = 1;
    if (*height < 1)
        *height = 1;
}

static void add_note(struct fh_render_plan *plan, const char *text)
{
    size_t n = strlen(plan->note);

    snprintf(plan->note + n, sizeof plan->note - n, "%s%s", n ? "; " : "", text);
}

/* width x height times percent, at least FH_MIN_SIZE a side; bigger than FH_MAX_SIZE a side, made smaller with its
 * shape kept. */
static void scale_size(uint32_t *width, uint32_t *height, uint32_t percent, struct fh_render_plan *plan)
{
    double w = (double)*width * percent / 100.0, h = (double)*height * percent / 100.0;
    double most = w > h ? w : h;

    if (most > FH_MAX_SIZE) {
        char text[96];

        snprintf(text, sizeof text, "%.0fx%.0f is more than %u a side: made smaller", w, h, FH_MAX_SIZE);
        add_note(plan, text);
        w = w * FH_MAX_SIZE / most;
        h = h * FH_MAX_SIZE / most;
    }
    *width = w < FH_MIN_SIZE ? FH_MIN_SIZE : (uint32_t)(w + 0.5);
    *height = h < FH_MIN_SIZE ? FH_MIN_SIZE : (uint32_t)(h + 0.5);
}

void fh_resolution_plan(uint32_t view_width, uint32_t view_height, struct fh_render_plan *plan)
{
    const char *choice = getenv("FH_RENDER_RESOLUTION");
    const char *old_size = getenv("FH_SIZE");
    int original = 0, exact = 0;
    double percent;
    size_t i;

    memset(plan, 0, sizeof *plan);
    plan->view_width = view_width;
    plan->view_height = view_height;
    plan->fit = fh_resolution_fit();
    if ((!choice || !choice[0]) && old_size && old_size[0]) {
        choice = old_size;      /* FH_SIZE: the target exactly so, as before */
        exact = 1;
    } else {
        choice = fh_setting_text("display.render_resolution", "FH_RENDER_RESOLUTION");
    }
    if (!choice || !choice[0])
        choice = "auto";
    snprintf(plan->choice, sizeof plan->choice, "%s", choice);

    /* 1. the size */
    for (i = 0; i < sizeof named_sizes / sizeof named_sizes[0]; i++)
        if (!strcmp(choice, named_sizes[i].value) || !strcmp(choice, named_sizes[i].other)) {
            plan->size_width = named_sizes[i].width;
            plan->size_height = named_sizes[i].height;
            original = i == 0;
            break;
        }
    if (!plan->size_width && strcmp(choice, "auto") &&
        !fh_resolution_parse_size(choice, FH_MIN_CUSTOM_WIDTH, FH_MIN_CUSTOM_HEIGHT, FH_MAX_SIZE, &plan->size_width,
                                  &plan->size_height)) {
        char text[120];

        snprintf(text, sizeof text, "\"%s\" isn't a size (auto, original, 720p ... 4320p, or WxH from %ux%u to %ux%u): "
                 "auto", choice, FH_MIN_CUSTOM_WIDTH, FH_MIN_CUSTOM_HEIGHT, FH_MAX_SIZE, FH_MAX_SIZE);
        add_note(plan, text);
        exact = 0;
    }
    if (!plan->size_width) {    /* auto: the view's own size, or the game's without one */
        plan->size_width = view_width && view_height ? view_width : FH_GAME_WIDTH;
        plan->size_height = view_width && view_height ? view_height : FH_GAME_HEIGHT;
    }

    /* 2. the shape: the picture's (what the 3D view is drawn at, fh_aspect.c), and the target's, which is the
     * picture's with bars and the window's when the picture is stretched over it */
    plan->picture_shape = fh_resolution_picture_shape(view_width, view_height, plan->size_width, plan->size_height);
    plan->shape = plan->fit == FH_FIT_STRETCH && view_width && view_height ? (float)view_width / (float)view_height
                                                                           : plan->picture_shape;
    plan->width = plan->size_width;
    plan->height = plan->size_height;
    if (exact) {
        add_note(plan, "FH_SIZE: the target as given");
        plan->percent = 100;
        plan->shape = (float)plan->width / (float)plan->height;
        return;
    }
    fit_shape(&plan->width, &plan->height, plan->shape);

    /* 3. the scale */
    percent = fh_setting_number("display.render_percent", "FH_RENDER_PERCENT", 100.0);
    if (percent < FH_MIN_PERCENT || percent > FH_MAX_PERCENT) {
        char text[96];

        snprintf(text, sizeof text, "render_percent %g kept to %u-%u", percent, FH_MIN_PERCENT, FH_MAX_PERCENT);
        add_note(plan, text);
        percent = percent < FH_MIN_PERCENT ? FH_MIN_PERCENT : FH_MAX_PERCENT;
    }
    plan->percent = original ? 100u : (uint32_t)(percent + 0.5);
    if (original && percent != 100.0)
        add_note(plan, "the scale doesn't apply to original");
    scale_size(&plan->width, &plan->height, plan->percent, plan);
}

int fh_resolution_same_target(const struct fh_render_plan *a, const struct fh_render_plan *b)
{
    return a->width == b->width && a->height == b->height;
}

void fh_resolution_describe(const struct fh_render_plan *plan, char *out, size_t size)
{
    char shown[96] = "offscreen";

    if (plan->view_width && plan->view_height) {
        const int bigger = plan->width > plan->view_width || plan->height > plan->view_height;
        const int same = fabsf((float)plan->width / (float)plan->height -
                               (float)plan->view_width / (float)plan->view_height) < 0.002f;

        snprintf(shown, sizeof shown, "shown in %ux%u%s%s", plan->view_width, plan->view_height,
                 same ? "" : plan->fit == FH_FIT_BARS ? " with bars" : " stretched",
                 bigger ? ", shrunk to fit" : "");
    }
    snprintf(out, size, "%s -> %ux%u (%u%%, shape %.3f:1), %s%s%s", plan->choice, plan->width, plan->height,
             plan->percent, plan->shape, shown, plan->note[0] ? "; " : "", plan->note);
}
