/**
 * board.h -- the small surface that the web UI needs from the LED board.
 *
 * Implemented in main.cpp. Kept deliberately narrow so web.cpp never touches
 * the entry table, FastLED, or NVS directly.
 */
#pragma once

#include <Arduino.h>

/** What the strip is currently showing. Persisted in NVS. */
enum ViewMode : uint8_t {
  VIEW_ON   = 0,   // every mapped office, coloured by its status
  VIEW_OFF  = 1,   // all LEDs dark
  VIEW_BUSY = 2,   // only system_status 4 (sold)
  VIEW_FREE = 3    // only system_status 1 (free)
};

static const uint8_t STATUS_FREE = 1;
static const uint8_t STATUS_SOLD = 4;

ViewMode    boardGetViewMode();
/** Applies the mode, repaints the strip and persists it. */
void        boardSetViewMode(ViewMode mode);
const char *boardViewModeName(ViewMode mode);
/** Parses "on"/"off"/"busy"/"free" (case-insensitive). false if unrecognised. */
bool        boardParseViewMode(const char *name, ViewMode &out);

size_t   boardEntryCount();                    // mapped offices
size_t   boardCountWithStatus(uint8_t status);
size_t   boardCountKnown();                    // offices with a status != 0
/** millis() of the last fully successful data fetch, or 0 if never. */
uint32_t boardLastFetchMs();
