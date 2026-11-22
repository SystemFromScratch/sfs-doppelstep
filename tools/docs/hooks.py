"""MkDocs hooks for the Doppelstep documentation.

Turns an image that stands alone on its line, the usual Markdown form

    ![Caption text](images/picture.png)

into a captioned figure (rendered with the md_in_html extension):

    <figure class="dps-figure"><img ...><figcaption>Caption text</figcaption></figure>

The Markdown files stay plain, so they also render on GitHub, and the printed
manual (tools/docs/build_manual.py, which runs the same hook) gets numbered
captions ("Figure 3: ...").
The change is made on the Markdown so that the print page sees it as well.

Adds the board reference pages, boards/<board>.md, generated from the Zephyr board
documentation boards/<vendor>/<board>/doc/index.rst (tools/docs/boards.py).
"""

from __future__ import annotations

import html
import os
import re
import sys

sys.path.insert(0, os.path.dirname(__file__))

import boards  # noqa: E402  (tools/docs/boards.py, next to this file)

_IMAGE_LINE = re.compile(r"^!\[(?P<alt>[^\]]+)\]\((?P<src>[^)\s]+)\)[ \t]*$", re.MULTILINE)
_FENCE = re.compile(r"^(```|~~~).*?^\1[ \t]*$", re.MULTILINE | re.DOTALL)


def on_page_markdown(markdown: str, **kwargs) -> str:
    def to_figure(match: re.Match) -> str:
        alt, src = match.group("alt"), match.group("src")
        caption = html.escape(alt, quote=False)
        return (
            '<figure class="dps-figure" markdown="1">\n\n'
            f"![{alt}]({src})\n\n"
            f"<figcaption>{caption}</figcaption>\n"
            "</figure>"
        )

    # Leave fenced code blocks untouched.
    parts, last = [], 0
    for fence in _FENCE.finditer(markdown):
        parts.append(_IMAGE_LINE.sub(to_figure, markdown[last:fence.start()]))
        parts.append(fence.group(0))
        last = fence.end()
    parts.append(_IMAGE_LINE.sub(to_figure, markdown[last:]))
    return "".join(parts)


def on_files(files, config, **kwargs):
    from mkdocs.structure.files import File

    for path, text in boards.generated_pages(config["repo_url"] or "").items():
        files.append(File.generated(config, path, content=text))
    return files


def on_serve(server, config, builder, **kwargs):
    for path in boards.board_sources():
        server.watch(path)
    return server
