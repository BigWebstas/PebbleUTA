#include "departures.h"

static Departure s_deps[MAX_DEPARTURES];
static uint8_t s_received;
static uint8_t s_expected;

void departures_clear(void) {
  memset(s_deps, 0, sizeof(s_deps));
  s_received = 0;
  s_expected = 0;
}

void departures_set_expected(uint8_t expected) {
  s_expected = expected > MAX_DEPARTURES ? MAX_DEPARTURES : expected;
}

uint8_t departures_expected(void) {
  return s_expected;
}

void departures_put(uint8_t idx, const Departure *d) {
  if (idx >= MAX_DEPARTURES) {
    return;
  }
  s_deps[idx] = *d;
  if (idx + 1 > s_received) {
    s_received = idx + 1;
  }
}

uint8_t departures_received(void) {
  return s_received;
}

bool departures_complete(void) {
  return s_expected > 0 && s_received >= s_expected;
}

uint8_t departures_section_count(uint8_t section) {
  uint8_t n = 0;
  for (uint8_t i = 0; i < s_received; i++) {
    if (s_deps[i].section == section) {
      n++;
    }
  }
  return n;
}

const Departure *departures_section_row(uint8_t section, uint8_t row) {
  uint8_t n = 0;
  for (uint8_t i = 0; i < s_received; i++) {
    if (s_deps[i].section != section) {
      continue;
    }
    if (n == row) {
      return &s_deps[i];
    }
    n++;
  }
  return NULL;
}

const Departure *departures_at(uint8_t i) {
  return i < s_received ? &s_deps[i] : NULL;
}
