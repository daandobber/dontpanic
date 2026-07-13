#!/usr/bin/env python3
"""
Packs plain-text wiki articles into the flat binary format the Hitchhiker's
Guide app reads straight off the SD card:

    <out>/<slug>/meta.json     {"name", "description", "language"}
    <out>/<slug>/index.bin     sorted array of fixed-width records
    <out>/<slug>/articles.dat  concatenated raw UTF-8 article bodies

index.bin records are packed as "<96sII" (title, offset, length) and sorted
by lowercase title, so the firmware can binary-search the index directly
without loading it into RAM.

Input sources (pick one):

  --dir DIR             One article per *.txt file. The filename (without
                         extension, underscores turned into spaces) becomes
                         the title, the file content becomes the body.

  --jsonl FILE           A JSON-Lines file, one article per line:
                         {"title": "...", "text": "..."}

  --wikiextractor DIR    A directory produced by WikiExtractor
                         (https://github.com/attardi/wikiextractor), i.e.
                         nested AA/wiki_00 files containing one JSON object
                         per line with "title" and "text" fields. This is
                         the easiest way to turn a full Wikipedia XML dump
                         into something this tool can consume.

  --zim FILE             A Kiwix .zim file (https://library.kiwix.org/ has
                         ready-made offline dumps of Wikipedia in every
                         language/size, Wikivoyage, Wiktionary, Stack
                         Exchange, Project Gutenberg, ...). This is usually
                         the easiest starting point: download a .zim and
                         point this tool at it directly, no XML/wikitext
                         processing required. Requires:
                             pip install libzim beautifulsoup4

Example:

    python wiki_pack.py --zim simplewiki.zim \\
        --slug simplewiki --name "Simple English Wikipedia" \\
        --language en --out ./sdcard_wiki

Copy the resulting ./sdcard_wiki/simplewiki folder to /wiki/simplewiki on
the SD card.
"""
import argparse
import json
import os
import struct
import sys

TITLE_MAX = 96
RECORD_FORMAT = "<%dsII" % TITLE_MAX
RECORD_SIZE = struct.calcsize(RECORD_FORMAT)

DEFAULT_SKIPPED_PREFIXES = (
    "Category:", "Template:", "Wikipedia:", "File:", "Portal:",
    "Draft:", "Module:", "MediaWiki:", "Help:", "Talk:",
)


def encode_title(title: str) -> bytes:
    raw = title.encode("utf-8")[: TITLE_MAX - 1]
    # Don't cut a multi-byte UTF-8 sequence in half.
    while raw and (raw[-1] & 0xC0) == 0x80:
        raw = raw[:-1]
    return raw.ljust(TITLE_MAX, b"\x00")


def clean_body(text: str) -> str:
    lines = [line.rstrip() for line in text.replace("\r\n", "\n").split("\n")]
    out_lines = []
    blank_run = 0
    for line in lines:
        if line == "":
            blank_run += 1
            if blank_run > 1:
                continue
        else:
            blank_run = 0
        out_lines.append(line)
    return "\n".join(out_lines).strip("\n")


def should_skip(title: str, keep_non_articles: bool) -> bool:
    if keep_non_articles:
        return False
    return title.startswith(DEFAULT_SKIPPED_PREFIXES)


def iter_dir_articles(directory):
    for name in sorted(os.listdir(directory)):
        if not name.lower().endswith(".txt"):
            continue
        path = os.path.join(directory, name)
        if not os.path.isfile(path):
            continue
        title = os.path.splitext(name)[0].replace("_", " ")
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            text = f.read()
        yield title, text


def iter_jsonl_articles(path):
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            obj = json.loads(line)
            title = obj.get("title")
            text = obj.get("text")
            if title is None or text is None:
                continue
            yield title, text


def iter_wikiextractor_articles(root):
    for dirpath, _dirnames, filenames in os.walk(root):
        for name in sorted(filenames):
            path = os.path.join(dirpath, name)
            with open(path, "r", encoding="utf-8", errors="replace") as f:
                for line in f:
                    line = line.strip()
                    if not line:
                        continue
                    obj = json.loads(line)
                    title = obj.get("title")
                    text = obj.get("text")
                    if title is None or text is None:
                        continue
                    # WikiExtractor repeats the title as the first line of
                    # the body; drop it so it isn't shown twice.
                    if text.startswith(title):
                        text = text[len(title):].lstrip("\n")
                    yield title, text


def html_to_text(html: str) -> str:
    from bs4 import BeautifulSoup

    soup = BeautifulSoup(html, "html.parser")
    # Strip the boilerplate that clutters rendered Wikipedia/MediaWiki HTML
    # but isn't part of the actual article prose.
    for selector in (
        "script", "style", "sup.reference", "table.navbox", "table.infobox",
        "table.metadata", "span.mw-editsection", "div.navbox", "ol.references",
        "div.printfooter", "div.catlinks", "div.reflist",
    ):
        for tag in soup.select(selector):
            tag.decompose()
    return soup.get_text("\n")


def iter_zim_articles(path, max_articles=0, progress=True):
    from libzim.reader import Archive

    archive = Archive(path)
    count = 0
    total = archive.entry_count
    for i in range(total):
        if progress and i % 5000 == 0:
            print(f"  scanning entry {i:,}/{total:,} ({count:,} articles so far)...", file=sys.stderr)
        entry = archive._get_entry_by_id(i)
        if entry.is_redirect:
            continue
        item = entry.get_item()
        if not item.mimetype.startswith("text/html"):
            continue
        html = bytes(item.content).decode("utf-8", errors="replace")
        yield entry.title, html_to_text(html)
        count += 1
        if max_articles and count >= max_articles:
            return


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--dir", help="Directory of *.txt articles")
    source.add_argument("--jsonl", help="JSON-Lines file with {title, text} per line")
    source.add_argument("--wikiextractor", help="WikiExtractor output directory")
    source.add_argument("--zim", help="Kiwix .zim file (requires: pip install libzim beautifulsoup4)")

    parser.add_argument("--out", default="./sdcard_wiki", help="Output root directory (default: ./sdcard_wiki)")
    parser.add_argument("--slug", required=True, help="Dataset folder name, e.g. 'simplewiki'")
    parser.add_argument("--name", help="Display name shown in the app (default: slug)")
    parser.add_argument("--description", default="", help="One-line description shown in the app")
    parser.add_argument("--language", default="en", help="Language code shown in the app (default: en)")
    parser.add_argument("--min-length", type=int, default=1, help="Skip articles shorter than this many characters")
    parser.add_argument("--max-articles", type=int, default=0, help="Stop after this many articles (0 = no limit)")
    parser.add_argument(
        "--keep-non-articles", action="store_true",
        help="Don't filter out Category:/Template:/Talk:/etc. namespace pages",
    )
    args = parser.parse_args()

    if args.dir:
        source_iter = iter_dir_articles(args.dir)
    elif args.jsonl:
        source_iter = iter_jsonl_articles(args.jsonl)
    elif args.zim:
        source_iter = iter_zim_articles(args.zim, max_articles=args.max_articles)
    else:
        source_iter = iter_wikiextractor_articles(args.wikiextractor)

    articles = []
    seen_titles = set()
    for title, text in source_iter:
        title = title.strip()
        if not title or should_skip(title, args.keep_non_articles):
            continue
        if title.lower() in seen_titles:
            continue
        body = clean_body(text)
        if len(body) < args.min_length:
            continue
        seen_titles.add(title.lower())
        articles.append((title, body))
        if args.max_articles and len(articles) >= args.max_articles:
            break

    if not articles:
        print("No articles found -- check your input path and filters.", file=sys.stderr)
        sys.exit(1)

    articles.sort(key=lambda item: item[0].lower())

    out_dir = os.path.join(args.out, args.slug)
    os.makedirs(out_dir, exist_ok=True)

    index_path = os.path.join(out_dir, "index.bin")
    articles_path = os.path.join(out_dir, "articles.dat")
    meta_path = os.path.join(out_dir, "meta.json")

    with open(articles_path, "wb") as articles_file, open(index_path, "wb") as index_file:
        offset = 0
        for title, body in articles:
            body_bytes = body.encode("utf-8")
            articles_file.write(body_bytes)
            index_file.write(struct.pack(RECORD_FORMAT, encode_title(title), offset, len(body_bytes)))
            offset += len(body_bytes)

    meta = {
        "name": args.name or args.slug,
        "description": args.description,
        "language": args.language,
    }
    with open(meta_path, "w", encoding="utf-8") as f:
        json.dump(meta, f, ensure_ascii=False, indent=2)

    total_bytes = os.path.getsize(articles_path) + os.path.getsize(index_path)
    print(f"Wrote {len(articles)} articles to {out_dir}")
    print(f"  index.bin:    {os.path.getsize(index_path):,} bytes ({RECORD_SIZE} bytes/record)")
    print(f"  articles.dat: {os.path.getsize(articles_path):,} bytes")
    print(f"  total:        {total_bytes:,} bytes")
    print(f"Copy '{out_dir}' to /wiki/{args.slug} on the SD card.")


if __name__ == "__main__":
    main()
