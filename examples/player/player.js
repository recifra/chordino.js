// Drop an audio file, let the worker precompute Chordino's per-frame bins, then
// play it: every panel scrolls under a fixed playhead driven by audio.currentTime,
// so pause and seek need no extra handling. A chord JSON (same shape as
// test/fixtures/training.json) dropped with it, or later, is shown as the
// expected chords and graded against the detected ones.

import { chordKey, grade, segmentAt } from './chords.js';
import { buildExport } from './export.js';

const SAMPLE_RATE = 44100;
// Left margin of every canvas, where bin labels go.
const GUTTER = 72;
// Width of each prerendered heatmap tile; keeps long recordings under the
// browsers' canvas size limits.
const TILE_FRAMES = 4096;
const BAND = 24;
const STRIP = 5;
const TICK_BAND = 14;
// A chord the Viterbi path never takes still gets a salience row when it wins
// at least this many frames on its own.
const MIN_FRAME_WINS = 5;
const MAX_EXTRA_ROWS = 12;
const STORAGE_KEY = 'chordino-inspector:hidden';

const PANELS = [
  {
    id: 'chroma',
    title: 'Chroma',
    note: 'NNLS chroma, treble on top of bass, each half normalized per frame',
    rowHeight: 11,
  },
  {
    id: 'smoothedchroma',
    title: 'Smoothed chroma',
    note: 'EMA the chord matcher actually scores, each half normalized per frame',
    rowHeight: 11,
  },
  {
    id: 'chordsalience',
    title: 'Chord salience',
    note: 'after the confidence gate (Viterbi input), normalized per frame; ▸ = expected chord, blue = expected but never on the path, italic = frame winner the path never takes',
    rowHeight: 14,
  },
  {
    id: 'semitonespectrum',
    title: 'Semitone spectrum',
    note: 'NNLS note activations A0–Ab7, normalized per frame (sqrt)',
    rowHeight: 3,
    gamma: 0.5,
  },
];

const $ = (id) => document.getElementById(id);
const audio = $('audio');
const statusText = $('status');
const progress = $('progress');
const chordText = $('chord');
const timeText = $('time');
const candidates = $('candidates');
const windowSelect = $('window');
const expectedBox = $('expected');
const expectedChord = $('expected-chord');
const scoreText = $('score');
const expectedFile = $('expected-file');
const exportButton = $('export');
const exportStep = $('export-step');

const LUT = buildLut([[68, 1, 84], [59, 82, 139], [33, 145, 140], [94, 201, 98], [253, 231, 37]]);

let view = null;
let audioName = '';
let expected = null;
let worker = null;
let audioUrl = null;
let theme = readTheme();
let windowSec = Number(windowSelect.value);
let lastKey = '';
let lastFrame = -1;

const hidden = new Set(loadHidden());
const lane = createLane();
const panels = PANELS.map(createPanel);

// --- loading -----------------------------------------------------------------

$('file').addEventListener('change', (event) => {
  openFiles([...event.target.files]);
  event.target.value = '';
});

const overlay = $('drop-overlay');
document.addEventListener('dragover', (event) => {
  event.preventDefault();
  overlay.hidden = false;
});
document.addEventListener('dragleave', (event) => {
  if (!event.relatedTarget) {
    overlay.hidden = true;
  }
});
document.addEventListener('drop', (event) => {
  event.preventDefault();
  overlay.hidden = true;
  openFiles([...event.dataTransfer.files]);
});

exportButton.addEventListener('click', () => {
  const text = buildExport({ view, expected, audioName, step: Number(exportStep.value) });
  const link = document.createElement('a');
  link.href = URL.createObjectURL(new Blob([text], { type: 'application/json' }));
  link.download = `${audioName.replace(/\.[^.]+$/, '')}.chordino.json`;
  link.click();
  setTimeout(() => URL.revokeObjectURL(link.href), 0);
});

$('clear-expected').addEventListener('click', () => {
  expected = null;
  expectedFile.hidden = true;
  refreshExpected();
});

/** Audio and chord JSON can come together or one at a time, in any order. */
function openFiles(files) {
  const isJson = (file) => file.type === 'application/json' || file.name.toLowerCase().endsWith('.json');
  const media = files.find((file) => !isJson(file));
  const json = files.find(isJson);
  if (media) {
    load(media);
  }
  if (json) {
    loadExpected(json);
  }
}

async function load(file) {
  worker?.terminate();
  worker = null;
  view = null;
  audioName = file.name;
  exportButton.disabled = true;
  lastKey = '';
  if (audioUrl) {
    URL.revokeObjectURL(audioUrl);
  }
  audioUrl = URL.createObjectURL(file);
  audio.src = audioUrl;
  clearNow();
  for (const panel of [lane, ...panels]) {
    panel.canvas.getContext('2d').clearRect(0, 0, panel.canvas.width, panel.canvas.height);
  }

  const started = performance.now();
  setStatus(`Decoding ${file.name}…`, 0);
  let samples;
  try {
    samples = await decode(file);
  } catch (err) {
    setStatus(`Could not decode ${file.name}: ${err.message}`);
    return;
  }

  const current = worker = new Worker('./analyzer-worker.js');
  current.onmessage = ({ data }) => {
    if (current !== worker) {
      return;
    }
    if (data.type === 'progress') {
      setStatus(`Analyzing ${file.name}… ${Math.round(data.value * 100)}%`, data.value);
    } else if (data.type === 'decoding') {
      setStatus(`Estimating chords for ${file.name}…`, null);
    } else if (data.type === 'done') {
      current.terminate();
      worker = null;
      if (data.analysis.times.length === 0) {
        setStatus(`${file.name} has no audio to analyze`);
        return;
      }
      view = buildView(data.analysis);
      refreshExpected();
      exportButton.disabled = false;
      const seconds = ((performance.now() - started) / 1000).toFixed(1);
      setStatus(`${file.name} · ${formatTime(view.duration)} · ${view.frames} frames · ` +
        `${view.segments.length - 1} chords · analyzed in ${seconds} s`);
    } else if (data.type === 'error') {
      current.terminate();
      worker = null;
      setStatus(`Analysis failed: ${data.message}`);
    }
  };
  current.postMessage({ samples, sampleRate: SAMPLE_RATE }, [samples.buffer]);
}

/** Decodes at the rate the plugin is initialised with and mixes down to mono. */
async function decode(file) {
  const context = new OfflineAudioContext(1, 1, SAMPLE_RATE);
  const buffer = await context.decodeAudioData(await file.arrayBuffer());
  const mono = new Float32Array(buffer.length);
  for (let channel = 0; channel < buffer.numberOfChannels; channel++) {
    const data = buffer.getChannelData(channel);
    for (let i = 0; i < mono.length; i++) {
      mono[i] += data[i] / buffer.numberOfChannels;
    }
  }
  return mono;
}

async function loadExpected(file) {
  expectedFile.hidden = false;
  try {
    expected = { name: file.name, segments: parseExpected(JSON.parse(await file.text())) };
    $('expected-name').textContent = `Expected: ${file.name} (${expected.segments.length} marks)`;
  } catch (err) {
    expected = null;
    $('expected-name').textContent = `Could not read ${file.name}: ${err.message}`;
  }
  refreshExpected();
}

// {"timestamps": [[chord, seconds], ...]} as in test/fixtures/training.json, where
// "" is no chord (it closes the last one); getResult()'s [{chord, time}] works too.
function parseExpected(data) {
  const entries = Array.isArray(data) ? data : data?.timestamps;
  if (!Array.isArray(entries)) {
    throw new Error('expected {"timestamps": [[chord, seconds], ...]}');
  }
  const marks = entries
    .map((entry) => (Array.isArray(entry) ? { chord: entry[0], time: entry[1] } : entry))
    .sort((a, b) => a.time - b.time);
  for (const mark of marks) {
    if (typeof mark?.chord !== 'string' || !Number.isFinite(mark.time)) {
      throw new Error(`bad entry ${JSON.stringify(mark)}`);
    }
  }
  return marks.map((mark, i) => ({
    chord: mark.chord || 'N',
    start: mark.time,
    end: i + 1 < marks.length ? marks[i + 1].time : Infinity,
  }));
}

function setStatus(text, value) {
  statusText.textContent = text;
  progress.hidden = value === undefined;
  if (value === null) {
    progress.removeAttribute('value');
  } else if (value !== undefined) {
    progress.value = value;
  }
}

// --- analysis -> drawable view ---------------------------------------------------

function buildView(analysis) {
  const { times, chords } = analysis;
  const frames = times.length;
  const frameRate = frames > 1 ? (frames - 1) / (times[frames - 1] - times[0]) : SAMPLE_RATE / 2048;
  const duration = frames / frameRate;
  const salience = aggregateSalience(analysis.chordsalience);
  const segments = chords.map((item, i) => ({
    chord: item.chord,
    start: item.time,
    end: i + 1 < chords.length ? chords[i + 1].time : duration,
  }));

  const labelByKey = new Map();
  salience.binNames.forEach((label, i) => {
    const { id } = chordKey(label);
    if (!labelByKey.has(id)) {
      labelByKey.set(id, i);
    }
  });

  // Chord salience is laid out by refreshExpected(), since its rows depend on the expected chords
  applyLayout(panelById('chroma'), chromaLayout(analysis.chroma));
  applyLayout(panelById('smoothedchroma'), chromaLayout(analysis.smoothedchroma));
  applyLayout(panelById('semitonespectrum'), semitoneLayout(analysis.semitonespectrum));
  return {
    frames,
    frameRate,
    duration,
    chords,
    segments,
    salience,
    labelByKey,
    chroma: analysis.chroma,
    templateCount: analysis.chordsalience.binCount,
    comparison: null,
  };
}

/** Grades the detected chords against the expected ones and relays out the salience rows. */
function refreshExpected() {
  lastKey = '';
  lastFrame = -1;
  expectedBox.hidden = !expected;
  if (!view) {
    return;
  }
  view.comparison = expected ? compare(view.segments, expected.segments, view.duration) : null;
  applyLayout(panelById('chordsalience'), salienceLayout(view, expected?.segments ?? []));
  scoreText.textContent = view.comparison
    ? `exact ${formatPercent(view.comparison.exact)} · root ${formatPercent(view.comparison.root)}`
    : '';
}

function panelById(id) {
  return panels.find((panel) => panel.id === id);
}

function applyLayout(panel, layout) {
  panel.rows = layout.rows;
  panel.separators = layout.separators ?? [];
  panel.tiles = buildTiles(layout.bins, layout.rows, layout.groups, panel.gamma ?? 1);
}

/** Overlaps of detected and expected segments, graded, plus time-weighted scores. */
function compare(detected, wanted, duration) {
  const intervals = [];
  const totals = { exact: 0, root: 0, miss: 0 };
  for (const want of wanted) {
    for (const got of detected) {
      const start = Math.max(want.start, got.start);
      const end = Math.min(want.end, got.end, duration);
      if (start < end) {
        const result = grade(got.chord, want.chord);
        intervals.push({ start, end, result, expected: want.chord, detected: got.chord });
        totals[result] += end - start;
      }
    }
  }
  const total = totals.exact + totals.root + totals.miss;
  return {
    intervals,
    exact: total > 0 ? totals.exact / total : 0,
    root: total > 0 ? (totals.exact + totals.root) / total : 0,
  };
}

// Chroma bins are A-based, bass in 0-11 and treble in 12-23. Treble is drawn on
// top and pitch goes up; each half is normalized on its own, as the matcher does.
function chromaLayout(bins) {
  const rows = [];
  for (let bin = 23; bin >= 0; bin--) {
    rows.push({ bin, label: bins.binNames[bin] });
  }
  const groups = new Uint8Array(24).map((_, bin) => (bin < 12 ? 0 : 1));
  return { bins, rows, groups, separators: [12] };
}

function semitoneLayout(bins) {
  const rows = [];
  for (let bin = bins.binCount - 1; bin >= 0; bin--) {
    const label = bins.binNames[bin];
    rows.push({ bin, label: /^C\d/.test(label) ? label : '' });
  }
  return { bins, rows, groups: new Uint8Array(bins.binCount) };
}

// The dictionary holds several templates per label (e.g. C with and without a
// bass note), so salience is summed per label: that is the label's probability.
function aggregateSalience(bins) {
  const labels = [...new Set(bins.binNames)];
  const indexOf = new Map(labels.map((label, i) => [label, i]));
  const labelOfBin = bins.binNames.map((name) => indexOf.get(name));
  const frames = bins.values.length / bins.binCount;
  const values = new Float32Array(frames * labels.length);
  for (let frame = 0; frame < frames; frame++) {
    for (let bin = 0; bin < bins.binCount; bin++) {
      values[frame * labels.length + labelOfBin[bin]] += bins.values[frame * bins.binCount + bin];
    }
  }
  return { binCount: labels.length, binNames: labels, values };
}

// Rows: chords on the Viterbi path in order of appearance, then expected chords
// the path never takes, then labels that win frames on their own, then N.
function salienceLayout({ salience: bins, chords, labelByKey }, wanted) {
  const { binCount, binNames, values } = bins;
  const frames = values.length / binCount;
  const wins = new Map();
  for (let frame = 0; frame < frames; frame++) {
    let best = 0;
    for (let label = 1; label < binCount; label++) {
      if (values[frame * binCount + label] > values[frame * binCount + best]) {
        best = label;
      }
    }
    wins.set(best, (wins.get(best) ?? 0) + 1);
  }
  const onPath = [...new Set(chords.map((item) => item.chord))].filter((chord) => chord !== 'N');
  // Expected chords spelled the way the dictionary does; ones it has no template for are left out
  const expectedOnly = [...new Set(wanted.map((segment) => labelByKey.get(chordKey(segment.chord).id)))]
    .filter((label) => label !== undefined && binNames[label] !== 'N' && !onPath.includes(binNames[label]))
    .map((label) => binNames[label]);
  const extra = [...wins]
    .filter(([label, count]) => count >= MIN_FRAME_WINS && binNames[label] !== 'N' &&
      !onPath.includes(binNames[label]) && !expectedOnly.includes(binNames[label]))
    .sort((a, b) => b[1] - a[1])
    .slice(0, MAX_EXTRA_ROWS)
    .map(([label]) => binNames[label]);
  const rows = [
    ...onPath.map((label) => ({ bin: binNames.indexOf(label), label })),
    ...expectedOnly.map((label) => ({ bin: binNames.indexOf(label), label, expectedOnly: true })),
    ...extra.map((label) => ({ bin: binNames.indexOf(label), label, offPath: true })),
    { bin: binNames.indexOf('N'), label: 'N' },
  ];
  return { bins, rows, groups: new Uint8Array(binCount) };
}

/** Renders the whole recording once, one pixel per frame and row, into canvas tiles. */
function buildTiles({ binCount, values }, rows, groups, gamma) {
  const frames = values.length / binCount;
  const peaks = new Float32Array(Math.max(...groups) + 1);
  const tiles = [];
  for (let start = 0; start < frames; start += TILE_FRAMES) {
    const width = Math.min(TILE_FRAMES, frames - start);
    const canvas = document.createElement('canvas');
    canvas.width = width;
    canvas.height = rows.length;
    const context = canvas.getContext('2d');
    const image = context.createImageData(width, rows.length);
    for (let x = 0; x < width; x++) {
      const offset = (start + x) * binCount;
      peaks.fill(0);
      for (let bin = 0; bin < binCount; bin++) {
        peaks[groups[bin]] = Math.max(peaks[groups[bin]], values[offset + bin]);
      }
      rows.forEach((row, y) => {
        const peak = peaks[groups[row.bin]];
        const level = peak > 0 ? (values[offset + row.bin] / peak) ** gamma : 0;
        const color = Math.round(level * 255) * 3;
        const pixel = (y * width + x) * 4;
        image.data[pixel] = LUT[color];
        image.data[pixel + 1] = LUT[color + 1];
        image.data[pixel + 2] = LUT[color + 2];
        image.data[pixel + 3] = 255;
      });
    }
    context.putImageData(image, 0, 0);
    tiles.push({ canvas, start });
  }
  return tiles;
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

// --- DOM -----------------------------------------------------------------------

function createLane() {
  const section = document.createElement('section');
  section.className = 'panel';
  section.innerHTML = '<h2>Chords <small>Viterbi path, and the expected chords when a JSON is loaded; ' +
    'click any panel to seek</small></h2>';
  const canvas = document.createElement('canvas');
  section.append(canvas);
  $('panels').append(section);
  addSeek(canvas);
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
  addSeek(canvas);

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

  return { ...config, section, canvas, rows: [], separators: [], tiles: [] };
}

function addSeek(canvas) {
  canvas.addEventListener('click', (event) => {
    const rect = canvas.getBoundingClientRect();
    const x = event.clientX - rect.left;
    if (!view || x < GUTTER) {
      return;
    }
    const plotWidth = rect.width - GUTTER;
    const time = audio.currentTime + ((x - GUTTER - plotWidth / 2) / plotWidth) * windowSec;
    audio.currentTime = Math.min(Math.max(time, 0), view.duration);
  });
}

windowSelect.addEventListener('change', () => {
  windowSec = Number(windowSelect.value);
  lastKey = '';
});

document.addEventListener('keydown', (event) => {
  if (event.key === ' ' && event.target === document.body && audio.src) {
    event.preventDefault();
    if (audio.paused) {
      audio.play();
    } else {
      audio.pause();
    }
  }
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
    exact: read('--ok'),
    root: read('--warn'),
    miss: read('--bad'),
    laneA: read('--lane-a'),
    laneB: read('--lane-b'),
    empty: read('--plot-empty'),
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
  if (!view) {
    return;
  }
  const time = audio.currentTime;
  const key = `${time}|${windowSec}|${document.documentElement.clientWidth}|${devicePixelRatio}`;
  if (key === lastKey) {
    return;
  }
  lastKey = key;
  drawLane(time);
  for (const panel of panels) {
    if (!panel.section.hidden) {
      drawPanel(panel, time);
    }
  }
  updateNow(time);
});

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

/** Maps time to x for a window centred on `time`. */
function timeScale(area, time) {
  const pxPerSec = area.plotWidth / windowSec;
  const start = time - windowSec / 2;
  return { pxPerSec, start, x: (t) => GUTTER + (t - start) * pxPerSec };
}

function drawLane(time) {
  const { comparison } = view;
  const bands = comparison ? 2 * BAND + STRIP : BAND;
  const area = fit(lane.canvas, bands + TICK_BAND);
  const { context, width, height } = area;
  const { pxPerSec, start, x } = timeScale(area, time);

  context.save();
  context.beginPath();
  context.rect(GUTTER, 0, area.plotWidth, height);
  context.clip();
  drawSegments(context, view.segments, 0, x, time, width);
  if (comparison) {
    for (const interval of comparison.intervals) {
      const x0 = x(interval.start);
      const x1 = x(interval.end);
      if (x1 >= GUTTER && x0 <= width) {
        context.fillStyle = theme[interval.result];
        context.fillRect(x0, BAND + 1, x1 - x0, STRIP - 2);
      }
    }
    drawSegments(context, expected.segments, BAND + STRIP, x, time, width);
  }

  context.font = '10px system-ui, sans-serif';
  context.textBaseline = 'middle';
  context.fillStyle = theme.muted;
  const labelEvery = pxPerSec >= 60 ? 1 : pxPerSec >= 25 ? 2 : 5;
  for (let second = Math.ceil(Math.max(start, 0)); second <= Math.min(start + windowSec, view.duration); second++) {
    context.fillRect(x(second), bands, 1, second % labelEvery === 0 ? 6 : 3);
    if (second % labelEvery === 0) {
      context.fillText(formatTime(second, 0), x(second) + 3, bands + 9);
    }
  }
  context.restore();

  context.font = '11px system-ui, sans-serif';
  context.fillStyle = theme.muted;
  context.textAlign = 'right';
  context.fillText(comparison ? 'detected' : 'chord', GUTTER - 6, BAND / 2);
  if (comparison) {
    context.fillText('expected', GUTTER - 6, BAND + STRIP + BAND / 2);
  }
  context.textAlign = 'left';
  drawPlayhead(context, area);
}

function drawSegments(context, segments, top, x, time, width) {
  context.font = '600 13px system-ui, sans-serif';
  context.textBaseline = 'middle';
  segments.forEach((segment, i) => {
    const end = Math.min(segment.end, view.duration);
    const x0 = x(segment.start);
    const x1 = x(end);
    if (x1 <= x0 || x1 < GUTTER || x0 > width) {
      return;
    }
    const current = segment.start <= time && time < end;
    context.fillStyle = current ? theme.accentSoft : i % 2 ? theme.laneB : theme.laneA;
    context.fillRect(x0, top, x1 - x0, BAND);
    const label = segment.chord;
    const labelWidth = context.measureText(label).width;
    // Keep the label of a chord that started off-screen pinned to the left edge
    const labelX = Math.min(Math.max(x0, GUTTER) + 4, x1 - labelWidth - 4);
    if (labelX >= Math.max(x0, GUTTER) + 2) {
      context.fillStyle = label === 'N' ? theme.muted : theme.fg;
      context.fillText(label, labelX, top + BAND / 2);
    }
  });
}

function drawPanel(panel, time) {
  const area = fit(panel.canvas, panel.rows.length * panel.rowHeight);
  const { context, height } = area;
  const { pxPerSec, start } = timeScale(area, time);

  context.fillStyle = theme.empty;
  context.fillRect(GUTTER, 0, area.plotWidth, height);
  const end = start + windowSec;
  for (const tile of panel.tiles) {
    const tileStart = tile.start / view.frameRate;
    const tileEnd = (tile.start + tile.canvas.width) / view.frameRate;
    const from = Math.max(start, tileStart);
    const to = Math.min(end, tileEnd);
    if (from >= to) {
      continue;
    }
    context.drawImage(
      tile.canvas,
      (from - tileStart) * view.frameRate, 0, (to - from) * view.frameRate, tile.canvas.height,
      GUTTER + (from - start) * pxPerSec, 0, (to - from) * pxPerSec, height,
    );
  }

  context.fillStyle = theme.empty;
  for (const row of panel.separators) {
    context.fillRect(GUTTER, row * panel.rowHeight - 1, area.plotWidth, 2);
  }

  context.textAlign = 'right';
  context.textBaseline = 'middle';
  const fontSize = Math.min(11, Math.max(9, panel.rowHeight - 2));
  const marked = panel.id === 'chordsalience' ? expectedLabelAt(time) : undefined;
  panel.rows.forEach((row, y) => {
    if (!row.label) {
      return;
    }
    const middle = (y + 0.5) * panel.rowHeight;
    context.font = `${row.offPath ? 'italic ' : ''}${row.bin === marked ? '700 ' : ''}${fontSize}px system-ui, sans-serif`;
    context.fillStyle = row.expectedOnly ? theme.accent : row.offPath || row.label === 'N' ? theme.muted : theme.fg;
    context.fillText(row.label, GUTTER - 6, middle);
    if (row.bin === marked) {
      context.fillStyle = theme.accent;
      context.fillText('▸', 12, middle);
    }
  });
  context.textAlign = 'left';
  drawPlayhead(context, area);
}

function drawPlayhead(context, area) {
  context.fillStyle = theme.playhead;
  context.fillRect(GUTTER + area.plotWidth / 2 - 0.75, 0, 1.5, area.height);
}

// --- readout ---------------------------------------------------------------------

function expectedAt(time) {
  return expected && segmentAt(expected.segments, time);
}

/** Salience label of the expected chord at `time`, if the dictionary has it. */
function expectedLabelAt(time) {
  const segment = expectedAt(time);
  return segment && view.labelByKey.get(chordKey(segment.chord).id);
}

function updateNow(time) {
  timeText.textContent = `${formatTime(time)} / ${formatTime(view.duration)}`;
  const segment = view.segments.findLast((item) => item.start <= time);
  chordText.textContent = segment ? segment.chord : '–';
  if (expected) {
    const want = expectedAt(time);
    expectedChord.textContent = want ? want.chord : '–';
    expectedChord.className = want && segment ? grade(segment.chord, want.chord) : '';
  }

  const frame = Math.min(Math.floor(time * view.frameRate), view.frames - 1);
  if (frame === lastFrame) {
    return;
  }
  lastFrame = frame;
  const { binCount, binNames, values } = view.salience;
  const row = values.subarray(frame * binCount, (frame + 1) * binCount);
  const top = [...row.keys()].sort((a, b) => row[b] - row[a]).slice(0, 5);
  // Salience is spread over ~250 labels, so bars are scaled to the winner
  const best = row[top[0]] || 1;
  candidates.replaceChildren(...top.map((label) => {
    const item = document.createElement('li');
    const name = document.createElement('span');
    name.textContent = binNames[label];
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

function clearNow() {
  chordText.textContent = '–';
  expectedChord.textContent = '–';
  expectedChord.className = '';
  scoreText.textContent = '';
  timeText.textContent = '';
  candidates.replaceChildren();
  lastFrame = -1;
}

function formatPercent(value) {
  return `${(value * 100).toFixed(1)}%`;
}

function formatTime(seconds, digits = 1) {
  const minutes = Math.floor(seconds / 60);
  const rest = seconds - minutes * 60;
  return `${minutes}:${rest.toFixed(digits).padStart(digits ? digits + 3 : 2, '0')}`;
}
