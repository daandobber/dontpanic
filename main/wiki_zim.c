#include "wiki_zim.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "zstd.h"

// Uses FatFs directly instead of newlib stdio. On ESP32-P4, stdio fread/fseek
// has been observed to crash on >2 GiB FAT32 files, while FatFs' FSIZE_t is
// an unsigned 32-bit offset and can address FAT32 files up to 4 GiB.

static const char *TAG = "zim";

#define ZIM_MAGIC          0x044D495Au
#define ZIM_HEADER_SIZE    80
#define ZIM_REDIRECT_MIME  0xFFFFu
#define ZIM_LINKTARGET_MIME 0xFFFEu
#define ZIM_DELETED_MIME   0xFFFDu
#define ZIM_MAX_CLUSTER_DECOMPRESSED (32u * 1024u * 1024u)

static void make_fatfs_path(const char *path, char *out, size_t out_cap) {
    if (strncmp(path, "/sdcard/", 8) == 0) {
        snprintf(out, out_cap, "0:/%s", path + 8);
    } else if (strcmp(path, "/sdcard") == 0) {
        snprintf(out, out_cap, "0:");
    } else {
        snprintf(out, out_cap, "%s", path);
    }
}

static bool read_exact(FIL *f, uint64_t offset, void *buf, size_t size) {
    if (offset > (uint64_t)UINT32_MAX || size > (size_t)UINT32_MAX) {
        ESP_LOGW(TAG, "read_exact rejected: offset=%llu size=%u", (unsigned long long)offset, (unsigned)size);
        return false;
    }
    if (f_lseek(f, (FSIZE_t)offset) != FR_OK) {
        ESP_LOGW(TAG, "f_lseek failed: offset=%llu size=%u", (unsigned long long)offset, (unsigned)size);
        return false;
    }
    UINT got = 0;
    FRESULT res = f_read(f, buf, (UINT)size, &got);
    if (res != FR_OK || got != (UINT)size) {
        ESP_LOGW(TAG, "f_read failed: offset=%llu want=%u got=%u res=%d",
                 (unsigned long long)offset, (unsigned)size, (unsigned)got, (int)res);
        return false;
    }
    return true;
}

static void *zim_malloc(size_t size) {
    void *ptr = NULL;
    if (size >= 64u * 1024u) {
        ptr = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (!ptr) {
        ptr = malloc(size);
    }
    return ptr;
}

static uint16_t le16(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t le64(const uint8_t *p) {
    return (uint64_t)le32(p) | ((uint64_t)le32(p + 4) << 32);
}

bool zim_probe(const char *path, zim_probe_info_t *out) {
    memset(out, 0, sizeof(*out));

    char fat_path[ZIM_MAX_PATH_LEN + 4];
    make_fatfs_path(path, fat_path, sizeof(fat_path));

    FIL f;
    if (f_open(&f, fat_path, FA_READ | FA_OPEN_EXISTING) != FR_OK) {
        return false;
    }

    uint8_t hdr[ZIM_HEADER_SIZE];
    if (!read_exact(&f, 0, hdr, sizeof(hdr)) || le32(hdr) != ZIM_MAGIC) {
        f_close(&f);
        return false;
    }

    f_close(&f);

    out->entry_count = le32(hdr + 24);
    out->main_page   = le32(hdr + 64);
    out->file_size   = 0;
    return true;
}

bool zim_open(const char *path, zim_archive_t *out) {
    memset(out, 0, sizeof(*out));

    char fat_path[ZIM_MAX_PATH_LEN + 4];
    make_fatfs_path(path, fat_path, sizeof(fat_path));

    if (f_open(&out->file, fat_path, FA_READ | FA_OPEN_EXISTING) != FR_OK) {
        return false;
    }
    out->file_open = true;

    uint8_t hdr[ZIM_HEADER_SIZE];
    if (!read_exact(&out->file, 0, hdr, sizeof(hdr))) {
        zim_close(out);
        return false;
    }

    if (le32(hdr) != ZIM_MAGIC) {
        zim_close(out);
        return false;
    }

    out->major_version  = le16(hdr + 4);
    out->minor_version  = le16(hdr + 6);
    out->entry_count     = le32(hdr + 24);
    out->cluster_count   = le32(hdr + 28);
    out->path_ptr_pos    = le64(hdr + 32);
    out->title_idx_pos   = le64(hdr + 40);
    out->cluster_ptr_pos = le64(hdr + 48);
    out->mime_list_pos   = le64(hdr + 56);
    out->main_page       = le32(hdr + 64);
    out->checksum_pos    = le64(hdr + 72);

    // Avoid seeking to EOF here: on some ESP-IDF/FATFS builds stdio EOF seek
    // can still fail above 2 GiB. ZIM headers include checksum_pos near EOF,
    // which is enough as a fallback boundary for the final cluster.
    out->file_size = out->checksum_pos;

    // MIME type list: NUL-terminated strings starting at mime_list_pos,
    // terminated by an empty string.
    uint64_t pos = out->mime_list_pos;
    while (out->mime_type_count < ZIM_MAX_MIMETYPES) {
        char buf[ZIM_MAX_MIMETYPE_LEN];
        size_t len = 0;
        bool   terminated = false;
        while (len + 1 < sizeof(buf)) {
            char c;
            if (!read_exact(&out->file, pos + len, &c, 1)) {
                break;
            }
            if (c == '\0') {
                terminated = true;
                break;
            }
            buf[len++] = c;
        }
        buf[len] = '\0';
        if (!terminated || len == 0) {
            break;
        }
        memcpy(out->mime_types[out->mime_type_count], buf, len + 1);
        out->mime_type_count++;
        pos += len + 1;
    }

    return true;
}

void zim_close(zim_archive_t *zim) {
    if (zim->file_open) {
        f_close(&zim->file);
        zim->file_open = false;
    }
}

uint32_t zim_title_count(const zim_archive_t *zim) {
    if (zim->title_idx_pos == UINT64_MAX || zim->title_idx_pos == 0) {
        return 0;
    }
    return zim->entry_count;
}

static bool read_dirent_at_offset(zim_archive_t *zim, uint64_t offset, zim_dirent_t *out) {
    memset(out, 0, sizeof(*out));

    // Dirents are small in practice; read a generous fixed window and bail
    // out if a field doesn't fit (rather than trying to grow dynamically).
    uint8_t buf[2048];
    size_t  got = 0;
    if (!read_exact(&zim->file, offset, buf, sizeof(buf))) {
        return false;
    }
    got = sizeof(buf);
    if (got < 8) {
        return false;
    }

    uint16_t mimetype  = le16(buf + 0);
    uint8_t  extra_len = buf[2];
    // buf[3] is the namespace byte; unused (we only browse/search by title).
    size_t pos = 8;  // mimetype(2) + extraLen(1) + ns(1) + version(4)

    out->mimetype_index = mimetype;
    out->is_redirect     = (mimetype == ZIM_REDIRECT_MIME);

    if (out->is_redirect) {
        if (pos + 4 > got) {
            return false;
        }
        out->redirect_index = le32(buf + pos);
        pos += 4;
    } else if (mimetype == ZIM_LINKTARGET_MIME || mimetype == ZIM_DELETED_MIME) {
        // No cluster/blob location for these (rare, not real content).
    } else {
        if (pos + 8 > got) {
            return false;
        }
        out->cluster_number = le32(buf + pos);
        out->blob_number    = le32(buf + pos + 4);
        pos += 8;
    }

    // path (NUL-terminated), then title (NUL-terminated, may be empty).
    size_t path_start = pos;
    size_t i          = path_start;
    while (i < got && buf[i] != '\0') {
        i++;
    }
    if (i >= got) {
        return false;  // path didn't fit in our read window
    }
    size_t path_len = i - path_start;
    if (path_len >= sizeof(out->path)) {
        path_len = sizeof(out->path) - 1;
    }
    memcpy(out->path, buf + path_start, path_len);
    out->path[path_len] = '\0';

    size_t title_start = i + 1;
    i                   = title_start;
    while (i < got && buf[i] != '\0') {
        i++;
    }
    if (i >= got) {
        return false;  // title didn't fit in our read window
    }
    size_t title_len = i - title_start;
    if (title_len == 0) {
        // Untitled entries display as their path.
        memcpy(out->title, out->path, path_len + 1);
    } else {
        if (title_len >= sizeof(out->title)) {
            title_len = sizeof(out->title) - 1;
        }
        memcpy(out->title, buf + title_start, title_len);
        out->title[title_len] = '\0';
    }

    (void)extra_len;  // "parameter" trailing bytes are never used by us
    return true;
}

bool zim_dirent_by_entry_index(zim_archive_t *zim, uint32_t entry_index, zim_dirent_t *out) {
    if (entry_index >= zim->entry_count) {
        return false;
    }
    uint8_t buf[8];
    if (!read_exact(&zim->file, zim->path_ptr_pos + (uint64_t)entry_index * 8, buf, 8)) {
        return false;
    }
    uint64_t dirent_offset = le64(buf);
    return read_dirent_at_offset(zim, dirent_offset, out);
}

bool zim_dirent_by_path(zim_archive_t *zim, const char *path, uint32_t *out_entry_index, zim_dirent_t *out) {
    uint32_t lo = 0;
    uint32_t hi = zim->entry_count;

    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        zim_dirent_t d;
        if (!zim_dirent_by_entry_index(zim, mid, &d)) {
            return false;
        }
        int cmp = strcmp(d.path, path);
        if (cmp < 0) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }

    if (lo >= zim->entry_count) {
        return false;
    }

    zim_dirent_t d;
    if (!zim_dirent_by_entry_index(zim, lo, &d) || strcmp(d.path, path) != 0) {
        return false;
    }

    if (out_entry_index) {
        *out_entry_index = lo;
    }
    if (out) {
        *out = d;
    }
    return true;
}

bool zim_title_pos_to_entry_index(zim_archive_t *zim, uint32_t title_pos, uint32_t *out_entry_index) {
    if (title_pos >= zim_title_count(zim)) {
        return false;
    }
    uint8_t buf[4];
    if (!read_exact(&zim->file, zim->title_idx_pos + (uint64_t)title_pos * 4, buf, 4)) {
        return false;
    }
    *out_entry_index = le32(buf);
    return true;
}

bool zim_dirent_by_title_pos(zim_archive_t *zim, uint32_t title_pos, zim_dirent_t *out) {
    uint32_t entry_index;
    if (!zim_title_pos_to_entry_index(zim, title_pos, &entry_index)) {
        return false;
    }
    return zim_dirent_by_entry_index(zim, entry_index, out);
}

bool zim_resolve_redirect(zim_archive_t *zim, zim_dirent_t *dirent, int max_hops) {
    return zim_resolve_redirect_with_index(zim, dirent, NULL, max_hops);
}

bool zim_resolve_redirect_with_index(zim_archive_t *zim, zim_dirent_t *dirent, uint32_t *entry_index, int max_hops) {
    int hops = 0;
    while (dirent->is_redirect) {
        if (hops++ >= max_hops) {
            return false;
        }
        uint32_t next_index = dirent->redirect_index;
        zim_dirent_t next;
        if (!zim_dirent_by_entry_index(zim, next_index, &next)) {
            return false;
        }
        *dirent = next;
        if (entry_index) {
            *entry_index = next_index;
        }
    }
    return true;
}

uint32_t zim_title_lower_bound(zim_archive_t *zim, const char *prefix) {
    uint32_t count = zim_title_count(zim);
    size_t   plen  = strlen(prefix);

    uint32_t lo = 0, hi = count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;

        zim_dirent_t d;
        if (!zim_dirent_by_title_pos(zim, mid, &d)) {
            break;
        }
        int cmp = strncasecmp(d.title, prefix, plen);
        if (cmp < 0) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo;
}

uint32_t zim_path_lower_bound(zim_archive_t *zim, const char *prefix) {
    uint32_t count = zim->entry_count;
    size_t   plen  = strlen(prefix);

    uint32_t lo = 0, hi = count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;

        zim_dirent_t d;
        if (!zim_dirent_by_entry_index(zim, mid, &d)) {
            break;
        }
        int cmp = strncmp(d.path, prefix, plen);
        if (cmp < 0) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo;
}

const char *zim_mimetype_str(const zim_archive_t *zim, uint16_t index) {
    if (index >= zim->mime_type_count) {
        return "";
    }
    return zim->mime_types[index];
}

static uint64_t cluster_end_offset(zim_archive_t *zim, uint32_t cluster_index, uint64_t this_offset) {
    if (cluster_index + 1 < zim->cluster_count) {
        uint8_t buf[8];
        if (read_exact(&zim->file, zim->cluster_ptr_pos + (uint64_t)(cluster_index + 1) * 8, buf, 8)) {
            return le64(buf);
        }
    }
    if (zim->checksum_pos > this_offset) {
        return zim->checksum_pos;
    }
    return zim->file_size;
}

bool zim_load_blob(zim_archive_t *zim, const zim_dirent_t *dirent, uint8_t **out_data, size_t *out_size) {
    *out_data = NULL;
    *out_size = 0;

    if (dirent->is_redirect) {
        ESP_LOGW(TAG, "blob load rejected redirect: path='%s' title='%s' redirect=%u",
                 dirent->path, dirent->title, (unsigned)dirent->redirect_index);
        return false;
    }

    uint8_t cluster_ptr_buf[8];
    if (!read_exact(&zim->file, zim->cluster_ptr_pos + (uint64_t)dirent->cluster_number * 8, cluster_ptr_buf, 8)) {
        ESP_LOGW(TAG, "cluster pointer read failed: cluster=%u path='%s'",
                 (unsigned)dirent->cluster_number, dirent->path);
        return false;
    }
    uint64_t cluster_offset = le64(cluster_ptr_buf);
    uint64_t cluster_end    = cluster_end_offset(zim, dirent->cluster_number, cluster_offset);
    if (cluster_end <= cluster_offset + 1) {
        ESP_LOGW(TAG, "bad cluster bounds: cluster=%u start=%llu end=%llu checksum=%llu",
                 (unsigned)dirent->cluster_number, (unsigned long long)cluster_offset,
                 (unsigned long long)cluster_end, (unsigned long long)zim->checksum_pos);
        return false;
    }

    uint8_t info;
    if (!read_exact(&zim->file, cluster_offset, &info, 1)) {
        ESP_LOGW(TAG, "cluster info read failed: cluster=%u offset=%llu",
                 (unsigned)dirent->cluster_number, (unsigned long long)cluster_offset);
        return false;
    }
    int  compression = info & 0x0F;
    bool extended     = (info & 0x10) != 0;

    size_t raw_size = (size_t)(cluster_end - cluster_offset - 1);
    uint8_t *raw    = zim_malloc(raw_size);
    if (!raw) {
        ESP_LOGW(TAG, "raw cluster malloc failed: cluster=%u raw_size=%u",
                 (unsigned)dirent->cluster_number, (unsigned)raw_size);
        return false;
    }
    if (!read_exact(&zim->file, cluster_offset + 1, raw, raw_size)) {
        free(raw);
        ESP_LOGW(TAG, "raw cluster read failed: cluster=%u offset=%llu raw_size=%u",
                 (unsigned)dirent->cluster_number, (unsigned long long)(cluster_offset + 1), (unsigned)raw_size);
        return false;
    }

    uint8_t *payload      = NULL;
    size_t   payload_size = 0;
    bool     owns_payload = false;

    if (compression == 1) {
        payload      = raw;
        payload_size = raw_size;
    } else if (compression == 5) {
        unsigned long long content_size = ZSTD_getFrameContentSize(raw, raw_size);
        if (content_size == ZSTD_CONTENTSIZE_ERROR ||
            (content_size != ZSTD_CONTENTSIZE_UNKNOWN && content_size > ZIM_MAX_CLUSTER_DECOMPRESSED)) {
            ESP_LOGW(TAG, "bad zstd content size: cluster=%u content_size=%llu raw_size=%u",
                     (unsigned)dirent->cluster_number, content_size, (unsigned)raw_size);
            free(raw);
            return false;
        }

        size_t capacity;
        if (content_size == ZSTD_CONTENTSIZE_UNKNOWN) {
            capacity = raw_size * 4;
            if (capacity < (256u * 1024u)) {
                capacity = 256u * 1024u;
            }
        } else {
            capacity = (size_t)content_size;
        }
        if (capacity == 0) {
            capacity = 1;
        }
        if (capacity > ZIM_MAX_CLUSTER_DECOMPRESSED) {
            capacity = ZIM_MAX_CLUSTER_DECOMPRESSED;
        }

        size_t result = 0;
        while (true) {
            payload = zim_malloc(capacity);
            if (!payload) {
                ESP_LOGW(TAG, "payload malloc failed: cluster=%u capacity=%u",
                         (unsigned)dirent->cluster_number, (unsigned)capacity);
                free(raw);
                return false;
            }

            result = ZSTD_decompress(payload, capacity, raw, raw_size);
            if (!ZSTD_isError(result)) {
                break;
            }

            ZSTD_ErrorCode code = ZSTD_getErrorCode(result);
            free(payload);
            payload = NULL;

            if (code != ZSTD_error_dstSize_tooSmall || capacity >= ZIM_MAX_CLUSTER_DECOMPRESSED) {
                ESP_LOGW(TAG, "zstd decompress failed: cluster=%u capacity=%u err=%s",
                         (unsigned)dirent->cluster_number, (unsigned)capacity, ZSTD_getErrorName(result));
                free(raw);
                return false;
            }
            capacity *= 2;
            if (capacity > ZIM_MAX_CLUSTER_DECOMPRESSED) {
                capacity = ZIM_MAX_CLUSTER_DECOMPRESSED;
            }
        }

        free(raw);
        raw = NULL;
        payload_size = result;
        owns_payload  = true;
    } else {
        // Lzma/Bzip2/unknown: not supported.
        ESP_LOGW(TAG, "unsupported cluster compression=%d: cluster=%u path='%s'",
                 compression, (unsigned)dirent->cluster_number, dirent->path);
        free(raw);
        return false;
    }

    size_t   off_size = extended ? 8 : 4;
    uint64_t first_offset;
    if (payload_size < off_size) {
        ESP_LOGW(TAG, "cluster payload too small for offsets: cluster=%u payload_size=%u off_size=%u",
                 (unsigned)dirent->cluster_number, (unsigned)payload_size, (unsigned)off_size);
        if (owns_payload) {
            free(payload);
        } else {
            free(raw);
        }
        return false;
    }
    first_offset = extended ? le64(payload) : le32(payload);

    uint64_t n_offsets = first_offset / off_size;
    uint32_t blob      = dirent->blob_number;
    if (blob + 1 >= n_offsets) {
        ESP_LOGW(TAG, "blob index out of range: cluster=%u blob=%u n_offsets=%llu first_offset=%llu",
                 (unsigned)dirent->cluster_number, (unsigned)blob,
                 (unsigned long long)n_offsets, (unsigned long long)first_offset);
        if (owns_payload) {
            free(payload);
        } else {
            free(raw);
        }
        return false;
    }

    uint64_t start_off, stop_off;
    if (extended) {
        start_off = le64(payload + blob * 8);
        stop_off  = le64(payload + (blob + 1) * 8);
    } else {
        start_off = le32(payload + blob * 4);
        stop_off  = le32(payload + (blob + 1) * 4);
    }

    if (stop_off < start_off || stop_off > payload_size) {
        ESP_LOGW(TAG, "bad blob bounds: cluster=%u blob=%u start=%llu stop=%llu payload_size=%u",
                 (unsigned)dirent->cluster_number, (unsigned)blob,
                 (unsigned long long)start_off, (unsigned long long)stop_off, (unsigned)payload_size);
        if (owns_payload) {
            free(payload);
        } else {
            free(raw);
        }
        return false;
    }

    size_t blob_size = (size_t)(stop_off - start_off);
    uint8_t *blob_data = zim_malloc(blob_size ? blob_size : 1);
    if (!blob_data) {
        ESP_LOGW(TAG, "blob malloc failed: cluster=%u blob=%u blob_size=%u",
                 (unsigned)dirent->cluster_number, (unsigned)blob, (unsigned)blob_size);
        if (owns_payload) {
            free(payload);
        } else {
            free(raw);
        }
        return false;
    }
    memcpy(blob_data, payload + start_off, blob_size);

    if (owns_payload) {
        free(payload);
    } else {
        free(raw);
    }

    *out_data = blob_data;
    *out_size = blob_size;
    return true;
}
