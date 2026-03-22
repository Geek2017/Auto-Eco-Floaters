#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"

// === AUTO ECO-FLOATERS — FLOOD MONITOR WITH WEB DASHBOARD ===
//
// Wiring (LOLIN32):
//   5V       -> Buzzer + (middle), HC-SR04 VCC
//   GPIO 5   -> Buzzer S
//   GPIO 13  -> HC-SR04 TRIG
//   GPIO 14  -> HC-SR04 ECHO (via voltage divider: 2k + 1k)
//   GND      -> Buzzer -, HC-SR04 GND, divider 2k leg

// ===== WIFI CREDENTIALS =====
// Set via Serial Monitor: type "wifi SSID PASSWORD" and press Enter
// Saved to flash — survives reboots
// ==================================

Preferences prefs;
String savedSSID;
String savedPass;
unsigned long lastIPPrint = 0;

// AP+STA mode
DNSServer dnsServer;
const char *AP_SSID = "EcoFloaters";
const char *AP_PASS = "ecofloaters";

// Deferred WiFi connect (avoid disconnecting inside HTTP handler)
bool pendingConnect = false;
String pendingSSID;
String pendingPass;
unsigned long pendingTime = 0;

#define BUZZER_PIN 5
#define TRIG_PIN 13
#define ECHO_PIN 14

#define LEDC_CHANNEL 0
#define LEDC_RESOLUTION 8

// Tank dimensions — adjust to your setup
#define TANK_HEIGHT_CM 14 // sensor calibration (max reading in empty tube)
#define TANK_LABEL_CM 32  // actual physical tube length for display
#define MAX_DISTANCE_CM 200
#define MIN_DISTANCE_CM 3

WebServer server(80);

// Global state
volatile long currentDistance = -1;
volatile int waterLevelPct = 0;
const char *currentZone = "---";
int currentFreq = 0;
int buzzerHz = 800; // configurable 100-1000, default 800

long singleRead()
{
    digitalWrite(TRIG_PIN, LOW);
    delayMicroseconds(2);
    digitalWrite(TRIG_PIN, HIGH);
    delayMicroseconds(10);
    digitalWrite(TRIG_PIN, LOW);

    long duration = pulseIn(ECHO_PIN, HIGH, 30000);
    if (duration == 0)
        return -1;
    return duration / 58;
}

// Median of 5 reads — fast and stable
long getDistanceCM()
{
    const int SAMPLES = 5;
    long all[SAMPLES];

    for (int i = 0; i < SAMPLES; i++)
    {
        all[i] = singleRead();
        delayMicroseconds(2000); // 2ms between samples (was 10ms)
    }

    // Sort
    for (int i = 0; i < SAMPLES - 1; i++)
        for (int j = i + 1; j < SAMPLES; j++)
            if (all[j] < all[i])
            {
                long t = all[i];
                all[i] = all[j];
                all[j] = t;
            }

    return all[SAMPLES / 2]; // median
}

// --- Web Dashboard HTML ---
const char DASHBOARD_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no">
<title>Auto Eco-Floaters — Flood Monitor</title>
<style>
:root{--bg:#1a1a1a;--card:#252525;--bdr:#333;--txt:#e0e0e0;--mut:#888;--safe:#22c55e;--watch:#3b82f6;--warn:#eab308;--danger:#ef4444;--evac:#dc2626}
*{margin:0;padding:0;box-sizing:border-box}
html,body{width:100%;height:100%}
body{font-family:'Segoe UI',system-ui,sans-serif;background:var(--bg);color:var(--txt)}
.app{width:100%;min-height:100vh;min-height:100dvh;padding:2vh 3vw;display:flex;flex-direction:column}
.hdr{text-align:center;padding-bottom:1vh;flex-shrink:0}
.hdr h1{font-size:4vh;font-weight:800;color:#d4a55a}
.hdr .sub{font-size:2vh;color:var(--mut);letter-spacing:2px;text-transform:uppercase}
.live{display:inline-flex;align-items:center;gap:6px;font-size:2vh;color:var(--safe);margin-top:0.3vh;font-weight:600;letter-spacing:1px}
.dot{width:8px;height:8px;border-radius:50%;background:var(--safe);animation:bk 1.5s infinite}
@keyframes bk{0%,100%{opacity:1}50%{opacity:.2}}
.cd{background:var(--card);border:1px solid var(--bdr);border-radius:12px;padding:1.8vh 2.5vw;margin-bottom:1vh;flex-shrink:0}
.ct{font-size:1.8vh;text-transform:uppercase;letter-spacing:1.5px;color:var(--mut);margin-bottom:0.8vh;font-weight:600}
/* Alert Zone hero */
.az{display:flex;align-items:center;justify-content:space-between}
.az-left{flex:1}
.az-right{text-align:right;flex-shrink:0;display:flex;align-items:baseline;gap:1.5vw}
.az-right .dt{font-size:6vh;font-weight:800;color:var(--txt);line-height:1}
.az-right .dd{font-size:6vh;font-weight:800;color:var(--mut)}
.az .al{font-size:1.8vh;text-transform:uppercase;letter-spacing:2px;color:var(--mut);font-weight:600;margin-bottom:0.3vh}
.az .an{font-size:6vh;font-weight:900;line-height:1.1}
.az .ad{font-size:2.2vh;color:var(--mut);margin-top:0.3vh}
.az-safe .an{color:var(--safe)}
.az-watch .an{color:var(--watch)}
.az-warn .an{color:var(--warn)}
.az-danger .an{color:var(--danger)}
.az-evac .an{color:var(--evac);animation:pulse .6s infinite}
@keyframes pulse{50%{opacity:.5}}
/* Blood pulse overlay */
.blood-overlay{position:fixed;top:0;left:0;width:100%;height:100%;pointer-events:none;z-index:9999;opacity:0;transition:opacity .3s}
.blood-danger{opacity:1;animation:bloodPulse 1.5s ease-in-out infinite;background:radial-gradient(ellipse at center,transparent 40%,rgba(220,38,38,.25) 100%)}
.blood-evac{opacity:1;animation:bloodPulse .6s ease-in-out infinite;background:radial-gradient(ellipse at center,rgba(220,38,38,.1) 0%,rgba(127,29,29,.45) 100%)}
@keyframes bloodPulse{0%,100%{opacity:.7}50%{opacity:1}}
/* Main content row: tank+stats on left, history on right */
.main{display:flex;gap:2vw;flex:1;min-height:0}
.left{display:flex;flex-direction:column;gap:1vh;width:45%}
.right{flex:1;display:flex;flex-direction:column;min-height:0}
/* Stats + tank */
.sr{display:flex;gap:2vw;align-items:stretch;flex:1;min-height:0}
.tk-wrap{flex-shrink:0;display:flex;flex-direction:column;align-items:center}
.tk{position:relative;width:10vh;flex:1;border:2px solid #444;border-radius:0 0 10px 10px;border-top:none;overflow:hidden;background:#111}
.tk-cap{width:calc(10vh + 8px);height:4px;background:#555;border-radius:2px 2px 0 0}
.wt{position:absolute;bottom:0;width:100%;background:linear-gradient(to top,#0369a1,#38bdf8);transition:height .8s cubic-bezier(.4,0,.2,1);border-radius:0 0 6px 6px}
.wp{position:absolute;top:50%;left:0;width:100%;text-align:center;transform:translateY(-50%);font-size:2.8vh;font-weight:800;color:#fff;text-shadow:0 1px 4px rgba(0,0,0,.7)}
.tk-marks{position:absolute;right:-2px;top:0;height:100%;display:flex;flex-direction:column;justify-content:space-between;padding:6px 0;font-size:1.3vh;font-weight:700;letter-spacing:.3px}
.mk-evac{color:var(--evac)}.mk-danger{color:var(--danger)}.mk-warn{color:var(--warn)}.mk-watch{color:var(--watch)}
/* Stats grid */
.stats{flex:1;display:grid;grid-template-columns:1fr 1fr;gap:1.2vh 1.5vw;align-content:stretch}
.st{background:#1a1a1a;border-radius:10px;padding:2vh 1.5vw;display:flex;flex-direction:column;justify-content:center;flex:1}
.st .sv{font-size:5vh;font-weight:700;color:var(--txt)}
.st .sl{font-size:1.6vh;text-transform:uppercase;letter-spacing:1px;color:var(--mut);margin-top:0.3vh}
/* Flood alert badges */
.badges{display:flex;gap:1vw}
.badge{flex:1;text-align:center;padding:1.2vh 0;border-radius:8px;font-size:1.8vh;font-weight:700;letter-spacing:.5px;opacity:.35;transition:opacity .3s,transform .2s}
.badge.active{opacity:1;transform:scale(1.05)}
.b-watch{background:rgba(59,130,246,.15);color:var(--watch);border:1px solid rgba(59,130,246,.3)}
.b-warn{background:rgba(234,179,8,.15);color:var(--warn);border:1px solid rgba(234,179,8,.3)}
.b-danger{background:rgba(239,68,68,.15);color:var(--danger);border:1px solid rgba(239,68,68,.3)}
.b-evac{background:rgba(220,38,38,.15);color:var(--evac);border:1px solid rgba(220,38,38,.3)}
/* Chart */
.chart-card{background:var(--card);border:1px solid var(--bdr);border-radius:12px;padding:1.2vh 1.5vw;flex-shrink:0;margin-bottom:1vh}
.chart-svg{width:100%;display:block}
.chart-line{fill:none;stroke:#38bdf8;stroke-width:2;stroke-linejoin:round;stroke-linecap:round}
.chart-area{fill:url(#chartGrad);opacity:.4}
.chart-grid{stroke:#333;stroke-width:0.5}
.chart-lbl{fill:var(--mut);font-size:1.3vh;font-family:'Segoe UI',system-ui,sans-serif}
.chart-ylbl{fill:var(--mut);font-size:1.2vh;font-family:'Segoe UI',system-ui,sans-serif;text-anchor:end}
.chart-dot{fill:#38bdf8}
.chart-zone-safe{stroke:var(--safe)}.chart-zone-watch{stroke:var(--watch)}.chart-zone-warn{stroke:var(--warn)}.chart-zone-danger{stroke:var(--danger)}.chart-zone-evac{stroke:var(--evac)}
/* History card fills right column */
.hist-card{background:var(--card);border:1px solid var(--bdr);border-radius:12px;padding:1.8vh 2vw;flex:1;display:flex;flex-direction:column;min-height:0}
.hist-wrap{flex:1;min-height:0;overflow-y:auto;-webkit-overflow-scrolling:touch}
.hist-wrap::-webkit-scrollbar{width:4px}
.hist-wrap::-webkit-scrollbar-thumb{background:#444;border-radius:3px}
.ht{width:100%;border-collapse:collapse}
.ht th{text-align:left;color:var(--mut);font-weight:600;padding:0.8vh 1.2vw;border-bottom:1px solid #333;font-size:1.6vh;text-transform:uppercase;letter-spacing:.8px;position:sticky;top:0;background:var(--card)}
.ht td{padding:0.8vh 1.2vw;border-bottom:1px solid #2a2a2a;color:var(--txt);font-size:1.8vh}
.ht tr:last-child td{border-bottom:none}
.ft{text-align:center;font-size:1.4vh;color:var(--mut);margin-top:0.5vh;flex-shrink:0}
/* Mobile: stack vertically, allow scroll */
@media(max-width:600px){
html,body{overflow-y:auto;-webkit-overflow-scrolling:touch}
.app{min-height:auto;padding:1.5vh 3vw}
.main{flex-direction:column;overflow:visible}
.left{width:100%}
.right{width:100%}
.cd{padding:1.5vh 3vw;margin-bottom:0.8vh}
.sr{max-height:none;min-height:auto;height:auto}
.tk-wrap{height:140px}
.tk{width:50px;height:120px;flex:none}
.tk-cap{width:58px}
.wp{font-size:2vh}
.tk-marks{font-size:1vh}
.hdr h1{font-size:2.8vh}
.hdr .sub{font-size:1.4vh}
.az{flex-direction:column;align-items:flex-start;gap:0.5vh}
.az-right{align-self:flex-end}
.az-right .dt,.az-right .dd{font-size:3.5vh}
.az .an{font-size:4vh}
.az .ad{font-size:1.6vh}
.st{padding:1.2vh 2vw}
.st .sv{font-size:3vh}
.st .sl{font-size:1.2vh}
.badge{font-size:1.4vh;padding:0.8vh 0}
.chart-card{padding:1vh 2vw}
.hist-card{padding:1.2vh 2vw;max-height:30vh}
.ht th{font-size:1.3vh;padding:0.5vh 1vw}
.ht td{font-size:1.5vh;padding:0.5vh 1vw}
.ft{font-size:1.2vh}
.live{font-size:1.5vh}
}
</style>
</head>
<body>
<div class="blood-overlay" id="bloodOv"></div>
<div class="app">

<div class="hdr">
<h1>Auto Eco-Floaters</h1>
<div class="sub">Flood Monitor &mdash; Live</div>
<div class="live"><span class="dot"></span> LIVE &nbsp;<span id="wifiInfo" style="color:#888;font-size:1.6vh;font-weight:400"></span></div>
<a href="/setup" style="position:absolute;top:2vh;right:3vw;font-size:3.5vh;text-decoration:none;color:#888;z-index:100" title="WiFi Settings">&#9881;</a>
</div>

<!-- Alert Zone Hero -->
<div class="cd az az-safe" id="azCard">
<div class="az-left">
<div class="al">Alert Zone</div>
<div class="an" id="azName">SAFE</div>
<div class="ad" id="azDesc">All clear &mdash; monitoring active</div>
</div>
<div class="az-right">
<div class="dd" id="dateDisp">---</div>
<div class="dt" id="clock">--:--:--</div>
</div>
</div>

<!-- Main content: left (tank+stats+badges) | right (history) -->
<div class="main">
<div class="left">
<!-- Tank + Stats -->
<div class="cd" style="display:flex;flex-direction:column">
<div class="sr">
<div class="tk-wrap">
<div class="tk-cap"></div>
<div class="tk">
<div class="wt" id="wt" style="height:0%"><div class="wp" id="wp">0%</div></div>
<div class="tk-marks">
<span class="mk-evac">&larr;EVAC</span>
<span class="mk-danger">&larr;DANGER</span>
<span class="mk-warn">&larr;WARN</span>
<span class="mk-watch">&larr;WATCH</span>
</div>
</div>
</div>
<div class="stats">
<div class="st"><div class="sl">Water CM</div><div class="sv" id="wcm">0</div></div>
<div class="st"><div class="sl">Distance CM</div><div class="sv" id="dcm">--</div></div>
<div class="st"><div class="sl">Level %</div><div class="sv" id="lvl">0<span style="font-size:.5em;font-weight:400">%</span></div></div>
<div class="st"><div class="sl">Rise Rate</div><div class="sv" id="rr">0<span style="font-size:.5em;font-weight:400">cm/m</span></div></div>
</div>
</div>
</div>
<!-- Flood Alert Levels -->
<div class="cd">
<div class="ct">Flood Alert Levels</div>
<div class="badges">
<div class="badge b-watch" id="bw">WATCH</div>
<div class="badge b-warn" id="bwn">WARNING</div>
<div class="badge b-danger" id="bd">DANGER</div>
<div class="badge b-evac" id="be">EVACUATE</div>
</div>
</div>
</div>

<!-- Chart + Reading History -->
<div class="right">
<div class="chart-card">
<div class="ct">Water Level Timeline</div>
<div id="chartBox"></div>
</div>
<div class="hist-card">
<div class="ct">Reading History (last 20)</div>
<div class="hist-wrap">
<table class="ht">
<thead><tr><th>Time</th><th>Dist</th><th>Level</th><th>Zone</th></tr></thead>
<tbody id="hb"></tbody>
</table>
</div>
</div>
</div>
</div>

<div class="ft">Auto Eco-Floaters &bull; Flood Monitor &bull; ESP32 &bull; 1s refresh</div>

</div>
<script>
var t0=Date.now();
var readings=[];
var lastDist=null,lastTime=null;
var riseRate=0;

function zoneInfo(level){
if(level>=90) return {name:'EVACUATE',cls:'az-evac',desc:'Critical level \u2014 evacuate immediately!',tier:4};
if(level>=70) return {name:'DANGER',cls:'az-danger',desc:'Dangerous water level \u2014 prepare to evacuate',tier:3};
if(level>=50) return {name:'WARNING',cls:'az-warn',desc:'Water rising \u2014 stay alert',tier:2};
if(level>=30) return {name:'WATCH',cls:'az-watch',desc:'Elevated level \u2014 monitoring closely',tier:1};
return {name:'SAFE',cls:'az-safe',desc:'All clear \u2014 monitoring active',tier:0};
}

function pad(n){return n<10?'0'+n:''+n;}
function ts(){var d=new Date();return pad(d.getHours())+':'+pad(d.getMinutes())+':'+pad(d.getSeconds());}
var days=['Sun','Mon','Tue','Wed','Thu','Fri','Sat'];
var months=['Jan','Feb','Mar','Apr','May','Jun','Jul','Aug','Sep','Oct','Nov','Dec'];
function updClock(){var d=new Date();document.getElementById('clock').textContent=pad(d.getHours())+':'+pad(d.getMinutes())+':'+pad(d.getSeconds());document.getElementById('dateDisp').textContent=days[d.getDay()]+', '+d.getDate()+' '+months[d.getMonth()]+' '+d.getFullYear();}
setInterval(updClock,1000);updClock();

function upd(){
fetch('/api/data').then(function(r){return r.json();}).then(function(d){
var p=Math.max(0,Math.min(100,d.level));
var waterCm=Math.round(d.tank_height*(p/100));

// Rise rate (cm per minute)
var now=Date.now();
if(lastDist!==null&&lastTime!==null){
var dt=(now-lastTime)/60000;
if(dt>0){var delta=lastDist-d.distance;riseRate=Math.round((delta/dt)*10)/10;}
}
lastDist=d.distance;lastTime=now;

// Tank
document.getElementById('wt').style.height=p+'%';
document.getElementById('wp').textContent=p+'%';

// WiFi info
if(d.ssid) document.getElementById('wifiInfo').textContent='\u2022 '+d.ssid+' ('+d.rssi+'dBm)';
// Stats
document.getElementById('wcm').textContent=waterCm;
document.getElementById('dcm').textContent=Math.round(d.distance);
document.getElementById('lvl').innerHTML=p+'<span style="font-size:.5em;font-weight:400">%</span>';
var rr=Math.max(-99,Math.min(99,riseRate));
document.getElementById('rr').innerHTML=rr+'<span style="font-size:.5em;font-weight:400">cm/m</span>';

// Alert zone
var zi=zoneInfo(p);
var ac=document.getElementById('azCard');
ac.className='cd az '+zi.cls;
document.getElementById('azName').textContent=zi.name;
document.getElementById('azDesc').textContent=zi.desc;

// Blood pulse overlay
var ov=document.getElementById('bloodOv');
if(zi.tier>=4) ov.className='blood-overlay blood-evac';
else if(zi.tier>=3) ov.className='blood-overlay blood-danger';
else ov.className='blood-overlay';

// Color water by severity
var wt=document.getElementById('wt');
if(zi.tier>=4) wt.style.background='linear-gradient(to top,#7f1d1d,#ef4444)';
else if(zi.tier>=3) wt.style.background='linear-gradient(to top,#9a3412,#f97316)';
else if(zi.tier>=2) wt.style.background='linear-gradient(to top,#854d0e,#eab308)';
else if(zi.tier>=1) wt.style.background='linear-gradient(to top,#1e3a5f,#3b82f6)';
else wt.style.background='linear-gradient(to top,#0369a1,#38bdf8)';

// Badges - highlight all tiers at or below current tier
var bids=['bw','bwn','bd','be'];
var bcls=['b-watch','b-warn','b-danger','b-evac'];
for(var i=0;i<4;i++){
var el=document.getElementById(bids[i]);
el.className='badge '+bcls[i]+(zi.tier>=i+1?' active':'');
}

// History
readings.unshift({t:ts(),dist:d.distance,level:p,zone:zi.name});
if(readings.length>60) readings.length=60;
drawChart();
var hb=document.getElementById('hb');
var html='';
var showN=Math.min(readings.length,20);
for(var i=0;i<showN;i++){
var h=readings[i];
html+='<tr><td>'+h.t+'</td><td>'+h.dist+'cm</td><td>'+h.level+'%</td><td>'+h.zone+'</td></tr>';
}
hb.innerHTML=html;

}).catch(function(e){
drawChart();
document.getElementById('azName').textContent='OFFLINE';
document.getElementById('azDesc').textContent='Connection lost \u2014 retrying...';
document.getElementById('azCard').className='cd az';
});
}
function drawChart(){
var box=document.getElementById('chartBox');
if(!readings.length){box.innerHTML='<div style="color:var(--mut);font-size:1.6vh;text-align:center;padding:2vh 0">Collecting data...</div>';return;}
var W=box.clientWidth||600;
var H=Math.round(W*0.28);
var pad_l=40,pad_r=10,pad_t=12,pad_b=24;
var gw=W-pad_l-pad_r,gh=H-pad_t-pad_b;
var pts=readings.slice().reverse();
var n=pts.length;
if(n<2){box.innerHTML='<div style="color:var(--mut);font-size:1.6vh;text-align:center;padding:2vh 0">Waiting for data...</div>';return;}
var svg='<svg class="chart-svg" viewBox="0 0 '+W+' '+H+'" xmlns="http://www.w3.org/2000/svg">';
svg+='<defs><linearGradient id="chartGrad" x1="0" y1="0" x2="0" y2="1"><stop offset="0%" stop-color="#38bdf8" stop-opacity="0.5"/><stop offset="100%" stop-color="#38bdf8" stop-opacity="0.02"/></linearGradient></defs>';
// Y grid lines + labels
for(var yy=0;yy<=100;yy+=25){
var gy=pad_t+gh-(yy/100)*gh;
svg+='<line x1="'+pad_l+'" y1="'+gy+'" x2="'+(W-pad_r)+'" y2="'+gy+'" class="chart-grid"/>';
svg+='<text x="'+(pad_l-4)+'" y="'+(gy+4)+'" class="chart-ylbl">'+yy+'%</text>';}
// Build path
var line='',area='M'+pad_l+','+(pad_t+gh);
for(var i=0;i<n;i++){
var x=pad_l+(i/(n-1))*gw;
var y=pad_t+gh-(pts[i].level/100)*gh;
if(i===0)line+='M'+x+','+y;else line+=' L'+x+','+y;
area+=' L'+x+','+y;}
area+=' L'+(pad_l+((n-1)/(n-1))*gw)+','+(pad_t+gh)+' Z';
svg+='<path d="'+area+'" class="chart-area"/>';
// Determine line color from latest level
var latest=pts[n-1].level;
var lcls='chart-line';
if(latest>=90)lcls+=' chart-zone-evac';else if(latest>=70)lcls+=' chart-zone-danger';else if(latest>=50)lcls+=' chart-zone-warn';else if(latest>=30)lcls+=' chart-zone-watch';else lcls+=' chart-zone-safe';
svg+='<path d="'+line+'" class="'+lcls+'"/>';
// Dot on latest point
var lx=pad_l+((n-1)/(n-1))*gw;
var ly=pad_t+gh-(latest/100)*gh;
svg+='<circle cx="'+lx+'" cy="'+ly+'" r="4" class="chart-dot"/>';
// Time labels (show ~6 labels max)
var step=Math.max(1,Math.floor(n/6));
for(var i=0;i<n;i+=step){
var x=pad_l+(i/(n-1))*gw;
svg+='<text x="'+x+'" y="'+(H-2)+'" class="chart-lbl" text-anchor="middle">'+pts[i].t.substring(0,5)+'</text>';}
// Always show last label
if((n-1)%step!==0){
var x2=pad_l+((n-1)/(n-1))*gw;
svg+='<text x="'+x2+'" y="'+(H-2)+'" class="chart-lbl" text-anchor="end">'+pts[n-1].t.substring(0,5)+'</text>';}
svg+='</svg>';
box.innerHTML=svg;}

setInterval(upd,1000);upd();
</script>
</body>
</html>
)rawliteral";

// --- WiFi Setup Page HTML ---
const char SETUP_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no">
<title>Auto Eco-Floaters — WiFi Setup</title>
<style>
:root{--bg:#1a1a1a;--card:#252525;--bdr:#333;--txt:#e0e0e0;--mut:#888;--acc:#d4a55a;--ok:#22c55e;--err:#ef4444;--info:#3b82f6}
*{margin:0;padding:0;box-sizing:border-box}
body{font-family:'Segoe UI',system-ui,sans-serif;background:var(--bg);color:var(--txt);min-height:100vh;display:flex;align-items:center;justify-content:center}
.box{width:90%;max-width:500px;padding:3vh 4vw;background:var(--card);border:1px solid var(--bdr);border-radius:16px}
h1{font-size:3vh;font-weight:800;color:var(--acc);text-align:center;margin-bottom:0.5vh}
.sub{text-align:center;font-size:1.6vh;color:var(--mut);margin-bottom:2vh}
.lbl{font-size:1.4vh;text-transform:uppercase;letter-spacing:1px;color:var(--mut);font-weight:600;margin-bottom:0.5vh;margin-top:1.5vh}
.nets{max-height:30vh;overflow-y:auto;border:1px solid var(--bdr);border-radius:8px;margin-bottom:1.5vh}
.nets::-webkit-scrollbar{width:4px}.nets::-webkit-scrollbar-thumb{background:#444;border-radius:3px}
.net{display:flex;justify-content:space-between;align-items:center;padding:1.2vh 1.5vw;border-bottom:1px solid var(--bdr);cursor:pointer;transition:background .2s}
.net:hover,.net.sel{background:#333}
.net:last-child{border-bottom:none}
.nn{font-size:1.8vh;font-weight:600}
.nr{font-size:1.3vh;color:var(--mut)}
.nr.strong{color:var(--ok)}.nr.medium{color:#eab308}.nr.weak{color:var(--err)}
input[type=password],input[type=text]{width:100%;padding:1.2vh 1vw;background:var(--bg);border:1px solid #444;border-radius:8px;color:var(--txt);font-size:1.8vh;outline:none}
input:focus{border-color:var(--acc)}
.btn{display:block;width:100%;padding:1.5vh;background:var(--acc);color:var(--bg);border:none;border-radius:8px;font-size:2vh;font-weight:700;cursor:pointer;margin-top:2vh;letter-spacing:1px}
.btn:hover{background:#c4953a}.btn:disabled{opacity:.5;cursor:not-allowed}
.btn-back{background:#333;color:var(--txt);border:1px solid var(--bdr);margin-top:1vh;font-size:1.6vh;padding:1.2vh}
.btn-back:hover{background:#444}
.msg{text-align:center;font-size:1.6vh;margin-top:1.5vh;min-height:2vh}
.msg.ok{color:var(--ok)}.msg.err{color:var(--err)}.msg.wait{color:var(--info)}
.scan-btn{display:inline-block;padding:0.8vh 2vw;background:#333;color:var(--acc);border:1px solid #444;border-radius:6px;font-size:1.4vh;font-weight:600;cursor:pointer;margin-bottom:1vh}
.scan-btn:hover{background:#444}
.cur{text-align:center;font-size:1.4vh;color:var(--mut);margin-bottom:1.5vh;padding:1vh;background:var(--bg);border-radius:8px}
.cur b{color:var(--ok)}
</style>
</head>
<body>
<div class="box">
<h1>&#9881; Auto Eco-Floaters</h1>
<div class="sub">WiFi Setup</div>
<div class="cur" id="cur">Loading...</div>
<button class="scan-btn" onclick="scan()">Scan Networks</button>
<div class="lbl">Available Networks</div>
<div class="nets" id="nets"><div style="padding:2vh;text-align:center;color:#888">Click "Scan Networks"</div></div>
<div class="lbl">Password</div>
<input type="password" id="pw" placeholder="Enter WiFi password">
<input type="hidden" id="ss" value="">
<button class="btn" id="cbtn" onclick="connect()" disabled>Select a Network First</button>
<div class="msg" id="msg"></div>
<button class="btn btn-back" onclick="window.location.href='/'">&#8592; Back to Dashboard</button>
</div>
<script>
var selSSID='';
function scan(){
document.getElementById('nets').innerHTML='<div style="padding:2vh;text-align:center;color:#3b82f6">Scanning...</div>';
fetch('/api/scan').then(function(r){return r.json();}).then(function(d){
var h='';
if(!d.networks||!d.networks.length){h='<div style="padding:2vh;text-align:center;color:#888">No networks found</div>';}
else{for(var i=0;i<d.networks.length;i++){var n=d.networks[i];var cls=n.rssi>-50?'strong':n.rssi>-70?'medium':'weak';
h+='<div class="net" onclick="pick(this,\''+n.ssid.replace(/'/g,"\\\'")+'\')"><span class="nn">'+n.ssid+'</span><span class="nr '+cls+'">'+n.rssi+'dBm</span></div>';}}
document.getElementById('nets').innerHTML=h;
if(d.connected){document.getElementById('cur').innerHTML='Connected to: <b>'+d.current_ssid+'</b> ('+d.ip+', '+d.rssi+'dBm)';}
else{document.getElementById('cur').innerHTML='Not connected to any WiFi';}
}).catch(function(){document.getElementById('nets').innerHTML='<div style="padding:2vh;text-align:center;color:#ef4444">Scan failed</div>';});}
function pick(el,ssid){selSSID=ssid;document.getElementById('ss').value=ssid;
var all=document.querySelectorAll('.net');for(var i=0;i<all.length;i++)all[i].className='net';el.className='net sel';
document.getElementById('cbtn').disabled=false;document.getElementById('cbtn').textContent='Connect to '+ssid;}
function connect(){var ssid=document.getElementById('ss').value;var pw=document.getElementById('pw').value;
if(!ssid){document.getElementById('msg').className='msg err';document.getElementById('msg').textContent='Select a network first';return;}
document.getElementById('msg').className='msg wait';document.getElementById('msg').textContent='Connecting to '+ssid+'...';
document.getElementById('cbtn').disabled=true;
fetch('/api/connect',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'ssid='+encodeURIComponent(ssid)+'&pass='+encodeURIComponent(pw)})
.then(function(r){return r.json();}).then(function(d){
if(d.success){showOk(d.ip);}
else if(d.pending){document.getElementById('msg').className='msg wait';document.getElementById('msg').textContent='Switching WiFi... checking status';pollConnect(ssid,0);}
else{document.getElementById('msg').className='msg err';document.getElementById('msg').textContent='Failed: '+(d.error||'Unknown error');document.getElementById('cbtn').disabled=false;}
}).catch(function(){document.getElementById('msg').className='msg err';document.getElementById('msg').textContent='Connection error';document.getElementById('cbtn').disabled=false;});}
function showOk(ip){document.getElementById('msg').className='msg ok';document.getElementById('msg').innerHTML='Connected! IP: <b>'+ip+'</b><br>Also: <a href="http://ecofloaters.local" style="color:#d4a55a">ecofloaters.local</a> | <a href="http://'+ip+'" style="color:#d4a55a">'+ip+'</a>';scan();}
function pollConnect(ssid,count){if(count>10){document.getElementById('msg').className='msg err';document.getElementById('msg').textContent='Connection timed out. Check Serial Monitor.';document.getElementById('cbtn').disabled=false;return;}
setTimeout(function(){fetch('/api/scan').then(function(r){return r.json();}).then(function(d){
if(d.connected&&d.current_ssid===ssid){showOk(d.ip);}
else{document.getElementById('msg').textContent='Switching WiFi... attempt '+(count+1)+'/10';pollConnect(ssid,count+1);}
}).catch(function(){document.getElementById('msg').textContent='Switching WiFi... attempt '+(count+1)+'/10';pollConnect(ssid,count+1);});},3000);}
scan();
</script>
</body>
</html>
)rawliteral";

// === Serial WiFi Configuration ===
void processSerialCommand(String line)
{
    line.trim();
    if (line.startsWith("wifi "))
    {
        // Parse: wifi SSID PASSWORD
        int firstSpace = line.indexOf(' ');
        int secondSpace = line.indexOf(' ', firstSpace + 1);
        String ssid, pass;
        if (secondSpace > 0)
        {
            ssid = line.substring(firstSpace + 1, secondSpace);
            pass = line.substring(secondSpace + 1);
        }
        else
        {
            ssid = line.substring(firstSpace + 1);
            pass = "";
        }
        if (ssid.length() == 0)
        {
            Serial.println("Usage: wifi SSID PASSWORD");
            return;
        }

        Serial.printf("Connecting to '%s'...\n", ssid.c_str());
        WiFi.disconnect(false); // keep AP alive
        WiFi.begin(ssid.c_str(), pass.c_str());
        int attempts = 0;
        while (WiFi.status() != WL_CONNECTED && attempts < 40)
        {
            delay(500);
            Serial.print(".");
            attempts++;
        }
        Serial.println();

        if (WiFi.status() == WL_CONNECTED)
        {
            savedSSID = ssid;
            savedPass = pass;
            prefs.begin("wifi", false);
            prefs.putString("ssid", savedSSID);
            prefs.putString("pass", savedPass);
            prefs.end();
            Serial.printf("Connected! IP: %s — saved to flash\n", WiFi.localIP().toString().c_str());
            Serial.printf("Dashboard: http://%s or http://ecofloaters.local\n", WiFi.localIP().toString().c_str());
            MDNS.begin("ecofloaters");
        }
        else
        {
            Serial.println("Connection failed — check SSID and password");
        }
    }
    else if (line == "reset")
    {
        prefs.begin("wifi", false);
        prefs.clear();
        prefs.end();
        savedSSID = "";
        savedPass = "";
        WiFi.disconnect(false); // keep AP alive
        Serial.println("WiFi credentials cleared. Type: wifi SSID PASSWORD");
    }
    else if (line == "status")
    {
        if (WiFi.status() == WL_CONNECTED)
            Serial.printf("Connected to '%s' — IP: %s — RSSI: %d dBm\n",
                          savedSSID.c_str(), WiFi.localIP().toString().c_str(), WiFi.RSSI());
        else
            Serial.println("Not connected to WiFi");
    }
    else if (line.length() > 0)
    {
        Serial.println("Commands: wifi SSID PASSWORD | reset | status");
    }
}

// === Web Handlers ===
void handleRoot()
{
    server.send(200, "text/html", DASHBOARD_HTML);
}

void handleAPI()
{
    String json = "{";
    json += "\"distance\":" + String(currentDistance) + ",";
    json += "\"level\":" + String(waterLevelPct) + ",";
    json += "\"zone\":\"" + String(currentZone) + "\",";
    json += "\"freq\":" + String(currentFreq) + ",";
    json += "\"tank_height\":" + String(TANK_LABEL_CM) + ",";
    json += "\"buzzer_hz\":" + String(buzzerHz) + ",";
    json += "\"ssid\":\"" + savedSSID + "\",";
    json += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
    json += "\"rssi\":" + String(WiFi.RSSI());
    json += "}";
    server.send(200, "application/json", json);
}

void handleSettings()
{
    if (server.hasArg("hz"))
    {
        int hz = server.arg("hz").toInt();
        if (hz >= 100 && hz <= 1000)
            buzzerHz = hz;
    }
    server.send(200, "application/json", "{\"buzzer_hz\":" + String(buzzerHz) + "}");
}

void handleSetup()
{
    server.send(200, "text/html", SETUP_HTML);
}

void handleScan()
{
    int n = WiFi.scanNetworks();
    String json = "{\"connected\":" + String(WiFi.status() == WL_CONNECTED ? "true" : "false");
    json += ",\"current_ssid\":\"" + savedSSID + "\"";
    json += ",\"ip\":\"" + WiFi.localIP().toString() + "\"";
    json += ",\"rssi\":" + String(WiFi.RSSI());
    json += ",\"networks\":[";
    for (int i = 0; i < n; i++)
    {
        if (i > 0)
            json += ",";
        json += "{\"ssid\":\"" + WiFi.SSID(i) + "\",\"rssi\":" + String(WiFi.RSSI(i)) + "}";
    }
    json += "]}";
    server.send(200, "application/json", json);
}

void handleConnect()
{
    if (!server.hasArg("ssid"))
    {
        server.send(400, "application/json", "{\"success\":false,\"error\":\"Missing SSID\"}");
        return;
    }
    String newSSID = server.arg("ssid");
    String newPass = server.hasArg("pass") ? server.arg("pass") : "";

    // Already connected to this SSID? Respond success immediately
    if (newSSID == savedSSID && WiFi.status() == WL_CONNECTED)
    {
        String ip = WiFi.localIP().toString();
        Serial.printf("Web: Already connected to '%s' (IP=%s)\n", newSSID.c_str(), ip.c_str());
        server.send(200, "application/json", "{\"success\":true,\"ip\":\"" + ip + "\"}");
        return;
    }

    // Defer the actual reconnection so the HTTP response reaches the client
    pendingSSID = newSSID;
    pendingPass = newPass;
    pendingConnect = true;
    pendingTime = millis();
    Serial.printf("Web: Will connect to '%s' (deferred)\n", newSSID.c_str());
    server.send(200, "application/json", "{\"pending\":true}");
}

void setup()
{
    // Disable brownout detector — USB power marginal but usable
    WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);

    Serial.begin(115200);
    delay(1000); // let power stabilize

    pinMode(TRIG_PIN, OUTPUT);
    pinMode(ECHO_PIN, INPUT);

    ledcSetup(LEDC_CHANNEL, 1000, LEDC_RESOLUTION);
    ledcAttachPin(BUZZER_PIN, LEDC_CHANNEL);
    ledcWrite(LEDC_CHANNEL, 0);

    Serial.println();
    Serial.println("=== AUTO ECO-FLOATERS ===");

    // Start AP+STA mode — AP always on, STA connects in background
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(AP_SSID, AP_PASS);
    delay(100);
    dnsServer.start(53, "*", WiFi.softAPIP());
    MDNS.begin("ecofloaters");
    Serial.printf("AP ready: %s (pass: %s) — http://%s\n", AP_SSID, AP_PASS, WiFi.softAPIP().toString().c_str());

    // Try STA connection in background (non-blocking)
    if (prefs.begin("wifi", false))
    {
        savedSSID = prefs.getString("ssid", "");
        savedPass = prefs.getString("pass", "");
        prefs.end();
    }
    if (savedSSID.length() == 0)
    {
        savedSSID = "stellarum2.4";
        savedPass = "da3k0aram";
    }
    Serial.printf("STA: connecting to '%s' in background...\n", savedSSID.c_str());
    WiFi.begin(savedSSID.c_str(), savedPass.c_str());
    // Don't block — STA will connect while dashboard is already serving

    // Short beep to confirm boot
    ledcSetup(LEDC_CHANNEL, 2000, LEDC_RESOLUTION);
    ledcAttachPin(BUZZER_PIN, LEDC_CHANNEL);
    ledcWrite(LEDC_CHANNEL, 128);
    delay(200);
    ledcWrite(LEDC_CHANNEL, 0);

    // Start web server
    server.on("/", handleRoot);
    server.on("/api/data", handleAPI);
    server.on("/api/settings", handleSettings);
    server.on("/setup", handleSetup);
    server.on("/api/scan", handleScan);
    server.on("/api/connect", HTTP_POST, handleConnect);
    server.onNotFound([]()
                      {
        // Captive portal: redirect unknown requests to /setup
        server.sendHeader("Location", "http://" + WiFi.softAPIP().toString() + "/setup", true);
        server.send(302, "text/plain", ""); });
    server.begin();
    Serial.println("Web server started");
    Serial.println();
}

void loop()
{
    server.handleClient();
    dnsServer.processNextRequest();

    // Deferred WiFi connect — triggered by /api/connect
    if (pendingConnect && millis() - pendingTime > 500)
    {
        pendingConnect = false;
        String oldSSID = savedSSID;
        String oldPass = savedPass;
        Serial.printf("Switching STA to '%s'...\n", pendingSSID.c_str());
        WiFi.disconnect(false); // keep AP alive
        delay(100);
        WiFi.begin(pendingSSID.c_str(), pendingPass.c_str());
        int attempts = 0;
        while (WiFi.status() != WL_CONNECTED && attempts < 30)
        {
            delay(500);
            Serial.print(".");
            attempts++;
        }
        Serial.println();

        if (WiFi.status() == WL_CONNECTED)
        {
            savedSSID = pendingSSID;
            savedPass = pendingPass;
            prefs.begin("wifi", false);
            prefs.putString("ssid", savedSSID);
            prefs.putString("pass", savedPass);
            prefs.end();
            MDNS.begin("ecofloaters");
            Serial.printf("STA connected: %s (IP=%s)\n", savedSSID.c_str(), WiFi.localIP().toString().c_str());
        }
        else
        {
            Serial.printf("STA failed for '%s' \u2014 reconnecting to '%s'\n", pendingSSID.c_str(), oldSSID.c_str());
            if (oldSSID.length() > 0)
            {
                WiFi.begin(oldSSID.c_str(), oldPass.c_str());
            }
        }
    }

    // Process serial commands (wifi, reset, status)
    if (Serial.available())
    {
        String cmd = Serial.readStringUntil('\n');
        processSerialCommand(cmd);
    }

    // Print status every 30 seconds
    static bool staAnnounced = false;
    if (WiFi.status() == WL_CONNECTED && !staAnnounced)
    {
        staAnnounced = true;
        MDNS.begin("ecofloaters");
        Serial.printf("STA connected: %s (IP=%s) — also at http://ecofloaters.local\n", savedSSID.c_str(), WiFi.localIP().toString().c_str());
    }
    else if (WiFi.status() != WL_CONNECTED)
    {
        staAnnounced = false;
    }
    if (millis() - lastIPPrint > 30000)
    {
        lastIPPrint = millis();
        Serial.printf("AP: %s (%s)", AP_SSID, WiFi.softAPIP().toString().c_str());
        if (WiFi.status() == WL_CONNECTED)
            Serial.printf(" | STA: %s (%s)\n", savedSSID.c_str(), WiFi.localIP().toString().c_str());
        else
            Serial.println(" | STA: connecting...");
    }

    long dist = getDistanceCM();
    currentDistance = dist;

    if (dist <= 0 || dist > MAX_DISTANCE_CM)
    {
        ledcWrite(LEDC_CHANNEL, 0);
        currentZone = "OUT OF RANGE";
        currentFreq = 0;
        waterLevelPct = 0;
        return;
    }

    // Calculate water level %
    int level = map(dist, TANK_HEIGHT_CM, MIN_DISTANCE_CM, 0, 100);
    level = constrain(level, 0, 100);
    waterLevelPct = level;

    // Determine zone for display
    const char *zone;
    if (dist > 14)
        zone = "FAR";
    else if (dist > 11)
        zone = "NEAR";
    else if (dist > 8)
        zone = "CLOSE";
    else if (dist > 5)
        zone = "SO CLOSE";
    else
        zone = "CONTACT";

    currentZone = zone;

    // Morse-style beeping: faster as water rises
    if (level > 50)
    {
        currentFreq = buzzerHz;
        ledcSetup(LEDC_CHANNEL, buzzerHz, LEDC_RESOLUTION);
        ledcAttachPin(BUZZER_PIN, LEDC_CHANNEL);

        int beepOn, beepOff, beeps;
        if (level >= 90)
        {
            beepOn = 40;
            beepOff = 30;
            beeps = 5;
        }
        else if (level >= 80)
        {
            beepOn = 60;
            beepOff = 50;
            beeps = 4;
        }
        else if (level >= 70)
        {
            beepOn = 80;
            beepOff = 80;
            beeps = 3;
        }
        else if (level >= 60)
        {
            beepOn = 100;
            beepOff = 150;
            beeps = 2;
        }
        else
        {
            beepOn = 120;
            beepOff = 300;
            beeps = 1;
        }

        for (int i = 0; i < beeps; i++)
        {
            ledcWrite(LEDC_CHANNEL, 128);
            delay(beepOn);
            ledcWrite(LEDC_CHANNEL, 0);
            server.handleClient(); // keep dashboard responsive during beeps
            if (i < beeps - 1)
                delay(beepOff);
        }
    }
    else
    {
        currentFreq = 0;
        ledcWrite(LEDC_CHANNEL, 0);
    }

    Serial.print("Dist: ");
    Serial.print(dist);
    Serial.print("cm | Water: ");
    Serial.print(level);
    Serial.print("% | ");
    Serial.print(zone);
    Serial.print(" | Buzzer: ");
    Serial.println(level >= 90 ? "!!!!!" : level >= 80 ? "!!!!"
                                       : level >= 70   ? "!!!"
                                       : level >= 60   ? "!!"
                                       : level > 50    ? "!"
                                                       : "off");
}
