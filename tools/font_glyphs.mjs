#!/usr/bin/env node
// Draw the UI's own glyphs into the UI font.
//
//     node tools/font_glyphs.mjs [source font]
//
// The Gea build rasterises a fixed set of codepoints into the panel's font
// atlas: printable ASCII plus a short list of punctuation, arrows and
// guillemets (embeddedFontExtraCodepoints in @geastack/core's
// generate-gea-embedded-fonts.mjs). Anything outside that set draws as `?`, so
// an icon has to replace the glyph at one of those codepoints. Add an entry to
// GLYPHS, run this, and use the character in the UI; the browser preview loads
// the same file, so it shows the same shape.
//
// Rewrites assets/fonts/Saira-Bold-Pedal.otf in place (opentype.js writes CFF
// outlines), so running it again is harmless. To start over from upstream, pass
// Saira Bold (a static weight-700 Saira.ttf instance from
// https://github.com/Omnibus-Type/Saira) as the source. Saira is OFL-1.1, which
// allows this; the result is renamed so it is not mistaken for the upstream font.
import fs from 'node:fs'
import path from 'node:path'
import { createRequire } from 'node:module'
import { fileURLToPath } from 'node:url'

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..')
// The same opentype.js the Gea font generator uses.
const opentype = createRequire(path.join(root, 'node_modules/@geastack/core/package.json'))(
  'opentype.js',
)

// Units are the font's (1000 per em). Saira's x-height is 510 and its cap
// height 688.
const CHEVRON = { left: 70, bottom: 60, width: 340, height: 580, stroke: 185, advance: 480 }

// A chevron as one closed outline, apex on the right: two strokes of equal
// horizontal thickness meeting at the middle.
function chevronRight({ left, bottom, width, height, stroke }) {
  const top = bottom + height
  const middle = bottom + height / 2
  return [
    [left, top],
    [left + stroke, top],
    [left + width, middle],
    [left + stroke, bottom],
    [left, bottom],
    [left + width - stroke, middle],
  ]
}

const mirror = (points, { left, width }) =>
  points.map(([x, y]) => [2 * left + width - x, y]).reverse()

const GLYPHS = [
  {
    codepoint: 0x2039,
    name: 'chevronleft',
    advance: CHEVRON.advance,
    contours: [mirror(chevronRight(CHEVRON), CHEVRON)],
  },
  {
    codepoint: 0x203a,
    name: 'chevronright',
    advance: CHEVRON.advance,
    contours: [chevronRight(CHEVRON)],
  },
]

function glyphPath(contours) {
  const outline = new opentype.Path()
  for (const points of contours) {
    points.forEach(([x, y], index) => (index === 0 ? outline.moveTo(x, y) : outline.lineTo(x, y)))
    // CFF closes a contour implicitly and the panel's rasteriser does not, so
    // the closing edge has to be drawn: without it half the glyph is lost.
    outline.lineTo(...points[0])
    outline.close()
  }
  return outline
}

const output = path.join(root, 'assets/fonts/Saira-Bold-Pedal.otf')
const source = process.argv[2] ? path.resolve(process.argv[2]) : output
const data = fs.readFileSync(source)
const font = opentype.parse(data.buffer.slice(data.byteOffset, data.byteOffset + data.byteLength))

for (const glyph of GLYPHS) {
  const existing = font.charToGlyph(String.fromCodePoint(glyph.codepoint))
  if (!existing || existing.index === 0)
    throw new Error(`U+${glyph.codepoint.toString(16)} is not in the font`)
  existing.name = glyph.name
  existing.advanceWidth = glyph.advance
  existing.path = glyphPath(glyph.contours)
  // Drop the TrueType outline so the new path is the one written.
  delete existing.points
  existing.leftSideBearing = Math.min(...glyph.contours.flat().map(([x]) => x))
}

// Keep only what the panel can draw: the build embeds the font file itself as
// well as the atlas, and opentype.js writes unsubroutinised CFF, so the full
// 897-glyph font costs 220 KB of flash. This mirrors the generator's list.
const PANEL_CODEPOINTS = new Set([
  ...Array.from({ length: 0x7f - 0x20 }, (_, i) => 0x20 + i),
  0x00a0,
  0x00a9,
  0x00ab,
  0x00b0,
  0x00b7,
  0x00bb,
  0x2010,
  0x2011,
  0x2013,
  0x2014,
  0x2018,
  0x2019,
  0x201c,
  0x201d,
  0x2022,
  0x2026,
  0x2212,
  0x2032,
  0x2033,
  0x2039,
  0x203a,
  0x20ac,
  0x2122,
  0x2190,
  0x2191,
  0x2192,
  0x2193,
  0x21bb,
])
const glyphs = [font.glyphs.get(0)]
for (let index = 1; index < font.glyphs.length; index += 1) {
  const glyph = font.glyphs.get(index)
  const unicodes = (glyph.unicodes ?? []).filter((codepoint) => PANEL_CODEPOINTS.has(codepoint))
  if (unicodes.length === 0) continue
  glyphs.push(
    new opentype.Glyph({
      name: glyph.name,
      unicode: unicodes[0],
      unicodes,
      advanceWidth: glyph.advanceWidth,
      path: glyph.path,
    }),
  )
}

// Upstream keeps its names in the Windows table only.
function nameOf(key) {
  for (const platform of ['windows', 'unicode', 'macintosh'])
    if (font.names[platform]?.[key]?.en) return font.names[platform][key].en
  return undefined
}

const subset = new opentype.Font({
  familyName: 'Saira Pedal',
  styleName: 'Bold',
  fullName: 'Saira Pedal Bold',
  postScriptName: 'SairaPedal-Bold',
  copyright: nameOf('copyright'),
  license: nameOf('license'),
  licenseURL: nameOf('licenseURL'),
  designer: nameOf('designer'),
  designerURL: nameOf('designerURL'),
  manufacturer: nameOf('manufacturer'),
  manufacturerURL: nameOf('manufacturerURL'),
  unitsPerEm: font.unitsPerEm,
  ascender: font.ascender,
  descender: font.descender,
  glyphs,
})
// The vertical metrics the browser lays text out with.
Object.assign(subset.tables, {
  os2: {
    sTypoAscender: font.tables.os2.sTypoAscender,
    sTypoDescender: font.tables.os2.sTypoDescender,
    sTypoLineGap: font.tables.os2.sTypoLineGap,
    usWinAscent: font.tables.os2.usWinAscent,
    usWinDescent: font.tables.os2.usWinDescent,
    fsSelection: font.tables.os2.fsSelection,
    usWeightClass: font.tables.os2.usWeightClass,
  },
  hhea: { lineGap: font.tables.hhea.lineGap },
})

fs.writeFileSync(output, Buffer.from(subset.toArrayBuffer()))
console.log(
  `wrote ${path.relative(root, output)}: ${glyphs.length} glyphs, ${GLYPHS.length} of them the UI's own`,
)
