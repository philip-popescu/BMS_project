export const API_URL = import.meta.env.VITE_API_BASE_URL || (() => {
  const url = new URL(window.location.href);
  url.port = '2026';
  return url.origin;
})();

export async function api(path, { method = 'GET', body, signal } = {}) {
  const response = await fetch(`${API_URL}${path}`, {
    method, signal: signal || AbortSignal.timeout(15000),
    headers: body === undefined ? {} : { 'Content-Type': 'application/json' },
    body: body === undefined ? undefined : JSON.stringify(body),
  });
  if (response.status === 204) return null;
  const result = await response.json().catch(() => null);
  if (!response.ok) throw new Error(result?.error?.message || `Request failed (${response.status}).`);
  if (result === null) throw new Error('The API returned an unreadable response.');
  return result;
}

export const roomPath = (house, room) => `/households/${encodeURIComponent(house)}/rooms${room ? '/' + encodeURIComponent(room) : ''}`;
export const programPath = (house, room, program) => `${roomPath(house, room)}/programs${program ? '/' + encodeURIComponent(program) : ''}`;
