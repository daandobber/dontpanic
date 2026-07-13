# Don't Panic — an offline Hitchhiker's Guide for Tanmatsu

An offline encyclopedia reader for the [Tanmatsu](https://docs.tanmatsu.cloud)
badge, styled after the "Book" graphics from the 1981 BBC TV adaptation of
*The Hitchhiker's Guide to the Galaxy*: black CRT background, phosphor green
body text, amber headers, and a monospace terminal font.

The app reads wiki datasets off an SD card and is usable fully offline. You
choose which dataset to browse on the device itself — drop as many
`/wiki/<name>/` folders or `/wiki/<name>.zim` files onto the card as you like
(a Simple English Wikipedia dump, a fan wiki, your own notes, ...) and they
all show up in the dataset picker. The recommended way to add datasets is to
download them on a computer and copy them to the SD card; reading never touches
the network.

This is built on top of the [Tanmatsu app template](https://github.com/Nicolai-Electronics/tanmatsu-template),
using the [PAX graphics](https://github.com/robotman2412/pax-graphics/tree/release/1.1.1/docs)
library.

## Controls

| Screen | Keys |
|---|---|
| Dataset picker | Up/Down select, Enter open, Esc/F1 exit to launcher |
| Browse & search | Type to search, Up/Down select, Enter open, Esc back |
| Article reader | Up/Down scroll, Left/Right select link, Enter open link, PgUp/PgDn page, Home/End jump, Esc back |
| Any screen | F1 restart to launcher, F2/F3 backlight down/up |

If no SD card is found, the app shows a "NO DATA CARTRIDGE DETECTED" screen;
press Enter to retry after inserting a card. If the card is present but has
no `/wiki` content yet, copy a ZIM file or generated dataset folder to the SD
card as described below.

## Adding wikis to the SD card

This is the user-facing path. It is much faster and more reliable than letting
the badge download large files over WiFi.

1. Put the SD card in your computer.
2. Create a folder named `wiki` on the SD card if it does not already exist.
3. Download a `.zim` file from https://library.kiwix.org/ or
   https://download.kiwix.org/zim/.
4. Prefer `nopic` ZIM files. They are much smaller and avoid image-heavy files
   that are too large for FAT32.
5. Copy the `.zim` file to the SD card under `/wiki/`.
6. Eject the SD card safely, put it back in the Tanmatsu, and start Don't
   Panic.

Example SD card layout:

```text
/wiki/
  wikivoyage_en_all_nopic_2026-06.zim
  wikipedia_en_simple_all_nopic_2026-05.zim
```

The app also supports preprocessed dataset folders generated with
`tools/wiki_pack.py`:

```text
/wiki/
  h2g2-sampler/
    meta.json
    index.bin
    articles.dat
```

Use the preprocessed folder format when you want cleaner plain text and better
control over filtering. Use direct `.zim` files when you want the simplest
copy-to-card workflow.

`main/wiki_zim.c` reads the ZIM container format natively on the device
(header, title index, clusters) and decompresses Zstd-compressed clusters
with a vendored single-file build of [zstd](https://github.com/facebook/zstd)
(`components/zstddeclib`); `main/html_text.c` strips the rendered HTML down
to plain text for the reader. Browse https://library.kiwix.org/ for available
ZIM files.

### On-device ZIM reader limitations

- Only "None" and "Zstd" cluster compression are supported (modern Kiwix
  downloads use Zstd by default). Older ZIM files using LZMA/Bzip2
  compression will fail to open.
- ZIM files are read through FatFs with 32-bit file offsets, so FAT32's
  practical 4 GiB per-file limit is the upper bound. Prefer `nopic` files
  because image-heavy editions are too large and slow for this device.
- HTML-to-text conversion is a simple tag stripper with no DOM, unlike
  `tools/wiki_pack.py --zim`'s BeautifulSoup-based conversion (used for
  datasets prepared on a PC) — it can't selectively drop navboxes/infoboxes/
  references by class name, so direct ZIM articles can look noisier than ones
  packed with `wiki_pack.py`.
- There's no on-device full-text catalog search.

## Building

Same as any Tanmatsu app template project — see the upstream
[tanmatsu-template](https://github.com/Nicolai-Electronics/tanmatsu-template)
README and [docs.tanmatsu.cloud](https://docs.tanmatsu.cloud) for toolchain
setup. In short, with ESP-IDF >= 5.5.1 and the Tanmatsu `sdkconfig` merged in:

```sh
idf.py build
idf.py flash monitor   # or `make badgelink` per the template's Makefile
```

This app only supports the Tanmatsu / Konsool board (it talks to the SD card
slot directly using that board's pin wiring in `main/storage.c`); building
for the other targets under `sdkconfigs/` will fail with a `#error` rather
than silently mounting the wrong pins.

## Preparing wiki datasets

Each dataset is a folder placed at `/wiki/<slug>/` on the SD card, containing:

- `meta.json` — `{"name", "description", "language"}`, shown in the picker
- `index.bin` — fixed-width records (title + offset + length into
  `articles.dat`), sorted by lowercase title so the firmware can binary
  search it directly from the SD card
- `articles.dat` — the concatenated raw article bodies (plain UTF-8 text,
  no markup)

You don't write these by hand — `tools/wiki_pack.py` generates them. It reads
from one of four input shapes:

```sh
# Easiest: a Kiwix .zim file. https://library.kiwix.org/ has ready-made
# offline dumps of Wikipedia (every language and size), Wikivoyage,
# Wiktionary, Stack Exchange, Project Gutenberg, and more — just download
# one and point this tool at it, no wikitext/XML processing needed.
# Requires: pip install libzim beautifulsoup4
python tools/wiki_pack.py --zim simplewiki.zim \
    --slug simplewiki --name "Simple English Wikipedia" --language en \
    --out sdcard_wiki

# A folder of one-article-per-file plain text (filename = title)
python tools/wiki_pack.py --dir path/to/txt_files \
    --slug my-notes --name "My Notes" --out sdcard_wiki

# A generic JSON-Lines file: {"title": "...", "text": "..."} per line
python tools/wiki_pack.py --jsonl articles.jsonl \
    --slug my-dataset --name "My Dataset" --out sdcard_wiki

# The output of WikiExtractor (https://github.com/attardi/wikiextractor),
# for when you have a raw Wikipedia XML dump instead of a .zim:
python -m wikiextractor.WikiExtractor --json -o extracted simplewiki-latest-pages-articles.xml.bz2
python tools/wiki_pack.py --wikiextractor extracted \
    --slug simplewiki --name "Simple English Wikipedia" --language en \
    --out sdcard_wiki
```

`--zim` reads Kiwix's rendered HTML per article and strips it down to plain
text with BeautifulSoup (dropping references, navboxes, infoboxes, and
edit-section links); redirects and non-HTML entries (images, CSS, JS) are
skipped. For a huge ZIM (full Wikipedia editions can have millions of
entries including media), use `--max-articles` first to sanity-check the
output before committing to a full conversion.

Then copy the resulting `sdcard_wiki/<slug>/` folder onto the SD card as
`/wiki/<slug>/`. Run `--help` for filtering options (`--min-length`,
`--max-articles`, namespace filtering for real Wikipedia dumps, etc).

A small demo dataset is included and pre-built at `sdcard_wiki/h2g2-sampler/`
(sources in `tools/sample_articles/`) — copy it to `/wiki/h2g2-sampler/` on
the card to try the reader immediately without preparing your own data.

### Format details / limits

- Article bodies are capped at 96 KB when loaded (`WIKI_ARTICLE_MAX_BYTES` in
  `main/wiki.h`); longer bodies are truncated.
- Titles are capped at 95 bytes of UTF-8.
- The index is sorted case-insensitively using Python's `str.lower()`, while
  the firmware compares with C's ASCII-only `strncasecmp`; this can only
  affect the sort position of non-ASCII titles, not lookups of ASCII ones.
- The SD card is only scanned at boot and when returning to the dataset
  picker; there's no card-detect pin wired on Tanmatsu, so hot-swapping isn't
  detected automatically.

## Source layout

```
main/
  main.c          boot sequence (NVS, BSP, framebuffer) + input loop
  storage.c/h     mounts the SD card over Tanmatsu's 4-bit SDIO bus
  wiki.c/h        dataset discovery + a common wiki_result_t API over both
                  backends (tools/wiki_pack.py folders and .zim files)
  wiki_zim.c/h    native ZIM file reader (header/dirent/cluster parsing)
  html_text.c/h   simple on-device HTML-to-plain-text conversion
  screens.c/h     the screens (no card / dataset picker / browser / reader /
                  home)
  ui_theme.c/h    Guide colour palette + shared drawing helpers (panels,
                  header/footer bars, CRT scanline overlay)
components/
  zstddeclib/     vendored single-file Zstd decompressor (see wiki_zim.c)
tools/
  wiki_pack.py       SD-card dataset packer (see above)
  sample_articles/   source text for the bundled demo dataset
sdcard_wiki/
  h2g2-sampler/      pre-built demo dataset, ready to copy to the SD card
```

## License

The contents of this repository may be considered in the public domain or
[CC0-1.0](https://creativecommons.org/publicdomain/zero/1.0) licensed at your
disposal, per the upstream template. *The Hitchhiker's Guide to the Galaxy*
is a trademark/property of its respective rights holders; the sample dataset
in `tools/sample_articles/` contains only original text written for this
project, not excerpts from the books or TV series.
