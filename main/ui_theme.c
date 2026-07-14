#include "ui_theme.h"

#include "pax_fonts.h"
#include "pax_shapes.h"

void theme_clear(pax_buf_t *fb) {
    pax_background(fb, GUIDE_COLOR_BG);
}

void theme_draw_panel(pax_buf_t *fb, float x, float y, float w, float h, pax_col_t color) {
    pax_outline_rect(fb, color, x, y, w, h);
    pax_simple_line(fb, GUIDE_COLOR_BLUE_DIM, x + 8, y + 8, x + w - 8, y + 8);
    pax_simple_line(fb, GUIDE_COLOR_BLUE_DIM, x + 8, y + h - 8, x + w - 8, y + h - 8);
}

void theme_draw_header(pax_buf_t *fb, float w, const char *title) {
    pax_draw_text(fb, GUIDE_COLOR_BLUE, GUIDE_FONT, 22, GUIDE_MARGIN, 4, title);
    pax_simple_line(fb, GUIDE_COLOR_BLUE, GUIDE_MARGIN, GUIDE_HEADER_H, w - GUIDE_MARGIN, GUIDE_HEADER_H);
    pax_simple_line(fb, GUIDE_COLOR_GREEN_DIM, GUIDE_MARGIN, GUIDE_HEADER_H + 3, w - GUIDE_MARGIN, GUIDE_HEADER_H + 3);
}

void theme_draw_footer(pax_buf_t *fb, float w, float h, const char *hint) {
    float y = h - GUIDE_FOOTER_H;
    pax_simple_line(fb, GUIDE_COLOR_BLUE_DIM, GUIDE_MARGIN, y, w - GUIDE_MARGIN, y);
    pax_draw_text(fb, GUIDE_COLOR_BLUE, GUIDE_FONT, 14, GUIDE_MARGIN, y + 6, hint);
}

void theme_draw_scanlines(pax_buf_t *fb, float w, float h) {
    for (float y = 0; y < h; y += 3) {
        pax_simple_line(fb, GUIDE_COLOR_SCANLINE, 0, y, w, y);
    }
}

float theme_glyph_width(float font_size) {
    pax_vec2f size = pax_text_size(GUIDE_FONT, font_size, "M");
    return size.x;
}
