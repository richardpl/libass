/*
 * Copyright (C) 2011 Grigori Goronzy <greg@chown.ath.cx>
 *
 * This file is part of libass.
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <fribidi/fribidi.h>
#include <hb-ft.h>

#include "ass_shaper.h"
#include "ass_render.h"
#include "ass_font.h"
#include "ass_parse.h"

#define MAX_RUNS 50

enum {
    VERT = 0,
    VKNA,
    KERN
};
#define NUM_FEATURES 3

struct ass_shaper {
    // FriBidi log2vis
    int n_glyphs;
    FriBidiChar *event_text;
    FriBidiCharType *ctypes;
    FriBidiLevel *emblevels;
    FriBidiStrIndex *cmap;
    FriBidiParType base_direction;
    // OpenType features
    int n_features;
    hb_feature_t *features;
};

struct ass_shaper_font_data {
    hb_font_t *fonts[ASS_FONT_MAX_FACES];
};

/**
 * \brief Print version information
 */
void ass_shaper_info(ASS_Library *lib)
{
    ass_msg(lib, MSGL_V, "Complex text layout enabled, using FriBidi "
            FRIBIDI_VERSION " HarfBuzz-ng %s", hb_version_string());
}

/**
 * \brief grow arrays, if needed
 * \param new_size requested size
 */
static void check_allocations(ASS_Shaper *shaper, size_t new_size)
{
    if (new_size > shaper->n_glyphs) {
        shaper->event_text = realloc(shaper->event_text, sizeof(FriBidiChar) * new_size);
        shaper->ctypes     = realloc(shaper->ctypes, sizeof(FriBidiCharType) * new_size);
        shaper->emblevels  = realloc(shaper->emblevels, sizeof(FriBidiLevel) * new_size);
        shaper->cmap       = realloc(shaper->cmap, sizeof(FriBidiStrIndex) * new_size);
    }
}

/**
 * \brief set up the HarfBuzz OpenType feature list with some
 * standard features.
 */
static void init_features(ASS_Shaper *shaper)
{
    shaper->features = calloc(sizeof(hb_feature_t), NUM_FEATURES);

    shaper->n_features = NUM_FEATURES;
    shaper->features[VERT].tag = HB_TAG('v', 'e', 'r', 't');
    shaper->features[VERT].end = INT_MAX;
    shaper->features[VKNA].tag = HB_TAG('v', 'k', 'n', 'a');
    shaper->features[VKNA].end = INT_MAX;
    shaper->features[KERN].tag = HB_TAG('k', 'e', 'r', 'n');
    shaper->features[KERN].end = INT_MAX;
}

/**
 * \brief Create a new shaper instance and preallocate data structures
 * \param prealloc preallocation size
 */
ASS_Shaper *ass_shaper_new(size_t prealloc)
{
    ASS_Shaper *shaper = calloc(sizeof(*shaper), 1);

    shaper->base_direction = FRIBIDI_PAR_ON;
    init_features(shaper);
    check_allocations(shaper, prealloc);

    return shaper;
}

/**
 * \brief Free shaper and related data
 */
void ass_shaper_free(ASS_Shaper *shaper)
{
    free(shaper->event_text);
    free(shaper->ctypes);
    free(shaper->emblevels);
    free(shaper->cmap);
    free(shaper->features);
    free(shaper);
}

void ass_shaper_font_data_free(ASS_ShaperFontData *priv)
{
    int i;
    for (i = 0; i < ASS_FONT_MAX_FACES; i++)
        if (priv->fonts[i])
            hb_font_destroy(priv->fonts[i]);
    free(priv);
}

/**
 * \brief Set features depending on properties of the run
 */
static void set_run_features(ASS_Shaper *shaper, GlyphInfo *info)
{
        // enable vertical substitutions for @font runs
        if (info->font->desc.vertical)
            shaper->features[VERT].value = shaper->features[VKNA].value = 1;
        else
            shaper->features[VERT].value = shaper->features[VKNA].value = 0;
}

/**
 * \brief Update HarfBuzz's idea of font metrics
 * \param hb_font HarfBuzz font
 * \param face associated FreeType font face
 */
static void update_hb_size(hb_font_t *hb_font, FT_Face face)
{
    hb_font_set_scale (hb_font,
            ((uint64_t) face->size->metrics.x_scale * (uint64_t) face->units_per_EM) >> 16,
            ((uint64_t) face->size->metrics.y_scale * (uint64_t) face->units_per_EM) >> 16);
    hb_font_set_ppem (hb_font, face->size->metrics.x_ppem,
            face->size->metrics.y_ppem);
}

/**
 * \brief Retrieve HarfBuzz font from cache.
 * Create it from FreeType font, if needed.
 * \param info glyph cluster
 * \return HarfBuzz font
 */
static hb_font_t *get_hb_font(GlyphInfo *info)
{
    ASS_Font *font = info->font;
    hb_font_t **hb_fonts;

    if (!font->shaper_priv)
        font->shaper_priv = calloc(sizeof(ASS_ShaperFontData), 1);


    hb_fonts = font->shaper_priv->fonts;
    if (!hb_fonts[info->face_index])
        hb_fonts[info->face_index] =
            hb_ft_font_create(font->faces[info->face_index], NULL);

    ass_face_set_size(font->faces[info->face_index], info->font_size);
    update_hb_size(hb_fonts[info->face_index], font->faces[info->face_index]);

    return hb_fonts[info->face_index];
}

/**
 * \brief Shape event text with HarfBuzz. Full OpenType shaping.
 * \param glyphs glyph clusters
 * \param len number of clusters
 */
static void shape_harfbuzz(ASS_Shaper *shaper, GlyphInfo *glyphs, size_t len)
{
    int i, j;
    int run = 0;
    struct {
        int offset;
        int end;
        hb_buffer_t *buf;
        hb_font_t *font;
    } runs[MAX_RUNS];


    for (i = 0; i < len && run < MAX_RUNS; i++, run++) {
        // get length and level of the current run
        int k = i;
        int level = glyphs[i].shape_run_id;
        int direction = shaper->emblevels[k] % 2;
        while (i < (len - 1) && level == glyphs[i+1].shape_run_id)
            i++;
        //printf("run %d from %d to %d with level %d\n", run, k, i, level);
        runs[run].offset = k;
        runs[run].end    = i;
        runs[run].buf    = hb_buffer_create(i - k + 1);
        runs[run].font   = get_hb_font(glyphs + k);
        set_run_features(shaper, glyphs + k);
        hb_buffer_set_direction(runs[run].buf, direction ? HB_DIRECTION_RTL :
                HB_DIRECTION_LTR);
        hb_buffer_add_utf32(runs[run].buf, shaper->event_text + k, i - k + 1,
                0, i - k + 1);
        hb_shape(runs[run].font, runs[run].buf, shaper->features,
                shaper->n_features);
    }
    //printf("shaped %d runs\n", run);

    // Initialize: skip all glyphs, this is undone later as needed
    for (i = 0; i < len; i++)
        glyphs[i].skip = 1;

    // Update glyph indexes, positions and advances from the shaped runs
    for (i = 0; i < run; i++) {
        int num_glyphs = hb_buffer_get_length(runs[i].buf);
        hb_glyph_info_t *glyph_info = hb_buffer_get_glyph_infos(runs[i].buf, NULL);
        hb_glyph_position_t *pos    = hb_buffer_get_glyph_positions(runs[i].buf, NULL);
        //printf("run text len %d num_glyphs %d\n", runs[i].end - runs[i].offset + 1,
        //        num_glyphs);
        // Update glyphs
        for (j = 0; j < num_glyphs; j++) {
            int idx = glyph_info[j].cluster + runs[i].offset;
            GlyphInfo *info = glyphs + idx;
            GlyphInfo *root = info;
#if 0
            printf("run %d cluster %d codepoint %d -> '%c'\n", i, idx,
                    glyph_info[j].codepoint, event_text[idx]);
            printf("position %d %d advance %d %d\n",
                    pos[j].x_offset, pos[j].y_offset,
                    pos[j].x_advance, pos[j].y_advance);
#endif

            // if we have more than one glyph per cluster, allocate a new one
            // and attach to the root glyph
            if (info->skip == 0) {
                //printf("duplicate cluster entry, adding glyph\n");
                while (info->next)
                    info = info->next;
                info->next = malloc(sizeof(GlyphInfo));
                memcpy(info->next, info, sizeof(GlyphInfo));
                info = info->next;
                info->next = NULL;
            }

            // set position and advance
            info->skip = 0;
            info->glyph_index = glyph_info[j].codepoint;
            info->offset.x    = pos[j].x_offset * info->scale_x;
            info->offset.y    = -pos[j].y_offset * info->scale_y;
            info->advance.x   = pos[j].x_advance * info->scale_x;
            info->advance.y   = -pos[j].y_advance * info->scale_y;

            // accumulate advance in the root glyph
            root->cluster_advance.x += info->advance.x;
            root->cluster_advance.y += info->advance.y;
        }
    }

    // Free runs and associated data
    for (i = 0; i < run; i++) {
        hb_buffer_destroy(runs[i].buf);
    }

}

/**
 * \brief Shape event text with FriBidi. Does mirroring and simple
 * Arabic shaping.
 * \param len number of clusters
 */
static void shape_fribidi(ASS_Shaper *shaper, size_t len)
{
    FriBidiJoiningType *joins = calloc(sizeof(*joins), len);

    fribidi_get_joining_types(shaper->event_text, len, joins);
    fribidi_join_arabic(shaper->ctypes, len, shaper->emblevels, joins);
    fribidi_shape(FRIBIDI_FLAGS_DEFAULT | FRIBIDI_FLAGS_ARABIC,
            shaper->emblevels, len, joins, shaper->event_text);

    free(joins);
}

/**
 * \brief Toggle kerning for HarfBuzz shaping.
 * NOTE: currently only works with OpenType fonts, the TrueType fallback *always*
 * kerns. It's a bug in HarfBuzz.
 */
void ass_shaper_set_kerning(ASS_Shaper *shaper, int kern)
{
    shaper->features[KERN].value = !!kern;
}

/**
 * \brief Find shape runs according to the event's selected fonts
 */
void ass_shaper_find_runs(ASS_Shaper *shaper, ASS_Renderer *render_priv,
                          GlyphInfo *glyphs, size_t len)
{
    int i;
    int shape_run = 0;

    for (i = 0; i < len; i++) {
        GlyphInfo *last = glyphs + i - 1;
        GlyphInfo *info = glyphs + i;
        // skip drawings
        if (info->symbol == 0xfffc)
            continue;
        // set size and get glyph index
        ass_font_get_index(render_priv->fontconfig_priv, info->font,
                info->symbol, &info->face_index, &info->glyph_index);
        // shape runs share the same font face and size
        if (i > 0 && (last->font != info->font ||
                    last->font_size != info->font_size ||
                    last->face_index != info->face_index))
            shape_run++;
        info->shape_run_id = shape_run;
        //printf("glyph '%c' shape run id %d face %d\n", info->symbol, info->shape_run_id,
        //        info->face_index);
    }

}

/**
 * \brief Set base direction (paragraph direction) of the text.
 * \param dir base direction
 */
void ass_shaper_set_base_direction(ASS_Shaper *shaper, FriBidiParType dir)
{
    shaper->base_direction = dir;
}

/**
 * \brief Shape an event's text. Calculates directional runs and shapes them.
 * \param text_info event's text
 */
void ass_shaper_shape(ASS_Shaper *shaper, TextInfo *text_info)
{
    int i, last_break;
    FriBidiParType dir;
    GlyphInfo *glyphs = text_info->glyphs;

    check_allocations(shaper, text_info->length);

    // Get bidi character types and embedding levels
    last_break = 0;
    for (i = 0; i < text_info->length; i++) {
        shaper->event_text[i] = glyphs[i].symbol;
        // embedding levels should be calculated paragraph by paragraph
        if (glyphs[i].symbol == '\n' || i == text_info->length - 1) {
            //printf("paragraph from %d to %d\n", last_break, i);
            dir = shaper->base_direction;
            fribidi_get_bidi_types(shaper->event_text + last_break,
                    i - last_break + 1, shaper->ctypes + last_break);
            fribidi_get_par_embedding_levels(shaper->ctypes + last_break,
                    i - last_break + 1, &dir, shaper->emblevels + last_break);
            last_break = i + 1;
        }
    }

    // add embedding levels to shape runs for final runs
    for (i = 0; i < text_info->length; i++) {
        glyphs[i].shape_run_id += shaper->emblevels[i];
    }

#if 0
    printf("levels ");
    for (i = 0; i < text_info->length; i++) {
        printf("%d ", glyphs[i].shape_run_id);
    }
    printf("\n");
#endif

    //shape_fribidi(shaper, text_info->length);
    shape_harfbuzz(shaper, glyphs, text_info->length);

    // Update glyphs
    for (i = 0; i < text_info->length; i++) {
        glyphs[i].symbol = shaper->event_text[i];
        // Skip direction override control characters
        // NOTE: Behdad said HarfBuzz is supposed to remove these, but this hasn't
        // been implemented yet
        if (glyphs[i].symbol <= 0x202F && glyphs[i].symbol >= 0x202a) {
            glyphs[i].symbol = 0;
            glyphs[i].skip++;
        }
    }
}

/**
 * \brief clean up additional data temporarily needed for shaping and
 * (e.g. additional glyphs allocated)
 */
void ass_shaper_cleanup(ASS_Shaper *shaper, TextInfo *text_info)
{
    int i;

    for (i = 0; i < text_info->length; i++) {
        GlyphInfo *info = text_info->glyphs + i;
        info = info->next;
        while (info) {
            GlyphInfo *next = info->next;
            free(info);
            info = next;
        }
    }
}

/**
 * \brief Calculate reorder map to render glyphs in visual order
 */
FriBidiStrIndex *ass_shaper_reorder(ASS_Shaper *shaper, TextInfo *text_info)
{
    int i;

    // Initialize reorder map
    for (i = 0; i < text_info->length; i++)
        shaper->cmap[i] = i;

    // Create reorder map line-by-line
    for (i = 0; i < text_info->n_lines; i++) {
        LineInfo *line = text_info->lines + i;
        int level;
        FriBidiParType dir = FRIBIDI_PAR_ON;

        // FIXME: we should actually specify
        // the correct paragraph base direction
        level = fribidi_reorder_line(FRIBIDI_FLAGS_DEFAULT,
                shaper->ctypes + line->offset, line->len, 0, dir,
                shaper->emblevels + line->offset, NULL,
                shaper->cmap + line->offset);
        //printf("reorder line %d to level %d\n", i, level);
    }

#if 0
    printf("map ");
    for (i = 0; i < text_info->length; i++) {
        printf("%d ", cmap[i]);
    }
    printf("\n");
#endif

    return shaper->cmap;
}

/**
 * \brief Resolve a Windows font encoding number to a suitable
 * base direction. 177 and 178 are Hebrew and Arabic respectively, and
 * they map to RTL. 1 is autodetection and is mapped to just that.
 * Everything else is mapped to LTR.
 * \param enc Windows font encoding
 */
FriBidiParType resolve_base_direction(int enc)
{
    switch (enc) {
        case 1:
            return FRIBIDI_PAR_ON;
        case 177:
        case 178:
            return FRIBIDI_PAR_RTL;
        default:
            return FRIBIDI_PAR_LTR;
    }
}
