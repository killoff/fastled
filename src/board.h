/**
 * board.h -- the small surface that the web UI needs from the LED board.
 *
 * Implemented in main.cpp. Kept deliberately narrow so web.cpp never touches
 * the entry table, FastLED, or NVS directly.
 */
#pragma once

#include <Arduino.h>

static const uint8_t STATUS_FREE    = 1;   // ВІЛЬНО
static const uint8_t STATUS_RESERVE = 2;   // РЕЗЕРВ
static const uint8_t STATUS_SOLD    = 4;   // ПРОДАНО

/** Which statuses are drawn: bit N set => offices with system_status N are
 *  lit in their colour. Only STATUS_FREE / STATUS_RESERVE / STATUS_SOLD are
 *  toggleable; every other status is never lit. Persisted in NVS. */
static const uint8_t SHOW_BIT_FREE    = 1u << STATUS_FREE;
static const uint8_t SHOW_BIT_RESERVE = 1u << STATUS_RESERVE;
static const uint8_t SHOW_BIT_SOLD    = 1u << STATUS_SOLD;
static const uint8_t SHOW_MASK_ALL    = SHOW_BIT_FREE | SHOW_BIT_RESERVE | SHOW_BIT_SOLD;

uint8_t boardGetShowMask();
/** Bits outside SHOW_MASK_ALL are ignored. Repaints the strip and persists. */
void    boardSetShowMask(uint8_t mask);
/** true if `status` is one of the three toggleable statuses. */
bool    boardStatusToggleable(uint8_t status);

/** STANDBY: no data is sent to the strip (it is blanked once on entry) and
 *  STANDBY_LED_PIN is driven HIGH. Not persisted -- always off after boot. */
bool    boardGetStandby();
void    boardSetStandby(bool on);

/** "#RRGGBB" currently configured for `status` (1..7), or the fallback colour. */
const char *boardStatusColorHex(uint8_t status);

size_t   boardEntryCount();                    // mapped offices
size_t   boardCountWithStatus(uint8_t status);
size_t   boardCountKnown();                    // offices with a status != 0
/** millis() of the last fully successful data fetch, or 0 if never. */
uint32_t boardLastFetchMs();
