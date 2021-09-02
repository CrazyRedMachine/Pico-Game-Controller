/*
 * Pico Game Controller
 * @author SpeedyPotato
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bsp/board.h"
#include "encoders.pio.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "hardware/pio.h"
#include "pico/stdlib.h"
#include "tusb.h"
#include "usb_descriptors.h"

#define SW_COUNT 11  // Number of switches
#define ENC_COUNT 2  // Number of encoders
#define ENC_PPR 24   // Encoder PPR

#define ENC_PULSE (ENC_PPR * 4)       // 4 pulses per PPR
#define ENC_ROLLOVER (ENC_PULSE * 2)  // Delta Rollover threshold
#define REACTIVE_TIMEOUT_MAX 100000  // Cycles before HID falls back to reactive
#define ENC_DEBOUNCE_CYCLES 10000    // Number of consistent cycles needed

// MODIFY KEYBINDS HERE, MAKE SURE LENGTHS MATCH SW_GPIO_SIZE
const uint8_t SW_KEYCODE[] = {HID_KEY_D, HID_KEY_F, HID_KEY_J, HID_KEY_K,
                              HID_KEY_C, HID_KEY_M, HID_KEY_A, HID_KEY_B,
                              HID_KEY_1, HID_KEY_E, HID_KEY_G};
const uint8_t SW_GPIO[] = {
    4, 6, 8, 10, 12, 14, 16, 18, 20, 22, 27,
};
const uint8_t LED_GPIO[] = {
    5, 7, 9, 11, 13, 15, 17, 19, 21, 26, 28,
};
const uint8_t ENC_GPIO[] = {0, 2};      // L_ENC(0, 1); R_ENC(2, 3)
const bool ENC_REV[] = {false, false};  // Reverse Encoders

PIO pio;
volatile uint32_t enc_val[ENC_COUNT];
uint32_t prev_enc_val[ENC_COUNT];
int cur_enc_val[ENC_COUNT];
/*
debouncing notes
store the last N values, have to do per cycle i guess
if all are decreasing/increasing, move that direction
otherwise don't move
*/
uint32_t enc_debounce_val[ENC_COUNT][ENC_DEBOUNCE_CYCLES];
uint8_t enc_debounce_idx[ENC_COUNT];
bool enc_changed[ENC_COUNT];

bool sw_val[SW_COUNT];
bool prev_sw_val[SW_COUNT];
bool sw_changed;

bool leds_changed;
unsigned long reactive_timeout_count = REACTIVE_TIMEOUT_MAX;

void (*loop_mode)();

struct lights_report {
  uint8_t buttons[11];
} lights_report;

/**
 * HID/Reactive Lights
 **/
void update_lights() {
  if (reactive_timeout_count < REACTIVE_TIMEOUT_MAX) {
    reactive_timeout_count++;
  }
  if (leds_changed) {
    for (int i = 0; i < SW_COUNT; i++) {
      if (reactive_timeout_count >= REACTIVE_TIMEOUT_MAX) {
        if (sw_val[i]) {
          gpio_put(LED_GPIO[i], 1);
        } else {
          gpio_put(LED_GPIO[i], 0);
        }
      } else {
        if (lights_report.buttons[i] == 0) {
          gpio_put(LED_GPIO[i], 0);
        } else {
          gpio_put(LED_GPIO[i], 1);
        }
      }
    }
    leds_changed = false;
  }
}

struct report {
  uint16_t buttons;
  uint8_t joy0;
  uint8_t joy1;
} report;

/**
 * Gamepad Mode
 **/
void joy_mode() {
  if (tud_hid_ready()) {
    bool send_report = false;
    if (sw_changed) {
      send_report = true;
      uint16_t translate_buttons = 0;
      for (int i = SW_COUNT - 1; i >= 0; i--) {
        translate_buttons = (translate_buttons << 1) | (sw_val[i] ? 1 : 0);
        prev_sw_val[i] = sw_val[i];
      }
      report.buttons = translate_buttons;
      sw_changed = false;
    }

    // find the delta between previous and current enc_val
    for (int i = 0; i < ENC_COUNT; i++) {
      if (enc_changed[i]) {
        send_report = true;
        int delta;
        int changeType;                      // -1 for negative 1 for positive
        if (enc_val[i] > prev_enc_val[i]) {  // if the new value is bigger its
                                             // a positive change
          delta = enc_val[i] - prev_enc_val[i];
          changeType = 1;
        } else {  // otherwise its a negative change
          delta = prev_enc_val[i] - enc_val[i];
          changeType = -1;
        }
        // Overflow / Underflow
        if (delta > ENC_ROLLOVER) {
          // Reverse the change type due to overflow / underflow
          changeType *= -1;
          delta = UINT32_MAX - delta + 1;  // this should give us how much we
                                           // overflowed / underflowed by
        }

        cur_enc_val[i] =
            cur_enc_val[i] + ((ENC_REV[i] ? 1 : -1) * delta * changeType);
        while (cur_enc_val[i] < 0) {
          cur_enc_val[i] = ENC_PULSE - cur_enc_val[i];
        }

        prev_enc_val[i] = enc_val[i];
      }

      report.joy0 = ((double)cur_enc_val[0] / ENC_PULSE) * 256;
      report.joy1 = ((double)cur_enc_val[1] / ENC_PULSE) * 256;
      enc_changed[i] = false;
    }

    if (send_report) {
      tud_hid_n_report(0x00, REPORT_ID_JOYSTICK, &report, sizeof(report));
    }
  }
}

/**
 * Keyboard Mode
 **/
void key_mode() {
  if (tud_hid_ready()) {
    /*------------- Keyboard -------------*/
    if (sw_changed) {
      uint8_t nkro_report[32] = {0};
      for (int i = 0; i < SW_COUNT; i++) {
        if (sw_val[i]) {
          uint8_t bit = SW_KEYCODE[i] % 8;
          uint8_t byte = (SW_KEYCODE[i] / 8) + 1;
          if (SW_KEYCODE[i] >= 240 && SW_KEYCODE[i] <= 247) {
            nkro_report[0] |= (1 << bit);
          } else if (byte > 0 && byte <= 31) {
            nkro_report[byte] |= (1 << bit);
          }

          prev_sw_val[i] = sw_val[i];
        }
      }
      // Send key report
      tud_hid_n_report(0x00, REPORT_ID_KEYBOARD, &nkro_report,
                       sizeof(nkro_report));
      sw_changed = false;
    }

    /*------------- Mouse -------------*/
    bool should_send_mouse = false;
    // find the delta between previous and current enc_val
    int delta[ENC_COUNT] = {0};
    for (int i = 0; i < ENC_COUNT; i++) {
      if (enc_changed[i]) {
        should_send_mouse = true;
        for (int i = 0; i < ENC_COUNT; i++) {
          int changeType;                      // -1 for negative 1 for positive
          if (enc_val[i] > prev_enc_val[i]) {  // if the new value is bigger its
                                               // a positive change
            delta[i] = enc_val[i] - prev_enc_val[i];
            changeType = 1;
          } else {  // otherwise its a negative change
            delta[i] = prev_enc_val[i] - enc_val[i];
            changeType = -1;
          }
          // Overflow / Underflow
          if (delta[i] > ENC_ROLLOVER) {
            // Reverse the change type due to overflow / underflow
            changeType *= -1;
            delta[i] =
                UINT32_MAX - delta[i] + 1;  // this should give us how much we
                                            // overflowed / underflowed by
          }
          delta[i] *= changeType * (ENC_REV[i] ? 1 : -1);  // set direction
          prev_enc_val[i] = enc_val[i];
        }
        enc_changed[0] = false;
      }
    }
    if (should_send_mouse) {
      // Delay if needed before attempt to send mouse report
      while (!tud_hid_ready()) {
        board_delay(1);
      }
      tud_hid_mouse_report(REPORT_ID_MOUSE, 0x00, delta[0], delta[1], 0, 0);
    }
  }
}

/**
 * Update Input States
 **/
void update_inputs() {
  // Switch Update & Flag
  for (int i = 0; i < SW_COUNT; i++) {
    if (gpio_get(SW_GPIO[i])) {
      sw_val[i] = false;
    } else {
      sw_val[i] = true;
    }
    if (!sw_changed && sw_val[i] != prev_sw_val[i]) {
      sw_changed = true;
    }
  }

  // Update LEDs if input changed while in reactive mode
  if (sw_changed && reactive_timeout_count >= REACTIVE_TIMEOUT_MAX)
    leds_changed = true;

  // Encoder Flag
  for (int i = 0; i < ENC_COUNT; i++) {
    enc_debounce_val[i][enc_debounce_idx[i]] = enc_val[i];
    enc_debounce_idx[i] = ++enc_debounce_idx[i] % ENC_DEBOUNCE_CYCLES;
    if (enc_val[i] !=
        enc_debounce_val[i][(enc_debounce_idx[i] + ENC_DEBOUNCE_CYCLES + 1) %
                            ENC_DEBOUNCE_CYCLES]) {
      enc_changed[i] = true;
      int dir = enc_debounce_val[i][0] - enc_debounce_val[i][1];
      for (int j = 1; j < ENC_DEBOUNCE_CYCLES - 1; j++) {
        if (dir > 0 &&
                enc_debounce_val[i][j] - enc_debounce_val[i][j + 1] < 0 ||
            dir < 0 &&
                enc_debounce_val[i][j] - enc_debounce_val[i][j + 1] > 0) {
          enc_changed[i] = false;
          break;
        }
      }
    }
  }
}

/**
 * DMA Encoder Logic For 2 Encoders
 **/
void dma_handler() {
  uint i = 1;
  int interrupt_channel = 0;
  while ((i & dma_hw->ints0) == 0) {
    i = i << 1;
    ++interrupt_channel;
  }
  dma_hw->ints0 = 1u << interrupt_channel;
  if (interrupt_channel < 4) {
    dma_channel_set_read_addr(interrupt_channel, &pio->rxf[interrupt_channel],
                              true);
  }
}

/**
 * Initialize Board Pins
 **/
void init() {
  // LED Pin on when connected
  gpio_init(25);
  gpio_set_dir(25, GPIO_OUT);
  gpio_put(25, 1);

  // Set up the state machine for encoders
  pio = pio0;
  uint offset = pio_add_program(pio, &encoders_program);
  // Setup Encoders
  for (int i = 0; i < ENC_COUNT; i++) {
    enc_val[i] = 0;
    prev_enc_val[i] = 0;
    cur_enc_val[i] = 0;
    encoders_program_init(pio, i, offset, ENC_GPIO[i]);

    dma_channel_config c = dma_channel_get_default_config(i);
    channel_config_set_read_increment(&c, false);
    channel_config_set_write_increment(&c, false);
    channel_config_set_dreq(&c, pio_get_dreq(pio, i, false));

    dma_channel_configure(i, &c,
                          &enc_val[i],   // Destinatinon pointer
                          &pio->rxf[i],  // Source pointer
                          0x10,          // Number of transfers
                          true           // Start immediately
    );
    irq_set_exclusive_handler(DMA_IRQ_0, dma_handler);
    irq_set_enabled(DMA_IRQ_0, true);
    dma_channel_set_irq0_enabled(i, true);

    enc_debounce_idx[i] = 0;
    for (int j = 0; j < ENC_DEBOUNCE_CYCLES; j++) {
      enc_debounce_val[i][j] = 0;
    }
    enc_changed[i] = false;
  }

  // Setup Button GPIO
  for (int i = 0; i < SW_COUNT; i++) {
    sw_val[i] = false;
    prev_sw_val[i] = false;
    gpio_init(SW_GPIO[i]);
    gpio_set_function(SW_GPIO[i], GPIO_FUNC_SIO);
    gpio_set_dir(SW_GPIO[i], GPIO_IN);
    gpio_pull_up(SW_GPIO[i]);
  }
  sw_changed = false;

  // Setup LED GPIO
  for (int i = 0; i < SW_COUNT; i++) {
    gpio_init(LED_GPIO[i]);
    gpio_set_dir(LED_GPIO[i], GPIO_OUT);
  }
  leds_changed = false;

  // Joy/KB Mode Switching
  if (gpio_get(SW_GPIO[0])) {
    loop_mode = &joy_mode;
  } else {
    loop_mode = &key_mode;
  }
}

/**
 * Main Loop Function
 **/
int main(void) {
  board_init();
  tusb_init();
  init();

  while (1) {
    tud_task();  // tinyusb device task
    update_inputs();
    loop_mode();
    update_lights();
  }

  return 0;
}

// Invoked when received GET_REPORT control request
// Application must fill buffer report's content and return its length.
// Return zero will cause the stack to STALL request
uint16_t tud_hid_get_report_cb(uint8_t itf, uint8_t report_id,
                               hid_report_type_t report_type, uint8_t* buffer,
                               uint16_t reqlen) {
  // TODO not Implemented
  (void)itf;
  (void)report_id;
  (void)report_type;
  (void)buffer;
  (void)reqlen;

  return 0;
}

// Invoked when received SET_REPORT control request or
// received data on OUT endpoint ( Report ID = 0, Type = 0 )
void tud_hid_set_report_cb(uint8_t itf, uint8_t report_id,
                           hid_report_type_t report_type, uint8_t const* buffer,
                           uint16_t bufsize) {
  (void)itf;
  if (report_id == 2 && report_type == HID_REPORT_TYPE_OUTPUT &&
      buffer[0] == 2 && bufsize >= sizeof(lights_report))  // light data
  {
    size_t i = 0;
    for (i; i < sizeof(lights_report); i++) {
      lights_report.buttons[i] = buffer[i + 1];
    }
    reactive_timeout_count = 0;
    leds_changed = true;
  }
}
