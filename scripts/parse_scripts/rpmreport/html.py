# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

"""Self-contained HTML report: plain tables plus CSS-only top-down bars, no scripts."""
from __future__ import annotations

from html import escape
from typing import Optional

from . import metrics
from .results import Results, Table

STYLE = """
body { font: 14px/1.4 system-ui, sans-serif; margin: 0; background: #fcfcfd; color: #23272e; }
header { background: #2b3a55; color: #fff; padding: 16px 24px; }
header h1 { margin: 0 0 8px; font-size: 20px; }
header dl { display: grid; grid-template-columns: max-content 1fr; gap: 2px 12px; margin: 0; }
header dt { opacity: .75; } header dd { margin: 0; }
nav { padding: 8px 24px; border-bottom: 1px solid #dde1e7; }
nav a { margin-right: 14px; }
main { padding: 0 24px 48px; }
h2 { margin-top: 32px; border-bottom: 2px solid #2b3a55; }
h3 { margin: 18px 0 6px; font-size: 15px; }
.scroll { overflow-x: auto; }
table { border-collapse: collapse; font-variant-numeric: tabular-nums; }
th, td { border: 1px solid #dde1e7; padding: 3px 8px; }
th { background: #eef1f5; text-align: left; }
td { text-align: right; }
tr.foot td, tr.foot th { font-weight: 600; border-top: 2px solid #9aa3b0; }
.bars td { text-align: left; }
.bar { display: flex; width: 420px; height: 14px; }
.bar span { display: block; height: 100%; }
.key span { display: inline-block; width: 12px; height: 12px; margin: 0 4px 0 12px; vertical-align: middle; }
"""
NODE_COLOURS = {"retiring": "#5aa469", "bad_speculation": "#c95b56",
                "frontend_bound": "#e0a73e", "backend_bound": "#4a78b0"}


def fmt(v, pct: bool = False) -> str:
    if v is None:
        return ""
    if isinstance(v, str):
        return escape(v)
    if isinstance(v, int):
        return str(v)
    if pct:
        return f"{v:+.4g}%"
    return f"{v:,.0f}" if abs(v) >= 1e4 else f"{v:.5g}"


def shade(v: Optional[float], better: Optional[str]) -> str:
    if v is None or not better or v == 0:
        return ""
    good = (v > 0) == (better == "higher")
    alpha = 0.15 + 0.6 * min(abs(v) / 10.0, 1.0)
    rgb = "60,160,90" if good else "205,70,70"
    return f' style="background: rgba({rgb},{alpha:.2f})"'


def table(t: Table, heading: bool = True) -> str:
    out = [f"<h3>{escape(t.title)}</h3>" if heading and t.title else "", '<div class="scroll"><table>',
           "<tr><th></th>" + "".join(f"<th>{escape(c)}</th>" for c in t.columns) + "</tr>"]
    for i, (row, cells) in enumerate(zip(t.rows, t.cells)):
        label = " / ".join(x for x in row if x) if isinstance(row, tuple) else row
        cls = ' class="foot"' if i >= len(t.rows) - t.footer else ""
        tds = "".join(f"<td{shade(v, t.better.get(c)) if c in t.pct else ''}>{fmt(v, c in t.pct)}</td>"
                      for c, v in zip(t.columns, cells))
        out.append(f"<tr{cls}><th>{escape(str(label))}</th>{tds}</tr>")
    out.append("</table></div>")
    return "\n".join(out)


def bars(r: Results, col) -> str:
    """Stacked level-1 top-down bar per complete benchmark."""
    l1 = [n for n in metrics.TOPDOWN if n.key in metrics.TOPDOWN_L1]
    rows = []
    for bench in r.benchmarks:
        vals = [r.value(n.fn, col, bench) for n in l1]
        if None in vals:
            continue
        spans = "".join(f'<span title="{n.label} {v:.1%}" style="width:{max(v, 0) * 100:.2f}%;'
                        f'background:{NODE_COLOURS[n.key]}"></span>' for n, v in zip(l1, vals))
        rows.append(f"<tr><th>{escape(bench)}</th><td><div class=\"bar\">{spans}</div></td></tr>")
    if not rows:
        return ""
    key = "".join(f'<span style="background:{NODE_COLOURS[n.key]}"></span>{n.label}' for n in l1)
    return f'<div class="key">{key}</div><table class="bars">{"".join(rows)}</table>'


def section(anchor: str, title: str, body: list[str]) -> tuple[str, str, str]:
    content = f'<section id="{anchor}"><h2>{escape(title)}</h2>\n' + "\n".join(body) + "</section>"
    return anchor, title, content


def page(r: Results) -> str:
    h = r.header()
    missing = ", ".join(f"{c}: {b}" for c, b in h["missing"]) or "none"
    info = [("Studies", "<br>".join(escape(s) for s in h["studies"])),
            ("Generated", h["generated"]), ("Columns", escape(", ".join(h["columns"]))),
            ("Benchmarks", str(h["benchmarks"])), ("Benchmarks in means", escape(h["in_means"])),
            ("Suite", "<br>".join(escape(s) for s in h["suite"])),
            ("Baseline", escape(h["baseline"])), ("Left out (incomplete or absent)", escape(missing)),
            ("Skipped configs and dumps", "<br>".join(escape(s) for s in h["skipped"]) or "none")]
    head = ("<header><h1>RPM performance report</h1><dl>"
            + "".join(f"<dt>{k}</dt><dd>{v}</dd>" for k, v in info) + "</dl></header>")

    sections = []
    if len(r.cols) > 1:
        sections.append(section("comparison", "Comparison", [table(t) for t in r.comparison_tables()]))
    sections.append(section("headline", "Headline", [table(r.headline_table())]))
    sections.append(section("summary", "Summary", [table(r.summary_table(c)) for c in r.cols]))
    td = []
    for c in r.cols:
        td += [table(t) for t in r.topdown_tables(c)]
        if metrics.TOPDOWN_COUNTER in r.present():
            td.append(bars(r, c))
    sections.append(section("topdown", "Top-down", td or ["<p>No top-down counters in these dumps.</p>"]))
    for i, group in enumerate(metrics.GROUPS):
        tables = r.group_tables(group)
        if tables:
            sections.append(section(f"group{i}", group, [table(t) for t in tables]))
    sections.append(section("inputs", "Per input", [table(r.input_table(c)) for c in r.cols]))
    sections.append(section("runtime", "Host runtime",
                            [table(r.runtime_table(c)) for c in r.cols] + [table(r.slowest_table())]))

    nav = "<nav>" + "".join(f'<a href="#{a}">{escape(t)}</a>' for a, t, _ in sections) + "</nav>"
    return ("<!DOCTYPE html>\n<html><head><meta charset=\"utf-8\"><title>RPM performance report</title>"
            f"<style>{STYLE}</style></head><body>\n{head}\n{nav}\n<main>\n"
            + "\n".join(s for _, _, s in sections) + "\n</main></body></html>\n")
