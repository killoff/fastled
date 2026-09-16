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
<html lang="en"><head>
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
.grid{display:grid;grid-template-columns:1fr 1fr;gap:12px}
button{appearance:none;border:1px solid var(--line);background:var(--card);color:var(--fg);
       border-radius:14px;padding:20px 12px;font:inherit;font-weight:600;cursor:pointer;
       display:flex;flex-direction:column;align-items:center;gap:6px;transition:.12s}
button:hover:not(:disabled){border-color:#4a5261}
button:active{transform:scale(.98)}
button:disabled{opacity:.55;cursor:default}
button[aria-pressed=true]{border-color:currentColor;box-shadow:inset 0 0 0 1px currentColor}
.dot{width:12px;height:12px;border-radius:50%;background:currentColor}
.lbl{font-size:15px}
.hint{font-size:11px;font-weight:400;color:var(--mut)}
#on{color:#9aa2b1}#off{color:#6b7280}#busy{color:#ff4d4d}#free{color:#35d07f}
.card{margin-top:20px;background:var(--card);border:1px solid var(--line);border-radius:14px;padding:14px 16px}
dl{display:grid;grid-template-columns:auto 1fr;gap:6px 16px;margin:0;font-size:13px}
dt{color:var(--mut)}dd{margin:0;text-align:right;font-variant-numeric:tabular-nums}
#msg{min-height:20px;margin-top:12px;font-size:13px;color:var(--mut)}
</style></head><body><main>
<h1>Office LED board</h1>
<p class="sub">Anyone on this Wi-Fi can change what the strip shows.</p>
<div class="grid">
  <button id="on"   data-m="on"   aria-pressed="false"><span class="dot"></span><span class="lbl">On</span><span class="hint">last known state</span></button>
  <button id="off"  data-m="off"  aria-pressed="false"><span class="dot"></span><span class="lbl">Off</span><span class="hint">all LEDs dark</span></button>
  <button id="busy" data-m="busy" aria-pressed="false"><span class="dot"></span><span class="lbl">Busy</span><span class="hint">sold only</span></button>
  <button id="free" data-m="free" aria-pressed="false"><span class="dot"></span><span class="lbl">Free</span><span class="hint">free only</span></button>
</div>
<div id="msg"></div>
<div class="card"><dl>
  <dt>Showing</dt><dd id="s-mode">&mdash;</dd>
  <dt>Mapped offices</dt><dd id="s-count">&mdash;</dd>
  <dt>Free / Sold</dt><dd id="s-fs">&mdash;</dd>
  <dt>Last data fetch</dt><dd id="s-fetch">&mdash;</dd>
  <dt>Uptime</dt><dd id="s-up">&mdash;</dd>
  <dt>Free heap</dt><dd id="s-heap">&mdash;</dd>
</dl></div>
</main>
<script>
const btns=[...document.querySelectorAll('button[data-m]')];
const msg=document.getElementById('msg');
const ago=s=>s<0?'never':s<60?s+'s ago':s<3600?Math.floor(s/60)+'m ago':Math.floor(s/3600)+'h '+Math.floor(s%3600/60)+'m ago';
const dur=s=>s<3600?Math.floor(s/60)+'m':s<86400?Math.floor(s/3600)+'h '+Math.floor(s%3600/60)+'m':Math.floor(s/86400)+'d '+Math.floor(s%86400/3600)+'h';
function paint(d){
  btns.forEach(b=>b.setAttribute('aria-pressed',String(b.dataset.m===d.mode)));
  document.getElementById('s-mode').textContent=d.mode;
  document.getElementById('s-count').textContent=d.entries+(d.known<d.entries?' ('+(d.entries-d.known)+' unknown)':'');
  document.getElementById('s-fs').textContent=d.free+' / '+d.sold;
  document.getElementById('s-fetch').textContent=ago(d.fetch_age_s);
  document.getElementById('s-up').textContent=dur(d.uptime_s);
  document.getElementById('s-heap').textContent=(d.heap/1024).toFixed(1)+' kB';
}
async function call(url){
  btns.forEach(b=>b.disabled=true); msg.textContent='';
  try{
    const r=await fetch(url,{cache:'no-store'});
    if(!r.ok) throw new Error('HTTP '+r.status);
    paint(await r.json());
  }catch(e){ msg.textContent='Board not reachable ('+e.message+')'; }
  finally{ btns.forEach(b=>b.disabled=false); }
}
btns.forEach(b=>b.onclick=()=>call('/api/mode?m='+b.dataset.m));
call('/api/status');
setInterval(()=>{ if(!document.hidden) call('/api/status'); },5000);
</script></body></html>)HTML";

// --------------------------------------------------------------- handlers

static void sendStatusJson() {
  uint32_t now   = millis();
  uint32_t fetch = boardLastFetchMs();
  long ageS = (fetch == 0) ? -1L : (long)((now - fetch) / 1000UL);

  char buf[256];
  int n = snprintf(buf, sizeof(buf),
      "{\"mode\":\"%s\",\"entries\":%u,\"known\":%u,\"free\":%u,\"sold\":%u,"
      "\"fetch_age_s\":%ld,\"uptime_s\":%lu,\"heap\":%lu}",
      boardViewModeName(boardGetViewMode()),
      (unsigned)boardEntryCount(),
      (unsigned)boardCountKnown(),
      (unsigned)boardCountWithStatus(STATUS_FREE),
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

static void handleMode() {
  if (!server.hasArg("m")) {
    server.send(400, "application/json", "{\"error\":\"missing m\"}");
    return;
  }
  ViewMode mode;
  if (!boardParseViewMode(server.arg("m").c_str(), mode)) {
    server.send(400, "application/json", "{\"error\":\"bad mode\"}");
    return;
  }
  boardSetViewMode(mode);                 // repaints + persists
  Serial.printf("[web] %s set mode -> %s\n",
                server.client().remoteIP().toString().c_str(),
                boardViewModeName(mode));
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
    server.on("/api/mode",   HTTP_ANY, handleMode);   // GET or POST both fine
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
