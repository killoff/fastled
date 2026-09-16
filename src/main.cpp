/**
 * WS2812B office-status board  --  Arduino Nano ESP32 (ESP32-S3)
 *
 * Flow:
 *   1. Boot: restore last-good mapping + statuses + view mode from NVS and
 *      light the strip immediately, before Wi-Fi is up.
 *   2. Connect Wi-Fi, fetch config / mapping / data, start the LAN web UI.
 *   3. Re-fetch every hour. Any resource that fails to fetch keeps its last
 *      known-good value (NVS-backed, so it survives a power cut).
 *
 * Web UI (see web.cpp): http://<ip>  --  four buttons, On / Off / Busy / Free.
 *
 * Memory notes:
 *   - sanitized.json is ~100 kB. It is NEVER loaded into RAM as a whole.
 *     It is parsed record-by-record straight off the TLS socket, so peak
 *     usage for the parse is a few hundred bytes instead of ~200 kB.
 *   - No std::map / std::vector / String in the hot path: all state lives in
 *     fixed-size POD arrays, so there is no heap fragmentation over months
 *     of hourly refreshes.
 */

#define FASTLED_INTERNAL          // silence FastLED's #pragma banner
#include <Arduino.h>
#include <FastLED.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <strings.h>   // strcasecmp

#include "board.h"
#include "web.h"

// ---------------------------------------------------------------- settings

#define SSID      "xxx_2GHz_7D6DF7"
#define SSID_PWD  "hello world"

#define CONFIG_URI  "https://raw.githubusercontent.com/killoff/esp32/refs/heads/main/config.json"
#define DATA_URI    "https://raw.githubusercontent.com/killoff/esp32/refs/heads/main/sanitized.json"
#define MAPPING_URI "https://raw.githubusercontent.com/killoff/esp32/refs/heads/main/mapping.json"

#define NUM_LEDS  1500
// NOTE: FastLED takes the raw ESP32 *GPIO* number here, not the Nano's "Dx"
// silkscreen label. GPIO5 is the pin labelled D2 on the Arduino Nano ESP32.
// If you wired the strip to the pin marked D5, use 8 instead of 5.
#define LED_PIN   5

#define BRIGHTNESS         100
#define MAX_POWER_MA      2000   // strip PSU budget; FastLED dims to stay under it

static const uint32_t REFRESH_INTERVAL_MS =  5UL * 60UL * 1000UL;  // normal cadence
static const uint32_t RETRY_INTERVAL_MS   =  5UL * 60UL * 1000UL;  // after a failure
static const uint32_t WIFI_CONNECT_TIMEOUT_MS = 30000UL;
static const uint16_t HTTP_CONNECT_TIMEOUT_MS = 10000;
static const uint16_t HTTP_READ_TIMEOUT_MS    = 20000;
static const uint8_t  FETCH_ATTEMPTS          = 3;

// ---------------------------------------------------------------- state

static const size_t KEY_LEN     = 48;         // "building|number" + NUL
static const size_t MAX_ENTRIES = NUM_LEDS;   // one mapped office per LED, max

struct Entry {
  char     key[KEY_LEN];   // "Maxima 1|оф1"
  uint16_t led;            // index into leds[]
  uint8_t  status;         // 0 = never successfully fetched
  uint8_t  reserved;
};

static Entry  g_entries[MAX_ENTRIES];
static size_t g_count = 0;

// index 0..6 -> system_status 1..7, index 7 -> unknown/fallback
static const size_t COLOR_COUNT = 8;
static const size_t COLOR_UNKNOWN = 7;

static const char *const kColorKeys[COLOR_COUNT] = {
  "status_color_free",      // 1
  "status_color_reserve",   // 2
  "status_color_contract",  // 3
  "status_color_sold",      // 4
  "status_color_repair",    // 5
  "status_color_rent",      // 6
  "status_color_vip",       // 7  <-- present in config.json; ID 7 is an assumption
  "status_color_unknown"    // fallback
};

// Compiled-in fallbacks, used until (or if) config.json parses successfully.
static const char *const kColorDefaults[COLOR_COUNT] = {
  "#00FF00", "#0000FF", "#8A2BE2", "#FF0000",
  "#FFA500", "#FFFF00", "#808023", "#808080"
};

struct ColorConfig {
  char hex[COLOR_COUNT][8];   // "#RRGGBB" + NUL
};

static ColorConfig g_colors;
static CRGB        leds[NUM_LEDS];
static Preferences g_prefs;
static ViewMode    g_viewMode = VIEW_ON;

static const uint32_t CACHE_MAGIC   = 0x4C454431UL;  // 'LED1'
static const uint16_t CACHE_VERSION = 1;

struct CacheHeader {
  uint32_t magic;
  uint16_t version;
  uint16_t count;
};

static void render();

// ---------------------------------------------------------------- helpers

static bool hexPairToByte(const char *p, uint8_t &out) {
  uint8_t v = 0;
  for (int i = 0; i < 2; i++) {
    char c = p[i];
    uint8_t d;
    if      (c >= '0' && c <= '9') d = c - '0';
    else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
    else return false;
    v = (uint8_t)((v << 4) | d);
  }
  out = v;
  return true;
}

/** Accepts "#RRGGBB" or "RRGGBB". Returns false (and leaves out untouched)
 *  on anything malformed, so a bad config can never produce garbage colour. */
static bool hexToCRGB(const char *hex, CRGB &out) {
  if (!hex) return false;
  if (*hex == '#') hex++;
  if (strlen(hex) < 6) return false;
  uint8_t r, g, b;
  if (!hexPairToByte(hex + 0, r)) return false;
  if (!hexPairToByte(hex + 2, g)) return false;
  if (!hexPairToByte(hex + 4, b)) return false;
  out = CRGB(r, g, b);
  return true;
}

static CRGB statusToColor(uint8_t status) {
  size_t idx = (status >= 1 && status <= 7) ? (size_t)(status - 1) : COLOR_UNKNOWN;
  CRGB c;
  if (hexToCRGB(g_colors.hex[idx], c)) return c;
  if (hexToCRGB(g_colors.hex[COLOR_UNKNOWN], c)) return c;
  return CRGB::Black;
}

static void makeKey(char *dst, const char *building, const char *number) {
  snprintf(dst, KEY_LEN, "%s|%s", building ? building : "", number ? number : "");
}

static int findEntry(const Entry *table, size_t count, const char *key) {
  for (size_t i = 0; i < count; i++) {
    if (strncmp(table[i].key, key, KEY_LEN) == 0) return (int)i;
  }
  return -1;
}

// ---------------------------------------------------------------- NVS cache

static void loadColorsFromNvs() {
  for (size_t i = 0; i < COLOR_COUNT; i++) {
    strncpy(g_colors.hex[i], kColorDefaults[i], sizeof(g_colors.hex[i]) - 1);
    g_colors.hex[i][sizeof(g_colors.hex[i]) - 1] = '\0';
  }
  ColorConfig tmp;
  if (g_prefs.getBytesLength("colors") == sizeof(tmp) &&
      g_prefs.getBytes("colors", &tmp, sizeof(tmp)) == sizeof(tmp)) {
    bool sane = true;
    CRGB probe;
    for (size_t i = 0; i < COLOR_COUNT; i++) {
      tmp.hex[i][sizeof(tmp.hex[i]) - 1] = '\0';
      if (!hexToCRGB(tmp.hex[i], probe)) { sane = false; break; }
    }
    if (sane) {
      g_colors = tmp;
      Serial.println(F("[nvs] colors restored"));
    }
  }
}

static void saveColorsToNvs() {
  // NVS skips the physical write when the value is byte-identical, so this
  // is cheap to call every hour and does not chew through flash endurance.
  g_prefs.putBytes("colors", &g_colors, sizeof(g_colors));
}

static void loadEntriesFromNvs() {
  size_t len = g_prefs.getBytesLength("entries");
  if (len < sizeof(CacheHeader)) return;

  static uint8_t buf[sizeof(CacheHeader) + MAX_ENTRIES * sizeof(Entry)];
  if (len > sizeof(buf)) {
    Serial.println(F("[nvs] cached entries larger than buffer, ignoring"));
    return;
  }
  if (g_prefs.getBytes("entries", buf, len) != len) return;

  CacheHeader h;
  memcpy(&h, buf, sizeof(h));
  if (h.magic != CACHE_MAGIC || h.version != CACHE_VERSION) {
    Serial.println(F("[nvs] cache magic/version mismatch, ignoring"));
    return;
  }
  if (h.count > MAX_ENTRIES) return;
  if (len != sizeof(CacheHeader) + (size_t)h.count * sizeof(Entry)) {
    Serial.println(F("[nvs] cache size mismatch, ignoring"));
    return;
  }

  memcpy(g_entries, buf + sizeof(CacheHeader), (size_t)h.count * sizeof(Entry));
  g_count = h.count;
  for (size_t i = 0; i < g_count; i++) {
    g_entries[i].key[KEY_LEN - 1] = '\0';
    if (g_entries[i].led >= NUM_LEDS) g_entries[i].led = 0xFFFF;  // ignore later
  }
  Serial.printf("[nvs] restored %u cached entries\n", (unsigned)g_count);
}

static void saveEntriesToNvs() {
  static uint8_t buf[sizeof(CacheHeader) + MAX_ENTRIES * sizeof(Entry)];
  CacheHeader h = { CACHE_MAGIC, CACHE_VERSION, (uint16_t)g_count };
  memcpy(buf, &h, sizeof(h));
  memcpy(buf + sizeof(h), g_entries, g_count * sizeof(Entry));
  size_t len = sizeof(h) + g_count * sizeof(Entry);
  if (g_prefs.putBytes("entries", buf, len) != len) {
    Serial.println(F("[nvs] WARNING: failed to persist entries"));
  }
}

static void loadViewModeFromNvs() {
  uint8_t v = g_prefs.getUChar("view", (uint8_t)VIEW_ON);
  g_viewMode = (v <= (uint8_t)VIEW_FREE) ? (ViewMode)v : VIEW_ON;
}

// ---------------------------------------------------------------- board API
// (declared in board.h, consumed by web.cpp)

ViewMode boardGetViewMode() { return g_viewMode; }

const char *boardViewModeName(ViewMode mode) {
  switch (mode) {
    case VIEW_OFF:  return "off";
    case VIEW_BUSY: return "busy";
    case VIEW_FREE: return "free";
    case VIEW_ON:
    default:        return "on";
  }
}

bool boardParseViewMode(const char *name, ViewMode &out) {
  if (!name) return false;
  if      (!strcasecmp(name, "on"))   out = VIEW_ON;
  else if (!strcasecmp(name, "off"))  out = VIEW_OFF;
  else if (!strcasecmp(name, "busy")) out = VIEW_BUSY;
  else if (!strcasecmp(name, "free")) out = VIEW_FREE;
  else return false;
  return true;
}

void boardSetViewMode(ViewMode mode) {
  g_viewMode = mode;
  g_prefs.putUChar("view", (uint8_t)mode);   // survives a power cut
  render();
}

size_t boardEntryCount() { return g_count; }

size_t boardCountWithStatus(uint8_t status) {
  size_t n = 0;
  for (size_t i = 0; i < g_count; i++) if (g_entries[i].status == status) n++;
  return n;
}

size_t boardCountKnown() {
  size_t n = 0;
  for (size_t i = 0; i < g_count; i++) if (g_entries[i].status != 0) n++;
  return n;
}

static uint32_t g_lastFetchMs = 0;          // millis() of last good data fetch
uint32_t boardLastFetchMs() { return g_lastFetchMs; }

// ---------------------------------------------------------------- http

/** Opens an HTTPS GET. On success the caller must read from http.getStream()
 *  and then call http.end(). Returns false if anything went wrong. */
static bool httpOpen(WiFiClientSecure &client, HTTPClient &http, const char *url) {
  // NOTE: setInsecure() skips certificate validation. It is fine for public
  // read-only data on a trusted LAN; to harden, pin GitHub's root CA with
  // client.setCACert(kGithubRootCa) instead.
  client.setInsecure();
  client.setTimeout(HTTP_READ_TIMEOUT_MS / 1000);

  http.setConnectTimeout(HTTP_CONNECT_TIMEOUT_MS);
  http.setTimeout(HTTP_READ_TIMEOUT_MS);
  http.setReuse(false);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  if (!http.begin(client, url)) {
    Serial.printf("[http] begin() failed for %s\n", url);
    return false;
  }
  // HTTP/1.0 => no chunked transfer encoding, which makes streaming parsing
  // dramatically faster and simpler (recommended by ArduinoJson).
  http.useHTTP10(true);

  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("[http] GET %s -> %d\n", url, code);
    http.end();
    return false;
  }
  return true;
}

// ---------------------------------------------------------------- fetchers

/** config.json -> g_colors. Small file, safe to parse whole. */
static bool fetchConfig() {
  WiFiClientSecure client;
  HTTPClient http;
  if (!httpOpen(client, http, CONFIG_URI)) return false;

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, http.getStream());
  http.end();
  if (err) {
    // As of now the published config.json is INVALID (missing comma before
    // "status_color_vip"), so expect this branch until it is fixed upstream.
    Serial.printf("[config] parse failed: %s\n", err.c_str());
    return false;
  }

  ColorConfig tmp = g_colors;   // start from current/defaults
  size_t applied = 0;
  CRGB probe;
  for (size_t i = 0; i < COLOR_COUNT; i++) {
    const char *v = doc[kColorKeys[i]];
    if (v && hexToCRGB(v, probe)) {
      strncpy(tmp.hex[i], v, sizeof(tmp.hex[i]) - 1);
      tmp.hex[i][sizeof(tmp.hex[i]) - 1] = '\0';
      applied++;
    }
  }
  if (applied == 0) {
    Serial.println(F("[config] no usable colours in payload, keeping previous"));
    return false;
  }
  g_colors = tmp;
  saveColorsToNvs();
  Serial.printf("[config] %u colours applied\n", (unsigned)applied);
  return true;
}

/** mapping.json is nested:  { "Maxima 1": { "оф1": 5, ... }, ... }
 *  Flattened here into "building|number" -> led. Statuses already known for a
 *  key are carried over so a mapping refresh never blanks the board. */
static bool fetchMapping() {
  WiFiClientSecure client;
  HTTPClient http;
  if (!httpOpen(client, http, MAPPING_URI)) return false;

  JsonDocument doc;                       // mapping.json is < 2 kB
  DeserializationError err = deserializeJson(doc, http.getStream());
  http.end();
  if (err) {
    Serial.printf("[mapping] parse failed: %s\n", err.c_str());
    return false;
  }
  JsonObject root = doc.as<JsonObject>();
  if (root.isNull()) {
    Serial.println(F("[mapping] root is not an object"));
    return false;
  }

  static Entry tmp[MAX_ENTRIES];
  size_t n = 0;
  bool ledUsed[NUM_LEDS] = { false };
  bool truncated = false;

  for (JsonPair building : root) {
    JsonObject rooms = building.value().as<JsonObject>();
    if (rooms.isNull()) {
      Serial.printf("[mapping] '%s' is not an object, skipped\n", building.key().c_str());
      continue;
    }
    for (JsonPair room : rooms) {
      if (n >= MAX_ENTRIES) { truncated = true; break; }

      int led = room.value().as<int>();
      if (!room.value().is<int>() || led < 0 || led >= NUM_LEDS) {
        Serial.printf("[mapping] %s/%s -> %d out of range 0..%d, skipped\n",
                      building.key().c_str(), room.key().c_str(), led, NUM_LEDS - 1);
        continue;
      }
      if (ledUsed[led]) {
        Serial.printf("[mapping] LED %d assigned more than once (%s/%s)\n",
                      led, building.key().c_str(), room.key().c_str());
      }
      ledUsed[led] = true;

      memset(&tmp[n], 0, sizeof(tmp[n]));
      makeKey(tmp[n].key, building.key().c_str(), room.key().c_str());
      tmp[n].led    = (uint16_t)led;
      tmp[n].status = 0;

      int old = findEntry(g_entries, g_count, tmp[n].key);
      if (old >= 0) tmp[n].status = g_entries[old].status;   // carry last-known
      n++;
    }
    if (truncated) break;
  }

  if (truncated) {
    Serial.printf("[mapping] more than %u entries, extras ignored "
                  "(raise NUM_LEDS/MAX_ENTRIES)\n", (unsigned)MAX_ENTRIES);
  }
  if (n == 0) {
    Serial.println(F("[mapping] empty result, keeping previous mapping"));
    return false;
  }

  memcpy(g_entries, tmp, n * sizeof(Entry));
  g_count = n;
  Serial.printf("[mapping] %u offices mapped\n", (unsigned)g_count);
  return true;
}

/** sanitized.json (~100 kB) -> statuses for the mapped offices.
 *
 *  Parsed one array element at a time straight off the socket. Only records
 *  whose "building|number" is in the mapping are kept, and the new statuses
 *  are committed only if the whole stream parsed cleanly, so a truncated
 *  download can never leave the board half-updated. */
static bool fetchOffices() {
  if (g_count == 0) {
    Serial.println(F("[data] no mapping yet, skipping"));
    return false;
  }

  WiFiClientSecure client;
  HTTPClient http;
  if (!httpOpen(client, http, DATA_URI)) return false;

  // Read timeout is already set on `client` in httpOpen(). Do not call
  // setTimeout() on the stream here: on ESP32 WiFiClient::setTimeout() takes
  // SECONDS, not milliseconds, and shadows Stream::setTimeout().
  Stream &stream = http.getStream();

  if (!stream.find("\"data\"") || !stream.find("[")) {
    Serial.println(F("[data] could not locate data array"));
    http.end();
    return false;
  }

  static uint8_t newStatus[MAX_ENTRIES];
  static bool    touched[MAX_ENTRIES];
  memset(newStatus, 0, sizeof(newStatus));
  memset(touched,   0, sizeof(touched));

  size_t seen = 0, matched = 0;
  bool   ok   = true;

  do {
    JsonDocument rec;   // one record only: a few hundred bytes, freed each pass
    DeserializationError err = deserializeJson(rec, stream);
    if (err) {
      if (seen == 0 && err == DeserializationError::EmptyInput) break;  // "data": []
      Serial.printf("[data] record %u parse failed: %s\n", (unsigned)seen, err.c_str());
      ok = false;
      break;
    }
    seen++;

    const char *building = rec["building"];
    const char *number   = rec["number"];
    if (building && number) {
      char key[KEY_LEN];
      makeKey(key, building, number);
      int idx = findEntry(g_entries, g_count, key);
      if (idx >= 0) {
        int st = rec["system_status"] | 0;
        if (st < 0 || st > 255) st = 0;
        newStatus[idx] = (uint8_t)st;
        touched[idx]   = true;
        matched++;
      }
    }
  } while (stream.findUntil(",", "]"));

  http.end();

  if (!ok) {
    Serial.println(F("[data] aborted, keeping previous statuses"));
    return false;
  }
  if (matched == 0) {
    Serial.printf("[data] parsed %u records but matched none of the %u mapped "
                  "offices, keeping previous statuses\n",
                  (unsigned)seen, (unsigned)g_count);
    return false;
  }

  for (size_t i = 0; i < g_count; i++) {
    if (touched[i]) {
      g_entries[i].status = newStatus[i];
    } else {
      Serial.printf("[data] '%s' absent from payload, keeping status %u\n",
                    g_entries[i].key, (unsigned)g_entries[i].status);
    }
  }
  g_lastFetchMs = millis();
  if (g_lastFetchMs == 0) g_lastFetchMs = 1;   // 0 means "never"
  Serial.printf("[data] %u records scanned, %u mapped offices updated\n",
                (unsigned)seen, (unsigned)matched);
  return true;
}

// ---------------------------------------------------------------- rendering

static void render() {
  FastLED.clear();
  if (g_viewMode != VIEW_OFF) {
    for (size_t i = 0; i < g_count; i++) {
      uint16_t led = g_entries[i].led;
      if (led >= NUM_LEDS) continue;                 // hard bounds guard
      uint8_t st = g_entries[i].status;

      switch (g_viewMode) {
        case VIEW_ON:
          leds[led] = statusToColor(st);
          break;
        case VIEW_BUSY:
          if (st == STATUS_SOLD) leds[led] = statusToColor(STATUS_SOLD);
          break;
        case VIEW_FREE:
          if (st == STATUS_FREE) leds[led] = statusToColor(STATUS_FREE);
          break;
        default:
          break;
      }
    }
  }
  FastLED.show();                                  // one show() for the strip
}

// ---------------------------------------------------------------- wifi

static bool wifiEnsureConnected(uint32_t timeoutMs) {
  if (WiFi.status() == WL_CONNECTED) return true;

  Serial.print(F("[wifi] connecting"));
  WiFi.disconnect(false, false);
  WiFi.begin(SSID, SSID_PWD);

  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - start) < timeoutMs) {
    delay(250);
    Serial.print('.');
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print(F("[wifi] connected, ip="));
    Serial.println(WiFi.localIP());
    return true;
  }
  Serial.println(F("[wifi] connect timed out"));
  return false;
}

// ---------------------------------------------------------------- refresh

static uint32_t g_lastAttemptMs = 0;
static uint32_t g_nextDelayMs   = REFRESH_INTERVAL_MS;

template <typename Fn>
static bool withRetries(const char *what, Fn fn) {
  for (uint8_t attempt = 1; attempt <= FETCH_ATTEMPTS; attempt++) {
    if (fn()) return true;
    Serial.printf("[%s] attempt %u/%u failed\n", what,
                  (unsigned)attempt, (unsigned)FETCH_ATTEMPTS);
    if (attempt < FETCH_ATTEMPTS) delay(2000UL * attempt);
  }
  return false;
}

static void refreshAll() {
  g_lastAttemptMs = millis();

  if (!wifiEnsureConnected(WIFI_CONNECT_TIMEOUT_MS)) {
    g_nextDelayMs = RETRY_INTERVAL_MS;
    Serial.println(F("[refresh] no wifi, using cached data"));
    return;
  }

  Serial.printf("[refresh] free heap before: %u\n", (unsigned)ESP.getFreeHeap());

  withRetries("config", fetchConfig);              // non-fatal: defaults apply
  bool mapOk  = withRetries("mapping", fetchMapping);
  bool dataOk = withRetries("data", fetchOffices);

  if (mapOk || dataOk) saveEntriesToNvs();
  render();                                        // honours the current mode

  Serial.printf("[refresh] free heap after:  %u (min ever %u)\n",
                (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap());

  g_nextDelayMs = (mapOk && dataOk) ? REFRESH_INTERVAL_MS : RETRY_INTERVAL_MS;
  Serial.printf("[refresh] next attempt in %lu s\n",
                (unsigned long)(g_nextDelayMs / 1000UL));
}

// ---------------------------------------------------------------- arduino

void setup() {
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && (millis() - t0) < 2000) delay(10);   // USB-CDC, don't block forever
  Serial.println(F("\n[boot] office status board"));

  FastLED.addLeds<WS2812B, LED_PIN, GRB>(leds, NUM_LEDS);
  FastLED.setBrightness(BRIGHTNESS);
  FastLED.setMaxPowerInVoltsAndMilliamps(5, MAX_POWER_MA);
  FastLED.clear(true);

  g_prefs.begin("ledboard", false);
  loadColorsFromNvs();
  loadEntriesFromNvs();
  loadViewModeFromNvs();
  Serial.printf("[boot] view mode: %s\n", boardViewModeName(g_viewMode));
  render();                       // show last-good state before Wi-Fi is up

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);           // steadier throughput for the 100 kB pull
  WiFi.setAutoReconnect(true);

  refreshAll();
  webBegin();                     // control panel on http://<ip>
}

void loop() {
  webLoop();

  if ((uint32_t)(millis() - g_lastAttemptMs) >= g_nextDelayMs) {
    refreshAll();                 // millis() rollover-safe (unsigned subtraction)
  }

  // Short delay so the web server stays responsive. Note that the strip is
  // unreachable for the ~10-30 s an hourly refresh takes -- loop() is blocked
  // for the duration of the fetch.
  delay(2);
}
