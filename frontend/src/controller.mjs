const CONFIG_FIELDS = ['ssid', 'ip', 'subnet', 'gateway', 'dns', 'api_host', 'api_port', 'sensor_mac'];

export function controllerDraft(config) {
  return Object.fromEntries(CONFIG_FIELDS.map(key => [key, String(config[key] ?? '')]));
}

// An untouched password must never turn into an empty-password write.
export function controllerPatch(draft, reported, passwordMode, password) {
  const patch = {};
  for (const key of CONFIG_FIELDS) {
    const value = key === 'api_port' ? Number(draft[key]) : key === 'ssid' ? draft[key] : draft[key].trim();
    const previous = key === 'api_port' ? Number(reported[key]) : String(reported[key] ?? '');
    if (value !== previous) patch[key] = value;
  }
  if (passwordMode === 'replace') {
    if (!password) throw new Error('Enter a Wi-Fi password, or explicitly select an open network.');
    patch.password = password;
  } else if (passwordMode === 'open') patch.password = '';
  if (!Object.keys(patch).length) throw new Error('Change at least one setting before saving.');
  return patch;
}

export function elapsed(milliseconds) {
  if (milliseconds === null || milliseconds === undefined || !Number.isFinite(milliseconds)) return 'Not received';
  const seconds = Math.floor(Math.max(0, milliseconds) / 1000);
  if (seconds < 60) return `${seconds}s`;
  const minutes = Math.floor(seconds / 60);
  if (minutes < 60) return `${minutes}m ${seconds % 60}s`;
  const hours = Math.floor(minutes / 60);
  if (hours < 24) return `${hours}h ${minutes % 60}m`;
  return `${Math.floor(hours / 24)}d ${hours % 24}h`;
}

export function temperature(value) {
  return typeof value === 'number' && Number.isFinite(value)
    ? `${value.toLocaleString('en', { maximumFractionDigits: 3 })} °C` : 'Not received';
}

export function operationNotice(pending) {
  if (!pending) return '';
  const destination = pending.new_ip && pending.new_ip !== pending.old_ip ? ` Checking ${pending.old_ip} and ${pending.new_ip}.` : '';
  return pending.state === 'accepted'
    ? `Command accepted. Waiting for the controller to finish restarting and confirm it.${destination}`
    : `Command outcome is unconfirmed. Your settings are kept here. Check status before retrying the same command.${destination}`;
}
