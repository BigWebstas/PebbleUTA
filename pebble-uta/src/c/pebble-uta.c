#include <pebble.h>
#include "departures.h"
#include "alerts.h"
#include "detail.h"
#include "cache.h"

#if defined(PBL_COLOR)
// Parse a GTFS route_color ("RRGGBB", no '#'). Returns GColorClear when unset
// or malformed so callers can fall back to a default. Hand-rolled hex: the
// Pebble libc's strtol is unreliable and faulted the app.
static int hex_digit(char c) {
  if (c >= '0' && c <= '9') { return c - '0'; }
  if (c >= 'a' && c <= 'f') { return c - 'a' + 10; }
  if (c >= 'A' && c <= 'F') { return c - 'A' + 10; }
  return -1;
}

// 0..255 -> nearest of {0, 85, 170, 255} (the 2-bit-per-channel display).
// GColorFromRGB truncates (>>6); rounding lands closer to the real colour.
static uint8_t chan2(int c) {
  return (uint8_t)((c * 3 + 127) / 255);
}

static GColor color_from_hex(const char *hex) {
  if (!hex) {
    return GColorClear;
  }
  int v[6];
  for (int i = 0; i < 6; i++) {
    if (hex[i] == '\0') {
      return GColorClear;
    }
    v[i] = hex_digit(hex[i]);
    if (v[i] < 0) {
      return GColorClear;
    }
  }
  GColor8 c;
  c.a = 3;
  c.r = chan2((v[0] << 4) | v[1]);
  c.g = chan2((v[2] << 4) | v[3]);
  c.b = chan2((v[4] << 4) | v[5]);
  return c;
}
#endif

static Window *s_window;
static MenuLayer *s_menu;
static StatusBarLayer *s_status_bar;
static TextLayer *s_fresh;

static char s_status[64] = "Loading...";
static char s_fresh_txt[40] = "";
static bool s_have_data = false;
static time_t s_data_time = 0;   // when the shown rows were fetched
static bool s_offline = false;   // last refresh could not reach the proxy

// Minutes until a departure: live from its epoch when known, else the
// fetch-time value (which goes stale).
static int live_minutes(const Departure *d) {
  if (d->dep_epoch > 0) {
    return (int)((d->dep_epoch - time(NULL)) / 60);
  }
  return d->minutes;
}

static void human_ago(int secs, char *out, size_t len) {
  if (secs < 60) {
    snprintf(out, len, "just now");
  } else if (secs < 3600) {
    snprintf(out, len, "%d min ago", secs / 60);
  } else {
    snprintf(out, len, "%dh ago", secs / 3600);
  }
}

static void update_freshness(void) {
  if (!s_have_data || s_data_time == 0) {
    s_fresh_txt[0] = '\0';
  } else {
    int age = (int)(time(NULL) - s_data_time);
    char ago[16];
    human_ago(age, ago, sizeof(ago));
    if (s_offline) {
      snprintf(s_fresh_txt, sizeof(s_fresh_txt), "offline - %s", ago);
    } else if (age >= 90) {
      snprintf(s_fresh_txt, sizeof(s_fresh_txt), "%s", ago);
    } else {
      s_fresh_txt[0] = '\0';
    }
  }
  if (s_fresh) {
    text_layer_set_text(s_fresh, s_fresh_txt);
  }
}

// ---------------------------------------------------------------------------
// AppMessage
// ---------------------------------------------------------------------------

static void send_simple(uint32_t key, const char *value) {
  DictionaryIterator *iter;
  if (app_message_outbox_begin(&iter) != APP_MSG_OK) {
    return;
  }
  if (value) {
    dict_write_cstring(iter, key, value);
  } else {
    dict_write_uint8(iter, key, 1);
  }
  app_message_outbox_send();
}

static time_t s_last_fetch;

static void request_fetch(void) {
  time_t now = time(NULL);
  if (now - s_last_fetch < 15) {
    return;   // debounce: accel taps and quick clicks must not spam the phone
  }
  s_last_fetch = now;
  snprintf(s_status, sizeof(s_status), "%s", "Loading...");
  s_have_data = false;
  if (s_menu) {
    menu_layer_reload_data(s_menu);
  }
  send_simple(MESSAGE_KEY_FETCH, NULL);
}

static int32_t tuple_int(const Tuple *t) {
  if (!t) {
    return 0;
  }
  switch (t->type) {
    case TUPLE_INT:
    case TUPLE_UINT:
      switch (t->length) {
        case 1: return t->value->int8;
        case 2: return t->value->int16;
        default: return t->value->int32;
      }
    default:
      return 0;
  }
}

static void copy_str(DictionaryIterator *iter, uint32_t key, char *dst, size_t len) {
  Tuple *t = dict_find(iter, key);
  if (t && t->type == TUPLE_CSTRING) {
    strncpy(dst, t->value->cstring, len - 1);
    dst[len - 1] = '\0';
  } else {
    dst[0] = '\0';
  }
}

// A batch refresh streams ~30 AppMessages. Reloading the MenuLayer on each one
// makes the list jump under the user's finger, so coalesce into one reload
// shortly after the last message lands.
static AppTimer *s_reload_timer;

static void do_reload(void *ctx) {
  s_reload_timer = NULL;
  if (s_menu) {
    menu_layer_reload_data(s_menu);
  }
}

static void schedule_reload(void) {
  if (s_reload_timer) {
    app_timer_reschedule(s_reload_timer, 150);
  } else {
    s_reload_timer = app_timer_register(150, do_reload, NULL);
  }
}

static void inbox_received(DictionaryIterator *iter, void *context) {
  // Detail-page text chunks.
  if (dict_find(iter, MESSAGE_KEY_D_TEXT)) {
    detail_handle_message(iter);
    return;
  }

  // Alerts (pinned section). COUNT rides in every message; IDX 0 clears.
  Tuple *aidx_t = dict_find(iter, MESSAGE_KEY_A_IDX);
  if (aidx_t) {
    uint8_t aidx = (uint8_t)tuple_int(aidx_t);
    if (aidx == 0) {
      alerts_clear();
    }
    Tuple *acount_t = dict_find(iter, MESSAGE_KEY_A_COUNT);
    if (acount_t) {
      alerts_set_expected((uint8_t)tuple_int(acount_t));
    }
    Tuple *head_t = dict_find(iter, MESSAGE_KEY_A_HEAD);
    if (head_t) {
      Tuple *body_t = dict_find(iter, MESSAGE_KEY_A_BODY);
      alerts_put(aidx, head_t->value->cstring,
                 body_t ? body_t->value->cstring : "");
    }
    APP_LOG(APP_LOG_LEVEL_DEBUG, "alert msg idx=%d count=%d have=%d",
            aidx, alerts_expected(), alerts_count());
    schedule_reload();
    return;
  }

  Tuple *status_t = dict_find(iter, MESSAGE_KEY_STATUS);
  if (status_t && status_t->type == TUPLE_CSTRING) {
    snprintf(s_status, sizeof(s_status), "%s", status_t->value->cstring);
    // Keep any existing rows on screen during a refresh; the next batch
    // replaces them atomically when its IDX 0 arrives.
    if (departures_received() == 0) {
      s_have_data = false;
    }
    if (strstr(s_status, "unreachable") || strstr(s_status, "timeout")) {
      s_offline = true;   // keep the cached rows, just mark them stale
    }
    update_freshness();
    schedule_reload();
    return;
  }

  Tuple *idx_t = dict_find(iter, MESSAGE_KEY_IDX);
  Tuple *count_t = dict_find(iter, MESSAGE_KEY_COUNT);
  if (!idx_t || !count_t) {
    return;
  }

  uint8_t idx = (uint8_t)tuple_int(idx_t);
  uint8_t count = (uint8_t)tuple_int(count_t);
  if (idx == 0) {
    departures_clear();
  }
  departures_set_expected(count);

  Departure d;
  memset(&d, 0, sizeof(d));
  copy_str(iter, MESSAGE_KEY_ROUTE, d.route, sizeof(d.route));
  copy_str(iter, MESSAGE_KEY_HEADSIGN, d.headsign, sizeof(d.headsign));
  copy_str(iter, MESSAGE_KEY_STOP_NAME, d.stop_name, sizeof(d.stop_name));
  copy_str(iter, MESSAGE_KEY_STOP_ID, d.stop_id, sizeof(d.stop_id));
  copy_str(iter, MESSAGE_KEY_TRIP_ID, d.trip_id, sizeof(d.trip_id));
  copy_str(iter, MESSAGE_KEY_COLOR, d.color, sizeof(d.color));
  d.dep_epoch = tuple_int(dict_find(iter, MESSAGE_KEY_DEP));
  d.minutes = (int16_t)tuple_int(dict_find(iter, MESSAGE_KEY_MINUTES));
  d.walk_min = (int16_t)tuple_int(dict_find(iter, MESSAGE_KEY_WALK));
  d.delay_min = (int16_t)tuple_int(dict_find(iter, MESSAGE_KEY_DELAY));
  d.flags = (uint8_t)tuple_int(dict_find(iter, MESSAGE_KEY_FLAGS));
  d.section = (uint8_t)tuple_int(dict_find(iter, MESSAGE_KEY_SECTION));

  departures_put(idx, &d);
  s_have_data = true;

  if (departures_complete()) {
    s_data_time = time(NULL);
    s_offline = false;
    cache_save();
    update_freshness();
  }
  schedule_reload();
}

static void inbox_dropped(AppMessageResult reason, void *context) {
  APP_LOG(APP_LOG_LEVEL_WARNING, "inbox dropped: %d", reason);
}

// ---------------------------------------------------------------------------
// Menu
// ---------------------------------------------------------------------------

enum { KIND_ALERTS, KIND_NEARBY, KIND_FAVORITE };

static uint8_t fav_count(void) {
  return departures_section_count(SECTION_FAVORITE);
}

// Section order: Alerts (if any), Favorites (if any), Nearby (always).
static int section_kind(uint16_t s) {
  uint16_t i = 0;
  if (alerts_count() > 0) {
    if (s == i) { return KIND_ALERTS; }
    i++;
  }
  if (fav_count() > 0) {
    if (s == i) { return KIND_FAVORITE; }
    i++;
  }
  return KIND_NEARBY;
}

static uint8_t dep_section_for(int kind) {
  return kind == KIND_FAVORITE ? SECTION_FAVORITE : SECTION_NEARBY;
}

static uint16_t menu_get_num_sections(MenuLayer *menu, void *ctx) {
  return 1 + (alerts_count() > 0 ? 1 : 0) + (fav_count() > 0 ? 1 : 0);
}

static uint16_t menu_get_num_rows(MenuLayer *menu, uint16_t section, void *ctx) {
  int kind = section_kind(section);
  if (kind == KIND_ALERTS) {
    return alerts_count();
  }
  if (kind == KIND_FAVORITE) {
    return fav_count();   // section only exists when > 0
  }
  uint8_t n = departures_section_count(SECTION_NEARBY);
  return n == 0 ? 1 : n;
}

static int16_t menu_get_header_height(MenuLayer *menu, uint16_t section, void *ctx) {
  return MENU_CELL_BASIC_HEADER_HEIGHT;
}

static int16_t menu_get_cell_height(MenuLayer *menu, MenuIndex *idx, void *ctx) {
  if (section_kind(idx->section) == KIND_ALERTS) {
    return 54;
  }
  return PBL_IF_ROUND_ELSE(64, 62);
}

static void menu_draw_header(GContext *gctx, const Layer *cell, uint16_t section, void *ctx) {
  const char *t = "Nearby";
  switch (section_kind(section)) {
    case KIND_ALERTS: t = "Alerts"; break;
    case KIND_FAVORITE: t = "Favorites"; break;
    default: t = "Nearby"; break;
  }
  menu_cell_basic_header_draw(gctx, cell, t);
}

static void menu_draw_row(GContext *gctx, const Layer *cell, MenuIndex *idx, void *ctx) {
  int kind = section_kind(idx->section);

  if (kind == KIND_ALERTS) {
    const Alert *a = alerts_get((uint8_t)idx->row);
    if (a) {
      menu_cell_basic_draw(gctx, cell, a->head, a->body, NULL);
    }
    return;
  }

  uint8_t section = dep_section_for(kind);
  uint8_t n = departures_section_count(section);

  if (n == 0) {
    const char *msg;
    if (!s_have_data) {
      msg = s_status;
    } else if (section == SECTION_NEARBY) {
      msg = "No departures nearby";
    } else {
      msg = "Hold a row to pin a stop";
    }
    menu_cell_basic_draw(gctx, cell, msg, NULL, NULL);
    return;
  }

  const Departure *d = departures_section_row(section, (uint8_t)idx->row);
  if (!d) {
    return;
  }

  GRect b = layer_get_bounds(cell);

  // Route-colour bar down the left edge.
  GColor bar;
#if defined(PBL_COLOR)
  bar = color_from_hex(d->color);
  if (gcolor_equal(bar, GColorClear)) {
    bar = GColorLightGray;
  }
#else
  bar = GColorBlack;
#endif
  graphics_context_set_fill_color(gctx, bar);
  graphics_fill_rect(gctx, GRect(b.origin.x, b.origin.y, 6, b.size.h), 0, GCornerNone);

  char title[DEP_ROUTE_LEN + DEP_HEADSIGN_LEN + 8];
  snprintf(title, sizeof(title), "%s%s  %s",
           (d->flags & DEP_FLAG_ALERT) ? "! " : "", d->route, d->headsign);

  const char *live = (d->flags & DEP_FLAG_REALTIME) ? " - live" : "";
  int mins = live_minutes(d);
  char mid[56];
  if (mins <= -2) {
    snprintf(mid, sizeof(mid), "gone%s", live);
  } else if (mins <= 0) {
    snprintf(mid, sizeof(mid), "now%s", live);
  } else {
    snprintf(mid, sizeof(mid), "%d min%s", mins, live);
  }
  if (d->delay_min != -999 && d->delay_min != 0) {
    size_t len = strlen(mid);
    if (d->delay_min > 0) {
      snprintf(mid + len, sizeof(mid) - len, "  +%d late", d->delay_min);
    } else {
      snprintf(mid + len, sizeof(mid) - len, "  %d early", -d->delay_min);
    }
  }
  if (d->walk_min > 0) {
    size_t len = strlen(mid);
    snprintf(mid + len, sizeof(mid) - len, "   walk %d min", d->walk_min);
  }

  const int16_t x = b.origin.x + 9;
  const int16_t w = b.size.w - 9 - 4;
  const GTextAlignment al = PBL_IF_ROUND_ELSE(GTextAlignmentCenter, GTextAlignmentLeft);

  graphics_draw_text(gctx, title, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
                     GRect(x, b.origin.y + 1, w, 20),
                     GTextOverflowModeTrailingEllipsis, al, NULL);
  graphics_draw_text(gctx, mid, fonts_get_system_font(FONT_KEY_GOTHIC_18),
                     GRect(x, b.origin.y + 21, w, 20),
                     GTextOverflowModeTrailingEllipsis, al, NULL);

  char sname[DEP_STOPNAME_LEN + 4];
  snprintf(sname, sizeof(sname), "%s%s",
           (d->flags & DEP_FLAG_FAVORITE) ? "* " : "", d->stop_name);
  graphics_draw_text(gctx, sname, fonts_get_system_font(FONT_KEY_GOTHIC_14),
                     GRect(x, b.origin.y + 41, w, 18),
                     GTextOverflowModeTrailingEllipsis, al, NULL);
}

static void menu_select_long(MenuLayer *menu, MenuIndex *idx, void *ctx) {
  int kind = section_kind(idx->section);
  if (kind == KIND_ALERTS) {
    return;
  }
  const Departure *cd =
      departures_section_row(dep_section_for(kind), (uint8_t)idx->row);
  if (!cd || cd->stop_id[0] == '\0') {
    return;
  }
  Departure *d = (Departure *)cd;   // points into the static departures buffer
  bool was_fav = (d->flags & DEP_FLAG_FAVORITE) != 0;

  vibes_short_pulse();

  DictionaryIterator *iter;
  if (app_message_outbox_begin(&iter) != APP_MSG_OK) {
    APP_LOG(APP_LOG_LEVEL_WARNING, "fav: outbox busy");
    return;
  }
  dict_write_cstring(iter, MESSAGE_KEY_FAV_ROUTE, d->route);
  if (was_fav) {
    dict_write_cstring(iter, MESSAGE_KEY_DEL_FAV, d->stop_id);
  } else {
    dict_write_cstring(iter, MESSAGE_KEY_SAVE_FAV, d->stop_id);
    dict_write_cstring(iter, MESSAGE_KEY_FAV_NAME, d->stop_name);
  }
  app_message_outbox_send();
  APP_LOG(APP_LOG_LEVEL_DEBUG, "fav: %s %s/%s", was_fav ? "remove" : "add",
          d->stop_id, d->route);

  // Optimistic marker flip; the next phone reply is authoritative.
  d->flags ^= DEP_FLAG_FAVORITE;
  menu_layer_reload_data(s_menu);
}

static void menu_select(MenuLayer *menu, MenuIndex *idx, void *ctx) {
  int kind = section_kind(idx->section);

  if (kind == KIND_ALERTS) {
    const Alert *a = alerts_get((uint8_t)idx->row);
    if (a) {
      detail_push_alert(a->head, a->body);
    }
    return;
  }

  const Departure *d =
      departures_section_row(dep_section_for(kind), (uint8_t)idx->row);
  if (d && d->trip_id[0] != '\0') {
    GColor rc;
#if defined(PBL_COLOR)
    rc = color_from_hex(d->color);
#else
    rc = GColorClear;
#endif
    detail_push_trip(d->trip_id, d->stop_id, d->route, d->stop_name,
                     (d->flags & DEP_FLAG_FAVORITE) != 0, rc);
  } else {
    request_fetch();
  }
}

// ---------------------------------------------------------------------------
// Window / lifecycle
// ---------------------------------------------------------------------------

static void accel_tap(AccelAxisType axis, int32_t direction) {
  if (detail_is_open()) {
    return;
  }
  request_fetch();
}

static void window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(root);

  s_status_bar = status_bar_layer_create();
  status_bar_layer_set_colors(s_status_bar, GColorWhite, GColorBlack);
  layer_add_child(root, status_bar_layer_get_layer(s_status_bar));

  // Freshness strip: shows "3 min ago" / "offline - ..." when data is stale.
  const int16_t strip = 14;
  const int16_t top = bounds.origin.y + STATUS_BAR_LAYER_HEIGHT;
  s_fresh = text_layer_create(GRect(bounds.origin.x, top, bounds.size.w, strip));
  text_layer_set_font(s_fresh, fonts_get_system_font(FONT_KEY_GOTHIC_14));
  text_layer_set_text_alignment(s_fresh, GTextAlignmentCenter);
  text_layer_set_text(s_fresh, s_fresh_txt);
#if defined(PBL_COLOR)
  text_layer_set_text_color(s_fresh, GColorDarkGray);
#endif
  layer_add_child(root, text_layer_get_layer(s_fresh));

  GRect menu_frame = GRect(bounds.origin.x,
                           top + strip,
                           bounds.size.w,
                           bounds.size.h - STATUS_BAR_LAYER_HEIGHT - strip);
  s_menu = menu_layer_create(menu_frame);
  menu_layer_set_click_config_onto_window(s_menu, window);
  menu_layer_set_callbacks(s_menu, NULL, (MenuLayerCallbacks) {
    .get_num_sections = menu_get_num_sections,
    .get_num_rows = menu_get_num_rows,
    .get_header_height = menu_get_header_height,
    .get_cell_height = menu_get_cell_height,
    .draw_header = menu_draw_header,
    .draw_row = menu_draw_row,
    .select_click = menu_select,
    .select_long_click = menu_select_long,
  });
#if defined(PBL_COLOR)
  menu_layer_set_normal_colors(s_menu, GColorWhite, GColorBlack);
  menu_layer_set_highlight_colors(s_menu, GColorBlue, GColorWhite);
#endif
  layer_add_child(root, menu_layer_get_layer(s_menu));
}

static void window_unload(Window *window) {
  if (s_reload_timer) {
    app_timer_cancel(s_reload_timer);
    s_reload_timer = NULL;
  }
  menu_layer_destroy(s_menu);
  s_menu = NULL;
  text_layer_destroy(s_fresh);
  s_fresh = NULL;
  status_bar_layer_destroy(s_status_bar);
}

// Every minute: countdown values drift, so redraw, and refresh the strip.
static void tick_handler(struct tm *t, TimeUnits units) {
  update_freshness();
  schedule_reload();
}

static void init(void) {
  departures_clear();

  // Paint last-known rows instantly; a live refresh follows from pkjs.
  int age = cache_load();
  if (age >= 0) {
    s_have_data = true;
    s_data_time = time(NULL) - age;
    s_offline = true;   // until the first fresh batch confirms otherwise
    snprintf(s_status, sizeof(s_status), "%s", "Refreshing...");
  }
  update_freshness();

  app_message_register_inbox_received(inbox_received);
  app_message_register_inbox_dropped(inbox_dropped);
  app_message_open(4096, 512);

  accel_tap_service_subscribe(accel_tap);
  tick_timer_service_subscribe(MINUTE_UNIT, tick_handler);
  detail_init();

  // Emery has a touchscreen. Opt into the system touch-navigation bridge so a
  // swipe scrolls the list (and the detail page), a tap acts as SELECT (opens
  // the row's detail page / refreshes), and a swipe-right goes back. No-op on
  // button-only watches, where the SDK stubs it to (0), so the (void) cast
  // keeps the compiler quiet. Buttons keep working everywhere.
  (void) app_touch_navigation_enable(true);

  s_window = window_create();
  window_set_window_handlers(s_window, (WindowHandlers) {
    .load = window_load,
    .unload = window_unload,
  });
  window_stack_push(s_window, true);
}

static void deinit(void) {
  tick_timer_service_unsubscribe();
  accel_tap_service_unsubscribe();
  detail_deinit();
  window_destroy(s_window);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}
