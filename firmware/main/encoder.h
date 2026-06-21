#pragma once

/**
 * encoder.h — Rotary encoder driver
 *
 * Pins:
 *   CLK → GPIO32   DT → GPIO33   SW → GPIO34
 *
 * GPIO34 is input-only on ESP32 — caller must supply a 10kΩ pull-up to 3.3V
 * on the SW pin.  CLK and DT use the ESP32's internal pull-ups.
 */

typedef void (*encoder_rotate_cb_t)(int delta); /* +1 = CW, -1 = CCW */
typedef void (*encoder_click_cb_t)(void);

/**
 * Initialise the encoder.  Both callbacks may be NULL.
 * on_rotate fires once per detent.
 * on_click  fires on SW press (debounced).
 */
void encoder_init(encoder_rotate_cb_t on_rotate, encoder_click_cb_t on_click);
