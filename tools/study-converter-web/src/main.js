import './style.css';
import { inspect, convert, bundleMedia } from './converter.js';

const $ = id => document.getElementById(id);

const ICONS = {
  info: '<svg class="status-icon" viewBox="0 0 24 24" width="16" height="16" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><circle cx="12" cy="12" r="10"/><path d="M12 16v-4m0-4h.01"/></svg>',
  success: '<svg class="status-icon" viewBox="0 0 24 24" width="16" height="16" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><circle cx="12" cy="12" r="10"/><path d="m16 9l-5.5 5.5L8 12"/></svg>',
  error: '<svg class="status-icon" viewBox="0 0 24 24" width="16" height="16" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="m21.73 18l-8-14a2 2 0 0 0-3.48 0l-8 14A2 2 0 0 0 4 21h16a2 2 0 0 0 1.73-3M12 9v4m0 4h.01"/></svg>',
  warn: '<svg viewBox="0 0 24 24" width="15" height="15" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="m21.73 18l-8-14a2 2 0 0 0-3.48 0l-8 14A2 2 0 0 0 4 21h16a2 2 0 0 0 1.73-3M12 9v4m0 4h.01"/></svg>',
};

const MAX_BYTES = 512 * 1024 * 1024;

let current = null;
let downloadUrl = null;
let downloadName = '';
let busy = false;

const status = (message, kind = 'info') => {
  const el = $('status');
  el.className = kind === 'info' ? 'status' : `status ${kind}`;
  el.innerHTML = ICONS[kind] || ICONS.info;
  const text = document.createElement('span');
  text.textContent = message;
  el.append(text);
};

const setFlow = step => {
  document.querySelectorAll('.flow-step').forEach((li, i) => {
    li.classList.toggle('is-done', i + 1 < step);
    li.classList.toggle('is-active', i + 1 === step);
  });
};

const STAGES = ['read', 'convert', 'media'];
const setStage = name => {
  const index = name === 'done' ? STAGES.length : STAGES.indexOf(name);
  document.querySelectorAll('.progress-stages li').forEach((li, i) => {
    li.classList.toggle('is-active', i === index);
    li.classList.toggle('is-done', i < index);
  });
};

const resetResult = () => {
  $('result').hidden = true;
  $('next-wrap').hidden = true;
  $('progress').hidden = true;
  if (downloadUrl) URL.revokeObjectURL(downloadUrl);
  downloadUrl = null;
};

const stemOf = fileName => fileName.replace(/\.apkg$/i, '').replace(/[^\p{L}\p{N}._-]/gu, '_') || 'study';

const appendCodeItem = (list, parts) => {
  const li = document.createElement('li');
  const label = document.createElement('label');
  const box = document.createElement('input');
  box.type = 'checkbox';
  const text = document.createElement('span');
  for (const part of parts) {
    if (typeof part === 'string') text.append(part);
    else {
      const code = document.createElement('code');
      code.textContent = part.code;
      text.append(code);
    }
  }
  label.append(box, text);
  li.append(label);
  list.append(li);
};

const showAlert = message => {
  const alert = $('next-alert');
  alert.innerHTML = ICONS.warn;
  const text = document.createElement('span');
  text.textContent = message;
  alert.append(text);
  alert.hidden = false;
};

const buildNextSteps = (stem, bundled) => {
  const list = $('next-steps');
  list.replaceChildren();
  if (bundled) {
    appendCodeItem(list, ['Extraia o ZIP em uma pasta.']);
    appendCodeItem(list, ['Copie ', { code: `${stem}.cps` }, ' e a pasta ', { code: `${stem}.media/` }, ' juntos para ', { code: '/study/' }, ' na raiz do SD.']);
    appendCodeItem(list, ['Abra Study no X4 Pro.']);
    showAlert('A pasta .media precisa ficar ao lado do .cps — sem ela, as imagens não aparecem.');
  } else {
    appendCodeItem(list, ['Copie ', { code: `${stem}.cps` }, ' para ', { code: '/study/' }, ' na raiz do SD.']);
    appendCodeItem(list, ['Abra Study no X4 Pro.']);
    $('next-alert').hidden = true;
  }
  $('next-wrap').hidden = false;
};

async function openFile(file) {
  if (!file || busy) return;
  current?.db.close();
  current = null;
  resetResult();
  $('selection').hidden = true;
  setFlow(1);

  if (!/\.(apkg|zip)$/i.test(file.name)) {
    status(`“${file.name}” não é um pacote .apkg. No Anki Desktop, use Arquivo → Exportar → Pacote de baralho Anki (.apkg).`, 'error');
    return;
  }
  if (file.size > MAX_BYTES) {
    status('Arquivo acima de 512 MB. Exporte apenas o deck que precisa, sem mídias desnecessárias.', 'error');
    return;
  }

  status('Lendo pacote localmente…');
  $('progress').hidden = false;
  setStage('read');
  try {
    await new Promise(resolve => setTimeout(resolve, 0));
    current = await inspect(file);
    if (!current.total) {
      current.db.close();
      current = null;
      $('progress').hidden = true;
      status('Este pacote não tem cartões para converter. Confira se o deck certo foi exportado do Anki.', 'error');
      return;
    }
    $('name').textContent = file.name;
    $('summary').textContent = `${current.total.toLocaleString('pt-BR')} cartões · ${current.decks.length} decks`;
    $('deck').replaceChildren(new Option('Todos os decks do pacote', ''));
    for (const deck of current.decks) {
      const depth = deck.name.split('::').length - 1;
      $('deck').add(new Option(`${'  '.repeat(depth)}${deck.name} · ${current.counts.get(deck.id) || 0} cartões`, deck.name));
    }
    $('selection').hidden = false;
    setFlow(2);
    $('progress').hidden = true;
    status('Pronto. Confira o deck e clique em converter — nada sai do seu dispositivo.', 'success');
  } catch (error) {
    $('progress').hidden = true;
    status(error.message || 'Não foi possível ler este pacote. Ele pode estar corrompido ou não ser .apkg.', 'error');
  }
}

$('file').addEventListener('change', event => {
  const file = event.target.files[0];
  event.target.value = '';
  openFile(file);
});
$('change').addEventListener('click', () => $('file').click());
document.querySelectorAll('.js-open-file').forEach(btn => btn.addEventListener('click', () => $('file').click()));

const drop = $('drop');
for (const name of ['dragenter', 'dragover']) drop.addEventListener(name, event => { event.preventDefault(); drop.classList.add('dragover'); });
for (const name of ['dragleave', 'drop']) drop.addEventListener(name, event => { event.preventDefault(); drop.classList.remove('dragover'); });
drop.addEventListener('drop', event => openFile(event.dataTransfer.files[0]));

$('convert').addEventListener('click', async () => {
  if (!current || busy) return;
  busy = true;
  resetResult();
  const convertBtn = $('convert');
  convertBtn.disabled = true;
  convertBtn.classList.add('is-busy');
  convertBtn.querySelector('.btn-label').textContent = 'Convertendo…';
  $('progress').hidden = false;
  setStage('convert');
  status('Convertendo cartões…');
  try {
    await new Promise(resolve => setTimeout(resolve, 30));
    const result = convert(current, $('deck').value);
    const { bytes, decks, cards, stats } = result;
    const stem = stemOf(current.fileName);
    setStage('media');
    status('Preparando imagens, se houver…');
    const media = await bundleMedia(current, result, stem);
    const bundled = Boolean(media?.zip);
    downloadName = `${stem}.${bundled ? 'zip' : 'cps'}`;
    downloadUrl = URL.createObjectURL(new Blob([media?.zip || bytes], { type: bundled ? 'application/zip' : 'application/octet-stream' }));
    $('download-label').textContent = bundled ? 'Baixar pacote .zip' : 'Baixar arquivo .cps';
    const skipped = Object.values(stats.skipped).reduce((a, b) => a + b, 0);
    $('result-summary').textContent = `${cards.toLocaleString('pt-BR')} ${cards === 1 ? 'cartão' : 'cartões'} · ${decks} ${decks === 1 ? 'deck' : 'decks'}${skipped ? ` · ${skipped} ${skipped === 1 ? 'ignorado' : 'ignorados'}` : ''}${bundled ? ` · ${media.count} imagens` : ''}`;
    $('report').replaceChildren();
    const lines = [`${cards} convertidos · ${skipped} ignorados · ${stats.cloze} Cloze`, `Mídias no manifesto: ${current.media}; imagens incluídas: ${media?.count || 0}; imagens referenciadas: ${stats.images}; áudio omitido: ${stats.audio}; LaTeX omitido: ${stats.latex}.`, ...Object.entries(stats.skipped).map(([key, n]) => `${key}: ${n}`), ...stats.warnings, ...(media?.warnings || [])];
    for (const line of lines) {
      const p = document.createElement('p');
      p.textContent = line;
      $('report').append(p);
    }
    buildNextSteps(stem, bundled);
    setStage('done');
    $('progress').hidden = true;
    $('result').hidden = false;
    setFlow(3);
    status('Conversão concluída. Siga a lista abaixo para copiar ao leitor.', 'success');
  } catch (error) {
    $('progress').hidden = true;
    setFlow(2);
    status(error.message || 'Falha na conversão.', 'error');
  } finally {
    busy = false;
    convertBtn.disabled = false;
    convertBtn.classList.remove('is-busy');
    convertBtn.querySelector('.btn-label').textContent = 'Converter para .cps';
  }
});

$('download').addEventListener('click', () => {
  if (!downloadUrl) return;
  const a = document.createElement('a');
  a.href = downloadUrl;
  a.download = downloadName;
  a.click();
  setFlow(4);
  status('Arquivo salvo. Agora é só copiar para /study/ conforme a lista acima.', 'success');
});

window.addEventListener('beforeunload', () => {
  current?.db.close();
  if (downloadUrl) URL.revokeObjectURL(downloadUrl);
});

/* Scroll reveal — one-shot, GPU-friendly, skipped under reduced motion. */
const reducedMotion = window.matchMedia('(prefers-reduced-motion: reduce)').matches;
const reveals = document.querySelectorAll('.reveal');
if (reducedMotion || !('IntersectionObserver' in window)) {
  reveals.forEach(el => el.classList.add('in'));
} else {
  const observer = new IntersectionObserver(entries => {
    for (const entry of entries) {
      if (entry.isIntersecting) {
        entry.target.classList.add('in');
        observer.unobserve(entry.target);
      }
    }
  }, { rootMargin: '0px 0px -6% 0px', threshold: 0.08 });
  reveals.forEach(el => observer.observe(el));
}

status('Escolha um arquivo para começar.');
