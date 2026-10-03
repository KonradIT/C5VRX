import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import vm from 'node:vm';

// Exercise the actual click handler with a mock loader: never touch USB hardware.
const source = fs.readFileSync(process.env.APP_SOURCE_PATH || new URL('./app.js', import.meta.url), 'utf8').replace(/^import .*;\n/gm, '');
function harness(sourceName, packageType = 'merged', confirmed = true) {
  const elements = new Map();
  const get = id => {
    if (!elements.has(id)) elements.set(id, {
      style: {}, handlers: {}, value: '', textContent: '', checked: false,
      classList: { toggle() {}, add() {}, remove() {} },
      addEventListener(name, fn) { this.handlers[name] = fn; },
      setAttribute() {}, appendChild() {}, replaceChildren() {}
    });
    return elements.get(id);
  };
  const downloads = [], flashes = [], alerts = [];
  const context = vm.createContext({
    console, URL, Date, TextDecoder, TextEncoder,
    document: { baseURI: 'https://c5vrx.com/', getElementById: get,
      querySelectorAll: () => [], addEventListener() {}, createElement: () => get('option') },
    navigator: { serial: { addEventListener() {} } },
    window: { confirm: () => confirmed, matchMedia: () => ({ matches: true, addEventListener() {} }) },
    SerialTerminal: class { constructor() { this.state = 'disconnected'; } },
    alert: message => alerts.push(message),
    fetch: async url => { downloads.push(url); return { ok: true, arrayBuffer: async () => new Uint8Array([1, 2, 3, 4]).buffer }; },
    mockLoader: { writeFlash: async options => flashes.push(options), after: async () => {} }
  });
  vm.runInContext(source, context);
  for (const id of ['selectRelease', 'selectPrBuild', 'selectAlphaBuild']) get(id).value = '0';
  for (const id of ['selectPackageType', 'selectPrPackageType', 'selectAlphaPackageType']) get(id).value = packageType;
  vm.runInContext(`
    activeSource = ${JSON.stringify(sourceName)};
    isConnected = true; esploader = mockLoader;
    const rel = { tag_name: 'c5vrx4-v4.0.0-alpha.1', prerelease: true, assets: [
      { name: 'bootloader.bin', local_url: 'firmware/alpha/bootloader.bin', size: 4 },
      { name: 'partition-table.bin', local_url: 'firmware/alpha/partition-table.bin', size: 4 },
      { name: 'c5vrx4.bin', local_url: 'firmware/alpha/c5vrx4.bin', size: 4 },
      { name: 'c5vrx4_merged.bin', local_url: 'firmware/alpha/c5vrx4_merged.bin', size: 4 }
    ] };
    githubReleases = [rel]; githubPrBuilds = [rel]; githubAlphaBuilds = [rel];
  `, context);
  return { get, context, downloads, flashes, alerts, click: () => get('btnFlash').handlers.click() };
}

for (const remote of ['github', 'pr', 'alpha']) {
  test(`${remote} Full firmware downloads the merged binary at 0x0`, async () => {
    const h = harness(remote);
    await h.click();
    assert.deepEqual(h.downloads, ['https://c5vrx.com/firmware/alpha/c5vrx4_merged.bin']);
    assert.equal(h.flashes.length, 1);
    assert.equal(h.flashes[0].fileArray[0].address, 0);
    assert.ok(!h.alerts.some(x => x.startsWith('Flashing error:')));
  });
}
test('alpha App only downloads the application at 0x10000', async () => {
  const h = harness('alpha', 'app_only');
  await h.click();
  assert.deepEqual(h.downloads, ['https://c5vrx.com/firmware/alpha/c5vrx4.bin']);
  assert.equal(h.flashes[0].fileArray[0].address, 0x10000);
});
test('alpha Full firmware falls back to all three flash parts', async () => {
  const h = harness('alpha');
  vm.runInContext("githubAlphaBuilds[0].assets = githubAlphaBuilds[0].assets.filter(a => !a.name.includes('merged'))", h.context);
  await h.click();
  assert.equal(h.downloads.length, 3);
  assert.deepEqual(Array.from(h.flashes[0].fileArray, part => part.address), [0x2000, 0x8000, 0x10000]);
});
test('cancelled alpha confirmation never downloads or flashes', async () => {
  const h = harness('alpha', 'merged', false);
  await h.click();
  assert.equal(h.downloads.length, 0);
  assert.equal(h.flashes.length, 0);
});
test('only Local File requires a local binary', async () => {
  const h = harness('local');
  await h.click();
  assert.equal(h.downloads.length, 0);
  assert.equal(h.flashes.length, 0);
  assert.ok(h.alerts.includes('Flashing error: No local binary selected'));
});
