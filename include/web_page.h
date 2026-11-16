#pragma once
#include <Arduino.h>

// Self-contained dashboard (no internet needed - works in hotspot mode too)
static const char INDEX_HTML[] PROGMEM = R"HTML(<!doctype html>
<html lang="en"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Tilt Sensor</title>
<style>
:root{--bg:#f4f5f7;--card:#fff;--text:#16181d;--muted:#6b7280;--line:#e5e7eb;
--ok:#1e9e48;--warn:#f0a500;--bad:#d32020}
@media (prefers-color-scheme:dark){:root{--bg:#0f1115;--card:#181b21;--text:#eceef2;--muted:#9aa1ad;--line:#2a2e36}}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--text);font:15px/1.4 system-ui,-apple-system,Segoe UI,Roboto,sans-serif}
main{max-width:720px;margin:0 auto;padding:16px}
h1{font-size:18px;margin:4px 0 14px;display:flex;justify-content:space-between;align-items:center}
#conn{font-size:12px;font-weight:500;color:var(--muted)}
#status{border-radius:16px;padding:22px;color:#fff;display:flex;gap:18px;align-items:center;
justify-content:space-between;transition:background .2s;background:var(--ok)}
#status.warn{background:var(--warn);color:#1a1300}
#status.bad{background:var(--bad);animation:pulse 1s infinite}
@keyframes pulse{50%{filter:brightness(.8)}}
#state{font-size:30px;font-weight:800;letter-spacing:.5px}
#msg{opacity:.9;margin-top:4px}
#deg{font-size:56px;font-weight:800;font-variant-numeric:tabular-nums;line-height:1}
.card{background:var(--card);border:1px solid var(--line);border-radius:14px;padding:16px;margin-top:14px}
.row{display:flex;gap:14px;flex-wrap:wrap;align-items:center}
.grow{flex:1;min-width:200px}
svg{display:block;margin:auto}
.k{color:var(--muted);font-size:13px}
.v{font-weight:700;font-variant-numeric:tabular-nums}
button{font:inherit;border:1px solid var(--line);background:var(--card);color:var(--text);
padding:9px 14px;border-radius:10px;cursor:pointer}
button:hover{border-color:var(--muted)}
button.primary{background:var(--text);color:var(--bg);border-color:var(--text)}
input[type=range]{width:100%}
ul{list-style:none;margin:0;padding:0}
li{padding:7px 0;border-top:1px solid var(--line);display:flex;justify-content:space-between}
li:first-child{border-top:0}
.dot{display:inline-block;width:9px;height:9px;border-radius:50%;margin-right:8px}
</style></head><body><main>
<h1><span>Tilt Sensor</span><span id="conn">connecting...</span></h1>

<div id="status"><div><div id="state">--</div><div id="msg"></div></div><div id="deg">--</div></div>

<div class="card row">
 <svg id="lvl" width="190" height="190" viewBox="-100 -100 200 200">
  <circle r="92" fill="none" stroke="currentColor" stroke-opacity=".25" stroke-width="2"/>
  <circle id="ring" r="46" fill="none" stroke="var(--bad)" stroke-dasharray="5 5" stroke-width="2"/>
  <line x1="-92" x2="92" stroke="currentColor" stroke-opacity=".15"/>
  <line y1="-92" y2="92" stroke="currentColor" stroke-opacity=".15"/>
  <circle id="bub" r="12" fill="var(--ok)"/>
 </svg>
 <div class="grow">
  <div class="row" style="justify-content:space-between">
   <div><div class="k">X tilt</div><div class="v" id="px">--</div></div>
   <div><div class="k">Y tilt</div><div class="v" id="py">--</div></div>
   <div><div class="k">Alarm limit</div><div class="v" id="lim">--</div></div>
  </div>
  <div style="margin-top:14px"><input id="slider" type="range" min="1" max="45" step="0.5"></div>
  <div class="row" style="margin-top:12px">
   <button class="primary" id="zero">Set current position as level</button>
   <button id="mute">Mute device</button>
   <button id="snd">Enable sound here</button>
  </div>
 </div>
</div>

<div class="card"><div class="k" style="margin-bottom:6px">Recent events</div><ul id="ev"><li class="k">No events yet</li></ul></div>
</main>
<script>
const $=id=>document.getElementById(id);
let last=null, actx=null, beepT=0, dragging=false;
function fmt(s){s=Math.round(s);if(s<60)return s+'s ago';if(s<3600)return Math.floor(s/60)+'m ago';return Math.floor(s/3600)+'h ago'}
function beep(){if(!actx)return;const o=actx.createOscillator(),g=actx.createGain();o.frequency.value=1800;
 g.gain.value=.15;o.connect(g).connect(actx.destination);o.start();o.stop(actx.currentTime+.18)}
async function post(u){try{await fetch(u,{method:'POST'});poll()}catch(e){}}
async function poll(){
 try{
  const r=await fetch('/api/status',{cache:'no-store'});const d=await r.json();last=d;
  $('conn').textContent='live - '+d.ip;
  const st=$('status');st.className=d.alarm?'bad':(d.warn?'warn':'');
  $('state').textContent=d.alarm?'TILTED - FIX IT':(d.warn?'CAREFUL':'LEVEL - OK');
  $('msg').textContent=d.alarm?'Tilt is over the '+d.limit+'° limit. Straighten it.':
    (d.warn?'Getting close to the '+d.limit+'° limit.':'Within the '+d.limit+'° limit.');
  $('deg').textContent=d.tilt.toFixed(1)+'°';
  $('px').textContent=d.x.toFixed(1)+'°';$('py').textContent=d.y.toFixed(1)+'°';
  $('lim').textContent=d.limit+'°';
  if(!dragging)$('slider').value=d.limit;
  const s=46/d.limit;let bx=d.x*s,by=d.y*s;const m=Math.hypot(bx,by);if(m>80){bx*=80/m;by*=80/m}
  $('bub').setAttribute('cx',bx);$('bub').setAttribute('cy',by);
  $('bub').setAttribute('fill',d.alarm?'var(--bad)':(d.warn?'var(--warn)':'var(--ok)'));
  $('mute').textContent=d.muted?'Unmute device':'Mute device';
  document.title=(d.alarm?'⚠ TILTED ':'')+'Tilt Sensor';
  if(d.alarm&&Date.now()-beepT>1000){beep();beepT=Date.now()}
  const ev=$('ev');
  ev.innerHTML=d.events.length?d.events.slice().reverse().map(e=>
   `<li><span><span class="dot" style="background:var(${e.a?'--bad':'--ok'})"></span>${e.a?'Tilted':'Back to level'} (${e.deg.toFixed(1)}°)</span><span class="k">${fmt((d.now-e.t)/1000)}</span></li>`).join(''):'<li class="k">No events yet</li>';
 }catch(e){$('conn').textContent='offline - retrying';}
}
$('zero').onclick=()=>post('/api/zero');
$('mute').onclick=()=>post('/api/mute?on='+(last&&last.muted?0:1));
$('snd').onclick=()=>{actx=actx||new (window.AudioContext||window.webkitAudioContext)();actx.resume();beep();$('snd').textContent='Sound on';};
$('slider').oninput=e=>{dragging=true;$('lim').textContent=e.target.value+'°'};
$('slider').onchange=e=>{dragging=false;post('/api/limit?deg='+e.target.value)};
setInterval(poll,400);poll();
</script></body></html>)HTML";
