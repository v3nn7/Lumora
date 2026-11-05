#!/usr/bin/env python3
"""Build the website: copy site/ and render docs/GUIDE.md into docs.html.

  python3 scripts/site/build.py --out _site

docs/GUIDE.md is the single source of the user guide; it reads well on GitHub and becomes
the website's documentation page here. Needs the `markdown` package (pip install markdown).
"""

from __future__ import annotations

import argparse
import html
import re
import shutil
from pathlib import Path

import markdown

ROOT = Path(__file__).resolve().parents[2]
REPO = "https://github.com/v3nn7/Lumora/blob/main/"

PAGE = """<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Lumora Documentation</title>
  <meta name="description" content="How to install Lumora, build scenes in the editor, use physics and play mode, and edit together over LAN or playit.gg.">
  <link rel="icon" href="assets/logo.svg" type="image/svg+xml">
  <link rel="stylesheet" href="assets/style.css">
</head>
<body>
<header class="site-header">
  <div class="wrap">
    <a class="brand" href="./"><img src="assets/logo.svg" alt="">LUMORA</a>
    <nav class="nav">
      <a href="./#features">Features</a>
      <a href="./#download">Download</a>
      <a href="docs.html" aria-current="page">Documentation</a>
      <a href="https://github.com/v3nn7/Lumora">GitHub</a>
    </nav>
  </div>
</header>
<div class="wrap docs">
  <nav class="toc" aria-label="Contents">
{toc}
  </nav>
  <article class="doc">
{body}
    <p class="note">This page is generated from <a href="{repo}docs/GUIDE.md">docs/GUIDE.md</a>.
      Found a mistake? Edit it on GitHub.</p>
  </article>
</div>
<footer class="site-footer">
  <div class="wrap">
    <span>Lumora Engine · free software under the <a href="{repo}LICENSE">GNU GPL v3 or later</a></span>
    <span><a href="./">Home</a> · <a href="https://github.com/v3nn7/Lumora">Source</a> ·
      <a href="{repo}CONTRIBUTING.md">Contributing</a></span>
  </div>
</footer>
</body>
</html>
"""


def render_toc(tokens: list[dict]) -> str:
    """Sidebar from the guide's headings: each h2 is a group, its h3s are the links."""
    lines: list[str] = []
    for top in tokens:  # the single h1
        for section in top.get("children", []):
            lines.append(f'    <p><a href="#{section["id"]}">{section["name"]}</a></p>')
            for sub in section.get("children", []):
                lines.append(f'    <a href="#{sub["id"]}">{sub["name"]}</a>')
    return "\n".join(lines)


def rewrite_links(body: str) -> str:
    """Relative links in the guide point at repository files; on the website they go to GitHub."""

    def fix(match: re.Match[str]) -> str:
        target = html.unescape(match.group(1))
        if re.match(r"^(https?:|mailto:|#)", target):
            return match.group(0)
        return f'href="{REPO}docs/{target}"'

    return re.sub(r'href="([^"]+)"', fix, body)


def build(out: Path) -> None:
    if out.exists():
        shutil.rmtree(out)
    shutil.copytree(ROOT / "site", out)

    md = markdown.Markdown(
        extensions=["tables", "fenced_code", "attr_list", "toc"],
        extension_configs={"toc": {"toc_depth": "1-3"}},
    )
    body = md.convert((ROOT / "docs" / "GUIDE.md").read_text(encoding="utf-8"))
    body = rewrite_links(body)
    # The guide's first paragraph is its introduction.
    body = body.replace("<p>", '<p class="lead">', 1)
    page = PAGE.format(toc=render_toc(md.toc_tokens), body=body, repo=REPO)
    (out / "docs.html").write_text(page, encoding="utf-8")
    print(f"site written to {out}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--out", type=Path, default=ROOT / "_site")
    build(parser.parse_args().out.resolve())


if __name__ == "__main__":
    main()
