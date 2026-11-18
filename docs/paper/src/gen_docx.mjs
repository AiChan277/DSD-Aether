// Generates docs/paper/AETHER-2.5D_IEEE_Paper.docx (IEEE two-column layout) from content.mjs.
// Requires the `docx` npm package (set NODE_PATH to a folder containing node_modules/docx).
import { createRequire } from 'node:module';
import { readFileSync, writeFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { acknowledgment, body, licenseNote, meta, refs } from './content.mjs';
import { numberCitations, parseInline, texToRuns, toRoman } from './markup.mjs';

const require = createRequire(join(process.env.NODE_PATH ?? '.', 'x.js'));
const {
  AlignmentType, BorderStyle, Document, Footer, ImageRun, LevelFormat, Packer, PageNumber, Paragraph,
  SectionType, Table, TableCell, TableRow, TabStopType, TextRun, WidthType,
} = require('docx');

const here = dirname(fileURLToPath(import.meta.url));
const paperDir = join(here, '..');
const FONT = 'Times New Roman';
const PAGE_W = 12240; const MARGIN_LR = 900; const COL_GAP = 360;
const COL_W = (PAGE_W - 2 * MARGIN_LR - COL_GAP) / 2; // 5040 DXA = 3.5 in

const citeOrder = numberCitations(body, refs);
const citeNum = Object.fromEntries(citeOrder.map((k, i) => [k, i + 1]));

function citeLabel(keys) {
  const nums = keys.map((k) => citeNum[k]).sort((a, b) => a - b);
  return `[${nums.join(', ')}]`;
}

/** Markup -> TextRun[]; base = default run options (size, bold, italics). */
function runs(src, base = {}) {
  const out = [];
  for (const tok of parseInline(src)) {
    if (tok.t === 'text') out.push(new TextRun({ text: tok.v, ...base }));
    else if (tok.t === 'bold') out.push(new TextRun({ text: tok.v, ...base, bold: true }));
    else if (tok.t === 'italic') out.push(new TextRun({ text: tok.v, ...base, italics: !base.italics }));
    else if (tok.t === 'code') out.push(new TextRun({ text: tok.v, ...base, font: 'Courier New', size: (base.size ?? 20) - 2 }));
    else if (tok.t === 'cite') out.push(new TextRun({ text: citeLabel(tok.v), ...base }));
    else if (tok.t === 'math') {
      for (const r of texToRuns(tok.v)) {
        out.push(new TextRun({
          text: r.text, ...base, italics: r.italic || base.italics, bold: r.bold || base.bold,
          subScript: r.sub, superScript: r.sup,
        }));
      }
    }
  }
  return out;
}

const P = (children, opts = {}) => new Paragraph({ children, ...opts });
const body10 = { font: FONT, size: 20 };

let h1Count = 0; let h2Count = 0; let eqCount = 0; let tabCount = 0; let figCount = 0;
const content = [];

// Abstract and index terms (first column, IEEE style).
content.push(P([new TextRun({ text: 'Abstract', bold: true, italics: true, size: 18, font: FONT }),
  new TextRun({ text: '—', bold: true, size: 18, font: FONT }), ...runs(meta.abstract, { bold: true, size: 18, font: FONT })],
{ alignment: AlignmentType.JUSTIFIED, spacing: { after: 120 } }));
content.push(P([new TextRun({ text: 'Index Terms', bold: true, italics: true, size: 18, font: FONT }),
  new TextRun({ text: `—${meta.keywords}.`, bold: true, size: 18, font: FONT })],
{ alignment: AlignmentType.JUSTIFIED, spacing: { after: 160 } }));

function heading1(text) {
  h1Count += 1; h2Count = 0;
  return P([new TextRun({ text: `${toRoman(h1Count)}. ${text}`, font: FONT, size: 20, smallCaps: true })],
    { alignment: AlignmentType.CENTER, spacing: { before: 200, after: 100 }, keepNext: true });
}
function heading1Unnumbered(text) {
  return P([new TextRun({ text, font: FONT, size: 20, smallCaps: true })],
    { alignment: AlignmentType.CENTER, spacing: { before: 200, after: 100 }, keepNext: true });
}
function heading2(text) {
  h2Count += 1;
  return P([new TextRun({ text: `${String.fromCharCode(64 + h2Count)}. ${text}`, font: FONT, size: 20, italics: true })],
    { spacing: { before: 120, after: 60 }, keepNext: true });
}
function para(src) {
  return P(runs(src, body10), { alignment: AlignmentType.JUSTIFIED, indent: { firstLine: 245 }, spacing: { after: 0 } });
}

// Display equation: `text` uses TeX-style _{sub} / ^{sup} with Unicode symbols; ' ;; ' splits lines.
function equation(b) {
  eqCount += 1;
  const lines = (b.text ?? b.eq).split(' ;; ');
  return lines.map((line, li) => {
    const children = [new TextRun({ text: '\t', ...body10 })];
    texToRuns(line).forEach((r) => children.push(new TextRun({
      text: r.text, ...body10, italics: r.italic, bold: r.bold, subScript: r.sub, superScript: r.sup,
    })));
    if (li === lines.length - 1) children.push(new TextRun({ text: `\t(${eqCount})`, ...body10 }));
    return P(children, {
      tabStops: [{ type: TabStopType.CENTER, position: COL_W / 2 }, { type: TabStopType.RIGHT, position: COL_W }],
      spacing: { before: li === 0 ? 80 : 0, after: li === lines.length - 1 ? 80 : 0 },
      keepNext: li < lines.length - 1,
    });
  });
}

const thin = { style: BorderStyle.SINGLE, size: 4, color: '000000' };
const none = { style: BorderStyle.NONE, size: 0, color: 'FFFFFF' };
function table(t) {
  tabCount += 1;
  const widths = t.widths.map((w) => Math.round(w * COL_W));
  widths[widths.length - 1] = COL_W - widths.slice(0, -1).reduce((a, b) => a + b, 0);
  const cell = (text, i, header, lastRow) => new TableCell({
    width: { size: widths[i], type: WidthType.DXA },
    margins: { top: 30, bottom: 30, left: 60, right: 60 },
    borders: { top: header ? thin : none, bottom: header || lastRow ? thin : none, left: none, right: none },
    children: [P([new TextRun({ text, font: FONT, size: 16, bold: header })])],
  });
  const rows = [new TableRow({ tableHeader: true, children: t.cols.map((c, i) => cell(c, i, true, false)) }),
    ...t.rows.map((r, ri) => new TableRow({ cantSplit: true, children: r.map((c, i) => cell(c, i, false, ri === t.rows.length - 1)) }))];
  return [
    P([new TextRun({ text: `TABLE ${toRoman(tabCount)}`, font: FONT, size: 16 })], { alignment: AlignmentType.CENTER, spacing: { before: 160 }, keepNext: true }),
    P([new TextRun({ text: t.caption, font: FONT, size: 16, smallCaps: true })], { alignment: AlignmentType.CENTER, spacing: { after: 60 }, keepNext: true }),
    new Table({ width: { size: COL_W, type: WidthType.DXA }, columnWidths: widths, rows }),
    P([], { spacing: { after: 120 } }),
  ];
}

function pngSize(buf) { return { w: buf.readUInt32BE(16), h: buf.readUInt32BE(20) }; }
function figure(f) {
  figCount += 1;
  const data = readFileSync(join(paperDir, 'figures', f.file));
  const { w, h } = pngSize(data);
  const widthPx = 336; // 3.5 in at 96 dpi = column width
  return [
    P([new ImageRun({ type: 'png', data, transformation: { width: widthPx, height: Math.round((widthPx * h) / w) } })],
      { alignment: AlignmentType.CENTER, spacing: { before: 120 }, keepNext: true }),
    P([new TextRun({ text: `Fig. ${figCount}.  `, font: FONT, size: 16 }), ...runs(f.caption, { font: FONT, size: 16 })],
      { alignment: AlignmentType.JUSTIFIED, spacing: { before: 60, after: 160 } }),
  ];
}

for (const b of body) {
  if (b.h1) content.push(heading1(b.h1));
  else if (b.h2) content.push(heading2(b.h2));
  else if (b.p) content.push(para(b.p));
  else if (b.list) b.list.forEach((x) => content.push(P(runs(x, body10), { numbering: { reference: 'bullets', level: 0 }, alignment: AlignmentType.JUSTIFIED })));
  else if (b.eq) content.push(...equation(b));
  else if (b.table) content.push(...table(b.table));
  else if (b.fig) content.push(...figure(b.fig));
}

content.push(heading1Unnumbered('Availability and License'));
content.push(para(licenseNote));
content.push(heading1Unnumbered('Acknowledgment'));
content.push(para(acknowledgment));
content.push(heading1Unnumbered('References'));
citeOrder.forEach((k, i) => {
  content.push(P([new TextRun({ text: `[${i + 1}]\t`, font: FONT, size: 16 }), ...runs(refs[k], { font: FONT, size: 16 })], {
    tabStops: [{ type: TabStopType.LEFT, position: 360 }],
    indent: { left: 360, hanging: 360 }, alignment: AlignmentType.JUSTIFIED, spacing: { after: 30 },
  }));
});

const titleBlock = [
  P([new TextRun({ text: meta.title, font: FONT, size: 44 })], { alignment: AlignmentType.CENTER, spacing: { after: 200 } }),
  P([new TextRun({ text: meta.author, font: FONT, size: 22 })], { alignment: AlignmentType.CENTER }),
  P([new TextRun({ text: meta.affiliation, font: FONT, size: 20, italics: true })], { alignment: AlignmentType.CENTER }),
  P([new TextRun({ text: `Email: ${meta.email}`, font: FONT, size: 20 })], { alignment: AlignmentType.CENTER, spacing: { after: 280 } }),
];

const page = { size: { width: PAGE_W, height: 15840 }, margin: { top: 1080, bottom: 1440, left: MARGIN_LR, right: MARGIN_LR } };
const footer = new Footer({ children: [P([new TextRun({ children: [PageNumber.CURRENT], font: FONT, size: 16 })], { alignment: AlignmentType.CENTER })] });

const doc = new Document({
  creator: meta.author,
  title: meta.title,
  description: 'IEEE-style paper describing AETHER-2.5D Studio. Not for AI training or reproduction (see LICENSE).',
  styles: { default: { document: { run: { font: FONT, size: 20 } } } },
  numbering: { config: [{ reference: 'bullets', levels: [{ level: 0, format: LevelFormat.BULLET, text: '•', alignment: AlignmentType.LEFT, style: { paragraph: { indent: { left: 360, hanging: 200 } } } }] }] },
  sections: [
    { properties: { page }, footers: { default: footer }, children: titleBlock },
    { properties: { page, type: SectionType.CONTINUOUS, column: { count: 2, space: COL_GAP, equalWidth: true } }, footers: { default: footer }, children: content },
  ],
});

const out = join(paperDir, 'AETHER-2.5D_IEEE_Paper.docx');
writeFileSync(out, await Packer.toBuffer(doc));
console.log(`wrote ${out} (${citeOrder.length} refs, ${figCount} figs, ${tabCount} tables, ${eqCount} eqs)`);
