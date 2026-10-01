import { unzipSync, zipSync } from 'fflate';
import initSqlJs from 'sql.js';
import sqlWasmUrl from 'sql.js/dist/sql-wasm.wasm?url';
import { ZSTDDecoder } from 'zstddec/stream';
import { selectCollectionMember } from './collection-member.js';

const encoder = new TextEncoder();
const decoder = new TextDecoder('utf-8', { fatal: true });
const LIMIT = 128 * 1024 * 1024;
const MAX_CARDS = 100000;
let sqlPromise;
let zstdPromise;

function fail(message) { throw new Error(message); }
function sql() { return sqlPromise ||= initSqlJs({ locateFile: () => sqlWasmUrl }); }
async function zstd() {
  if (!zstdPromise) zstdPromise = new ZSTDDecoder().init().then(() => new ZSTDDecoder());
  return zstdPromise;
}
function proto(bytes) {
  const fields = [];
  let pos = 0;
  const varint = () => {
    let value = 0n, shift = 0n;
    for (let i = 0; i < 10 && pos < bytes.length; i++) {
      const b = bytes[pos++];
      value |= BigInt(b & 127) << shift;
      if (!(b & 128)) return Number(value);
      shift += 7n;
    }
    fail('Metadados protobuf incompletos.');
  };
  while (pos < bytes.length) {
    const tag = varint(), field = Math.floor(tag / 8), wire = tag % 8;
    if (!field) fail('Metadados protobuf inválidos.');
    let value;
    if (wire === 0) value = varint();
    else if (wire === 2) {
      const length = varint();
      if (!Number.isSafeInteger(length) || length > bytes.length - pos) fail('Metadados protobuf truncados.');
      value = bytes.subarray(pos, pos + length); pos += length;
    } else if (wire === 1 || wire === 5) {
      const length = wire === 1 ? 8 : 4;
      if (length > bytes.length - pos) fail('Metadados protobuf truncados.');
      pos += length; continue;
    } else fail('Tipo protobuf não suportado.');
    fields.push({ field, wire, value });
  }
  return fields;
}
function rows(db, statement) {
  const stmt = db.prepare(statement), result = [];
  try { while (stmt.step()) result.push(stmt.getAsObject()); }
  finally { stmt.free(); }
  return result;
}
function columns(db, name) { return new Set(rows(db, `PRAGMA table_info("${name}")`).map(row => row.name)); }
function metadata(db) {
  const col = rows(db, 'SELECT models, decks FROM col LIMIT 1')[0];
  if (!col) fail('Coleção sem metadados.');
  if (col.models && col.decks) {
    const models = JSON.parse(col.models), decks = JSON.parse(col.decks);
    return { models: Object.values(models), decks: Object.values(decks).map(d => ({ id: d.id, name: d.name, dyn: d.dyn })) };
  }
  const fields = rows(db, 'SELECT ntid, name FROM fields ORDER BY ntid, ord');
  const templates = rows(db, 'SELECT ntid, ord, name, config FROM templates ORDER BY ntid, ord');
  const models = rows(db, 'SELECT id, name, config FROM notetypes').map(model => {
    const config = proto(model.config || new Uint8Array());
    const kind = config.find(f => f.field === 1);
    const type = kind?.wire === 0 ? kind.value : kind?.wire === 2 ? proto(kind.value).find(f => f.field === 1)?.value : 0;
    const tmpls = templates.filter(t => t.ntid === model.id).map(t => {
      const options = proto(t.config || new Uint8Array());
      return { ord: t.ord, name: t.name, qfmt: decoder.decode(options.find(f => f.field === 1)?.value || new Uint8Array()), afmt: decoder.decode(options.find(f => f.field === 2)?.value || new Uint8Array()) };
    });
    return { ...model, type: type || (tmpls.some(t => t.qfmt.includes('{{cloze:')) ? 1 : 0), flds: fields.filter(f => f.ntid === model.id), tmpls };
  });
  const decks = rows(db, 'SELECT id, name, kind FROM decks').map(d => ({ id: d.id, name: d.name.replaceAll('\x1f', '::'), dyn: proto(d.kind || new Uint8Array()).some(f => f.field === 2 && f.wire === 2) ? 1 : 0 }));
  return { models, decks };
}
function decompress(bytes, max) {
  return zstd().then(dec => {
    const parts = []; let length = 0;
    for (const part of dec.decodeStreaming([bytes])) {
      length += part.length;
      if (length > max) fail('Conteúdo descompactado excede o limite permitido.');
      parts.push(part);
    }
    const out = new Uint8Array(length); let pos = 0;
    for (const part of parts) { out.set(part, pos); pos += part.length; }
    return out;
  });
}
export async function inspect(file) {
  if (!/\.(apkg|zip)$/i.test(file.name)) fail('Escolha um pacote de deck .apkg.');
  if (file.size > 512 * 1024 * 1024) fail('Pacote acima do limite de 512 MiB.');
  const bytes = new Uint8Array(await file.arrayBuffer());
  let archive;
  try {
    archive = unzipSync(bytes, { filter: entry => ['meta', 'media', 'collection.anki2', 'collection.anki21', 'collection.anki21b'].includes(entry.name) && entry.originalSize <= LIMIT });
  } catch { fail('O .apkg não é um ZIP válido ou contém uma entrada corrompida.'); }
  const metadataVersion = archive.meta ? proto(archive.meta).filter(f => f.field === 1 && f.wire === 0).at(-1)?.value ?? fail('Versão do pacote ausente.') : null;
  const { key, version } = selectCollectionMember(archive, metadataVersion);
  const sqlite = version >= 3 ? await decompress(archive[key], LIMIT) : archive[key];
  if (sqlite.length > LIMIT || decoder.decode(sqlite.subarray(0, 16)) !== 'SQLite format 3\0') fail('Banco SQLite inválido ou grande demais.');
  const SQL = await sql();
  const db = new SQL.Database(sqlite);
  try {
    const info = metadata(db);
    const countRows = rows(db, 'SELECT did, odid, COUNT(*) AS count FROM cards GROUP BY did, odid');
    const counts = new Map();
    for (const row of countRows) {
      const id = row.did || row.odid;
      counts.set(id, (counts.get(id) || 0) + row.count);
    }
    const decks = info.decks.filter(d => !d.dyn).sort((a, b) => a.name < b.name ? -1 : a.name > b.name ? 1 : 0);
    if (!decks.length || decks.length > 1024) fail('Quantidade de decks fora do limite do Study V1.');
    if (new Set(decks.map(d => d.id)).size !== decks.length || new Set(decks.map(d => d.name)).size !== decks.length) fail('Decks duplicados no pacote.');
    const media = archive.media ? version >= 3 ? proto(await decompress(archive.media, 4 * 1024 * 1024)).filter(f => f.field === 1).length : Object.keys(JSON.parse(decoder.decode(archive.media))).length : 0;
    const manifest = archive.media ? version >= 3
      ? Object.fromEntries(proto(await decompress(archive.media, 4 * 1024 * 1024)).filter(f => f.field === 1).map((f, i) => [decoder.decode(proto(f.value).find(v => v.field === 1)?.value || new Uint8Array()), String(i)]))
      : Object.fromEntries(Object.entries(JSON.parse(decoder.decode(archive.media))).map(([id, name]) => [name, id])) : {};
    return { db, decks, models: info.models, counts, media, manifest, file, fileName: file.name, total: countRows.reduce((n, r) => n + r.count, 0) };
  } catch (error) { db.close(); throw error; }
}

const unsupported = message => { throw new Error(message); };
function cloze(value, ordinal, reveal) {
  let found = false;
  const text = value.replace(/\{\{c(\d+(?:,\d+)*)::([\s\S]*?)\}\}/g, (_, nums, body) => {
    if (body.includes('{{') || body.includes('}}')) unsupported('Cloze aninhado');
    const [answer, hint] = body.split('::');
    if (!nums.split(',').map(Number).includes(ordinal + 1)) return answer;
    found = true;
    return reveal ? answer : `[${hint || '…'}]`;
  });
  if (text.includes('{{c')) unsupported('Cloze malformado');
  return { text, found };
}
function render(template, fields, specials, front, ordinal, reveal, seen, depth = 0) {
  if (depth > 24) unsupported('Condicionais aninhadas demais');
  let result = '', pos = 0;
  while (pos < template.length) {
    const start = template.indexOf('{{', pos);
    if (start < 0) return result + template.slice(pos);
    result += template.slice(pos, start);
    const end = template.indexOf('}}', start + 2);
    if (end < 0) unsupported('Template sem fechamento');
    const token = template.slice(start + 2, end).trim();
    if (token.startsWith('#') || token.startsWith('^')) {
      const name = token.slice(1).trim(), regexp = /\{\{\s*([#^/])\s*([^{}]+?)\s*\}\}/g;
      regexp.lastIndex = end + 2;
      const stack = [name]; let match;
      while ((match = regexp.exec(template))) {
        if (match[1] !== '/') stack.push(match[2].trim());
        else if (stack.pop() !== match[2].trim()) unsupported('Condicional malformada');
        if (stack.length > 24) unsupported('Condicionais aninhadas demais');
        if (!stack.length) break;
      }
      if (!match) unsupported('Condicional sem fechamento');
      const present = Boolean((fields[name] ?? specials[name] ?? '').trim());
      if (present !== token.startsWith('^')) result += render(template.slice(end + 2, match.index), fields, specials, front, ordinal, reveal, seen, depth + 1);
      pos = regexp.lastIndex; continue;
    }
    if (token.startsWith('/')) unsupported('Fechamento condicional inesperado');
    if (token === 'FrontSide') {
      if (front === null) unsupported('FrontSide na pergunta');
      result += front;
    } else if (token.startsWith('cloze:')) {
      if (ordinal === null || !(token.slice(6) in fields)) unsupported('Campo Cloze inválido');
      const expanded = cloze(fields[token.slice(6)], ordinal, reveal);
      result += expanded.text; if (expanded.found) seen.add(ordinal);
    } else if (token === 'info-Ease:') {
      // Review statistics do not carry over into Study V1.
      result += '\ue000';
    } else if (token.startsWith('type:')) {
      const name = token.slice(5);
      if (!(name in fields)) unsupported(`Campo de digitação desconhecido: ${name}`);
      // Typing is not supported on the reader. Showing the expected answer on
      // the question side would leak it before the user reveals the back.
      if (front !== null) result += fields[name];
    } else {
      const name = token.startsWith('text:') ? token.slice(5) : token;
      if (name.includes(':') || !(name in fields) && !(name in specials) || token.startsWith('text:') && !(name in fields)) unsupported(`Filtro ou campo não suportado: ${token}`);
      result += fields[name] ?? specials[name];
    }
    pos = end + 2;
  }
  return result;
}
function plain(html, stats) {
  const doc = new DOMParser().parseFromString(html, 'text/html');
  const blocks = new Set('address article blockquote div dl fieldset figcaption figure footer h1 h2 h3 h4 h5 h6 header li ol p pre section table tr ul'.split(' '));
  let text = '';
  const br = () => { if (text && !text.endsWith('\n')) text += '\n'; };
  const visit = node => {
    if (node.nodeType === Node.TEXT_NODE) {
      const latex = /\[latex\][\s\S]*?\[\/latex\]|\[\$\$\][\s\S]*?\[\/\$\$\]|\[\$\][\s\S]*?\[\/\$\]/gi;
      text += node.textContent.replace(latex, () => { stats.latex++; return '[LaTeX omitted]'; }).replace(/\[sound:[^\]]*\]/gi, () => { stats.audio++; return ''; });
      return;
    }
    if (node.nodeType !== Node.ELEMENT_NODE) return;
    const tag = node.tagName.toLowerCase();
    if (['audio', 'video', 'script', 'style'].includes(tag)) { if (tag === 'audio' || tag === 'video') stats.audio++; return; }
    if (tag === 'img') { stats.images++; text += ` ${node.getAttribute('alt')?.trim() || '[image omitted]'} `; return; }
    if (tag === 'source') { stats.audio++; return; }
    if (tag === 'br') { br(); return; }
    if (blocks.has(tag)) br();
    for (const child of node.childNodes) visit(child);
    if (blocks.has(tag)) br();
  };
  for (const child of doc.body.childNodes) visit(child);
  const lines = text.replace(/\r\n?/g, '\n').split('\n').map(line => line.replace(/[\t\f\v ]+/g, ' ').trim().replaceAll('\ue000', '').trim()).filter(line => line !== 'Ease: %' && line !== 'Ease:');
  while (lines.length && !lines[0]) lines.shift();
  while (lines.length && !lines.at(-1)) lines.pop();
  return lines.join('\n');
}
function crc32(bytes) {
  let crc = -1;
  for (const byte of bytes) {
    crc ^= byte;
    for (let i = 0; i < 8; i++) crc = (crc >>> 1) ^ (crc & 1 ? 0xedb88320 : 0);
  }
  return (crc ^ -1) >>> 0;
}
export function convert(packageInfo, selected = '') {
  const { db, decks, models } = packageInfo;
  const chosen = decks.filter(d => !selected || d.name === selected || d.name.startsWith(selected + '::'));
  if (!chosen.length || chosen.length > 1024) fail('Deck selecionado não encontrado ou acima do limite.');
  const nameMap = new Map(chosen.map((d, index) => [d.id, index + 1]));
  const byName = new Map(chosen.map(d => [d.name, d.id]));
  const modelMap = new Map(models.map(m => [m.id, m]));
  const stats = { images: 0, audio: 0, latex: 0, cloze: 0, skipped: {}, warnings: [] };
  const cards = [], images = [];
  const noteCols = columns(db, 'notes'), cardCols = columns(db, 'cards');
  if (!['id', 'mid', 'flds'].every(c => noteCols.has(c)) || !['id', 'nid', 'did', 'ord'].every(c => cardCols.has(c))) fail('Schema de notes/cards incompatível.');
  const query = `SELECT cards.id AS id, cards.did AS did, cards.ord AS ord, ${cardCols.has('odid') ? 'cards.odid' : '0'} AS odid, notes.mid AS mid, notes.flds AS flds, ${noteCols.has('tags') ? 'notes.tags' : "''"} AS tags FROM cards JOIN notes ON notes.id = cards.nid ORDER BY cards.id`;
  const stmt = db.prepare(query); let previous = 0;
  try {
    while (stmt.step()) {
      const row = stmt.getAsObject();
      if (!Number.isSafeInteger(row.id) || row.id <= previous) fail('IDs de cartões inválidos ou fora do limite de precisão do browser.');
      previous = row.id;
      const assigned = nameMap.has(row.did) ? row.did : row.odid;
      if (!nameMap.has(assigned)) continue;
      const model = modelMap.get(row.mid);
      const skip = (reason, detail) => {
        stats.skipped[reason] = (stats.skipped[reason] || 0) + 1;
        if (stats.warnings.length < 24 && detail) stats.warnings.push(`Cartão ${row.id}: ${detail}`);
      };
      if (!model || typeof row.flds !== 'string') { skip('Modelo/campos ausentes'); continue; }
      if (encoder.encode(row.flds).length > 65536) { skip('Nota acima de 64 KiB'); continue; }
      try {
        const fields = Object.fromEntries(model.flds.map((f, i) => [f.name, row.flds.split('\x1f')[i]]));
        if (model.flds.length !== row.flds.split('\x1f').length) unsupported('Número de campos incompatível');
        const isCloze = Number(model.type) === 1;
        const template = isCloze ? model.tmpls[0] : model.tmpls.find(t => Number(t.ord) === row.ord);
        if (!template || encoder.encode(template.qfmt).length > 65536 || encoder.encode(template.afmt).length > 65536) unsupported('Template ausente ou grande demais');
        const deckName = decks.find(d => d.id === assigned).name;
        const specials = { Tags: row.tags || '', Type: model.name, Deck: deckName, Subdeck: deckName.split('::').at(-1), Card: template.name };
        if (isCloze) for (const value of Object.values(fields)) for (const match of value.matchAll(/\{\{c(\d+(?:,\d+)*)::/g)) for (const n of match[1].split(',')) specials[`c${n}`] = Number(n) === row.ord + 1 ? '1' : '';
        const seen = new Set();
        const frontHtml = render(template.qfmt, fields, specials, null, isCloze ? row.ord : null, false, seen);
        if (isCloze && !seen.has(row.ord)) unsupported('Ordinal Cloze sem exclusão');
        const backHtml = render(template.afmt, fields, specials, frontHtml, isCloze ? row.ord : null, true, new Set());
        const front = plain(frontHtml, stats), back = plain(backHtml, stats);
        if (!front) { skip('Frente vazia'); continue; }
        const a = encoder.encode(front), b = encoder.encode(back);
        if (a.length > 4096 || b.length > 4096) { skip('Face acima de 4 KiB'); continue; }
        if (/[\x00-\x08\x0b-\x1f\x7f]/.test(front + back)) { skip('Caractere de controle'); continue; }
        cards.push({ id: BigInt(row.id), deck: nameMap.get(assigned), a, b });
        for (const [face, html] of [[0, frontHtml], [1, backHtml]]) {
          const doc = new DOMParser().parseFromString(html, 'text/html');
          for (const image of doc.querySelectorAll('img[src]')) images.push({ id: BigInt(row.id), face, source: image.getAttribute('src') });
        }
        if (isCloze) stats.cloze++;
      } catch (error) { skip('Template/Cloze não suportado', error.message); }
      if (cards.length > MAX_CARDS) fail('Seleção acima de 100.000 cartões.');
    }
  } finally { stmt.free(); }
  if (!cards.length) {
    const reasons = Object.entries(stats.skipped).map(([reason, count]) => `${reason}: ${count}`).join('; ');
    fail(packageInfo.total ? `Nenhum cartão pôde ser convertido${reasons ? ` (${reasons})` : ' na seleção'}. Escolha outro deck ou confira o pacote.` : 'Este pacote não contém cartões para converter.');
  }
  const index = 40 + chosen.length * 108, data = index + cards.length * 4;
  const size = data + cards.reduce((sum, c) => sum + 16 + c.a.length + c.b.length, 0);
  if (size > 0xffffffff) fail('Arquivo .cps acima de 4 GiB.');
  const bytes = new Uint8Array(size), view = new DataView(bytes.buffer);
  bytes.set(encoder.encode('CPSTUDY\0'));
  view.setUint16(8, 1, true); view.setUint16(10, 40, true);
  for (const [offset, value] of [[12, size], [16, chosen.length], [20, cards.length], [24, 40], [28, index], [32, data]]) view.setUint32(offset, value, true);
  chosen.forEach((deck, i) => {
    const offset = 40 + i * 108, name = encoder.encode(deck.name.split('::').at(-1));
    if (!name.length || name.length > 96 || /[\x00-\x1f\x7f]/.test(deck.name)) fail(`Nome do deck fora do limite: ${deck.name}`);
    const parent = deck.name.slice(0, deck.name.lastIndexOf('::'));
    view.setUint32(offset, i + 1, true); view.setUint32(offset + 4, nameMap.get(byName.get(parent)) || 0, true);
    view.setUint16(offset + 8, name.length, true); bytes.set(name, offset + 12);
  });
  let offset = data;
  cards.forEach((card, i) => {
    view.setUint32(index + i * 4, offset, true);
    view.setBigUint64(offset, card.id, true); view.setUint32(offset + 8, card.deck, true);
    view.setUint16(offset + 12, card.a.length, true); view.setUint16(offset + 14, card.b.length, true);
    bytes.set(card.a, offset + 16); bytes.set(card.b, offset + 16 + card.a.length);
    offset += 16 + card.a.length + card.b.length;
  });
  view.setUint32(36, crc32(bytes.subarray(40)), true);
  return { bytes, decks: chosen.length, cards: cards.length, stats, images };
}

// The optional Study V1 sidecar travels with the .cps in one downloadable ZIP.
// Only referenced media is inflated; image decoding and conversion happen locally.
export async function bundleMedia(info, result, stem) {
  if (!result.images.length) return null;
  const manifest = info.manifest;
  const members = new Set(result.images.map(ref => manifest[ref.source]).filter(Boolean));
  const input = new Uint8Array(await info.file.arrayBuffer());
  const archive = unzipSync(input, { filter: entry => members.has(entry.name) && entry.originalSize <= 16 * 1024 * 1024 });
  const files = {}, records = [], cache = new Map(), count = new Map(), warnings = [];
  for (const ref of result.images) {
    const key = `${ref.id}:${ref.face}`;
    if ((count.get(key) || 0) >= 4) { warnings.push(`Cartão ${ref.id}: mais de 4 imagens na mesma face.`); continue; }
    const member = manifest[ref.source];
    if (!member || !archive[member] || !/\.(png|jpe?g|gif|webp|bmp)$/i.test(ref.source)) { warnings.push(`Cartão ${ref.id}: imagem ausente ou não suportada (${ref.source}).`); continue; }
    if (!cache.has(member)) {
      try {
        const blob = new Blob([archive[member]]);
        const bitmap = await createImageBitmap(blob);
        if (bitmap.width * bitmap.height > 16_000_000) fail('Dimensões da imagem acima do limite.');
        const ratio = Math.min(1, 800 / bitmap.width, 480 / bitmap.height);
        const canvas = document.createElement('canvas');
        canvas.width = Math.max(1, Math.round(bitmap.width * ratio)); canvas.height = Math.max(1, Math.round(bitmap.height * ratio));
        const ctx = canvas.getContext('2d', { willReadFrequently: true });
        ctx.fillStyle = '#ffffff'; ctx.fillRect(0, 0, canvas.width, canvas.height);
        ctx.drawImage(bitmap, 0, 0, canvas.width, canvas.height); bitmap.close();
        const pixels = ctx.getImageData(0, 0, canvas.width, canvas.height);
        for (let i = 0; i < pixels.data.length; i += 4) {
          const gray = Math.round(.299 * pixels.data[i] + .587 * pixels.data[i + 1] + .114 * pixels.data[i + 2]);
          pixels.data[i] = pixels.data[i + 1] = pixels.data[i + 2] = gray;
        }
        ctx.putImageData(pixels, 0, 0);
        const png = new Uint8Array(await (await new Promise(resolve => canvas.toBlob(resolve, 'image/png'))).arrayBuffer());
        const hash = [...new Uint8Array(await crypto.subtle.digest('SHA-256', png))].map(b => b.toString(16).padStart(2, '0')).join('').slice(0, 32) + '.png';
        files[`${stem}.media/${hash}`] = png;
        cache.set(member, hash);
      } catch { cache.set(member, null); }
    }
    const name = cache.get(member);
    if (!name) { warnings.push(`Cartão ${ref.id}: imagem inválida (${ref.source}).`); continue; }
    records.push({ ...ref, name }); count.set(key, (count.get(key) || 0) + 1);
  }
  if (!records.length) return { warnings, zip: null, count: 0 };
  records.sort((a, b) => a.id < b.id ? -1 : a.id > b.id ? 1 : a.face - b.face);
  const index = new Uint8Array(20 + records.length * 48), view = new DataView(index.buffer);
  index.set(encoder.encode('CPSMEDIA'));
  view.setUint16(8, 1, true); view.setUint32(12, new DataView(result.bytes.buffer).getUint32(36, true), true);
  view.setUint32(16, records.length, true);
  records.forEach((record, i) => {
    const pos = 20 + i * 48;
    view.setBigUint64(pos, record.id, true); index[pos + 8] = record.face;
    view.setUint16(pos + 10, 36, true); index.set(encoder.encode(record.name), pos + 12);
  });
  files[`${stem}.media/index.bin`] = index;
  files[`${stem}.cps`] = result.bytes;
  return { zip: zipSync(files, { level: 0 }), warnings, count: records.length };
}
