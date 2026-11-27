#pragma once

/**
 * Wi-Fi configuration portal + web dashboard (secondary module).
 * Call webSetup() once from setup() and webLoop() every iteration of loop().
 * If saved credentials can reach their network, it connects as a station.
 * Otherwise it raises the "ESP32-Setup" access point and serves the same
 * dashboard page with the Wi-Fi configuration section active.
 */
void webSetup();
void webLoop();
