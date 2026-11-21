#pragma once

// Self-contained so opening the dashboard never contacts another server.
static const char kWebDashboard[] = R"HTML(<!doctype html>
<html lang="en"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Macintosh console</title>
<style>
:root{font:16px/1.45 ui-sans-serif,system-ui,sans-serif;color:#eeeae0;background:#222521;color-scheme:dark}
body{margin:0}main{max-width:1024px;margin:0 auto;padding:clamp(16px,4vw,60px)}
header{border-bottom:1px solid #626659;margin-bottom:24px}h1{font-size:clamp(26px,4vw,42px);letter-spacing:-.04em;margin:0}
#device{color:#bfbeaf;margin:3px 0 16px}section{display:grid;grid-template-columns:minmax(0,2.4fr) minmax(170px,1fr);gap:24px}
canvas{width:100%;height:auto;display:block;background:#121411;image-rendering:pixelated;outline:1px solid #54594b}
canvas.input{cursor:crosshair}.actions{display:flex;flex-wrap:wrap;gap:8px;margin:14px 0}
button{font:inherit;font-weight:550;border:1px solid #74786a;background:#32372e;padding:6px 12px;border-radius:4px;cursor:pointer}
textarea,select{font:inherit;border:1px solid #74786a;background:#272b24;color:inherit;border-radius:4px;padding:5px}textarea{width:100%;box-sizing:border-box}label{align-self:center}button:disabled{opacity:.55;cursor:default}button:hover:enabled{background:#41483b}
h2{font-size:18px;margin:0 0 8px}dl{margin:0 0 18px}dt{font-size:13px;color:#bebfaf}dd{margin:0 0 8px;font-variant-numeric:tabular-nums}
#notice,#result{min-height:1.5em}#notice{color:#efcda7}[hidden]{display:none!important}
@media(max-width:680px){section{grid-template-columns:1fr}}
</style>
<main><header><h1>Macintosh console</h1><p id="device">Connecting…</p></header><section>
<div><canvas id="screen" width="640" height="360" aria-label="Guest Macintosh screen"></canvas>
<div class="actions"><button id="refresh">Refresh screen</button><label><input type="checkbox" id="live"> Live view</label></div>
<div id="input" hidden><div class="actions"><label>Pointer <select id="pointer"><option value="click">Click</option>
<option value="double-click">Double-click</option></select></label><span id="keys">
<button data-key="36">Return</button> <button data-key="53">Escape</button> <button data-key="51">Delete</button></span></div>
<label for="text">Guest text (up to 64 ASCII characters)</label>
<textarea id="text" maxlength="64" rows="2"></textarea><button id="type">Type in guest</button></div>
<p id="notice" role="status"></p></div>
<aside><h2>Device</h2><dl><dt>Free internal memory</dt><dd id="internal">—</dd>
<dt>Free PSRAM</dt><dd id="psram">—</dd><dt>Uptime</dt><dd id="up">—</dd>
<dt>Access</dt><dd id="mode">—</dd><dt>Firmware updates</dt><dd id="updates">—</dd></dl>
<h2>Guest speed</h2><button id="measure">Measure for 15 s</button><p id="result" role="status"></p></aside>
</section></main>
<script>
'use strict';
const el=id=>document.getElementById(id), canvas=el('screen'), ctx=canvas.getContext('2d');
const token=location.hash.slice(1); history.replaceState(null,'',location.pathname);
let canControl=false, busy=false, timer=0, frameInFlight=null;
function decodeFrame(bytes,w,h){
  if(w<1 || h<1 || w>1280 || h>1280 || bytes.length<513) throw Error('Invalid frame size.');
  const out=new Uint8Array(w*h);
  let i=512,j=0;
  while(i<bytes.length){
    const code=bytes[i++];
    if(code&128){
      const length=(code&127)+4;
      if(i+2>bytes.length) throw Error('Incomplete frame.');
      const distance=bytes[i++] | (bytes[i++]<<8);
      if(!distance || distance>j || j+length>out.length) throw Error('Invalid frame.');
      for(let k=0;k<length;k++,j++) out[j]=out[j-distance];
    }else{
      const length=code+1;
      if(i+length>bytes.length || j+length>out.length) throw Error('Incomplete frame.');
      out.set(bytes.subarray(i,i+length),j);i+=length;j+=length;
    }
  }
  if(j!==out.length) throw Error('Incomplete frame.');
  const rgba=new Uint8ClampedArray(out.length*4);
  for(let k=0;k<out.length;k++){
    const p=out[k]*2, c=bytes[p]|bytes[p+1]<<8;
    rgba[4*k]=(c>>11)*255/31;rgba[4*k+1]=(c>>5&63)*255/63;
    rgba[4*k+2]=(c&31)*255/31;rgba[4*k+3]=255;
  }
  return rgba;
}
async function api(path,method='GET',body){
  const r=await fetch('/desk/'+path,{method,body,headers:{'X-M5Tab-Session':token},cache:'no-store',referrerPolicy:'no-referrer'});
  if(!r.ok) throw Error(r.status===403?'Session closed or permission denied. Open a new link over USB.':
                        r.status===409?'Device busy. Try again.':'Device unavailable ('+r.status+').');
  return r;
}
async function status(){
  const s=await (await api('status')).json();
  el('device').textContent=s.board+' · '+s.version;
  el('updates').textContent=s.ota_slots===2?'Wi-Fi update available. Authorize over USB in Startup Settings.':'Install over USB (single application slot).';
  el('internal').textContent=(s.internal/1024).toFixed(0)+' KiB';
  el('psram').textContent=(s.psram/1048576).toFixed(1)+' MiB';
  el('up').textContent=Math.floor(s.uptime_sec/60)+' min';
  canControl=s.control;el('mode').textContent=canControl?'View and control':'View only';
  el('input').hidden=!canControl;canvas.classList.toggle('input',canControl);return s;
}
async function drawFrame(){
  const r=await api('frame'), bytes=new Uint8Array(await r.arrayBuffer());
  const w=Number(r.headers.get('X-B2-Width')),h=Number(r.headers.get('X-B2-Height'));
  const rgba=decodeFrame(bytes,w,h);canvas.width=w;canvas.height=h;ctx.putImageData(new ImageData(rgba,w,h),0,0);
}
function frame(){
  if(!frameInFlight) frameInFlight=drawFrame().finally(()=>{frameInFlight=null});
  return frameInFlight;
}
function planLive(){
  clearTimeout(timer);
  if(!busy && el('live').checked && !document.hidden){
    timer=setTimeout(async()=>{
      try{await frame()}catch(e){el('notice').textContent=e.message;el('live').checked=false}
      planLive();
    },3000);
  }
}
el('live').onchange=planLive;
document.addEventListener('visibilitychange',planLive);
async function action(fn){
  if(busy) return;
  busy=true;clearTimeout(timer);el('notice').textContent='';
  document.querySelectorAll('button').forEach(b=>b.disabled=true);
  try{
    if(frameInFlight) await frameInFlight;
    await fn();
  }catch(e){el('notice').textContent=e.message}
  finally{busy=false;document.querySelectorAll('button').forEach(b=>b.disabled=false);planLive()}
}
async function refresh(){await status();await frame()}
const wait=ms=>new Promise(resolve=>setTimeout(resolve,ms));
el('refresh').onclick=()=>action(refresh);
canvas.onclick=e=>{
  if(!canControl) return;
  const rect=canvas.getBoundingClientRect();
  const x=Math.floor((e.clientX-rect.left)*canvas.width/rect.width);
  const y=Math.floor((e.clientY-rect.top)*canvas.height/rect.height);
  action(async()=>{await api(el('pointer').value+'/'+x+'/'+y,'POST');await wait(200);await frame()});
};
el('keys').onclick=e=>{if(e.target.dataset.key) action(async()=>{
  await api('key/'+e.target.dataset.key,'POST');await wait(200);await frame()})};
el('type').onclick=()=>action(async()=>{
  const field=el('text'), text=field.value;
  if(!/^[\x20-\x7e\t\n]{1,64}$/.test(text)) throw Error('Enter 1–64 ASCII characters.');
  await api('text','POST',text);
  if(field.value===text) field.value='';
  await wait(150);await frame();
});
el('measure').onclick=()=>action(async()=>{
  el('result').textContent='Measuring guest instructions…';
  try{
    const a=await status();if(!a.sample_ms) throw Error('CPU sample unavailable. Try again after startup.');
    await wait(15000);const b=await status(),ms=b.sample_ms-a.sample_ms;
    if(ms<=0 || b.instructions<a.instructions) throw Error('No new CPU sample.');
    el('result').textContent=((b.instructions-a.instructions)/ms/1000).toFixed(2)+' MIPS over '+(ms/1000).toFixed(1)+' s';
  }catch(e){el('result').textContent='';throw e}
});
if(/^[0-9a-f]{32}$/.test(token)) action(refresh);
else {el('device').textContent='Session required';el('notice').textContent='Open the complete link issued by web-open.'}
</script></html>)HTML";
