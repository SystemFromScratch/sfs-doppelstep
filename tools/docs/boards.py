"""Board reference pages of the documentation, from the Zephyr board documentation.

Every board of this repository, boards/<vendor>/<board>/board.yml, describes itself in
boards/<vendor>/<board>/doc/index.rst, as the boards of Zephyr do. That file stays the
only source: this module turns it into the Markdown page boards/<board>.md of the
website (tools/docs/hooks.py, on_files) and of the PDF manual (build_manual.py). A
new board directory with a doc/index.rst gets its page; list it in the nav of
mkdocs.yml (the strict build names a generated page that is missing there).

The reStructuredText of a board page may use: sections, paragraphs, inline literals,
emphasis, strong, links, bullet and enumerated lists, simple and grid tables without
spans, literal blocks and the admonitions note, tip, important, warning, caution,
danger. Anything else stops the build with the file and the construct, so a page never
loses content silently. The `.. zephyr:board::` directive of the Zephyr docs is
accepted and replaced by the summary table at the top of the page.

Usage outside MkDocs (check one page):

    python tools/docs/boards.py            # prints every generated page
"""

from __future__ import annotations

import glob
import os
import re
import sys
from typing import Dict, List

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
PAGE_DIR = "boards"  # pages land in docs/boards/<board>.md

_ADMONITIONS = ("note", "tip", "important", "warning", "caution", "danger", "hint",
                "attention")


class BoardDocError(Exception):
    """A construct of a board page that the converter does not support."""


def _board_yml(path: str) -> dict:
    import yaml

    with open(path, encoding="utf-8") as f:
        return (yaml.safe_load(f) or {}).get("board", {})


def _register_zephyr_directives() -> None:
    from docutils.parsers.rst import Directive, directives

    class ZephyrBoard(Directive):
        """`.. zephyr:board:: <name>`: the summary table of the page stands for it."""

        required_arguments = 1
        has_content = False

        def run(self):
            return []

    directives.register_directive("zephyr:board", ZephyrBoard)


def _doctree(source: str, path: str):
    from docutils.core import publish_doctree
    from docutils.utils import SystemMessage

    _register_zephyr_directives()
    try:
        return publish_doctree(
            source,
            source_path=path,
            settings_overrides={
                "doctitle_xform": False,  # "Overview" stays a section, the title is the board
                "report_level": 5,        # no messages on stderr ...
                "halt_level": 2,          # ... a warning or an error stops the build
                "file_insertion_enabled": False,
                "raw_enabled": False,
            },
        )
    except SystemMessage as err:
        raise BoardDocError(f"{path}: {err}") from err


class _Writer:
    """Doctree of a board page to Markdown (Python-Markdown with the mkdocs.yml extensions)."""

    def __init__(self, path: str) -> None:
        self.path = path

    def fail(self, node, what: str = "") -> None:
        line = getattr(node, "line", None) or ""
        raise BoardDocError(
            f"{self.path}:{line}: {what or 'unsupported reStructuredText'} "
            f"({node.tagname}); see tools/docs/boards.py")

    # ------------------------------------------------------------------ inline
    @staticmethod
    def escape(text: str) -> str:
        text = text.replace("\\", "\\\\").replace("&", "&amp;").replace("<", "&lt;")
        return re.sub(r"([*_`\[\]|])", r"\\\1", text)

    def inline(self, node) -> str:
        from docutils import nodes

        out: List[str] = []
        for child in node.children:
            if isinstance(child, nodes.Text):
                out.append(self.escape(child.astext()))
            elif isinstance(child, nodes.literal):
                text = child.astext()
                fence = "``" if "`" in text else "`"
                pad = " " if fence == "``" else ""
                out.append(f"{fence}{pad}{text}{pad}{fence}")
            elif isinstance(child, nodes.emphasis):
                out.append(f"*{self.inline(child)}*")
            elif isinstance(child, nodes.strong):
                out.append(f"**{self.inline(child)}**")
            elif isinstance(child, nodes.reference):
                uri = child.get("refuri")
                if not uri:
                    self.fail(child, "only links with a URL")
                text = self.inline(child)
                out.append(f"<{uri}>" if child.astext() == uri else f"[{text}]({uri})")
            elif isinstance(child, (nodes.target, nodes.problematic)):
                continue
            else:
                self.fail(child, "unsupported inline markup")
        return " ".join("".join(out).split())

    # ------------------------------------------------------------------ blocks
    def blocks(self, node, depth: int) -> List[str]:
        out: List[str] = []
        for child in node.children:
            out.extend(self.block(child, depth))
        return out

    def block(self, node, depth: int) -> List[str]:
        from docutils import nodes

        if isinstance(node, nodes.section):
            title = node.next_node(nodes.title)
            heading = "#" * min(depth + 2, 6)
            body = [c for c in node.children if not isinstance(c, nodes.title)]
            out = [f"{heading} {self.inline(title)}", ""]
            for child in body:
                out.extend(self.block(child, depth + 1))
            return out
        if isinstance(node, nodes.paragraph):
            return [self.inline(node), ""]
        if isinstance(node, (nodes.bullet_list, nodes.enumerated_list)):
            out = []
            for i, item in enumerate(node.children, start=1):
                marker = "-" if isinstance(node, nodes.bullet_list) else f"{i}."
                lines = self.blocks(item, depth)
                while lines and lines[-1] == "":
                    lines.pop()
                first, rest = (lines[0], lines[1:]) if lines else ("", [])
                out.append(f"{marker} {first}")
                out.extend(("    " + line) if line else "" for line in rest)
            return out + [""]
        if isinstance(node, nodes.literal_block):
            classes = [c for c in node.get("classes", []) if c != "code"]
            lang = classes[0] if classes else "text"
            return [f"```{lang}", node.astext(), "```", ""]
        if isinstance(node, nodes.table):
            return self.table(node)
        if node.tagname in _ADMONITIONS:
            lines = self.blocks(node, depth)
            return [f"!!! {node.tagname}", ""] + [("    " + line) if line else ""
                                                  for line in lines] + [""]
        if isinstance(node, (nodes.comment, nodes.target, nodes.substitution_definition)):
            return []
        self.fail(node)
        return []

    def table(self, node) -> List[str]:
        from docutils import nodes

        group = node.next_node(nodes.tgroup)
        cols = int(group["cols"])
        head = group.next_node(nodes.thead)
        body = group.next_node(nodes.tbody)

        def cells(row) -> List[str]:
            entries = [e for e in row.children if isinstance(e, nodes.entry)]
            for entry in entries:
                if entry.get("morecols") or entry.get("morerows"):
                    self.fail(entry, "table cells spanning rows or columns")
            texts = []
            for entry in entries:
                parts = []
                for child in entry.children:
                    if not isinstance(child, nodes.paragraph):
                        self.fail(child, "only paragraphs in table cells")
                    parts.append(self.inline(child))
                texts.append("<br>".join(parts))
            return texts + [""] * (cols - len(texts))

        header = cells(head.children[0]) if head is not None else [" "] * cols
        lines = ["| " + " | ".join(header) + " |", "|" + "---|" * cols]
        for row in (body.children if body is not None else []):
            lines.append("| " + " | ".join(cells(row)) + " |")
        return lines + [""]


def board_page(yml_path: str, repo_url: str = "") -> str:
    """The Markdown page of one board, from its board.yml and doc/index.rst."""
    board_dir = os.path.dirname(yml_path)
    rst_path = os.path.join(board_dir, "doc", "index.rst")
    rel_dir = os.path.relpath(board_dir, ROOT).replace(os.sep, "/")
    rel_rst = f"{rel_dir}/doc/index.rst"
    board = _board_yml(yml_path)
    name = board.get("name") or os.path.basename(board_dir)
    with open(rst_path, encoding="utf-8") as f:
        tree = _doctree(f.read(), rel_rst)

    socs = ", ".join(f"`{s['name']}`" for s in board.get("socs", []) if "name" in s)
    source = f"[`{rel_dir}/`]({repo_url.rstrip('/')}/tree/main/{rel_dir})" if repo_url \
        else f"`{rel_dir}/`"
    lines = [
        f"<!-- Generated by tools/docs/boards.py from {rel_rst}: edit that file. -->",
        "",
        f"# {board.get('full_name', name)}",
        "",
        "| Zephyr board | SoC | Board files |",
        "|---|---|---|",
        f"| `{name}` | {socs} | {source} |",
        "",
        "```bash",
        f"west build -p always --sysbuild -b {name} -s app",
        "```",
        "",
        f"This page is the Zephyr board documentation, `{rel_rst}`.",
        "",
    ]
    lines += _Writer(rel_rst).blocks(tree, depth=0)
    while lines and lines[-1] == "":
        lines.pop()
    return "\n".join(lines) + "\n"


def board_files() -> List[str]:
    """board.yml of every board that has a doc/index.rst, sorted by board directory."""
    found = []
    for yml in sorted(glob.glob(os.path.join(ROOT, "boards", "*", "*", "board.yml"))):
        if os.path.isfile(os.path.join(os.path.dirname(yml), "doc", "index.rst")):
            found.append(yml)
    return found


def generated_pages(repo_url: str = "") -> Dict[str, str]:
    """{docs-relative path: Markdown} of every board reference page."""
    pages = {}
    for yml in board_files():
        name = _board_yml(yml).get("name") or os.path.basename(os.path.dirname(yml))
        pages[f"{PAGE_DIR}/{name}.md"] = board_page(yml, repo_url)
    return pages


def board_sources() -> List[str]:
    """Files whose change changes a generated page (for `mkdocs serve`)."""
    out = []
    for yml in board_files():
        out += [yml, os.path.join(os.path.dirname(yml), "doc", "index.rst")]
    return out


if __name__ == "__main__":
    try:
        for path, text in generated_pages().items():
            print(f"===== docs/{path}\n{text}")
    except BoardDocError as err:
        sys.exit(f"error: {err}")
