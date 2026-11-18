// Generates docs/paper/aether_ieee.tex (IEEEtran conference format) from content.mjs.
import { writeFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { acknowledgment, body, licenseNote, meta, refs } from './content.mjs';
import { numberCitations, parseInline } from './markup.mjs';

const here = dirname(fileURLToPath(import.meta.url));
const outPath = join(here, '..', 'aether_ieee.tex');

const UNICODE = {
  '≤': '$\\le$', '≥': '$\\ge$', '×': '$\\times$', '·': '$\\cdot$', '−': '$-$', '°': '$^\\circ$',
  'Δ': '$\\Delta$', '→': '$\\rightarrow$', '≈': '$\\approx$', '±': '$\\pm$', '€': '\\texteuro{}',
  '–': '--', '—': '---', '‘': '`', '’': "'", '“': '``', '”': "''", '~': '$\\sim$',
};

function escText(s) {
  let out = s.replace(/[\\&%#_{}^]/g, (c) => ({
    '\\': '\\textbackslash{}', '&': '\\&', '%': '\\%', '#': '\\#', '_': '\\_', '{': '\\{', '}': '\\}',
    '^': '\\^{}',
  })[c]);
  out = out.replace(/[≤≥×·−°Δ→≈±€–—‘’“”~]/g, (c) => UNICODE[c]);
  // Straight double quotes -> TeX quotes, alternating open/close.
  let open = true;
  out = out.replace(/"/g, () => { const q = open ? '``' : "''"; open = !open; return q; });
  return out;
}

function escCode(s) {
  return escText(s).replace(/\\_/g, '\\_\\allowbreak{}').replace(/::/g, '::\\allowbreak{}');
}

function urlify(s) {
  // Wrap URLs in \url{} (url package) and escape the rest.
  const parts = s.split(/(https?:\/\/\S+)/);
  return parts.map((p, idx) => (idx % 2 ? `\\url{${p}}` : null)).map((u, idx) => u ?? inline(parts[idx])).join('');
}

let citeOrder = [];
function inline(src) {
  return parseInline(src).map((tok) => {
    switch (tok.t) {
      case 'text': return escText(tok.v);
      case 'math': return `$${tok.v}$`;
      case 'code': return `\\texttt{${escCode(tok.v)}}`;
      case 'bold': return `\\textbf{${escText(tok.v)}}`;
      case 'italic': return `\\textit{${escText(tok.v)}}`;
      case 'cite': return `~\\cite{${tok.v.join(',')}}`;
      default: throw new Error(tok.t);
    }
  }).join('').replace(/ ~\\cite/g, '~\\cite');
}

function table(t) {
  const spec = t.widths.map((w) => `p{${(w * 0.94).toFixed(3)}\\columnwidth}`).join('');
  const env = t.wide ? 'table*' : 'table';
  return [
    `\\begin{${env}}[!t]`,
    '\\renewcommand{\\arraystretch}{1.15}',
    `\\caption{${escText(t.caption)}}`,
    `\\label{${t.label}}`,
    '\\centering\\footnotesize',
    `\\begin{tabular}{${spec}}`,
    '\\hline',
    `${t.cols.map((c) => `\\textbf{${escText(c)}}`).join(' & ')} \\\\`,
    '\\hline',
    ...t.rows.map((r) => `${r.map(escText).join(' & ')} \\\\`),
    '\\hline',
    '\\end{tabular}',
    `\\end{${env}}`,
  ].join('\n');
}

function figure(f) {
  const env = f.wide ? 'figure*' : 'figure';
  const width = f.wide ? '\\textwidth' : '\\columnwidth';
  return [
    `\\begin{${env}}[!t]`,
    '\\centering',
    `\\includegraphics[width=${width}]{figures/${f.file}}`,
    `\\caption{${inline(f.caption)}}`,
    `\\label{${f.label}}`,
    `\\end{${env}}`,
  ].join('\n');
}

citeOrder = numberCitations(body, refs);

const lines = [];
lines.push(String.raw`% AETHER-2.5D Studio — IEEE-style paper (IEEEtran). Generated from docs/paper/src/content.mjs.
% Compile: pdflatex aether_ieee.tex (twice), or upload docs/paper/ to Overleaf.
% Copyright (c) 2026 Rachel (AiChan277). Licensed under the AETHER-2.5D Studio Source-Available,
% No-Derivatives, No-AI License v1.0 (see LICENSE). Not for AI training or reproduction.
\documentclass[conference]{IEEEtran}
\IEEEoverridecommandlockouts
\usepackage[utf8]{inputenc}
\usepackage[T1]{fontenc}
\usepackage{textcomp}
\usepackage{amsmath,amssymb,amsfonts}
\usepackage{graphicx}
\usepackage{cite}
\usepackage{url}
\usepackage{xcolor}
\def\UrlBreaks{\do\/\do-\do.\do_}
\def\BibTeX{{\rm B\kern-.05em{\sc i\kern-.025em b}\kern-.08em T\kern-.1667em\lower.7ex\hbox{E}\kern-.125emX}}

\begin{document}
`);
lines.push(`\\title{${escText(meta.title)}}`);
lines.push(String.raw`\author{\IEEEauthorblockN{${meta.author}}
\IEEEauthorblockA{\textit{${meta.affiliation}}\\
Email: \url{${meta.email}}}}`.replace('${meta.author}', escText(meta.author)).replace('${meta.affiliation}', escText(meta.affiliation)).replace('${meta.email}', meta.email));
lines.push('\\maketitle\n');
lines.push(`\\begin{abstract}\n${inline(meta.abstract)}\n\\end{abstract}\n`);
lines.push(`\\begin{IEEEkeywords}\n${escText(meta.keywords)}\n\\end{IEEEkeywords}\n`);

for (const b of body) {
  if (b.h1) lines.push(`\n\\section{${escText(b.h1)}}`);
  else if (b.h2) lines.push(`\n\\subsection{${escText(b.h2)}}`);
  else if (b.p) lines.push(`${inline(b.p)}\n`);
  else if (b.list) lines.push(`\\begin{itemize}\n${b.list.map((x) => `\\item ${inline(x)}`).join('\n')}\n\\end{itemize}\n`);
  else if (b.eq) lines.push(`\\begin{equation}\n${b.eq}\n\\label{${b.label}}\n\\end{equation}`);
  else if (b.table) lines.push(table(b.table) + '\n');
  else if (b.fig) lines.push(figure(b.fig) + '\n');
}

lines.push(`\n\\section*{Availability and License}\n${inline(licenseNote)}\n`);
lines.push(`\\section*{Acknowledgment}\n${inline(acknowledgment)}\n`);
lines.push(`\\begin{thebibliography}{${citeOrder.length}}`);
for (const k of citeOrder) lines.push(`\\bibitem{${k}} ${urlify(refs[k])}`);
lines.push('\\end{thebibliography}\n\n\\end{document}\n');

writeFileSync(outPath, lines.join('\n'), 'utf8');
console.log(`wrote ${outPath} (${citeOrder.length} references)`);
