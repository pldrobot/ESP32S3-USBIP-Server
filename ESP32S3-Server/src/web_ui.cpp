#include "web_ui.h"
#include "rdebug.h"

WebUI::WebUI(WiFiManager& wifi, UsbDevice& device, UsbIpServer& usbip)
    : _server(WEB_SERVER_PORT), _wifi(wifi), _device(device), _usbip(usbip) {}

void WebUI::begin() {
    _server.on("/",                  HTTP_GET,  [this](){ _handleRoot(); });
    _server.on("/api/status",        HTTP_GET,  [this](){ _handleStatus(); });
    _server.on("/api/usbip/kick",    HTTP_POST, [this](){ _handleUsbIpKick(); });
    _server.on("/api/wifi/scan",     HTTP_GET,  [this](){ _handleWifiScan(); });
    _server.on("/api/wifi/connect",  HTTP_POST, [this](){ _handleWifiConnect(); });
    _server.on("/api/restart",       HTTP_POST, [this](){ _handleRestart(); });
    _server.onNotFound([this](){ _handleNotFound(); });
    _server.begin();
    debugI("Web Dashboard on port %d", WEB_SERVER_PORT);
}

void WebUI::task() { _server.handleClient(); }

void WebUI::_handleRoot()     { _server.send(200, "text/html", _generatePage()); }
void WebUI::_handleWifiScan() { _server.send(200, "application/json", _wifi.scanNetworks()); }
void WebUI::_handleNotFound() { _server.send(404, "text/plain", "Not Found"); }

void WebUI::_handleStatus() {
    String j = "{";

    // WiFi
    j += "\"wifi\":{\"connected\":"  + String(_wifi.isConnected() ? "true" : "false")
       + ",\"ssid\":\""   + _wifi.getSSID() + "\""
       + ",\"ip\":\""     + _wifi.getIP()   + "\""
       + ",\"rssi\":"     + String(_wifi.getRSSI())
       + ",\"mode\":\""   + (_wifi.getMode() == WSTATE_STATION ? "station" : "ap") + "\"},";

    // Printer
    j += "\"printer\":{\"connected\":" + String(_device.isConnected() ? "true" : "false")
       + ",\"ready\":"    + (_device.isReady()   ? "true" : "false")
       + ",\"product\":\"" + _device.getProduct() + "\""
       + ",\"vid\":\"0x"  + String(_device.getVendorId(),  HEX) + "\""
       + ",\"pid\":\"0x"  + String(_device.getProductId(), HEX) + "\"},";

    // USB/IP (embed full status JSON from server)
    j += "\"usbip\":" + _usbip.statusJson() + ",";

    // System
    j += "\"sys\":{\"uptime\":"  + String(millis() / 1000)
       + ",\"heap\":"    + String(ESP.getFreeHeap())
       + ",\"psram\":"   + String(ESP.getFreePsram()) + "}";

    j += "}";
    _server.send(200, "application/json", j);
}

void WebUI::_handleUsbIpKick() {
    _usbip.kickClient();
    _server.send(200, "application/json", "{\"ok\":true}");
}

void WebUI::_handleWifiConnect() {
    String ssid = _server.arg("ssid");
    String pass = _server.arg("password");
    if (!ssid.length()) { _server.send(400, "application/json", "{\"error\":\"SSID required\"}"); return; }
    bool ok = _wifi.setCredentials(ssid, pass);
    _server.send(200, "application/json",
        "{\"success\":" + String(ok ? "true" : "false") + ",\"ip\":\"" + _wifi.getIP() + "\"}");
}

void WebUI::_handleRestart() {
    _server.send(200, "application/json", "{\"ok\":true}");
    delay(300); ESP.restart();
}

// ── Page ──────────────────────────────────────────────────────────────────────

String WebUI::_generatePage() {
    return R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>USB/IP Bridge</title>
<style>
*{margin:0;padding:0;box-sizing:border-box}
body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;background:#0f172a;color:#e2e8f0;min-height:100vh}
.wrap{max-width:600px;margin:0 auto;padding:16px}
h1{font-size:1.25rem;font-weight:700;margin-bottom:14px;color:#f1f5f9}
.card{background:#1e293b;border-radius:10px;padding:16px;margin-bottom:10px;border:1px solid #334155}
.card h2{font-size:.72rem;font-weight:700;text-transform:uppercase;letter-spacing:.07em;color:#64748b;margin-bottom:10px}
.row{display:flex;justify-content:space-between;align-items:center;padding:6px 0;border-bottom:1px solid #0f172a}
.row:last-of-type{border-bottom:none}
.lbl{color:#94a3b8;font-size:.86rem}
.val{font-weight:500;font-size:.88rem}
.badge{padding:2px 9px;border-radius:9999px;font-size:.75rem;font-weight:700;display:inline-block}
.g{background:#064e3b;color:#6ee7b7}
.r{background:#7f1d1d;color:#fca5a5}
.y{background:#713f12;color:#fde68a}
.pulse{animation:pulse 2s infinite}
@keyframes pulse{0%,100%{opacity:1}50%{opacity:.4}}
input,select{width:100%;padding:8px 11px;margin-bottom:7px;background:#0f172a;border:1px solid #475569;border-radius:7px;color:#e2e8f0;font-size:.9rem}
input:focus,select:focus{outline:none;border-color:#3b82f6}
.btn{padding:7px 14px;border:none;border-radius:7px;font-size:.82rem;font-weight:600;cursor:pointer;transition:background .15s}
.bp{background:#3b82f6;color:#fff}.bp:hover{background:#2563eb}
.br{background:#7f1d1d;color:#fca5a5}.br:hover{background:#991b1b}
.bs{background:#334155;color:#cbd5e1}.bs:hover{background:#475569}
.brow{display:flex;gap:7px;margin-top:8px;flex-wrap:wrap}

/* USB/IP specific */
.session-box{background:#0f172a;border:1px solid #334155;border-radius:8px;padding:12px;margin:6px 0}
.session-box.live{border-color:#065f46}
.session-ip{font-family:monospace;font-size:.95rem;font-weight:600;color:#e2e8f0;margin:5px 0 4px}
.session-meta{font-size:.78rem;color:#94a3b8}
.no-client{color:#475569;font-size:.88rem;padding:8px 0}
.cmd-wrap{display:flex;align-items:center;gap:8px;margin-top:10px}
.cmd-box{flex:1;background:#0f172a;border:1px solid #334155;border-radius:6px;padding:8px 10px;font-family:monospace;font-size:.78rem;color:#7dd3fc;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.hist-table{width:100%;border-collapse:collapse;margin-top:8px;font-size:.82rem}
.hist-table th{color:#64748b;font-weight:600;text-align:left;padding:4px 0;border-bottom:1px solid #1e293b}
.hist-table td{padding:5px 0;border-bottom:1px solid #1e293b;color:#cbd5e1}
.hist-table td:first-child{font-family:monospace;color:#e2e8f0}
.hist-table tr:last-child td{border-bottom:none}
.sect{font-size:.7rem;font-weight:700;text-transform:uppercase;letter-spacing:.06em;color:#475569;margin:12px 0 4px}
.foot{text-align:center;color:#334155;font-size:.7rem;margin-top:14px}
</style>
</head>
<body>
<div class="wrap">
<h1>USB/IP Bridge</h1>

<!-- ── Printer (USB) ─────────────────────────────────────────────────── -->
<div class="card">
<h2>USB Device</h2>
<div class="row"><span class="lbl">Connection</span><span id="p-conn" class="badge r">Disconnected</span></div>
<div class="row"><span class="lbl">Model</span><span id="p-model" class="val">—</span></div>
<div class="row"><span class="lbl">USB ID</span><span id="p-ids" class="val" style="font-family:monospace;font-size:.82rem">—</span></div>
</div>

<!-- ── USB/IP (port 3240) ────────────────────────────────────────────── -->
<div class="card">
<h2>USB/IP — port 3240</h2>

<div class="sect">Current Session</div>
<div id="u-session">
  <div class="no-client">No client attached</div>
</div>

<div class="sect">Attach Command</div>
<div class="cmd-wrap">
  <div id="u-cmd" class="cmd-box">usbip attach -r … -b 1-1</div>
  <button class="btn bs" onclick="copyCmd()">Copy</button>
</div>

<div id="u-hist-wrap" style="display:none">
  <div class="sect">Session History</div>
  <table class="hist-table">
    <thead><tr><th>Client IP</th><th>Ended</th><th>Duration</th></tr></thead>
    <tbody id="u-hist"></tbody>
  </table>
</div>
</div>

<!-- ── WiFi ─────────────────────────────────────────────────────────── -->
<div class="card">
<h2>WiFi</h2>
<div class="row"><span class="lbl">Status</span><span id="w-st" class="badge r">—</span></div>
<div class="row"><span class="lbl">Network</span><span id="w-ssid" class="val">—</span></div>
<div class="row"><span class="lbl">IP</span><span id="w-ip" class="val">—</span></div>
<div class="row"><span class="lbl">Signal</span><span id="w-rssi" class="val">—</span></div>
<div style="margin-top:10px">
  <select id="nets"><option value="">— Scan to list networks —</option></select>
  <input type="password" id="pass" placeholder="Password">
  <div class="brow">
    <button class="btn bp" style="flex:1" onclick="scan()">Scan</button>
    <button class="btn bp" style="flex:1" onclick="connect()">Connect</button>
  </div>
</div>
</div>

<!-- ── System ────────────────────────────────────────────────────────── -->
<div class="card">
<h2>System</h2>
<div class="row"><span class="lbl">Uptime</span><span id="s-up" class="val">—</span></div>
<div class="row"><span class="lbl">Free RAM</span><span id="s-heap" class="val">—</span></div>
<div class="row"><span class="lbl">Free PSRAM</span><span id="s-psram" class="val">—</span></div>
<div class="brow" style="margin-top:10px">
  <button class="btn br" onclick="restart()">Restart ESP32</button>
</div>
</div>

<div class="foot">USB/IP Bridge · ESP32-S3</div>
</div>

<script>
var serverIP = '';

function dur(s) {
  if (s < 60) return s + 's';
  if (s < 3600) return Math.floor(s/60) + 'm ' + (s%60) + 's';
  return Math.floor(s/3600) + 'h ' + Math.floor((s%3600)/60) + 'm';
}
function ago(s) {
  if (s < 5)     return 'just now';
  if (s < 60)    return s + 's ago';
  if (s < 3600)  return Math.floor(s/60) + 'm ago';
  if (s < 86400) return Math.floor(s/3600) + 'h ago';
  return Math.floor(s/86400) + 'd ago';
}

function updateStatus() {
  fetch('/api/status').then(r => r.json()).then(d => {

    // Printer
    var pc = document.getElementById('p-conn');
    if (d.printer.ready)       { pc.textContent='Ready';       pc.className='badge g'; }
    else if(d.printer.connected){ pc.textContent='Connected';  pc.className='badge y'; }
    else                        { pc.textContent='Disconnected';pc.className='badge r'; }
    document.getElementById('p-model').textContent = d.printer.product || '—';
    document.getElementById('p-ids').textContent   = d.printer.connected
      ? d.printer.vid + ':' + d.printer.pid : '—';

    // WiFi
    serverIP = d.wifi.ip || '';
    document.getElementById('u-cmd').textContent =
      'usbip attach -r ' + (serverIP||'…') + ' -b 1-1';

    var ws = document.getElementById('w-st');
    if (d.wifi.connected) { ws.textContent='Connected'; ws.className='badge g'; }
    else { ws.textContent = d.wifi.mode==='ap'?'AP Setup':'Disconnected';
           ws.className = d.wifi.mode==='ap'?'badge y':'badge r'; }
    document.getElementById('w-ssid').textContent = d.wifi.ssid || '—';
    document.getElementById('w-ip').textContent   = d.wifi.ip;
    document.getElementById('w-rssi').textContent = d.wifi.connected ? d.wifi.rssi + ' dBm' : '—';

    // USB/IP current session
    var u = d.usbip, sess = document.getElementById('u-session');
    if (u.attached && u.client) {
      sess.innerHTML =
        '<div class="session-box live">'
        + '<div style="display:flex;justify-content:space-between;align-items:center">'
        + '<span class="badge g pulse">ATTACHED ●</span>'
        + '<button class="btn br" onclick="kickClient()">Disconnect</button>'
        + '</div>'
        + '<div class="session-ip">' + u.client.ip + '</div>'
        + '<div class="session-meta">Connected ' + dur(u.client.connectedSec)
        + ' &nbsp;·&nbsp; Attached ' + dur(u.client.attachedSec) + '</div>'
        + '</div>';
    } else if (u.client) {
      sess.innerHTML =
        '<div class="session-box">'
        + '<span class="badge y">CONNECTING</span>'
        + '<div class="session-ip">' + u.client.ip + '</div>'
        + '</div>';
    } else {
      sess.innerHTML = '<div class="no-client">No client attached — new connection auto-takes over</div>';
    }

    // USB/IP history
    var hist = u.history || [];
    var hw = document.getElementById('u-hist-wrap');
    var tb = document.getElementById('u-hist');
    if (hist.length) {
      hw.style.display = '';
      tb.innerHTML = hist.map(function(r) {
        return '<tr><td>' + r.ip + '</td>'
             + '<td>' + ago(r.agoSec) + '</td>'
             + '<td>' + dur(r.durationSec) + '</td></tr>';
      }).join('');
    } else {
      hw.style.display = 'none';
    }

    // System
    var u2 = d.sys.uptime;
    document.getElementById('s-up').textContent    =
      Math.floor(u2/3600)+'h '+Math.floor((u2%3600)/60)+'m '+(u2%60)+'s';
    document.getElementById('s-heap').textContent  = (d.sys.heap/1024).toFixed(0)+' KB';
    document.getElementById('s-psram').textContent = (d.sys.psram/1024).toFixed(0)+' KB';

  }).catch(function(){});
}

function kickClient() {
  if (!confirm('Disconnect the current USB/IP client?')) return;
  fetch('/api/usbip/kick', {method:'POST'})
    .then(function(){ updateStatus(); });
}

function copyCmd() {
  var cmd = document.getElementById('u-cmd').textContent;
  navigator.clipboard.writeText(cmd).then(function(){
    var b = event.target; b.textContent='Copied!';
    setTimeout(function(){ b.textContent='Copy'; }, 1500);
  });
}

function scan() {
  var s = document.getElementById('nets');
  s.innerHTML = '<option>Scanning...</option>';
  fetch('/api/wifi/scan').then(r => r.json()).then(nets => {
    s.innerHTML = '<option value="">— Select network —</option>';
    nets.sort((a,b) => b.rssi-a.rssi).forEach(n => {
      var o = document.createElement('option');
      o.value = n.ssid;
      o.textContent = n.ssid + ' (' + n.rssi + ' dBm)' + (n.secure ? ' 🔒' : '');
      s.appendChild(o);
    });
  });
}

function connect() {
  var ssid = document.getElementById('nets').value;
  var pass = document.getElementById('pass').value;
  if (!ssid) { alert('Select a network'); return; }
  fetch('/api/wifi/connect', {method:'POST',
    headers:{'Content-Type':'application/x-www-form-urlencoded'},
    body:'ssid='+encodeURIComponent(ssid)+'&password='+encodeURIComponent(pass)})
  .then(r => r.json()).then(d => {
    if (d.success) { alert('Connected!\nNew IP: '+d.ip+'\nRefreshing...'); setTimeout(()=>location.href='http://'+d.ip, 5000); }
    else alert('Connection failed — check password');
  });
}

function restart() {
  if (!confirm('Restart the ESP32?')) return;
  fetch('/api/restart', {method:'POST'});
  alert('Restarting… refresh in 15s');
  setTimeout(()=>location.reload(), 15000);
}

updateStatus();
setInterval(updateStatus, 2000);
</script>
</body>
</html>
)rawliteral";
}
