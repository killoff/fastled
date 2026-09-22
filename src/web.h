/**
 * web.h -- tiny LAN control panel for the LED board.
 *
 * Serves one page with three status toggles (ВІЛЬНО / ПРОДАНО / РЕЗЕРВ), a
 * STANDBY toggle, and a JSON status endpoint. No auth: anyone on the same
 * Wi-Fi can press the buttons.
 */
#pragma once

#include <Arduino.h>

/** Starts the HTTP server (and mDNS). Safe to call before Wi-Fi is up. */
void webBegin();
/** Pump the server. Call from loop() as often as possible. */
void webLoop();
