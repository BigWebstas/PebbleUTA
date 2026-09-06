/* Client for the PebbleUTA proxy.
 *
 * The proxy is permanently hosted at https://uta.webstas.net/ and does the
 * GTFS + realtime join, so pkjs only makes one or two plain GET calls.
 * A `proxy_url` value in localStorage overrides the host (for testing).
 */

var BASE = 'https://uta.webstas.net';
try {
  var override = localStorage.getItem('proxy_url');
  if (override) { BASE = override.replace(/\/+$/, ''); }
} catch (e) { /* ignore */ }

var REQUEST_MS = 8000;   // hard ceiling; the phone recycles idle pkjs ~12-15s

function plog(s) {
  // ASCII only: a split multibyte char crashes `pebble logs`.
  console.log('[proxy] ' + String(s).replace(/[^\x20-\x7E]/g, '?'));
}

function mkErr(message, status) {
  var e = new Error(message);
  e.status = status;
  return e;
}

function getJSON(url, cb) {
  plog('GET ' + url);
  var settled = false;
  var req = new XMLHttpRequest();

  function finish(err, data) {
    if (settled) { return; }
    settled = true;
    clearTimeout(guard);
    try { if (err) { req.abort(); } } catch (e) { /* ignore */ }
    cb(err, data);
  }

  var guard = setTimeout(function () {
    plog('timeout after ' + REQUEST_MS + 'ms');
    finish(mkErr('proxy timeout', 0), null);
  }, REQUEST_MS);

  req.onload = function () {
    plog('status ' + req.status);
    if (req.status >= 200 && req.status < 300) {
      var body;
      try { body = JSON.parse(req.responseText); }
      catch (e) { return finish(mkErr('bad JSON from proxy', req.status), null); }
      finish(null, body);
    } else {
      finish(mkErr('HTTP ' + req.status + ' from proxy', req.status), null);
    }
  };
  req.onerror = function () {
    plog('network error');
    finish(mkErr('proxy unreachable', 0), null);
  };
  req.ontimeout = function () {
    plog('xhr ontimeout');
    finish(mkErr('proxy timeout', 0), null);
  };

  try {
    req.open('GET', url, true);
    req.timeout = REQUEST_MS;
    req.send();
    plog('sent');
  } catch (e) {
    plog('send threw: ' + e.message);
    finish(mkErr('request failed: ' + e.message, 0), null);
  }
}

/* Nearest stops with their next departures already joined. */
function nearby(lat, lon, radiusM, windowMin, cb) {
  getJSON(BASE + '/departures' +
    '?lat=' + encodeURIComponent(lat) +
    '&lon=' + encodeURIComponent(lon) +
    '&radius=' + encodeURIComponent(radiusM) +
    '&stops=8' +
    '&window=' + encodeURIComponent(windowMin), cb);
}

/* One stop by id — used for pinned favorites when GPS is unavailable. */
function stopDepartures(stopId, windowMin, cb) {
  getJSON(BASE + '/stops/' + encodeURIComponent(stopId) + '/departures' +
    '?window=' + encodeURIComponent(windowMin), cb);
}

/* Full detail for one departure: vehicle position, upcoming stops, alerts. */
function detail(tripId, stopId, cb) {
  getJSON(BASE + '/detail' +
    '?trip=' + encodeURIComponent(tripId) +
    '&stop=' + encodeURIComponent(stopId), cb);
}

module.exports = {
  base: function () { return BASE; },
  nearby: nearby,
  stopDepartures: stopDepartures,
  detail: detail
};
