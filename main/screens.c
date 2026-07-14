#include "screens.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bsp/device.h"
#include "bsp/display.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "pax_fonts.h"
#include "pax_text.h"
#include "storage.h"
#include "ui_theme.h"

#define LIST_FONT_SIZE    16
#define BODY_FONT_SIZE    16
#define LINE_HEIGHT       20
#define LIST_ROW_HEIGHT   22
#define MAX_ARTICLE_LINES 4000
#define HOME_ENTRY_COUNT  5
#define SETTINGS_FIXED_ROWS 2
#define READER_SCROLL_STEP 3

static const char *TAG = "screens";
static const char *NVS_NAMESPACE = "dontpanic";
static const char *NVS_DEFAULT_DATASET_KEY = "default_ds";

// ---------------------------------------------------------------------------
// State helpers
// ---------------------------------------------------------------------------

static void open_article(app_state_t *st, const wiki_result_t *result);

static void copy_str(char *dst, size_t dst_cap, const char *src) {
    if (dst_cap == 0) {
        return;
    }
    snprintf(dst, dst_cap, "%s", src ? src : "");
}

static void present_now(app_state_t *st) {
    if (st->present_cb) {
        st->present_cb(st->present_ctx);
    }
}

static void show_loading(app_state_t *st, const char *title, const char *message, int percent) {
    copy_str(st->loading_title, sizeof(st->loading_title), title);
    copy_str(st->loading_message, sizeof(st->loading_message), message);
    if (percent < 0) {
        percent = 0;
    } else if (percent > 100) {
        percent = 100;
    }
    st->loading_percent = percent;
    st->screen          = SCREEN_LOADING;
    screen_render(st);
    present_now(st);
}

static void load_default_dataset_setting(app_state_t *st) {
    st->default_dataset_valid   = false;
    st->default_dataset_slug[0] = '\0';

    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return;
    }
    size_t len = sizeof(st->default_dataset_slug);
    if (nvs_get_str(nvs, NVS_DEFAULT_DATASET_KEY, st->default_dataset_slug, &len) == ESP_OK &&
        st->default_dataset_slug[0] != '\0') {
        st->default_dataset_valid = true;
    }
    nvs_close(nvs);
}

static void save_default_dataset_setting(app_state_t *st, const char *slug) {
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) {
        return;
    }
    if (slug && slug[0]) {
        nvs_set_str(nvs, NVS_DEFAULT_DATASET_KEY, slug);
        copy_str(st->default_dataset_slug, sizeof(st->default_dataset_slug), slug);
        st->default_dataset_valid = true;
    } else {
        nvs_erase_key(nvs, NVS_DEFAULT_DATASET_KEY);
        st->default_dataset_slug[0] = '\0';
        st->default_dataset_valid   = false;
    }
    nvs_commit(nvs);
    nvs_close(nvs);
}

static size_t settings_entry_count(app_state_t *st) {
    return st->dataset_count + SETTINGS_FIXED_ROWS;
}

static int find_default_dataset(app_state_t *st) {
    if (!st->default_dataset_valid) {
        return -1;
    }
    for (size_t i = 0; i < st->dataset_count; i++) {
        if (strcmp(st->datasets[i].slug, st->default_dataset_slug) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static void open_settings(app_state_t *st, app_screen_t return_screen) {
    st->settings_return_screen = return_screen;
    int default_index          = find_default_dataset(st);
    st->settings_selected      = default_index >= 0 ? (size_t)default_index + 1 : 0;
    st->screen                 = SCREEN_SETTINGS;
}

static void favorite_key(app_state_t *st, char *out, size_t out_cap) {
    snprintf(out, out_cap, "fav_%.20s", st->current_dataset.info.slug);
}

static void load_favorite(app_state_t *st) {
    st->favorite_valid = false;
    nvs_handle_t nvs;
    if (nvs_open("dontpanic", NVS_READONLY, &nvs) != ESP_OK) {
        ESP_LOGI(TAG, "No favorite stored for dataset '%s'", st->current_dataset.info.slug);
        return;
    }
    char key[32];
    favorite_key(st, key, sizeof(key));
    size_t len = sizeof(st->favorite_result);
    if (nvs_get_blob(nvs, key, &st->favorite_result, &len) == ESP_OK && len == sizeof(st->favorite_result)) {
        st->favorite_valid = true;
    }
    nvs_close(nvs);
    ESP_LOGI(TAG, "Favorite load complete for dataset '%s': valid=%d", st->current_dataset.info.slug,
             st->favorite_valid);
}

static void save_favorite(app_state_t *st) {
    nvs_handle_t nvs;
    if (nvs_open("dontpanic", NVS_READWRITE, &nvs) != ESP_OK) {
        return;
    }
    char key[32];
    favorite_key(st, key, sizeof(key));
    nvs_set_blob(nvs, key, &st->current_result, sizeof(st->current_result));
    nvs_commit(nvs);
    nvs_close(nvs);
    st->favorite_result = st->current_result;
    st->favorite_valid  = true;
}

static void reset_article(app_state_t *st) {
    wiki_article_free(&st->article);
    if (st->article_line_cache) {
        for (size_t i = 0; i < st->article_line_count; i++) {
            free(st->article_line_cache[i]);
        }
        free(st->article_line_cache);
    }
    free(st->article_lines);
    st->article_lines         = NULL;
    st->article_line_cache    = NULL;
    st->article_line_count    = 0;
    st->article_line_capacity = 0;
    st->reader_scroll         = 0;
    st->reader_link_selected  = 0;
}

static void close_dataset(app_state_t *st) {
    reset_article(st);
    if (st->dataset_open) {
        wiki_dataset_close(&st->current_dataset);
        st->dataset_open = false;
    }
    st->search_buf[0]   = '\0';
    st->search_len      = 0;
    st->result_count    = 0;
    st->result_selected = 0;
    st->result_skip     = 0;
    st->result_has_more = false;
    st->favorite_valid  = false;
}

static void refresh_results(app_state_t *st, size_t skip) {
    st->result_skip = skip;
    st->result_count =
        wiki_search_prefix(&st->current_dataset, st->search_buf, skip, st->results, WIKI_MAX_RESULTS, &st->result_has_more);
    st->result_selected = 0;
    ESP_LOGI(TAG, "Search refreshed: query='%s' skip=%u results=%u more=%d", st->search_buf,
             (unsigned)skip, (unsigned)st->result_count, st->result_has_more);
}

static bool open_dataset(app_state_t *st, size_t index) {
    ESP_LOGI(TAG, "Opening dataset UI index=%u name='%s'", (unsigned)index, st->datasets[index].name);
    show_loading(st, "LOADING DATA CARTRIDGE", st->datasets[index].name, 12);
    close_dataset(st);
    show_loading(st, "LOADING DATA CARTRIDGE", st->datasets[index].name, 28);
    if (!wiki_dataset_open(&st->datasets[index], &st->current_dataset)) {
        ESP_LOGW(TAG, "Dataset open failed");
        st->screen = SCREEN_DATASETS;
        return false;
    }
    show_loading(st, "LOADING DATA CARTRIDGE", st->datasets[index].name, 76);
    ESP_LOGI(TAG, "Dataset backend open complete; loading favorite");
    st->dataset_open     = true;
    st->dataset_selected = index;
    st->home_selected    = 0;
    load_favorite(st);
    show_loading(st, "LOADING DATA CARTRIDGE", st->datasets[index].name, 100);
    st->screen = SCREEN_HOME;
    ESP_LOGI(TAG, "Dataset open complete; switched to home screen");
    return true;
}

// ---------------------------------------------------------------------------
// Article word-wrapping
//
// pax_font_sky_mono is a fixed-width font, so wrapping is done by counting
// UTF-8 codepoints per line (a terminal-style "N columns" wrap) rather than
// re-measuring pixel widths for every candidate line.
// ---------------------------------------------------------------------------

static void push_line(app_state_t *st, uint32_t offset, size_t length) {
    if (st->article_line_count >= MAX_ARTICLE_LINES) {
        return;
    }
    if (st->article_line_count >= st->article_line_capacity) {
        size_t new_cap = st->article_line_capacity ? st->article_line_capacity * 2 : 256;
        if (new_cap > MAX_ARTICLE_LINES) {
            new_cap = MAX_ARTICLE_LINES;
        }
        wiki_text_line_t *grown = realloc(st->article_lines, new_cap * sizeof(wiki_text_line_t));
        if (!grown) {
            return;
        }
        st->article_lines         = grown;
        st->article_line_capacity = new_cap;
    }
    st->article_lines[st->article_line_count].offset = offset;
    st->article_lines[st->article_line_count].length = (uint16_t)(length > 0xFFFF ? 0xFFFF : length);
    st->article_line_count++;
}

static void copy_ascii_safe(char *dst, size_t dst_cap, const char *src, size_t src_len) {
    if (dst_cap == 0) {
        return;
    }

    size_t out = 0;
    for (size_t i = 0; i < src_len && out + 1 < dst_cap; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == '\t') {
            dst[out++] = '\t';
        } else if (c >= 32 && c <= 126) {
            dst[out++] = (char)c;
        } else if (c >= 0x80) {
            dst[out++] = '?';
            while (i + 1 < src_len && (((unsigned char)src[i + 1]) & 0xC0) == 0x80) {
                i++;
            }
        }
    }
    dst[out] = '\0';
}

static bool ranges_overlap(size_t a_start, size_t a_end, size_t b_start, size_t b_end) {
    return a_start < b_end && b_start < a_end;
}

static size_t utf8_columns_between(const char *text, size_t start, size_t end) {
    size_t cols = 0;
    size_t pos  = start;
    while (pos < end) {
        uint32_t    cp;
        char const *next = pax_utf8_getch(text + pos, &cp);
        size_t      adv  = (size_t)(next - (text + pos));
        if (adv == 0) {
            adv = 1;
        }
        pos += adv;
        cols++;
    }
    return cols;
}

static const char *safe_title(const wiki_result_t *result, char *buf, size_t buf_cap) {
    copy_ascii_safe(buf, buf_cap, wiki_result_title(result), strlen(wiki_result_title(result)));
    return buf;
}

static void ensure_selected_link_visible(app_state_t *st) {
    if (st->article.link_count == 0 || st->reader_link_selected >= st->article.link_count) {
        return;
    }

    html_text_link_t *link = &st->article.links[st->reader_link_selected];
    for (size_t i = 0; i < st->article_line_count; i++) {
        wiki_text_line_t *ln = &st->article_lines[i];
        size_t line_start = ln->offset;
        size_t line_end   = line_start + ln->length;
        if (!ranges_overlap(line_start, line_end, link->text_start, link->text_end)) {
            continue;
        }
        if (i < st->reader_scroll) {
            st->reader_scroll = i;
        } else if (st->reader_visible_lines > 0 && i >= st->reader_scroll + st->reader_visible_lines) {
            st->reader_scroll = i + 1 - st->reader_visible_lines;
        }
        return;
    }
}

static bool normalized_chars_equal(char a, char b) {
    if (a >= 'A' && a <= 'Z') {
        a = (char)(a + ('a' - 'A'));
    }
    if (b >= 'A' && b <= 'Z') {
        b = (char)(b + ('a' - 'A'));
    }
    if (a == '_') {
        a = ' ';
    }
    if (b == '_') {
        b = ' ';
    }
    return a == b;
}

static bool is_title_char(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == ' ' || c == '_' ||
           (unsigned char)c >= 0x80;
}

static bool paragraph_matches_title(app_state_t *st, const char *para, size_t para_len) {
    const char *title = wiki_result_title(&st->current_result);
    size_t ti = 0;
    size_t pi = 0;

    while (title[ti] && !is_title_char(title[ti])) {
        ti++;
    }
    while (pi < para_len && !is_title_char(para[pi])) {
        pi++;
    }

    size_t matched = 0;
    while (title[ti] && pi < para_len) {
        while (title[ti] && !is_title_char(title[ti])) {
            ti++;
        }
        while (pi < para_len && !is_title_char(para[pi])) {
            pi++;
        }
        if (!title[ti] || pi >= para_len) {
            break;
        }
        if (!normalized_chars_equal(title[ti], para[pi])) {
            return false;
        }
        ti++;
        pi++;
        matched++;
    }

    while (title[ti] && !is_title_char(title[ti])) {
        ti++;
    }
    while (pi < para_len && !is_title_char(para[pi])) {
        pi++;
    }

    return matched >= 4 && title[ti] == '\0' && pi >= para_len;
}

static void wrap_article(app_state_t *st, size_t cols) {
    if (cols < 1) {
        cols = 1;
    }

    const char *text       = st->article.text;
    size_t       len        = st->article.len;
    size_t       para_start = 0;

    for (size_t i = 0; i <= len; i++) {
        bool at_end     = (i == len);
        bool is_newline = !at_end && text[i] == '\n';
        if (!at_end && !is_newline) {
            continue;
        }

        size_t p        = para_start;
        size_t para_end = i;

        if (st->article_line_count < 8 && paragraph_matches_title(st, text + p, para_end - p)) {
            para_start = i + 1;
            continue;
        }

        if (para_end == p) {
            push_line(st, (uint32_t)p, 0);
        }

        if (memchr(text + p, '\t', para_end - p)) {
            push_line(st, (uint32_t)p, para_end - p);
            para_start = i + 1;
            if (st->article_line_count >= MAX_ARTICLE_LINES) {
                break;
            }
            continue;
        }

        while (p < para_end && st->article_line_count < MAX_ARTICLE_LINES) {
            size_t line_start     = p;
            size_t cur            = p;
            size_t col            = 0;
            bool   have_space     = false;
            size_t last_space_pos = 0;

            while (cur < para_end && col < cols) {
                uint32_t    cp;
                char const *next = pax_utf8_getch(text + cur, &cp);
                size_t      adv  = (size_t)(next - (text + cur));
                if (adv == 0) {
                    adv = 1;
                }
                if (cp == ' ') {
                    have_space     = true;
                    last_space_pos = cur;
                }
                cur += adv;
                col++;
            }

            size_t line_end, next_start;
            if (cur >= para_end) {
                line_end   = para_end;
                next_start = para_end;
            } else if (have_space) {
                line_end   = last_space_pos;
                next_start = last_space_pos + 1;
            } else {
                line_end   = cur;
                next_start = cur;
            }

            if (line_end > line_start) {
                push_line(st, (uint32_t)line_start, line_end - line_start);
            }
            p = next_start;
        }

        para_start = i + 1;
        if (st->article_line_count >= MAX_ARTICLE_LINES) {
            break;
        }
    }
}

static void build_article_line_cache(app_state_t *st) {
    st->article_line_cache = calloc(st->article_line_count ? st->article_line_count : 1, sizeof(char *));
    if (!st->article_line_cache) {
        return;
    }
    for (size_t i = 0; i < st->article_line_count; i++) {
        wiki_text_line_t *ln = &st->article_lines[i];
        size_t n = ln->length;
        if (n > 240) {
            n = 240;
        }
        char *buf = malloc(n + 1);
        if (!buf) {
            continue;
        }
        copy_ascii_safe(buf, n + 1, st->article.text + ln->offset, n);
        st->article_line_cache[i] = buf;
    }
}

static void open_article(app_state_t *st, const wiki_result_t *result) {
    reset_article(st);
    st->current_result = *result;

    char title_buf[WIKI_TITLE_MAX + 1];
    show_loading(st, "LOADING ENTRY", safe_title(result, title_buf, sizeof(title_buf)), 20);
    if (!wiki_load_article_ex(&st->current_dataset, result, &st->article)) {
        ESP_LOGW(TAG, "Article open failed: dataset='%s' backend=%d title='%s' entry=%u",
                 st->current_dataset.info.name, (int)st->current_dataset.info.backend,
                 wiki_result_title(result), (unsigned)result->zim.entry_index);
        show_loading(st, "LOADING ENTRY", "Entry could not be loaded.", 100);
        st->screen = SCREEN_READER;
        return;
    }
    show_loading(st, "LOADING ENTRY", safe_title(result, title_buf, sizeof(title_buf)), 72);

    float  content_w = st->w - 2 * GUIDE_MARGIN;
    float  glyph_w   = theme_glyph_width(BODY_FONT_SIZE);
    size_t cols      = glyph_w > 0 ? (size_t)(content_w / glyph_w) : 40;
    if (cols < 8) {
        cols = 8;
    }
    wrap_article(st, cols);
    build_article_line_cache(st);
    ESP_LOGI(TAG, "Article wrapped: title='%s' bytes=%u links=%u lines=%u cols=%u",
             wiki_result_title(result), (unsigned)st->article.len, (unsigned)st->article.link_count,
             (unsigned)st->article_line_count, (unsigned)cols);

    float content_h          = st->h - 14;
    st->reader_visible_lines = (size_t)(content_h / LINE_HEIGHT);
    if (st->reader_visible_lines < 1) {
        st->reader_visible_lines = 1;
    }
    st->reader_scroll = 0;
    st->reader_link_selected = 0;
    st->reader_links_enabled = true;
    st->reader_status[0]     = '\0';
    st->screen               = SCREEN_READER;
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

static void render_no_card(app_state_t *st) {
    theme_clear(st->fb);
    theme_draw_panel(st->fb, 3, 3, st->w - 6, st->h - 6, GUIDE_COLOR_GREEN_DIM);
    theme_draw_header(st->fb, st->w, "THE HITCHHIKER'S GUIDE TO THE GALAXY");

    float y = GUIDE_HEADER_H + 30;
    pax_draw_text(st->fb, GUIDE_COLOR_RED, GUIDE_FONT, 20, GUIDE_MARGIN, y, "NO DATA CARTRIDGE DETECTED");
    y += 34;
    pax_draw_text(st->fb, GUIDE_COLOR_GREEN, GUIDE_FONT, 15, GUIDE_MARGIN, y,
                  "This unit cannot find a Sub-Etha data cartridge.");
    y += 22;
    pax_draw_text(st->fb, GUIDE_COLOR_GREEN, GUIDE_FONT, 15, GUIDE_MARGIN, y,
                  "Insert an SD card, then press ENTER to try again.");
    y += 34;
    pax_draw_text(st->fb, GUIDE_COLOR_CYAN, GUIDE_FONT, 15, GUIDE_MARGIN, y, "(Don't Panic.)");

    theme_draw_footer(st->fb, st->w, st->h, "ENTER: retry    F1: launcher");
    theme_draw_scanlines(st->fb, st->w, st->h);
}

static void render_datasets(app_state_t *st) {
    theme_clear(st->fb);
    theme_draw_panel(st->fb, 3, 3, st->w - 6, st->h - 6, GUIDE_COLOR_GREEN_DIM);
    theme_draw_header(st->fb, st->w, "SELECT A DATA CARTRIDGE");

    float y = GUIDE_HEADER_H + 10;
    for (size_t i = 0; i < st->dataset_count; i++) {
        bool selected = (i == st->dataset_selected);
        if (selected) {
            pax_simple_rect(st->fb, GUIDE_COLOR_GREEN_DIM, GUIDE_MARGIN, y, st->w - 2 * GUIDE_MARGIN, LIST_ROW_HEIGHT);
        }
        char line[96];
        snprintf(line, sizeof(line), "%s %.80s", selected ? ">" : " ", st->datasets[i].name);
        pax_draw_text(st->fb, selected ? GUIDE_COLOR_YELLOW : GUIDE_COLOR_GREEN, GUIDE_FONT, LIST_FONT_SIZE,
                      GUIDE_MARGIN + 4, y + 3, line);
        y += LIST_ROW_HEIGHT;
    }

    if (st->dataset_count == 0) {
        pax_draw_text(st->fb, GUIDE_COLOR_CYAN, GUIDE_FONT, 14, GUIDE_MARGIN, st->h - GUIDE_FOOTER_H - 38,
                      "Copy .zim files from a computer to /wiki on the SD card.");
        pax_draw_text(st->fb, GUIDE_COLOR_YELLOW, GUIDE_FONT, 14, GUIDE_MARGIN, st->h - GUIDE_FOOTER_H - 20,
                      "No valid datasets found.");
    } else if (st->dataset_count > 0) {
        char desc[220];
        snprintf(desc, sizeof(desc), "%.140s -- %u entries", st->datasets[st->dataset_selected].description,
                 (unsigned)st->datasets[st->dataset_selected].article_count);
        pax_draw_text(st->fb, GUIDE_COLOR_CYAN, GUIDE_FONT, 14, GUIDE_MARGIN, st->h - GUIDE_FOOTER_H - 22, desc);
    }

    theme_draw_footer(st->fb, st->w, st->h, "UP/DOWN select   ENTER open   ESC settings   F1 exit");
    theme_draw_scanlines(st->fb, st->w, st->h);
}

static void render_loading(app_state_t *st) {
    theme_clear(st->fb);
    theme_draw_panel(st->fb, 3, 3, st->w - 6, st->h - 6, GUIDE_COLOR_GREEN_DIM);
    theme_draw_header(st->fb, st->w, st->loading_title[0] ? st->loading_title : "LOADING");

    float y = GUIDE_HEADER_H + 70;
    pax_draw_text(st->fb, GUIDE_COLOR_GREEN, GUIDE_FONT, 18, GUIDE_MARGIN, y,
                  st->loading_message[0] ? st->loading_message : "Please wait...");

    float bar_x = GUIDE_MARGIN;
    float bar_y = y + 48;
    float bar_w = st->w - 2 * GUIDE_MARGIN;
    float bar_h = 24;
    pax_outline_rect(st->fb, GUIDE_COLOR_GREEN, bar_x, bar_y, bar_w, bar_h);
    float fill_w = (bar_w - 4) * (float)st->loading_percent / 100.0f;
    if (fill_w > 0) {
        pax_simple_rect(st->fb, GUIDE_COLOR_GREEN_DIM, bar_x + 2, bar_y + 2, fill_w, bar_h - 4);
    }

    char pct[16];
    snprintf(pct, sizeof(pct), "%d%%", st->loading_percent);
    pax_draw_text(st->fb, GUIDE_COLOR_YELLOW, GUIDE_FONT, 18, GUIDE_MARGIN, bar_y + bar_h + 16, pct);

    theme_draw_footer(st->fb, st->w, st->h, "Reading from SD card");
    theme_draw_scanlines(st->fb, st->w, st->h);
}

static void render_settings(app_state_t *st) {
    theme_clear(st->fb);
    theme_draw_panel(st->fb, 3, 3, st->w - 6, st->h - 6, GUIDE_COLOR_GREEN_DIM);
    theme_draw_header(st->fb, st->w, "SETTINGS");

    int  default_index = find_default_dataset(st);
    char current[128];
    snprintf(current, sizeof(current), "Default library: %s",
             default_index >= 0 ? st->datasets[default_index].name : "none");
    pax_draw_text(st->fb, GUIDE_COLOR_CYAN, GUIDE_FONT, 15, GUIDE_MARGIN, GUIDE_HEADER_H + 8, current);

    float  y     = GUIDE_HEADER_H + 36;
    size_t total = settings_entry_count(st);
    for (size_t row = 0; row < total; row++) {
        bool selected = row == st->settings_selected;
        if (selected) {
            pax_simple_rect(st->fb, GUIDE_COLOR_GREEN_DIM, GUIDE_MARGIN, y, st->w - 2 * GUIDE_MARGIN, LIST_ROW_HEIGHT);
        }

        char line[128];
        if (row == 0) {
            snprintf(line, sizeof(line), "%s No default library", selected ? ">" : " ");
        } else if (row <= st->dataset_count) {
            size_t dataset_index = row - 1;
            bool   is_default    = default_index == (int)dataset_index;
            snprintf(line, sizeof(line), "%s %s%.92s", selected ? ">" : " ", is_default ? "* " : "  ",
                     st->datasets[dataset_index].name);
        } else {
            snprintf(line, sizeof(line), "%s Back", selected ? ">" : " ");
        }
        pax_draw_text(st->fb, selected ? GUIDE_COLOR_YELLOW : GUIDE_COLOR_GREEN, GUIDE_FONT, LIST_FONT_SIZE,
                      GUIDE_MARGIN + 4, y + 3, line);
        y += LIST_ROW_HEIGHT;
    }

    theme_draw_footer(st->fb, st->w, st->h, "UP/DOWN select   ENTER save   ESC back");
    theme_draw_scanlines(st->fb, st->w, st->h);
}

static void render_browser(app_state_t *st) {
    theme_clear(st->fb);
    theme_draw_panel(st->fb, 3, 3, st->w - 6, st->h - 6, GUIDE_COLOR_GREEN_DIM);

    char header[80];
    snprintf(header, sizeof(header), "%.60s", st->current_dataset.info.name);
    theme_draw_header(st->fb, st->w, header);

    float input_w = st->w - 56;
    float input_x = 28;
    float input_y = GUIDE_HEADER_H + 36;
    float input_h = 54;
    pax_outline_rect(st->fb, GUIDE_COLOR_GREEN, input_x, input_y, input_w, input_h);
    pax_simple_rect(st->fb, GUIDE_COLOR_GREEN_DIM, input_x + 2, input_y + input_h - 6, input_w - 4, 4);

    char search_line[80];
    snprintf(search_line, sizeof(search_line), "%.48s_", st->search_buf);
    pax_draw_text(st->fb, GUIDE_COLOR_YELLOW, GUIDE_FONT, 28, input_x + 14, input_y + 13, search_line);

    float y = input_y + input_h + 26;

    if (st->search_len == 0) {
        pax_draw_text(st->fb, GUIDE_COLOR_GREEN_DIM, GUIDE_FONT, 18, GUIDE_MARGIN, y,
                      "Type a subject and press ENTER.");
    } else if (st->result_count == 0) {
        pax_draw_text(st->fb, GUIDE_COLOR_GREEN_DIM, GUIDE_FONT, 18, GUIDE_MARGIN, y,
                      "Press ENTER to search.");
    } else {
        for (size_t i = 0; i < st->result_count; i++) {
            bool selected = (i == st->result_selected);
            float row_h = 28;
            if (selected) {
                pax_simple_rect(st->fb, GUIDE_COLOR_GREEN_DIM, GUIDE_MARGIN, y - 2, st->w - 2 * GUIDE_MARGIN,
                                row_h);
            }
            char line[128];
            snprintf(line, sizeof(line), "%s %.100s", selected ? ">" : " ", wiki_result_title(&st->results[i]));
            pax_draw_text(st->fb, selected ? GUIDE_COLOR_YELLOW : GUIDE_COLOR_GREEN, GUIDE_FONT, 18,
                          GUIDE_MARGIN + 8, y + 3, line);
            y += row_h + 2;
        }
        if (st->result_has_more) {
            pax_draw_text(st->fb, GUIDE_COLOR_CYAN, GUIDE_FONT, 15, GUIDE_MARGIN, y + 2, "More results below");
        }
    }

    theme_draw_footer(st->fb, st->w, st->h, "TYPE query   ENTER search/open   BACKSPACE edit   ESC back");
    theme_draw_scanlines(st->fb, st->w, st->h);
}

static void render_home(app_state_t *st) {
    theme_clear(st->fb);
    theme_draw_panel(st->fb, 3, 3, st->w - 6, st->h - 6, GUIDE_COLOR_GREEN_DIM);
    theme_draw_header(st->fb, st->w, "DON'T PANIC");

    const char *items[HOME_ENTRY_COUNT] = {
        "Search the Guide",
        "Surprise me",
        "Open source frontpage",
        st->favorite_valid ? wiki_result_title(&st->favorite_result) : "No favorite saved",
        "Settings",
    };

    float y = GUIDE_HEADER_H + 12;
    pax_draw_text(st->fb, GUIDE_COLOR_YELLOW, GUIDE_FONT, 22, GUIDE_MARGIN, y, "THE GUIDE IS ONLINE");
    y += 32;
    pax_draw_text(st->fb, GUIDE_COLOR_GREEN, GUIDE_FONT, 15, GUIDE_MARGIN, y, "Mostly harmless local knowledge cartridge:");
    y += 22;
    pax_draw_text(st->fb, GUIDE_COLOR_BLUE, GUIDE_FONT, 15, GUIDE_MARGIN, y, st->current_dataset.info.name);
    y += 34;

    for (size_t i = 0; i < HOME_ENTRY_COUNT; i++) {
        bool selected = (i == st->home_selected);
        float row_h = 28;
        if (selected) {
            pax_simple_rect(st->fb, GUIDE_COLOR_GREEN_DIM, GUIDE_MARGIN, y - 2, st->w - 2 * GUIDE_MARGIN, row_h);
            pax_simple_line(st->fb, GUIDE_COLOR_YELLOW, GUIDE_MARGIN + 2, y + row_h - 2, st->w - GUIDE_MARGIN - 2,
                            y + row_h - 2);
        }
        char line[128];
        snprintf(line, sizeof(line), "%s %.100s", selected ? ">" : " ", items[i]);
        pax_draw_text(st->fb, selected ? GUIDE_COLOR_YELLOW : GUIDE_COLOR_GREEN, GUIDE_FONT, 18,
                      GUIDE_MARGIN + 8, y + 4, line);
        y += row_h + 6;
    }

    pax_draw_text(st->fb, GUIDE_COLOR_GREEN_DIM, GUIDE_FONT, 13, GUIDE_MARGIN, st->h - GUIDE_FOOTER_H - 42,
                  "F4 stores an entry as favorite. ESC opens settings.");
    theme_draw_footer(st->fb, st->w, st->h, "UP/DOWN select   ENTER activate   ESC settings");
    theme_draw_scanlines(st->fb, st->w, st->h);
}

static void render_reader_line(app_state_t *st, size_t line_index, float y) {
    if (line_index >= st->article_line_count) {
        return;
    }

    wiki_text_line_t *ln = &st->article_lines[line_index];
    bool highlight = false;
    float highlight_x = GUIDE_MARGIN;
    float highlight_w = 0;
    if (st->reader_links_enabled && st->article.link_count > 0 && st->reader_link_selected < st->article.link_count) {
        html_text_link_t *link = &st->article.links[st->reader_link_selected];
        size_t line_start = ln->offset;
        size_t line_end   = line_start + ln->length;
        if (ranges_overlap(line_start, line_end, link->text_start, link->text_end)) {
            size_t sel_start = link->text_start > line_start ? link->text_start : line_start;
            size_t sel_end   = link->text_end < line_end ? link->text_end : line_end;
            float glyph_w    = theme_glyph_width(BODY_FONT_SIZE);
            highlight_x      = GUIDE_MARGIN +
                          (float)utf8_columns_between(st->article.text, line_start, sel_start) * glyph_w;
            highlight_w      = (float)utf8_columns_between(st->article.text, sel_start, sel_end) * glyph_w;
            if (highlight_w < glyph_w) {
                highlight_w = glyph_w;
            }
            highlight = true;
        }
    }
    if (highlight) {
        pax_simple_rect(st->fb, GUIDE_COLOR_GREEN_DIM, highlight_x - 2, y - 1, highlight_w + 4, LINE_HEIGHT);
    }

    char fallback_buf[256];
    const char *line_text = st->article_line_cache && st->article_line_cache[line_index]
                                ? st->article_line_cache[line_index]
                                : NULL;
    if (!line_text) {
        size_t n = ln->length;
        if (n >= sizeof(fallback_buf)) {
            n = sizeof(fallback_buf) - 1;
        }
        copy_ascii_safe(fallback_buf, sizeof(fallback_buf), st->article.text + ln->offset, n);
        line_text = fallback_buf;
    }
    char split_buf[256];
    const char *tab = strchr(line_text, '\t');
    if (tab) {
        snprintf(split_buf, sizeof(split_buf), "%s", line_text);
        char *split_tab = strchr(split_buf, '\t');
        if (split_tab) {
            *split_tab = '\0';
            char *value = split_tab + 1;
            while (*value == ' ' || *value == '\t') {
                value++;
            }
            pax_draw_text(st->fb, GUIDE_COLOR_BLUE, GUIDE_FONT, BODY_FONT_SIZE, GUIDE_MARGIN, y, split_buf);
            pax_draw_text(st->fb, highlight ? GUIDE_COLOR_YELLOW : GUIDE_COLOR_GREEN, GUIDE_FONT, BODY_FONT_SIZE,
                          GUIDE_MARGIN + 210, y, value);
            return;
        }
    }

    pax_draw_text(st->fb, highlight ? GUIDE_COLOR_YELLOW : GUIDE_COLOR_GREEN, GUIDE_FONT, BODY_FONT_SIZE,
                  GUIDE_MARGIN, y, line_text);
}

static void render_reader_status(app_state_t *st) {
    pax_simple_rect(st->fb, GUIDE_COLOR_BG, 0, st->h - 38, st->w, 38);
    if (st->reader_status[0]) {
        pax_draw_text(st->fb, GUIDE_COLOR_AMBER, GUIDE_FONT, 13, GUIDE_MARGIN, st->h - 34, st->reader_status);
    }

    size_t total      = st->article_line_count ? st->article_line_count : 1;
    size_t shown_last = st->reader_scroll + st->reader_visible_lines;
    if (shown_last > total) {
        shown_last = total;
    }
    char hint[128];
    if (st->reader_links_enabled && st->article.link_count > 0) {
        snprintf(hint, sizeof(hint), "L/R link %u/%u  ENTER open  ESC back  [%u/%u]",
                 (unsigned)(st->reader_link_selected + 1), (unsigned)st->article.link_count, (unsigned)shown_last,
                 (unsigned)total);
    } else {
        snprintf(hint, sizeof(hint), "UP/DOWN scroll  F6 links %s  ESC back  [%u/%u]",
                 st->reader_links_enabled ? "on" : "off", (unsigned)shown_last, (unsigned)total);
    }
    pax_simple_line(st->fb, GUIDE_COLOR_BLUE_DIM, GUIDE_MARGIN, st->h - 18, st->w - GUIDE_MARGIN, st->h - 18);
    pax_draw_text(st->fb, GUIDE_COLOR_BLUE, GUIDE_FONT, 12, GUIDE_MARGIN, st->h - 15, hint);
}

static void render_reader(app_state_t *st) {
    theme_clear(st->fb);

    float y = 6;
    if (!st->article.text) {
        pax_draw_text(st->fb, GUIDE_COLOR_RED, GUIDE_FONT, BODY_FONT_SIZE, GUIDE_MARGIN, y,
                      "ENTRY COULD NOT BE LOADED.");
    } else {
        size_t end = st->reader_scroll + st->reader_visible_lines;
        if (end > st->article_line_count) {
            end = st->article_line_count;
        }
        for (size_t i = st->reader_scroll; i < end; i++) {
            render_reader_line(st, i, y);
            y += LINE_HEIGHT;
        }
    }

    render_reader_status(st);
}

static void reader_fast_scroll(app_state_t *st, size_t old_scroll, size_t new_scroll) {
    if (!st->article.text || old_scroll == new_scroll) {
        return;
    }

    int delta_lines = (int)new_scroll - (int)old_scroll;
    int delta_px    = delta_lines * LINE_HEIGHT;
    if (delta_px == 0 || abs(delta_lines) >= (int)st->reader_visible_lines) {
        render_reader(st);
        present_now(st);
        st->frame_presented = true;
        return;
    }

    pax_buf_scroll(st->fb, GUIDE_COLOR_BG, 0, -delta_px);

    if (delta_lines > 0) {
        size_t first = new_scroll + st->reader_visible_lines - (size_t)delta_lines;
        size_t last  = new_scroll + st->reader_visible_lines;
        if (last > st->article_line_count) {
            last = st->article_line_count;
        }
        float y = 6 + (float)(st->reader_visible_lines - (size_t)delta_lines) * LINE_HEIGHT;
        pax_simple_rect(st->fb, GUIDE_COLOR_BG, 0, y - 2, st->w, (float)delta_px + 4);
        for (size_t i = first; i < last; i++) {
            render_reader_line(st, i, y);
            y += LINE_HEIGHT;
        }
    } else {
        size_t count = (size_t)(-delta_lines);
        size_t last  = new_scroll + count;
        if (last > st->article_line_count) {
            last = st->article_line_count;
        }
        float y = 6;
        pax_simple_rect(st->fb, GUIDE_COLOR_BG, 0, 0, st->w, (float)(-delta_px) + 8);
        for (size_t i = new_scroll; i < last; i++) {
            render_reader_line(st, i, y);
            y += LINE_HEIGHT;
        }
    }

    render_reader_status(st);
    present_now(st);
    st->frame_presented = true;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void app_state_init(app_state_t *st, pax_buf_t *fb, float w, float h) {
    memset(st, 0, sizeof(*st));
    st->fb     = fb;
    st->w      = w;
    st->h      = h;
    st->screen = SCREEN_NO_CARD;
    st->settings_return_screen = SCREEN_DATASETS;
}

void screens_set_present_callback(app_state_t *st, void (*present_cb)(void *ctx), void *ctx) {
    st->present_cb  = present_cb;
    st->present_ctx = ctx;
}

void screens_refresh_datasets(app_state_t *st) {
    close_dataset(st);
    st->dataset_count    = 0;
    st->dataset_selected = 0;
    load_default_dataset_setting(st);
    if (storage_sdcard_mounted()) {
        st->dataset_count = wiki_discover_datasets(st->datasets, WIKI_MAX_DATASETS);
        st->screen        = SCREEN_DATASETS;
        int default_index = find_default_dataset(st);
        if (default_index >= 0) {
            ESP_LOGI(TAG, "Opening default dataset '%s'", st->datasets[default_index].name);
            open_dataset(st, (size_t)default_index);
        }
    } else {
        st->screen = SCREEN_NO_CARD;
    }
}

void screen_render(app_state_t *st) {
    switch (st->screen) {
        case SCREEN_NO_CARD:
            render_no_card(st);
            break;
        case SCREEN_DATASETS:
            render_datasets(st);
            break;
        case SCREEN_SETTINGS:
            render_settings(st);
            break;
        case SCREEN_LOADING:
            render_loading(st);
            break;
        case SCREEN_HOME:
            render_home(st);
            break;
        case SCREEN_BROWSER:
            render_browser(st);
            break;
        case SCREEN_READER:
            render_reader(st);
            break;
    }
}

void screen_on_navigation(app_state_t *st, bsp_input_navigation_key_t key) {
    switch (st->screen) {
        case SCREEN_NO_CARD:
            if (key == BSP_INPUT_NAVIGATION_KEY_RETURN) {
                storage_mount_sdcard();
                screens_refresh_datasets(st);
            }
            break;

        case SCREEN_DATASETS: {
            if (st->dataset_count == 0) {
                if (key == BSP_INPUT_NAVIGATION_KEY_ESC) {
                    open_settings(st, SCREEN_DATASETS);
                }
                break;
            }
            size_t total_entries = st->dataset_count;
            if (key == BSP_INPUT_NAVIGATION_KEY_UP) {
                st->dataset_selected = (st->dataset_selected == 0) ? total_entries - 1 : st->dataset_selected - 1;
            } else if (key == BSP_INPUT_NAVIGATION_KEY_DOWN) {
                st->dataset_selected = (st->dataset_selected + 1) % total_entries;
            } else if (key == BSP_INPUT_NAVIGATION_KEY_RETURN) {
                open_dataset(st, st->dataset_selected);
            } else if (key == BSP_INPUT_NAVIGATION_KEY_ESC) {
                open_settings(st, SCREEN_DATASETS);
            }
            break;
        }

        case SCREEN_SETTINGS: {
            size_t total_entries = settings_entry_count(st);
            if (key == BSP_INPUT_NAVIGATION_KEY_UP) {
                st->settings_selected = (st->settings_selected == 0) ? total_entries - 1 : st->settings_selected - 1;
            } else if (key == BSP_INPUT_NAVIGATION_KEY_DOWN) {
                st->settings_selected = (st->settings_selected + 1) % total_entries;
            } else if (key == BSP_INPUT_NAVIGATION_KEY_ESC) {
                st->screen = st->settings_return_screen;
            } else if (key == BSP_INPUT_NAVIGATION_KEY_RETURN) {
                if (st->settings_selected == 0) {
                    save_default_dataset_setting(st, NULL);
                } else if (st->settings_selected <= st->dataset_count) {
                    size_t dataset_index = st->settings_selected - 1;
                    save_default_dataset_setting(st, st->datasets[dataset_index].slug);
                    st->dataset_selected = dataset_index;
                    open_dataset(st, dataset_index);
                } else {
                    st->screen = st->settings_return_screen;
                }
            }
            break;
        }

        case SCREEN_LOADING:
            break;

        case SCREEN_HOME:
            if (key == BSP_INPUT_NAVIGATION_KEY_ESC) {
                ESP_LOGI(TAG, "Home: ESC");
                open_settings(st, SCREEN_HOME);
            } else if (key == BSP_INPUT_NAVIGATION_KEY_UP) {
                st->home_selected = (st->home_selected == 0) ? HOME_ENTRY_COUNT - 1 : st->home_selected - 1;
            } else if (key == BSP_INPUT_NAVIGATION_KEY_DOWN) {
                st->home_selected = (st->home_selected + 1) % HOME_ENTRY_COUNT;
            } else if (key == BSP_INPUT_NAVIGATION_KEY_RETURN) {
                ESP_LOGI(TAG, "Home: ENTER item=%u", (unsigned)st->home_selected);
                wiki_result_t target;
                if (st->home_selected == 0) {
                    st->search_buf[0]   = '\0';
                    st->search_len      = 0;
                    st->result_count    = 0;
                    st->result_selected = 0;
                    st->result_skip     = 0;
                    st->result_has_more = false;
                    st->screen = SCREEN_BROWSER;
                } else if (st->home_selected == 1 &&
                           wiki_dataset_random_article(&st->current_dataset, esp_random(), &target)) {
                    open_article(st, &target);
                } else if (st->home_selected == 2 && wiki_dataset_main_page(&st->current_dataset, &target)) {
                    open_article(st, &target);
                } else if (st->home_selected == 3 && st->favorite_valid) {
                    open_article(st, &st->favorite_result);
                } else if (st->home_selected == 4) {
                    open_settings(st, SCREEN_HOME);
                }
            }
            break;

        case SCREEN_BROWSER:
            if (key == BSP_INPUT_NAVIGATION_KEY_ESC) {
                st->screen = SCREEN_HOME;
            } else if (key == BSP_INPUT_NAVIGATION_KEY_RETURN) {
                if (st->result_count > 0) {
                    open_article(st, &st->results[st->result_selected]);
                } else if (st->search_len > 0) {
                    refresh_results(st, 0);
                }
            } else if (key == BSP_INPUT_NAVIGATION_KEY_UP) {
                if (st->result_selected > 0) {
                    st->result_selected--;
                } else if (st->result_skip > 0) {
                    size_t new_skip = (st->result_skip > WIKI_MAX_RESULTS) ? st->result_skip - WIKI_MAX_RESULTS : 0;
                    refresh_results(st, new_skip);
                    st->result_selected = st->result_count > 0 ? st->result_count - 1 : 0;
                }
            } else if (key == BSP_INPUT_NAVIGATION_KEY_DOWN) {
                if (st->result_selected + 1 < st->result_count) {
                    st->result_selected++;
                } else if (st->result_has_more) {
                    refresh_results(st, st->result_skip + st->result_count);
                }
            } else if (key == BSP_INPUT_NAVIGATION_KEY_BACKSPACE) {
                if (st->search_len > 0) {
                    st->search_len--;
                    st->search_buf[st->search_len] = '\0';
                    st->result_count    = 0;
                    st->result_selected = 0;
                    st->result_skip     = 0;
                    st->result_has_more = false;
                }
            }
            break;

        case SCREEN_READER:
            if (key == BSP_INPUT_NAVIGATION_KEY_ESC) {
                reset_article(st);
                st->screen = SCREEN_HOME;
            } else if (key == BSP_INPUT_NAVIGATION_KEY_F4) {
                if (st->article.text) {
                    save_favorite(st);
                }
            } else if (key == BSP_INPUT_NAVIGATION_KEY_F6) {
                st->reader_links_enabled = !st->reader_links_enabled;
                st->reader_status[0] = '\0';
                screen_render(st);
                present_now(st);
                st->frame_presented = true;
            } else if (key == BSP_INPUT_NAVIGATION_KEY_LEFT) {
                if (st->reader_links_enabled && st->article.link_count > 0) {
                    st->reader_link_selected =
                        (st->reader_link_selected == 0) ? st->article.link_count - 1 : st->reader_link_selected - 1;
                    snprintf(st->reader_status, sizeof(st->reader_status), "Link: %.100s",
                             st->article.links[st->reader_link_selected].target);
                    ensure_selected_link_visible(st);
                }
            } else if (key == BSP_INPUT_NAVIGATION_KEY_RIGHT) {
                if (st->reader_links_enabled && st->article.link_count > 0) {
                    st->reader_link_selected = (st->reader_link_selected + 1) % st->article.link_count;
                    snprintf(st->reader_status, sizeof(st->reader_status), "Link: %.100s",
                             st->article.links[st->reader_link_selected].target);
                    ensure_selected_link_visible(st);
                }
            } else if (key == BSP_INPUT_NAVIGATION_KEY_RETURN) {
                if (st->reader_links_enabled && st->article.link_count > 0) {
                    wiki_result_t target;
                    ESP_LOGI(TAG, "Opening selected link %u/%u from '%s': %s",
                             (unsigned)(st->reader_link_selected + 1), (unsigned)st->article.link_count,
                             wiki_result_title(&st->current_result),
                             st->article.links[st->reader_link_selected].target);
                    if (wiki_result_from_zim_path(
                            &st->current_dataset, &st->current_result,
                            st->article.links[st->reader_link_selected].target, &target
                        )) {
                        open_article(st, &target);
                    } else {
                        snprintf(st->reader_status, sizeof(st->reader_status), "Link not found: %.96s",
                                 st->article.links[st->reader_link_selected].target);
                        ESP_LOGW(TAG, "Selected link not opened: %s",
                                 st->article.links[st->reader_link_selected].target);
                    }
                }
            } else if (key == BSP_INPUT_NAVIGATION_KEY_UP) {
                if (st->reader_scroll > 0) {
                    size_t old_scroll = st->reader_scroll;
                    st->reader_scroll =
                        (st->reader_scroll > READER_SCROLL_STEP) ? st->reader_scroll - READER_SCROLL_STEP : 0;
                    reader_fast_scroll(st, old_scroll, st->reader_scroll);
                }
            } else if (key == BSP_INPUT_NAVIGATION_KEY_DOWN) {
                if (st->reader_scroll + st->reader_visible_lines < st->article_line_count) {
                    size_t old_scroll = st->reader_scroll;
                    size_t max_scroll = st->article_line_count - st->reader_visible_lines;
                    st->reader_scroll += READER_SCROLL_STEP;
                    if (st->reader_scroll > max_scroll) {
                        st->reader_scroll = max_scroll;
                    }
                    reader_fast_scroll(st, old_scroll, st->reader_scroll);
                }
            } else if (key == BSP_INPUT_NAVIGATION_KEY_PGUP) {
                st->reader_scroll =
                    (st->reader_scroll > st->reader_visible_lines) ? st->reader_scroll - st->reader_visible_lines : 0;
            } else if (key == BSP_INPUT_NAVIGATION_KEY_PGDN) {
                if (st->article_line_count > st->reader_visible_lines) {
                    size_t max_scroll = st->article_line_count - st->reader_visible_lines;
                    st->reader_scroll += st->reader_visible_lines;
                    if (st->reader_scroll > max_scroll) {
                        st->reader_scroll = max_scroll;
                    }
                }
            } else if (key == BSP_INPUT_NAVIGATION_KEY_HOME) {
                st->reader_scroll = 0;
            } else if (key == BSP_INPUT_NAVIGATION_KEY_END) {
                st->reader_scroll =
                    (st->article_line_count > st->reader_visible_lines) ? st->article_line_count - st->reader_visible_lines : 0;
            }
            break;
    }
}

void screen_on_keyboard(app_state_t *st, char ascii) {
    if (st->screen != SCREEN_BROWSER) {
        ESP_LOGI(TAG, "Keyboard ignored outside browser: screen=%d ascii=%d", st->screen, (int)ascii);
        return;
    }
    if (ascii < 0x20 || ascii > 0x7e) {
        // Control characters (backspace, tab, ...) are handled as
        // navigation events instead.
        ESP_LOGI(TAG, "Keyboard ignored control ascii=%d", (int)ascii);
        return;
    }
    if (st->search_len + 1 >= sizeof(st->search_buf)) {
        ESP_LOGI(TAG, "Keyboard ignored full search buffer ascii='%c'", ascii);
        return;
    }
    st->search_buf[st->search_len++] = ascii;
    st->search_buf[st->search_len]   = '\0';
    ESP_LOGI(TAG, "Keyboard search char='%c' query='%s'", ascii, st->search_buf);
    st->result_count    = 0;
    st->result_selected = 0;
    st->result_skip     = 0;
    st->result_has_more = false;
}
