/* Flatten the proxy's per-stop departure lists into a sorted, capped list of
 * rows ready to stream to the watch. */

var MAX_ROWS = 15;
var MAX_FAV_ROWS = 10;       // reserve the rest of MAX_ROWS for nearby
var DEFAULT_WALK_MPM = 80;   // metres per minute, ~4.8 km/h

/* stopResults: [{ stop_id, name, distance_m, departures: [proxy departure] }]
 *   proxy departure: { route, route_long, color, headsign, departure(epoch s),
 *                      realtime, minutes }
 * favKeys:  { "stopId|route": true } set (route "" = whole stop)
 * nowMs:    Date.now()
 * walkPace: metres/min (0 or undefined -> default)
 */
function buildRows(stopResults, favKeys, nowMs, walkPace) {
  var pace = walkPace > 0 ? walkPace : DEFAULT_WALK_MPM;
  var rows = [];
  var seen = {};

  stopResults.forEach(function (sr) {
    var walk = (sr.distance_m != null && sr.distance_m >= 0)
      ? Math.max(1, Math.round(sr.distance_m / pace))
      : null;

    (sr.departures || []).forEach(function (d) {
      var epochMs = (d.departure || 0) * 1000;
      var minutes = epochMs
        ? Math.round((epochMs - nowMs) / 60000)
        : (d.minutes != null ? d.minutes : null);
      if (minutes == null || minutes < 0) { return; }

      var route = d.route || d.route_long || 'Route';
      var headsign = d.headsign || '';
      var isFav = !!favKeys[sr.stop_id + '|' + route];
      var key = sr.stop_id + '|' + route + '|' + headsign + '|' + minutes;
      if (seen[key]) { return; }
      seen[key] = true;

      rows.push({
        // Every favorite goes in the pinned Favorites section at the top,
        // in range or not; other routes at the same stop stay under Nearby.
        section: isFav ? 1 : 0,
        stop_id: sr.stop_id,
        stop_name: sr.name || '',
        trip_id: d.trip_id || '',
        route: route,
        route_color: d.color || '',
        headsign: headsign,
        minutes: minutes,
        dep_epoch: epochMs ? Math.round(epochMs / 1000) : 0,
        walk_min: walk,
        delay_min: (d.delay_min != null) ? d.delay_min : null,
        realtime: !!d.realtime,
        wheelchair: !!d.wheelchair,
        has_alert: !!d.has_alert,
        favorite: isFav
      });
    });
  });

  rows.sort(function (a, b) {
    if (a.minutes !== b.minutes) { return a.minutes - b.minutes; }
    return a.route < b.route ? -1 : 1;
  });

  // Favorites (section 1) are never dropped by the row cap, even when their
  // next departure is far out; nearby departures fill the remaining slots.
  var fav = [];
  var near = [];
  rows.forEach(function (r) { (r.section === 1 ? fav : near).push(r); });
  fav = fav.slice(0, MAX_FAV_ROWS);
  return fav.concat(near.slice(0, MAX_ROWS - fav.length));
}

module.exports = { buildRows: buildRows };
