import test from 'node:test';
import assert from 'node:assert/strict';
import { newId } from '../src/identifiers.mjs';

test('record creation works on a plain HTTP LAN origin without randomUUID', () => {
  const insecureContext = { getRandomValues(array) { array.fill(15); return array; } };
  assert.equal(newId('house', insecureContext), 'house-0f0f0f0f0f0f0f0f0f0f0f0f');
});

test('generated record IDs are distinct and safe in API paths', () => {
  const first = newId('room'), second = newId('room');
  assert.match(first, /^room-[0-9a-f]{24}$/);
  assert.notEqual(first, second);
});
