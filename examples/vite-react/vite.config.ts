import react from '@vitejs/plugin-react';
import { defineConfig } from 'vite';

export default defineConfig({
  plugins: [react()],
  // ES module chunks for the worker and the worklet. The default, iife, swaps
  // import.meta.url for self.location, which an AudioWorklet does not have.
  worker: { format: 'es' },
});
