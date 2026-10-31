// Page shell of the browser build (GitLab #36, docs/web.md).
// Mounts IndexedDB at /scooby (IDBFS), finds the ROM, starts the C program (port/src/web/main_web.c).
// ROM order: rom/baserom.md beside the page (a private deployment), the copy kept in IndexedDB,
// then the player's own file (picker / drag and drop). The ROM is checked by sha1 (in C) and never
// leaves the browser.
'use strict';

const ROM_SHA1 = 'ccc9542d964de5bdc1e9101620cae40dd98e7127';
const ROM_DIR = '/scooby/rom';
const ROM_CACHED = ROM_DIR + '/baserom.md';
const $ = id => document.getElementById(id);

window.scoobyState = 'loading';           // loading | need-rom | running | error (for tests)
const errLines = [];

function setStatus(t) { $('status').textContent = t; }

function showFatal(title, text) {
  window.scoobyState = 'error';
  $('rom-panel').hidden = true;
  $('error-title').textContent = title;
  $('error-text').textContent = text;
  $('error-log').textContent = errLines.join('\n');
  $('error-panel').hidden = false;
  setStatus('stopped');
}

// a fatal error of the C program: abort() (the native runner's "no C block at $X"), or a trap
function onAbort(what) {
  const nb = errLines.map(l => /native: (.*) at \$([0-9A-F]{6})/.exec(l)).find(m => m);
  if (nb && nb[1] === 'no C block') {
    showFatal('The game reached code this port does not have yet',
      'At 68000 address $' + nb[2] + ' the game went somewhere that has no C translation in this build. ' +
      'Nothing is emulated in this port, so it has to stop here. Please report the address and the text below.');
  } else if (nb) {
    showFatal('The game stopped', nb[1] + ' at $' + nb[2] + '.');
  } else {
    showFatal('The game stopped', String(what || 'unknown error'));
  }
}

let syncTimer = 0, syncing = false, syncAgain = false;
function syncNow() {
  if (syncing) { syncAgain = true; return; }
  syncing = true;
  Module.FS.syncfs(false, err => {
    syncing = false;
    if (err) console.warn('IndexedDB sync failed', err);
    if (syncAgain) { syncAgain = false; syncNow(); }
  });
}

var Module = window.Module = {
  canvas: $('canvas'),
  noInitialRun: true,
  print: t => console.log(t),
  printErr: t => {
    console.warn(t);
    errLines.push(t);
    if (errLines.length > 60) errLines.shift();
  },
  onAbort,
  setStatus: t => { if (t && window.scoobyState === 'loading') setStatus(t); },
  preRun: [() => {
    const FS = Module.FS;
    FS.mkdir('/scooby');
    FS.mount(Module.IDBFS || FS.filesystems.IDBFS, {}, '/scooby');
    Module.addRunDependency('idbfs');
    FS.syncfs(true, err => {
      if (err) console.warn('IndexedDB unavailable: saves will not be kept', err);
      Module.removeRunDependency('idbfs');
    });
  }],
  onRuntimeInitialized: () => { findRom(); },
  // called from C
  scoobySync: () => { clearTimeout(syncTimer); syncTimer = setTimeout(syncNow, 300); },
  scoobyStatus: t => setStatus(t),
  scoobyStarted: () => { window.scoobyState = 'running'; },
  scoobyFatal: t => showFatal('Cannot start the game', t),
};

function exists(path) {
  try { Module.FS.stat(path); return true; } catch (e) { return false; }
}

function romOk(path) {
  return Module.ccall('web_rom_check', 'number', ['string'], [path]) === 1;
}

// Browsers only let a page make sound after a user gesture. If the game would start without one
// (the server-ROM path), wait for a click first: SDL creates its AudioContext inside callMain, so
// creating it in the click handler means it starts 'running' instead of silently 'suspended'.
function start(path, source) {
  const activated = navigator.userActivation ? navigator.userActivation.hasBeenActive : false;
  if (!activated && !navigator.webdriver) {
    $('rom-panel').hidden = true;
    $('play-panel').hidden = false;
    setStatus('ready');
    const go = () => { $('play-panel').hidden = true; startNow(path, source); resumeAudio(); };
    $('btn-play').addEventListener('click', go, { once: true });
    return;
  }
  startNow(path, source);
}

function startNow(path, source) {
  $('rom-panel').hidden = true;
  $('btn-forget').hidden = source !== 'cached';
  setStatus('starting (' + source + ' ROM)');
  console.log('scooby: starting with the ' + source + ' ROM');
  $('canvas').focus();
  try {
    const args = [path];
    if (new URLSearchParams(location.search).get('hifps') === 'force') args.push('--hifps-force');
    Module.callMain(args);
  } catch (e) {
    if (e !== 'unwind' && !(e && e.name === 'ExitStatus')) throw e;
  }
}

async function findRom() {
  const FS = Module.FS;
  $('rom-sha1').textContent = ROM_SHA1;
  // 1. a ROM served beside the page (only on a private deployment)
  try {
    const r = await fetch('rom/baserom.md', { cache: 'no-cache' });
    if (r.ok) {
      const buf = new Uint8Array(await r.arrayBuffer());
      if (buf.length > 0x1000) {
        FS.writeFile('/tmp/server.md', buf);
        if (romOk('/tmp/server.md')) return start('/tmp/server.md', 'server');
        FS.unlink('/tmp/server.md');
        console.warn('rom/baserom.md on the server is not the right ROM');
      }
    }
  } catch (e) { /* no server ROM */ }
  // 2. the copy kept in IndexedDB
  if (exists(ROM_CACHED)) {
    if (romOk(ROM_CACHED)) return start(ROM_CACHED, 'cached');
    FS.unlink(ROM_CACHED);
  }
  // 3. ask
  window.scoobyState = 'need-rom';
  setStatus('waiting for the ROM');
  $('rom-panel').hidden = false;
}

async function takeRomFile(file) {
  const FS = Module.FS;
  const err = $('rom-error');
  err.hidden = true;
  if (!file) return;
  if (file.size < 0x1000 || file.size > 8 * 1024 * 1024) {
    err.textContent = file.name + ' is not a Genesis ROM (size ' + file.size + ' bytes).';
    err.hidden = false;
    return;
  }
  const buf = new Uint8Array(await file.arrayBuffer());
  if (!exists(ROM_DIR)) FS.mkdir(ROM_DIR);
  const tmp = ROM_DIR + '/incoming.md';
  FS.writeFile(tmp, buf);
  if (!romOk(tmp)) {
    FS.unlink(tmp);
    err.textContent = file.name + ' is not the right ROM: this needs Scooby-Doo Mystery (USA), sha1 ' + ROM_SHA1 + '.';
    err.hidden = false;
    return;
  }
  FS.rename(tmp, ROM_CACHED);
  syncNow();
  start(ROM_CACHED, 'cached');
}

$('rom-file').addEventListener('change', e => takeRomFile(e.target.files[0]));
const drop = $('drop');
for (const t of ['dragenter', 'dragover']) drop.addEventListener(t, e => { e.preventDefault(); drop.classList.add('over'); });
for (const t of ['dragleave', 'drop']) drop.addEventListener(t, e => { e.preventDefault(); drop.classList.remove('over'); });
drop.addEventListener('drop', e => takeRomFile(e.dataTransfer.files[0]));
// a file dropped anywhere on the page while it asks for the ROM
document.addEventListener('dragover', e => e.preventDefault());
document.addEventListener('drop', e => {
  e.preventDefault();
  if (window.scoobyState === 'need-rom' && e.dataTransfer.files[0]) takeRomFile(e.dataTransfer.files[0]);
});

$('btn-forget').addEventListener('click', () => {
  if (!confirm('Remove the ROM stored in this browser? (Saves are kept.) The page reloads.')) return;
  try { Module.FS.unlink(ROM_CACHED); } catch (e) { /* already gone */ }
  Module.FS.syncfs(false, () => location.reload());
});
$('btn-reload').addEventListener('click', () => location.reload());

// fullscreen: the stage (canvas) only; F11 does the same
function toggleFullscreen() {
  if (document.fullscreenElement) document.exitFullscreen();
  else $('stage').requestFullscreen().catch(() => {});
  $('canvas').focus();
}
$('btn-fullscreen').addEventListener('click', toggleFullscreen);

// the game's keys must not scroll the page or trigger browser shortcuts (F5 reload, F3 find...)
const GAME_KEYS = new Set(['ArrowUp', 'ArrowDown', 'ArrowLeft', 'ArrowRight', 'Tab', 'Enter', ' ',
  'F1', 'F2', 'F3', 'F4', 'F5', 'F6', 'F7', 'F8', 'F9', 'F10']);
window.addEventListener('keydown', e => {
  if (window.scoobyState !== 'running') return;
  if (e.key === 'F11') { e.preventDefault(); toggleFullscreen(); return; }
  if (GAME_KEYS.has(e.key)) e.preventDefault();
}, true);

// sound: browsers keep an AudioContext suspended until a user gesture
function audioCtx() { return Module.SDL2 && Module.SDL2.audioContext; }
function resumeAudio() {
  const ctx = audioCtx();
  if (ctx && ctx.state !== 'running') ctx.resume().catch(() => {});
}
for (const t of ['pointerdown', 'keydown', 'touchstart']) document.addEventListener(t, resumeAudio, true);
$('btn-sound').addEventListener('click', resumeAudio);
setInterval(() => {
  const ctx = audioCtx();
  $('btn-sound').hidden = !(ctx && ctx.state !== 'running' && window.scoobyState === 'running');
}, 500);

// keep IndexedDB current when the tab goes away
document.addEventListener('visibilitychange', () => { if (document.hidden && Module.FS) syncNow(); });

window.addEventListener('error', e => {
  if (window.scoobyState === 'running' || window.scoobyState === 'loading')
    errLines.push(String(e.message || e.error));
});
