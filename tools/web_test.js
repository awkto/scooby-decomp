// Headless browser test of the web build (GitLab #36, docs/web.md).
//   node tools/web_test.js URL MODE OUTDIR [ROM]
// MODE picker: fresh profile, the page must ask for the ROM; ROM is given through the file input,
//              the game must boot to the title and reach the Lobby with Start presses (the
//              harness route: frame 1500, then 7 x (Start 4 frames, 150 frames), then 120);
//              then a reload must start from the copy kept in IndexedDB.
// MODE server: the page must find rom/baserom.md beside it and start without asking.
// Needs puppeteer-core (NODE_PATH) and CHROME=<chrome or chrome-headless-shell binary>.
// Writes OUTDIR/<mode>-*.png and prints a JSON summary (console errors, fps, load time).
'use strict';
const puppeteer = require('puppeteer-core');
const fs = require('fs');
const path = require('path');

const [url, mode, outdir, rom] = process.argv.slice(2);
if (!url || !mode || !outdir) { console.log('usage: web_test.js URL picker|server OUTDIR [ROM]'); process.exit(2); }
fs.mkdirSync(outdir, { recursive: true });
const errors = [], warnings = [], logs = [];
const res = { mode };
let gpage = null;
const watchdog = setTimeout(async () => {
  try { res.status_at_timeout = await gpage.evaluate(() => document.getElementById('status').textContent + ' frame ' + Module._web_frame_count()); } catch (e) { /* page gone */ }
  res.errors = errors; res.logs = logs.slice(-20);
  console.log(JSON.stringify(res, null, 1));
  console.log('WATCHDOG: timed out');
  process.exit(3);
}, 240000);
const sleep = ms => new Promise(r => setTimeout(r, ms));

(async () => {
  const browser = await puppeteer.launch({
    executablePath: process.env.CHROME,
    headless: true,
    // HIFPS=1: rAF not tied to a 60 Hz vsync, so the page sees a fast display and interpolates
    args: ['--no-sandbox', '--enable-unsafe-swiftshader', '--use-angle=swiftshader', '--window-size=1280,860']
      .concat(process.env.HIFPS ? ['--disable-gpu-vsync', '--disable-frame-rate-limit'] : []),
  });
  const page = await browser.newPage();
  gpage = page;
  await page.setViewport({ width: 1280, height: 860, deviceScaleFactor: Number(process.env.DPR || 1) });

  page.on('console', m => {
    const t = m.text();
    const loc = m.location() && m.location().url || '';
    if (m.type() === 'error' && loc.endsWith('rom/baserom.md') && mode === 'picker') res.rom_404_logged = true;
    else if (m.type() === 'error') errors.push(t + (loc ? ' (' + loc + ')' : ''));
    else if (m.type() === 'warning') warnings.push(t);
    else logs.push(t);
  });
  page.on('pageerror', e => errors.push('pageerror: ' + e.message));
  page.on('requestfailed', r => errors.push('requestfailed: ' + r.url()));

  const state = () => page.evaluate(() => window.scoobyState);
  const frame = () => page.evaluate(() => Module._web_frame_count());
  const waitState = async (want, ms) => {
    await page.waitForFunction(w => window.scoobyState === w || window.scoobyState === 'error', { timeout: ms }, want);
    const s = await state();
    if (s !== want) throw new Error('state ' + s + ': ' + await page.evaluate(() => document.getElementById('error-text').textContent + '\n' + document.getElementById('error-log').textContent));
  };
  const waitFrame = n => page.waitForFunction(n => Module._web_frame_count() >= n, { timeout: 120000, polling: 'raf' }, n);
  const shot = async name => {
    const p = path.join(outdir, mode + '-' + name + '.png');
    await (await page.$('#stage')).screenshot({ path: p });
    return p;
  };

  const t0 = Date.now();
  // a 404 for rom/baserom.md is the expected answer on a public site: not an error
  page.on('response', r => { if (r.url().endsWith('rom/baserom.md')) res.server_rom_status = r.status(); });
  await page.goto(url, { waitUntil: 'load' });
  const wasm = await page.evaluate(() => performance.getEntriesByType('resource')
    .filter(e => e.name.endsWith('.wasm')).map(e => ({ bytes: e.transferSize || e.encodedBodySize, ms: Math.round(e.duration) }))[0]);
  res.wasm = wasm;

  if (mode === 'picker') {
    await waitState('need-rom', 30000);
    res.need_rom_ms = Date.now() - t0;
    res.shots = [await shot('rom-panel')];
    const input = await page.$('#rom-file');
    await input.uploadFile(rom);
    await waitState('running', 30000);
  } else {
    await waitState('running', 30000);
  }
  res.running_ms = Date.now() - t0;
  res.shots = res.shots || [];
  await page.focus('#canvas');
  res.audio_before_gesture = await page.evaluate(() => Module.SDL2 && Module.SDL2.audioContext ? Module.SDL2.audioContext.state : 'none');
  await page.keyboard.press('Shift');         // the gesture that starts the sound (no game key)
  await sleep(300);
  res.audio_after_gesture = await page.evaluate(() => Module.SDL2.audioContext.state);

  // title
  await waitFrame(1400);
  res.shots.push(await shot('title'));
  res.room_title = await page.evaluate(() => Module._web_ram16(0xFF06AC));
  // Start presses (harness route)
  await waitFrame(1500);
  let f = 1500;
  for (let i = 0; i < 7; i++) {
    await page.keyboard.down('Enter');
    await waitFrame(f + 4);
    await page.keyboard.up('Enter');
    f += 154;
    await waitFrame(f);
    if (i === 0) res.shots.push(await shot('after-start1'));
  }
  await waitFrame(f + 120);
  res.shots.push(await shot('lobby'));
  res.room_lobby = await page.evaluate(() => [Module._web_ram16(0xFF06AA), Module._web_ram16(0xFF06AC)]);
  // walk a bit (keyboard) and click (mouse)
  const fw = await frame();
  await page.keyboard.down('ArrowRight'); await waitFrame(fw + 48); await page.keyboard.up('ArrowRight');
  res.shots.push(await shot('walked'));
  // mouse: click on the floor left of Shaggy, he walks there (X = word at $FF04DC)
  const box = await (await page.$('#canvas')).boundingBox();
  const x0 = await page.evaluate(() => Module._web_ram16(0xFF04DC));
  await page.mouse.move(box.x + box.width * 0.42, box.y + box.height * 0.42);
  await sleep(100);
  await page.mouse.click(box.x + box.width * 0.42, box.y + box.height * 0.42);
  await waitFrame(await frame() + 150);
  const x1 = await page.evaluate(() => Module._web_ram16(0xFF04DC));
  res.mouse_walk_x = [x0, x1];
  res.shots.push(await shot('mouse-walked'));

  // cost of an in-between picture while Shaggy walks (high fps on only)
  if (process.env.HIFPS) {
    await page.keyboard.down('ArrowLeft');
    const costs = [];
    for (let i = 0; i < 20 && costs.length < 5; i++) {
      await sleep(50);
      const c = await page.evaluate(() => Module._web_bench_hifps(20));
      if (c >= 0) costs.push(Math.round(c * 100) / 100);
    }
    await page.keyboard.up('ArrowLeft');
    res.hifps_ms_per_picture = costs;
  }
  // fps over 5 s
  const f0 = await frame(), tA = Date.now();
  await sleep(5000);
  res.logic_fps = Math.round((await frame() - f0) * 1000 / (Date.now() - tA) * 10) / 10;
  res.status = await page.evaluate(() => document.getElementById('status').textContent);
  res.audio = await page.evaluate(() => Module.SDL2 && Module.SDL2.audioContext ? Module.SDL2.audioContext.state : 'none');
  res.canvas = await page.evaluate(() => { const c = document.getElementById('canvas'); return [c.width, c.height, c.clientWidth, c.clientHeight, devicePixelRatio]; });

  if (mode === 'server') {
    // the error panel for the native runner's fatal stop (simulated: the message, then abort)
    await page.evaluate(() => { Module.printErr('native: no C block at $01ABCD (SR $2700, SP $FFFE00, long at SP $000123).'); Module.onAbort('Aborted()'); });
    res.fatal_panel = await page.evaluate(() => !document.getElementById('error-panel').hidden && document.getElementById('error-text').textContent);
    res.shots.push(await shot('fatal'));
  }
  if (mode === 'picker') {
    // the ROM was kept: a reload starts without asking
    await sleep(800);                           // the debounced IndexedDB sync
    await page.reload({ waitUntil: 'load' });
    await waitState('running', 30000);
    res.reload_from_cache = await page.evaluate(() => !document.getElementById('btn-forget').hidden);
    await waitFrame(300);
    res.shots.push(await shot('reload'));
  }
  res.errors = errors;
  res.warnings = warnings.slice(0, 20);
  res.logs = logs.slice(0, 20);
  console.log(JSON.stringify(res, null, 1));
  clearTimeout(watchdog);
  await browser.close();
  process.exit(errors.length ? 1 : 0);
})().catch(e => {
  res.errors = errors; res.warnings = warnings.slice(-30); res.logs = logs.slice(-30);
  console.log(JSON.stringify(res, null, 1));
  console.log('TEST ERROR: ' + e.message);
  process.exit(2);
});
