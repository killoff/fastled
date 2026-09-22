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
<title>LED board</title>
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
.hint{font-size:11px;font-weight:400;color:var(--mut)}
#standby{margin-top:12px;width:100%;flex-direction:row;justify-content:center;gap:12px;padding:16px;--c:#ffb020}
.card{margin-top:20px;background:var(--card);border:1px solid var(--line);border-radius:14px;padding:14px 16px}
dl{display:grid;grid-template-columns:auto 1fr;gap:6px 16px;margin:0;font-size:13px}
dt{color:var(--mut)}dd{margin:0;text-align:right;font-variant-numeric:tabular-nums}
#msg{min-height:20px;margin-top:12px;font-size:13px;color:var(--mut)}
</style></head><body><main>
<h1>Office LED board</h1>
<p class="sub">Anyone on this Wi-Fi can change what the strip shows.</p>
<div class="grid">
  <button data-s="1" aria-pressed="true"><span class="dot"></span><span class="lbl">ВІЛЬНО</span></button>
  <button data-s="4" aria-pressed="true"><span class="dot"></span><span class="lbl">ПРОДАНО</span></button>
  <button data-s="2" aria-pressed="true"><span class="dot"></span><span class="lbl">РЕЗЕРВ</span></button>
</div>
<button id="standby" aria-pressed="false"><span class="dot"></span><span class="lbl">STANDBY</span><span class="hint">strip off, pin 8 high</span></button>
<div id="msg"></div>
<div class="card"><dl>
  <dt>Mapped offices</dt><dd id="s-count">&mdash;</dd>
  <dt>Free / Reserve / Sold</dt><dd id="s-frs">&mdash;</dd>
  <dt>Last data fetch</dt><dd id="s-fetch">&mdash;</dd>
  <dt>Uptime</dt><dd id="s-up">&mdash;</dd>
  <dt>Free heap</dt><dd id="s-heap">&mdash;</dd>
</dl></div>
</main>
<script>
const tg=[...document.querySelectorAll('button[data-s]')];
const sb=document.getElementById('standby');
const all=[...tg,sb];
const msg=document.getElementById('msg');
let st=null;
const ago=s=>s<0?'never':s<60?s+'s ago':s<3600?Math.floor(s/60)+'m ago':Math.floor(s/3600)+'h '+Math.floor(s%3600/60)+'m ago';
const dur=s=>s<3600?Math.floor(s/60)+'m':s<86400?Math.floor(s/3600)+'h '+Math.floor(s%3600/60)+'m':Math.floor(s/86400)+'d '+Math.floor(s%86400/3600)+'h';
function paint(d){
  st=d;
  tg.forEach(b=>{const s=+b.dataset.s;b.setAttribute('aria-pressed',String(!!(d.show&(1<<s))));b.style.setProperty('--c',d.colors[s]||'#9aa2b1');});
  sb.setAttribute('aria-pressed',String(!!d.standby));
  document.getElementById('s-count').textContent=d.entries+(d.known<d.entries?' ('+(d.entries-d.known)+' unknown)':'');
  document.getElementById('s-frs').textContent=d.free+' / '+d.reserve+' / '+d.sold;
  document.getElementById('s-fetch').textContent=ago(d.fetch_age_s);
  document.getElementById('s-up').textContent=dur(d.uptime_s);
  document.getElementById('s-heap').textContent=(d.heap/1024).toFixed(1)+' kB';
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
      "{\"show\":%u,\"standby\":%s,"
      "\"colors\":{\"1\":\"%s\",\"2\":\"%s\",\"4\":\"%s\"},"
      "\"entries\":%u,\"known\":%u,\"free\":%u,\"reserve\":%u,\"sold\":%u,"
      "\"fetch_age_s\":%ld,\"uptime_s\":%lu,\"heap\":%lu}",
      (unsigned)boardGetShowMask(),
      boardGetStandby() ? "true" : "false",
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
