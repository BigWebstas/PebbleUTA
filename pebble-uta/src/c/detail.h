#pragma once
#include <pebble.h>

void detail_init(void);
void detail_deinit(void);

// Open the scrolling detail page for one departure and poll the proxy while up.
// SELECT on the page pins / unpins this (stop, route). route_color tints the
// page's route header (pass GColorClear for the default).
void detail_push_trip(const char *trip_id, const char *stop_id, const char *route,
                      const char *stop_name, bool is_fav, GColor route_color);

// Open the detail page showing a single service alert (no polling).
void detail_push_alert(const char *head, const char *body);

bool detail_is_open(void);

// Feed D_SEQ / D_LAST / D_TEXT messages here from the app's inbox handler.
void detail_handle_message(DictionaryIterator *iter);
