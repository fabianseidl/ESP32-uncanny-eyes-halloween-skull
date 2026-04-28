#include <Arduino.h>

#include "config.h"
#include "eye_sync.h"

#if EYE_SYNC_ENABLE

#ifndef EYE_SYNC_LOG
#define EYE_SYNC_LOG 0
#endif

void eye_sync_init(void) {
#if EYE_SYNC_LOG
  Serial.println("eye_sync: init ok (stub)");
#endif
}

void eye_sync_tick(void) {
  // Filled in Task 5 (TX) and Task 6 (RX).
}

void eye_sync_broadcast_index(uint8_t idx) {
  (void)idx;
  // Filled in Task 5.
}

#else  // EYE_SYNC_ENABLE == 0 — fallback no-ops, no WiFi code linked.

void eye_sync_init(void)                    {}
void eye_sync_tick(void)                    {}
void eye_sync_broadcast_index(uint8_t idx)  { (void)idx; }

#endif
