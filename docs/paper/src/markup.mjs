// Inline markup parser shared by the LaTeX and Word generators.
//   $tex$  math    {{code}}  monospace    **bold**    *italic*    [@a,b]  citation

const INLINE = /\$([^$]+)\$|\{\{(.+?)\}\}|\*\*([^*]+)\*\*|\*([^*]+)\*|\[@([^\]]+)\]/g;

export function parseInline(src) {
  const out = [];
  let last = 0;
  for (const m of src.matchAll(INLINE)) {
    if (m.index > last) out.push({ t: 'text', v: src.slice(last, m.index) });
    if (m[1] !== undefined) out.push({ t: 'math', v: m[1] });
    else if (m[2] !== undefined) out.push({ t: 'code', v: m[2] });
    else if (m[3] !== undefined) out.push({ t: 'bold', v: m[3] });
    else if (m[4] !== undefined) out.push({ t: 'italic', v: m[4] });
    else out.push({ t: 'cite', v: m[5].split(/[;,]/).map((s) => s.trim().replace(/^@/, '')) });
    last = m.index + m[0].length;
  }
  if (last < src.length) out.push({ t: 'text', v: src.slice(last) });
  return out;
}

/** Assigns IEEE reference numbers in order of first citation. */
export function numberCitations(blocks, refs) {
  const order = [];
  const visit = (s) => {
    for (const tok of parseInline(s)) {
      if (tok.t !== 'cite') continue;
      for (const k of tok.v) {
        if (!(k in refs)) throw new Error(`Unknown reference key: ${k}`);
        if (!order.includes(k)) order.push(k);
      }
    }
  };
  for (const b of blocks) {
    if (b.p) visit(b.p);
    if (b.list) b.list.forEach(visit);
    if (b.fig) visit(b.fig.caption);
  }
  const unused = Object.keys(refs).filter((k) => !order.includes(k));
  if (unused.length) throw new Error(`Uncited references: ${unused.join(', ')}`);
  return order;
}

export function toRoman(n) {
  const map = [[10, 'X'], [9, 'IX'], [5, 'V'], [4, 'IV'], [1, 'I']];
  let s = '';
  for (const [v, r] of map) while (n >= v) { s += r; n -= v; }
  return s;
}

// ---------------------------------------------------------------------------------------------
// Minimal TeX math -> styled text runs (for Word). Supports the subset used in content.mjs.
// Returns [{ text, sub?, sup?, italic?, bold? }].

const SYMBOLS = {
  cdot: '·', le: '≤', ge: '≥', times: '×', in: '∈', odot: '⊙', infty: '∞', circ: '°', pm: '±',
  alpha: 'α', beta: 'β', gamma: 'γ', delta: 'δ', epsilon: 'ε', theta: 'θ', phi: 'φ', pi: 'π',
  sigma: 'σ', tau: 'τ', ell: 'ℓ', Lambda: 'Λ', Delta: 'Δ', lVert: '‖', rVert: '‖', lvert: '|',
  rvert: '|', quad: '  ', qquad: '    ', ',': ' ', ';': ' ', ' ': ' ', '!': '', left: '', right: '',
};
const FUNCS = new Set(['tanh', 'max', 'min', 'sech', 'sgn', 'exp', 'log']);
const ACCENTS = { hat: '̂', dot: '̇', overline: '̅', bar: '̄' };

export function texToRuns(tex) {
  let i = 0;
  const runs = [];
  const push = (text, style = {}) => {
    if (!text) return;
    // Italicize single Latin letters (math variables); keep digits/operators upright.
    if (style.upright || style.bold) { runs.push({ text, ...style }); return; }
    for (const part of text.match(/[A-Za-z]+|[^A-Za-z]+/g) || []) {
      runs.push({ text: part, ...style, italic: /^[A-Za-z]+$/.test(part) });
    }
  };
  const readGroup = () => {
    // Returns raw TeX of the next argument ({...} or single token).
    while (tex[i] === ' ') i++;
    if (tex[i] === '{') {
      let depth = 0; const start = i + 1;
      for (; i < tex.length; i++) {
        if (tex[i] === '{') depth++;
        else if (tex[i] === '}') { depth--; if (depth === 0) { i++; return tex.slice(start, i - 1); } }
      }
      return tex.slice(start);
    }
    if (tex[i] === '\\') {
      const m = /^\\([A-Za-z]+|.)/.exec(tex.slice(i)); i += m[0].length; return m[0];
    }
    return tex[i++] ?? '';
  };
  const SUP = { 0: '⁰', 1: '¹', 2: '²', 3: '³', 4: '⁴', 5: '⁵', 6: '⁶', 7: '⁷', 8: '⁸', 9: '⁹', '−': '⁻', '-': '⁻', '+': '⁺' };
  const flat = (r) => {
    if (r.sup) return [...r.text].every((c) => c in SUP) ? [...r.text].map((c) => SUP[c]).join('') : `^${r.text}`;
    if (r.sub) return `_${r.text}`;
    return r.text;
  };
  const plain = (s) => texToRuns(s).map(flat).join('');
  while (i < tex.length) {
    const ch = tex[i];
    if (ch === '\\') {
      const m = /^\\([A-Za-z]+|.)/.exec(tex.slice(i));
      i += m[0].length;
      const cmd = m[1];
      if (cmd in SYMBOLS) push(SYMBOLS[cmd], { upright: true });
      else if (FUNCS.has(cmd)) push(cmd, { upright: true });
      else if (cmd === 'operatorname' || cmd === 'mathrm' || cmd === 'text') push(plain(readGroup()), { upright: true });
      else if (cmd === 'mathbf') push(plain(readGroup()), { bold: true });
      else if (cmd === 'frac') {
        const a = plain(readGroup()); const b = plain(readGroup());
        const wrap = (s) => (/^[\w.]+$/.test(s) ? s : `(${s})`);
        push(`${wrap(a)}/${wrap(b)}`);
      } else if (cmd in ACCENTS) {
        const inner = plain(readGroup());
        push([...inner].map((c) => c + ACCENTS[cmd]).join(''));
      } else push(cmd, { upright: true });
    } else if (ch === '^' || ch === '_') {
      i++;
      const g = readGroup();
      const text = g === '\\circ' ? '°' : plain(g);
      if (text === '°') push('°', { upright: true });
      else runs.push({ text, [ch === '^' ? 'sup' : 'sub']: true, italic: false });
    } else if (ch === '{' || ch === '}') {
      i++;
    } else {
      let j = i;
      while (j < tex.length && !'\\^_{}'.includes(tex[j])) j++;
      push(tex.slice(i, j).replace(/'/g, '′').replace(/-/g, '−'));
      i = j;
    }
  }
  return runs;
}
