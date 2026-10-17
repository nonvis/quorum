// Quorum Radio's voice sidecar: one line at a time through its speaker's bound Kokoro voice, panned to the
// speaker's bound stereo position, the pauses given by the job (from the script's structure, never the model),
// joined into one stereo track and encoded to MP3 by ffmpeg.
//
//   node render.mjs --cast <cast.json> [--fake-tts] <job.json> <out.mp3>
//
// job: {speed, tail, lines: [{i, speaker, voice, pan, pause, text}]}. Every line's voice must be one the cast file
// binds (anything else is refused before a sample is made). Weights: Kokoro-82M v1.0 ONNX q8, read from
// QUORUM_KOKORO_CACHE (default ~/.cache/quorum/kokoro), checked against weights.sha256, never fetched once there.
// --fake-tts: silence of a length from the text (tests: no weights, no kokoro import).
// Prints one JSON line: {file, duration, sampleRate, lines: [{i, speaker, voice, pan, start, end}]}.
import { execFileSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import { existsSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { homedir } from 'node:os';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const HERE = dirname(fileURLToPath(import.meta.url));
const MODEL_ID = 'onnx-community/Kokoro-82M-v1.0-ONNX';
const RATE = 24000;

function die(msg) {
  process.stderr.write(`radio-voice: ${msg}\n`);
  process.exit(1);
}

const args = process.argv.slice(2);
let castFile = null;
let fake = false;
const pos = [];
for (let i = 0; i < args.length; i++) {
  if (args[i] === '--cast') castFile = args[++i];
  else if (args[i] === '--fake-tts') fake = true;
  else pos.push(args[i]);
}
if (!castFile || pos.length !== 2) die('usage: render.mjs --cast <cast.json> [--fake-tts] <job.json> <out.mp3>');
const [jobFile, outFile] = pos;

const cast = JSON.parse(readFileSync(castFile, 'utf8')).cast;
const bound = new Map(cast.map((d) => [d.id, d.voice]));
const job = JSON.parse(readFileSync(jobFile, 'utf8'));
for (const l of job.lines) {
  if (bound.get(l.speaker) !== l.voice) die(`line ${l.i}: ${l.voice} is not ${l.speaker}'s bound voice in the cast file (${bound.get(l.speaker) ?? 'no such speaker'})`);
  if (typeof l.pan !== 'number' || l.pan < -1 || l.pan > 1) die(`line ${l.i}: pan ${l.pan} is outside -1..1`);
}

async function kokoro() {
  const cache = process.env.QUORUM_KOKORO_CACHE || join(homedir(), '.cache', 'quorum', 'kokoro');
  const weights = join(cache, MODEL_ID, 'onnx', 'model_quantized.onnx');
  const pinned = readFileSync(join(HERE, 'weights.sha256'), 'utf8').trim().split(/\s+/)[0];
  const { env } = await import('@huggingface/transformers');
  const { KokoroTTS } = await import('kokoro-js');
  env.cacheDir = cache;
  if (existsSync(weights)) {
    const got = createHash('sha256').update(readFileSync(weights)).digest('hex');
    if (got !== pinned) die(`the Kokoro weights in ${cache} are ${got.slice(0, 12)}…, not the pinned ${pinned.slice(0, 12)}… (a weight change changes every voice)`);
    env.allowRemoteModels = false;
  }
  const tts = await KokoroTTS.from_pretrained(MODEL_ID, { dtype: 'q8', device: 'cpu' });
  return async (text, voice, speed) => {
    const a = await tts.generate(text, { voice, speed });
    return { rate: a.sampling_rate, samples: a.audio };
  };
}

const fakeTts = async (text) => ({ rate: RATE, samples: new Float32Array(Math.round(RATE * Math.max(0.4, text.length * 0.055))) });

function resample(x, from, to) {
  if (from === to) return x;
  const n = Math.round((x.length * to) / from);
  const out = new Float32Array(n);
  for (let i = 0; i < n; i++) {
    const t = (i * from) / to;
    const j = Math.floor(t);
    const f = t - j;
    out[i] = (x[j] ?? 0) * (1 - f) + (x[j + 1] ?? x[j] ?? 0) * f;
  }
  return out;
}

function wav(L, R, rate) {
  const n = L.length;
  const b = Buffer.alloc(44 + n * 4);
  b.write('RIFF', 0);
  b.writeUInt32LE(36 + n * 4, 4);
  b.write('WAVE', 8);
  b.write('fmt ', 12);
  b.writeUInt32LE(16, 16);
  b.writeUInt16LE(1, 20);
  b.writeUInt16LE(2, 22);
  b.writeUInt32LE(rate, 24);
  b.writeUInt32LE(rate * 4, 28);
  b.writeUInt16LE(4, 32);
  b.writeUInt16LE(16, 34);
  b.write('data', 36);
  b.writeUInt32LE(n * 4, 40);
  const s = (v) => Math.max(-32768, Math.min(32767, Math.round(v * 32767)));
  for (let i = 0; i < n; i++) {
    b.writeInt16LE(s(L[i]), 44 + i * 4);
    b.writeInt16LE(s(R[i]), 46 + i * 4);
  }
  return b;
}

const synth = fake ? fakeTts : await kokoro().catch((e) => die(`kokoro: ${String(e?.message ?? e).slice(0, 300)}`));
const speed = Number(job.speed) || 1;
const parts = [];
for (const l of job.lines) {
  const a = await synth(String(l.text), l.voice, speed);
  parts.push({ l, samples: resample(a.samples, a.rate, RATE) });
}
const total = parts.reduce((n, p) => n + Math.round(p.l.pause * RATE) + p.samples.length, 0) + Math.round((Number(job.tail) || 1) * RATE);
const L = new Float32Array(total);
const R = new Float32Array(total);
let at = 0;
const lines = [];
for (const p of parts) {
  at += Math.round(p.l.pause * RATE);
  // Constant-power pan: -1 = all left, 0 = centre, +1 = all right.
  const th = ((p.l.pan + 1) * Math.PI) / 4;
  const gl = Math.cos(th);
  const gr = Math.sin(th);
  for (let i = 0; i < p.samples.length; i++) {
    L[at + i] = p.samples[i] * gl;
    R[at + i] = p.samples[i] * gr;
  }
  const start = at / RATE;
  at += p.samples.length;
  lines.push({ i: p.l.i, speaker: p.l.speaker, voice: p.l.voice, pan: p.l.pan, start: Math.round(start * 100) / 100, end: Math.round((at / RATE) * 100) / 100 });
}
const tmp = `${outFile}.render.wav`;
writeFileSync(tmp, wav(L, R, RATE));
try {
  execFileSync('ffmpeg', ['-hide_banner', '-loglevel', 'error', '-y', '-i', tmp, '-codec:a', 'libmp3lame', '-b:a', '96k', outFile]);
} catch (e) {
  die(`ffmpeg: ${String(e?.stderr ?? e?.message ?? e).slice(-300)}`);
} finally {
  rmSync(tmp, { force: true });
}
if (!existsSync(outFile)) die('no file written');
process.stdout.write(`${JSON.stringify({ file: outFile, duration: Math.round((total / RATE) * 100) / 100, sampleRate: RATE, lines })}\n`);
