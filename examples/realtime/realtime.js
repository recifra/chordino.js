// Listens to the microphone the way an app does, through chord-processor.mjs and
// chord-live-worker.mjs (the production worklet and worker), and lays the chords
// they post over the bins the inspect build works out from the same audio in
// inspect-worker.js. Like the player,
// every panel scrolls under the clock, but here the newest audio is at the right
// edge. Left of the read line, delayMs behind the newest frame, frames are
// settled: they show the values the worker's decision there was made on. Right
// of it they still move with every buffer, and the chords there are the Viterbi
// path as it stands, not yet posted.

import { chordKey } from '../player/chords.js';
import { buildExport } from '../player/export.js';

// Where chord-processor.mjs, chord-live-worker.mjs, chordino.mjs and
// chordino.wasm live
const LIB = new URL('../lib/', location.href);
// Chordino's step size: frames come sampleRate / STEP times a second
const STEP = 2048;
// Left margin of every canvas, where bin labels go.
const GUTTER = 72;
const BAND = 24;
const GATE_HEIGHT = 4;
const TICK_BAND = 14;
// A chord that is never posted still gets a salience row when it wins at least
// this many frames in the window on its own.
const MIN_FRAME_WINS = 5;
const MAX_EXTRA_ROWS = 12;
// Settled frames kept for the export
const KEEP_SECONDS = 600;
const STORAGE_KEY = 'chordino-realtime:hidden';
// chord-live-worker.mjs's silence gate, held against the sustained level (see
// inspect-worker.js): under it, nothing is posted
const GATE_RMS = 0.008;
const GATE_DB = 20 * Math.log10(GATE_RMS);
// Each frame is normalized on its own, which paints a quiet room's noise as
// brightly as a chord; frames under the gate are drawn this dim instead.
const QUIET_DIM = 0.15;
const PROCESSING = { echoCancellation: 'echo', noiseSuppression: 'noise', autoGainControl: 'gain' };

const PANELS = [
  {
    id: 'chroma',
    field: 'chroma',
    title: 'Chroma',
    note: 'NNLS chroma, treble on top of bass, each half normalized per frame',
    rowHeight: 11,
  },
  {
    id: 'smoothedchroma',
    field: 'smoothed',
    title: 'Smoothed chroma',
    note: 'EMA the chord matcher actually scores, each half normalized per frame; it runs both ways, so right of the read line it still changes',
    rowHeight: 11,
  },
  {
    id: 'chordsalience',
    field: 'salience',
    title: 'Chord salience',
    note: 'after the confidence gate (Viterbi input), normalized per frame; italic = frame winner never posted',
    rowHeight: 14,
  },
  {
    id: 'semitonespectrum',
    field: 'semitones',
    title: 'Semitone spectrum',
    note: 'NNLS note activations A0–Ab7, normalized per frame (sqrt)',
    rowHeight: 3,
    gamma: 0.5,
  },
];

const $ = (id) => document.getElementById(id);
const listenButton = $('listen');
const statusText = $('status');
const postedChord = $('posted-chord');
const newestChord = $('newest-chord');
const timeText = $('time');
const candidates = $('candidates');
const windowSelect = $('window');
const delaySelect = $('delay');
const exportButton = $('export');
const exportStep = $('export-step');
const levelText = $('level');
const processingText = $('processing');
const downloadButton = $('download');
const processingBox = $('voice-processing');

const LUT = buildLut([[68, 1, 84], [59, 82, 139], [33, 145, 140], [94, 201, 98], [253, 231, 37]]);

let session = null;
// What inspect-worker.js announces: labels, templates, chromaNames, semitoneNames
let names = null;
let frameRate = 0;
let startedAt = 0;
let stoppedAt = null;
let settled = [];
let pending = [];
let path = [];
let posted = [];
let readAt = null;
let newest = null;
// Bumped whenever the data changes, so panels only rebuild their images then
let version = 0;
let theme = readTheme();
let windowSec = Number(windowSelect.value);
let lastKey = '';
let wakeLock = null;
// The WAV of the last session, kept once it stops, and the request for one
// waiting on inspect-worker.js
let audio = null;
let wavRequest = null;
// The loudest input since the level was last shown, and when that was
let levelPeak = 0;
let levelShownAt = 0;
const levelSamples = new Float32Array(2048);

const hidden = new Set(loadHidden());
const lane = createLane();
const panels = PANELS.map(createPanel);

// --- listening -------------------------------------------------------------------

listenButton.addEventListener('click', () => {
  if (session) {
    stop();
  } else {
    start();
  }
});

async function start() {
  if (!navigator.mediaDevices) {
    setStatus(`No microphone: browsers only give it to https and localhost pages, and this is ${location.origin}. ` +
      'On Android, open it on localhost through adb reverse, or list this origin in chrome://flags/#unsafely-treat-insecure-origin-as-secure.');
    return;
  }
  listenButton.disabled = true;
  delaySelect.disabled = true;
  processingBox.disabled = true;
  setStatus('Asking for the microphone…');
  let stream;
  let context;
  try {
    // Voice processing is for speech: noise suppression takes sustained notes for
    // noise and cuts them, and gain control lifts the noise floor between them.
    // It is only on here to compare.
    const processing = processingBox.checked;
    stream = await navigator.mediaDevices.getUserMedia({
      audio: { echoCancellation: processing, noiseSuppression: processing, autoGainControl: processing },
    });
    showProcessing(stream.getAudioTracks()[0].getSettings());
    context = new AudioContext();
    const source = context.createMediaStreamSource(stream);
    const analyser = new AnalyserNode(context, { fftSize: levelSamples.length });
    await context.audioWorklet.addModule(new URL('chord-processor.mjs', LIB));
    await context.audioWorklet.addModule('./tap-processor.js');

    const delayMs = Number(delaySelect.value);
    const mono = { channelCount: 1, channelCountMode: 'explicit', channelInterpretation: 'discrete' };
    const chordNode = new AudioWorkletNode(context, 'chord-processor', mono);
    const tapNode = new AudioWorkletNode(context, 'tap-processor', mono);
    // It loads chordino.wasm from next to chordino.mjs
    const chordWorker = new Worker(new URL('chord-live-worker.mjs', LIB), { type: 'module' });
    const worker = new Worker('./inspect-worker.js');

    clearData();
    frameRate = context.sampleRate / STEP;
    startedAt = context.currentTime;
    stoppedAt = null;
    session = { stream, context, chordNode, chordWorker, tapNode, worker, analyser };
    chordWorker.onmessage = ({ data }) => onChordMessage(data, context);
    worker.onmessage = ({ data }) => onWorkerMessage(data);
    worker.onerror = (event) => {
      event.preventDefault();
      setStatus(`Inspect worker failed: ${event.message || 'could not run inspect-worker.js'}`);
    };

    // The audio goes from each worklet straight to its worker, not through this thread
    const chordChannel = new MessageChannel();
    chordWorker.postMessage({ type: 'start', sampleRate: context.sampleRate, delayMs, port: chordChannel.port1 }, [chordChannel.port1]);
    chordNode.port.postMessage({ type: 'port', port: chordChannel.port2 }, [chordChannel.port2]);
    const channel = new MessageChannel();
    worker.postMessage({ type: 'start', sampleRate: context.sampleRate, delayMs, port: channel.port1 }, [channel.port1]);
    tapNode.port.postMessage({ type: 'port', port: channel.port2 }, [channel.port2]);
    source.connect(chordNode);
    source.connect(tapNode);
    source.connect(analyser);
    chordNode.connect(context.destination);
    tapNode.connect(context.destination);

    listenButton.textContent = 'Stop listening';
    keepAwake();
    setStatus('Loading chordino.wasm…');
  } catch (err) {
    stream?.getTracks().forEach((track) => track.stop());
    context?.close();
    session = null;
    delaySelect.disabled = false;
    processingBox.disabled = false;
    setStatus(`Could not listen: ${err.message}`);
  }
  listenButton.disabled = false;
}

/** Keeps the screen on while listening: a tablet left on a stand sleeps mid-test otherwise */
async function keepAwake() {
  try {
    wakeLock = (await navigator.wakeLock?.request('screen')) ?? null;
  } catch {
    // Refused, as on battery saver: the screen sleeps as usual
  }
}

// The lock goes whenever the page is hidden
document.addEventListener('visibilitychange', () => {
  if (session && document.visibilityState === 'visible') {
    keepAwake();
  }
});

async function stop() {
  const current = session;
  session = null;
  stoppedAt = current.context.currentTime;
  listenButton.disabled = true;
  wakeLock?.release();
  wakeLock = null;
  current.stream.getTracks().forEach((track) => track.stop());
  current.tapNode.port.postMessage({ type: 'stop' });
  current.chordNode.port.postMessage({ type: 'stop' });
  audio = await fetchWav(current.worker);
  current.worker.terminate();
  // The plugin goes with the worker
  current.chordWorker.terminate();
  await current.context.close();
  levelText.textContent = 'input –';
  levelText.className = 'caption';
  listenButton.textContent = 'Start listening';
  listenButton.disabled = false;
  delaySelect.disabled = false;
  processingBox.disabled = false;
  downloadButton.disabled = !audio;
  setStatus(`Stopped after ${formatTime(stoppedAt - startedAt)} · ${posted.length} chords posted`);
}

function onChordMessage(data, context) {
  if (data.type === 'ready') {
    setStatus('Listening…');
  } else if (data.type === 'chordResult') {
    const postedAt = context.currentTime;
    posted.push({ ...data.result, postedAt });
    version++;
    setStatus(`Listening · ${data.result.chord} posted ${(postedAt - data.result.time).toFixed(2)} s after it started`);
  } else if (data.type === 'error') {
    setStatus(`Could not load chordino.wasm: ${data.message}`);
  }
}

function onWorkerMessage(data) {
  if (data.type === 'ready') {
    names = data;
  } else if (data.type === 'frames') {
    settled.push(...data.settled);
    const keepFrom = data.newest - KEEP_SECONDS;
    const drop = settled.findIndex((frame) => frame.time >= keepFrom);
    if (drop > 0) {
      settled.splice(0, drop);
    }
    ({ pending, path, readAt, newest } = data);
    version++;
    exportButton.disabled = settled.length === 0;
    downloadButton.disabled = false;
  } else if (data.type === 'wav') {
    wavRequest?.(data.blob);
    wavRequest = null;
  } else if (data.type === 'error') {
    setStatus(`Inspect worker failed: ${data.message}`);
  }
}

/** The audio inspect-worker.js kept, as a WAV; null if it does not answer within 5 s */
function fetchWav(worker) {
  return new Promise((resolve) => {
    wavRequest = resolve;
    worker.postMessage({ type: 'wav' });
    setTimeout(() => resolve(null), 5000);
  });
}

function clearData() {
  settled = [];
  pending = [];
  path = [];
  posted = [];
  readAt = null;
  newest = null;
  version++;
  exportButton.disabled = true;
  downloadButton.disabled = true;
  audio = null;
  lastKey = '';
}

function setStatus(text) {
  statusText.textContent = text;
}

/** The voice processing the browser applied, which can differ from what was asked for */
function showProcessing(settings) {
  const on = Object.keys(PROCESSING).filter((name) => settings[name] !== false);
  processingText.textContent = Object.entries(PROCESSING)
    .map(([name, label]) => `${label} ${settings[name] === undefined ? '?' : settings[name] ? 'on' : 'off'}`)
    .join(' · ');
  processingText.className = `caption ${on.length ? 'bad' : 'ok'}`;
}

/** The loudest RMS reaching the worklets over the last 200 ms, in dB, against the gate */
function showLevel(analyser) {
  analyser.getFloatTimeDomainData(levelSamples);
  let sumSq = 0;
  for (const sample of levelSamples) {
    sumSq += sample * sample;
  }
  levelPeak = Math.max(levelPeak, Math.sqrt(sumSq / levelSamples.length));
  const now = performance.now();
  if (now - levelShownAt < 200) {
    return;
  }
  const db = 20 * Math.log10(levelPeak);
  const sustained = settled.at(-1)?.sustained ?? 0;
  const held = 20 * Math.log10(sustained);
  levelText.textContent = `input ${formatDb(db)} · sustained ${formatDb(held)} · gate ${formatDb(GATE_DB)}`;
  levelText.className = `caption ${sustained >= GATE_RMS ? 'ok' : 'bad'}`;
  levelPeak = 0;
  levelShownAt = now;
}

/** The clock everything scrolls by: the AudioContext's, frozen once stopped. */
function clock() {
  return session ? session.context.currentTime : stoppedAt;
}

// --- export ----------------------------------------------------------------------

exportButton.addEventListener('click', () => {
  const audioName = fileName();
  const text = buildExport({ view: buildView(), expected: null, audioName, step: Number(exportStep.value) });
  save(new Blob([text], { type: 'application/json' }), `${audioName}.chordino.json`);
});

downloadButton.addEventListener('click', async () => {
  const blob = session ? await fetchWav(session.worker) : audio;
  if (blob) {
    save(blob, `${fileName()}.wav`);
  }
});

function fileName() {
  return `realtime-${new Date().toISOString().replace(/[:.]/g, '-')}`;
}

function save(blob, name) {
  const link = document.createElement('a');
  link.href = URL.createObjectURL(blob);
  link.download = name;
  link.click();
  setTimeout(() => URL.revokeObjectURL(link.href), 0);
}

/** The settled frames and posted chords in the shape the player hands buildExport. */
function buildView() {
  const frames = settled.length;
  const origin = settled[0].time;
  const duration = frames / frameRate;
  const stack = (field, size) => {
    const values = new Float32Array(frames * size);
    settled.forEach((frame, i) => values.set(frame[field], i * size));
    return values;
  };
  const segments = postedSegments(settled.at(-1).time)
    .map((segment) => ({ ...segment, start: segment.start - origin, end: segment.end - origin }))
    .filter((segment) => segment.end > 0)
    .map((segment) => ({ ...segment, start: Math.max(segment.start, 0), end: Math.min(segment.end, duration) }));
  // The production and the inspect worker started on different buffers, so their
  // frames can be a few milliseconds apart
  if ((segments[0]?.start ?? duration) > 0.5 / frameRate) {
    segments.unshift({ chord: 'N', start: 0, end: segments[0]?.start ?? duration });
  } else {
    segments[0].start = 0;
  }
  const labelByKey = new Map();
  names.labels.forEach((label, i) => {
    const { id } = chordKey(label);
    if (!labelByKey.has(id)) {
      labelByKey.set(id, i);
    }
  });
  return {
    frames,
    frameRate,
    duration,
    segments,
    salience: { binCount: names.labels.length, binNames: names.labels, values: stack('salience', names.labels.length) },
    chroma: { binCount: 24, binNames: names.chromaNames, values: stack('chroma', 24) },
    gate: Uint8Array.from(settled, (frame) => frame.gate),
    comparison: null,
    labelByKey,
    templateCount: names.templates,
  };
}

/**
 * The posted chords as segments, each from the time it reports until the next
 * one, the last until `until`. A chord can report a start before the previous
 * one's, when the decoder moved that change back after posting it; the earlier
 * one is then cut short.
 */
function postedSegments(until) {
  const segments = [];
  for (const { chord, time } of posted) {
    const start = Math.max(time, segments.at(-1)?.start ?? -Infinity);
    if (segments.length > 0) {
      segments.at(-1).end = start;
    }
    segments.push({ chord, start, end: Math.max(until, start) });
  }
  return segments;
}

// --- layout ----------------------------------------------------------------------

/** Rows of a panel: chroma and semitones are fixed, salience follows the chords in view. */
function layout(panel, frames) {
  if (panel.id === 'chroma' || panel.id === 'smoothedchroma') {
    const rows = [];
    for (let bin = 23; bin >= 0; bin--) {
      rows.push({ bin, label: names.chromaNames[bin] });
    }
    return { rows, groups: new Uint8Array(24).map((_, bin) => (bin < 12 ? 0 : 1)), binCount: 24, separators: [12] };
  }
  if (panel.id === 'semitonespectrum') {
    const rows = [];
    for (let bin = names.semitoneNames.length - 1; bin >= 0; bin--) {
      const label = names.semitoneNames[bin];
      rows.push({ bin, label: /^C\d/.test(label) ? label : '' });
    }
    return { rows, groups: new Uint8Array(rows.length), binCount: rows.length, separators: [] };
  }
  return salienceLayout(frames);
}

// Rows: chords posted or on the path in view, in order of appearance, then labels
// that win frames on their own, then N.
function salienceLayout(frames) {
  const { labels } = names;
  const first = frames[0]?.time ?? Infinity;
  const shown = [
    ...postedSegments(readAt ?? first).filter((segment) => segment.end >= first),
    ...path,
  ].map((segment) => segment.chord);
  const onPath = [...new Set(shown)].filter((chord) => chord !== 'N' && labels.includes(chord));
  const wins = new Map();
  for (const frame of frames) {
    let best = 0;
    for (let label = 1; label < labels.length; label++) {
      if (frame.salience[label] > frame.salience[best]) {
        best = label;
      }
    }
    wins.set(best, (wins.get(best) ?? 0) + 1);
  }
  const extra = [...wins]
    .filter(([label, count]) => count >= MIN_FRAME_WINS && labels[label] !== 'N' && !onPath.includes(labels[label]))
    .sort((a, b) => b[1] - a[1])
    .slice(0, MAX_EXTRA_ROWS)
    .map(([label]) => labels[label]);
  const rows = [
    ...onPath.map((label) => ({ bin: labels.indexOf(label), label })),
    ...extra.map((label) => ({ bin: labels.indexOf(label), label, offPath: true })),
    { bin: labels.indexOf('N'), label: 'N' },
  ];
  return { rows, groups: new Uint8Array(labels.length), binCount: labels.length, separators: [] };
}

/** One pixel per frame and row, each frame normalized per group as the player does. */
function renderImage(panel, frames, { rows, groups, binCount }) {
  const canvas = panel.image ??= document.createElement('canvas');
  canvas.width = Math.max(frames.length, 1);
  canvas.height = rows.length;
  const context = canvas.getContext('2d');
  const image = context.createImageData(canvas.width, rows.length);
  const peaks = new Float32Array(Math.max(...groups) + 1);
  const gamma = panel.gamma ?? 1;
  frames.forEach((frame, x) => {
    const values = frame[panel.field];
    const dim = frame.sustained < GATE_RMS ? QUIET_DIM : 1;
    peaks.fill(0);
    for (let bin = 0; bin < binCount; bin++) {
      peaks[groups[bin]] = Math.max(peaks[groups[bin]], values[bin]);
    }
    rows.forEach((row, y) => {
      const peak = peaks[groups[row.bin]];
      const level = peak > 0 ? dim * (values[row.bin] / peak) ** gamma : 0;
      const color = Math.round(level * 255) * 3;
      const pixel = (y * canvas.width + x) * 4;
      image.data[pixel] = LUT[color];
      image.data[pixel + 1] = LUT[color + 1];
      image.data[pixel + 2] = LUT[color + 2];
      image.data[pixel + 3] = 255;
    });
  });
  context.putImageData(image, 0, 0);
}

function buildLut(stops) {
  const lut = new Uint8ClampedArray(256 * 3);
  for (let i = 0; i < 256; i++) {
    const x = (i / 255) * (stops.length - 1);
    const k = Math.min(Math.floor(x), stops.length - 2);
    for (let c = 0; c < 3; c++) {
      lut[i * 3 + c] = stops[k][c] + (stops[k + 1][c] - stops[k][c]) * (x - k);
    }
  }
  return lut;
}

// --- DOM -------------------------------------------------------------------------

function createLane() {
  const section = document.createElement('section');
  section.className = 'panel';
  section.innerHTML = '<h2>Chords <small>posted by chord-live-worker.mjs, from the time each reports; ' +
    'faded, right of the read line: the Viterbi path there now, not posted yet; ' +
    'ticks on top: frames the confidence gate rejected (gray: chord too close to N, purple: another root too close); ' +
    'the panels below go dim where the sustained level is under the −42 dB silence gate, where nothing is posted</small></h2>';
  const canvas = document.createElement('canvas');
  section.append(canvas);
  $('panels').append(section);
  return { canvas };
}

function createPanel(config) {
  const section = document.createElement('section');
  section.className = 'panel';
  section.hidden = hidden.has(config.id);
  const title = document.createElement('h2');
  title.textContent = config.title;
  const note = document.createElement('small');
  note.textContent = config.note;
  title.append(note);
  const canvas = document.createElement('canvas');
  section.append(title, canvas);
  $('panels').append(section);

  const toggle = document.createElement('label');
  const checkbox = document.createElement('input');
  checkbox.type = 'checkbox';
  checkbox.checked = !section.hidden;
  checkbox.addEventListener('change', () => {
    section.hidden = !checkbox.checked;
    if (checkbox.checked) {
      hidden.delete(config.id);
    } else {
      hidden.add(config.id);
    }
    saveHidden();
    lastKey = '';
  });
  toggle.append(checkbox, ` ${config.title}`);
  $('toggles').append(toggle);

  return { ...config, section, canvas, layout: null, imageKey: '' };
}

windowSelect.addEventListener('change', () => {
  windowSec = Number(windowSelect.value);
  lastKey = '';
});

matchMedia('(prefers-color-scheme: dark)').addEventListener('change', () => {
  theme = readTheme();
  lastKey = '';
});

function readTheme() {
  const style = getComputedStyle(document.documentElement);
  const read = (name) => style.getPropertyValue(name).trim();
  return {
    fg: read('--fg'),
    muted: read('--muted'),
    accent: read('--accent'),
    accentSoft: read('--accent-soft'),
    gateN: read('--gate-n'),
    gateRoot: read('--gate-root'),
    laneA: read('--lane-a'),
    laneB: read('--lane-b'),
    empty: read('--plot-empty'),
    bg: read('--bg'),
    playhead: read('--playhead'),
  };
}

function loadHidden() {
  try {
    return JSON.parse(localStorage.getItem(STORAGE_KEY)) ?? [];
  } catch {
    return [];
  }
}

function saveHidden() {
  try {
    localStorage.setItem(STORAGE_KEY, JSON.stringify([...hidden]));
  } catch {
    // Only a convenience: the toggles still work for this visit
  }
}

// --- drawing ---------------------------------------------------------------------

requestAnimationFrame(function tick() {
  requestAnimationFrame(tick);
  if (session) {
    showLevel(session.analyser);
  }
  const time = clock();
  if (!names || time === null) {
    return;
  }
  const key = `${time}|${version}|${windowSec}|${document.documentElement.clientWidth}|${devicePixelRatio}`;
  if (key === lastKey) {
    return;
  }
  lastKey = key;
  const frames = visibleFrames(time - windowSec);
  drawLane(time, frames);
  for (const panel of panels) {
    if (!panel.section.hidden) {
      drawPanel(panel, time, frames);
    }
  }
  updateNow(time);
});

/** Settled frames from `start` on, then the pending ones, oldest first. */
function visibleFrames(start) {
  let first = settled.length;
  while (first > 0 && settled[first - 1].time >= start) {
    first--;
  }
  return [...settled.slice(first), ...pending];
}

/** Sizes the canvas backing store for the current width and returns a CSS-pixel context. */
function fit(canvas, height) {
  const ratio = devicePixelRatio || 1;
  canvas.style.height = `${height}px`;
  const width = canvas.clientWidth;
  if (canvas.width !== Math.round(width * ratio) || canvas.height !== Math.round(height * ratio)) {
    canvas.width = Math.round(width * ratio);
    canvas.height = Math.round(height * ratio);
  }
  const context = canvas.getContext('2d');
  context.setTransform(ratio, 0, 0, ratio, 0, 0);
  context.imageSmoothingEnabled = false;
  context.clearRect(0, 0, width, height);
  return { context, width, height, plotWidth: width - GUTTER };
}

/** Maps time to x for a window ending at `time`, the newest audio on the right. */
function timeScale(area, time) {
  const pxPerSec = area.plotWidth / windowSec;
  const start = time - windowSec;
  return { pxPerSec, start, x: (t) => GUTTER + (t - start) * pxPerSec };
}

function drawLane(time, frames) {
  const area = fit(lane.canvas, BAND + TICK_BAND);
  const { context, width, height } = area;
  const { pxPerSec, start, x } = timeScale(area, time);

  context.save();
  context.beginPath();
  context.rect(GUTTER, 0, area.plotWidth, height);
  context.clip();
  const segments = postedSegments(readAt ?? time);
  drawSegments(context, segments, x, width, segments.length - 1);
  context.globalAlpha = 0.45;
  drawSegments(context, path.map((segment) => ({ ...segment, start: Math.max(segment.start, readAt) })), x, width, path.length - 1);
  context.globalAlpha = 1;
  for (const frame of frames) {
    if (frame.gate) {
      context.fillStyle = frame.gate === 1 ? theme.gateN : theme.gateRoot;
      context.fillRect(x(frame.time - 0.5 / frameRate), 0, pxPerSec / frameRate, GATE_HEIGHT);
    }
  }

  context.font = '10px system-ui, sans-serif';
  context.textBaseline = 'middle';
  context.fillStyle = theme.muted;
  const labelEvery = pxPerSec >= 60 ? 1 : pxPerSec >= 25 ? 2 : 5;
  for (let second = Math.ceil(Math.max(start - startedAt, 0)); startedAt + second <= time; second++) {
    const tickX = x(startedAt + second);
    context.fillRect(tickX, BAND, 1, second % labelEvery === 0 ? 6 : 3);
    if (second % labelEvery === 0) {
      context.fillText(formatTime(second, 0), tickX + 3, BAND + 9);
    }
  }
  context.restore();

  context.font = '11px system-ui, sans-serif';
  context.fillStyle = theme.muted;
  context.textAlign = 'right';
  context.fillText('posted', GUTTER - 6, BAND / 2);
  context.textAlign = 'left';
  drawReadLine(context, area, x);
}

function drawSegments(context, segments, x, width, current) {
  context.font = '600 13px system-ui, sans-serif';
  context.textBaseline = 'middle';
  segments.forEach((segment, i) => {
    const x0 = x(segment.start);
    const x1 = x(segment.end);
    if (x1 <= x0 || x1 < GUTTER || x0 > width) {
      return;
    }
    context.fillStyle = i === current ? theme.accentSoft : i % 2 ? theme.laneB : theme.laneA;
    context.fillRect(x0, 0, x1 - x0, BAND);
    const label = segment.chord;
    const labelWidth = context.measureText(label).width;
    // Keep the label of a chord that started off-screen pinned to the left edge
    const labelX = Math.min(Math.max(x0, GUTTER) + 4, x1 - labelWidth - 4);
    if (labelX >= Math.max(x0, GUTTER) + 2) {
      context.fillStyle = label === 'N' ? theme.muted : theme.fg;
      context.fillText(label, labelX, BAND / 2);
    }
  });
}

function drawPanel(panel, time, frames) {
  // The image only changes with the data, the window or the theme; scrolling just moves it
  const imageKey = `${version}|${windowSec}`;
  if (panel.imageKey !== imageKey) {
    panel.imageKey = imageKey;
    panel.layout = layout(panel, frames);
    panel.frames = frames;
    renderImage(panel, frames, panel.layout);
  }
  const { rows, separators } = panel.layout;
  const area = fit(panel.canvas, rows.length * panel.rowHeight);
  const { context, height } = area;
  const { x } = timeScale(area, time);

  context.fillStyle = theme.empty;
  context.fillRect(GUTTER, 0, area.plotWidth, height);
  context.save();
  context.beginPath();
  context.rect(GUTTER, 0, area.plotWidth, height);
  context.clip();
  if (panel.frames.length > 0) {
    const half = 0.5 / frameRate;
    const x0 = x(panel.frames[0].time - half);
    const x1 = x(panel.frames.at(-1).time + half);
    context.drawImage(panel.image, x0, 0, x1 - x0, height);
    // Frames right of the read line still change
    if (readAt !== null) {
      context.globalAlpha = 0.3;
      context.fillStyle = theme.bg;
      context.fillRect(x(readAt + half), 0, x1 - x(readAt + half), height);
      context.globalAlpha = 1;
    }
  }
  context.fillStyle = theme.empty;
  for (const row of separators) {
    context.fillRect(GUTTER, row * panel.rowHeight - 1, area.plotWidth, 2);
  }
  context.restore();

  context.textAlign = 'right';
  context.textBaseline = 'middle';
  const fontSize = Math.min(11, Math.max(9, panel.rowHeight - 2));
  rows.forEach((row, y) => {
    if (!row.label) {
      return;
    }
    context.font = `${row.offPath ? 'italic ' : ''}${fontSize}px system-ui, sans-serif`;
    context.fillStyle = row.offPath || row.label === 'N' ? theme.muted : theme.fg;
    context.fillText(row.label, GUTTER - 6, (y + 0.5) * panel.rowHeight);
  });
  context.textAlign = 'left';
  drawReadLine(context, area, x);
}

/** The read point: what the production worker posts is read here, delayMs behind the newest frame. */
function drawReadLine(context, area, x) {
  if (readAt === null || x(readAt) < GUTTER) {
    return;
  }
  context.fillStyle = theme.playhead;
  context.fillRect(x(readAt) - 0.75, 0, 1.5, area.height);
}

// --- readout ---------------------------------------------------------------------

let readoutVersion = -1;

function updateNow(time) {
  timeText.textContent = formatTime(time - startedAt);
  if (readoutVersion === version) {
    return;
  }
  readoutVersion = version;
  postedChord.textContent = posted.at(-1)?.chord ?? '–';
  newestChord.textContent = path.at(-1)?.chord ?? '–';

  // Chord probabilities at the read point, where the posted chord was read
  const frame = settled.at(-1);
  if (!frame) {
    candidates.replaceChildren();
    return;
  }
  const row = frame.salience;
  const top = [...row.keys()].sort((a, b) => row[b] - row[a]).slice(0, 5);
  // Similar labels split the weight between them, so bars are scaled to the winner
  const best = row[top[0]] || 1;
  candidates.replaceChildren(...top.map((label) => {
    const item = document.createElement('li');
    const name = document.createElement('span');
    name.textContent = names.labels[label];
    const track = document.createElement('span');
    const bar = document.createElement('div');
    bar.className = 'bar';
    bar.style.width = `${(row[label] / best) * 100}%`;
    track.append(bar);
    const pct = document.createElement('span');
    pct.className = 'pct';
    pct.textContent = `${(row[label] * 100).toFixed(2)}%`;
    item.append(name, track, pct);
    return item;
  }));
}

function formatDb(db) {
  return `${db > -100 ? `−${Math.abs(db).toFixed(0)}` : '−∞'} dB`;
}

function formatTime(seconds, digits = 1) {
  const minutes = Math.floor(seconds / 60);
  const rest = seconds - minutes * 60;
  return `${minutes}:${rest.toFixed(digits).padStart(digits ? digits + 3 : 2, '0')}`;
}
