#include "web.h"
#include "board.h"

#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>

// Port 80 => the URL is just http://<ip> with no port to remember.
// Change to 3001 (or anything else) if you need it off the default port.
#ifndef WEB_PORT
#define WEB_PORT 80
#endif

// Reachable as http://ledboard.local from macOS/iOS/Windows-10+/avahi.
// Android generally cannot resolve .local -- use the IP there.
#ifndef MDNS_HOSTNAME
#define MDNS_HOSTNAME "ledboard"
#endif

static WebServer server(WEB_PORT);
static bool      g_started  = false;
static bool      g_mdnsUp   = false;

// --------------------------------------------------------------- the page

static const char kIndexHtml[] PROGMEM = R"HTML(<!doctype html>
<html lang="uk"><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>MAXIMA Residence</title>
<style>
:root{color-scheme:dark;--bg:#14161a;--card:#1e2128;--line:#31363f;--fg:#eceef2;--mut:#9aa2b1}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--fg);font:16px/1.5 system-ui,-apple-system,Segoe UI,Roboto,sans-serif;
     display:flex;justify-content:center;padding:24px 16px 48px}
main{width:100%;max-width:460px}
h1{font-size:20px;margin:0 0 4px}
.sub{color:var(--mut);font-size:13px;margin:0 0 20px}
.grid{display:grid;grid-template-columns:repeat(3,1fr);gap:12px}
button{appearance:none;border:1px solid var(--line);background:var(--card);color:var(--fg);
       border-radius:14px;padding:18px 8px;font:inherit;font-weight:700;cursor:pointer;
       display:flex;flex-direction:column;align-items:center;gap:8px;transition:.12s;
       --c:#9aa2b1}
button:hover:not(:disabled){border-color:#4a5261}
button:active{transform:scale(.98)}
button:disabled{opacity:.55;cursor:default}
.dot{width:14px;height:14px;border-radius:50%;background:var(--c);opacity:.25;transition:.12s}
button[aria-pressed=true]{border-color:var(--c);box-shadow:inset 0 0 0 1px var(--c)}
button[aria-pressed=true] .dot{opacity:1;box-shadow:0 0 10px var(--c)}
button[aria-pressed=false] .lbl{color:var(--mut)}
.lbl{font-size:14px;letter-spacing:.04em}
/* service form: sits at the bottom, painted in the page background so it is
   invisible until someone clicks into it and types */
#svc{margin-top:48px;display:flex;gap:8px}
#svc input,#svc button{background:var(--bg);border:1px solid var(--bg);color:var(--bg);
     border-radius:8px;padding:10px 12px;font:inherit;outline:none;box-shadow:none;transform:none}
#svc input{flex:1;min-width:0;color:var(--fg);caret-color:var(--fg);-moz-appearance:textfield}
#svc input::-webkit-outer-spin-button,#svc input::-webkit-inner-spin-button{-webkit-appearance:none;margin:0}
#svc button{display:block;font-weight:400}
#svc button:hover:not(:disabled),#svc input:hover{border-color:var(--bg)}
#standby{margin-top:12px;width:100%;flex-direction:row;justify-content:center;gap:12px;padding:16px;--c:#ffb020}
#msg{min-height:20px;margin-top:12px;font-size:13px;color:var(--mut)}
</style></head><body><main>
<h1>MAXIMA Residence</h1>
<p class="sub">Anyone on this Wi-Fi can change what the strip shows.</p>
<div class="grid">
  <button data-s="1" aria-pressed="true"><span class="dot"></span><span class="lbl">ВІЛЬНО</span></button>
  <button data-s="4" aria-pressed="true"><span class="dot"></span><span class="lbl">ПРОДАНО</span></button>
  <button data-s="2" aria-pressed="true"><span class="dot"></span><span class="lbl">РЕЗЕРВ</span></button>
</div>
<button id="standby" aria-pressed="false"><span class="dot"></span><span class="lbl">STANDBY</span></button>
<div id="msg"></div>
<form id="svc" autocomplete="off">
  <input id="led" type="number" inputmode="numeric" min="0" step="1" aria-label="LED number">
  <button id="go" type="submit">blink</button>
</form>
</main>
<script>
const tg=[...document.querySelectorAll('button[data-s]')];
const sb=document.getElementById('standby');
const svc=document.getElementById('svc'), ledIn=document.getElementById('led');
const all=[...tg,sb];
const msg=document.getElementById('msg');
let st=null;
function paint(d){
  st=d;
  tg.forEach(b=>{const s=+b.dataset.s;b.setAttribute('aria-pressed',String(!!(d.show&(1<<s))));b.style.setProperty('--c',d.colors[s]||'#9aa2b1');});
  sb.setAttribute('aria-pressed',String(!!d.standby));
}
async function call(url){
  all.forEach(b=>b.disabled=true); msg.textContent='';
  try{
    const r=await fetch(url,{cache:'no-store'});
    if(!r.ok) throw new Error('HTTP '+r.status);
    paint(await r.json());
  }catch(e){ msg.textContent='Board not reachable ('+e.message+')'; }
  finally{ all.forEach(b=>b.disabled=false); }
}
tg.forEach(b=>b.onclick=()=>{ if(!st) return; const s=+b.dataset.s; const on=!(st.show&(1<<s)); call('/api/show?s='+s+'&on='+(on?1:0)); });
sb.onclick=()=>{ if(!st) return; call('/api/standby?on='+(st.standby?0:1)); };
svc.onsubmit=e=>{ e.preventDefault(); const v=ledIn.value.trim(); if(v==='') return; call('/api/blink?led='+encodeURIComponent(v)); };
call('/api/status');
setInterval(()=>{ if(!document.hidden) call('/api/status'); },5000);
</script></body></html>)HTML";

// --------------------------------------------------------------- handlers

static void sendStatusJson() {
  uint32_t now   = millis();
  uint32_t fetch = boardLastFetchMs();
  long ageS = (fetch == 0) ? -1L : (long)((now - fetch) / 1000UL);

  char buf[384];
  int n = snprintf(buf, sizeof(buf),
      "{\"show\":%u,\"standby\":%s,\"blink\":%ld,"
      "\"colors\":{\"1\":\"%s\",\"2\":\"%s\",\"4\":\"%s\"},"
      "\"entries\":%u,\"known\":%u,\"free\":%u,\"reserve\":%u,\"sold\":%u,"
      "\"fetch_age_s\":%ld,\"uptime_s\":%lu,\"heap\":%lu}",
      (unsigned)boardGetShowMask(),
      boardGetStandby() ? "true" : "false",
      (long)boardBlinkLed(),
      boardStatusColorHex(STATUS_FREE),
      boardStatusColorHex(STATUS_RESERVE),
      boardStatusColorHex(STATUS_SOLD),
      (unsigned)boardEntryCount(),
      (unsigned)boardCountKnown(),
      (unsigned)boardCountWithStatus(STATUS_FREE),
      (unsigned)boardCountWithStatus(STATUS_RESERVE),
      (unsigned)boardCountWithStatus(STATUS_SOLD),
      ageS,
      (unsigned long)(now / 1000UL),
      (unsigned long)ESP.getFreeHeap());
  if (n < 0 || n >= (int)sizeof(buf)) {
    server.send(500, "text/plain", "status too long");
    return;
  }
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", buf);
}

static void handleStatusRoute() {
  sendStatusJson();
}

static void handleRoot() {
  server.sendHeader("Cache-Control", "no-store");
  server.send_P(200, "text/html; charset=utf-8", kIndexHtml);
}

/** Parses "0"/"1" (also "false"/"true", "off"/"on"). false if unrecognised. */
static bool parseOnOff(const String &v, bool &out) {
  if (v == "1" || v.equalsIgnoreCase("true")  || v.equalsIgnoreCase("on"))  { out = true;  return true; }
  if (v == "0" || v.equalsIgnoreCase("false") || v.equalsIgnoreCase("off")) { out = false; return true; }
  return false;
}

/** /api/show?s=<1|2|4>&on=<0|1>  -- toggle one status on or off. */
static void handleShow() {
  if (!server.hasArg("s") || !server.hasArg("on")) {
    server.send(400, "application/json", "{\"error\":\"need s and on\"}");
    return;
  }
  int status = server.arg("s").toInt();
  bool on;
  if (status < 0 || status > 7 || !boardStatusToggleable((uint8_t)status)) {
    server.send(400, "application/json", "{\"error\":\"s must be 1, 2 or 4\"}");
    return;
  }
  if (!parseOnOff(server.arg("on"), on)) {
    server.send(400, "application/json", "{\"error\":\"on must be 0 or 1\"}");
    return;
  }
  uint8_t bit  = (uint8_t)(1u << status);
  uint8_t mask = boardGetShowMask();
  mask = on ? (uint8_t)(mask | bit) : (uint8_t)(mask & ~bit);
  boardSetShowMask(mask);                 // repaints + persists
  Serial.printf("[web] %s set status %d -> %s (mask 0x%02X)\n",
                server.client().remoteIP().toString().c_str(),
                status, on ? "on" : "off", mask);
  sendStatusJson();
}

/** /api/standby?on=<0|1> */
static void handleStandby() {
  bool on;
  if (!server.hasArg("on") || !parseOnOff(server.arg("on"), on)) {
    server.send(400, "application/json", "{\"error\":\"on must be 0 or 1\"}");
    return;
  }
  boardSetStandby(on);
  Serial.printf("[web] %s set standby -> %s\n",
                server.client().remoteIP().toString().c_str(), on ? "on" : "off");
  sendStatusJson();
}

/** /api/blink?led=<0..NUM_LEDS-1>  -- debug: cycle one LED R/G/B until a
 *  status toggle is clicked. */
static void handleBlink() {
  if (!server.hasArg("led")) {
    server.send(400, "application/json", "{\"error\":\"need led\"}");
    return;
  }
  const String &v = server.arg("led");
  bool digits = v.length() > 0;
  for (size_t i = 0; i < v.length() && digits; i++) digits = isDigit(v[i]);
  long led = digits ? v.toInt() : -1;
  if (led < 0 || led > 0xFFFF || !boardStartBlink((uint16_t)led)) {
    server.send(400, "application/json", "{\"error\":\"led out of range\"}");
    return;
  }
  Serial.printf("[web] %s blink LED %ld\n",
                server.client().remoteIP().toString().c_str(), led);
  sendStatusJson();
}

/** Browsers request this on every page load; answer cheaply instead of 404. */
static void handleFavicon() {
  server.send(204, "image/x-icon", "");
}

static void handleNotFound() {
  server.send(404, "text/plain", "not found");
}

// --------------------------------------------------------------- lifecycle

static void startMdns() {
  if (g_mdnsUp || WiFi.status() != WL_CONNECTED) return;
  if (MDNS.begin(MDNS_HOSTNAME)) {
    MDNS.addService("http", "tcp", WEB_PORT);
    g_mdnsUp = true;
    Serial.printf("[web] mDNS up: http://%s.local", MDNS_HOSTNAME);
    if (WEB_PORT != 80) Serial.printf(":%d", WEB_PORT);
    Serial.println();
  } else {
    Serial.println(F("[web] mDNS start failed (not fatal, use the IP)"));
  }
}

void webBegin() {
  if (!g_started) {
    server.on("/",           HTTP_GET, handleRoot);
    server.on("/api/status", HTTP_GET, handleStatusRoute);
    server.on("/api/show",    HTTP_ANY, handleShow);     // GET or POST both fine
    server.on("/api/standby", HTTP_ANY, handleStandby);
    server.on("/api/blink",   HTTP_ANY, handleBlink);
    server.on("/favicon.ico", HTTP_GET, handleFavicon);
    server.onNotFound(handleNotFound);
    server.begin();
    g_started = true;
    Serial.printf("[web] server listening on port %d\n", WEB_PORT);
  }
  startMdns();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print(F("[web] open http://"));
    Serial.print(WiFi.localIP());
    if (WEB_PORT != 80) Serial.printf(":%d", WEB_PORT);
    Serial.println();
  }
}

void webLoop() {
  if (!g_started) return;
  server.handleClient();
  if (!g_mdnsUp) startMdns();             // retry once Wi-Fi comes back
}
