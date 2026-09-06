/* PebbleUTA — phone-side orchestration.
 *
 * On a fetch request from the watch:
 *   1. get GPS from the phone
 *   2. ask the proxy (uta.webstas.net) for nearby stops + joined departures
 *   3. add departures for any saved favorite not already in that list
 *   4. flatten / sort / cap, then stream rows to the watch over AppMessage
 */

var PROXY = require('./proxy');
var FMT = require('./format');
var FAV = require('./favorites');
var CFG = require('./config');

var MIN_FETCH_GAP_MS = 15000;
var LAST_LOC_KEY = 'uta_last_loc';

var busy = false;
var lastFetchAt = 0;

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

// `pebble logs` (libpebble2) crashes on a non-ASCII byte that lands on a socket
// buffer boundary, so every console.log goes through here ASCII-only. Also
// redact the API key so it never lands in a log file.
function log(s) {
  console.log(String(s)
    .replace(/apikey=[^&\s]+/gi, 'apikey=***')
    .replace(/[^\x20-\x7E]/g, '?'));
}

// Single AppMessage outbox queue: exactly one send in flight at a time.
// Firing Pebble.sendAppMessage while another send is pending throws, and an
// uncaught throw here restarts pkjs -> the whole pipeline loops.
var outq = [];
var sending = false;

function enqueue(dict) {
  outq.push(dict);
  if (outq.length > 40) { outq.splice(0, outq.length - 40); }
  pump();
}

function pump() {
  if (sending || !outq.length) { return; }
  sending = true;
  var dict = outq.shift();
  try {
    Pebble.sendAppMessage(dict,
      function () { sending = false; pump(); },
      function (e) { log('send failed: ' + JSON.stringify(e)); sending = false; pump(); });
  } catch (e) {
    log('send threw: ' + e.message);
    sending = false;
    setTimeout(pump, 200);
  }
}

function sendStatus(msg) {
  log('status: ' + msg);
  enqueue({ STATUS: msg });
}

function saveLastLoc(lat, lon) {
  try {
    localStorage.setItem(LAST_LOC_KEY, JSON.stringify({ lat: lat, lon: lon }));
  } catch (e) { /* ignore */ }
}

function lastLoc() {
  try {
    var v = localStorage.getItem(LAST_LOC_KEY);
    return v ? JSON.parse(v) : null;
  } catch (e) {
    return null;
  }
}

// Hand the rows to the watch through the outbox queue.
function sendRows(rows) {
  busy = false;
  if (!rows.length) {
    sendStatus('No departures found');
    return;
  }
  rows.forEach(function (r, i) {
    enqueue({
      IDX: i,
      COUNT: rows.length,
      ROUTE: r.route || '',
      HEADSIGN: r.headsign || '',
      STOP_NAME: r.stop_name || '',
      STOP_ID: r.stop_id || '',
      TRIP_ID: r.trip_id || '',
      MINUTES: r.minutes,
      DEP: r.dep_epoch || 0,
      WALK: (r.walk_min != null) ? r.walk_min : -1,
      DELAY: (r.delay_min != null) ? r.delay_min : -999,
      COLOR: r.route_color || '',
      FLAGS: (r.realtime ? 1 : 0) | (r.favorite ? 2 : 0) |
             (r.has_alert ? 4 : 0) | (r.wheelchair ? 8 : 0),
      SECTION: r.section
    });
  });
}

// Launcher glance subtext: the soonest pinned departure, as a live countdown.
// rows are already sorted by minutes, so the first favorite is the next one.
function updateGlance(rows) {
  var fav = null;
  for (var i = 0; i < rows.length; i++) {
    if (rows[i].favorite) { fav = rows[i]; break; }
  }

  if (!fav) {
    // No pinned departure right now. Clear the glance only if the user has no
    // favorites at all; otherwise leave the last one until it expires.
    if (!FAV.load().length) {
      try { Pebble.appGlanceReload([], function () {}, function () {}); }
      catch (e) { /* older firmware */ }
    }
    return;
  }

  var nowS = Math.round(Date.now() / 1000);
  var epoch = fav.dep_epoch || (nowS + (fav.minutes || 0) * 60);

  var label = fav.route || 'Route';
  if ((label + ' ' + (fav.headsign || '')).length <= 18 && fav.headsign) {
    label += ' ' + fav.headsign;
  }

  var slice = {
    layout: { subtitleTemplateString: label + '  {time_until(' + epoch + ')}' },
    expirationTime: new Date((epoch + 180) * 1000).toISOString()
  };
  log('glance: ' + slice.layout.subtitleTemplateString);
  try {
    Pebble.appGlanceReload([slice], function () {}, function (e) {
      log('glance fail: ' + JSON.stringify(e));
    });
  } catch (e) {
    log('glance err: ' + e.message);
  }
}

// Alerts get their own pinned section at the top of the watch menu.
// COUNT rides in every message; IDX 0 tells the watch to clear.
function sendAlerts(alerts) {
  log('sendAlerts: ' + alerts.length);
  if (!alerts.length) {
    enqueue({ A_IDX: 0, A_COUNT: 0 });
    return;
  }
  alerts.forEach(function (a, i) {
    enqueue({
      A_IDX: i,
      A_COUNT: alerts.length,
      A_HEAD: (a.header || 'Alert').slice(0, 96),
      A_BODY: (a.description || '').slice(0, 480)
    });
  });
}

// ---------------------------------------------------------------------------
// fetch pipeline
// ---------------------------------------------------------------------------

function collectDepartures(settings, lat, lon) {
  var favKeys = FAV.keys();       // { "stop|route": true }  for per-row marking
  var favByStop = FAV.byStop();   // { stop_id: { route: true, "": true } }
  var win = settings.window_min;

  var results = [];     // { stop_id, name, distance_m, from_nearby, route_filter, departures }
  var have = {};
  var nearbyError = null;
  var alerts = [];
  var alertSeen = {};

  function addAlerts(list) {
    (list || []).forEach(function (a) {
      var k = (a.header || '') + '|' + (a.description || '');
      if (alertSeen[k]) { return; }
      alertSeen[k] = true;
      alerts.push(a);
    });
  }

  function addStop(s, fromNearby, routeFilter) {
    if (!s || !s.stop_id || have[s.stop_id]) { return; }
    have[s.stop_id] = true;
    var deps = s.departures || [];
    if (routeFilter) {
      deps = deps.filter(function (d) {
        return routeFilter[d.route || d.route_long || ''];
      });
    }
    results.push({
      stop_id: s.stop_id,
      name: s.name || '',
      distance_m: (s.distance_m != null) ? s.distance_m : null,
      from_nearby: !!fromNearby,
      departures: deps
    });
  }

  function finish() {
    var rows = FMT.buildRows(results, favKeys, Date.now(), settings.walk_pace);
    log('finish: ' + results.length + ' stops -> ' + rows.length + ' rows, ' +
        alerts.length + ' alerts');
    if (!rows.length && nearbyError) {
      busy = false;   // leave the proxy-error status on screen
      return;
    }
    sendAlerts(alerts);
    sendRows(rows);
    updateGlance(rows);
  }

  function fetchFavorites() {
    var stopIds = Object.keys(favByStop).filter(function (sid) { return !have[sid]; });
    if (!stopIds.length) { finish(); return; }
    var left = stopIds.length;
    stopIds.forEach(function (sid) {
      PROXY.stopDepartures(sid, win, function (err, res) {
        if (err) {
          log('favorite ' + sid + ': ' + err.message);
        } else if (res && res.stops && res.stops[0]) {
          addStop(res.stops[0], false, favByStop[sid]);
          addAlerts(res.alerts);
        }
        if (--left === 0) { finish(); }
      });
    });
  }

  if (lat != null && lon != null) {
    sendStatus('Finding stops...');
    log('proxy.nearby ' + lat.toFixed(4) + ',' + lon.toFixed(4) + ' r=' + settings.radius);
    PROXY.nearby(lat, lon, settings.radius, win, function (err, res) {
      if (err) {
        nearbyError = err;
        log('nearby err: ' + err.message);
        sendStatus(err.status === 503 ? 'Proxy is starting up' : 'Proxy unreachable');
      } else {
        var n = (res.stops || []).length;
        log('nearby ok: ' + n + ' stops, rt_age=' + res.rt_age_s);
        (res.stops || []).forEach(function (st) { addStop(st, true); });
        addAlerts(res.alerts);
      }
      fetchFavorites();
    });
  } else {
    fetchFavorites();
  }
}

function fetchAndSend(force) {
  var now = Date.now();
  if (busy) { log('fetch skipped: busy'); return; }
  if (!force && now - lastFetchAt < MIN_FETCH_GAP_MS) { return; }
  lastFetchAt = now;

  var settings = CFG.readSettings();
  busy = true;

  // Location override from settings ("lat, lon"); blank -> use GPS.
  if (settings.test_loc && settings.test_loc.indexOf(',') > 0) {
    var p = settings.test_loc.split(',');
    var olat = parseFloat(p[0]), olon = parseFloat(p[1]);
    if (isFinite(olat) && isFinite(olon)) {
      log('location override ' + olat + ',' + olon);
      sendStatus('Finding stops...');
      collectDepartures(settings, olat, olon);
      return;
    }
  }

  sendStatus('Locating...');

  navigator.geolocation.getCurrentPosition(
    function (pos) {
      saveLastLoc(pos.coords.latitude, pos.coords.longitude);
      collectDepartures(settings, pos.coords.latitude, pos.coords.longitude);
    },
    function (err) {
      log('geolocation error: ' + err.message);
      var last = lastLoc();
      if (last) {
        sendStatus('No GPS - using last location');
        collectDepartures(settings, last.lat, last.lon);
      } else if (FAV.load().length) {
        sendStatus('No GPS - favorites only');
        collectDepartures(settings, null, null);
      } else {
        busy = false;
        sendStatus('No GPS and no favorites');
      }
    },
    { timeout: 15000, maximumAge: 60000, enableHighAccuracy: false }
  );
}

// ---------------------------------------------------------------------------
// events
// ---------------------------------------------------------------------------

Pebble.addEventListener('ready', function () {
  log('PebbleUTA pkjs ready');
  fetchAndSend();
});

Pebble.addEventListener('appmessage', function (e) {
  var d = e.payload || {};
  if (d.SAVE_FAV) {
    log('save fav ' + d.SAVE_FAV + ' / ' + (d.FAV_ROUTE || '*'));
    FAV.add(d.SAVE_FAV, d.FAV_ROUTE || '', d.FAV_NAME || d.SAVE_FAV);
    fetchAndSend(true);        // force past the refresh cooldown
  } else if (d.DEL_FAV) {
    log('del fav ' + d.DEL_FAV + ' / ' + (d.FAV_ROUTE || '*'));
    FAV.remove(d.DEL_FAV, d.FAV_ROUTE || '');
    fetchAndSend(true);
  } else if (d.DETAIL_REQ != null) {
    sendDetail(d.D_TRIP, d.D_STOP);
  } else if (d.FETCH != null) {
    fetchAndSend();
  }
});

// --------------------------------------------------------------------- detail

// Header lines are "\x01" + kind + text. The watch draws them as coloured
// bands: R = route (tinted with the route colour), H = section, A = alert.
var HDR = '';
function hR(t) { return HDR + 'R' + t; }
function hH(t) { return HDR + 'H' + t; }
function hA(t) { return HDR + 'A' + t; }

function detailText(x) {
  var L = [];
  L.push(hR(x.route + (x.route_long ? '  ' + x.route_long : '')));
  if (x.headsign) { L.push(x.headsign); }

  var line = (x.minutes <= 0 ? 'now' : x.minutes + ' min');
  if (x.realtime) { line += ' - live'; }
  if (x.delay_min != null && x.delay_min !== 0) {
    line += x.delay_min > 0 ? '  +' + x.delay_min + ' late' : '  ' + (-x.delay_min) + ' early';
  }
  L.push(line);
  if (x.next_minutes && x.next_minutes.length) {
    L.push('then ' + x.next_minutes.join(', ') + ' min');
  }
  L.push('');

  L.push(hH(x.stop_name || 'Stop'));
  if (x.stop_code) { L.push('Stop #' + x.stop_code); }
  if (x.stop_desc) { L.push(x.stop_desc); }
  if (x.wheelchair) { L.push('wheelchair accessible'); }

  if (x.vehicle) {
    var v = x.vehicle;
    L.push('');
    L.push(hH('Vehicle'));
    var d = (v.distance_m >= 1609)
      ? (v.distance_m / 1609).toFixed(1) + ' mi'
      : v.distance_m + ' m';
    var vl = d + (v.compass ? ' ' + v.compass : '') + ' away';
    if (v.minutes_away != null) { vl += ', ~' + v.minutes_away + ' min out'; }
    L.push(vl);
    if (v.speed_mph > 0) { L.push(v.speed_mph + ' mph'); }
  }

  if (x.upcoming && x.upcoming.length) {
    L.push('');
    L.push(hH('Next stops'));
    x.upcoming.forEach(function (u) {
      L.push(u.minutes + ' min  ' + u.name + (u.realtime ? '' : ' (sched)'));
    });
  }

  (x.alerts || []).forEach(function (a) {
    L.push('');
    L.push(hA(a.header || 'Alert'));
    if (a.description) { L.push(a.description); }
  });

  return L.join('\n');
}

// Stream a text blob to the watch detail window in <=400-byte chunks.
function sendDetailText(text) {
  var CH = 400;
  var total = Math.max(1, Math.ceil(text.length / CH));
  for (var i = 0; i < total; i++) {
    enqueue({
      D_SEQ: i,
      D_LAST: (i === total - 1) ? 1 : 0,
      D_TEXT: text.substr(i * CH, CH)
    });
  }
}

function sendDetail(tripId, stopId) {
  if (!tripId || !stopId) {
    sendDetailText('No detail for this row.');
    return;
  }
  PROXY.detail(tripId, stopId, function (err, res) {
    if (err) {
      log('detail err: ' + err.message);
      sendDetailText(err.status === 404 ? 'This trip just ended.' : 'Detail unavailable.\n' + err.message);
      return;
    }
    sendDetailText(detailText(res));
  });
}

Pebble.addEventListener('showConfiguration', function () {
  Pebble.openURL(CFG.page(CFG.readSettings()));
});

Pebble.addEventListener('webviewclosed', function (e) {
  if (!e || !e.response) { return; }
  var cfg = null;
  try {
    cfg = JSON.parse(decodeURIComponent(e.response));
  } catch (err) {
    try {
      cfg = JSON.parse(e.response);   // some apps pre-decode the response
    } catch (err2) {
      log('bad config response: ' + err2.message);
      return;
    }
  }
  CFG.writeSettings(cfg);
  fetchAndSend(true);
});
