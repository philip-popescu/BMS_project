// randomUUID requires HTTPS; getRandomValues also works at http://BMS:3000.
export function newId(prefix, source = globalThis.crypto) {
  const bytes = new Uint8Array(12);
  source.getRandomValues(bytes);
  return `${prefix}-${Array.from(bytes, byte => byte.toString(16).padStart(2, '0')).join('')}`;
}
