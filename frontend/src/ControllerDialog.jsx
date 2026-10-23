import React, { useEffect, useRef, useState } from 'react';
import { api, roomPath } from './api';
import { controllerDraft, controllerPatch, elapsed, temperature, operationNotice } from './controller.mjs';

function Setting({ label, hint, children }) {
  return <label className="field"><span>{label}</span>{children}{hint && <small>{hint}</small>}</label>;
}

function Reading({ label, children, hint }) {
  return <div className="controller-reading"><dt>{label}</dt><dd>{children}{hint && <small>{hint}</small>}</dd></div>;
}

export default function ControllerDialog({ house, room, onClose, onChanged }) {
  const setup = !room;
  const base = setup ? '/controllers/setup' : `${roomPath(house.id, room.id)}/controller`;
  const dialog = useRef(null);
  const changed = useRef(onChanged);
  changed.current = onChanged;
  const live = useRef(true);
  const requests = useRef(new Set());
  const refreshNow = useRef(() => {});
  const writing = useRef(false);
  const operationGeneration = useRef(0);
  const trackedCommand = useRef(null);
  const resetDraftOnStatus = useRef(false);
  const unknownOutcome = useRef(false);
  const [snapshot, setSnapshot] = useState(null);
  const [reachable, setReachable] = useState(false);
  const [receivedAt, setReceivedAt] = useState(null);
  const [pending, setPending] = useState(null);
  const [expectedCommand, setExpectedCommand] = useState(null);
  const [refreshing, setRefreshing] = useState(false);
  const [statusError, setStatusError] = useState('');
  const [commandError, setCommandError] = useState('');
  const [confirmation, setConfirmation] = useState('');
  const [draft, setDraft] = useState(null);
  const [passwordMode, setPasswordMode] = useState('keep');
  const [password, setPassword] = useState('');
  const [tab, setTab] = useState('status');
  const [busy, setBusy] = useState(false);
  const [confirmRestart, setConfirmRestart] = useState(false);

  useEffect(() => {
    live.current = true;
    dialog.current.showModal();
    return () => { live.current = false; requests.current.forEach(controller => controller.abort()); };
  }, []);

  useEffect(() => {
    let cancelled = false, timer, activeRequest = null;
    async function refresh() {
      if (activeRequest || cancelled || writing.current) return;
      clearTimeout(timer);
      const controller = new AbortController();
      activeRequest = controller;
      const generation = operationGeneration.current;
      requests.current.add(controller);
      // The Pi may check both the old and new IP, with 15 seconds per IP.
      const timeout = setTimeout(() => controller.abort(), 35000);
      setRefreshing(true);
      try {
        const result = await api(`${base}/status`, { signal: controller.signal });
        if (cancelled || generation !== operationGeneration.current) return;
        setReachable(result.reachable === true);
        setPending(result.pending || null);
        if (result.pending) {
          trackedCommand.current = result.pending.command_id;
          setExpectedCommand(result.pending.command_id);
          unknownOutcome.current = false;
        }
        setStatusError(result.error?.message || (!result.reachable ? 'The controller could not be reached.' : ''));
        if (result.reachable && result.status) {
          setSnapshot(result.status);
          setReceivedAt(new Date());
          const resetDraft = resetDraftOnStatus.current;
          setDraft(current => !current || resetDraft ? controllerDraft(result.status.config) : current);
          if (resetDraft) { setPassword(''); setPasswordMode('keep'); resetDraftOnStatus.current = false; }
        }
        const confirmed = result.confirmed_command_id || (!result.pending && trackedCommand.current && result.status?.last_command_id === trackedCommand.current && !result.status.restart_pending);
        if (confirmed) {
          trackedCommand.current = null;
          unknownOutcome.current = false;
          setExpectedCommand(null);
          setConfirmation('The controller confirmed the command and is responding after restart.');
          setCommandError('');
          if (result.status) { setDraft(controllerDraft(result.status.config)); setPassword(''); setPasswordMode('keep'); }
          changed.current?.();
        } else if (unknownOutcome.current && result.reachable && !result.pending) {
          unknownOutcome.current = false;
          setExpectedCommand(null);
          setCommandError('Delivery could not be confirmed. The Pi reports no pending command; review the reported settings before submitting again. Your form is preserved.');
        }
      } catch (error) {
        if (cancelled || generation !== operationGeneration.current) return;
        setReachable(false);
        setStatusError(error.name === 'AbortError' ? 'The status request timed out. Last received values may be out of date.' : error.message);
      } finally {
        clearTimeout(timeout);
        requests.current.delete(controller);
        activeRequest = null;
        if (!cancelled) { setRefreshing(false); timer = setTimeout(refresh, 5000); }
      }
    }
    refreshNow.current = refresh;
    refresh();
    return () => { cancelled = true; clearTimeout(timer); activeRequest?.abort(); };
  }, [base]);

  async function command(action, body) {
    writing.current = true;
    operationGeneration.current += 1;
    setBusy(true); setCommandError(''); setConfirmation(''); setConfirmRestart(false);
    const controller = new AbortController();
    requests.current.add(controller);
    const timeout = setTimeout(() => controller.abort(), 50000);
    try {
      const result = await api(`${base}/${action}`, { method: action === 'config' ? 'PATCH' : 'POST', body, signal: controller.signal });
      if (!live.current) return;
      setPending(result.pending || null);
      setExpectedCommand(result.confirmed ? null : result.command_id);
      trackedCommand.current = result.confirmed ? null : result.command_id;
      unknownOutcome.current = false;
      if (result.confirmed) {
        resetDraftOnStatus.current = true;
        setConfirmation('The controller confirmed the command and is responding after restart.');
      }
      // The Pi may have updated the room address or sensor assignment already.
      changed.current?.();
    } catch (error) {
      if (!live.current) return;
      if (error.details?.pending) {
        setPending(error.details.pending);
        setExpectedCommand(error.details.pending.command_id);
        trackedCommand.current = error.details.pending.command_id;
        changed.current?.();
      } else if (error.details && Object.hasOwn(error.details, 'pending')) {
        // An explicit rejection removed the reservation and rolled back its assignment.
        setPending(null); setExpectedCommand(null); trackedCommand.current = null;
        unknownOutcome.current = false;
        changed.current?.();
      } else if (!error.status) {
        unknownOutcome.current = true;
        setExpectedCommand('unknown');
      }
      setCommandError(error.name === 'AbortError' || !error.status
        ? 'The request timed out; delivery is unconfirmed. Your form is preserved. Refresh status before sending another command.'
        : error.message);
    } finally {
      clearTimeout(timeout);
      requests.current.delete(controller);
      writing.current = false;
      if (live.current) { setBusy(false); refreshNow.current(); }
    }
  }

  async function save(event) {
    event.preventDefault();
    try { await command('config', controllerPatch(draft, snapshot.config, passwordMode, password)); }
    catch (error) { setCommandError(error.message); }
  }
  function useReported() {
    setDraft(controllerDraft(snapshot.config)); setPassword(''); setPasswordMode('keep'); setCommandError('');
  }
  const locked = busy || Boolean(pending) || Boolean(expectedCommand);
  const update = key => event => setDraft(current => ({ ...current, [key]: event.target.value }));
  const config = snapshot?.config;
  const stale = snapshot && !reachable;
  const ageSuffix = stale ? ' at last contact' : ' ago';
  return <dialog ref={dialog} className="dialog controller-dialog" onCancel={event => { event.preventDefault(); onClose(); }} aria-labelledby="controller-title">
    <div className="dialog-head"><div><span className="eyebrow">CONTROLLER MANAGEMENT</span><h2 id="controller-title">{setup ? 'Set up a controller' : `${room.name} controller`}</h2><p className="controller-address">{setup ? 'Default address · 192.168.0.230' : `Assigned address · ${room.controller_ip}`}</p></div><button className="icon-button" aria-label="Close controller window" onClick={onClose}>✕</button></div>
    {setup && <section className="setup-guide" aria-label="First-time Wi-Fi setup"><h3>Start with the controller’s own web page</h3><ol><li>Power on one new controller. Connect your phone or computer to its open <strong>BMS-Setup-…</strong> Wi-Fi network.</li><li>Open <a href="http://192.168.0.230" target="_blank" rel="noreferrer">http://192.168.0.230</a> and enter your home Wi-Fi credentials and network settings.</li><li>Return to your home Wi-Fi. If you kept the default IP, use this window to assign a unique IP. If you already assigned a unique IP on the setup page, add the controller to a room using that IP, then open its Controller status window.</li></ol><p>Configure one default-address controller at a time. The Raspberry Pi and controller must be on the same 192.168.0.x network to reach the default address.</p></section>}
    <div className="controller-toolbar"><span className={`connection-state ${reachable ? 'connected' : 'disconnected'}`}><span className="status-dot"/>{reachable ? 'Controller reachable' : snapshot ? 'Offline · last known status' : 'No live status yet'}</span><button className="button secondary" type="button" disabled={refreshing || busy} onClick={() => refreshNow.current()}>{refreshing ? 'Checking…' : 'Refresh status'}</button></div>
    <p className="controller-refresh">{receivedAt ? `Last received ${receivedAt.toLocaleTimeString()}. ` : ''}Status refreshes every 5 seconds while this window is open.</p>
    {statusError && <p className="error controller-message" role="status">{statusError}</p>}
    {(pending || expectedCommand) && <div className="operation-notice" role="status"><p>{pending ? operationNotice(pending) : expectedCommand === 'unknown' ? 'Delivery is unconfirmed. Checking the Pi for a pending operation before another command is allowed.' : 'Command accepted. Waiting for the controller to confirm it after restart.'}</p>{pending && <button type="button" className="button secondary" disabled={busy || refreshing} onClick={() => command('retry', {})}>Check and retry same command</button>}</div>}
    {confirmation && <p className="controller-confirmation" role="status">{confirmation}</p>}
    {commandError && <p className="error controller-message" role="alert">{commandError}</p>}
    <div className="controller-tabs" role="tablist" aria-label="Controller details"><button type="button" role="tab" id="status-tab" aria-controls="controller-status" aria-selected={tab === 'status'} onClick={() => setTab('status')}>Live status</button><button type="button" role="tab" id="settings-tab" aria-controls="controller-settings" aria-selected={tab === 'settings'} onClick={() => setTab('settings')}>Controller settings</button></div>
    <section id="controller-status" role="tabpanel" aria-labelledby="status-tab" hidden={tab !== 'status'}>
      {snapshot ? <><p className="status-caption">{stale ? 'Values below are from the last successful contact.' : 'Reported by the controller.'}</p><dl className="controller-readings">
        <Reading label="Wi-Fi" hint={`Radio status ${snapshot.wifi_status ?? 'unknown'}`}>{snapshot.wifi_connected ? 'Connected' : 'Disconnected'}</Reading>
        <Reading label="Current IP">{snapshot.current_ip || 'Unavailable'}</Reading>
        <Reading label="Heating">{snapshot.heater_on === true ? 'ON' : snapshot.heater_on === false ? 'OFF' : 'Unknown'}</Reading>
        <Reading label="RuuviTag temperature" hint={snapshot.sensor_age_ms == null ? 'No assigned-sensor reading received' : `${elapsed(snapshot.sensor_age_ms)}${ageSuffix}${snapshot.sensor_age_ms > 30000 ? ' · stale' : ''}`}>{temperature(snapshot.sensor_temperature)}</Reading>
        <Reading label="Target temperature">{temperature(snapshot.target_temperature)}</Reading>
        <Reading label="Heating deltas">On −{snapshot.negative_delta ?? '?'}° / Off +{snapshot.positive_delta ?? '?'}°</Reading>
        <Reading label="Settings source" hint={snapshot.api_age_ms == null ? 'No API configuration received' : `API received ${elapsed(snapshot.api_age_ms)}${ageSuffix}`}>{snapshot.settings_source === 'api' ? 'API configuration' : snapshot.settings_source === 'defaults' ? 'Temperature defaults' : snapshot.settings_source || 'Unknown'}</Reading>
        <Reading label="Assigned sensor">{config?.sensor_mac || 'Not assigned'}</Reading>
        <Reading label="Bluetooth" hint={snapshot.ble_age_ms == null ? 'No advertisement received' : `Any advertisement ${elapsed(snapshot.ble_age_ms)}${ageSuffix}`}>{snapshot.ble_scanning ? 'Scanning' : 'Not scanning'}</Reading>
        <Reading label="NINA firmware">{snapshot.nina_firmware || 'Unavailable'}</Reading>
        <Reading label="Uptime">{elapsed(snapshot.uptime_ms)}</Reading>
        <Reading label="Operating mode">{snapshot.setup_mode ? 'Wi-Fi setup' : 'Normal control'}</Reading>
      </dl><p className="form-note controller-device-id">Device {snapshot.device_id || 'unknown'}{snapshot.restart_pending ? ' · restart pending' : ''}</p></> : <p className="form-note controller-empty">{refreshing ? 'Contacting the controller…' : 'No controller status has been received. Check its power, Wi-Fi connection and assigned IP.'}</p>}
      <p className="form-note">The controller restarts after 10 continuous minutes without Wi-Fi or any Bluetooth advertisement. Heating turns ON after more than 30 seconds without a valid reading from the assigned sensor.</p>
      {!confirmRestart ? <button type="button" className="button secondary" disabled={locked || !reachable} onClick={() => setConfirmRestart(true)}>Restart controller</button> : <div className="restart-confirm"><p>Restart {setup ? 'this controller at 192.168.0.230' : `the controller for ${room.name}`}? Heating starts ON and remains ON until valid sensor readings allow normal regulation.</p><div><button type="button" className="button secondary" onClick={() => setConfirmRestart(false)}>Cancel</button><button type="button" className="button primary" disabled={locked || !reachable} onClick={() => command('restart', {})}>Restart now</button></div></div>}
    </section>
    <section id="controller-settings" role="tabpanel" aria-labelledby="settings-tab" hidden={tab !== 'settings'}>
      {draft && config ? <form onSubmit={save}><fieldset disabled={locked}>
        <div className="controller-form-heading"><h3>Wi-Fi connection</h3><button type="button" className="text-button" onClick={useReported}>Use reported settings</button></div>
        <Setting label="Wi-Fi network (SSID)"><input required maxLength={32} value={draft.ssid} onChange={update('ssid')} autoComplete="off"/></Setting>
        <Setting label="Wi-Fi password" hint={config.password_set ? 'A password is saved. It is never returned by the controller.' : 'No password is currently saved.'}><select value={passwordMode} onChange={event => { setPasswordMode(event.target.value); setPassword(''); }}><option value="keep">Keep current password</option><option value="replace">Set a new password</option><option value="open">Use an open network (no password)</option></select></Setting>
        {passwordMode === 'replace' && <Setting label="New Wi-Fi password" hint="8 to 63 bytes."><input type="password" required minLength={8} maxLength={63} value={password} onChange={event => setPassword(event.target.value)} autoComplete="new-password"/></Setting>}
        <h3 className="controller-form-heading">Static network address</h3>
        <div className="form-grid"><Setting label="Controller IP"><input required value={draft.ip} onChange={update('ip')} placeholder="192.168.0.211" inputMode="decimal"/></Setting><Setting label="Subnet mask"><input required value={draft.subnet} onChange={update('subnet')} placeholder="255.255.255.0" inputMode="decimal"/></Setting></div>
        <div className="form-grid"><Setting label="Gateway"><input required value={draft.gateway} onChange={update('gateway')} placeholder="192.168.0.1" inputMode="decimal"/></Setting><Setting label="DNS server"><input required value={draft.dns} onChange={update('dns')} placeholder="192.168.0.1" inputMode="decimal"/></Setting></div>
        <h3 className="controller-form-heading">API and temperature sensor</h3>
        <div className="form-grid"><Setting label="API host" hint="Raspberry Pi IP address or hostname, without http:// or a path."><input required maxLength={127} value={draft.api_host} onChange={update('api_host')} placeholder="192.168.0.11"/></Setting><Setting label="API port"><input required type="number" min="1" max="65535" step="1" value={draft.api_port} onChange={update('api_port')}/></Setting></div>
        <Setting label="Assigned RuuviTag MAC" hint={setup ? 'Optional during first-time setup. Assign a sensor before normal use.' : 'Also updates this room’s sensor assignment.'}><input required={!setup} value={draft.sensor_mac} onChange={update('sensor_mac')} pattern="([0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}" placeholder="AA:BB:CC:DD:EE:FF"/></Setting>
        <p className="form-note">Saving restarts the controller. Heating starts ON. A changed IP is also saved in the room assignment; status checks both addresses until the controller confirms. Wi-Fi credentials are sent only when you change them.</p>
      </fieldset><div className="dialog-actions"><button type="button" className="button secondary" onClick={onClose}>Close</button><button type="submit" className="button primary" disabled={locked || !reachable}>{busy ? 'Sending…' : 'Save and restart'}</button></div></form> : <p className="form-note controller-empty">Settings become available after a successful status check. Your Wi-Fi password is never displayed.</p>}
    </section>
  </dialog>;
}
