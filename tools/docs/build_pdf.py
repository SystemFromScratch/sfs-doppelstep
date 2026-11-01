#!/usr/bin/env python3
"""Build the printable PDF manual from docs/.

Steps:
  1. `mkdocs build` (mkdocs.yml in the repository root). The print-site plugin
     combines every page into site/print_page/index.html with a cover page,
     a table of contents and numbered chapters, headings and figures.
  2. Chromium (through Playwright) opens that page, waits for the Mermaid
     diagrams and images, and prints it to A4 with a header and page numbers.
     A first pass finds the page of every table-of-contents entry, and the
     second pass prints the table of contents with those page numbers.
  3. The cover page (no header or page number) and the body are merged, with
     PDF bookmarks and document metadata.

Usage (repository root, with the docs dependencies installed):
    python tools/docs/build_pdf.py
    python tools/docs/build_pdf.py --output manual.pdf --browser msedge

Browser: --browser auto tries Playwright's Chromium, then Microsoft Edge, then
Google Chrome, so on Windows no separate browser download is needed.
Mermaid diagrams load mermaid.min.js from the internet (unpkg.com); offline,
pass a local copy with --mermaid-js PATH.
"""

from __future__ import annotations

import argparse
import datetime as dt
import functools
import http.server
import io
import os
import re
import subprocess
import sys
import threading
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_OUTPUT = ROOT / "site" / "Doppelstep.pdf"
TITLE = "Doppelstep"
SUBJECT = "Stepper motor controller seen as a DYNAMIXEL XM430-W350"

# Page geometry. The CSS @page margin must match the Playwright margin,
# otherwise Chromium lets the CSS value win and the header overlaps the text.
MARGIN = {"top": "22mm", "bottom": "20mm", "left": "17mm", "right": "17mm"}
PAGE_CSS = (
    "@page { size: A4 portrait; margin: 22mm 17mm 20mm 17mm;"
    " @bottom-left { content: none; } @bottom-center { content: none; }"
    " @bottom-right { content: none; } }"
)
# (The print-site plugin CSS adds its own page numbers in @page margin boxes;
# they are switched off because the header and footer come from Playwright.)
FONT = "font-family: 'Segoe UI', Roboto, Helvetica, Arial, sans-serif;"
MARKER = "DPSTOCMARK{:04d}Z"
MARKER_RE = re.compile(r"DPSTOCMARK(\d{4})Z")


def fail(message: str) -> "NoReturn":  # type: ignore[name-defined]
    print(f"error: {message}", file=sys.stderr)
    sys.exit(1)


def git_version() -> str:
    try:
        out = subprocess.run(
            ["git", "describe", "--tags", "--always", "--dirty"],
            cwd=ROOT, capture_output=True, text=True, timeout=10, check=True,
        )
        return out.stdout.strip() or "development snapshot"
    except (OSError, subprocess.SubprocessError):
        return "development snapshot"


def mkdocs_build(version: str, date: str) -> None:
    env = dict(os.environ, DPS_DOCS_VERSION=version, DPS_DOCS_DATE=date, NO_MKDOCS_2_WARNING="1")
    print(f"mkdocs build (version {version}, {date})")
    result = subprocess.run([sys.executable, "-m", "mkdocs", "build", "--clean"], cwd=ROOT, env=env)
    if result.returncode != 0:
        fail("mkdocs build failed (see the messages above)")


class QuietHandler(http.server.SimpleHTTPRequestHandler):
    def log_message(self, *args) -> None:  # noqa: D401 - silence the access log
        pass


def serve(directory: Path) -> tuple[http.server.ThreadingHTTPServer, str]:
    handler = functools.partial(QuietHandler, directory=str(directory))
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return server, f"http://127.0.0.1:{server.server_address[1]}"


def launch_browser(pw, choice: str):
    order = {"auto": [None, "msedge", "chrome"], "chromium": [None], "msedge": ["msedge"], "chrome": ["chrome"]}[choice]
    errors = []
    for channel in order:
        try:
            browser = pw.chromium.launch(channel=channel) if channel else pw.chromium.launch()
            print(f"browser: {channel or 'Playwright Chromium'} {browser.version}")
            return browser
        except Exception as exc:  # Playwright raises its own Error type
            errors.append(f"{channel or 'chromium'}: {str(exc).splitlines()[0]}")
    fail("no usable browser found:\n  " + "\n  ".join(errors)
         + "\nInstall one with:  python -m playwright install chromium")


# JavaScript run in the print page -------------------------------------------

JS_WAIT_READY = """() => {
  const mermaidPending = document.querySelectorAll('pre.mermaid').length;
  const imagesPending = [...document.images].filter(i => !i.complete).length;
  return mermaidPending === 0 && imagesPending === 0;
}"""

# Adds dot leaders and page-number placeholders to the ToC, and an invisible,
# absolutely positioned (so layout-neutral) text marker at every ToC target.
JS_PREPARE_TOC = """(markerFmt) => {
  // The plugin lists chapters only; add every chapter's level-2 headings.
  document.querySelectorAll('#print-page-toc a[href^="#"]').forEach(a => {
    const section = document.getElementById(decodeURIComponent(a.getAttribute('href').slice(1)));
    if (!section || !section.matches('section.print-page') || section.matches('.md-section')) return;
    const headings = [...section.querySelectorAll(':scope > h2[id]')];
    if (!headings.length) return;
    const prefix = section.getAttribute('heading-number');
    const ul = document.createElement('ul');
    ul.className = 'dps-toc-headings';
    headings.forEach((h, k) => {
      const clone = h.cloneNode(true);
      clone.querySelectorAll('.headerlink').forEach(x => x.remove());
      const li = document.createElement('li');
      const link = document.createElement('a');
      link.href = '#' + h.id;
      link.textContent = (prefix ? prefix + '.' + (k + 1) + ' ' : '') + clone.textContent.trim();
      li.appendChild(link);
      ul.appendChild(li);
    });
    a.parentElement.appendChild(ul);
  });
  const links = [...document.querySelectorAll('#print-page-toc a[href^="#"]')];
  const targets = [];
  links.forEach((a, i) => {
    const id = decodeURIComponent(a.getAttribute('href').slice(1));
    const target = document.getElementById(id);
    const text = document.createElement('span');
    text.className = 'dps-toc-text';
    while (a.firstChild) text.appendChild(a.firstChild);
    const leader = document.createElement('span');
    leader.className = 'dps-toc-leader';
    const page = document.createElement('span');
    page.className = 'dps-toc-page';
    page.textContent = '000';
    a.append(text, leader, page);
    a.dataset.dpsIndex = i;
    if (target) {
      const m = document.createElement('span');
      m.className = 'dps-pdf-marker';
      m.textContent = markerFmt.replace('{:04d}', String(i).padStart(4, '0'));
      m.style.cssText = 'position:absolute;font-size:2px;line-height:1;color:#fff;';
      target.insertBefore(m, target.firstChild);
      targets.push(i);
    }
  });
  return {links: links.length, targets: targets.length};
}"""

# Material renders Mermaid inside a shadow root, which page CSS cannot reach,
# so tall diagrams are scaled down here to fit on a page with their heading.
DIAGRAM_MAX_HEIGHT_PX = 760  # about 200 mm
JS_FIT_DIAGRAMS = """(maxHeight) => {
  document.querySelectorAll('.mermaid').forEach(el => {
    const h = el.getBoundingClientRect().height;
    if (h > maxHeight) el.style.zoom = (maxHeight / h).toFixed(3);
  });
}"""

JS_FILL_TOC = """(pages) => {
  document.querySelectorAll('#print-page-toc a[data-dps-index]').forEach(a => {
    const p = pages[a.dataset.dpsIndex];
    a.querySelector('.dps-toc-page').textContent = p === undefined ? '' : String(p);
  });
  document.querySelectorAll('.dps-pdf-marker').forEach(m => m.remove());
}"""


def header_footer(version: str) -> tuple[str, str]:
    style = f"{FONT} font-size: 7.5pt; color: #666; width: 100%; margin: 0 17mm;"
    header = (
        f'<div style="{style} display: flex; justify-content: space-between;'
        f' border-bottom: 0.5pt solid #bbb; padding-bottom: 1.5mm;">'
        f"<span>{TITLE}</span><span>{SUBJECT}</span></div>"
    )
    footer = (
        f'<div style="{style} display: flex; justify-content: space-between;">'
        f"<span>Version {version}</span>"
        '<span>Page <span class="pageNumber"></span> of <span class="totalPages"></span></span></div>'
    )
    return header, footer


def marker_pages(pdf_bytes: bytes) -> dict[int, int]:
    from pypdf import PdfReader

    found: dict[int, int] = {}
    for number, page in enumerate(PdfReader(io.BytesIO(pdf_bytes)).pages, start=1):
        text = re.sub(r"\s+", "", page.extract_text() or "")
        for match in MARKER_RE.finditer(text):
            found.setdefault(int(match.group(1)), number)
    return found


def build_pdf(args: argparse.Namespace, version: str, date: str) -> None:
    from playwright.sync_api import sync_playwright
    from pypdf import PdfReader, PdfWriter

    site = ROOT / "site"
    if not (site / "print_page" / "index.html").is_file():
        fail("site/print_page/index.html is missing; run without --skip-build")

    server, base = serve(site)
    header, footer = header_footer(version)
    try:
        with sync_playwright() as pw:
            browser = launch_browser(pw, args.browser)
            page = browser.new_page(viewport={"width": 1100, "height": 1500})
            if args.mermaid_js:
                local = Path(args.mermaid_js).read_bytes()
                page.route(re.compile(r".*/mermaid(@[^/]+)?/dist/mermaid(\.min)?\.js.*"),
                           lambda route: route.fulfill(body=local, content_type="application/javascript"))
            page.goto(f"{base}/print_page/index.html", wait_until="networkidle")
            page.emulate_media(media="print")
            try:
                page.wait_for_function(JS_WAIT_READY, timeout=args.timeout * 1000)
            except Exception:
                fail("Mermaid diagrams or images did not finish loading. Without internet access, "
                     "pass a local mermaid.min.js (Mermaid 11) with --mermaid-js PATH.")
            page.add_style_tag(content=PAGE_CSS)
            page.evaluate(JS_FIT_DIAGRAMS, DIAGRAM_MAX_HEIGHT_PX)
            page.wait_for_timeout(300)

            counts = page.evaluate(JS_PREPARE_TOC, MARKER)
            print(f"table of contents: {counts['links']} entries")

            body_css = page.add_style_tag(content="#print-site-cover-page { display: none !important; }"
                                                  " #print-site-cover-page + section.print-page"
                                                  " { break-before: auto !important; }")
            pdf_options = dict(
                format="A4", margin=MARGIN, print_background=True,
                display_header_footer=True, header_template=header, footer_template=footer,
                outline=True, tagged=True,
            )
            first = page.pdf(**pdf_options)
            pages = marker_pages(first)
            missing = counts["targets"] - len(pages)
            if missing:
                print(f"warning: page number not found for {missing} table-of-contents entries")
            page.evaluate(JS_FILL_TOC, {str(k): v for k, v in pages.items()})
            body = page.pdf(**pdf_options)
            if len(PdfReader(io.BytesIO(body)).pages) != len(PdfReader(io.BytesIO(first)).pages):
                print("warning: page count changed between passes; check the table of contents")

            # Cover page only, without header and footer.
            body_css.evaluate("el => el.remove()")
            page.add_style_tag(content="#print-site-page > section.print-page { display: none !important; }"
                                       " #print-site-cover-page { break-after: auto !important; }")
            cover = page.pdf(format="A4", margin=MARGIN, print_background=True)
            browser.close()
    finally:
        server.shutdown()

    writer = PdfWriter()
    cover_reader = PdfReader(io.BytesIO(cover))
    writer.add_page(cover_reader.pages[0])
    writer.append(PdfReader(io.BytesIO(body)), import_outline=True)
    writer.add_metadata({
        "/Title": f"{TITLE} - documentation",
        "/Subject": SUBJECT,
        "/Author": "Anton Vynohradov (SystemFromScratch)",
        "/Creator": "MkDocs + mkdocs-print-site-plugin + Chromium (tools/docs/build_pdf.py)",
        "/Keywords": f"Zephyr, B-G431B-ESC1, STM32G431, Stepper-C, DYNAMIXEL Protocol 2.0, version {version}, {date}",
    })
    writer.page_mode = "/UseOutlines"
    output = Path(args.output).resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open("wb") as fh:
        writer.write(fh)
    print(f"PDF: {output} ({len(writer.pages)} pages)")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--output", default=str(DEFAULT_OUTPUT), help=f"PDF file (default: {DEFAULT_OUTPUT.relative_to(ROOT)})")
    ap.add_argument("--skip-build", action="store_true", help="reuse the existing site/ instead of running mkdocs build")
    ap.add_argument("--browser", choices=["auto", "chromium", "msedge", "chrome"], default="auto")
    ap.add_argument("--mermaid-js", metavar="PATH", help="local mermaid.min.js for offline builds")
    ap.add_argument("--version-label", help="version printed on the cover (default: git describe)")
    ap.add_argument("--timeout", type=int, default=60, help="seconds to wait for diagrams and images")
    args = ap.parse_args()

    try:
        import playwright  # noqa: F401
        import pypdf  # noqa: F401
    except ImportError as exc:
        fail(f"{exc.name} is not installed; run: python -m pip install -r requirements.txt")

    version = args.version_label or git_version()
    date = dt.date.today().isoformat()
    if not args.skip_build:
        mkdocs_build(version, date)
    build_pdf(args, version, date)
    return 0


if __name__ == "__main__":
    sys.exit(main())
