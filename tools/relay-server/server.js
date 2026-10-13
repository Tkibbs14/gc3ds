#!/usr/bin/env node
// gc3ds reference relay: lets a New 3DS / New 2DS XL running gc3ds "Remote runs" (core/host3ds/relay.c) take jobs
// queued from a PC (core/tools/relay.sh), from anywhere. Same HTTP protocol as docs/REMOTE_RUNS.md; no dependencies,
// state in a directory on disk. Put it behind HTTPS (a reverse proxy such as Caddy) when it is reachable from the
// internet: the token travels in a header.
//
//   RELAY_TOKEN=<64 hex chars> node tools/relay-server/server.js [--port 8787] [--data ./relay-data]
//
// Client settings (core/host3ds/relay.txt on the PC, sdmc:/gc3ds/relay.txt on the console):
//   url https://your-host/        (the base: the routes below are appended to it)
//   token <the same RELAY_TOKEN>
//   auto 1                        (console only: enter Remote runs at launch)
'use strict';
const http = require('node:http');
const fs = require('node:fs');
const path = require('node:path');
const zlib = require('node:zlib');
const crypto = require('node:crypto');

const arg = (name, def) => { const i = process.argv.indexOf(name); return i > 0 ? process.argv[i + 1] : def; };
const PORT = parseInt(arg('--port', process.env.PORT || '8787'), 10);
const DATA = path.resolve(arg('--data', process.env.RELAY_DATA || './relay-data'));
const TOKEN = process.env.RELAY_TOKEN || '';
if (TOKEN.length < 32) { console.error('set RELAY_TOKEN (at least 32 characters, e.g. `openssl rand -hex 32`)'); process.exit(1); }
const TOKEN_HASH = crypto.createHash('sha256').update(TOKEN).digest();

for (const d of ['builds', 'logs', 'frames']) fs.mkdirSync(path.join(DATA, d), { recursive: true });
const JOBS = path.join(DATA, 'jobs.json');
let jobs = fs.existsSync(JOBS) ? JSON.parse(fs.readFileSync(JOBS, 'utf8')) : [];
const saveJobs = () => fs.writeFileSync(JOBS, JSON.stringify(jobs, null, 1));
const nextId = () => jobs.reduce((m, j) => Math.max(m, j.id), 0) + 1;
const pendingCmds = new Map();  // job id -> [command text], taken by the console's next log post
const buildCache = new Map();   // id -> { raw, gz }

const text = (res, code, body, headers = {}) => { res.writeHead(code, { 'Content-Type': 'text/plain', ...headers }); res.end(body); };
const now = () => new Date().toISOString();

function authorized(req) {
    const t = req.headers['x-relay-token'] || '';
    const h = crypto.createHash('sha256').update(String(t)).digest();
    return crypto.timingSafeEqual(h, TOKEN_HASH);
}

function body(req, limit) {
    return new Promise((resolve, reject) => {
        const parts = [];
        let n = 0;
        req.on('data', c => { n += c.length; if (n > limit) { reject(new Error('too large')); req.destroy(); } else parts.push(c); });
        req.on('end', () => resolve(Buffer.concat(parts)));
        req.on('error', reject);
    });
}

function loadBuild(id) {
    let b = buildCache.get(id);
    if (b) return b;
    const f = path.join(DATA, 'builds', `${id}.3dsx`);
    if (!fs.existsSync(f)) return null;
    const raw = fs.readFileSync(f);
    b = { raw, gz: zlib.gzipSync(raw, { level: 9 }) };
    buildCache.set(id, b);
    while (buildCache.size > 4) buildCache.delete(buildCache.keys().next().value);
    return b;
}

const routes = [
    // PC: upload a build (the .3dsx); its id is the first 16 hex digits of its sha256.
    ['PUT', /^\/build$/, async (req, res, m, q) => {
        const buf = await body(req, 32 << 20);
        if (buf.length < 1024) return text(res, 400, 'error empty build\n');
        const id = crypto.createHash('sha256').update(buf).digest('hex').slice(0, 16);
        fs.writeFileSync(path.join(DATA, 'builds', `${id}.3dsx`), buf);
        fs.writeFileSync(path.join(DATA, 'builds', `${id}.note`), String(q.get('note') || '').slice(0, 200));
        text(res, 200, `build ${id}\nbytes ${buf.length}\n`);
    }],
    // Console: download a build, gzipped when the client accepts it.
    ['GET', /^\/build\/([0-9a-f]{16})$/, async (req, res, m) => {
        const b = loadBuild(m[1]);
        if (!b) return text(res, 404, 'error no such build\n');
        const gz = /\bgzip\b/.test(req.headers['accept-encoding'] || '');
        const out = gz ? b.gz : b.raw;
        res.writeHead(200, { 'Content-Type': 'application/octet-stream', 'Content-Length': out.length,
                             'X-Raw-Length': b.raw.length, ...(gz ? { 'Content-Encoding': 'gzip' } : {}) });
        res.end(out);
    }],
    // PC: queue a job. Body: {"args": "--game Twilight --seconds 300", "build": "<id>"}.
    ['POST', /^\/jobs$/, async (req, res) => {
        const j = JSON.parse((await body(req, 64 << 10)).toString('utf8') || '{}');
        const job = { id: nextId(), args: String(j.args || '').slice(0, 1000), build: j.build ? String(j.build) : null,
                      status: 'queued', created: now() };
        jobs.push(job);
        saveJobs();
        text(res, 200, `job ${job.id}\n`);
    }],
    // PC: the last 20 jobs, newest first: id, status, build, created, args (tab-separated).
    ['GET', /^\/jobs$/, async (req, res) => {
        const rows = jobs.slice(-20).reverse().map(j => `${j.id}\t${j.status}\t${j.build || '-'}\t${j.created}\t${j.args}`);
        text(res, 200, rows.join('\n') + '\n');
    }],
    // PC: cancel a queued job.
    ['POST', /^\/jobs\/(\d+)\/cancel$/, async (req, res, m) => {
        const j = jobs.find(x => x.id === +m[1]);
        if (j && j.status === 'queued') { j.status = 'cancelled'; j.finished = now(); saveJobs(); }
        text(res, 200, 'ok\n');
    }],
    // Console: claim the oldest queued job ("job N", "build ID|-", "args ..."); 204 when there is none.
    ['GET', /^\/next$/, async (req, res) => {
        const j = jobs.find(x => x.status === 'queued');
        if (!j) { res.writeHead(204); return res.end(); }
        j.status = 'running'; j.claimed = now(); saveJobs();
        text(res, 200, `job ${j.id}\nbuild ${j.build || '-'}\nargs ${j.args}\n`);
    }],
    // PC: a live command for a running job; the console takes it with its next log post.
    ['POST', /^\/jobs\/(\d+)\/cmd$/, async (req, res, m) => {
        const t = (await body(req, 4096)).toString('utf8').replace(/[\r\n]+/g, ' ').trim().slice(0, 200);
        if (!t) return text(res, 400, 'error empty command\n');
        const qd = pendingCmds.get(m[1]) || [];
        if (qd.length >= 32) return text(res, 429, 'error too many pending commands\n');
        qd.push(t);
        pendingCmds.set(m[1], qd);
        text(res, 200, `queued ${qd.length}\n`);
    }],
    // Console: a chunk of the run's log, numbered by ?seq= (a chunk sent again replaces the old one). The reply
    // carries pending commands as "cmd <text>" lines.
    ['POST', /^\/jobs\/(\d+)\/log$/, async (req, res, m, q) => {
        const seq = parseInt(q.get('seq'), 10);
        if (!Number.isFinite(seq) || seq < 0) return text(res, 400, 'error seq\n');
        const dir = path.join(DATA, 'logs', m[1]);
        fs.mkdirSync(dir, { recursive: true });
        fs.writeFileSync(path.join(dir, String(seq).padStart(8, '0')), await body(req, 2 << 20));
        const qd = pendingCmds.get(m[1]) || [];
        pendingCmds.delete(m[1]);
        text(res, 200, 'ok\n' + qd.map(c => `cmd ${c}\n`).join(''));
    }],
    // PC: log chunks after ?after= (exclusive), concatenated; X-Relay-Last: the last seq sent; X-Relay-Status.
    ['GET', /^\/jobs\/(\d+)\/log$/, async (req, res, m, q) => {
        const after = parseInt(q.get('after') ?? '-1', 10);
        const dir = path.join(DATA, 'logs', m[1]);
        const names = fs.existsSync(dir) ? fs.readdirSync(dir).filter(n => +n > after).sort().slice(0, 200) : [];
        const j = jobs.find(x => x.id === +m[1]);
        const out = Buffer.concat(names.map(n => fs.readFileSync(path.join(dir, n))));
        text(res, 200, out, { 'X-Relay-Last': String(names.length ? +names[names.length - 1] : after),
                              'X-Relay-Status': j ? j.status : 'unknown' });
    }],
    // Console: a top-screen frame (BMP) numbered by ?n=.
    ['POST', /^\/jobs\/(\d+)\/frame$/, async (req, res, m, q) => {
        const n = parseInt(q.get('n'), 10), buf = await body(req, 2 << 20);
        if (!Number.isFinite(n) || buf.length < 54) return text(res, 400, 'error n or body\n');
        const dir = path.join(DATA, 'frames', m[1]);
        fs.mkdirSync(dir, { recursive: true });
        fs.writeFileSync(path.join(dir, `${String(n).padStart(4, '0')}.bmp`), buf);
        text(res, 200, 'ok\n');
    }],
    // PC: the job's frame names, and one frame.
    ['GET', /^\/jobs\/(\d+)\/frames$/, async (req, res, m) => {
        const dir = path.join(DATA, 'frames', m[1]);
        text(res, 200, (fs.existsSync(dir) ? fs.readdirSync(dir).sort() : []).join('\n') + '\n');
    }],
    ['GET', /^\/jobs\/(\d+)\/frame\/(\d{4}\.bmp)$/, async (req, res, m) => {
        const f = path.join(DATA, 'frames', m[1], m[2]);
        if (!fs.existsSync(f)) return text(res, 404, 'error no such frame\n');
        const buf = fs.readFileSync(f);
        res.writeHead(200, { 'Content-Type': 'image/bmp', 'Content-Length': buf.length });
        res.end(buf);
    }],
    // Console: the job ended (body: done, failed or stopped).
    ['POST', /^\/jobs\/(\d+)\/done$/, async (req, res, m) => {
        const s = (await body(req, 4096)).toString('utf8').trim();
        const j = jobs.find(x => x.id === +m[1]);
        if (j) { j.status = /^(done|failed|stopped)$/.test(s) ? s : 'done'; j.finished = now(); saveJobs(); }
        text(res, 200, 'ok\n');
    }],
];

http.createServer(async (req, res) => {
    try {
        if (!authorized(req)) return text(res, 401, 'error token\n');
        const u = new URL(req.url, 'http://relay');
        const p = u.pathname.replace(/\/+$/, '') || '/';
        for (const [method, re, fn] of routes) {
            const m = p.match(re);
            if (m && req.method === method) return await fn(req, res, m, u.searchParams);
        }
        text(res, 404, 'error no such route\n');
    } catch (e) {
        if (!res.headersSent) text(res, 500, `error ${e.message}\n`);
    }
}).listen(PORT, () => console.log(`gc3ds relay on :${PORT}, data in ${DATA}`));
