#pragma once
#include <pebble.h>

#define MAX_DEPARTURES 15

#define DEP_ROUTE_LEN 16
#define DEP_HEADSIGN_LEN 40
#define DEP_STOPNAME_LEN 40
#define DEP_STOPID_LEN 44
#define DEP_COLOR_LEN 8
#define DEP_TRIPID_LEN 24

#define DEP_FLAG_REALTIME   (1 << 0)
#define DEP_FLAG_FAVORITE   (1 << 1)
#define DEP_FLAG_ALERT      (1 << 2)
#define DEP_FLAG_WHEELCHAIR (1 << 3)

#define SECTION_NEARBY 0
#define SECTION_FAVORITE 1
#define SECTION_COUNT 2

typedef struct {
  char route[DEP_ROUTE_LEN];
  char headsign[DEP_HEADSIGN_LEN];
  char stop_name[DEP_STOPNAME_LEN];
  char stop_id[DEP_STOPID_LEN];
  char trip_id[DEP_TRIPID_LEN];
  char color[DEP_COLOR_LEN];  // GTFS route_color hex, no '#', "" if unknown
  int32_t dep_epoch;          // unix departure time; 0 if unknown -> use minutes
  int16_t minutes;            // minutes at fetch time (fallback)
  int16_t walk_min;           // minutes on foot to the stop, <0 if unknown
  int16_t delay_min;          // + late / - early, -999 if unknown
  uint8_t flags;
  uint8_t section;
} Departure;

// Reset the buffer for a fresh batch.
void departures_clear(void);

// Total rows the phone says it will send this batch.
void departures_set_expected(uint8_t expected);
uint8_t departures_expected(void);

// Store one row at its batch index.
void departures_put(uint8_t idx, const Departure *d);

// How many rows have actually arrived.
uint8_t departures_received(void);

// True once every expected row has arrived.
bool departures_complete(void);

// Rows filtered to one section (SECTION_NEARBY / SECTION_FAVORITE).
uint8_t departures_section_count(uint8_t section);
const Departure *departures_section_row(uint8_t section, uint8_t row);

// Raw buffer access, ignoring sections (for the offline cache).
const Departure *departures_at(uint8_t i);
