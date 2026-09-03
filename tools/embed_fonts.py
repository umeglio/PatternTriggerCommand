#!/usr/bin/env python3
# embed_fonts.py - rigenera miraFONT_embedded.h a partire dai file WOFF in fonts/miraFONT
# Autore: Umberto Meglio - Supporto alla creazione: Claude di Anthropic
import base64, os, sys
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FILES = [
    ("SPLINE_REGULAR", "fonts/miraFONT/miraFONT-Regular-Spline.woff"),
    ("SPLINE_BOLD", "fonts/miraFONT/miraFONT-Bold-Spline.woff"),
    ("LINEAR_REGULAR", "fonts/miraFONT/miraFONT-Regular-Linear.woff"),
    ("DOTS_REGULAR", "fonts/miraFONT/miraFONT-Regular-Dots.woff"),
]
out = ["// miraFONT_embedded.h - font miraFONT incorporati nell'eseguibile (base64, formato WOFF)",
       "// miraFONT e' il carattere tipografico originale di Umberto Meglio (https://github.com/umeglio/miraFONT), licenza CC0 1.0.",
       "// File generato da tools/embed_fonts.py - non modificare a mano.",
       "// Autore: Umberto Meglio - Supporto alla creazione: Claude di Anthropic", "",
       "#ifndef MIRAFONT_EMBEDDED_H", "#define MIRAFONT_EMBEDDED_H", ""]
for name, rel in FILES:
    with open(os.path.join(ROOT, rel), "rb") as f:
        b = base64.b64encode(f.read()).decode()
    out.append("static const char MIRAFONT_%s_B64[] =" % name)
    for i in range(0, len(b), 100):
        out.append('    "%s"' % b[i:i + 100])
    out[-1] += ";"
    out.append("")
out.append("#endif // MIRAFONT_EMBEDDED_H")
with open(os.path.join(ROOT, "miraFONT_embedded.h"), "w") as f:
    f.write("\n".join(out) + "\n")
print("miraFONT_embedded.h rigenerato")
