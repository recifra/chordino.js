import { useChordDetection, useLiveChords } from './chordino';

export default function App() {
  return (
    <main>
      <h1>chordino.js with React and Vite</h1>
      <Recording />
      <Microphone />
    </main>
  );
}

function Recording() {
  const { state, detect } = useChordDetection();
  return (
    <section>
      <h2>Chords of a recording</h2>
      <input
        type="file"
        accept="audio/*"
        onChange={(event) => {
          const file = event.target.files?.[0];
          if (file) {
            void detect(file);
          }
        }}
      />
      {state.status === 'decoding' && <p>Decoding…</p>}
      {state.status === 'detecting' && <progress value={state.progress} />}
      {state.status === 'error' && <p role="alert">{state.message}</p>}
      {state.status === 'done' && (
        <ol id="timestamps">
          {state.timestamps.map(([chord, seconds]) => (
            <li key={seconds}>
              {seconds.toFixed(2)} s: {chord || 'no chord'}
            </li>
          ))}
        </ol>
      )}
    </section>
  );
}

function Microphone() {
  const { state, start, stop } = useLiveChords();
  const active = state.status === 'starting' || state.status === 'listening';
  return (
    <section>
      <h2>Chords from the microphone</h2>
      <button id="listen" disabled={state.status === 'starting'} onClick={() => void (active ? stop() : start())}>
        {active ? 'Stop' : 'Listen'}
      </button>
      {state.status === 'starting' && <p>Starting…</p>}
      {state.status === 'listening' && <p id="chord">{state.chord ?? 'Play something'}</p>}
      {state.status === 'error' && <p role="alert">{state.message}</p>}
    </section>
  );
}
