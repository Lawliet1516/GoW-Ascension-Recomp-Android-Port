import sys, markdown, weasyprint
src, out = sys.argv[1], sys.argv[2]
body = markdown.markdown(open(src, encoding="utf-8").read(), extensions=["tables"])
css = """
@page { size: A4 landscape; margin: 14mm 12mm; @bottom-right { content: counter(page) " / " counter(pages); font-size: 8pt; color: #666; } }
body { font-family: "Liberation Sans", sans-serif; font-size: 9.5pt; color: #1a1a1a; line-height: 1.35; }
h1 { font-size: 18pt; margin: 0 0 2pt; color: #7a1010; }
h2 { font-size: 12.5pt; margin: 14pt 0 5pt; color: #7a1010; border-bottom: 1px solid #ddd; padding-bottom: 2pt; }
h1 + h2 { border: none; color: #444; margin-top: 0; }
table { border-collapse: collapse; width: 100%; margin: 4pt 0 6pt; page-break-inside: auto; }
tr { page-break-inside: avoid; }
th { background: #7a1010; color: #fff; text-align: left; padding: 4pt 5pt; font-size: 9pt; }
td { border-bottom: 1px solid #e2e2e2; padding: 3.5pt 5pt; vertical-align: top; }
tr:nth-child(even) td { background: #f7f5f5; }
code { font-family: "Liberation Mono", monospace; font-size: 8.6pt; background: #f1efef; padding: 0 2pt; border-radius: 2pt; }
ul { margin: 3pt 0; padding-left: 16pt; } li { margin: 2pt 0; }
"""
weasyprint.HTML(string=f"<html><head><meta charset='utf-8'><style>{css}</style></head><body>{body}</body></html>").write_pdf(out)
