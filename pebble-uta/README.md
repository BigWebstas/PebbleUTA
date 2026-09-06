# UTA Transit (PebbleUTA)

A Pebble watch app that shows the next Utah Transit Authority (UTA) departures
for the stops closest to you, plus stops you pin as favorites.

## How it works

The watch has no GPS and cannot talk to the internet directly, so all the work
happens in the phone-side JavaScript (`src/pkjs/`):

1. Watch asks the phone for a refresh.
2. Phone reads GPS (`navigator.geolocation`).
3. Phone calls the **PebbleUTA proxy** at `https://uta.webstas.net`
   (`GET /departures?lat=&lon=&radius=`), which returns nearby stops with
   their next departures — schedule + realtime — already joined.
4. Phone flattens, sorts, and streams up to 15 rows to the watch.
5. Watch shows a menu: **Nearby** and **Favorites**.

The proxy (see [`../proxy/`](../proxy/)) does the GTFS + GTFS-realtime join so
the phone never parses protobuf or carries UTA's stop list. No API key. To
point the app at a different proxy, set `proxy_url` in the phone app's
localStorage (there is no settings field for it).

## Setup

```
pebble build
pebble install --emulator basalt --logs      # or: --phone <PHONE_IP>
```

No key or account. The app talks to `uta.webstas.net` out of the box. Open the
settings screen only to change search radius, time window, or walking speed.

## Using it

| Action | Result |
|---|---|
| Open the app | Auto-refreshes |
| Shake wrist | Refresh |
| Select (click) a departure | Open the detail page (vehicle, upcoming stops, alerts); polls every 15s while up |
| Select (click) an alert | Open the full alert text |
| Long-press a departure | Pin / unpin that stop as a favorite |

An **Alerts** section appears pinned at the top only when a service alert
affects one of the routes or stops shown. Rows carry a `!` when their route
has an alert, and `+N late` / `N early` when the proxy has a delay figure.

Each row shows: a colour bar for the route (GTFS `route_color`), the route and
headsign, then `<n> min` until it leaves, `- live` when that time is realtime,
`walk <n> min` on foot to the stop, and the stop name. Walk time comes
from the proxy's `distance_m`, so favorites reached without GPS show none. On
black-and-white watches (diorite) the colour bar is solid black.

## Emulator notes

- Geolocation in the emulator comes from `pypkjs` and often does not resolve;
  you may sit on "Locating...". The physical watch (via `--phone <IP>`) gets a
  real fix from the phone.
- `pebble logs` prints the pkjs status line and any `Proxy unreachable` /
  `Proxy is starting up` errors. All pkjs logging is ASCII-only on purpose
  (`pebble logs` crashes on a split multibyte char).

## Config reference (`src/pkjs/config.js`)

| Setting | Default | Notes |
|---|---|---|
| Search radius | ½ mile | ¼ / ½ / ¾ / 1 / 2 / 5 / 10 mile; how far to look for stops (stored in metres) |
| Departure window | 120 min | 45 / 90 / 120 min / 4 hours; passed to the proxy as `window` (proxy max 240) |
| Walking speed | Normal (3 mph) | Slow / Normal / Brisk / Fast; sets the "walk N" time per row |

The proxy host is not a setting. It is hardcoded to `https://uta.webstas.net`
in `src/pkjs/proxy.js`; override with `localStorage['proxy_url']`.

## Known gaps / next steps

- Availability rides on the proxy at `uta.webstas.net`. If it is down the app
  shows "Proxy unreachable"; if it is mid-reload, "Proxy is starting up".
- Favorites live only in phone `localStorage`. The watch shows nothing under
  Favorites until the first successful phone reply.
- No local persistence on the watch, so a launch shows "Locating..." until the
  phone answers.

## Files

```
src/c/pebble-uta.c     window, MenuLayer, AppMessage, shake-to-refresh
src/c/departures.[ch]  received-rows buffer + per-section lookup
src/pkjs/index.js      orchestration: GPS -> proxy -> stream to watch
src/pkjs/proxy.js      PebbleUTA proxy client (uta.webstas.net)
src/pkjs/format.js     flatten / dedupe / sort / cap departure rows
src/pkjs/favorites.js  saved stops in localStorage
src/pkjs/config.js     settings page (served as a data: URI, no hosting)
```
