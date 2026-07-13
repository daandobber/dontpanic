#pragma once

#include "pax_gfx.h"
#include "pax_text.h"

// Colour palette, styled after the "Book" graphics from the 1981 Hitchhiker's
// Guide to the Galaxy TV series: black CRT background, phosphor green body
// text, amber/yellow headers and cyan accents.
#define GUIDE_COLOR_BG        pax_col_rgb(0x00, 0x00, 0x00)
#define GUIDE_COLOR_GREEN     pax_col_rgb(0x39, 0xff, 0x6a)
#define GUIDE_COLOR_GREEN_DIM pax_col_rgb(0x14, 0x66, 0x2b)
#define GUIDE_COLOR_YELLOW    pax_col_rgb(0xff, 0xd5, 0x00)
#define GUIDE_COLOR_CYAN      pax_col_rgb(0x33, 0xe6, 0xe6)
#define GUIDE_COLOR_AMBER     pax_col_rgb(0xff, 0x99, 0x00)
#define GUIDE_COLOR_RED       pax_col_rgb(0xff, 0x33, 0x33)
#define GUIDE_COLOR_SCANLINE  pax_col_argb(0x28, 0x00, 0x00, 0x00)

#define GUIDE_FONT       pax_font_sky_mono
#define GUIDE_FONT_TITLE pax_font_marker

// Layout margins shared by every screen.
#define GUIDE_MARGIN       12
#define GUIDE_HEADER_H     34
#define GUIDE_FOOTER_H     26

// Clears the framebuffer to the guide background colour.
void theme_clear(pax_buf_t *fb);

// Draws the rounded "book" frame used around content panels.
void theme_draw_panel(pax_buf_t *fb, float x, float y, float w, float h, pax_col_t color);

// Draws the top title bar (dataset/article name) shared by every screen.
void theme_draw_header(pax_buf_t *fb, float w, const char *title);

// Draws the bottom hint/status bar shared by every screen.
void theme_draw_footer(pax_buf_t *fb, float w, float h, const char *hint);

// Overlays faint horizontal scanlines to give the framebuffer a CRT feel.
void theme_draw_scanlines(pax_buf_t *fb, float w, float h);

// Returns the pixel width of a single monospace glyph at the given size,
// used to lay out fixed-width text columns.
float theme_glyph_width(float font_size);
