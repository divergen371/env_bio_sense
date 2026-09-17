// Exercise the actual boot/API code without a browser, device or network.
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import vm from 'node:vm';

const source = readFileSync(new URL('../web/app.js', import.meta.url), 'utf8');
const statusRoutes = ['/api/status', '/api/location', '/api/weather', '/api/altitude', '/api/ppg'];

async function run(search) {
  const calls = [];
  const intervals = [];
  const context = vm.createContext({
    URLSearchParams, Headers,
    window: {
      location: { search }, addEventListener() {},
      setInterval(fn, ms) { intervals.push(ms); }
    },
    document: { querySelectorAll() { return []; } },
    fetch: async (path, options) => {
      calls.push([path, options.method]);
      return { ok: true, json: async () => ({ csrf_token: 'test', clock_valid: false }) };
    }
  });
  vm.runInContext(source, context);
  // DOM rendering is irrelevant to the request isolation contract. Keep the
  // real api() gate and boot() decision; substitute only render/event helpers.
  vm.runInContext(`
    setupTabs = () => {};
    setupActions = () => {};
    showToast = () => {};
    setConnection = () => {};
    syncTime = () => api('/api/time', {method: 'POST', body: '{}'});
    loadStatus = () => Promise.all(${JSON.stringify(statusRoutes)}.map(path => api(path)));
    loadFiles = () => api('/api/files?page=1');
  `, context);
  await vm.runInContext('boot()', context);
  return { context, calls, intervals };
}

for (const search of ['', '?io_probe=unknown']) {
  const result = await run(search);
  assert.deepEqual(result.calls, [
    ['/api/session', 'GET'], ['/api/time', 'POST'],
    ...statusRoutes.map(path => [path, 'GET'])
  ]);
  assert.deepEqual(result.intervals, [10000]);
}

for (const mode of ['status', 'files']) {
  const { context, calls, intervals } = await run(`?io_probe=${mode}`);
  assert.deepEqual(calls, [
    ['/api/session', 'GET'],
    ...(mode === 'status' ? statusRoutes : ['/api/files?page=1']).map(path => [path, 'GET'])
  ]);
  assert.deepEqual(intervals, []);
  const before = calls.length;
  await assert.rejects(vm.runInContext("api('/api/flush', {method: 'POST'})", context), /調査モード/);
  await assert.rejects(vm.runInContext(`api('${mode === 'status' ? '/api/files' : '/api/status'}')`, context), /調査モード/);
  assert.equal(calls.length, before, 'blocked operations must never reach fetch');
}
console.log('PASS: normal boot defers files; status/files isolated; writes and other routes blocked');
