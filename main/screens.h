#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bsp/input.h"
#include "pax_gfx.h"
#include "wiki.h"

typedef enum {
    SCREEN_NO_CARD,
    SCREEN_DATASETS,
    SCREEN_HOME,
    SCREEN_BROWSER,
    SCREEN_READER,
} app_screen_t;

typedef struct {
    uint32_t offset;
    uint16_t length;
} wiki_text_line_t;

typedef struct {
    pax_buf_t   *fb;
    float        w;
    float        h;
    app_screen_t screen;

    // SCREEN_DATASETS
    wiki_dataset_info_t datasets[WIKI_MAX_DATASETS];
    size_t               dataset_count;
    size_t               dataset_selected;

    // Currently opened dataset (valid while browsing/reading)
    wiki_dataset_t current_dataset;
    bool           dataset_open;

    // SCREEN_HOME
    size_t        home_selected;
    wiki_result_t favorite_result;
    bool          favorite_valid;

    // SCREEN_BROWSER
    char          search_buf[48];
    size_t        search_len;
    wiki_result_t results[WIKI_MAX_RESULTS];
    size_t        result_count;
    size_t        result_selected;
    size_t        result_skip;
    bool          result_has_more;

    // SCREEN_READER
    wiki_result_t      current_result;
    wiki_article_t      article;
    wiki_text_line_t   *article_lines;
    size_t              article_line_count;
    size_t              article_line_capacity;
    size_t              reader_scroll;
    size_t              reader_visible_lines;
    size_t              reader_link_selected;
    char                reader_status[128];
} app_state_t;

// Sets up an empty app state for the given framebuffer/resolution.
void app_state_init(app_state_t *st, pax_buf_t *fb, float w, float h);

// (Re)scans the SD card for wiki datasets and switches to SCREEN_DATASETS, or
// SCREEN_NO_CARD if none were found / the card isn't mounted.
void screens_refresh_datasets(app_state_t *st);

// Draws the current screen into st->fb. Does not blit to the display.
void screen_render(app_state_t *st);

// Feeds a navigation key press (arrows, enter, escape, ...) to the current
// screen.
void screen_on_navigation(app_state_t *st, bsp_input_navigation_key_t key);

// Feeds a printable keyboard character to the current screen (used to type
// into the search box).
void screen_on_keyboard(app_state_t *st, char ascii);
