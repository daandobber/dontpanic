#include "wiki.h"

#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cJSON.h"
#include "esp_log.h"
#include "html_text.h"
#include "storage.h"

static char const TAG[] = "wiki";

#define WIKI_ROOT STORAGE_MOUNT_POINT "/wiki"

// On-disk record for the WIKI_BACKEND_FLAT format (tools/wiki_pack.py
// output). Kept private to this file; wiki_result_t is what the rest of the
// app sees.
typedef struct __attribute__((packed)) {
    char     title[WIKI_TITLE_MAX];  // NUL-padded, not guaranteed NUL-terminated
    uint32_t offset;
    uint32_t length;
} flat_record_t;

static void flat_title_cstr(const flat_record_t *rec, char out[WIKI_TITLE_MAX + 1]) {
    memcpy(out, rec->title, WIKI_TITLE_MAX);
    out[WIKI_TITLE_MAX] = '\0';
}

static void make_result(wiki_result_t *out, const char *title, size_t title_len) {
    memset(out, 0, sizeof(*out));
    if (title_len >= sizeof(out->title)) {
        title_len = sizeof(out->title) - 1;
    }
    memcpy(out->title, title, title_len);
    out->title[title_len] = '\0';
}

static bool file_exists(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        return false;
    }
    fclose(f);
    return true;
}

static bool has_suffix_ci(const char *s, const char *suffix) {
    size_t slen = strlen(s), suflen = strlen(suffix);
    if (suflen > slen) {
        return false;
    }
    return strcasecmp(s + (slen - suflen), suffix) == 0;
}

static uint32_t count_flat_records(const char *index_path) {
    FILE *f = fopen(index_path, "rb");
    if (!f) {
        return 0;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fclose(f);
    if (size <= 0) {
        return 0;
    }
    return (uint32_t)(size / sizeof(flat_record_t));
}

static void load_meta_json(const char *meta_path, wiki_dataset_info_t *info) {
    FILE *f = fopen(meta_path, "rb");
    if (!f) {
        return;
    }

    char   buf[2048];
    size_t len = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[len] = '\0';

    cJSON *root = cJSON_ParseWithLength(buf, len);
    if (!root) {
        return;
    }

    cJSON *name = cJSON_GetObjectItemCaseSensitive(root, "name");
    if (cJSON_IsString(name) && name->valuestring) {
        snprintf(info->name, sizeof(info->name), "%s", name->valuestring);
    }
    cJSON *description = cJSON_GetObjectItemCaseSensitive(root, "description");
    if (cJSON_IsString(description) && description->valuestring) {
        snprintf(info->description, sizeof(info->description), "%s", description->valuestring);
    }
    cJSON *language = cJSON_GetObjectItemCaseSensitive(root, "language");
    if (cJSON_IsString(language) && language->valuestring) {
        snprintf(info->language, sizeof(info->language), "%s", language->valuestring);
    }

    cJSON_Delete(root);
}

// Turns a filename like "wikipedia_en_simple_all_nopic_2026-05.zim" into a
// display name "wikipedia en simple all nopic 2026-05". Crude but avoids
// having to parse ZIM metadata dirents for a display label.
static void zim_display_name(const char *filename_no_ext, char *out, size_t out_cap) {
    size_t i = 0;
    for (; filename_no_ext[i] != '\0' && i + 1 < out_cap; i++) {
        char c   = filename_no_ext[i];
        out[i]    = (c == '_') ? ' ' : c;
    }
    out[i] = '\0';
}

size_t wiki_discover_datasets(wiki_dataset_info_t *out, size_t max_out) {
    size_t count = 0;

    DIR *dir = opendir(WIKI_ROOT);
    if (!dir) {
        ESP_LOGI(TAG, "No %s directory on SD card", WIKI_ROOT);
        return 0;
    }

    struct dirent *entry;
    while (count < max_out && (entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') {
            continue;
        }

        char full_path[192];
        snprintf(full_path, sizeof(full_path), "%s/%s", WIKI_ROOT, entry->d_name);

        if (has_suffix_ci(entry->d_name, ".zim")) {
            if (!file_exists(full_path)) {
                continue;
            }

            zim_probe_info_t probe;
            if (!zim_probe(full_path, &probe)) {
                ESP_LOGW(TAG, "Skipping invalid ZIM file: %s", full_path);
                continue;
            }

            wiki_dataset_info_t *info = &out[count];
            memset(info, 0, sizeof(*info));
            info->backend = WIKI_BACKEND_ZIM;

            size_t name_len = strlen(entry->d_name) - 4;  // strip ".zim"
            if (name_len >= sizeof(info->slug)) {
                name_len = sizeof(info->slug) - 1;
            }
            memcpy(info->slug, entry->d_name, name_len);
            info->slug[name_len] = '\0';

            zim_display_name(info->slug, info->name, sizeof(info->name));
            snprintf(info->description, sizeof(info->description), "Kiwix ZIM archive");
            snprintf(info->language, sizeof(info->language), "??");

            info->article_count = probe.entry_count;
            ESP_LOGI(TAG, "Found ZIM: %s (%u entries)", entry->d_name, (unsigned)info->article_count);

            count++;
            continue;
        }

        char index_path[192];
        char articles_path[192];
        char meta_path[192];
        snprintf(index_path, sizeof(index_path), "%s/index.bin", full_path);
        snprintf(articles_path, sizeof(articles_path), "%s/articles.dat", full_path);
        snprintf(meta_path, sizeof(meta_path), "%s/meta.json", full_path);

        if (!file_exists(index_path) || !file_exists(articles_path)) {
            continue;
        }

        wiki_dataset_info_t *info = &out[count];
        memset(info, 0, sizeof(*info));
        info->backend = WIKI_BACKEND_FLAT;
        snprintf(info->slug, sizeof(info->slug), "%s", entry->d_name);
        snprintf(info->name, sizeof(info->name), "%s", entry->d_name);
        snprintf(info->description, sizeof(info->description), "No description available.");
        snprintf(info->language, sizeof(info->language), "??");
        info->article_count = count_flat_records(index_path);

        load_meta_json(meta_path, info);

        count++;
    }

    closedir(dir);
    return count;
}

bool wiki_dataset_open(const wiki_dataset_info_t *info, wiki_dataset_t *ds) {
    memset(ds, 0, sizeof(*ds));
    ds->info = *info;

    if (info->backend == WIKI_BACKEND_ZIM) {
        char path[192];
        snprintf(path, sizeof(path), "%s/%s.zim", WIKI_ROOT, info->slug);
        ESP_LOGI(TAG, "Opening ZIM: %s", path);
        if (!zim_open(path, &ds->zim)) {
            ESP_LOGW(TAG, "Failed to open ZIM dataset '%s'", info->slug);
            return false;
        }
        ESP_LOGI(TAG, "Opened ZIM '%s': entries=%u clusters=%u checksum_pos=%llu", info->slug,
                 (unsigned)ds->zim.entry_count, (unsigned)ds->zim.cluster_count,
                 (unsigned long long)ds->zim.checksum_pos);
        return true;
    }

    char index_path[192];
    char articles_path[192];
    snprintf(index_path, sizeof(index_path), "%s/%s/index.bin", WIKI_ROOT, info->slug);
    snprintf(articles_path, sizeof(articles_path), "%s/%s/articles.dat", WIKI_ROOT, info->slug);

    ds->index_file    = fopen(index_path, "rb");
    ds->articles_file = fopen(articles_path, "rb");

    if (!ds->index_file || !ds->articles_file) {
        ESP_LOGW(TAG, "Failed to open dataset '%s'", info->slug);
        wiki_dataset_close(ds);
        return false;
    }

    return true;
}

void wiki_dataset_close(wiki_dataset_t *ds) {
    if (ds->info.backend == WIKI_BACKEND_ZIM) {
        zim_close(&ds->zim);
        return;
    }
    if (ds->index_file) {
        fclose(ds->index_file);
        ds->index_file = NULL;
    }
    if (ds->articles_file) {
        fclose(ds->articles_file);
        ds->articles_file = NULL;
    }
}

static bool read_flat_record(wiki_dataset_t *ds, size_t index, flat_record_t *out) {
    if (fseek(ds->index_file, (long)(index * sizeof(flat_record_t)), SEEK_SET) != 0) {
        return false;
    }
    return fread(out, sizeof(*out), 1, ds->index_file) == 1;
}

static size_t wiki_search_prefix_flat(
    wiki_dataset_t *ds, const char *prefix, size_t skip, wiki_result_t *out, size_t max_out, bool *out_has_more
) {
    size_t prefix_len = strnlen(prefix, WIKI_TITLE_MAX - 1);

    fseek(ds->index_file, 0, SEEK_END);
    long   file_size    = ftell(ds->index_file);
    size_t record_count = file_size > 0 ? (size_t)file_size / sizeof(flat_record_t) : 0;

    size_t lo = 0, hi = record_count;
    while (lo < hi) {
        size_t        mid = lo + (hi - lo) / 2;
        flat_record_t rec;
        if (!read_flat_record(ds, mid, &rec)) {
            break;
        }
        int cmp = strncasecmp(rec.title, prefix, prefix_len);
        if (cmp < 0) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }

    size_t        idx       = lo;
    size_t        matched   = 0;
    size_t        collected = 0;
    flat_record_t rec;
    while (idx < record_count && read_flat_record(ds, idx, &rec) && strncasecmp(rec.title, prefix, prefix_len) == 0) {
        if (matched >= skip) {
            if (collected < max_out) {
                char title[WIKI_TITLE_MAX + 1];
                flat_title_cstr(&rec, title);
                make_result(&out[collected], title, strlen(title));
                out[collected].flat.offset = rec.offset;
                out[collected].flat.length = rec.length;
                collected++;
            } else {
                if (out_has_more) {
                    *out_has_more = true;
                }
                break;
            }
        }
        matched++;
        idx++;
    }

    return collected;
}

static bool zim_entry_is_browsable(zim_archive_t *zim, const zim_dirent_t *d) {
    if (d->is_redirect) {
        return true;
    }
    const char *mt = zim_mimetype_str(zim, d->mimetype_index);
    if (strncmp(mt, "text/html", 9) == 0) {
        return true;
    }

    // Some large Kiwix ZIMs use MIME indexes beyond the subset we managed to
    // read from the MIME list. If the entry has article-like metadata and a
    // resolvable blob, let the loader try it instead of rejecting valid links.
    return mt[0] == '\0' && d->title[0] != '\0' && d->path[0] != '\0';
}

static void search_prefix_to_path_prefix(const char *prefix, char *out, size_t out_cap, bool title_case) {
    if (out_cap == 0) {
        return;
    }
    size_t out_len = 0;
    bool capitalize_next = title_case;
    for (size_t i = 0; prefix[i] && out_len + 1 < out_cap; i++) {
        char c = prefix[i] == ' ' ? '_' : prefix[i];
        if (capitalize_next && c >= 'a' && c <= 'z') {
            c = (char)(c - ('a' - 'A'));
        }
        capitalize_next = c == '_' || c == '-' || c == '(';
        out[out_len++] = c;
    }
    out[out_len] = '\0';
}

static bool result_entry_seen(const wiki_result_t *out, size_t count, uint32_t entry_index) {
    for (size_t i = 0; i < count; i++) {
        if (out[i].zim.entry_index == entry_index) {
            return true;
        }
    }
    return false;
}

static void collect_zim_path_prefix(
    wiki_dataset_t *ds, const char *path_prefix, size_t skip, wiki_result_t *out, size_t max_out,
    bool *out_has_more, size_t *matched, size_t *collected, uint32_t *out_scanned
) {
    uint32_t start      = zim_path_lower_bound(&ds->zim, path_prefix);
    uint32_t total      = ds->zim.entry_count;
    size_t   prefix_len = strlen(path_prefix);
    uint32_t pos        = start;
    uint32_t scanned    = 0;

    while (pos < total && scanned < 5000) {
        zim_dirent_t d;
        if (!zim_dirent_by_entry_index(&ds->zim, pos, &d)) {
            break;
        }
        if (strncmp(d.path, path_prefix, prefix_len) != 0) {
            break;
        }
        scanned++;

        uint32_t entry_index = pos;
        pos++;

        if (!zim_entry_is_browsable(&ds->zim, &d)) {
            continue;
        }
        if (result_entry_seen(out, *collected, entry_index)) {
            continue;
        }

        if (*matched >= skip) {
            if (*collected < max_out) {
                make_result(&out[*collected], d.title, strlen(d.title));
                out[*collected].zim.entry_index = entry_index;
                (*collected)++;
            } else {
                if (out_has_more) {
                    *out_has_more = true;
                }
                break;
            }
        }
        (*matched)++;
    }

    if (out_scanned) {
        *out_scanned += scanned;
    }
    ESP_LOGI(TAG, "ZIM path search prefix='%s' start=%u scanned=%u matched=%u collected=%u more=%d",
             path_prefix, (unsigned)start, (unsigned)scanned, (unsigned)*matched, (unsigned)*collected,
             out_has_more ? *out_has_more : false);
}

static size_t wiki_search_prefix_zim(
    wiki_dataset_t *ds, const char *prefix, size_t skip, wiki_result_t *out, size_t max_out, bool *out_has_more
) {
    size_t   matched   = 0;
    size_t   collected = 0;
    uint32_t scanned    = 0;

    char path_prefix[ZIM_MAX_PATH_LEN];
    char title_path_prefix[ZIM_MAX_PATH_LEN];
    search_prefix_to_path_prefix(prefix, path_prefix, sizeof(path_prefix), false);
    search_prefix_to_path_prefix(prefix, title_path_prefix, sizeof(title_path_prefix), true);

    collect_zim_path_prefix(ds, path_prefix, skip, out, max_out, out_has_more, &matched, &collected, &scanned);
    if (collected < max_out && strcmp(title_path_prefix, path_prefix) != 0) {
        collect_zim_path_prefix(ds, title_path_prefix, skip, out, max_out, out_has_more, &matched, &collected, &scanned);
    }

    ESP_LOGI(TAG, "ZIM search prefix='%s' skip=%u scanned=%u collected=%u more=%d",
             prefix, (unsigned)skip, (unsigned)scanned, (unsigned)collected, out_has_more ? *out_has_more : false);
    return collected;
}

size_t wiki_search_prefix(
    wiki_dataset_t *ds, const char *prefix, size_t skip, wiki_result_t *out, size_t max_out, bool *out_has_more
) {
    if (out_has_more) {
        *out_has_more = false;
    }
    if (!ds || max_out == 0) {
        return 0;
    }
    if (ds->info.backend == WIKI_BACKEND_ZIM) {
        return wiki_search_prefix_zim(ds, prefix, skip, out, max_out, out_has_more);
    }
    return wiki_search_prefix_flat(ds, prefix, skip, out, max_out, out_has_more);
}

static bool wiki_load_article_flat(wiki_dataset_t *ds, const wiki_result_t *result, char **out_text, size_t *out_len) {
    size_t length = result->flat.length;
    if (length > WIKI_ARTICLE_MAX_BYTES) {
        length = WIKI_ARTICLE_MAX_BYTES;
    }

    char *buf = malloc(length + 1);
    if (!buf) {
        return false;
    }
    if (fseek(ds->articles_file, (long)result->flat.offset, SEEK_SET) != 0) {
        free(buf);
        return false;
    }
    size_t read = fread(buf, 1, length, ds->articles_file);
    buf[read]    = '\0';

    *out_text = buf;
    *out_len  = read;
    return true;
}

static bool wiki_load_article_zim(wiki_dataset_t *ds, const wiki_result_t *result, char **out_text, size_t *out_len) {
    zim_dirent_t d;
    if (!zim_dirent_by_entry_index(&ds->zim, result->zim.entry_index, &d)) {
        ESP_LOGW(TAG, "ZIM load failed: cannot read dirent entry=%u title='%s'",
                 (unsigned)result->zim.entry_index, wiki_result_title(result));
        return false;
    }
    if (!zim_resolve_redirect(&ds->zim, &d, 8)) {
        ESP_LOGW(TAG, "ZIM load failed: redirect resolve failed entry=%u path='%s' title='%s'",
                 (unsigned)result->zim.entry_index, d.path, d.title);
        return false;
    }

    uint8_t *blob;
    size_t   blob_size;
    if (!zim_load_blob(&ds->zim, &d, &blob, &blob_size)) {
        ESP_LOGW(TAG, "ZIM load failed: blob load failed entry=%u path='%s' title='%s' mime=%u cluster=%u blob=%u",
                 (unsigned)result->zim.entry_index, d.path, d.title, (unsigned)d.mimetype_index,
                 (unsigned)d.cluster_number, (unsigned)d.blob_number);
        return false;
    }
    if (blob_size > WIKI_ARTICLE_MAX_BYTES) {
        blob_size = WIKI_ARTICLE_MAX_BYTES;
    }

    size_t out_cap = blob_size + 1;
    char  *text     = malloc(out_cap);
    if (!text) {
        ESP_LOGW(TAG, "ZIM load failed: text malloc failed entry=%u bytes=%u path='%s'",
                 (unsigned)result->zim.entry_index, (unsigned)out_cap, d.path);
        free(blob);
        return false;
    }
    size_t written = html_to_text((const char *)blob, blob_size, text, out_cap);
    free(blob);
    if (written >= out_cap) {
        written = out_cap - 1;
    }
    *out_text = text;
    *out_len  = written;
    return true;
}

bool wiki_load_article(wiki_dataset_t *ds, const wiki_result_t *result, char **out_text, size_t *out_len) {
    *out_text = NULL;
    *out_len  = 0;
    if (!ds) {
        return false;
    }
    if (ds->info.backend == WIKI_BACKEND_ZIM) {
        return wiki_load_article_zim(ds, result, out_text, out_len);
    }
    return wiki_load_article_flat(ds, result, out_text, out_len);
}

void wiki_article_free(wiki_article_t *article) {
    if (!article) {
        return;
    }
    free(article->text);
    memset(article, 0, sizeof(*article));
}

static bool wiki_load_article_zim_ex(wiki_dataset_t *ds, const wiki_result_t *result, wiki_article_t *out) {
    zim_dirent_t d;
    if (!zim_dirent_by_entry_index(&ds->zim, result->zim.entry_index, &d)) {
        ESP_LOGW(TAG, "ZIM load failed: cannot read dirent entry=%u title='%s'",
                 (unsigned)result->zim.entry_index, wiki_result_title(result));
        return false;
    }
    if (!zim_resolve_redirect(&ds->zim, &d, 8)) {
        ESP_LOGW(TAG, "ZIM load failed: redirect resolve failed entry=%u path='%s' title='%s'",
                 (unsigned)result->zim.entry_index, d.path, d.title);
        return false;
    }

    uint8_t *blob;
    size_t   blob_size;
    if (!zim_load_blob(&ds->zim, &d, &blob, &blob_size)) {
        ESP_LOGW(TAG, "ZIM load failed: blob load failed entry=%u path='%s' title='%s' mime=%u cluster=%u blob=%u",
                 (unsigned)result->zim.entry_index, d.path, d.title, (unsigned)d.mimetype_index,
                 (unsigned)d.cluster_number, (unsigned)d.blob_number);
        return false;
    }
    if (blob_size > WIKI_ARTICLE_MAX_BYTES) {
        blob_size = WIKI_ARTICLE_MAX_BYTES;
    }

    size_t out_cap = blob_size + 1;
    char  *text    = malloc(out_cap);
    if (!text) {
        ESP_LOGW(TAG, "ZIM load failed: text malloc failed entry=%u bytes=%u path='%s'",
                 (unsigned)result->zim.entry_index, (unsigned)out_cap, d.path);
        free(blob);
        return false;
    }
    size_t link_count = 0;
    size_t written =
        html_to_text_with_links((const char *)blob, blob_size, text, out_cap, out->links, HTML_TEXT_MAX_LINKS, &link_count);
    free(blob);
    if (written >= out_cap) {
        written = out_cap - 1;
    }
    ESP_LOGI(TAG, "Article loaded: entry=%u path='%s' title='%s' mime=%u blob=%u text=%u links=%u",
             (unsigned)result->zim.entry_index, d.path, d.title, (unsigned)d.mimetype_index,
             (unsigned)blob_size, (unsigned)written, (unsigned)link_count);
    out->text       = text;
    out->len        = written;
    out->link_count = link_count;
    return true;
}

bool wiki_load_article_ex(wiki_dataset_t *ds, const wiki_result_t *result, wiki_article_t *out) {
    memset(out, 0, sizeof(*out));
    if (!ds) {
        return false;
    }
    if (ds->info.backend == WIKI_BACKEND_ZIM) {
        return wiki_load_article_zim_ex(ds, result, out);
    }
    return wiki_load_article_flat(ds, result, &out->text, &out->len);
}

bool wiki_dataset_main_page(wiki_dataset_t *ds, wiki_result_t *out) {
    if (!ds || ds->info.backend != WIKI_BACKEND_ZIM || ds->zim.main_page == UINT32_MAX) {
        ESP_LOGW(TAG, "Main page unavailable: ds=%p backend=%d main_page=%u",
                 (void *)ds, ds ? (int)ds->info.backend : -1, ds ? (unsigned)ds->zim.main_page : 0);
        return false;
    }
    zim_dirent_t d;
    if (!zim_dirent_by_entry_index(&ds->zim, ds->zim.main_page, &d)) {
        ESP_LOGW(TAG, "Main page unavailable: dirent read failed entry=%u", (unsigned)ds->zim.main_page);
        return false;
    }
    if (!zim_entry_is_browsable(&ds->zim, &d)) {
        ESP_LOGW(TAG, "Main page unavailable: not browsable entry=%u path='%s' title='%s' mime=%u redirect=%d",
                 (unsigned)ds->zim.main_page, d.path, d.title, (unsigned)d.mimetype_index, d.is_redirect);
        return false;
    }
    make_result(out, d.title, strlen(d.title));
    out->zim.entry_index = ds->zim.main_page;
    return true;
}

bool wiki_dataset_random_article(wiki_dataset_t *ds, uint32_t seed, wiki_result_t *out) {
    if (!ds || ds->info.backend != WIKI_BACKEND_ZIM || ds->zim.entry_count == 0) {
        return false;
    }
    uint32_t start = seed % ds->zim.entry_count;
    for (uint32_t n = 0; n < ds->zim.entry_count && n < 4000; n++) {
        uint32_t idx = (start + n * 97u) % ds->zim.entry_count;
        zim_dirent_t d;
        if (!zim_dirent_by_entry_index(&ds->zim, idx, &d)) {
            continue;
        }
        if (!zim_entry_is_browsable(&ds->zim, &d)) {
            continue;
        }
        make_result(out, d.title, strlen(d.title));
        out->zim.entry_index = idx;
        return true;
    }
    return false;
}

static void strip_fragment_and_query(char *s) {
    for (size_t i = 0; s[i]; i++) {
        if (s[i] == '#' || s[i] == '?') {
            s[i] = '\0';
            return;
        }
    }
}

static int hex_value(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static void url_decode_in_place(char *s) {
    size_t w = 0;
    for (size_t r = 0; s[r]; r++) {
        if (s[r] == '%' && s[r + 1] && s[r + 2]) {
            int hi = hex_value(s[r + 1]);
            int lo = hex_value(s[r + 2]);
            if (hi >= 0 && lo >= 0) {
                s[w++] = (char)((hi << 4) | lo);
                r += 2;
                continue;
            }
        }
        s[w++] = s[r];
    }
    s[w] = '\0';
}

static void collapse_path(char *path) {
    char result[ZIM_MAX_PATH_LEN] = "";
    char work[ZIM_MAX_PATH_LEN];
    snprintf(work, sizeof(work), "%s", path);

    char *segments[48];
    size_t count = 0;
    char *save = NULL;
    for (char *seg = strtok_r(work, "/", &save); seg && count < sizeof(segments) / sizeof(segments[0]);
         seg = strtok_r(NULL, "/", &save)) {
        if (strcmp(seg, ".") == 0 || seg[0] == '\0') {
            continue;
        }
        if (strcmp(seg, "..") == 0) {
            if (count > 0) {
                count--;
            }
            continue;
        }
        segments[count++] = seg;
    }

    for (size_t i = 0; i < count; i++) {
        if (i > 0) {
            strncat(result, "/", sizeof(result) - strlen(result) - 1);
        }
        strncat(result, segments[i], sizeof(result) - strlen(result) - 1);
    }
    snprintf(path, ZIM_MAX_PATH_LEN, "%s", result);
}

static void normalize_link_path(const char *base_path, const char *href, char *out, size_t out_cap) {
    char tmp[ZIM_MAX_PATH_LEN];
    if (href[0] == '/') {
        snprintf(tmp, sizeof(tmp), "%s", href + 1);
    } else {
        const char *slash = strrchr(base_path, '/');
        if (slash) {
            size_t dir_len = (size_t)(slash - base_path + 1);
            if (dir_len >= sizeof(tmp)) {
                dir_len = sizeof(tmp) - 1;
            }
            memcpy(tmp, base_path, dir_len);
            tmp[dir_len] = '\0';
            strncat(tmp, strncmp(href, "./", 2) == 0 ? href + 2 : href, sizeof(tmp) - strlen(tmp) - 1);
        } else {
            snprintf(tmp, sizeof(tmp), "%s", strncmp(href, "./", 2) == 0 ? href + 2 : href);
        }
    }
    strip_fragment_and_query(tmp);
    url_decode_in_place(tmp);
    collapse_path(tmp);
    snprintf(out, out_cap, "%s", tmp);
}

static void path_to_title_candidate(const char *path, char *out, size_t out_cap) {
    const char *name = strrchr(path, '/');
    name             = name ? name + 1 : path;

    size_t w = 0;
    for (size_t r = 0; name[r] && w + 1 < out_cap; r++) {
        out[w++] = (name[r] == '_') ? ' ' : name[r];
    }
    out[w] = '\0';
}

static bool title_starts_with(const char *title, const char *prefix) {
    return strncmp(title, prefix, strlen(prefix)) == 0;
}

static bool zim_find_path_fast(zim_archive_t *zim, const char *path, uint32_t *out_entry_index, zim_dirent_t *out_dirent) {
    char title[ZIM_MAX_TITLE_LEN];
    path_to_title_candidate(path, title, sizeof(title));
    if (!title[0]) {
        return false;
    }

    uint32_t pos   = zim_title_lower_bound(zim, title);
    uint32_t count = zim_title_count(zim);
    uint32_t limit = pos + 96;
    if (limit > count) {
        limit = count;
    }

    for (uint32_t p = pos; p < limit; p++) {
        uint32_t entry_index;
        if (!zim_title_pos_to_entry_index(zim, p, &entry_index)) {
            continue;
        }
        zim_dirent_t d;
        if (!zim_dirent_by_entry_index(zim, entry_index, &d)) {
            continue;
        }
        if (!title_starts_with(d.title, title)) {
            break;
        }
        if (strcmp(d.path, path) == 0 || strcmp(d.title, title) == 0) {
            *out_entry_index = entry_index;
            *out_dirent      = d;
            return true;
        }
    }

    ESP_LOGW(TAG, "Link target not found quickly: path='%s' title='%s' start_pos=%u", path, title, (unsigned)pos);
    return false;
}

bool wiki_result_from_zim_path(wiki_dataset_t *ds, const wiki_result_t *current, const char *href, wiki_result_t *out) {
    if (!ds || ds->info.backend != WIKI_BACKEND_ZIM || !href || !href[0]) {
        return false;
    }
    zim_dirent_t current_dirent;
    if (!zim_dirent_by_entry_index(&ds->zim, current->zim.entry_index, &current_dirent)) {
        return false;
    }

    char path[ZIM_MAX_PATH_LEN];
    normalize_link_path(current_dirent.path, href, path, sizeof(path));
    if (!path[0]) {
        return false;
    }
    ESP_LOGI(TAG, "Opening link: current='%s' href='%s' normalized='%s'", current_dirent.path, href, path);

    uint32_t    entry_index;
    zim_dirent_t d;
    if (!zim_dirent_by_path(&ds->zim, path, &entry_index, &d) &&
        !zim_find_path_fast(&ds->zim, path, &entry_index, &d)) {
        return false;
    }
    if (!zim_resolve_redirect_with_index(&ds->zim, &d, &entry_index, 8) || !zim_entry_is_browsable(&ds->zim, &d)) {
        ESP_LOGW(TAG, "Link target rejected: path='%s' resolved_path='%s' title='%s' entry=%u mime=%u mt='%s'",
                 path, d.path, d.title, (unsigned)entry_index, (unsigned)d.mimetype_index,
                 zim_mimetype_str(&ds->zim, d.mimetype_index));
        return false;
    }
    make_result(out, d.title, strlen(d.title));
    out->zim.entry_index = entry_index;
    return true;
}
