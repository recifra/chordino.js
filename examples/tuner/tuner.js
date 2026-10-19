// A tuner run the way an app runs one: the microphone goes through the band
// filters into chord-processor.mjs, in buffers of 2048, and on to
// tuner-live-worker.mjs, which posts every steady pitch and, with clockReports,
// the clock ratio. The note and the cents are worked out here, from A4 = 440 Hz.

// Where chord-processor.mjs, tuner-live-worker.mjs, chordino.mjs and
// chordino.wasm live
const LIB = new URL('../lib/', location.href);
const A4 = 440;
const NOTE_NAMES = ['C', 'C♯', 'D', 'D♯', 'E', 'F', 'F♯', 'G', 'G♯', 'A', 'A♯', 'B'];
// Half the worker's 4096-sample window, so the windows overlap by half
const BUFFER_SIZE = 2048;
// With no reading for this long, the one shown greys out
const STALE_MS = 1000;
const STRIP_SECONDS = 10;
const IN_TUNE_CENTS = 5;
const CLOSE_CENTS = 15;
// app-recifra's bands per instrument: highpass and lowpass cutoffs, and high
// shelves as [frequency, dB] that lift the quiet top of the piano over the
// worker's RMS gate
const BANDS = {
  off: null,
  guitar: { min: 70, max: 1200, shelves: [] },
  bass: { min: 35, max: 500, shelves: [] },
  keyboard: { min: 27.5, max: 4186, shelves: [[700, 14], [2000, 14]] },
};

const $ = (id) => document.getElementById(id);
const listenButton = $('listen');
const referenceButton = $('reference');
const bandSelect = $('band');
const statusEl = $('status');
const clockEl = $('clock');
const readoutEl = $('readout');
const noteEl = $('note');
const centsEl = $('cents');
const needleEl = $('needle');
const frequencyEl = $('frequency');
const strip = $('strip');

let session = null;
let reference = null;
let stoppedAt = 0;
// Every reading of the last STRIP_SECONDS: AudioContext time, cents and note
let readings = [];
let readingCount = 0;
let shown = null;
let lastPitchAt = -Infinity;
let theme = readTheme();

matchMedia('(prefers-color-scheme: dark)').addEventListener('change', () => {
  theme = readTheme();
});

// --- listening -------------------------------------------------------------------

listenButton.addEventListener('click', () => {
  if (session) {
    stop();
  } else {
    start();
  }
});

bandSelect.addEventListener('change', () => {
  if (session) {
    connectBand(session);
  }
});

async function start() {
  listenButton.disabled = true;
  setStatus('Asking for the microphone…');
  let stream;
  let context;
  try {
    stream = await navigator.mediaDevices.getUserMedia({
      audio: { echoCancellation: false, noiseSuppression: false, autoGainControl: false },
    });
    // A tuner needs no low latency, and bigger buffers give the audio thread
    // room to fall behind under load without losing samples
    context = new AudioContext({ latencyHint: 'playback' });
    if (context.state === 'suspended') {
      context.resume();
    }
    await context.audioWorklet.addModule(new URL('chord-processor.mjs', LIB));
    const worklet = new AudioWorkletNode(context, 'chord-processor', {
      channelCount: 1,
      channelCountMode: 'explicit',
      channelInterpretation: 'discrete',
      processorOptions: { bufferSize: BUFFER_SIZE },
    });
    // It loads chordino.wasm from next to chordino.mjs
    const worker = new Worker(new URL('tuner-live-worker.mjs', LIB), { type: 'module' });
    const source = context.createMediaStreamSource(stream);

    session = { stream, context, source, worklet, worker, filters: [] };
    readings = [];
    readingCount = 0;
    shown = null;
    worker.onmessage = ({ data }) => onMessage(data);
    worker.onerror = (event) => {
      event.preventDefault();
      setStatus(`The worker failed: ${event.message || 'could not run tuner-live-worker.mjs'}`);
    };

    // The audio goes from the worklet straight to the worker, not through this thread
    const { port1, port2 } = new MessageChannel();
    worker.postMessage({ type: 'start', sampleRate: context.sampleRate, port: port1, clockReports: true }, [port1]);
    worklet.port.postMessage({ type: 'port', port: port2 }, [port2]);
    connectBand(session);
    worklet.connect(context.destination);

    listenButton.textContent = 'Stop listening';
    referenceButton.disabled = false;
    clockEl.textContent = 'clock –';
    clockEl.className = 'caption';
    setStatus(`Loading chordino.wasm… (${context.sampleRate} Hz)`);
  } catch (err) {
    stream?.getTracks().forEach((track) => track.stop());
    context?.close();
    session = null;
    setStatus(`Could not listen: ${err.message}`);
  }
  listenButton.disabled = false;
}

async function stop() {
  const current = session;
  session = null;
  stoppedAt = current.context.currentTime;
  listenButton.disabled = true;
  if (reference) {
    stopReference();
  }
  referenceButton.disabled = true;
  current.stream.getTracks().forEach((track) => track.stop());
  current.worklet.port.postMessage({ type: 'stop' });
  // The detector goes with the worker
  current.worker.terminate();
  await current.context.close();
  listenButton.textContent = 'Start listening';
  listenButton.disabled = false;
  setStatus(`Stopped · ${readingCount} readings`);
}

/** Microphone → the band's filters, if any → the worklet */
function connectBand(current) {
  const { context, source, worklet } = current;
  source.disconnect();
  current.filters.forEach((filter) => filter.disconnect());
  const band = BANDS[bandSelect.value];
  current.filters = band
    ? [
      biquad(context, 'highpass', band.min),
      biquad(context, 'lowpass', band.max),
      ...band.shelves.map(([frequency, gain]) => biquad(context, 'highshelf', frequency, gain)),
    ]
    : [];
  [source, ...current.filters].reduce((from, to) => from.connect(to)).connect(worklet);
}

function biquad(context, type, frequency, gain = 0) {
  return new BiquadFilterNode(context, { type, frequency, gain });
}

function onMessage(data) {
  if (data.type === 'ready') {
    setStatus('Listening…');
  } else if (data.type === 'pitch') {
    showPitch(data.frequency, data.time);
  } else if (data.type === 'clock') {
    showClock(data.ratio);
  } else if (data.type === 'error') {
    setStatus(`Could not load chordino.wasm: ${data.message}`);
  }
}

// --- reference tone --------------------------------------------------------------

referenceButton.addEventListener('click', () => {
  if (reference) {
    stopReference();
  } else {
    startReference();
  }
});

/**
 * An oscillator at the note shown (A4 before any), in an AudioContext of its
 * own, as app-recifra plays its reference note through a synth in another
 * context. The microphone hears it too, and should read it in tune.
 */
function startReference() {
  const midi = shown?.midi ?? 69;
  const context = new AudioContext();
  const oscillator = new OscillatorNode(context, { type: 'triangle', frequency: noteFrequency(midi) });
  oscillator.connect(new GainNode(context, { gain: 0.2 })).connect(context.destination);
  oscillator.start();
  reference = { context };
  referenceButton.setAttribute('aria-pressed', 'true');
  referenceButton.textContent = `Reference ${noteName(midi)} ${noteFrequency(midi).toFixed(1)} Hz`;
}

function stopReference() {
  reference.context.close();
  reference = null;
  referenceButton.setAttribute('aria-pressed', 'false');
  referenceButton.textContent = 'Reference tone';
}

// --- readout ---------------------------------------------------------------------

const noteFrequency = (midi) => A4 * 2 ** ((midi - 69) / 12);
const noteName = (midi) => `${NOTE_NAMES[midi % 12]}${Math.floor(midi / 12) - 1}`;

function tuneClass(cents) {
  const off = Math.abs(cents);
  return off <= IN_TUNE_CENTS ? 'in-tune' : off <= CLOSE_CENTS ? 'close' : 'off';
}

function showPitch(frequency, time) {
  const midi = Math.round(69 + 12 * Math.log2(frequency / A4));
  const cents = 1200 * Math.log2(frequency / noteFrequency(midi));
  shown = { midi, cents };
  lastPitchAt = performance.now();
  readingCount++;
  readings.push({ time, cents, midi });

  const sub = document.createElement('sub');
  sub.textContent = String(Math.floor(midi / 12) - 1);
  noteEl.replaceChildren(NOTE_NAMES[midi % 12], sub);
  centsEl.textContent = `${cents < 0 ? '−' : '+'}${Math.abs(cents).toFixed(1)} ¢`;
  centsEl.className = tuneClass(cents);
  needleEl.style.left = `${50 + Math.max(-50, Math.min(50, cents))}%`;
  frequencyEl.textContent = `${frequency.toFixed(1)} Hz · ${noteName(midi)} is ${noteFrequency(midi).toFixed(1)} Hz`;
  readoutEl.classList.remove('stale');
}

function showClock(ratio) {
  clockEl.textContent = `clock ${ratio.toFixed(3)}`;
  clockEl.className = `caption ${ratio >= 0.995 ? 'in-tune' : ratio >= 0.98 ? 'close' : 'off'}`;
}

function setStatus(text) {
  statusEl.textContent = text;
}

// --- strip -----------------------------------------------------------------------

function readTheme() {
  const style = getComputedStyle(document.documentElement);
  const color = (name) => style.getPropertyValue(name).trim();
  return {
    fg: color('--fg'),
    muted: color('--muted'),
    border: color('--border'),
    ok: color('--ok'),
    warn: color('--warn'),
    bad: color('--bad'),
  };
}

function draw() {
  requestAnimationFrame(draw);
  if (performance.now() - lastPitchAt > STALE_MS) {
    readoutEl.classList.add('stale');
  }

  const ratio = devicePixelRatio || 1;
  const width = strip.clientWidth;
  const height = strip.clientHeight;
  if (strip.width !== Math.round(width * ratio) || strip.height !== Math.round(height * ratio)) {
    strip.width = Math.round(width * ratio);
    strip.height = Math.round(height * ratio);
  }
  const g = strip.getContext('2d');
  g.setTransform(ratio, 0, 0, ratio, 0, 0);
  g.clearRect(0, 0, width, height);

  const label = 28;
  const plot = width - label;
  const y = (cents) => height / 2 - (Math.max(-50, Math.min(50, cents)) / 50) * (height / 2 - 8);
  g.globalAlpha = 0.15;
  g.fillStyle = theme.ok;
  g.fillRect(label, y(IN_TUNE_CENTS), plot, y(-IN_TUNE_CENTS) - y(IN_TUNE_CENTS));
  g.globalAlpha = 1;
  g.font = '10px system-ui, sans-serif';
  g.textBaseline = 'middle';
  for (const cents of [50, 25, 0, -25, -50]) {
    g.fillStyle = theme.border;
    g.fillRect(label, Math.round(y(cents)), plot, cents === 0 ? 1 : 0.5);
    g.fillStyle = theme.muted;
    g.fillText(cents > 0 ? `+${cents}` : cents < 0 ? `−${-cents}` : '0', 2, y(cents));
  }

  const now = session ? session.context.currentTime : stoppedAt;
  readings = readings.filter((reading) => reading.time > now - STRIP_SECONDS);
  const x = (time) => label + plot - ((now - time) / STRIP_SECONDS) * plot;
  g.textBaseline = 'top';
  let lastMidi = null;
  for (const { time, cents, midi } of readings) {
    if (midi !== lastMidi) {
      g.fillStyle = theme.muted;
      g.fillText(noteName(midi), x(time) + 3, 2);
      lastMidi = midi;
    }
    const tune = tuneClass(cents);
    g.fillStyle = tune === 'in-tune' ? theme.ok : tune === 'close' ? theme.warn : theme.bad;
    g.beginPath();
    g.arc(x(time), y(cents), 2.5, 0, 2 * Math.PI);
    g.fill();
  }
}

requestAnimationFrame(draw);
