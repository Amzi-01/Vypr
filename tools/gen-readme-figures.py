#!/usr/bin/env python3
"""Draw the README's figures, light and dark: docs/readme/*.svg.

Plain SVG with no scripts or external fonts, because GitHub shows README images
through an <img> tag that runs neither. The light and dark files are picked by
<picture> in the README, the same way the logo is, so each matches the reader's
theme rather than their OS. Re-run after changing the numbers below.

The two chart colours are the first two slots of the data-viz reference palette,
validated for both surfaces (CVD and contrast) before use.
"""
from pathlib import Path

OUT = Path(__file__).resolve().parent.parent / "docs" / "readme"

THEMES = {
    "light": dict(ink="#101218", soft="#3a3f4a", muted="#666d7a", line="#d0d4db",
                  grid="#eceef1", raised="#ffffff", key="#101218",
                  before="#2a78d6", after="#eb6834"),
    "dark":  dict(ink="#f2f3f5", soft="#c3c7cf", muted="#8b919c", line="#30353e",
                  grid="#1c2027", raised="#151a21", key="#f2f3f5",
                  before="#3987e5", after="#d95926"),
}

MONO = "ui-monospace,SFMono-Regular,'SF Mono',Menlo,Consolas,'Liberation Mono',monospace"
SANS = "-apple-system,BlinkMacSystemFont,'Segoe UI',Helvetica,Arial,sans-serif"


def esc(t):
    return t.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


# ------------------------------------------------------------ latency chart

# Capture to publish, in the guest, same session, same metric (docs/technical.md).
LATENCY = [
    ("1920 × 1080 window", 5.98, 5.02),
    ("2560 × 1440 window", 7.42, 6.29),
    ("3200 × 1800 window", 10.80, 8.72),
    ("Whole 4K desktop",   13.82, 11.74),
]


def latency_svg(c):
    W, left, right = 760, 190, 96
    top, row, bar, gap = 92, 54, 14, 2
    H = top + row * len(LATENCY) + 40
    plot_w = W - left - right
    xmax = 15.0
    x = lambda v: left + v / xmax * plot_w

    o = [f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {W} {H}" width="{W}" height="{H}" '
         f'role="img" aria-labelledby="t d">',
         '<title id="t">Frame latency, Vypr 0.4.13 against 0.5.0</title>',
         '<desc id="d">Milliseconds from Windows capturing a frame to Vypr publishing it, lower is better. '
         + "; ".join(f"{n}: {a:.2f} ms before, {b:.2f} ms now" for n, a, b in LATENCY) + ".</desc>",
         f'<text x="0" y="22" font-family="{SANS}" font-size="17" font-weight="600" fill="{c["ink"]}">'
         'From capture in Windows to the frame being ready on Linux</text>',
         f'<text x="0" y="44" font-family="{SANS}" font-size="13" fill="{c["muted"]}">'
         'Milliseconds, measured on the same VM — lower is better</text>']

    # legend, one row above the plot
    lx = 0
    for label, col in (("0.4.13 · IVSHMEM, Looking Glass driver", c["before"]),
                       ("0.5.0 · the VM's own memory, no driver", c["after"])):
        o.append(f'<rect x="{lx}" y="60" width="12" height="12" rx="3" fill="{col}"/>')
        o.append(f'<text x="{lx + 18}" y="70.5" font-family="{SANS}" font-size="12.5" fill="{c["soft"]}">{esc(label)}</text>')
        lx += 300

    o.append(f'<text x="{W - 6}" y="{top - 14}" text-anchor="end" font-family="{MONO}" font-size="11" '
             f'fill="{c["muted"]}">CHANGE</text>')

    # recessive grid and axis labels
    for v in (0, 5, 10, 15):
        gx = x(v)
        o.append(f'<line x1="{gx:.1f}" y1="{top - 8}" x2="{gx:.1f}" y2="{top + row * len(LATENCY) - 10}" '
                 f'stroke="{c["line"] if v == 0 else c["grid"]}" stroke-width="1"/>')
        o.append(f'<text x="{gx:.1f}" y="{top + row * len(LATENCY) + 8}" text-anchor="middle" '
                 f'font-family="{MONO}" font-size="11.5" fill="{c["muted"]}">{v} ms</text>')

    def hbar(y, v, col):
        # Square at the baseline, rounded at the data end (4 px).
        x0, x1, r = x(0), x(v), 4
        return (f'<path d="M{x0:.1f},{y} H{x1 - r:.1f} Q{x1:.1f},{y} {x1:.1f},{y + r} '
                f'V{y + bar - r} Q{x1:.1f},{y + bar} {x1 - r:.1f},{y + bar} H{x0:.1f} Z" fill="{col}"/>')

    for i, (name, before, after) in enumerate(LATENCY):
        y = top + i * row
        o.append(f'<text x="0" y="{y + bar + 2}" font-family="{SANS}" font-size="13.5" fill="{c["ink"]}">{esc(name)}</text>')
        o.append(hbar(y, before, c["before"]))
        o.append(hbar(y + bar + gap, after, c["after"]))
        # values in text ink, never the series colour
        o.append(f'<text x="{x(before) + 6:.1f}" y="{y + 11}" font-family="{MONO}" font-size="11.5" fill="{c["muted"]}">{before:.2f}</text>')
        o.append(f'<text x="{x(after) + 6:.1f}" y="{y + bar + gap + 11}" font-family="{MONO}" font-size="11.5" fill="{c["ink"]}">{after:.2f}</text>')
        pct = round((before - after) / before * 100)
        o.append(f'<text x="{W - 6}" y="{y + bar + 6}" text-anchor="end" font-family="{MONO}" font-size="13" '
                 f'font-weight="600" fill="{c["ink"]}">−{pct}%</text>')
    o.append('</svg>')
    return "\n".join(o) + "\n"


# --------------------------------------------------------- how it works

def box(o, c, x, y, w, h, title, sub=None, key=False):
    o.append(f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="6" fill="{c["raised"]}" '
             f'stroke="{c["key"] if key else c["line"]}" stroke-width="{1.6 if key else 1}"/>')
    ty = y + (h / 2 + 5 if not sub else h / 2 - 2)
    o.append(f'<text x="{x + w / 2}" y="{ty}" text-anchor="middle" font-family="{SANS}" font-size="13.5" '
             f'font-weight="600" fill="{c["ink"]}">{esc(title)}</text>')
    if sub:
        o.append(f'<text x="{x + w / 2}" y="{ty + 17}" text-anchor="middle" font-family="{MONO}" '
                 f'font-size="11" fill="{c["muted"]}">{esc(sub)}</text>')


def arrow(o, c, x1, y1, x2, y2, dashed=False):
    dash = 'stroke-dasharray="4 4" ' if dashed else ''
    o.append(f'<line x1="{x1}" y1="{y1}" x2="{x2}" y2="{y2}" stroke="{c["muted"]}" stroke-width="1.4" '
             f'{dash}marker-end="url(#ah)"/>')


def pipeline_svg(c):
    W, H = 764, 336
    bw, bh = 168, 54
    xs = [2, 199, 396, 594]
    ytop, ybot = 50, 236
    o = [f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {W} {H}" width="{W}" height="{H}" '
         f'role="img" aria-labelledby="t d">',
         '<title id="t">How a frame gets from a Windows app to your Linux desktop</title>',
         '<desc id="d">In Windows, the app window is captured, read back from the GPU in eight bands, and '
         'written into a ring of locked pages in the VM\'s own memory. The host reads those pages in place '
         'through QEMU\'s memory file, with no copy, no codec and no driver; vyprd checks every page, the '
         'GPU imports the ring without a copy, and the frame appears as a native window. Keys and mouse '
         'go back over a socket.</desc>',
         '<defs><marker id="ah" viewBox="0 0 8 8" refX="7" refY="4" markerWidth="7" markerHeight="7" '
         f'orient="auto"><path d="M0 1 L7 4 L0 7" fill="none" stroke="{c["muted"]}" stroke-width="1.3"/></marker></defs>']

    lab = lambda x, y, t: o.append(f'<text x="{x}" y="{y}" font-family="{MONO}" font-size="11.5" '
                                   f'letter-spacing="1.2" fill="{c["muted"]}">{esc(t)}</text>')
    lab(0, 30, "IN WINDOWS, INSIDE THE VM")
    box(o, c, xs[0], ytop, bw, bh, "The app's window", "any Windows app")
    box(o, c, xs[1], ytop, bw, bh, "Captured", "per window, by Windows")
    box(o, c, xs[2], ytop, bw, bh, "Read back from the GPU", "in 8 bands, overlapped")
    box(o, c, xs[3], ytop, bw, bh, "Locked pages", "in the VM's own RAM", key=True)
    for i in range(3):
        arrow(o, c, xs[i] + bw + 4, ytop + bh / 2, xs[i + 1] - 4, ytop + bh / 2)

    # the boundary, and the one crossing that matters
    yseam = 160
    o.append(f'<line x1="0" y1="{yseam}" x2="{W}" y2="{yseam}" stroke="{c["line"]}" stroke-width="1.2" stroke-dasharray="2 6"/>')
    o.append(f'<text x="{xs[1]}" y="{yseam - 10}" font-family="{SANS}" font-size="12.5" fill="{c["soft"]}">'
             'The VM boundary, crossed by reading the pages where they are</text>')
    o.append(f'<text x="{xs[1]}" y="{yseam + 22}" font-family="{SANS}" font-size="12.5" font-weight="600" fill="{c["ink"]}">'
             'no copy, no codec, no driver</text>')
    cx = xs[3] + bw / 2
    o.append(f'<line x1="{cx}" y1="{ytop + bh + 4}" x2="{cx}" y2="{ybot - 4}" stroke="{c["key"]}" '
             'stroke-width="1.6" marker-end="url(#ah)"/>')

    lab(xs[1], ybot - 14, "ON LINUX, YOUR DESKTOP")
    box(o, c, xs[3], ybot, bw, bh, "Checked page by page", "vyprd, read-only")
    box(o, c, xs[2], ybot, bw, bh, "Handed to the GPU", "zero-copy import")
    box(o, c, xs[1], ybot, bw, bh, "A native window", "own taskbar entry")
    for a, b in ((3, 2), (2, 1)):
        arrow(o, c, xs[a] - 4, ybot + bh / 2, xs[b] + bw + 4, ybot + bh / 2)

    # input goes back the other way
    ix = xs[0] + bw / 2
    o.append(f'<path d="M{xs[1] - 4},{ybot + bh / 2} H{ix} V{ytop + bh + 4}" fill="none" stroke="{c["muted"]}" '
             'stroke-width="1.4" stroke-dasharray="4 4" marker-end="url(#ah)"/>')
    o.append(f'<text x="{xs[0]}" y="{ybot + bh / 2 + 22}" font-family="{MONO}" font-size="11" fill="{c["muted"]}">keys and mouse go</text>')
    o.append(f'<text x="{xs[0]}" y="{ybot + bh / 2 + 37}" font-family="{MONO}" font-size="11" fill="{c["muted"]}">back over a socket</text>')
    o.append('</svg>')
    return "\n".join(o) + "\n"


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    for theme, c in THEMES.items():
        (OUT / f"latency-{theme}.svg").write_text(latency_svg(c))
        (OUT / f"how-it-works-{theme}.svg").write_text(pipeline_svg(c))
    print("wrote", ", ".join(sorted(p.name for p in OUT.glob("*.svg"))))


if __name__ == "__main__":
    main()
