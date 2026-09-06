/* Saved (stop, route) pairs, persisted in the phone app's localStorage.
 * Shape: [{ stop_id, route, name }]. Every favorite is a specific route at a
 * specific stop -- old whole-stop favorites are dropped on load. */

var KEY = 'uta_favorites';

function keyOf(stopId, route) {
  return String(stopId) + '|' + String(route || '');
}

function load() {
  try {
    var raw = localStorage.getItem(KEY);
    var list = raw ? JSON.parse(raw) : [];
    if (!Array.isArray(list)) { return []; }
    return list.filter(function (f) {
      return f && f.stop_id && f.route;   // drops legacy whole-stop entries
    });
  } catch (e) {
    return [];
  }
}

function save(list) {
  try {
    localStorage.setItem(KEY, JSON.stringify(list.slice(0, 15)));
  } catch (e) {
    // storage full / unavailable — favorites just won't persist
  }
}

// Set of "stop|route" keys, for quick per-row lookup.
function keys() {
  var set = {};
  load().forEach(function (f) { set[keyOf(f.stop_id, f.route)] = true; });
  return set;
}

// stop_id -> { route: true, ... } for the favorites that need their own fetch.
function byStop() {
  var m = {};
  load().forEach(function (f) {
    (m[f.stop_id] = m[f.stop_id] || {})[f.route] = true;
  });
  return m;
}

function add(stopId, route, name) {
  if (!stopId) { return; }
  var list = load();
  var k = keyOf(stopId, route);
  for (var i = 0; i < list.length; i++) {
    if (keyOf(list[i].stop_id, list[i].route) === k) {
      list[i].name = name || list[i].name;
      save(list);
      return;
    }
  }
  list.push({ stop_id: String(stopId), route: String(route || ''), name: name || String(stopId) });
  save(list);
}

function remove(stopId, route) {
  var k = keyOf(stopId, route);
  save(load().filter(function (f) { return keyOf(f.stop_id, f.route) !== k; }));
}

module.exports = {
  keyOf: keyOf,
  load: load,
  keys: keys,
  byStop: byStop,
  add: add,
  remove: remove
};
