#include <FastLED.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <map>
#include <string>

#define SSID "xxx_2GHz_7D6DF7"
#define SSID_PWD "hello world"

#define CONFIG_URI "https://raw.githubusercontent.com/killoff/esp32/refs/heads/main/config.json"
#define DATA_URI "https://raw.githubusercontent.com/killoff/esp32/refs/heads/main/offices.json"
#define MAPPING_URI "https://raw.githubusercontent.com/killoff/esp32/refs/heads/main/mapping.json"
#define NUM_LEDS  50  
#define LED_PIN   5

/**
 * Data structures
 */
struct Config {
  String status_color_free;
  String status_color_repair;
  String status_color_rent;
  String status_color_reserve;
  String status_color_contract;
  String status_color_sold;
  String status_color_unknown;
};

struct Office {
  int system_status;
};

struct Mapping {
  String office_number;
  int led_number;
};


/**
 * Globals
 */
Config config;
std::map<String, Office> offices;
std::vector<Mapping> mapping;


CRGB leds[NUM_LEDS];

String fetchRemoteResource(const char* url);
bool initConfig();
bool initOffices();
bool initMapping();
CRGB hexToCRGB(const String& hex);
CRGB statusToColor(int status);
String statusToColorHex(int status);


String fetchRemoteResource(const char* url) {
  HTTPClient http;
  http.begin(url);

  int httpCode = http.GET();
  if (httpCode == 200) {
    return http.getString();
  } else {
    Serial.printf("HTTP GET failed for %s. Code: %d\n", url, httpCode);
    return "";
  }

  http.end();
}

bool initConfig() {
  String configPayload = fetchRemoteResource(CONFIG_URI);
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, configPayload);
  if (!error) {
    config.status_color_free = doc["status_color_free"].as<String>();
    config.status_color_repair = doc["status_color_repair"].as<String>();
    config.status_color_rent = doc["status_color_rent"].as<String>();
    config.status_color_reserve = doc["status_color_reserve"].as<String>();
    config.status_color_contract = doc["status_color_contract"].as<String>();
    config.status_color_sold = doc["status_color_sold"].as<String>();
    config.status_color_unknown = doc["status_color_unknown"].as<String>();
    return true;
  }
  Serial.println("Failed to parse JSON");
  return false;
}

bool initOffices() {
  String officesPayload = fetchRemoteResource(DATA_URI);
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, officesPayload);
  if (error) {
    Serial.println("Failed to parse JSON from " + String(DATA_URI));
    return false;
  }
  JsonArray data = doc["data"].as<JsonArray>();
  for (JsonObject entry : data) {
    String number = entry["number"];
    int systemStatus = entry["system_status"];
    offices[number] = {systemStatus};
  }
  return true;
}

bool initMapping() {
  String mappingPayload = fetchRemoteResource(MAPPING_URI);
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, mappingPayload);
  if (error) {
    Serial.println("Failed to parse JSON from " + String(MAPPING_URI));
    return false;
  }
    // Clear existing data
  mapping.clear();

  for (JsonPair kv : doc.as<JsonObject>()) {
    Mapping mapEntry;
    mapEntry.office_number = kv.key().c_str();
    mapEntry.led_number = kv.value().as<int>();
    mapping.push_back(mapEntry);
  }
  return true;
}



void setup() {
  // Connect to Wi-Fi
  WiFi.begin(SSID, SSID_PWD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
    // FastLED.addLeds<WS2812, DATA_PIN, RGB>(leds, NUM_LEDS);  // GRB ordering is typical
    // FastLED.addLeds<WS2852, DATA_PIN, RGB>(leds, NUM_LEDS);  // GRB ordering is typical
    // FastLED.addLeds<WS2801, DATA_PIN, CLOCK_PIN, RGB>(leds, NUM_LEDS);
    // FastLED.addLeds<WS2803, DATA_PIN, CLOCK_PIN, RGB>(leds, NUM_LEDS);

  initConfig();
  Serial.println("status_color_free: " + config.status_color_free);
  Serial.println("status_color_repair: " + config.status_color_repair);

  FastLED.addLeds<WS2812B, LED_PIN, GRB>(leds, NUM_LEDS);
  
  FastLED.setBrightness(100); // Set brightness to avoid overpowering

  initOffices();
  initMapping();

  FastLED.clear();


  for (const auto& entry : mapping) {
    auto it = offices.find(entry.office_number);

    if (it != offices.end()) {



      // Key found, access the Office object
      Serial.print("Key: ");
      Serial.print(entry.office_number);
      Serial.print(", System Status: ");
      Serial.println(it->second.system_status);

    // Optional: Display LED index in green for a brief moment
      leds[entry.led_number] = statusToColor(it->second.system_status);
      FastLED.show();
    } else {
      // Key not found
      Serial.print("Office with key ");
      Serial.print(entry.office_number);
      Serial.println(" not found.");
    }
  }


}


void loop() {

//   for (int i = 0; i < NUM_LEDS; i++) {
//     // Turn off all LEDs
//     FastLED.clear();

//     // Light up the current LED in red
//     leds[i] = CRGB::Red;
//     FastLED.show();
//     delay(100); // Pause for visibility

//     // Optional: Display LED index in green for a brief moment
//     leds[i] = CRGB::Green;
//     FastLED.show();
//   }
//   delay(3000);
//   initConfig();


}


CRGB statusToColor(int status) {
  if (status == 1) {
    return hexToCRGB(config.status_color_free);
  } else if (status == 2) {
    return hexToCRGB(config.status_color_reserve);
  } else if (status == 3) {
    return hexToCRGB(config.status_color_contract);
  } else if (status == 4) {
    return hexToCRGB(config.status_color_sold);
  } else if (status == 5) {
    return hexToCRGB(config.status_color_repair);
  } else if (status == 6) {
    return hexToCRGB(config.status_color_rent);
  } else {
    return hexToCRGB(config.status_color_unknown);
  }
}

String statusToColorHex(int status) {
  if (status == 1) {
    return config.status_color_free;
  } else if (status == 2) {
    return config.status_color_reserve;
  } else if (status == 3) {
    return config.status_color_contract;
  } else if (status == 4) {
    return config.status_color_sold;
  } else if (status == 5) {
    return config.status_color_repair;
  } else if (status == 6) {
    return config.status_color_rent;
  } else {
    return config.status_color_unknown;
  }
}

CRGB hexToCRGB(const String& hex) {
  int red = strtol(hex.substring(1, 3).c_str(), nullptr, 16);
  int green = strtol(hex.substring(3, 5).c_str(), nullptr, 16);
  int blue = strtol(hex.substring(5, 7).c_str(), nullptr, 16);
  return CRGB(red, green, blue);
}

