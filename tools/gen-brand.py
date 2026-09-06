#!/usr/bin/env python3
"""Generate the Vypr brand SVGs from the traced geometry."""
import os, sys

# ---- the mark: two ribbons on +/-0.61 slopes, 56 wide, 27 apart ----
MARK_W, MARK_H = 247, 202
LEFT_ARM  = "M0 0 H56 L151 155 L123.5 202 Z"
RIGHT_ARM = "M191 0 H247 L165 135 L137 88 Z"

# ---- the wordmark: cap height 69, letters drawn as paths ----
WORD_W, WORD_H = 420, 69
LETTERS = [
    (0,   "M0 0 H17 L43 49 L69 0 H86 L49 69 H37 Z"),                                    # V
    (125, "M0 0 H18 L39 28 L61 0 H79 L47 39 V69 H31 V39 Z"),                            # Y
    (246, "M0 0 H44 A22 21 0 0 1 44 42 H15 V69 H0 Z M15 15 H41 A8 8 0 0 1 41 31 H15 Z"),# P
    (353, "M0 0 H44 A22 21 0 0 1 44 42 L68 69 H51 L33 44 H15 V69 H0 Z "
          "M15 15 H41 A8 8 0 0 1 41 31 H15 Z"),                                          # R
]

# Lockup: mark centred over the wordmark, 58 of air between them.
MARK_X, GAP = 87, 58
LOCK_W = WORD_W
LOCK_H = MARK_H + GAP + WORD_H

def wordmark_group(dx=0, dy=0):
    out = [f'  <g transform="translate({dx},{dy})">']
    for x, d in LETTERS:
        out.append(f'    <path transform="translate({x},0)" d="{d}"/>')
    out.append("  </g>")
    return "\n".join(out)

def mark_group(dx=0, dy=0):
    return (f'  <g transform="translate({dx},{dy})">\n'
            f'    <path d="{LEFT_ARM}"/>\n'
            f'    <path d="{RIGHT_ARM}"/>\n'
            f'  </g>')

HEAD = ('<svg xmlns="http://www.w3.org/2000/svg" viewBox="{vb}" width="{w}" height="{h}"'
        ' role="img" aria-label="{label}">\n'
        '  <title>{label}</title>\n'
        '<g fill="{fill}" fill-rule="evenodd">\n')

def write(path, vb, w, h, body, fill, label):
    with open(path, "w") as f:
        f.write(HEAD.format(vb=vb, w=w, h=h, fill=fill, label=label))
        f.write(body + "\n</g>\n</svg>\n")
    print("wrote", path)

def build(outdir):
    os.makedirs(outdir, exist_ok=True)
    for name, fill in (("white", "#ffffff"), ("black", "#000000")):
        write(f"{outdir}/vypr-mark-{name}.svg", f"0 0 {MARK_W} {MARK_H}",
              MARK_W, MARK_H, mark_group(), fill, "Vypr")
        write(f"{outdir}/vypr-lockup-{name}.svg", f"0 0 {LOCK_W} {LOCK_H}",
              LOCK_W, LOCK_H,
              mark_group(MARK_X, 0) + "\n" + wordmark_group(0, MARK_H + GAP),
              fill, "Vypr")
    # Square icon: the mark centred in a 512 box with even air around it.
    pad = 74
    side = 512
    inner_w = side - 2 * pad
    scale = inner_w / MARK_W
    ox = pad
    oy = (side - MARK_H * scale) / 2
    body = (f'  <g transform="translate({ox:.2f},{oy:.2f}) scale({scale:.5f})">\n'
            f'    <path d="{LEFT_ARM}"/>\n    <path d="{RIGHT_ARM}"/>\n  </g>')
    for name, fill in (("white", "#ffffff"), ("black", "#000000")):
        write(f"{outdir}/vypr-icon-{name}.svg", f"0 0 {side} {side}",
              side, side, body, fill, "Vypr")

if __name__ == "__main__":
    build(sys.argv[1] if len(sys.argv) > 1 else "brand")
