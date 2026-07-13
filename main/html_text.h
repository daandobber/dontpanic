#pragma once

#include <stddef.h>
#include <stdint.h>

#define HTML_TEXT_MAX_LINKS 32
#define HTML_TEXT_LINK_TARGET_MAX 192

typedef struct {
    char target[HTML_TEXT_LINK_TARGET_MAX];
    uint32_t text_start;
    uint32_t text_end;
} html_text_link_t;

// Converts (rendered Kiwix/MediaWiki) HTML into plain text: tags are
// stripped, <script>/<style> contents are dropped entirely, common HTML
// entities are decoded, and block-level tags become line breaks.
//
// This is deliberately simple -- unlike the PC-side tools/wiki_pack.py
// (which uses BeautifulSoup to also drop navboxes/infoboxes/references by
// CSS selector), this on-device converter has no DOM and can't selectively
// remove those by class name. Expect more visual clutter in ZIM-backed
// articles than in datasets prepared with wiki_pack.py.
//
// Writes at most out_cap-1 bytes plus a NUL terminator into `out` (safe to
// call with out_cap==0). Returns the number of bytes the *untruncated*
// output would have needed (excluding the NUL), so callers can detect
// truncation by comparing against out_cap-1, matching snprintf's contract.
size_t html_to_text(const char *html, size_t html_len, char *out, size_t out_cap);

size_t html_to_text_with_links(
    const char *html, size_t html_len, char *out, size_t out_cap, html_text_link_t *links, size_t max_links,
    size_t *out_link_count
);
