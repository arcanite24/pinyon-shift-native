#!/usr/bin/env python3
"""Build the Shift Engine documentation site in site/pages into a static folder.

Uses only the Python 3.11+ standard library (zero external packages). Pages are
Markdown with a small front-matter block. Links between pages are written as
relative .md links so the sources also read cleanly on GitHub; the build rewrites
them to site URLs and verifies every internal link and #anchor in the output.

    python site/build.py [--out site/_build] [--offline]
"""

from __future__ import annotations

import argparse
import html
import json
import os
import re
import shutil
import sys
import tomllib
import urllib.parse
import urllib.request
from dataclasses import dataclass
from html.parser import HTMLParser
from pathlib import Path

SITE = Path(__file__).resolve().parent
ROOT = SITE.parent
REPO = "arcanite24/pinyon-shift-native"
REPO_URL = f"https://github.com/{REPO}"
BRANCH = "main"
SECTIONS = ["Start", "Playing", "Technical", "Project"]

MD_LINK = re.compile(r'href="(?!https?:|mailto:|#)([^"#]+?)\.md(#[^"]*)?"')
CELL_STATES = {"yes": "Yes", "partly": "Partly", "not yet": "Not yet", "n/a": "—"}


@dataclass
class Page:
    source: Path
    slug: str  # "" for the home page
    title: str
    section: str
    order: int
    description: str
    body: str

    @property
    def depth(self) -> int:
        return 0 if not self.slug else 1

    @property
    def output(self) -> Path:
        return Path(self.slug, "index.html") if self.slug else Path("index.html")


def slugify(text: str) -> str:
    plain = re.sub(r"<[^>]+>", "", text)
    plain = re.sub(r"`([^`]*)`", r"\1", plain)
    plain = re.sub(r"\[([^\]]+)\]\([^)]+\)", r"\1", plain)
    plain = plain.lower().strip()
    plain = re.sub(r"[^\w\s-]", "", plain)
    plain = re.sub(r"[\s_]+", "-", plain)
    plain = re.sub(r"-{2,}", "-", plain).strip("-")
    return plain or "section"


def read_page(path: Path) -> Page:
    text = path.read_text(encoding="utf-8").replace("\r\n", "\n")
    meta: dict[str, str] = {}
    if text.startswith("---\n"):
        header, text = text[4:].split("\n---\n", 1)
        for line in header.splitlines():
            key, _, value = line.partition(":")
            if key.strip():
                meta[key.strip()] = value.strip()
    slug = "" if path.stem == "index" else path.stem
    section = meta.get("section", "Start")
    if section not in SECTIONS:
        raise SystemExit(f"{path.name}: unknown section {section!r}")
    if "title" not in meta:
        raise SystemExit(f"{path.name}: missing title in front matter")
    return Page(
        path,
        slug,
        meta["title"],
        section,
        int(meta.get("order", "99")),
        meta.get("description", ""),
        text,
    )


def latest_release(offline: bool = False) -> dict | None:
    """Return latest release tag, date and primary archive, or None offline."""
    if offline or os.environ.get("SITE_OFFLINE") == "1":
        return None
    request = urllib.request.Request(
        f"https://api.github.com/repos/{REPO}/releases/latest",
        headers={"Accept": "application/vnd.github+json", "User-Agent": "shift-engine-site"},
    )
    token = os.environ.get("GITHUB_TOKEN")
    if token:
        request.add_header("Authorization", f"Bearer {token}")
    try:
        with urllib.request.urlopen(request, timeout=8) as response:
            data = json.load(response)
    except (OSError, ValueError) as error:
        print(f"note: latest release unavailable ({error}); using GitHub Releases link", file=sys.stderr)
        return None
    assets = data.get("assets", [])
    asset = next((a for a in assets if a["name"].endswith(".zip")), None)
    return {
        "tag": data["tag_name"],
        "date": data["published_at"][:10],
        "url": data["html_url"],
        "asset": asset
        and {
            "name": asset["name"],
            "url": asset["browser_download_url"],
            "size": f"{asset['size'] / 1e6:.1f} MB",
        },
    }


def release_block(release: dict | None) -> str:
    latest = f"{REPO_URL}/releases/latest"
    if not release or not release["asset"]:
        return (
            '<div class="release"><div class="release-main">'
            '<p class="release-label">Releases</p>'
            '<p class="release-sub">Windows, Linux / Steam Deck, macOS and Android</p>'
            '</div><div class="release-actions">'
            f'<a class="button" href="{latest}">Download from GitHub Releases</a>'
            f'<a class="quiet-link" href="building/">Build from source</a>'
            "</div></div>"
        )
    asset = release["asset"]
    return (
        '<div class="release">\n'
        '  <div class="release-main">\n'
        '    <p class="release-label">Latest release</p>\n'
        f'    <p class="release-version">{html.escape(release["tag"])} '
        f'<span>{html.escape(release["date"])}</span></p>\n'
        "  </div>\n"
        '  <div class="release-actions">\n'
        f'    <a class="button" href="{asset["url"]}">{html.escape(asset["name"])} '
        f'<span>{html.escape(asset["size"])}</span></a>\n'
        f'    <a class="quiet-link" href="{release["url"]}">Release notes</a>\n'
        f'    <a class="quiet-link" href="{REPO_URL}/releases">All platforms</a>\n'
        "  </div>\n"
        "</div>"
    )


def channel_note(release: dict | None) -> str:
    newer = f"newer than {html.escape(release['tag'])}" if release else "ahead of the latest release"
    return (
        f'<p class="channel-note">These docs follow the <code>{BRANCH}</code> branch and can '
        f'describe changes {newer}. Release archives and checksums are published on '
        f'<a href="{REPO_URL}/releases">GitHub Releases</a>.</p>'
    )


def feature_cell(cell: str) -> str:
    state, _, note = cell.partition(":")
    state = state.strip()
    if state not in CELL_STATES:
        raise SystemExit(f"features.toml: unknown state {state!r} in {cell!r}")
    note = note.strip().replace("|", r"\|")
    label = CELL_STATES[state]
    css = state.replace(" ", "-").replace("/", "")
    return f'<span class="state state-{css}">{label}</span>' + (f" {note}" if note else "")


def features_markdown() -> str:
    """Render the two feature comparison tables from site/features.toml."""
    data = tomllib.loads((SITE / "features.toml").read_text(encoding="utf-8"))
    platforms = data["platforms"]
    lines = [
        "## Compared with the original",
        "",
        "| Feature | Xbox 360 | Shift Engine |",
        "| --- | --- | --- |",
    ]
    for row in data["original"]:
        lines.append(f"| [{row['feature']}]({row['link']}) | {row['xbox']} | {row['shift']} |")
    lines += [
        "",
        "## By platform",
        "",
        "| Feature | " + " | ".join(platforms) + " |",
        "| --- |" + " --- |" * len(platforms),
    ]
    for row in data["platform"]:
        if len(row["cells"]) != len(platforms):
            raise SystemExit(f"features.toml: {row['feature']} needs {len(platforms)} cells")
        lines.append(
            f"| [{row['feature']}]({row['link']}) | "
            + " | ".join(feature_cell(c) for c in row["cells"])
            + " |"
        )
    return "\n".join(lines) + "\n"


# ---------------------------------------------------------------------------
# Minimal, strict stdlib Markdown -> HTML renderer
# ---------------------------------------------------------------------------

INLINE_TAG_ALLOW = re.compile(r"</?(?:kbd|span|br|code|strong|em|sup|sub)(?:\s+class=\"[\w\s-]+\")?\s*/?>")


def render_inline(text: str) -> str:
    """Render inline Markdown while preserving safe inline HTML and escaping the rest."""
    placeholders: list[str] = []

    def stash(fragment: str) -> str:
        idx = len(placeholders)
        placeholders.append(fragment)
        return f"\x00PH{idx}\x00"

    # 1. Backtick code spans first so inner syntax is untouched.
    def repl_code(match: re.Match) -> str:
        return stash(f"<code>{html.escape(match.group(1))}</code>")

    text = re.sub(r"`([^`]+)`", repl_code, text)

    # 2. Safe inline HTML tags (<kbd>, <span class="...">, <br>).
    text = INLINE_TAG_ALLOW.sub(lambda m: stash(m.group(0)), text)

    # 3. Backslash escapes (\|, \*, \[, etc.).
    text = re.sub(r"\\([\\`*_{}\[\]()#+\-.!|])", lambda m: stash(html.escape(m.group(1))), text)

    # 4. Images ![alt](src) and links [label](href).
    def repl_img(match: re.Match) -> str:
        alt = html.escape(match.group(1), quote=True)
        src = html.escape(match.group(2).strip(), quote=True)
        return stash(f'<img src="{src}" alt="{alt}">')

    text = re.sub(r"!\[([^\]]*)\]\(([^)]+)\)", repl_img, text)

    def repl_link(match: re.Match) -> str:
        label_raw = match.group(1)
        href = html.escape(match.group(2).strip(), quote=True)
        label = render_inline_emphasis(html.escape(label_raw))
        return stash(f'<a href="{href}">{label}</a>')

    text = re.sub(r"\[([^\]]+)\]\(([^)]+)\)", repl_link, text)

    # 5. Escape remaining raw characters and apply bold/italic.
    text = html.escape(text)
    text = render_inline_emphasis(text)

    # 6. Restore stashed placeholders (repeatedly in case links wrapped code spans).
    for _ in range(4):
        if "\x00PH" not in text:
            break
        text = re.sub(r"\x00PH(\d+)\x00", lambda m: placeholders[int(m.group(1))], text)
    return text


def render_inline_emphasis(text: str) -> str:
    text = re.sub(r"\*\*([^*]+)\*\*", r"<strong>\1</strong>", text)
    text = re.sub(r"(?<!\*)\*([^*\n]+)\*(?!\*)", r"<em>\1</em>", text)
    return text


def split_table_row(line: str) -> list[str]:
    s = line.strip()
    if s.startswith("|"):
        s = s[1:]
    if s.endswith("|") and not s.endswith(r"\|"):
        s = s[:-1]
    cells: list[str] = []
    buf: list[str] = []
    i = 0
    while i < len(s):
        if s[i] == "\\" and i + 1 < len(s) and s[i + 1] == "|":
            buf.append("|")
            i += 2
        elif s[i] == "|":
            cells.append("".join(buf).strip())
            buf = []
            i += 1
        else:
            buf.append(s[i])
            i += 1
    cells.append("".join(buf).strip())
    return cells


def parse_table_align(sep_line: str) -> list[str] | None:
    cells = split_table_row(sep_line)
    if not cells:
        return None
    aligns: list[str] = []
    for cell in cells:
        c = cell.strip()
        if not re.fullmatch(r":?-{3,}:?", c):
            return None
        if c.startswith(":") and c.endswith(":"):
            aligns.append("center")
        elif c.endswith(":"):
            aligns.append("right")
        elif c.startswith(":"):
            aligns.append("left")
        else:
            aligns.append("")
    return aligns


def is_block_html(line: str) -> bool:
    s = line.lstrip()
    return bool(re.match(r"^<(?:div|details|summary|figure|figcaption|table|thead|tbody|tr|th|td|p|hr|/div|/details|/summary|/figure)\b", s))


def render_markdown(text: str) -> tuple[str, list[dict]]:
    lines = text.splitlines()
    out: list[str] = []
    toc: list[dict] = []
    used_ids: dict[str, int] = {}

    def unique_id(base: str) -> str:
        n = used_ids.get(base, 0) + 1
        used_ids[base] = n
        return base if n == 1 else f"{base}-{n}"

    i = 0
    n = len(lines)

    while i < n:
        line = lines[i]
        stripped = line.strip()

        # Blank line
        if not stripped:
            i += 1
            continue

        # Fenced code block
        fence = re.match(r"^```([\w+-]*)\s*$", stripped)
        if fence:
            lang = fence.group(1)
            code_lines: list[str] = []
            i += 1
            while i < n and not lines[i].strip().startswith("```"):
                code_lines.append(lines[i])
                i += 1
            if i < n:
                i += 1
            cls = f' class="language-{html.escape(lang)}"' if lang else ""
            code_body = html.escape("\n".join(code_lines))
            out.append(f'<div class="code"><pre><code{cls}>{code_body}\n</code></pre></div>')
            continue

        # Raw block HTML (e.g. {{release}} output or <details>)
        if is_block_html(line):
            html_lines = [line]
            i += 1
            while i < n and lines[i].strip() != "":
                html_lines.append(lines[i])
                i += 1
            out.append("\n".join(html_lines))
            continue

        # Horizontal rule
        if re.fullmatch(r"(?:-\s*){3,}|(?:\*\s*){3,}", stripped):
            out.append("<hr>")
            i += 1
            continue

        # ATX heading
        heading = re.match(r"^(#{1,4})\s+(.*?)\s*#*$", stripped)
        if heading:
            level = len(heading.group(1))
            raw_title = heading.group(2)
            ident = unique_id(slugify(raw_title))
            rendered_title = render_inline(raw_title)
            if level >= 2:
                anchor = f'<a class="anchor" href="#{ident}" title="Link to this section">#</a>'
                out.append(f'<h{level} id="{ident}">{rendered_title}{anchor}</h{level}>')
            else:
                out.append(f'<h{level} id="{ident}">{rendered_title}</h{level}>')
            if level in (2, 3):
                toc.append({"level": level, "id": ident, "name": re.sub(r"<[^>]+>", "", rendered_title)})
            i += 1
            continue

        # Table (header + separator)
        if "|" in line and i + 1 < n and parse_table_align(lines[i + 1]) is not None:
            headers = split_table_row(line)
            aligns = parse_table_align(lines[i + 1]) or []
            i += 2
            body_rows: list[list[str]] = []
            while i < n and "|" in lines[i] and lines[i].strip():
                body_rows.append(split_table_row(lines[i]))
                i += 1
            th_cells = []
            for col, htext in enumerate(headers):
                al = aligns[col] if col < len(aligns) else ""
                attr = f' align="{al}"' if al else ""
                th_cells.append(f"<th{attr}>{render_inline(htext)}</th>")
            tr_html = []
            for row in body_rows:
                td_cells = []
                for col in range(len(headers)):
                    cell = row[col] if col < len(row) else ""
                    al = aligns[col] if col < len(aligns) else ""
                    attr = f' align="{al}"' if al else ""
                    td_cells.append(f"<td{attr}>{render_inline(cell)}</td>")
                tr_html.append("<tr>" + "".join(td_cells) + "</tr>")
            out.append(
                '<div class="table-wrap"><table><thead><tr>'
                + "".join(th_cells)
                + "</tr></thead><tbody>"
                + "".join(tr_html)
                + "</tbody></table></div>"
            )
            continue

        # Blockquote
        if stripped.startswith(">"):
            q_lines: list[str] = []
            while i < n and lines[i].lstrip().startswith(">"):
                q_lines.append(re.sub(r"^\s*>\s?", "", lines[i]))
                i += 1
            inner_html, _ = render_markdown("\n".join(q_lines))
            out.append(f"<blockquote>{inner_html}</blockquote>")
            continue

        # Unordered or ordered list
        list_m = re.match(r"^(\s*)([-*]|\d+\.)\s+(.*)$", line)
        if list_m:
            tag = "ol" if list_m.group(2).endswith(".") else "ul"
            base_indent = len(list_m.group(1))
            items: list[list[str]] = []
            while i < n:
                cur = lines[i]
                if not cur.strip():
                    # Look ahead: does the list continue?
                    if i + 1 < n and re.match(
                        rf"^\s{{{base_indent}}}(?:{'\\d+\\.' if tag == 'ol' else '[-*]'}|\s{{2,}})\s*",
                        lines[i + 1],
                    ):
                        i += 1
                        continue
                    break
                m = re.match(r"^(\s*)([-*]|\d+\.)\s+(.*)$", cur)
                if m and len(m.group(1)) == base_indent and (m.group(2).endswith(".") == (tag == "ol")):
                    items.append([m.group(3)])
                    i += 1
                elif cur.startswith(" " * (base_indent + 2)) and items:
                    items[-1].append(cur[base_indent + 2 :])
                    i += 1
                else:
                    break
            li_html = []
            for item_lines in items:
                # Support nested sub-lists or plain continuation lines
                if any(re.match(r"^([-*]|\d+\.)\s+", l) for l in item_lines[1:]):
                    first = render_inline(item_lines[0])
                    rest, _ = render_markdown("\n".join(item_lines[1:]))
                    li_html.append(f"<li>{first}{rest}</li>")
                else:
                    li_html.append(f"<li>{render_inline(' '.join(l.strip() for l in item_lines))}</li>")
            out.append(f"<{tag}>{''.join(li_html)}</{tag}>")
            continue

        # Paragraph
        para_lines = [stripped]
        i += 1
        while i < n:
            nxt = lines[i]
            ns = nxt.strip()
            if (
                not ns
                or ns.startswith("```")
                or ns.startswith("#")
                or ns.startswith(">")
                or re.fullmatch(r"(?:-\s*){3,}|(?:\*\s*){3,}", ns)
                or re.match(r"^\s*([-*]|\d+\.)\s+", nxt)
                or is_block_html(nxt)
                or ("|" in nxt and i + 1 < n and parse_table_align(lines[i + 1]) is not None)
            ):
                break
            para_lines.append(ns)
            i += 1
        out.append(f"<p>{render_inline(' '.join(para_lines))}</p>")

    return "\n".join(out), toc


def rewrite_links(body: str, page: Page, slugs: set[str]) -> str:
    prefix = "../" * page.depth

    def replace(match: re.Match) -> str:
        target, anchor = match.group(1), match.group(2) or ""
        name = Path(target).name
        stem = "" if name == "index" else name
        if "/" in target or (stem and stem not in slugs):
            raise SystemExit(
                f"{page.source.name}: link to {target}.md is not a site page; "
                f"use a full {REPO_URL} URL"
            )
        if stem or anchor:
            return f'href="{prefix}{stem + "/" if stem else ""}{anchor}"'
        return f'href="{prefix or "./"}"'

    return MD_LINK.sub(replace, body)


def page_href(prefix: str, page: Page) -> str:
    return prefix + (page.slug + "/" if page.slug else "") or "./"


def nav_html(pages: list[Page], current: Page | None, prefix: str) -> str:
    parts = []
    for section in SECTIONS:
        entries = [p for p in pages if p.section == section]
        if not entries:
            continue
        parts.append(f'<p class="nav-section">{html.escape(section)}</p><ul>')
        for page in entries:
            attrs = ' aria-current="page"' if page is current else ""
            parts.append(
                f'<li><a href="{page_href(prefix, page)}"{attrs}>{html.escape(page.title)}</a></li>'
            )
        if section == "Project":
            parts.append(
                f'<li><a href="{REPO_URL}/blob/{BRANCH}/CONTRIBUTING.md">Contributing '
                '<span class="ext" aria-hidden="true">↗</span></a></li>'
            )
            parts.append(
                f'<li><a href="{REPO_URL}/blob/{BRANCH}/SECURITY.md">Security '
                '<span class="ext" aria-hidden="true">↗</span></a></li>'
            )
            parts.append(
                f'<li><a href="{REPO_URL}/blob/{BRANCH}/docs/BENCHMARK.md">Benchmark spec '
                '<span class="ext" aria-hidden="true">↗</span></a></li>'
            )
        parts.append("</ul>")
    return "\n".join(parts)


def toc_html(items: list[dict]) -> str:
    if len(items) < 2:
        return ""
    links = "".join(
        f'<li class="toc-l{item["level"]}"><a href="#{item["id"]}">{html.escape(item["name"])}</a></li>'
        for item in items
    )
    return f'<nav class="toc" aria-label="On this page"><p>On this page</p><ul>{links}</ul></nav>'


def pager_html(pages: list[Page], current: Page) -> str:
    index = pages.index(current)
    prefix = "../" * current.depth
    cells = []
    for offset, label, cls in ((-1, "Previous", "prev"), (1, "Next", "next")):
        position = index + offset
        if 0 <= position < len(pages):
            page = pages[position]
            cells.append(
                f'<a class="pager-{cls}" href="{page_href(prefix, page)}"><span>{label}</span>'
                f"{html.escape(page.title)}</a>"
            )
        else:
            cells.append("<span></span>")
    return f'<nav class="pager" aria-label="Pages">{"".join(cells)}</nav>'


# ---------------------------------------------------------------------------
# Post-build internal link & anchor verification
# ---------------------------------------------------------------------------


class _LinkScanner(HTMLParser):
    def __init__(self) -> None:
        super().__init__()
        self.ids: set[str] = set()
        self.refs: list[tuple[str, str]] = []

    def handle_starttag(self, tag: str, attrs: list[tuple[str, str | None]]) -> None:
        attr_map = dict(attrs)
        ident = attr_map.get("id")
        if ident:
            self.ids.add(ident)
        if tag in ("a", "link") and attr_map.get("href"):
            self.refs.append((tag, attr_map["href"] or ""))
        elif tag in ("script", "img") and attr_map.get("src"):
            self.refs.append((tag, attr_map["src"] or ""))


def verify_site(out: Path, base_prefix: str) -> tuple[int, int]:
    """Verify every internal file reference and #anchor across generated HTML."""
    html_files = sorted(out.rglob("*.html"))
    ids_by_file: dict[Path, set[str]] = {}
    refs_by_file: dict[Path, list[tuple[str, str]]] = {}

    for path in html_files:
        scanner = _LinkScanner()
        scanner.feed(path.read_text(encoding="utf-8"))
        ids_by_file[path.resolve()] = scanner.ids
        refs_by_file[path.resolve()] = scanner.refs

    errors: list[str] = []
    checked_links = 0
    checked_anchors = 0

    for src, refs in refs_by_file.items():
        rel_src = src.relative_to(out).as_posix()
        for tag, raw in refs:
            if not raw or raw.startswith(("http://", "https://", "mailto:", "data:")):
                continue
            parsed = urllib.parse.urlsplit(raw)
            path_part = parsed.path
            fragment = parsed.fragment

            if not path_part:
                target = src
            elif path_part.startswith(base_prefix):
                rel = path_part[len(base_prefix) :]
                target = out / rel
            elif path_part.startswith("/"):
                errors.append(f"{rel_src}: unexpected root-absolute link {raw!r}")
                continue
            else:
                target = (src.parent / path_part).resolve()

            if target.is_dir():
                target = target / "index.html"
            elif raw.endswith("/") or (not target.exists() and (target / "index.html").is_file()):
                target = target / "index.html"

            checked_links += 1
            if not target.is_file():
                errors.append(f"{rel_src}: broken {tag} reference {raw!r} -> {target}")
                continue
            if fragment:
                checked_anchors += 1
                target_ids = ids_by_file.get(target.resolve(), set())
                if fragment not in target_ids:
                    errors.append(f"{rel_src}: broken anchor {raw!r} (no id={fragment!r} in {target.relative_to(out).as_posix()})")

    if errors:
        for err in errors:
            print(f"link-check error: {err}", file=sys.stderr)
        raise SystemExit(f"site verification failed with {len(errors)} broken link(s)/anchor(s)")
    return checked_links, checked_anchors


def build(out: Path, offline: bool = False) -> None:
    pages = sorted(
        (read_page(p) for p in (SITE / "pages").glob("*.md")),
        key=lambda p: (SECTIONS.index(p.section), p.order, p.title),
    )
    if not pages:
        raise SystemExit("site/pages contains no Markdown files")
    slugs = {p.slug for p in pages if p.slug}
    template = (SITE / "template.html").read_text(encoding="utf-8")
    release = latest_release(offline=offline)

    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)

    for static_file in (SITE / "static").iterdir():
        if static_file.is_file():
            shutil.copyfile(static_file, out / static_file.name)

    for page in pages:
        text = page.body.replace("{{release}}", release_block(release))
        if "{{features}}" in text:
            text = text.replace("{{features}}", features_markdown())
        body, toc = render_markdown(text)
        body = rewrite_links(body, page, slugs)
        prefix = "../" * page.depth
        version = release["tag"] if release else ""
        source = page.source.relative_to(ROOT).as_posix()
        values = {
            "title": "Shift Engine" if not page.slug else f"{page.title} · Shift Engine",
            "description": html.escape(page.description, quote=True),
            "root": prefix or "./",
            "nav": nav_html(pages, page, prefix),
            "toc": toc_html(toc),
            "content": body,
            "pager": pager_html(pages, page),
            "repo": REPO_URL,
            "version": html.escape(version),
            "edit": f'<a href="{REPO_URL}/edit/{BRANCH}/{source}">Edit this page on GitHub</a>',
            "page_class": "home" if not page.slug else "doc",
            "channel": channel_note(release),
        }
        rendered = re.sub(r"\{\{(\w+)\}\}", lambda m: values[m.group(1)], template)
        target = out / page.output
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(rendered, encoding="utf-8")

    (out / ".nojekyll").write_text("", encoding="utf-8")
    base = os.environ.get("SITE_BASE", "/pinyon-shift-native/")
    missing = {
        "title": "Not found · Shift Engine",
        "description": "",
        "root": base,
        "nav": nav_html(pages, None, base),
        "toc": "",
        "pager": "",
        "repo": REPO_URL,
        "content": (
            '<h1>Page not found</h1><p>Nothing lives at this address. '
            f'Start from the <a href="{base}">overview</a>.</p>'
        ),
        "version": html.escape(release["tag"] if release else ""),
        "edit": "",
        "page_class": "doc",
        "channel": "",
    }
    (out / "404.html").write_text(
        re.sub(r"\{\{(\w+)\}\}", lambda m: missing[m.group(1)], template),
        encoding="utf-8",
    )

    links, anchors = verify_site(out, base)
    print(f"built {len(pages)} pages into {out}; verified {links} links and {anchors} anchors (0 broken)")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--out", type=Path, default=SITE / "_build")
    parser.add_argument("--offline", action="store_true", help="skip GitHub Releases API lookup")
    args = parser.parse_args()
    build(args.out.resolve(), offline=args.offline)


if __name__ == "__main__":
    main()
