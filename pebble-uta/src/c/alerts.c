#include "alerts.h"

static Alert s_alerts[MAX_ALERTS];
static uint8_t s_received;
static uint8_t s_expected;

void alerts_clear(void) {
  memset(s_alerts, 0, sizeof(s_alerts));
  s_received = 0;
  s_expected = 0;
}

void alerts_set_expected(uint8_t n) {
  s_expected = n > MAX_ALERTS ? MAX_ALERTS : n;
}

uint8_t alerts_expected(void) {
  return s_expected;
}

void alerts_put(uint8_t idx, const char *head, const char *body) {
  if (idx >= MAX_ALERTS) {
    return;
  }
  strncpy(s_alerts[idx].head, head ? head : "", ALERT_HEAD_LEN - 1);
  s_alerts[idx].head[ALERT_HEAD_LEN - 1] = '\0';
  strncpy(s_alerts[idx].body, body ? body : "", ALERT_BODY_LEN - 1);
  s_alerts[idx].body[ALERT_BODY_LEN - 1] = '\0';
  if (idx + 1 > s_received) {
    s_received = idx + 1;
  }
}

uint8_t alerts_count(void) {
  return s_received;
}

const Alert *alerts_get(uint8_t idx) {
  return idx < s_received ? &s_alerts[idx] : NULL;
}
