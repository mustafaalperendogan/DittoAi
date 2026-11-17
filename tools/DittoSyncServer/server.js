// Minimal Ditto cloud sync server, for local testing and as a reference for the API contract (README.md).
//
//   node server.js --port 8787 --data ./data [--host 127.0.0.1]
//   API keys: environment variable DITTO_SYNC_KEYS (comma separated). Empty = no authorization check.
//
// Stores one opaque (already encrypted) package per URL path. The server never sees clip contents.

'use strict';

const http = require('http');
const fs = require('fs');
const path = require('path');
const crypto = require('crypto');

function arg(name, def) {
  const i = process.argv.indexOf('--' + name);
  return i > 0 && i + 1 < process.argv.length ? process.argv[i + 1] : def;
}

const port = parseInt(arg('port', '8787'), 10);
const host = arg('host', '127.0.0.1');
const dataDir = path.resolve(arg('data', path.join(__dirname, 'data')));
const maxBytes = parseInt(arg('max-mb', '200'), 10) * 1024 * 1024;
const keys = (process.env.DITTO_SYNC_KEYS || '').split(',').map(k => k.trim()).filter(Boolean);

fs.mkdirSync(dataDir, { recursive: true });

function fileFor(urlPath) {
  return path.join(dataDir, crypto.createHash('sha256').update(urlPath).digest('hex') + '.json');
}

function etagOf(buffer) {
  return '"' + crypto.createHash('sha256').update(buffer).digest('hex').slice(0, 32) + '"';
}

function send(res, status, body, headers) {
  const data = Buffer.from(body === undefined ? '' : JSON.stringify(body));
  res.writeHead(status, Object.assign({ 'Content-Type': 'application/json', 'Content-Length': data.length }, headers || {}));
  res.end(data);
}

function authorized(req) {
  if (keys.length === 0) return true;
  const header = req.headers['authorization'] || '';
  const match = /^Bearer\s+(.+)$/i.exec(header);
  if (!match) return false;
  const given = Buffer.from(match[1]);
  return keys.some(k => {
    const expected = Buffer.from(k);
    return expected.length === given.length && crypto.timingSafeEqual(expected, given);
  });
}

// one request at a time per path: the If-Match check and the write must not interleave
const locks = new Map();
function withLock(key, fn) {
  const previous = locks.get(key) || Promise.resolve();
  const next = previous.then(fn, fn);
  locks.set(key, next.catch(() => {}));
  return next;
}

const server = http.createServer((req, res) => {
  const urlPath = decodeURIComponent(new URL(req.url, 'http://x').pathname);
  const started = Date.now();
  res.on('finish', () => console.log(new Date().toISOString(), req.method, urlPath, res.statusCode, (Date.now() - started) + 'ms'));

  if (!authorized(req)) return send(res, 401, { error: 'invalid api key' });

  const file = fileFor(urlPath);

  if (req.method === 'GET') {
    return withLock(file, () => {
      if (!fs.existsSync(file)) return send(res, 404, { error: 'no package yet' });
      const data = fs.readFileSync(file);
      res.writeHead(200, { 'Content-Type': 'application/json', 'Content-Length': data.length, 'ETag': etagOf(data) });
      res.end(data);
    });
  }

  if (req.method === 'PUT') {
    const chunks = [];
    let size = 0;
    let tooBig = false;
    req.on('data', chunk => {
      size += chunk.length;
      if (size > maxBytes) tooBig = true; else chunks.push(chunk);
    });
    req.on('end', () => withLock(file, () => {
      if (tooBig) return send(res, 413, { error: 'package too large' });
      const body = Buffer.concat(chunks);
      try {
        const json = JSON.parse(body.toString('utf8'));
        if (json.format !== 'ditto-sync') return send(res, 400, { error: 'not a ditto-sync package' });
      } catch (e) {
        return send(res, 400, { error: 'invalid json' });
      }

      const exists = fs.existsSync(file);
      const ifMatch = req.headers['if-match'];
      const ifNoneMatch = req.headers['if-none-match'];
      if (ifNoneMatch === '*' && exists) return send(res, 412, { error: 'package already exists' });
      if (ifMatch !== undefined && (!exists || etagOf(fs.readFileSync(file)) !== ifMatch)) return send(res, 412, { error: 'package changed' });
      if (ifMatch === undefined && ifNoneMatch === undefined) return send(res, 428, { error: 'If-Match or If-None-Match required' });

      const temp = file + '.tmp';
      fs.writeFileSync(temp, body);
      fs.renameSync(temp, file);
      send(res, exists ? 200 : 201, { ok: true }, { 'ETag': etagOf(body) });
    }));
    return;
  }

  send(res, 405, { error: 'method not allowed' }, { 'Allow': 'GET, PUT' });
});

server.listen(port, host, () => {
  console.log('Ditto sync test server on http://' + host + ':' + port + '/  data: ' + dataDir + '  keys: ' + (keys.length ? keys.length : 'none (no auth)'));
});
