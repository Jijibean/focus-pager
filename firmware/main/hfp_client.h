#pragma once

/**
 * hfp_client.h — HFP Hands-Free client (ESP32 = HF device, iPhone = AG)
 *
 * Phase 4: Classic BT HFP for call audio through the pager's
 * speaker (MAX98357A I2S amp) and microphone (INMP441 I2S mic).
 *
 * I2S pin assignments:
 *   BCLK  → GPIO26   (shared between INMP441 and MAX98357A)
 *   WS    → GPIO25   (shared between INMP441 and MAX98357A)
 *   DOUT  → GPIO22   (ESP32 → MAX98357A DIN)
 *   DIN   → GPIO19   (INMP441 SD → ESP32)
 */

/**
 * Initialise Classic BT GAP, HFP HF profile, and I2S audio.
 * Call after esp_bluedroid_enable().
 */
void hfp_client_init(void);

/**
 * Answer an incoming call, or hang up an active call.
 * Safe to call when no call is in progress (no-op).
 * Wired to the short-press button callback in main.c.
 */
void hfp_client_button_press(void);
