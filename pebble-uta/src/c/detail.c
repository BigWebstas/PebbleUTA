#include "detail.h"

#define DETAIL_BODY 2048
#define DETAIL_POLL_MS 15000

// A header line in the body blob is: 0x01, a kind byte, then the header text.
// Kinds: 'R' route (tinted with the route colour), 'H' section, 'A' alert.
#define HDR_MARK 0x01

static Window *s_window;
static ScrollLayer *s_scroll;
static Layer *s_content;
static AppTimer *s_timer;

static char s_body[DETAIL_BODY];       // text from the proxy (or the alert)
static char s_render[DETAIL_BODY + 80]; // pin line + body, what the layer shows
static char s_trip[24];
static char s_stop[48];
static char s_route[16];
static char s_stop_name[48];
static GColor s_route_color;
static bool s_poll;      // trip mode -> poll + allow pinning
static bool s_is_fav;
static bool s_open;

// ---------------------------------------------------------------------------

static void send_request(void) {
  DictionaryIterator *iter;
  if (app_message_outbox_begin(&iter) != APP_MSG_OK) {
    return;
  }
  dict_write_uint8(iter, MESSAGE_KEY_DETAIL_REQ, 1);
  dict_write_cstring(iter, MESSAGE_KEY_D_TRIP, s_trip);
  dict_write_cstring(iter, MESSAGE_KEY_D_STOP, s_stop);
  app_message_outbox_send();
}

static void timer_cb(void *ctx) {
  s_timer = NULL;
  if (!s_open || !s_poll) {
    return;
  }
  send_request();
  s_timer = app_timer_register(DETAIL_POLL_MS, timer_cb, NULL);
}

// Length-aware int read: Pebble packs small ints as int8/int16.
static int32_t tup_i(const Tuple *t) {
  if (!t) {
    return 0;
  }
  switch (t->length) {
    case 1: return t->value->int8;
    case 2: return t->value->int16;
    default: return t->value->int32;
  }
}

// ---------------------------------------------------------------------------
// Rendering: walk s_render line by line, drawing coloured header bands and
// wrapped body paragraphs. Called once to measure (ctx NULL) and again from the
// content layer's update_proc to draw. Returns the total content height.
// ---------------------------------------------------------------------------

#define HDR_H 24
#define PAD_X 4
#define LINE_GAP 2
#define BLANK_GAP 6

static void header_colors(char kind, GColor *bg, GColor *fg) {
  if (kind == 'R') {
    *bg = gcolor_equal(s_route_color, GColorClear)
        ? PBL_IF_COLOR_ELSE(GColorLightGray, GColorBlack)
        : s_route_color;
    // GColor8 channels are 0..3; pick readable text off a rough luma.
    unsigned luma = bg->r * 2u + bg->g * 3u + bg->b;
    *fg = (luma >= 12) ? GColorBlack : GColorWhite;
  } else if (kind == 'A') {
    *bg = PBL_IF_COLOR_ELSE(GColorDarkCandyAppleRed, GColorBlack);
    *fg = GColorWhite;
  } else {
    *bg = PBL_IF_COLOR_ELSE(GColorCobaltBlue, GColorBlack);
    *fg = GColorWhite;
  }
}

static int16_t layout(GContext *ctx, int16_t width) {
  const int16_t tw = width - 2 * PAD_X;
  GFont body_font = fonts_get_system_font(FONT_KEY_GOTHIC_18);
  GFont hdr_font = fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD);
  int16_t y = 0;

  static char line[512];   // not reentrant: one draw pass at a time
  const char *p = s_render;
  while (*p) {
    const char *nl = strchr(p, '\n');
    size_t len = nl ? (size_t)(nl - p) : strlen(p);
    if (len > sizeof(line) - 1) {
      len = sizeof(line) - 1;
    }
    memcpy(line, p, len);
    line[len] = '\0';

    if (len == 0) {
      y += BLANK_GAP;
    } else if ((uint8_t)line[0] == HDR_MARK) {
      char kind = line[1] ? line[1] : 'H';
      const char *text = line[1] ? line + 2 : line + 1;
      GColor bg, fg;
      header_colors(kind, &bg, &fg);
      if (ctx) {
        graphics_context_set_fill_color(ctx, bg);
        graphics_fill_rect(ctx, GRect(0, y, width, HDR_H), 0, GCornerNone);
        graphics_context_set_text_color(ctx, fg);
        graphics_draw_text(ctx, text, hdr_font,
                           GRect(PAD_X, y + 1, tw, HDR_H),
                           GTextOverflowModeTrailingEllipsis,
                           GTextAlignmentLeft, NULL);
      }
      y += HDR_H + LINE_GAP;
    } else {
      GSize u = graphics_text_layout_get_content_size(
          line, body_font, GRect(0, 0, tw, 4000),
          GTextOverflowModeWordWrap, GTextAlignmentLeft);
      if (ctx) {
        graphics_context_set_text_color(ctx, GColorBlack);
        graphics_draw_text(ctx, line, body_font,
                           GRect(PAD_X, y, tw, u.h + 4),
                           GTextOverflowModeWordWrap, GTextAlignmentLeft, NULL);
      }
      y += u.h + LINE_GAP;
    }

    if (!nl) {
      break;
    }
    p = nl + 1;
  }
  return y;
}

static void content_update(Layer *layer, GContext *ctx) {
  layout(ctx, layer_get_bounds(layer).size.w);
}

static void render(void) {
  const char *pin = "";
  if (s_poll) {
    pin = s_is_fav ? "* PINNED  -  SELECT to unpin\n\n"
                   : "SELECT to pin this route here\n\n";
  }
  snprintf(s_render, sizeof(s_render), "%s%s", pin, s_body);

  GRect b = layer_get_bounds(scroll_layer_get_layer(s_scroll));
  int16_t h = layout(NULL, b.size.w) + 8;
  layer_set_frame(s_content, GRect(0, 0, b.size.w, h));
  layer_mark_dirty(s_content);
  scroll_layer_set_content_size(s_scroll, GSize(b.size.w, h + 8));
  scroll_layer_set_content_offset(s_scroll, GPointZero, false);
}

void detail_handle_message(DictionaryIterator *iter) {
  if (!s_open) {
    return;
  }
  Tuple *seq_t = dict_find(iter, MESSAGE_KEY_D_SEQ);
  Tuple *txt_t = dict_find(iter, MESSAGE_KEY_D_TEXT);
  Tuple *last_t = dict_find(iter, MESSAGE_KEY_D_LAST);
  if (!seq_t || !txt_t) {
    return;
  }
  if (tup_i(seq_t) == 0) {
    s_body[0] = '\0';
  }
  size_t len = strlen(s_body);
  if (len < DETAIL_BODY - 1) {
    strncpy(s_body + len, txt_t->value->cstring, DETAIL_BODY - 1 - len);
    s_body[DETAIL_BODY - 1] = '\0';
  }
  APP_LOG(APP_LOG_LEVEL_DEBUG, "detail seq=%d last=%d len=%d",
          (int)tup_i(seq_t), (int)tup_i(last_t), (int)strlen(s_body));
  if (tup_i(last_t) == 1) {
    render();
  }
}

// ---------------------------------------------------------------------------

static void select_click(ClickRecognizerRef rec, void *ctx) {
  if (!s_poll) {
    return;   // alert view: nothing to do
  }
  DictionaryIterator *iter;
  if (app_message_outbox_begin(&iter) != APP_MSG_OK) {
    return;
  }
  dict_write_cstring(iter, MESSAGE_KEY_FAV_ROUTE, s_route);
  if (s_is_fav) {
    dict_write_cstring(iter, MESSAGE_KEY_DEL_FAV, s_stop);
  } else {
    dict_write_cstring(iter, MESSAGE_KEY_SAVE_FAV, s_stop);
    dict_write_cstring(iter, MESSAGE_KEY_FAV_NAME, s_stop_name);
  }
  app_message_outbox_send();

  s_is_fav = !s_is_fav;
  vibes_short_pulse();
  render();
}

static void click_config(void *ctx) {
  window_single_click_subscribe(BUTTON_ID_SELECT, select_click);
}

static void window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(root);

  s_scroll = scroll_layer_create(bounds);
  scroll_layer_set_callbacks(s_scroll, (ScrollLayerCallbacks) {
    .click_config_provider = click_config,
  });
  scroll_layer_set_click_config_onto_window(s_scroll, window);
  scroll_layer_set_shadow_hidden(s_scroll, false);

  s_content = layer_create(GRect(0, 0, bounds.size.w, bounds.size.h));
  layer_set_update_proc(s_content, content_update);
  scroll_layer_add_child(s_scroll, s_content);

  layer_add_child(root, scroll_layer_get_layer(s_scroll));
}

static void window_unload(Window *window) {
  if (s_timer) {
    app_timer_cancel(s_timer);
    s_timer = NULL;
  }
  s_poll = false;
  s_open = false;
  layer_destroy(s_content);
  scroll_layer_destroy(s_scroll);
}

static void window_appear(Window *window) {
  s_open = true;
  render();
  if (s_poll) {
    send_request();
    if (s_timer) { app_timer_cancel(s_timer); }
    s_timer = app_timer_register(DETAIL_POLL_MS, timer_cb, NULL);
  }
}

// ---------------------------------------------------------------------------

void detail_init(void) {
  s_window = window_create();
  window_set_window_handlers(s_window, (WindowHandlers) {
    .load = window_load,
    .appear = window_appear,
    .unload = window_unload,
  });
}

void detail_deinit(void) {
  window_destroy(s_window);
}

bool detail_is_open(void) {
  return s_open;
}

void detail_push_trip(const char *trip_id, const char *stop_id, const char *route,
                      const char *stop_name, bool is_fav, GColor route_color) {
  snprintf(s_trip, sizeof(s_trip), "%s", trip_id ? trip_id : "");
  snprintf(s_stop, sizeof(s_stop), "%s", stop_id ? stop_id : "");
  snprintf(s_route, sizeof(s_route), "%s", route ? route : "");
  snprintf(s_stop_name, sizeof(s_stop_name), "%s", stop_name ? stop_name : "");
  s_route_color = route_color;
  s_is_fav = is_fav;
  s_poll = true;
  snprintf(s_body, sizeof(s_body), "%s", "Loading...");
  window_stack_push(s_window, true);
}

void detail_push_alert(const char *head, const char *body) {
  s_poll = false;
  s_is_fav = false;
  s_route_color = GColorClear;
  snprintf(s_body, sizeof(s_body), "%c%c%s\n\n%s",
           HDR_MARK, 'A', head ? head : "Alert", body ? body : "");
  window_stack_push(s_window, true);
}
