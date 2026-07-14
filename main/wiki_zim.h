#pragma once

// Minimal reader for the ZIM file format (https://wiki.openzim.org), as
// produced by Kiwix (https://library.kiwix.org/) for offline Wikipedia,
// Wikivoyage, Wiktionary, Stack Exchange, Project Gutenberg, etc. dumps.
//
// This only supports what's needed to browse/search by title and fetch an
// article's HTML body:
//   - "None" and "Zstd" compressed clusters (not the deprecated Lzma/Bzip2
//     compression some older ZIM files use -- those are rejected cleanly).
//   - Both the old and new ZIM namespace schemes (namespace byte is read but
//     ignored; we only care about titles, not path-based routing).
//   - Following a bounded number of redirect hops.
//
// The on-disk layout (verified against libzim's src/fileheader.cpp,
// src/dirent.cpp and src/cluster.cpp, and against a real downloaded ZIM
// file) is, in short:
//
//   Header (80 bytes)     magic, version, counts, and the positions below
//   MIME type list        NUL-terminated strings at mime_list_pos, ending
//                          in an empty string
//   Path pointer list      `entry_count` x uint64 file offsets of dirents,
//   (at path_ptr_pos)      indexed by raw entry index (creation order)
//   Title index list       `entry_count` x uint32 entry indices, but sorted
//   (at title_idx_pos)     by title -- this is what we binary search
//   Cluster pointer list    `cluster_count` x uint64 file offsets of
//   (at cluster_ptr_pos)    clusters
//   Dirents (variable)     mimetype/path/title/cluster+blob location
//   Clusters (variable)    optionally-compressed blob storage; each blob is
//                          one article/asset's raw content

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ff.h"

#define ZIM_MAX_MIMETYPES  128
#define ZIM_MAX_MIMETYPE_LEN 64
#define ZIM_MAX_TITLE_LEN  512
#define ZIM_MAX_PATH_LEN   512

typedef struct {
    FIL      file;
    bool     file_open;
    uint16_t major_version;
    uint16_t minor_version;
    uint32_t entry_count;    // total dirents, incl. redirects & assets
    uint32_t cluster_count;
    uint64_t path_ptr_pos;
    uint64_t title_idx_pos;  // UINT64_MAX if this archive has no title index
    uint64_t cluster_ptr_pos;
    uint64_t mime_list_pos;
    uint32_t main_page;      // entry index, or UINT32_MAX if none
    uint64_t checksum_pos;   // used as the implicit end of the last cluster
    uint64_t file_size;

    char   mime_types[ZIM_MAX_MIMETYPES][ZIM_MAX_MIMETYPE_LEN];
    size_t mime_type_count;
} zim_archive_t;

typedef struct {
    uint32_t entry_count;
    uint32_t main_page;
    uint64_t file_size;
} zim_probe_info_t;

typedef struct {
    bool     is_redirect;
    uint16_t mimetype_index;  // meaningless when is_redirect
    uint32_t cluster_number;  // meaningless when is_redirect
    uint32_t blob_number;     // meaningless when is_redirect
    uint32_t redirect_index;  // meaningless unless is_redirect
    char     path[ZIM_MAX_PATH_LEN];
    char     title[ZIM_MAX_TITLE_LEN];  // falls back to path if untitled
} zim_dirent_t;

// Opens a .zim file and parses its header, MIME type list and basic
// metadata. Returns false (and closes any partially-opened file) if the
// file isn't a ZIM file this reader supports.
bool zim_open(const char *path, zim_archive_t *out);

// Lightweight validation used during dataset discovery. Reads only the ZIM
// header and file size, so large archives still show up even if later metadata
// parsing fails.
bool zim_probe(const char *path, zim_probe_info_t *out);

void zim_close(zim_archive_t *zim);

// Number of entries reachable via the title index (0 if this archive has no
// title index, which real Kiwix downloads always do).
uint32_t zim_title_count(const zim_archive_t *zim);

// Reads the dirent at position `title_pos` (0..zim_title_count-1) of the
// title-sorted index.
bool zim_dirent_by_title_pos(zim_archive_t *zim, uint32_t title_pos, zim_dirent_t *out);

// Resolves a title-sorted position to the raw entry index that
// zim_dirent_by_entry_index() expects -- needed by callers (like wiki.c)
// that must remember "which article was this" across a search and a later
// load, since a title-sorted position alone isn't enough to re-fetch it.
bool zim_title_pos_to_entry_index(zim_archive_t *zim, uint32_t title_pos, uint32_t *out_entry_index);

// Reads the dirent at raw entry index `entry_index` (0..entry_count-1).
bool zim_dirent_by_entry_index(zim_archive_t *zim, uint32_t entry_index, zim_dirent_t *out);

// Finds a dirent by its exact ZIM path.
bool zim_dirent_by_path(zim_archive_t *zim, const char *path, uint32_t *out_entry_index, zim_dirent_t *out);

// If `dirent` is a redirect, follows it (up to `max_hops` times) and
// replaces *dirent with the final non-redirect entry. Returns false on a
// broken/looping redirect chain.
bool zim_resolve_redirect(zim_archive_t *zim, zim_dirent_t *dirent, int max_hops);
bool zim_resolve_redirect_with_index(zim_archive_t *zim, zim_dirent_t *dirent, uint32_t *entry_index, int max_hops);

// Returns the lowest title-index position whose title is not lexically
// before `prefix` (a case-sensitive lower_bound, matching ZIM's own sort
// order). Every title starting with `prefix` forms a contiguous run from
// this position onward.
uint32_t zim_title_lower_bound(zim_archive_t *zim, const char *prefix);

// Finds the lowest path-index position whose path is not lexically before
// `prefix`. This is a practical fallback for ZIMs without a usable title
// index; article paths are title-like in Kiwix Wikipedia dumps.
uint32_t zim_path_lower_bound(zim_archive_t *zim, const char *prefix);

const char *zim_mimetype_str(const zim_archive_t *zim, uint16_t index);

// Loads and (if needed) decompresses a dirent's full blob content.
// Caller must free() *out_data. *out_data is NOT NUL-terminated by this
// call; callers that treat it as text should account for out_size.
bool zim_load_blob(zim_archive_t *zim, const zim_dirent_t *dirent, uint8_t **out_data, size_t *out_size);
