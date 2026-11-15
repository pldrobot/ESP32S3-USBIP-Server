#include "web_ui.h"
#include "applog.h"
#include "persist_log.h"

WebUI::WebUI(WiFiManager& wifi, UsbDevice& device, UsbIpServer& usbip)
    : _server(WEB_SERVER_PORT), _ws("/ws"), _wifi(wifi), _device(device), _usbip(usbip) {}

void WebUI::begin() {
    _server.on("/",                  HTTP_GET,  [this](AsyncWebServerRequest* r){ _handleRoot(r); });
    _server.on("/login",             HTTP_POST, [this](AsyncWebServerRequest* r){ _handleLogin(r); });
    _server.on("/api/logout",        HTTP_POST, [this](AsyncWebServerRequest* r){ _handleLogout(r); });
    _server.on("/api/usbip/kick",    HTTP_POST, [this](AsyncWebServerRequest* r){ if (_requireAuth(r)) _handleUsbIpKick(r); });
    _server.on("/api/wifi/scan",     HTTP_GET,  [this](AsyncWebServerRequest* r){ if (_requireAuth(r)) _handleWifiScan(r); });
    _server.on("/api/wifi/connect",  HTTP_POST, [this](AsyncWebServerRequest* r){ if (_requireAuth(r)) _handleWifiConnect(r); });
    _server.on("/api/wifi/result",   HTTP_GET,  [this](AsyncWebServerRequest* r){ if (_requireAuth(r)) _handleWifiResult(r); });
    _server.on("/api/logs",          HTTP_GET,  [this](AsyncWebServerRequest* r){
        if (_requireAuth(r)) r->send(200, "text/plain; charset=utf-8", PersistLog::dump());
    });
    _server.on("/api/restart",       HTTP_POST, [this](AsyncWebServerRequest* r){ if (_requireAuth(r)) _handleRestart(r); });

    // Live status. Handlers that fail the filter are skipped, so an upgrade request without the
    // login cookie falls through to the 404 below.
    _ws.setFilter([this](AsyncWebServerRequest* r){ return _authedReq(r); });
    _ws.onEvent([this](AsyncWebSocket*, AsyncWebSocketClient* c, AwsEventType type, void*, uint8_t*, size_t) {
        if (type == WS_EVT_CONNECT) {
            debugI("Status WebSocket #%u connected", (unsigned)c->id());
            c->text(_statusJson());                // first frame right away, no wait for the next tick
        } else if (type == WS_EVT_DISCONNECT) {
            debugI("Status WebSocket #%u closed", (unsigned)c->id());
        }
    });
    _server.addHandler(&_ws);

    _server.onNotFound([](AsyncWebServerRequest* r){
        if (r->url() == "/ws") debugW("Status WebSocket refused (not logged in) - reload the dashboard page");
        r->send(404, "text/plain", "Not Found");
    });
    _server.begin();
    debugI("Web Dashboard on port %d (status WebSocket at /ws)", WEB_SERVER_PORT);
}

void WebUI::task() {
    // Push the status once a second to whoever has the dashboard open.
    if (millis() - _lastPush >= 1000) {
        _lastPush = millis();
        _ws.cleanupClients();                      // drop dead sockets
        if (_ws.count() > 0) _ws.textAll(_statusJson());
    }

    // Joining WiFi takes seconds: done here, not on the AsyncTCP task.
    if (_connState == CONN_PENDING) {
        _connState = CONN_RUNNING;
        bool ok = _wifi.setCredentials(_connSsid, _connPass);
        _connIp   = _wifi.getIP();
        _connPass = "";
        _connState = ok ? CONN_OK : CONN_FAILED;
    }
    if (_restartAt && (long)(millis() - _restartAt) >= 0) ESP.restart();
}

// ── Auth ──────────────────────────────────────────────────────────────────────

String WebUI::_arg(AsyncWebServerRequest* r, const char* name) {
    return r->hasParam(name, true) ? r->getParam(name, true)->value() : String();
}

String WebUI::_cookieToken(AsyncWebServerRequest* r) {
    if (!r->hasHeader("Cookie")) return "";
    String c = r->header("Cookie");
    int i = c.indexOf("sid=");
    if (i < 0) return "";
    int e = c.indexOf(';', i);
    return c.substring(i + 4, e < 0 ? c.length() : e);
}

bool WebUI::_authedReq(AsyncWebServerRequest* r) {
    String t = _cookieToken(r);
    if (t.length() != 32) return false;
    for (int i = 0; i < MAX_SESSIONS; i++)
        if (_tokens[i].length() == 32 && _tokens[i] == t) return true;
    return false;
}

bool WebUI::_requireAuth(AsyncWebServerRequest* r) {
    if (_authedReq(r)) return true;
    r->send(401, "application/json", "{\"error\":\"login required\"}");
    return false;
}

String WebUI::_newToken() {
    char buf[33];
    for (int i = 0; i < 4; i++) snprintf(buf + i * 8, 9, "%08x", (unsigned)esp_random());
    return String(buf);
}

void WebUI::_dropToken(const String& t) {
    if (t.length() != 32) return;
    for (int i = 0; i < MAX_SESSIONS; i++) if (_tokens[i] == t) _tokens[i] = "";
}

static bool ctEq(const String& a, const char* b) {
    size_t n = strlen(b);
    if (a.length() != n) return false;
    uint8_t d = 0;
    for (size_t i = 0; i < n; i++) d |= (uint8_t)a[i] ^ (uint8_t)b[i];
    return d == 0;
}

// Device-supplied text (USB product string) must not break the status JSON.
static String jsonEsc(String s) {
    s.replace("\\", "\\\\");
    s.replace("\"", "\\\"");
    for (unsigned i = 0; i < s.length(); i++)
        if ((uint8_t)s[i] < 0x20) s.setCharAt(i, ' ');
    return s;
}

// ── Handlers (AsyncTCP task: never block) ─────────────────────────────────────

void WebUI::_handleRoot(AsyncWebServerRequest* r) {
    AsyncWebServerResponse* res =
        r->beginResponse(200, "text/html", _authedReq(r) ? _generatePage() : _loginPage(false));
    res->addHeader("Cache-Control", "no-store");
    r->send(res);
}

void WebUI::_handleLogin(AsyncWebServerRequest* r) {
    // Throttle guessing without blocking: attempts within 1 s of a failure are refused.
    if (_lastLoginFailMs && millis() - _lastLoginFailMs < 1000) {
        r->send(429, "text/html", _loginPage(true));
        return;
    }
    bool ok = ctEq(_arg(r, "user"), WEB_USER) & ctEq(_arg(r, "password"), WEB_PASSWORD);
    if (!ok) {
        debugW("Web login failed from %s", r->client()->remoteIP().toString().c_str());
        _lastLoginFailMs = millis();
        r->send(401, "text/html", _loginPage(true));
        return;
    }
    _lastLoginFailMs = 0;
    String t = _newToken();
    _tokens[_tokenNext] = t;                       // oldest session is evicted
    _tokenNext = (_tokenNext + 1) % MAX_SESSIONS;
    AsyncWebServerResponse* res = r->beginResponse(303, "text/plain", "");
    res->addHeader("Set-Cookie", "sid=" + t + "; Path=/; HttpOnly; SameSite=Strict");
    res->addHeader("Location", "/");
    r->send(res);
}

void WebUI::_handleLogout(AsyncWebServerRequest* r) {
    _dropToken(_cookieToken(r));
    AsyncWebServerResponse* res = r->beginResponse(200, "application/json", "{\"ok\":true}");
    res->addHeader("Set-Cookie", "sid=; Path=/; Max-Age=0");
    r->send(res);
}

void WebUI::_handleUsbIpKick(AsyncWebServerRequest* r) {
    _usbip.kickClient();
    r->send(200, "application/json", "{\"ok\":true}");
}

// A scan takes seconds. First call starts one and answers 202; the page asks again until
// the results are in.
void WebUI::_handleWifiScan(AsyncWebServerRequest* r) {
    String json;
    if (_wifi.scanPoll(json)) r->send(200, "application/json", json);
    else                      r->send(202, "application/json", "{\"scanning\":true}");
}

// Joining takes up to ~15 s and drops the setup AP: accept the request here, join in task(),
// and let the page poll /api/wifi/result.
void WebUI::_handleWifiConnect(AsyncWebServerRequest* r) {
    String ssid = _arg(r, "ssid");
    String pass = _arg(r, "password");
    if (!ssid.length()) { r->send(400, "application/json", "{\"error\":\"SSID required\"}"); return; }
    if (_connState == CONN_PENDING || _connState == CONN_RUNNING) {
        r->send(409, "application/json", "{\"error\":\"already connecting\"}");
        return;
    }
    _connSsid = ssid; _connPass = pass; _connIp = "";
    __sync_synchronize();                          // strings are written before the state flips
    _connState = CONN_PENDING;
    r->send(202, "application/json", "{\"accepted\":true}");
}

void WebUI::_handleWifiResult(AsyncWebServerRequest* r) {
    const char* st = "idle";
    switch (_connState) {
        case CONN_PENDING: case CONN_RUNNING: st = "connecting"; break;
        case CONN_OK:      st = "ok";         break;
        case CONN_FAILED:  st = "failed";     break;
        default: break;
    }
    r->send(200, "application/json", String("{\"state\":\"") + st + "\",\"ip\":\"" + _connIp + "\"}");
}

void WebUI::_handleRestart(AsyncWebServerRequest* r) {
    r->send(200, "application/json", "{\"ok\":true}");
    _restartAt = millis() + 300;                   // task() restarts after the reply is out
    if (!_restartAt) _restartAt = 1;
}

String WebUI::_loginPage(bool failed) {
    String p = R"rawliteral(<!DOCTYPE html><html lang="en"><head><meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>Login</title>
<style>*{box-sizing:border-box;margin:0;padding:0}
body{font-family:-apple-system,'Segoe UI',sans-serif;background:#0f172a;color:#e2e8f0;min-height:100vh;display:flex;align-items:center;justify-content:center}
form{background:#1e293b;border:1px solid #334155;border-radius:10px;padding:24px;width:min(340px,92vw)}
h1{font-size:1.2rem;margin-bottom:16px}
input{width:100%;padding:9px 11px;margin-bottom:10px;background:#0f172a;border:1px solid #475569;border-radius:7px;color:#e2e8f0;font-size:.9rem}
input:focus{outline:none;border-color:#3b82f6}
button{width:100%;padding:9px;border:none;border-radius:7px;background:#3b82f6;color:#fff;font-weight:600;cursor:pointer}
.err{color:#fca5a5;font-size:.82rem;margin-bottom:10px}
.pw{position:relative}.pw input{padding-right:58px}
.pw button{position:absolute;right:4px;top:4px;width:auto;padding:5px 9px;background:none;color:#94a3b8;font-size:.78rem;font-weight:500}
.pw button:hover{color:#e2e8f0}</style></head><body>
<form method="POST" action="/login"><h1>USB/IP Server Login</h1>)rawliteral";
    if (failed) p += "<div class=\"err\">Invalid username or password</div>";
    p += R"rawliteral(<input name="user" placeholder="Username" autocomplete="username" autofocus>
<div class="pw"><input id="pw" name="password" type="password" placeholder="Password" autocomplete="current-password">
<button type="button" id="pwt" aria-label="Show password" onclick="var i=document.getElementById('pw'),s=i.type=='password';i.type=s?'text':'password';this.textContent=s?'Hide':'Show';this.setAttribute('aria-label',s?'Hide password':'Show password')">Show</button></div>
<button type="submit">Sign in</button></form></body></html>)rawliteral";
    return p;
}

String WebUI::_statusJson() {
    String j = "{";

    // WiFi
    j += "\"wifi\":{\"connected\":"  + String(_wifi.isConnected() ? "true" : "false")
       + ",\"ssid\":\""   + _wifi.getSSID() + "\""
       + ",\"ip\":\""     + _wifi.getIP()   + "\""
       + ",\"rssi\":"     + String(_wifi.getRSSI())
       + ",\"mode\":\""   + (_wifi.getMode() == WSTATE_STATION ? "station" : "ap") + "\"},";

    // USB device
    j += "\"usbdev\":{\"connected\":" + String(_device.isConnected() ? "true" : "false")
       + ",\"ready\":"    + (_device.isReady()   ? "true" : "false")
       + ",\"product\":\"" + jsonEsc(_device.getProduct()) + "\""
       + ",\"vid\":\"0x"  + String(_device.getVendorId(),  HEX) + "\""
       + ",\"pid\":\"0x"  + String(_device.getProductId(), HEX) + "\"},";

    // USB/IP (embed full status JSON from server)
    j += "\"usbip\":" + _usbip.statusJson() + ",";

    // System
    j += "\"sys\":{\"uptime\":"  + String(millis() / 1000)
       + ",\"heap\":"    + String(ESP.getFreeHeap())
       + ",\"psram\":"   + String(ESP.getFreePsram()) + "}";

    j += "}";
    return j;
}


// ── Page ──────────────────────────────────────────────────────────────────────

String WebUI::_generatePage() {
    String p = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>%DEVNAME%</title>
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
<div id="wsb" style="display:none;position:fixed;top:0;left:0;right:0;z-index:9;padding:6px 12px;background:#92400e;color:#fef3c7;font-size:.8rem;text-align:center">Live status disconnected - reconnecting... (reload the page if this persists)</div>
<div class="wrap">
<div style="display:flex;justify-content:space-between;align-items:center"><h1>%DEVNAME%</h1><button class="btn bs" onclick="logout()">Logout</button></div>

<!-- ── USB device ─────────────────────────────────────────────────── -->
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
  <button class="btn bs" onclick="copyCmd(this)">Copy</button>
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

<!-- ── Logs ─────────────────────────────────────────────────────────── -->
<div class="card">
<h2>Logs</h2>
<div class="brow" style="margin-top:0;margin-bottom:8px">
  <button class="btn bp" onclick="loadLogs()">Load / Refresh</button>
  <button class="btn bs" onclick="copyLogs(this)">Copy</button>
</div>
<pre id="logbox" style="background:#0f172a;border:1px solid #334155;border-radius:6px;padding:8px;font-size:.7rem;color:#cbd5e1;max-height:320px;overflow:auto;white-space:pre-wrap;word-break:break-all">Press Load to view (current boot + previous boots, survives crashes).</pre>
</div>

<div class="foot">%DEVNAME% · USB/IP Server · ESP32-S3</div>
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

function applyStatus(d) {

    // USB device
    var pc = document.getElementById('p-conn');
    if (d.usbdev.ready)       { pc.textContent='Ready';       pc.className='badge g'; }
    else if(d.usbdev.connected){ pc.textContent='Connected';  pc.className='badge y'; }
    else                        { pc.textContent='Disconnected';pc.className='badge r'; }
    document.getElementById('p-model').textContent = d.usbdev.product || '—';
    document.getElementById('p-ids').textContent   = d.usbdev.connected
      ? d.usbdev.vid + ':' + d.usbdev.pid : '—';

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
      sess.innerHTML = '<div class="no-client">No client attached — only one client may connect at a time</div>';
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

}

function loadLogs() {
  var b = document.getElementById('logbox');
  b.textContent = 'Loading...';
  fetch('/api/logs').then(function(r){ if (r.status===401){location.reload();throw 0;} return r.text(); })
    .then(function(t){ b.textContent = t; b.scrollTop = 0; }).catch(function(){});
}
// navigator.clipboard only exists on HTTPS/localhost; this page is plain HTTP,
// so fall back to a hidden textarea + execCommand('copy').
function copyText(txt, btn) {
  function done(ok) {
    if (!btn) return;
    var old = btn.textContent;
    btn.textContent = ok ? 'Copied!' : 'Failed';
    setTimeout(function(){ btn.textContent = old; }, 1500);
  }
  function legacy() {
    var ta = document.createElement('textarea');
    ta.value = txt; ta.style.position = 'fixed'; ta.style.opacity = '0';
    document.body.appendChild(ta); ta.focus(); ta.select();
    var ok = false;
    try { ok = document.execCommand('copy'); } catch (e) {}
    document.body.removeChild(ta);
    done(ok);
  }
  if (navigator.clipboard && window.isSecureContext)
    navigator.clipboard.writeText(txt).then(function(){ done(true); }, legacy);
  else legacy();
}
function copyLogs(btn) {
  copyText(document.getElementById('logbox').textContent, btn);
}

function kickClient() {
  if (!confirm('Disconnect the current USB/IP client?')) return;
  fetch('/api/usbip/kick', {method:'POST'});     // the next pushed status shows the result
}

function logout() {
  fetch('/api/logout', {method:'POST'}).then(function(){ location.reload(); });
}

function copyCmd(btn) {
  copyText(document.getElementById('u-cmd').textContent, btn);
}

function scan() {
  var s = document.getElementById('nets');
  s.innerHTML = '<option>Scanning...</option>';
  var tries = 0;
  (function poll() {
    fetch('/api/wifi/scan').then(function(r) {
      if (r.status === 202 && tries++ < 15) { setTimeout(poll, 1000); throw 0; }   // still scanning
      return r.json();
    }).then(function(nets) {
      if (!Array.isArray(nets)) { s.innerHTML = '<option value="">Scan failed - try again</option>'; return; }
      s.innerHTML = '<option value="">— Select network —</option>';
      nets.sort(function(a, b) { return b.rssi - a.rssi; }).forEach(function(n) {
        var o = document.createElement('option');
        o.value = n.ssid;
        o.textContent = n.ssid + ' (' + n.rssi + ' dBm)' + (n.secure ? ' 🔒' : '');
        s.appendChild(o);
      });
    }).catch(function() {});
  })();
}

function connect() {
  var ssid = document.getElementById('nets').value;
  var pass = document.getElementById('pass').value;
  if (!ssid) { alert('Select a network'); return; }
  fetch('/api/wifi/connect', {method:'POST',
    headers:{'Content-Type':'application/x-www-form-urlencoded'},
    body:'ssid='+encodeURIComponent(ssid)+'&password='+encodeURIComponent(pass)})
  .then(function(r) { return r.json(); }).then(function(d) {
    if (!d.accepted) { alert(d.error || 'Busy, try again'); return; }
    pollWifi(0);
  });
}

// The device joins in the background and the setup network drops while it does, so the page
// polls for the result and explains what to do if contact is lost.
function pollWifi(n) {
  fetch('/api/wifi/result').then(function(r) { return r.json(); }).then(function(d) {
    if (d.state === 'ok') { alert('Connected!\nNew IP: ' + d.ip + '\nRefreshing...'); setTimeout(function() { location.href = 'http://' + d.ip; }, 5000); }
    else if (d.state === 'failed') alert('Connection failed — check password');
    else if (n < 30) setTimeout(function() { pollWifi(n + 1); }, 1000);
    else alert('Still connecting - check the device log');
  }).catch(function() {
    if (n < 30) setTimeout(function() { pollWifi(n + 1); }, 1500);
    else alert('Lost contact. The setup network drops when the device joins your WiFi: reconnect to your normal network and open the device IP.');
  });
}

function restart() {
  if (!confirm('Restart the ESP32?')) return;
  fetch('/api/restart', {method:'POST'});
  alert('Restarting… refresh in 15s');
  setTimeout(()=>location.reload(), 15000);
}

// Status is pushed over a WebSocket once a second; the page never polls. The first frame
// arrives as soon as the socket opens. If the socket drops it reconnects with a growing delay.
var wsLive = false, wsDelay = 1000;
function setLive(on) {
  wsLive = on;
  var b = document.getElementById('wsb');
  if (b) b.style.display = on ? 'none' : 'block';
}
function startWs() {
  var ws;
  try { ws = new WebSocket('ws://' + location.host + '/ws'); }
  catch (e) { setLive(false); setTimeout(startWs, wsDelay); return; }
  ws.onopen    = function()  { wsDelay = 1000; };
  ws.onmessage = function(e) { setLive(true); try { applyStatus(JSON.parse(e.data)); } catch (x) {} };
  ws.onclose   = function()  { setLive(false); setTimeout(startWs, wsDelay); wsDelay = Math.min(wsDelay * 2, 10000); };
}
setLive(false);
startWs();
</script>
</body>
</html>
)rawliteral";
    p.replace("%DEVNAME%", DEVICE_NAME);
    return p;
}
