#include "html_text.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    char             *out;
    size_t            out_cap;
    size_t            oi;
    int               trailing_newlines;  // caps collapsing at one blank line, like tools/wiki_pack.py's clean_body
    bool              pending_space;
    bool              any_output;
    html_text_link_t *links;
    size_t            max_links;
    size_t            link_count;
    int               active_link;
    uint32_t          active_link_start;
} sink_t;

static void put_char(sink_t *s, char c) {
    if (c == '\n') {
        if (s->trailing_newlines >= 2) {
            return;
        }
        s->trailing_newlines++;
        s->pending_space = false;
    } else if (c == ' ' || c == '\t') {
        if (!s->any_output || s->trailing_newlines > 0) {
            return;
        }
        s->pending_space = true;
        return;
    } else {
        if (s->pending_space) {
            if (s->oi + 1 < s->out_cap) {
                s->out[s->oi] = ' ';
            }
            s->oi++;
            s->pending_space = false;
        }
        s->trailing_newlines = 0;
    }

    if (s->oi + 1 < s->out_cap) {
        s->out[s->oi] = c;
    }
    s->oi++;
    s->any_output = true;
}

static void put_utf8(sink_t *s, uint32_t cp) {
    char   buf[4];
    size_t n = 0;
    if (cp == 0) {
        return;
    } else if (cp < 0x80) {
        buf[n++] = (char)cp;
    } else if (cp < 0x800) {
        buf[n++] = (char)(0xC0 | (cp >> 6));
        buf[n++] = (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        buf[n++] = (char)(0xE0 | (cp >> 12));
        buf[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        buf[n++] = (char)(0x80 | (cp & 0x3F));
    } else {
        buf[n++] = (char)(0xF0 | (cp >> 18));
        buf[n++] = (char)(0x80 | ((cp >> 12) & 0x3F));
        buf[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        buf[n++] = (char)(0x80 | (cp & 0x3F));
    }
    for (size_t k = 0; k < n; k++) {
        put_char(s, buf[k]);
    }
}

static bool is_tag_name_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == ':';
}

static bool tag_name_eq(const char *html, size_t start, size_t len, const char *name) {
    size_t nlen = strlen(name);
    if (len != nlen) {
        return false;
    }
    for (size_t k = 0; k < len; k++) {
        char c = html[start + k];
        if (c >= 'A' && c <= 'Z') {
            c = (char)(c - 'A' + 'a');
        }
        if (c != name[k]) {
            return false;
        }
    }
    return true;
}

static bool starts_ci(const char *s, const char *prefix) {
    for (size_t i = 0; prefix[i]; i++) {
        char a = s[i];
        char b = prefix[i];
        if (a >= 'A' && a <= 'Z') {
            a = (char)(a - 'A' + 'a');
        }
        if (b >= 'A' && b <= 'Z') {
            b = (char)(b - 'A' + 'a');
        }
        if (a != b) {
            return false;
        }
    }
    return true;
}

static bool href_is_internal(const char *href) {
    return href[0] != '\0' && href[0] != '#' && !starts_ci(href, "http:") && !starts_ci(href, "https:") &&
           !starts_ci(href, "mailto:") && !starts_ci(href, "javascript:");
}

static bool attr_name_eq(const char *html, size_t start, size_t end, const char *name) {
    size_t nlen = strlen(name);
    if (end < start || end - start != nlen) {
        return false;
    }
    for (size_t i = 0; i < nlen; i++) {
        char c = html[start + i];
        if (c >= 'A' && c <= 'Z') {
            c = (char)(c - 'A' + 'a');
        }
        if (c != name[i]) {
            return false;
        }
    }
    return true;
}

static bool extract_href(const char *html, size_t attr_start, size_t tag_end, char *out, size_t out_cap) {
    size_t p = attr_start;
    while (p < tag_end) {
        while (p < tag_end && (html[p] == ' ' || html[p] == '\t' || html[p] == '\r' || html[p] == '\n')) {
            p++;
        }
        size_t name_start = p;
        while (p < tag_end && is_tag_name_char(html[p])) {
            p++;
        }
        size_t name_end = p;
        while (p < tag_end && (html[p] == ' ' || html[p] == '\t' || html[p] == '\r' || html[p] == '\n')) {
            p++;
        }
        if (p >= tag_end || html[p] != '=') {
            while (p < tag_end && html[p] != ' ' && html[p] != '\t' && html[p] != '\r' && html[p] != '\n') {
                p++;
            }
            continue;
        }
        p++;
        while (p < tag_end && (html[p] == ' ' || html[p] == '\t' || html[p] == '\r' || html[p] == '\n')) {
            p++;
        }
        if (p >= tag_end) {
            break;
        }

        char quote = 0;
        if (html[p] == '"' || html[p] == '\'') {
            quote = html[p++];
        }
        size_t value_start = p;
        if (quote) {
            while (p < tag_end && html[p] != quote) {
                p++;
            }
        } else {
            while (p < tag_end && html[p] != ' ' && html[p] != '\t' && html[p] != '\r' && html[p] != '\n') {
                p++;
            }
        }
        size_t value_end = p;
        if (quote && p < tag_end) {
            p++;
        }

        if (attr_name_eq(html, name_start, name_end, "href")) {
            size_t len = value_end - value_start;
            if (len >= out_cap) {
                len = out_cap - 1;
            }
            memcpy(out, html + value_start, len);
            out[len] = '\0';
            return true;
        }
    }
    return false;
}

static void finish_active_link(sink_t *s) {
    if (s->active_link < 0) {
        return;
    }
    s->links[s->active_link].text_start = s->active_link_start;
    s->links[s->active_link].text_end   = (uint32_t)(s->oi > UINT32_MAX ? UINT32_MAX : s->oi);
    char ref[8];
    int  n = snprintf(ref, sizeof(ref), "[%d]", s->active_link + 1);
    put_char(s, ' ');
    for (int i = 0; i < n && ref[i]; i++) {
        put_char(s, ref[i]);
    }
    s->link_count++;
    s->active_link = -1;
}

static bool is_block_tag(const char *html, size_t start, size_t len) {
    static const char *const tags[] = {
        "p",     "div",       "br",        "li",     "h1",     "h2",     "h3", "h4", "h5", "h6",
        "tr",    "table",     "ul",        "ol",     "blockquote", "pre", "section", "article",
        "header", "footer",   "dd",        "dt",     "hr",     "figure", "figcaption",
    };
    for (size_t k = 0; k < sizeof(tags) / sizeof(tags[0]); k++) {
        if (tag_name_eq(html, start, len, tags[k])) {
            return true;
        }
    }
    return false;
}

static size_t find_ci(const char *html, size_t html_len, size_t from, const char *needle) {
    size_t nlen = strlen(needle);
    if (nlen == 0 || from + nlen > html_len) {
        return (size_t)-1;
    }
    for (size_t i = from; i + nlen <= html_len; i++) {
        bool match = true;
        for (size_t k = 0; k < nlen; k++) {
            char a = html[i + k];
            char b = needle[k];
            if (a >= 'A' && a <= 'Z') {
                a = (char)(a - 'A' + 'a');
            }
            if (b >= 'A' && b <= 'Z') {
                b = (char)(b - 'A' + 'a');
            }
            if (a != b) {
                match = false;
                break;
            }
        }
        if (match) {
            return i;
        }
    }
    return (size_t)-1;
}

static bool decode_entity(const char *html, size_t html_len, size_t i, size_t *entity_end, uint32_t *out_cp) {
    size_t start = i + 1;
    if (start >= html_len) {
        return false;
    }

    if (html[start] == '#') {
        size_t   p    = start + 1;
        bool     hexn = (p < html_len && (html[p] == 'x' || html[p] == 'X'));
        if (hexn) {
            p++;
        }
        size_t   digits_start = p;
        uint32_t cp           = 0;
        while (p < html_len) {
            char c = html[p];
            int  v;
            if (c >= '0' && c <= '9') {
                v = c - '0';
            } else if (hexn && c >= 'a' && c <= 'f') {
                v = c - 'a' + 10;
            } else if (hexn && c >= 'A' && c <= 'F') {
                v = c - 'A' + 10;
            } else {
                break;
            }
            cp = cp * (uint32_t)(hexn ? 16 : 10) + (uint32_t)v;
            p++;
        }
        if (p == digits_start) {
            return false;
        }
        if (p < html_len && html[p] == ';') {
            p++;
        }
        *entity_end = p;
        *out_cp      = cp;
        return true;
    }

    static const struct {
        const char *name;
        uint32_t    cp;
    } table[] = {
        {"amp", '&'},        {"lt", '<'},          {"gt", '>'},      {"quot", '"'},     {"apos", '\''},
        {"nbsp", ' '},       {"mdash", 0x2014},    {"ndash", 0x2013}, {"hellip", 0x2026}, {"copy", 0x00A9},
        {"reg", 0x00AE},     {"trade", 0x2122},    {"rsquo", 0x2019}, {"lsquo", 0x2018}, {"rdquo", 0x201D},
        {"ldquo", 0x201C},   {"times", 0x00D7},    {"divide", 0x00F7}, {"deg", 0x00B0},  {"shy", 0},
    };
    for (size_t k = 0; k < sizeof(table) / sizeof(table[0]); k++) {
        size_t nlen = strlen(table[k].name);
        if (start + nlen < html_len && html[start + nlen] == ';' &&
            strncmp(html + start, table[k].name, nlen) == 0) {
            *entity_end = start + nlen + 1;
            *out_cp      = table[k].cp;
            return true;
        }
    }
    return false;
}

size_t html_to_text_with_links(
    const char *html, size_t html_len, char *out, size_t out_cap, html_text_link_t *links, size_t max_links,
    size_t *out_link_count
) {
    sink_t s = {out, out_cap, 0, 2, false, false, links, max_links, 0, -1, 0};

    size_t i = 0;
    while (i < html_len) {
        char c = html[i];

        if (c == '<') {
            size_t tag_start = i + 1;
            bool   closing   = false;
            if (tag_start < html_len && html[tag_start] == '/') {
                closing = true;
                tag_start++;
            }
            size_t name_start = tag_start;
            size_t name_end   = name_start;
            while (name_end < html_len && is_tag_name_char(html[name_end])) {
                name_end++;
            }
            size_t name_len = name_end - name_start;

            size_t close_i   = name_end;
            bool   in_squote = false, in_dquote = false;
            while (close_i < html_len) {
                char cc = html[close_i];
                if (in_squote) {
                    if (cc == '\'') {
                        in_squote = false;
                    }
                } else if (in_dquote) {
                    if (cc == '"') {
                        in_dquote = false;
                    }
                } else if (cc == '\'') {
                    in_squote = true;
                } else if (cc == '"') {
                    in_dquote = true;
                } else if (cc == '>') {
                    break;
                }
                close_i++;
            }

            bool is_script = tag_name_eq(html, name_start, name_len, "script");
            bool is_style   = tag_name_eq(html, name_start, name_len, "style");
            bool is_anchor  = tag_name_eq(html, name_start, name_len, "a");

            if (!closing && (is_script || is_style)) {
                const char *close_tag  = is_script ? "</script" : "</style";
                size_t       search_from = (close_i < html_len) ? close_i + 1 : html_len;
                size_t       found       = find_ci(html, html_len, search_from, close_tag);
                if (found == (size_t)-1) {
                    i = html_len;
                } else {
                    size_t k = found;
                    while (k < html_len && html[k] != '>') {
                        k++;
                    }
                    i = (k < html_len) ? k + 1 : html_len;
                }
                continue;
            }

            if (is_anchor) {
                if (closing) {
                    finish_active_link(&s);
                } else if (s.links && s.link_count < s.max_links && s.active_link < 0) {
                    char href[HTML_TEXT_LINK_TARGET_MAX];
                    if (extract_href(html, name_end, close_i, href, sizeof(href)) && href_is_internal(href)) {
                        memcpy(s.links[s.link_count].target, href, strlen(href) + 1);
                        s.links[s.link_count].text_start = (uint32_t)(s.oi > UINT32_MAX ? UINT32_MAX : s.oi);
                        s.links[s.link_count].text_end   = s.links[s.link_count].text_start;
                        s.active_link       = (int)s.link_count;
                        s.active_link_start = s.links[s.link_count].text_start;
                    }
                }
            }

            if (is_block_tag(html, name_start, name_len)) {
                put_char(&s, '\n');
            }

            i = (close_i < html_len) ? close_i + 1 : html_len;
            continue;
        } else if (c == '&') {
            size_t   entity_end;
            uint32_t cp;
            if (decode_entity(html, html_len, i, &entity_end, &cp)) {
                put_utf8(&s, cp);
                i = entity_end;
                continue;
            }
            put_char(&s, '&');
            i++;
        } else {
            put_char(&s, c);
            i++;
        }
    }

    finish_active_link(&s);

    if (out_cap > 0) {
        size_t term = (s.oi < out_cap) ? s.oi : out_cap - 1;
        out[term]    = '\0';
    }
    if (out_link_count) {
        *out_link_count = s.link_count;
    }
    return s.oi;
}

size_t html_to_text(const char *html, size_t html_len, char *out, size_t out_cap) {
    return html_to_text_with_links(html, html_len, out, out_cap, NULL, 0, NULL);
}
