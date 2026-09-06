#pragma once
#include <pebble.h>

#define MAX_ALERTS 15
#define ALERT_HEAD_LEN 100
#define ALERT_BODY_LEN 512

typedef struct {
  char head[ALERT_HEAD_LEN];
  char body[ALERT_BODY_LEN];
} Alert;

void alerts_clear(void);
void alerts_set_expected(uint8_t n);
uint8_t alerts_expected(void);
void alerts_put(uint8_t idx, const char *head, const char *body);
uint8_t alerts_count(void);
const Alert *alerts_get(uint8_t idx);
