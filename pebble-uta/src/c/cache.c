#include "cache.h"
#include "departures.h"

#define CACHE_MAX 12

// Persist keys.
#define K_TIME  200
#define K_COUNT 201
#define K_ROW0  210   // K_ROW0 + i, one Departure each (sizeof < 256)

void cache_save(void) {
  uint8_t n = departures_received();
  if (n > CACHE_MAX) {
    n = CACHE_MAX;
  }
  persist_write_int(K_TIME, (int32_t)time(NULL));
  persist_write_int(K_COUNT, n);
  for (uint8_t i = 0; i < n; i++) {
    const Departure *d = departures_at(i);
    if (d) {
      persist_write_data(K_ROW0 + i, d, sizeof(Departure));
    }
  }
}

int cache_load(void) {
  if (!persist_exists(K_COUNT)) {
    return -1;
  }
  int n = persist_read_int(K_COUNT);
  if (n <= 0) {
    return -1;
  }
  if (n > CACHE_MAX) {
    n = CACHE_MAX;
  }

  departures_clear();
  departures_set_expected((uint8_t)n);
  uint8_t got = 0;
  for (int i = 0; i < n; i++) {
    if (!persist_exists(K_ROW0 + i)) {
      break;
    }
    Departure d;
    persist_read_data(K_ROW0 + i, &d, sizeof(d));
    departures_put((uint8_t)i, &d);
    got++;
  }
  if (got == 0) {
    return -1;
  }

  int age = (int)time(NULL) - persist_read_int(K_TIME);
  return age < 0 ? 0 : age;
}
