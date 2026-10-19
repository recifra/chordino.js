// Serves examples/ over https, so that a phone or tablet on the same network
// gets the microphone: browsers only give it to https and localhost pages. The
// certificate is self-signed, made with openssl on the first run and kept in
// .cert/, so each device asks once to go on past the warning. The files go out
// as they are, build/ through the examples/lib symlink included.
//
//   npm run examples              # https://<this computer's IP>:8000/tuner/
//   PORT=8443 npm run examples

import { execFileSync } from 'node:child_process';
import { createReadStream, existsSync, mkdirSync, readdirSync, readFileSync, statSync } from 'node:fs';
import { createServer } from 'node:https';
import { networkInterfaces } from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const root = path.dirname(fileURLToPath(import.meta.url));
const certDir = path.join(root, '..', '.cert');
const port = Number(process.env.PORT) || 8000;
const TYPES = {
  '.html': 'text/html; charset=utf-8',
  '.js': 'text/javascript',
  '.mjs': 'text/javascript',
  '.css': 'text/css',
  '.json': 'application/json',
  '.wasm': 'application/wasm',
  '.mp3': 'audio/mpeg',
  '.png': 'image/png',
};

const addresses = Object.values(networkInterfaces())
  .flat()
  .filter((address) => address.family === 'IPv4' && !address.internal)
  .map((address) => address.address);

function certificate() {
  const key = path.join(certDir, 'key.pem');
  const cert = path.join(certDir, 'cert.pem');
  if (!existsSync(cert)) {
    mkdirSync(certDir, { recursive: true });
    const names = ['DNS:localhost', 'IP:127.0.0.1', ...addresses.map((address) => `IP:${address}`)];
    execFileSync('openssl', [
      'req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-days', '825',
      '-subj', '/CN=chordino.js examples',
      '-addext', `subjectAltName=${names.join(',')}`,
      '-keyout', key, '-out', cert,
    ], { stdio: 'ignore' });
  }
  return { key: readFileSync(key), cert: readFileSync(cert) };
}

/** A page of links to what dir holds, for the directories without an index.html */
function listing(dir, pathname) {
  const links = readdirSync(dir, { withFileTypes: true })
    .filter((entry) => !entry.name.startsWith('.'))
    .map((entry) => {
      const name = entry.name + (statSync(path.join(dir, entry.name)).isDirectory() ? '/' : '');
      return `<li><a href="${encodeURI(name)}">${name}</a></li>`;
    });
  return `<!doctype html><meta charset="utf-8"><title>${pathname}</title><ul>${links.join('')}</ul>`;
}

createServer(certificate(), (request, response) => {
  const { pathname } = new URL(request.url, 'https://localhost');
  let file = path.join(root, decodeURIComponent(pathname));
  if (file !== root && !file.startsWith(root + path.sep)) {
    response.writeHead(403).end();
    return;
  }
  try {
    if (statSync(file).isDirectory()) {
      if (!pathname.endsWith('/')) {
        response.writeHead(301, { Location: `${pathname}/` }).end();
        return;
      }
      if (!existsSync(path.join(file, 'index.html'))) {
        response.writeHead(200, { 'Content-Type': TYPES['.html'] }).end(listing(file, pathname));
        return;
      }
      file = path.join(file, 'index.html');
    }
    statSync(file);
  } catch {
    response.writeHead(404).end();
    return;
  }
  // No caching, so a rebuild shows on the next reload
  response.writeHead(200, {
    'Content-Type': TYPES[path.extname(file)] ?? 'application/octet-stream',
    'Cache-Control': 'no-store',
  });
  createReadStream(file).pipe(response);
}).listen(port, () => {
  for (const host of ['localhost', ...addresses]) {
    console.log(`https://${host}:${port}/`);
  }
});
