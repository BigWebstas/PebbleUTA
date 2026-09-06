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
static bool s_refreshing = false;   // a pull-to-refresh is in flight
static AppTimer *s_refresh_timer;   // safety clear for s_refreshing

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
  bool bar = false;
  if (s_refreshing) {
    // Pull-to-refresh: a highlighted "Refreshing..." bar over the strip.
    snprintf(s_fresh_txt, sizeof(s_fresh_txt), "%s", "Refreshing...");
    bar = true;
  } else if (!s_have_data || s_data_time == 0 || s_offline) {
    // While offline the pinned banner carries the state, so keep the strip clear.
    s_fresh_txt[0] = '\0';
  } else {
    int age = (int)(time(NULL) - s_data_time);
    if (age >= 90) {
      char ago[16];
      human_ago(age, ago, sizeof(ago));
      snprintf(s_fresh_txt, sizeof(s_fresh_txt), "%s", ago);
    } else {
      s_fresh_txt[0] = '\0';
    }
  }
  if (s_fresh) {
    text_layer_set_text(s_fresh, s_fresh_txt);
#if defined(PBL_COLOR)
    text_layer_set_background_color(s_fresh, bar ? GColorPictonBlue : GColorClear);
    text_layer_set_text_color(s_fresh, bar ? GColorBlack : GColorDarkGray);
#else
    text_layer_set_background_color(s_fresh, bar ? GColorBlack : GColorClear);
    text_layer_set_text_color(s_fresh, bar ? GColorWhite : GColorBlack);
#endif
  }
}

static void refresh_ui_end(void);

#if defined(PBL_PLATFORM_EMERY)
static void refresh_timeout(void *ctx) {
  s_refresh_timer = NULL;
  refresh_ui_end();
}

// Show the "Refreshing..." bar (pull-to-refresh only); a safety timer clears
// it if the phone stays quiet.
static void refresh_ui_begin(void) {
  s_refreshing = true;
  update_freshness();
  if (s_refresh_timer) {
    app_timer_reschedule(s_refresh_timer, 12000);
  } else {
    s_refresh_timer = app_timer_register(12000, refresh_timeout, NULL);
  }
}
#endif

static void refresh_ui_end(void) {
  if (!s_refreshing) {
    return;
  }
  s_refreshing = false;
  if (s_refresh_timer) {
    app_timer_cancel(s_refresh_timer);
    s_refresh_timer = NULL;
  }
  update_freshness();
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

// quiet=true keeps the current rows on screen (used for the 5-minute auto
// refresh); quiet=false shows "Loading..." for a user-driven refresh.
static void do_fetch(bool quiet) {
  time_t now = time(NULL);
  if (now - s_last_fetch < 15) {
    return;   // debounce: accel taps and quick clicks must not spam the phone
  }
  s_last_fetch = now;
  if (!quiet) {
    snprintf(s_status, sizeof(s_status), "%s", "Loading...");
    s_have_data = false;
    if (s_menu) {
      menu_layer_reload_data(s_menu);
    }
  }
  send_simple(MESSAGE_KEY_FETCH, NULL);
}

static void request_fetch(void) {
  do_fetch(false);
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

    if (strstr(s_status, "unreachable") || strstr(s_status, "timeout") ||
        strstr(s_status, "starting up")) {
      s_offline = true;   // keep the cached rows on screen, just flag them
      refresh_ui_end();
    } else if (strstr(s_status, "No departures")) {
      // The proxy answered; there is simply nothing to show.
      s_offline = false;
      departures_clear();
      s_data_time = time(NULL);
      cache_save();
      refresh_ui_end();
    }

    // Keep existing rows visible during a refresh; a real batch replaces them
    // atomically on its IDX 0. Only blank when we have nothing at all.
    s_have_data = (departures_received() > 0) || strstr(s_status, "No departures");

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
    s_offline = false;   // a batch is arriving, so the proxy was reached
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

  // Finalise on the last row of the batch even if an earlier one was dropped,
  // so a lost AppMessage can't leave the app stuck "offline".
  if (departures_complete() || idx + 1 >= count) {
    s_data_time = time(NULL);
    s_offline = false;
    cache_save();
    refresh_ui_end();
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

enum { KIND_STATUS, KIND_ALERTS, KIND_NEARBY, KIND_FAVORITE };

static uint8_t fav_count(void) {
  return departures_section_count(SECTION_FAVORITE);
}

// The offline banner is a pinned one-row section at the very top: tap it to
// force a refresh.
static bool status_banner_shown(void) {
  return s_offline;
}

// Section order: Offline banner (if offline), Alerts (if any), Favorites (if
// any), Nearby (always).
static int section_kind(uint16_t s) {
  uint16_t i = 0;
  if (status_banner_shown()) {
    if (s == i) { return KIND_STATUS; }
    i++;
  }
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
  return 1 + (status_banner_shown() ? 1 : 0)
           + (alerts_count() > 0 ? 1 : 0)
           + (fav_count() > 0 ? 1 : 0);
}

static uint16_t menu_get_num_rows(MenuLayer *menu, uint16_t section, void *ctx) {
  int kind = section_kind(section);
  if (kind == KIND_STATUS) {
    return 1;
  }
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
  return section_kind(section) == KIND_STATUS ? 0 : MENU_CELL_BASIC_HEADER_HEIGHT;
}

static int16_t menu_get_cell_height(MenuLayer *menu, MenuIndex *idx, void *ctx) {
  switch (section_kind(idx->section)) {
    case KIND_STATUS: return 46;
    case KIND_ALERTS: return 54;
    default: return PBL_IF_ROUND_ELSE(64, 62);
  }
}

static void menu_draw_header(GContext *gctx, const Layer *cell, uint16_t section, void *ctx) {
  const char *t = "Nearby";
  switch (section_kind(section)) {
    case KIND_STATUS: return;   // no header
    case KIND_ALERTS: t = "Alerts"; break;
    case KIND_FAVORITE: t = "Favorites"; break;
    default: t = "Nearby"; break;
  }
  menu_cell_basic_header_draw(gctx, cell, t);
}

static void menu_draw_row(GContext *gctx, const Layer *cell, MenuIndex *idx, void *ctx) {
  int kind = section_kind(idx->section);

  if (kind == KIND_STATUS) {
    GRect b = layer_get_bounds(cell);
    graphics_context_set_fill_color(
        gctx, PBL_IF_COLOR_ELSE(GColorDarkCandyAppleRed, GColorBlack));
    graphics_fill_rect(gctx, b, 0, GCornerNone);
    graphics_context_set_text_color(gctx, GColorWhite);

    char l2[64];
    if (s_have_data && s_data_time) {
      char ago[16];
      human_ago((int)(time(NULL) - s_data_time), ago, sizeof(ago));
      snprintf(l2, sizeof(l2), "data %s", ago);
    } else {
      snprintf(l2, sizeof(l2), "%s", s_status);
    }
    graphics_draw_text(gctx, "Offline - SELECT to retry",
                       fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
                       GRect(b.origin.x + 5, b.origin.y + 2, b.size.w - 10, 22),
                       GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
    graphics_draw_text(gctx, l2, fonts_get_system_font(FONT_KEY_GOTHIC_14),
                       GRect(b.origin.x + 5, b.origin.y + 24, b.size.w - 10, 18),
                       GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
    return;
  }

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

  // The Favorites section is all favorites, so only mark one that shows up
  // elsewhere (shouldn't happen today, but keeps the cue if that changes).
  char sname[DEP_STOPNAME_LEN + 4];
  bool mark = (d->flags & DEP_FLAG_FAVORITE) && kind != KIND_FAVORITE;
  snprintf(sname, sizeof(sname), "%s%s", mark ? "* " : "", d->stop_name);
  graphics_draw_text(gctx, sname, fonts_get_system_font(FONT_KEY_GOTHIC_14),
                     GRect(x, b.origin.y + 41, w, 18),
                     GTextOverflowModeTrailingEllipsis, al, NULL);
}

static void menu_select_long(MenuLayer *menu, MenuIndex *idx, void *ctx) {
  int kind = section_kind(idx->section);
  if (kind == KIND_ALERTS || kind == KIND_STATUS) {
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

  if (kind == KIND_STATUS) {
    vibes_short_pulse();
    do_fetch(false);
    return;
  }

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

// Auto-refresh the list this many seconds after the last good batch, and no
// more often than this while it keeps failing.
#define AUTO_REFRESH_SECS 300
#define AUTO_REFRESH_RETRY 120

static void maybe_auto_refresh(void) {
  if (detail_is_open()) {
    return;
  }
  time_t now = time(NULL);
  bool stale = s_have_data && s_data_time != 0 &&
               now - s_data_time >= AUTO_REFRESH_SECS;
  // Retry on a schedule when the data is old OR the last attempt failed.
  if ((stale || s_offline) && now - s_last_fetch >= AUTO_REFRESH_RETRY) {
    do_fetch(true);   // quiet: keep the current rows visible
  }
}

#if defined(PBL_PLATFORM_EMERY)
// Pull-to-refresh: a downward drag that starts at the top of the list fires a
// refresh on liftoff. The touch-navigation bridge still scrolls the list; at
// the top a downward drag scrolls nothing, so the two do not fight.
#define PULL_TRIGGER_PX 44

static int16_t s_pull_y0;
static bool s_pull_active;
static bool s_pull_armed;

static bool list_at_top(void) {
  if (!s_menu) {
    return false;
  }
  MenuIndex i = menu_layer_get_selected_index(s_menu);
  return i.section == 0 && i.row == 0;
}

static void touch_handler(const TouchEvent *e, void *ctx) {
  if (detail_is_open() || e->non_navigational) {
    return;
  }
  switch (e->type) {
    case TouchEvent_Touchdown:
      s_pull_y0 = e->y;
      s_pull_active = list_at_top();
      s_pull_armed = false;
      break;
    case TouchEvent_PositionUpdate:
      if (s_pull_active && !s_pull_armed &&
          (e->y - s_pull_y0) > PULL_TRIGGER_PX) {
        s_pull_armed = true;
        vibes_short_pulse();   // "let go to refresh"
      }
      break;
    case TouchEvent_Liftoff:
      if (s_pull_armed) {
        refresh_ui_begin();   // "Refreshing..." bar; the vibe already fired
        do_fetch(true);       // quiet: keep the rows on screen
      }
      s_pull_active = false;
      s_pull_armed = false;
      break;
  }
}
#endif

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
  if (s_refresh_timer) {
    app_timer_cancel(s_refresh_timer);
    s_refresh_timer = NULL;
  }
  s_refreshing = false;
  menu_layer_destroy(s_menu);
  s_menu = NULL;
  text_layer_destroy(s_fresh);
  s_fresh = NULL;
  status_bar_layer_destroy(s_status_bar);
}

// Every minute: countdown values drift, so redraw, refresh the strip, and
// pull a fresh batch once the data is old enough.
static void tick_handler(struct tm *t, TimeUnits units) {
  update_freshness();
  maybe_auto_refresh();
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
#if defined(PBL_PLATFORM_EMERY)
  touch_service_subscribe(touch_handler, NULL);   // pull-to-refresh
#endif

  s_window = window_create();
  window_set_window_handlers(s_window, (WindowHandlers) {
    .load = window_load,
    .unload = window_unload,
  });
  window_stack_push(s_window, true);
}

static void deinit(void) {
#if defined(PBL_PLATFORM_EMERY)
  touch_service_unsubscribe();
#endif
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
