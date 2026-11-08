# SPDX-License-Identifier: Apache-2.0
"""Test report: one self-contained HTML file (no external files)."""

import datetime
import html
import platform

from .scenarios import ERROR, FAIL, NOT_REPEATED, PASS, STOPPED

CSS = """
body{font-family:Segoe UI,Helvetica,Arial,sans-serif;margin:24px;color:#1d2330;background:#fff}
h1{font-size:22px;margin:0 0 4px} h2{font-size:17px;margin:28px 0 8px}
table{border-collapse:collapse;width:100%;font-size:13px;margin-bottom:8px}
th,td{border:1px solid #d5d9e0;padding:4px 8px;text-align:left;vertical-align:top}
th{background:#f1f3f6} .meta td:first-child{width:180px;color:#555}
.PASS{color:#0a7a32;font-weight:600} .FAIL,.ERROR{color:#b3261e;font-weight:600}
.STOPPED,.INFO{color:#7a5a00;font-weight:600}
.summary span{display:inline-block;margin-right:18px;font-size:15px}
.note{color:#555;font-size:12px} code{font-size:12px}
"""


def _e(text):
    return html.escape(str(text))


def _check_status(check):
    return "INFO" if check.ok is None else (PASS if check.ok else FAIL)


def build(results, connection, update=None, title="Doppelstep test report"):
    """HTML text. `connection`: dict of the connection data shown at the top."""
    now = datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    counts = {s: sum(1 for x in results if x.status == s) for s in (PASS, FAIL, ERROR, STOPPED)}
    parts = [counts[PASS] == len(results)] if results else []
    if update is not None:
        parts.append(update.ok)
    overall = PASS if parts and all(parts) else FAIL
    out = [f"<!doctype html><html lang='en'><head><meta charset='utf-8'><title>{_e(title)}"
           f"</title><style>{CSS}</style></head><body>",
           f"<h1>{_e(title)}</h1><p class='note'>{_e(now)} · host {_e(platform.node())}</p>",
           "<table class='meta'>"]
    for key, value in connection.items():
        out.append(f"<tr><td>{_e(key)}</td><td>{_e(value)}</td></tr>")
    out.append("</table>")

    out.append(f"<h2>Result: <span class='{overall}'>{overall}</span></h2>")
    if results:
        out.append("<p class='summary'>")
        for status, count in counts.items():
            out.append(f"<span class='{status}'>{status} {count}</span>")
        out.append(f"<span>of {len(results)} scenarios</span></p>")

    if update is not None:
        out.append("<h2>Firmware update</h2><table class='meta'>")
        out.append(f"<tr><td>File</td><td><code>{_e(update.file)}</code></td></tr>"
                   f"<tr><td>Version</td><td>{_e(update.file_version)}</td></tr>"
                   f"<tr><td>SHA-256 (TLV)</td><td><code>{_e(update.file_sha256)}</code></td></tr>"
                   f"<tr><td>Size</td><td>{update.size} bytes</td></tr>"
                   f"<tr><td>Result</td><td class='{PASS if update.ok else FAIL}'>"
                   f"{PASS if update.ok else FAIL}</td></tr></table>")
        out.append("<table><tr><th>Step</th><th>Result</th><th>Detail</th><th>Time</th></tr>")
        for step in update.steps:
            status = PASS if step.ok else FAIL
            out.append(f"<tr><td>{_e(step.name)}</td><td class='{status}'>{status}</td>"
                       f"<td>{_e(step.detail)}</td><td>{step.seconds:.2f} s</td></tr>")
        out.append("</table>")

    if results:
        out.append("<h2>Scenarios</h2><table><tr><th>Scenario</th><th>Result</th>"
                   "<th>SIL test</th><th>Time</th></tr>")
        for res in results:
            out.append(f"<tr><td>{_e(res.scenario.title)}</td><td class='{res.status}'>"
                       f"{res.status}</td><td><code>{_e(res.scenario.source)}</code></td>"
                       f"<td>{res.seconds:.1f} s</td></tr>")
        out.append("</table>")

        for res in results:
            sc = res.scenario
            out.append(f"<h2>{_e(sc.title)} — <span class='{res.status}'>{res.status}</span></h2>")
            notes = []
            if res.error:
                notes.append(f"<b>{_e(res.error)}</b>")
            if sc.board_note:
                notes.append("Board: " + _e(sc.board_note))
            if sc.omitted:
                notes.append("Not checked on the board (" + _e(sc.omitted) + ")")
            if sc.eeprom:
                notes.append("EEPROM changed and restored: " + _e(sc.eeprom))
            for note in notes:
                out.append(f"<p class='note'>{note}</p>")
            out.append("<table><tr><th>Check</th><th>Measured</th><th>Expected</th>"
                       "<th>Result</th></tr>")
            for check in res.checks:
                status = _check_status(check)
                note = f"<div class='note'>{_e(check.note)}</div>" if check.note else ""
                out.append(f"<tr><td>{_e(check.name)}{note}</td><td>{_e(check.measured)}</td>"
                           f"<td>{_e(check.expected)}</td><td class='{status}'>{status}</td></tr>")
            out.append("</table>")

        out.append("<h2>SIL tests not repeated on the board</h2><table>")
        for source, reason in NOT_REPEATED.items():
            out.append(f"<tr><td><code>{_e(source)}</code></td><td>{_e(reason)}</td></tr>")
        out.append("</table>")
    out.append("</body></html>")
    return "\n".join(out)


def save(path, text):
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)
