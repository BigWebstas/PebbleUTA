/* Self-contained settings page, served as a data: URI (no hosting needed).
 * Submitting navigates to the app's return URL (default `pebblejs://close#`)
 * with the settings JSON appended, which fires `webviewclosed`. */

var DEFAULTS = {
  radius: 800,    // metres (the proxy's unit); the page shows it as miles
  window_min: 120,
  walk_pace: 80   // metres per minute used to estimate walk time to a stop
};

function readSettings() {
  var s = {};
  Object.keys(DEFAULTS).forEach(function (k) {
    var v = null;
    try { v = localStorage.getItem('cfg_' + k); } catch (e) { v = null; }
    if (v === null || v === '') {
      s[k] = DEFAULTS[k];
    } else {
      s[k] = (typeof DEFAULTS[k] === 'number') ? Number(v) : v;
    }
  });
  return s;
}

function writeSettings(s) {
  Object.keys(DEFAULTS).forEach(function (k) {
    if (s[k] === undefined) { return; }
    try { localStorage.setItem('cfg_' + k, String(s[k])); } catch (e) { /* ignore */ }
  });
}

function page(settings) {
  var s = JSON.stringify(settings);
  var html =
'<!DOCTYPE html><html><head><meta charset="utf-8">' +
'<meta name="viewport" content="width=device-width,initial-scale=1">' +
'<title>UTA Transit</title><style>' +
'body{font:16px/1.4 -apple-system,Roboto,sans-serif;margin:0;padding:20px;background:#f4f4f4;color:#111}' +
'h1{font-size:20px;margin:0 0 16px}label{display:block;margin:16px 0 4px;font-weight:600}' +
'input,select{width:100%;box-sizing:border-box;padding:10px;font-size:16px;border:1px solid #bbb;border-radius:6px}' +
'p.hint{margin:4px 0 0;font-size:13px;color:#666}' +
'button{margin-top:24px;width:100%;padding:14px;font-size:17px;border:0;border-radius:6px;background:#0a84ff;color:#fff}' +
'a{color:#0a84ff}</style></head><body>' +
'<h1>UTA Transit settings</h1>' +
'<label for="radius">Search radius</label>' +
'<select id="radius">' +
'<option value="400">&frac14; mile</option>' +
'<option value="800">&frac12; mile</option>' +
'<option value="1200">&frac34; mile</option>' +
'<option value="1600">1 mile</option>' +
'<option value="3200">2 miles</option>' +
'<option value="8000">5 miles</option>' +
'<option value="16000">10 miles</option></select>' +
'<p class="hint">How far to look for stops around you.</p>' +
'<label for="window_min">Only show departures within</label>' +
'<select id="window_min">' +
'<option value="45">45 min</option>' +
'<option value="90">90 min</option>' +
'<option value="120">120 min</option>' +
'<option value="240">4 hours</option></select>' +
'<p class="hint">Useful where service is sparse.</p>' +
'<label for="walk_pace">Walking speed</label>' +
'<select id="walk_pace">' +
'<option value="60">Slow (2 mph)</option>' +
'<option value="80">Normal (3 mph)</option>' +
'<option value="100">Brisk (3.7 mph)</option>' +
'<option value="120">Fast (4.5 mph)</option></select>' +
'<p class="hint">Used to turn distance into the "walk N" time on each row.</p>' +
'<button id="save">Save</button>' +
'<script>' +
'var S=' + s + ';' +
'document.getElementById("radius").value=String(S.radius||800);' +
'document.getElementById("window_min").value=String(S.window_min||90);' +
'document.getElementById("walk_pace").value=String(S.walk_pace||80);' +
'function qp(n){var m=location.href.match(new RegExp("[?&]"+n+"=([^&#]*)"));' +
'return m?decodeURIComponent(m[1]):null;}' +
'var RT=qp("return_to")||"pebblejs://close#";' +
'document.getElementById("save").addEventListener("click",function(){' +
'var out={radius:Number(document.getElementById("radius").value),' +
'window_min:Number(document.getElementById("window_min").value),' +
'walk_pace:Number(document.getElementById("walk_pace").value)};' +
'location.href=RT+encodeURIComponent(JSON.stringify(out));});' +
'</script></body></html>';
  return 'data:text/html;charset=utf-8,' + encodeURIComponent(html);
}

module.exports = {
  DEFAULTS: DEFAULTS,
  readSettings: readSettings,
  writeSettings: writeSettings,
  page: page
};
