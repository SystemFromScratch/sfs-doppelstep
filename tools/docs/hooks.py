"""MkDocs hooks for the Doppelstep documentation.

Turns an image that stands alone on its line, the usual Markdown form

    ![Caption text](images/picture.png)

into a captioned figure (rendered with the md_in_html extension):

    <figure class="dps-figure"><img ...><figcaption>Caption text</figcaption></figure>

The Markdown files stay plain, so they also render on GitHub, and the printed
manual gets numbered captions ("Figure 3: ...") from mkdocs-print-site-plugin.
The change is made on the Markdown so that the print page sees it as well.
"""

from __future__ import annotations

import html
import re

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
