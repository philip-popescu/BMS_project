import test from 'node:test';
import assert from 'node:assert/strict';
import { DAYS, offsetMinutes, toUtc, toLocal, activeProgram } from '../src/schedule.mjs';

test('local weekday rolls back across midnight when converted to UTC', () => {
  assert.deepEqual(toUtc('00:30', ['monday'], 120), { start_time: '22:30', days: ['sunday'] });
  assert.deepEqual(toUtc('07:00', DAYS, 120), { start_time: '05:00' });
  assert.deepEqual(toUtc('23:30', ['sunday'], -300), { start_time: '04:30', days: ['monday'] });
});

test('local display shifts across daylight saving without mutating UTC', () => {
  const program = { start_time: '05:00' };
  assert.equal(toLocal(program, 'Europe/Bucharest', new Date('2026-01-01T12:00Z')).time, '07:00');
  assert.equal(toLocal(program, 'Europe/Bucharest', new Date('2026-07-01T12:00Z')).time, '08:00');
  assert.equal(program.start_time, '05:00');
  assert.equal(offsetMinutes('UTC'), 0);
});

test('local display weekdays follow UTC events across midnight', () => {
  const program = { start_time: '22:30', days: ['sunday'] };
  assert.deepEqual(toLocal(program, 'Europe/Bucharest', new Date('2026-01-01T12:00Z')), { time: '00:30', days: ['monday'] });
});

test('day-specific programs win ties; weekly and midnight spillover remains active', () => {
  const daily = { id: 'daily', start_time: '07:00', target_temperature: 22 };
  const weekend = { id: 'weekend', start_time: '07:00', days: ['saturday'], target_temperature: 24 };
  assert.equal(activeProgram([daily, weekend], new Date('2026-10-10T07:00Z')).id, 'weekend');
  assert.equal(activeProgram([weekend, daily], new Date('2026-10-10T07:00Z')).id, 'weekend');
  assert.equal(activeProgram([weekend], new Date('2026-10-17T06:00Z')).id, 'weekend');
  assert.equal(activeProgram([], new Date()), null);
});
