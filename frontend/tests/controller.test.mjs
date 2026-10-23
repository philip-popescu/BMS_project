import test from 'node:test';
import assert from 'node:assert/strict';
import { controllerDraft, controllerPatch, elapsed, temperature, operationNotice } from '../src/controller.mjs';

const config = {
  ssid: 'Home', password_set: true, ip: '192.168.0.211', subnet: '255.255.255.0',
  gateway: '192.168.0.1', dns: '192.168.0.1', api_host: 'bms.local', api_port: 2026,
  sensor_mac: 'AA:BB:CC:DD:EE:FF',
};

test('editing an IP preserves write-only credentials and sends only the changed field', () => {
  const draft = { ...controllerDraft(config), ip: '192.168.0.212' };
  assert.deepEqual(controllerPatch(draft, config, 'keep', ''), { ip: '192.168.0.212' });
  assert.equal(Object.hasOwn(controllerDraft(config), 'password'), false);
  assert.equal(Object.hasOwn(controllerDraft(config), 'password_set'), false);
});

test('empty Wi-Fi password requires explicit open-network intent', () => {
  const draft = controllerDraft(config);
  assert.throws(() => controllerPatch(draft, config, 'replace', ''), /open network/);
  assert.throws(() => controllerPatch(draft, config, 'keep', ''), /Change at least one/);
  assert.deepEqual(controllerPatch(draft, config, 'open', ''), { password: '' });
  assert.deepEqual(controllerPatch(draft, config, 'replace', ' secret pass '), { password: ' secret pass ' });
});

test('controller patch normalizes field input while preserving numerical API port', () => {
  const draft = { ...controllerDraft(config), api_host: '  192.168.0.11 ', api_port: '8080' };
  assert.deepEqual(controllerPatch(draft, config, 'keep', ''), { api_host: '192.168.0.11', api_port: 8080 });
});

test('network SSID whitespace is preserved as part of the network name', () => {
  const draft = { ...controllerDraft(config), ssid: ' Home Wi-Fi ' };
  assert.deepEqual(controllerPatch(draft, config, 'keep', ''), { ssid: ' Home Wi-Fi ' });
});

test('editing an unrelated field does not rewrite an existing SSID containing spaces', () => {
  const reported = { ...config, ssid: ' Home Wi-Fi ' };
  const draft = { ...controllerDraft(reported), api_port: '8080' };
  assert.deepEqual(controllerPatch(draft, reported, 'keep', ''), { api_port: 8080 });
});

test('missing measurements remain unknown instead of displaying a zero-temperature reading', () => {
  assert.equal(temperature(null), 'Not received');
  assert.equal(temperature(0), '0 °C');
  assert.equal(elapsed(null), 'Not received');
  assert.equal(elapsed(0), '0s');
  assert.equal(elapsed(63000), '1m 3s');
  assert.equal(elapsed(3600000), '1h 0m');
});

test('accepted and uncertain operations are never described as completed', () => {
  const pending = { state: 'accepted', old_ip: '192.168.0.230', new_ip: '192.168.0.211' };
  assert.match(operationNotice(pending), /Waiting/);
  assert.match(operationNotice(pending), /192\.168\.0\.230 and 192\.168\.0\.211/);
  assert.match(operationNotice({ ...pending, state: 'unconfirmed' }), /unconfirmed/);
});
