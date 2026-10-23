#pragma once

const char SETUP_PAGE[] = R"BMS(<!doctype html>
<html lang="en"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>BMS controller setup</title><style>
:root{font-family:system-ui,sans-serif;color:#17352d;background:#eef4ef}*{box-sizing:border-box}body{margin:0;padding:24px 14px}main{max-width:660px;margin:auto;background:white;border:1px solid #d5e2d8;border-radius:20px;padding:clamp(20px,5vw,36px)}h1{font-size:1.8rem;margin:8px 0}p{line-height:1.5;color:#496258}.eyebrow{font-size:.75rem;letter-spacing:.12em;text-transform:uppercase;color:#397753}fieldset{border:0;padding:0;margin:24px 0}legend{font-weight:700;margin-bottom:12px}label{display:block;font-size:.9rem;margin:12px 0 5px}input{width:100%;font:inherit;border:1px solid #adc6b6;border-radius:8px;padding:10px;background:#fcfefc}input[type=checkbox]{width:auto;margin-right:8px}.grid{display:grid;grid-template-columns:1fr 1fr;gap:0 14px}button{font:inherit;font-weight:700;width:100%;padding:13px;border:0;border-radius:10px;color:white;background:#205e43;cursor:pointer}button:disabled{opacity:.6}#message{padding:12px;border-radius:9px;background:#edf5ef;overflow-wrap:anywhere}#identity{font-size:.8rem;overflow-wrap:anywhere}@media(max-width:440px){.grid{grid-template-columns:1fr}}
</style><main><div class="eyebrow">BMS · Local setup</div><h1>Connect your controller</h1>
<p>Enter your home Wi-Fi and network settings. Save to restart the controller, then return to your home Wi-Fi and finish assigning its room in BMS.</p>
<p id="identity">Reading controller status…</p><form id="form">
<fieldset><legend>Home Wi-Fi</legend><label for="ssid">Network name (SSID)</label><input id="ssid" maxlength="32" required autocomplete="off">
<label for="password">Wi-Fi password</label><input id="password" type="password" maxlength="63" autocomplete="new-password" placeholder="Leave blank to keep the saved password">
<label><input id="open" type="checkbox">This is an open Wi-Fi network</label></fieldset>
<fieldset><legend>Controller network</legend><p>Configure one new controller at a time. Give each controller a unique address before connecting another device with the default address.</p><div class="grid">
<div><label for="ip">Static IP address</label><input id="ip" required inputmode="decimal"></div>
<div><label for="subnet">Subnet mask</label><input id="subnet" required inputmode="decimal"></div>
<div><label for="gateway">Gateway</label><input id="gateway" required inputmode="decimal"></div>
<div><label for="dns">DNS server</label><input id="dns" required inputmode="decimal"></div></div></fieldset>
<fieldset><legend>BMS and sensor</legend><div class="grid"><div><label for="api_host">API host</label><input id="api_host" maxlength="127" required></div>
<div><label for="api_port">API port</label><input id="api_port" type="number" min="1" max="65535" required></div></div>
<label for="sensor_mac">RuuviTag MAC address (optional until assigned)</label><input id="sensor_mac" maxlength="17" placeholder="AA:BB:CC:DD:EE:FF" autocomplete="off"></fieldset>
<button id="save" disabled>Save and restart</button><p id="message" role="status" aria-live="polite">Loading settings…</p></form></main>
<script>
let device,pendingAttempt;const fields=['ssid','ip','subnet','gateway','dns','api_host','api_port','sensor_mac'];const msg=document.getElementById('message');const save=document.getElementById('save');
function commandId(){return 'xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx'.replace(/[xy]/g,c=>{let r=Math.random()*16|0;if(window.crypto&&crypto.getRandomValues){let b=new Uint8Array(1);crypto.getRandomValues(b);r=b[0]&15}return(c==='x'?r:(r&3|8)).toString(16)})}
fetch('/status',{cache:'no-store'}).then(r=>{if(!r.ok)throw Error('Could not read controller status.');return r.json()}).then(s=>{device=s.device_id;fields.forEach(k=>document.getElementById(k).value=s.config[k]);document.getElementById('identity').textContent='Controller '+device+' · Heating output '+(s.heater_on?'ON':'OFF');msg.textContent=s.setup_mode?'Connected to the temporary setup network.':'Connected to the controller on your home network.';save.disabled=false}).catch(e=>msg.textContent=e.message);
document.getElementById('form').addEventListener('submit',async e=>{e.preventDefault();save.disabled=true;msg.textContent='Saving settings…';let config={};fields.forEach(k=>config[k]=document.getElementById(k).value);config.api_port=Number(config.api_port);let password=document.getElementById('password').value;if(document.getElementById('open').checked)config.password='';else if(password)config.password=password;
const signature=JSON.stringify({device_id:device,config});if(!pendingAttempt||pendingAttempt.signature!==signature)pendingAttempt={signature,body:JSON.stringify({command_id:commandId(),device_id:device,config})};
try{let r=await fetch('/config',{method:'POST',headers:{'Content-Type':'application/json'},body:pendingAttempt.body});let result=await r.json();if(!r.ok)throw Error(result.error?.message||'Settings were rejected.');msg.textContent='Saved. The controller is restarting. Join your home Wi-Fi, then open BMS. Saved controller address: '+config.ip+'. If joining fails, reconnect to this setup network to correct the settings.';document.getElementById('password').value=''}catch(error){msg.textContent=error.message+' If the connection closed during restart, reconnect and check the settings before trying again.';save.disabled=false}});
</script></html>)BMS";
