export const DAYS = ['monday', 'tuesday', 'wednesday', 'thursday', 'friday', 'saturday', 'sunday'];
export const shortDay = day => day.slice(0, 3).replace(/^./, c => c.toUpperCase());

export function offsetMinutes(timezone, now = new Date()) {
  const value = new Intl.DateTimeFormat('en', { timeZone: timezone, timeZoneName: 'longOffset' })
    .formatToParts(now).find(p => p.type === 'timeZoneName').value;
  if (value === 'GMT' || value === 'UTC') return 0;
  const match = value.match(/(?:GMT|UTC)([+-])(\d{2}):(\d{2})/);
  if (!match) throw new Error('Could not determine the household timezone offset.');
  return (match[1] === '-' ? -1 : 1) * (Number(match[2]) * 60 + Number(match[3]));
}

const clock = value => `${String(Math.floor(value / 60)).padStart(2, '0')}:${String(value % 60).padStart(2, '0')}`;
const minutes = time => {
  if (!/^(?:[01]\d|2[0-3]):[0-5]\d$/.test(time)) throw new Error('Enter a valid start time.');
  return Number(time.slice(0, 2)) * 60 + Number(time.slice(3));
};
const shiftDays = (days, shift) => DAYS.filter(d => (days || DAYS).some(day => DAYS[(DAYS.indexOf(day) + shift + 14) % 7] === d));

export function toUtc(time, days, offset) {
  const value = minutes(time) - offset;
  const shifted = shiftDays(days, Math.floor(value / 1440));
  const result = { start_time: clock((value % 1440 + 1440) % 1440) };
  if (shifted.length < 7) result.days = shifted;
  return result;
}

export function toLocal(program, timezone, now = new Date()) {
  const value = minutes(program.start_time) + offsetMinutes(timezone, now);
  return { time: clock((value % 1440 + 1440) % 1440), days: shiftDays(program.days, Math.floor(value / 1440)) };
}

export function activeProgram(programs, now = new Date()) {
  let best = null;
  let bestTime = -Infinity;
  for (const program of programs) {
    const [hour, minute] = program.start_time.split(':').map(Number);
    for (let ago = 0; ago < 8; ago++) {
      const date = new Date(Date.UTC(now.getUTCFullYear(), now.getUTCMonth(), now.getUTCDate() - ago, hour, minute));
      const weekday = DAYS[(date.getUTCDay() + 6) % 7];
      if (date > now || (program.days && !program.days.includes(weekday))) continue;
      if (date.getTime() > bestTime || (date.getTime() === bestTime && program.days && !best?.days)) {
        best = program;
        bestTime = date.getTime();
      }
      break;
    }
  }
  return best;
}
