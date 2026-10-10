import React, { useEffect, useRef, useState } from 'react';
import { createRoot } from 'react-dom/client';
import { api, roomPath, programPath } from './api';
import { DAYS, shortDay, offsetMinutes, toUtc, toLocal, activeProgram } from './schedule.mjs';
import { newId } from './identifiers.mjs';
import './styles.css';

function Icon({ name, size = 20 }) {
  const paths = {
    home: <><path d="m3 10 9-7 9 7v10H3z"/><path d="M9 20v-7h6v7"/></>,
    heat: <><path d="M8 20V5m4 15V3m4 17V5M5 8h14M5 17h14"/></>,
    plus: <path d="M12 5v14M5 12h14"/>,
    arrow: <path d="m9 5 7 7-7 7"/>,
    settings: <><path d="M4 7h16M4 17h16"/><circle cx="9" cy="7" r="3"/><circle cx="15" cy="17" r="3"/></>,
    clock: <><circle cx="12" cy="12" r="9"/><path d="M12 6v6l4 2"/></>,
    network: <><rect x="8" y="3" width="8" height="5" rx="1"/><path d="M12 8v6M4 14h16M4 14v3m8-3v3m8-3v3"/><path d="M2 17h4v4H2zm8 0h4v4h-4zm8 0h4v4h-4z"/></>,
    room: <><path d="M5 21V3h14v18M3 21h18"/><circle cx="15" cy="12" r=".5"/></>,
    edit: <><path d="m4 16 12-12 4 4L8 20H4zM13 7l4 4"/></>,
    trash: <><path d="M3 6h18M9 6V3h6v3M5 6l1 15h12l1-15M10 10v7m4-7v7"/></>,
    close: <path d="m6 6 12 12M6 18 18 6"/>,
    check: <path d="m5 12 4 4L19 6"/>,
  };
  return <svg width={size} height={size} viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.6" strokeLinecap="round" strokeLinejoin="round" aria-hidden="true">{paths[name] || paths.home}</svg>;
}

const degrees = number => Number(number).toLocaleString('en', { maximumFractionDigits: 2 });
const daysLabel = days => !days || days.length === 7 ? 'Every day' : days.map(shortDay).join(' · ');

function Dialog({ title, children, onClose, wide = false }) {
  const ref = useRef(null);
  useEffect(() => { ref.current.showModal(); }, []);
  return <dialog ref={ref} className={wide ? 'dialog wide' : 'dialog'} onCancel={event => { event.preventDefault(); onClose(); }} aria-labelledby="dialog-title">
    <div className="dialog-head"><div><span className="eyebrow">HEATING CONFIGURATION</span><h2 id="dialog-title">{title}</h2></div><button className="icon-button" aria-label="Close dialog" onClick={onClose}><Icon name="close"/></button></div>
    {children}
  </dialog>;
}

function Field({ label, children, hint }) {
  return <label className="field"><span>{label}</span>{children}{hint && <small>{hint}</small>}</label>;
}

function FormShell({ children, error, busy, onSubmit, onClose, submit = 'Save changes' }) {
  return <form onSubmit={onSubmit}>
    <fieldset disabled={busy}>{children}</fieldset>
    {error && <p className="error" role="alert">{error}</p>}
    <div className="dialog-actions"><button type="button" className="button secondary" disabled={busy} onClick={onClose}>Cancel</button><button className="button primary" disabled={busy}>{busy ? 'Saving…' : submit}</button></div>
  </form>;
}

function HouseForm({ onClose, onSave }) {
  const identifier = useRef(newId('house')).current;
  const [name, setName] = useState('');
  const [timezone, setTimezone] = useState(Intl.DateTimeFormat().resolvedOptions().timeZone || 'UTC');
  const [error, setError] = useState('');
  const [busy, setBusy] = useState(false);
  async function submit(event) {
    event.preventDefault(); setBusy(true); setError('');
    try {
      new Intl.DateTimeFormat('en', { timeZone: timezone });
      const house = await api(`/set_config?id=${encodeURIComponent(identifier)}`, { method: 'PUT', body: { name: name.trim(), timezone, rooms: [] } });
      await onSave(house); onClose();
    } catch (err) { setError(err.message); } finally { setBusy(false); }
  }
  return <Dialog title="Add a household" onClose={onClose}><FormShell {...{ error, busy, onClose }} onSubmit={submit} submit="Create household">
    <Field label="Household name"><input autoFocus required maxLength={128} value={name} onChange={e => setName(e.target.value)} placeholder="e.g. Main house"/></Field>
    <Field label="Local timezone" hint="Used to display your schedules in local time."><input required value={timezone} onChange={e => setTimezone(e.target.value)} list="timezones"/><datalist id="timezones">{['Europe/Bucharest', 'Europe/London', 'Europe/Berlin', 'America/New_York', 'UTC'].map(t => <option key={t} value={t}/>)}</datalist></Field>
  </FormShell></Dialog>;
}

function RoomForm({ house, room, onClose, onSave }) {
  const identifier = useRef(newId('room')).current;
  const [values, setValues] = useState(room ? { name: room.name, controller_ip: room.controller_ip, sensor_mac: room.sensor_mac, negative_delta: room.negative_delta, positive_delta: room.positive_delta } : { name: '', controller_ip: '', sensor_mac: '', negative_delta: 0.1, positive_delta: 0.1 });
  const [busy, setBusy] = useState(false), [error, setError] = useState('');
  const update = key => e => setValues(v => ({ ...v, [key]: e.target.value }));
  async function submit(event) {
    event.preventDefault(); setBusy(true); setError('');
    try {
      const body = { ...values, name: values.name.trim(), sensor_mac: values.sensor_mac.trim(), controller_ip: values.controller_ip.trim(), negative_delta: Number(values.negative_delta), positive_delta: Number(values.positive_delta) };
      if (!room) body.id = identifier;
      await api(roomPath(house.id, room?.id), { method: room ? 'PATCH' : 'POST', body });
      await onSave(); onClose();
    } catch (err) { setError(err.message); } finally { setBusy(false); }
  }
  return <Dialog title={room ? 'Room settings' : 'Add a room'} onClose={onClose}><FormShell {...{ error, busy, onClose }} onSubmit={submit} submit={room ? 'Save room' : 'Add room'}>
    <Field label="Room name"><input autoFocus required maxLength={128} placeholder="e.g. Living room" value={values.name} onChange={update('name')}/></Field>
    <div className="form-grid"><Field label="Controller IP" hint="Must match the static IP configured through serial."><input required placeholder="192.168.1.50" value={values.controller_ip} onChange={update('controller_ip')}/></Field><Field label="RuuviTag MAC" hint="Changes reach the controller on its next poll."><input required placeholder="AA:BB:CC:DD:EE:FF" value={values.sensor_mac} onChange={update('sensor_mac')} pattern="([0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}"/></Field></div>
    <div className="form-grid"><Field label="Heating-on delta (°C)"><input required type="number" min="0" step="any" value={values.negative_delta} onChange={update('negative_delta')}/></Field><Field label="Heating-off delta (°C)"><input required type="number" min="0" step="any" value={values.positive_delta} onChange={update('positive_delta')}/></Field></div>
    <p className="form-note">Heating turns on below target minus the on delta, and off above target plus the off delta.</p>
  </FormShell></Dialog>;
}

function ProgramForm({ house, room, program, onClose, onSave }) {
  const identifier = useRef(newId('program')).current;
  const initial = useRef(program ? toLocal(program, house.timezone) : { time: '07:00', days: DAYS }).current;
  const [time, setTime] = useState(initial.time), [days, setDays] = useState(initial.days);
  const [target, setTarget] = useState(program?.target_temperature ?? 23);
  const [busy, setBusy] = useState(false), [error, setError] = useState('');
  const scheduleChanged = time !== initial.time || days.join(',') !== initial.days.join(',');
  const converted = toUtc(time || '00:00', days, offsetMinutes(house.timezone));
  const preview = program && !scheduleChanged ? program : converted;
  async function submit(event) {
    event.preventDefault(); setBusy(true); setError('');
    try {
      if (!days.length) throw new Error('Choose at least one day.');
      let body = { target_temperature: Number(target) };
      if (!program || scheduleChanged) {
        const schedule = toUtc(time, days, offsetMinutes(house.timezone));
        body = { ...body, ...schedule };
        if (program && !schedule.days) body.days = DAYS;
      }
      if (!program) body.id = identifier;
      await api(programPath(house.id, room.id, program?.id), { method: program ? 'PATCH' : 'POST', body });
      await onSave(); onClose();
    } catch (err) { setError(err.message); } finally { setBusy(false); }
  }
  const toggle = day => setDays(current => DAYS.filter(d => d === day ? !current.includes(d) : current.includes(d)));
  return <Dialog title={program ? 'Edit temperature program' : 'Add a temperature program'} onClose={onClose}><FormShell {...{ error, busy, onClose }} onSubmit={submit} submit={program ? 'Save program' : 'Add program'}>
    <div className="form-grid"><Field label={`Start time · ${house.timezone}`}><input autoFocus required type="time" value={time} onChange={e => setTime(e.target.value)}/></Field><Field label="Target temperature (°C)"><input required type="number" step="any" value={target} onChange={e => setTarget(e.target.value)}/></Field></div>
    <div className="field"><span>Repeat on local days</span><label className="check-label"><input type="checkbox" checked={days.length === 7} onChange={e => setDays(e.target.checked ? [...DAYS] : [])}/>Every day</label><div className="day-picker">{DAYS.map(day => <button key={day} type="button" aria-pressed={days.includes(day)} onClick={() => toggle(day)}>{shortDay(day)}</button>)}</div></div>
    <div className="utc-preview"><Icon name="clock"/><div><strong>{preview.start_time} UTC · {daysLabel(preview.days)}</strong><p>This timing stays fixed. Local times shift when daylight saving changes.</p></div></div>
    <p className="form-note">A day-specific program takes priority over an everyday program starting at the same time.</p>
  </FormShell></Dialog>;
}

function DeleteForm({ title, description, path, onClose, onSave }) {
  const [busy, setBusy] = useState(false), [error, setError] = useState('');
  async function submit(event) {
    event.preventDefault(); setBusy(true); setError('');
    try { await api(path, { method: 'DELETE' }); await onSave(); onClose(); }
    catch (err) { setError(err.message); } finally { setBusy(false); }
  }
  return <Dialog title={title} onClose={onClose}><FormShell {...{ error, busy, onClose }} onSubmit={submit} submit="Remove"><p className="delete-description">{description}</p></FormShell></Dialog>;
}

function App() {
  const [houses, setHouses] = useState([]), [selected, setSelected] = useState('');
  const [house, setHouse] = useState(null), [roomId, setRoomId] = useState('');
  const [modal, setModal] = useState(null), [error, setError] = useState('');
  const [loading, setLoading] = useState(true), [now, setNow] = useState(new Date());
  const [notice, setNotice] = useState('');
  const generation = useRef(0);
  async function loadList() {
    const list = await api('/get_house_list'); setHouses(list);
    setSelected(current => current && list.some(h => h.id === current) ? current : list[0]?.id || '');
    return list;
  }
  useEffect(() => { loadList().catch(err => setError(err.message)).finally(() => setLoading(false)); }, []);
  useEffect(() => { const timer = setInterval(() => setNow(new Date()), 15000); return () => clearInterval(timer); }, []);
  useEffect(() => {
    const serial = ++generation.current;
    setHouse(null); setError('');
    if (!selected) return;
    const controller = new AbortController(); setLoading(true);
    api(`/get_config?id=${encodeURIComponent(selected)}`, { signal: controller.signal }).then(value => {
      if (generation.current === serial) { setHouse(value); setRoomId(value.rooms[0]?.id || ''); }
    }).catch(err => { if (err.name !== 'AbortError') setError(err.message); }).finally(() => { if (generation.current === serial) setLoading(false); });
    return () => controller.abort();
  }, [selected]);
  useEffect(() => { if (!notice) return; const timer = setTimeout(() => setNotice(''), 4500); return () => clearTimeout(timer); }, [notice]);
  async function reload() {
    try {
      const value = await api(`/get_config?id=${encodeURIComponent(selected)}`);
      setHouse(value); setRoomId(current => value.rooms.some(r => r.id === current) ? current : value.rooms[0]?.id || '');
      setError(''); setNotice('Configuration saved');
    } catch (err) {
      // The mutation has succeeded. A failed refresh must not invite a duplicate write.
      setError(`Configuration was saved, but refreshing failed: ${err.message}`);
    }
  }
  async function created(value) {
    setHouses(current => [...current.filter(h => h.id !== value.id), { id: value.id, name: value.name }].sort((a, b) => a.name.localeCompare(b.name)));
    setSelected(value.id); setNotice('Household created');
  }
  const room = house?.rooms.find(r => r.id === roomId);
  const active = room ? activeProgram(room.programs, now) : null;
  const programCount = house?.rooms.reduce((sum, r) => sum + r.programs.length, 0) || 0;
  const localClock = house ? new Intl.DateTimeFormat('en-GB', { timeZone: house.timezone, hour: '2-digit', minute: '2-digit' }).format(now) : '';

  return <div className="app-shell">
    <aside className="sidebar">
      <a className="brand" href="/" aria-label="BMS home"><span className="brand-symbol"><Icon name="heat" size={26}/></span><span>BMS<small>HOME HEATING</small></span></a>
      <div className="sidebar-label">YOUR HOUSEHOLDS <span>{houses.length.toString().padStart(2, '0')}</span></div>
      <nav aria-label="Households">{houses.map(h => <button key={h.id} className={`house-link ${selected === h.id ? 'selected' : ''}`} onClick={() => { setSelected(h.id); setModal(null); }}><Icon name="home"/><span>{h.name}</span><Icon name="arrow" size={16}/></button>)}</nav>
      <button className="add-house" onClick={() => setModal({ type: 'house' })}><Icon name="plus" size={18}/>Add household</button>
      <div className="sidebar-footer"><div className="network-label"><span className="status-dot"/>LOCAL NETWORK</div><p>A comfortable home.<br/>A schedule that works for you.</p><span className="sidebar-version">BMS / 01</span></div>
    </aside>
    <main>
      <header className="topbar"><div><Icon name="home" size={17}/><span>Households</span>{house && <><span className="breadcrumb-slash">/</span><strong>{house.name}</strong></>}</div><span className="local-badge"><span className="status-dot"/>Local configuration</span></header>
      {notice && <div className="toast" role="status"><Icon name="check"/>{notice}</div>}
      <div className="main-content">
        {error && <div className="error page-error" role="alert"><span>{error}</span><button className="button secondary" onClick={() => window.location.reload()}>Retry</button></div>}
        {loading && <div className="loading" role="status">Loading your heating configuration…</div>}
        {!loading && !house && !error && <section className="welcome"><div className="welcome-icon"><Icon name="home" size={36}/></div><span className="eyebrow">WELCOME HOME</span><h1>Comfort starts here.</h1><p>Add your first household, connect its rooms,<br/>and give every space a temperature schedule.</p><button className="button primary" onClick={() => setModal({ type: 'house' })}><Icon name="plus"/>Add your first household</button><div className="welcome-features"><span><Icon name="room"/>Room-by-room settings</span><span><Icon name="clock"/>Daily & weekday schedules</span><span><Icon name="network"/>Your local network</span></div></section>}
        {house && <>
          <section className="page-heading"><div><span className="eyebrow">YOUR HOME, YOUR COMFORT</span><h1>{house.name}</h1><p>Make every room feel just right.</p></div><button className="button primary" onClick={() => setModal({ type: 'room' })}><Icon name="plus" size={18}/>Add room</button></section>
          <section className="summary-grid" aria-label="Household summary"><div className="summary-card"><span className="summary-icon"><Icon name="room"/></span><div><span>ROOMS</span><strong>{house.rooms.length.toString().padStart(2, '0')}</strong></div><small>Individual control</small></div><div className="summary-card"><span className="summary-icon amber"><Icon name="clock"/></span><div><span>PROGRAMS</span><strong>{programCount.toString().padStart(2, '0')}</strong></div><small>Daily & weekday</small></div><div className="summary-card timezone-card"><span className="summary-icon"><Icon name="clock"/></span><div><span>LOCAL TIME</span><strong>{localClock}</strong></div><small>{house.timezone}</small></div></section>
          <section className="rooms-section"><div className="section-heading"><h2>Your rooms <span>{house.rooms.length}</span></h2><span>Choose a room to manage its schedule</span></div>
            {!house.rooms.length ? <div className="empty-rooms"><Icon name="room" size={30}/><h3>A place for every room</h3><p>Add a room and assign its controller and temperature sensor.</p><button className="button secondary" onClick={() => setModal({ type: 'room' })}><Icon name="plus" size={18}/>Add room</button></div> : <div className="room-grid">{house.rooms.map(r => {
              const p = activeProgram(r.programs, now);
              return <button key={r.id} className={`room-card ${r.id === roomId ? 'active' : ''}`} onClick={() => setRoomId(r.id)}><div><span className="room-symbol"><Icon name="room"/></span><span className="room-card-arrow"><Icon name="arrow" size={17}/></span></div><h3>{r.name}</h3><div className="room-target">{degrees(p?.target_temperature ?? 23)}<span>°C</span></div><div className="room-card-bottom"><span>Scheduled target</span><span>{r.programs.length} {r.programs.length === 1 ? 'program' : 'programs'}</span></div></button>;
            })}</div>}
          </section>
          {room && <section className="room-detail" aria-label={`${room.name} configuration`}><div className="detail-heading"><div><span className="eyebrow">ROOM CONFIGURATION</span><h2>{room.name}</h2></div><div className="detail-buttons"><button className="button secondary" onClick={() => setModal({ type: 'room', room })}><Icon name="settings" size={17}/>Room settings</button><button className="icon-button" aria-label={`Remove ${room.name}`} onClick={() => setModal({ type: 'delete-room', room })}><Icon name="trash"/></button></div></div>
            <div className="device-strip"><span><Icon name="network" size={17}/><small>CONTROLLER</small><strong>{room.controller_ip}</strong></span><span><Icon name="heat" size={17}/><small>RUUVITAG</small><strong>{room.sensor_mac}</strong></span><span className="delta-detail">On −{degrees(room.negative_delta)}° / Off +{degrees(room.positive_delta)}°</span></div>
            <div className="schedule-heading"><div><h3>Temperature programs</h3><p>Local times · {house.timezone}</p></div><button className="button secondary" onClick={() => setModal({ type: 'program', room })}><Icon name="plus" size={17}/>Add program</button></div>
            {room.programs.length ? <div className="program-list">{[...room.programs].sort((a, b) => toLocal(a, house.timezone, now).time.localeCompare(toLocal(b, house.timezone, now).time)).map(p => {
              const local = toLocal(p, house.timezone, now);
              return <article className={`program-row ${p.id === active?.id ? 'current' : ''}`} key={p.id}><span className="program-icon"><Icon name="clock"/></span><div className="program-time"><strong>{local.time}</strong><span>{daysLabel(local.days)}</span></div><div className="program-temperature"><strong>{degrees(p.target_temperature)}<small>°C</small></strong><span>Target temperature</span></div><span className={`program-state ${p.id === active?.id ? 'is-active' : ''}`}>{p.id === active?.id ? 'Current program' : `${p.start_time} UTC`}</span><div className="row-actions"><button className="icon-button" aria-label={`Edit program at ${local.time}`} onClick={() => setModal({ type: 'program', room, program: p })}><Icon name="edit" size={18}/></button><button className="icon-button" aria-label={`Remove program at ${local.time}`} onClick={() => setModal({ type: 'delete-program', room, program: p })}><Icon name="trash" size={18}/></button></div></article>;
            })}</div> : <div className="empty-programs"><Icon name="clock"/><div><strong>No programs yet</strong><p>This room uses the default target of 23°C. Add a program to set its daily rhythm.</p></div></div>}
            <p className="schedule-note"><Icon name="clock" size={15}/>UTC timing stays fixed across daylight-saving changes. The latest program continues until the next starts.</p>
          </section>}
          <footer className="page-footer"><span>BMS / Household heating management</span><span>Changes reach controllers on their next one-minute poll.</span></footer>
        </>}
      </div>
    </main>
    {modal?.type === 'house' && <HouseForm onClose={() => setModal(null)} onSave={created}/>}
    {modal?.type === 'room' && house && <RoomForm house={house} room={modal.room} onClose={() => setModal(null)} onSave={reload}/>}
    {modal?.type === 'program' && house && <ProgramForm house={house} room={modal.room} program={modal.program} onClose={() => setModal(null)} onSave={reload}/>}
    {modal?.type === 'delete-room' && house && <DeleteForm title={`Remove ${modal.room.name}?`} description="This removes the room and its programs from this household. The controller continues using its last settings for up to five hours, then uses its temperature defaults." path={roomPath(house.id, modal.room.id)} onClose={() => setModal(null)} onSave={reload}/>}
    {modal?.type === 'delete-program' && house && <DeleteForm title="Remove this program?" description="The room will use the most recent remaining program, or 23°C if no programs remain." path={programPath(house.id, modal.room.id, modal.program.id)} onClose={() => setModal(null)} onSave={reload}/>}
  </div>;
}

createRoot(document.getElementById('root')).render(<App/>);
