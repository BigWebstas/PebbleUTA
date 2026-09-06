#pragma once
#include <pebble.h>

// Offline cache: persist the current departure rows so a launch shows
// something instantly and a dropped connection keeps the last data on screen.

void cache_save(void);        // call once a full batch has arrived
int  cache_load(void);        // fill the departures buffer; returns age in
                              // seconds, or -1 if there is no cache
