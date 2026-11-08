"""Render the repository's AI strategy Markdown as a self-contained review document."""
from __future__ import annotations

import argparse
import hashlib
import html
import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "docs" / "ai-assistance-strategy.md"
OUTPUT = SOURCE.with_suffix(".html")
CATALOG = ROOT / "docs" / "ai-assistance-pages.json"


def inline(value: str) -> str:
    escaped = html.escape(value)
    return re.sub(
        r"\[([^\]]+)\]\((https://[^)\s]+)\)",
        lambda m: '<a href="' + m[2] + '" target="_blank" rel="noopener noreferrer">' + m[1] + "</a>",
        escaped,
    )


def render(source: str) -> str:
    lines = source.splitlines()
    body: list[str] = []
    nav: list[str] = []
    index = 0
    section = 0
    while index < len(lines):
        line = lines[index]
        if not line.strip():
            index += 1
            continue
        heading = re.match(r"^(#{1,3}) (.+)$", line)
        if heading:
            level = len(heading[1])
            label = heading[2]
            identifier = ""
            if level == 2:
                section += 1
                identifier = f' id="section-{section}"'
                nav.append(f'<a href="#section-{section}">{inline(label)}</a>')
            body.append(f"<h{level}{identifier}>{inline(label)}</h{level}>")
            index += 1
            continue
        if line.startswith("|"):
            rows = []
            while index < len(lines) and lines[index].startswith("|"):
                cells = [c.strip() for c in lines[index].strip("|").split("|")]
                if not all(re.fullmatch(r":?-+:?", c) for c in cells):
                    rows.append(cells)
                index += 1
            if not rows or not all(len(r) == len(rows[0]) for r in rows):
                raise ValueError("Malformed Markdown table")
            head = "".join(f'<th scope="col">{inline(c)}</th>' for c in rows[0])
            table_body = "".join("<tr>" + "".join(f"<td>{inline(c)}</td>" for c in row) + "</tr>" for row in rows[1:])
            body.append(f'<div class="table-scroll" tabindex="0" role="region" aria-label="对照表，可横向滚动"><table><thead><tr>{head}</tr></thead><tbody>{table_body}</tbody></table></div>')
            continue
        if line.startswith("    "):
            code = []
            while index < len(lines) and (lines[index].startswith("    ") or not lines[index]):
                code.append(lines[index][4:] if lines[index] else "")
                index += 1
            body.append("<pre><code>" + html.escape("\n".join(code).rstrip()) + "</code></pre>")
            continue
        paragraph = []
        while index < len(lines) and lines[index].strip() and not lines[index].startswith(("#", "|", "    ")):
            paragraph.append(lines[index])
            index += 1
        body.append("<p>" + inline(" ".join(paragraph)) + "</p>")
    if section != 17:
        raise ValueError(f"Expected 17 sections, found {section}")
    digest = hashlib.sha256(source.encode("utf-8")).hexdigest()
    return """<!doctype html>
<html lang="zh-CN"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<meta name="color-scheme" content="light">
<title>蓝牙网关平台 AI 辅助接入与页面优化策略</title>
<style>
*{box-sizing:border-box}html{scroll-behavior:smooth;scroll-padding-top:24px}
body{margin:0;color:#202a33;background:#fff;font:16px/1.9 "Microsoft YaHei","Noto Sans CJK SC",Arial,sans-serif}
.layout{max-width:1460px;margin:auto;display:grid;grid-template-columns:246px minmax(0,1fr)}
aside{position:sticky;top:0;height:100vh;overflow:auto;padding:34px 20px;background:#f5f7f8;border-right:1px solid #e0e5e8}
.nav-title{font-size:16px;font-weight:700;color:#000;margin:0 0 6px}.edition{font-size:12px;color:#53616d;margin:0 0 22px}
nav{display:grid;gap:4px}nav a{font-size:13px;line-height:1.55;padding:7px 8px;color:#344551;text-decoration:none;border-radius:4px}
nav a:hover,nav a:focus{background:#e3ebef;color:#004976}nav a[aria-current=true]{background:#e3ebef;font-weight:700}
main{min-width:0;padding:52px clamp(26px,4vw,66px) 64px;max-width:1130px}
.toolbar{display:flex;justify-content:space-between;align-items:center;gap:12px;color:#53616d;font-size:13px;margin-bottom:24px}
button{font:inherit;color:#194e6f;background:white;border:1px solid #bacad5;border-radius:5px;padding:7px 16px;cursor:pointer}
button:hover{background:#f1f6f9}a{color:#005b85;text-underline-offset:3px;overflow-wrap:anywhere}
h1,h2,h3{color:#000;line-height:1.4;font-weight:700;word-break:normal}
h1{font-size:31px;margin:0 0 20px;max-width:850px}h2{font-size:24px;margin:55px 0 20px;scroll-margin-top:25px}
h3{font-size:19px;margin:34px 0 13px}p{margin:0 0 18px;overflow-wrap:anywhere}
.table-scroll{width:100%;overflow-x:auto;margin:20px 0 25px}
table{width:100%;border-collapse:collapse;min-width:640px;font-size:14px;line-height:1.75}
th,td{border:1px solid #d9d9d9;padding:12px 14px;vertical-align:middle;text-align:left;overflow-wrap:anywhere}
th{background:#e7edf2;color:#000;font-weight:700}tbody tr:nth-child(even){background:#f8fafb}
td:first-child{min-width:105px;max-width:210px}thead{display:table-header-group}
pre{font:14px/1.8 Consolas,"Microsoft YaHei",monospace;white-space:pre;overflow-x:auto;padding:20px;background:#f5f7f8;border:1px solid #d9dfe3}
footer{margin-top:45px;font-size:12px;color:#62707b;overflow-wrap:anywhere}
:focus-visible{outline:3px solid #1677a3;outline-offset:2px}
@media(max-width:900px){.layout{grid-template-columns:1fr}aside{position:relative;height:auto;max-height:280px;padding:20px;border-right:0;border-bottom:1px solid #e0e5e8}
nav{grid-template-columns:repeat(2,minmax(0,1fr))}main{padding:30px 20px 48px}h1{font-size:26px}h2{font-size:22px;margin-top:42px}}
@media(max-width:480px){body{font-size:15px}nav{grid-template-columns:1fr}aside{max-height:230px}h1{font-size:24px}.toolbar{align-items:flex-start}table{font-size:13px}
.table-scroll:before{content:"左右滑动查看完整表格";display:block;color:#62707b;font-size:12px;margin-bottom:6px}}
@media print{@page{size:A4;margin:18mm 16mm}html{scroll-behavior:auto}.layout{display:block;max-width:none}aside,.toolbar{display:none}
body{font-size:10.5pt;line-height:1.7}main{padding:0;max-width:none}h1{font-size:22pt}h2{font-size:16pt;margin-top:26pt;break-after:avoid}
h3{font-size:12.5pt;break-after:avoid}p{orphans:3;widows:3}table{font-size:9pt;min-width:0}th,td{padding:7pt}
tr{break-inside:avoid}.table-scroll{overflow:visible}pre{white-space:pre-wrap;font-size:9pt;break-inside:avoid}a{color:#202a33}footer{font-size:8pt}}
</style></head><body><div class="layout">
<aside aria-label="文档目录"><p class="nav-title">AI 辅助接入策略</p><p class="edition">V1.0 · 2026年11月8日<br>方案文档 · 新增功能待开发</p><nav>""" + "\n".join(nav) + """</nav></aside>
<main><div class="toolbar"><span>本地部署蓝牙网关管理平台 · 产品与研发方案</span><button type="button" onclick="window.print()">打印文档</button></div>
""" + "\n".join(body) + f"""
<footer>文档由 ai-assistance-strategy.md 生成，内容版本以仓库源文件为准。<br>源文件 SHA256 {digest}</footer>
</main></div><script>
const sections=[...document.querySelectorAll('h2[id]')];
const observer=new IntersectionObserver(entries=>{{entries.forEach(e=>{{if(e.isIntersecting){{document.querySelectorAll('nav a').forEach(a=>a.removeAttribute('aria-current'));const a=document.querySelector('nav a[href="#'+e.target.id+'"]');if(a)a.setAttribute('aria-current','true');}}}})}},{{rootMargin:'0px 0px -70% 0px'}});
sections.forEach(s=>observer.observe(s));
</script></body></html>
"""


def validate_catalog() -> dict:
    data = json.loads(CATALOG.read_text(encoding="utf-8"))
    pages = data["pages"]
    actual = {
        "categories": len(data["groups"]),
        "businessPages": len([p for p in pages if p["id"] != "P00"]),
        "loginPages": 1,
        "totalPages": len(pages),
        "menuEntries": sum(p["menu"] for p in pages),
        "contextualPages": sum(not p["menu"] and p["id"] != "P00" for p in pages),
    }
    if actual != data["counts"]:
        raise ValueError(f"Catalog count mismatch: {actual}")
    if len({p["id"] for p in pages}) != len(pages) or len({p["route"] for p in pages}) != len(pages):
        raise ValueError("Duplicate page id or route")
    for group in data["groups"]:
        if sum(p["group"] == group["id"] for p in pages) != group["pages"]:
            raise ValueError("Group page count mismatch")
    return actual


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    source = SOURCE.read_text(encoding="utf-8")
    output = render(source)
    counts = validate_catalog()
    if args.check:
        if not OUTPUT.exists() or OUTPUT.read_text(encoding="utf-8") != output:
            raise SystemExit("HTML is stale; regenerate with this script")
    else:
        OUTPUT.write_text(output, encoding="utf-8")
    print(json.dumps({"status": "verified" if args.check else "generated", "counts": counts,
                      "sourceCharacters": len(source), "output": str(OUTPUT)}, ensure_ascii=False))
