import test from 'node:test';
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import { webcrypto } from 'node:crypto';
import { createContext, runInContext } from 'node:vm';

const setupPage = await readFile(new URL('../../firmware/BMSController/SetupPage.h', import.meta.url), 'utf8');
const setupScript = setupPage.match(/<script>([\s\S]*?)<\/script>/)?.[1];
assert.ok(setupScript, 'The controller setup page must contain its inline script.');

const deviceId = '0123456789abcdef0123456789abcdef';
const reportedConfig = {
  ssid: 'Home Wi-Fi', password_set: true, ip: '192.168.0.230', subnet: '255.255.255.0',
  gateway: '192.168.0.1', dns: '192.168.0.1', api_host: 'BMS', api_port: 2026, sensor_mac: '',
};
const configurationFields = ['ssid', 'ip', 'subnet', 'gateway', 'dns', 'api_host', 'api_port', 'sensor_mac'];

async function setupHarness(outcomes = []) {
  const elements = Object.fromEntries([...configurationFields, 'message', 'save', 'identity', 'form', 'password', 'open']
    .map(id => [id, { value: '', disabled: false, checked: false, textContent: '' }]));
  const requests = [];
  let submitHandler;
  elements.form.addEventListener = (event, callback) => {
    assert.equal(event, 'submit');
    submitHandler = callback;
  };
  const context = createContext({
    document: { getElementById: id => elements[id] },
    window: { crypto: webcrypto }, crypto: webcrypto, Uint8Array,
    fetch: async (path, options) => {
      if (path === '/status') {
        assert.equal(options.cache, 'no-store');
        return { ok: true, json: async () => ({ device_id: deviceId, config: reportedConfig, heater_on: true, setup_mode: true }) };
      }
      assert.equal(path, '/config');
      assert.equal(options.method, 'POST');
      assert.equal(options.headers['Content-Type'], 'application/json');
      const request = JSON.parse(options.body);
      requests.push(request);
      const outcome = outcomes.shift();
      if (outcome instanceof Error) throw outcome;
      return outcome || {
        ok: true,
        json: async () => ({ accepted: true, restart_required: true, command_id: request.command_id }),
      };
    },
  });
  runInContext(setupScript, context, { filename: 'embedded-controller-setup.js' });
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(elements.save.disabled, false, 'Saving is enabled after the controller supplies its identity and settings.');
  assert.equal(elements.ssid.value, reportedConfig.ssid);
  return {
    elements, requests,
    submit: () => submitHandler({ preventDefault() {} }),
  };
}

test('embedded setup submits controller identity and optional sensor with explicit password intent', async t => {
  for (const scenario of [
    { name: 'keep saved password', entered: '', open: false, included: false },
    { name: 'replace password verbatim', entered: ' example-pass ', open: false, included: true, password: ' example-pass ' },
    { name: 'explicit open network', entered: 'example-pass', open: true, included: true, password: '' },
  ]) {
    await t.test(scenario.name, async () => {
      const page = await setupHarness();
      page.elements.password.value = scenario.entered;
      page.elements.open.checked = scenario.open;
      await page.submit();
      const [body] = page.requests;
      assert.deepEqual(Object.keys(body).sort(), ['command_id', 'config', 'device_id']);
      assert.equal(body.device_id, deviceId);
      assert.match(body.command_id, /^[\da-f]{8}-[\da-f]{4}-4[\da-f]{3}-[89ab][\da-f]{3}-[\da-f]{12}$/);
      const expectedConfig = Object.fromEntries(configurationFields.map(key => [key, reportedConfig[key]]));
      if (scenario.included) expectedConfig.password = scenario.password;
      assert.deepEqual(body.config, expectedConfig);
      assert.equal(Object.hasOwn(body.config, 'password'), scenario.included);
      assert.equal(body.config.sensor_mac, '', 'An unassigned sensor is allowed during Wi-Fi provisioning.');
      assert.equal(typeof body.config.api_port, 'number');
      assert.equal(page.elements.password.value, '', 'An acknowledged save clears the entered password.');
      assert.equal(page.elements.save.disabled, true, 'The page waits for restart after acceptance.');
    });
  }
});

test('embedded setup retries an uncertain save with the same command ID until settings change', async () => {
  const page = await setupHarness([
    new Error('Connection closed during restart'),
    new Error('Connection closed during restart'),
  ]);
  page.elements.password.value = 'example-pass';
  await page.submit();
  assert.equal(page.elements.save.disabled, false);
  assert.equal(page.elements.password.value, 'example-pass', 'A failed response preserves entered settings.');
  await page.submit();
  assert.deepEqual(page.requests[1], page.requests[0], 'An identical retry must use firmware command deduplication.');
  page.elements.ip.value = '192.168.0.211';
  await page.submit();
  assert.notEqual(page.requests[2].command_id, page.requests[0].command_id, 'Changed settings require a new operation ID.');
  assert.equal(page.requests[2].config.ip, '192.168.0.211');
  assert.equal(page.requests[2].device_id, deviceId);
});
