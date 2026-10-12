#!/bin/bash
# Generates the Material You GRUB theme assets (needs ImageMagick `convert` and `grub-mkfont`).
# Output goes to ./out (theme.txt + png + pf2). Palette follows design/mockups (dark teal + light blue sun).
set -euo pipefail
cd "$(dirname "$0")"
OUT=${1:-out}; rm -rf "$OUT"; mkdir -p "$OUT"

W=1920; H=1080
BG=#07140f; SURF=#0f2a24; PRIM=#7fd6b8; SUN=#7cc4f0

# --- background: night teal, soft sun with halo rings, layered hills -----------------------
convert -size ${W}x${H} gradient:'#0b1f1a-#06110d' "$OUT/bg0.png"
convert "$OUT/bg0.png" \
  -fill none -stroke '#1b4a40' -strokewidth 2 -draw "circle 1500,330 1500,60" \
  -stroke '#16392f' -strokewidth 2 -draw "circle 1500,330 1500,-60" \
  -stroke none -fill "$SUN" -draw "circle 1500,330 1500,150" \
  -fill '#0d3a31' -draw "path 'M 0,860 C 300,780 520,900 820,850 C 1120,800 1400,900 1920,820 L 1920,1080 L 0,1080 Z'" \
  -fill '#0a2a24' -draw "path 'M 0,940 C 360,880 700,980 1040,930 C 1380,880 1640,980 1920,920 L 1920,1080 L 0,1080 Z'" \
  -fill '#07201b' -draw "path 'M 0,1010 C 420,970 800,1050 1180,1010 C 1500,975 1720,1050 1920,1000 L 1920,1080 L 0,1080 Z'" \
  -depth 8 "$OUT/background.png"
rm -f "$OUT/bg0.png"

# --- selected item: 9-slice rounded pill (radius 28) ---------------------------------------
R=28; PW=$((R*2+4)); PH=$((R*2))
convert -size ${PW}x${PH} xc:none -fill '#1e5a4c' -draw "roundrectangle 0,0 $((PW-1)),$((PH-1)) $R,$R" "$OUT/pill.png"
convert "$OUT/pill.png" -crop ${R}x${R}+0+0 +repage "$OUT/select_nw.png"
convert "$OUT/pill.png" -crop ${R}x${R}+$((PW-R))+0 +repage "$OUT/select_ne.png"
convert "$OUT/pill.png" -crop ${R}x${R}+0+$((PH-R)) +repage "$OUT/select_sw.png"
convert "$OUT/pill.png" -crop ${R}x${R}+$((PW-R))+$((PH-R)) +repage "$OUT/select_se.png"
convert "$OUT/pill.png" -crop 4x${R}+$R+0 +repage "$OUT/select_n.png"
convert "$OUT/pill.png" -crop 4x${R}+$R+$((PH-R)) +repage "$OUT/select_s.png"
convert "$OUT/pill.png" -crop ${R}x4+0+$R +repage "$OUT/select_w.png"
convert "$OUT/pill.png" -crop ${R}x4+$((PW-R))+$R +repage "$OUT/select_e.png"
convert "$OUT/pill.png" -crop 4x4+$R+$R +repage "$OUT/select_c.png"
rm -f "$OUT/pill.png"

# --- menu panel (rounded surface container) --------------------------------------------------
PR=36; QW=$((PR*2+4)); QH=$((PR*2))
convert -size ${QW}x${QH} xc:none -fill '#0f2a24' -draw "roundrectangle 0,0 $((QW-1)),$((QH-1)) $PR,$PR" "$OUT/panel.png"
convert "$OUT/panel.png" -crop ${PR}x${PR}+0+0 +repage "$OUT/menu_nw.png"
convert "$OUT/panel.png" -crop ${PR}x${PR}+$((QW-PR))+0 +repage "$OUT/menu_ne.png"
convert "$OUT/panel.png" -crop ${PR}x${PR}+0+$((QH-PR)) +repage "$OUT/menu_sw.png"
convert "$OUT/panel.png" -crop ${PR}x${PR}+$((QW-PR))+$((QH-PR)) +repage "$OUT/menu_se.png"
convert "$OUT/panel.png" -crop 4x${PR}+$PR+0 +repage "$OUT/menu_n.png"
convert "$OUT/panel.png" -crop 4x${PR}+$PR+$((QH-PR)) +repage "$OUT/menu_s.png"
convert "$OUT/panel.png" -crop ${PR}x4+0+$PR +repage "$OUT/menu_w.png"
convert "$OUT/panel.png" -crop ${PR}x4+$((QW-PR))+$PR +repage "$OUT/menu_e.png"
convert "$OUT/panel.png" -crop 4x4+$PR+$PR +repage "$OUT/menu_c.png"
rm -f "$OUT/panel.png"

# GRUB wants plain 8-bit RGBA PNGs for the slices
for f in "$OUT"/select_*.png "$OUT"/menu_*.png; do convert "$f" -depth 8 PNG32:"$f"; done

# --- fonts (Noto Sans: Latin + Cyrillic) ----------------------------------------------------
F=${FONT_DIR:-/usr/share/fonts/truetype/noto}
for spec in "Medium:20" "Medium:26" "Medium:30" "Bold:76"; do
  st=${spec%%:*}; sz=${spec##*:}
  grub-mkfont -s "$sz" -o "$OUT/noto_${st,,}_${sz}.pf2" "$F/NotoSans-$st.ttf" >/dev/null 2>&1
done

# --- theme.txt -----------------------------------------------------------------------------
cp theme.txt "$OUT/theme.txt"
echo "theme generated in $OUT: $(ls "$OUT" | wc -l) files"
