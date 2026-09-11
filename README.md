# PebbleUTA

**UTA Transit** — a Pebble watchapp showing nearby UTA (Utah Transit
Authority) bus and rail departures and pinned favorites, backed by a
self-hosted proxy that turns UTA's open GTFS feeds into small JSON the watch
can use.

The watch has no GPS and no internet, so the data flows through the phone:

```
watch C app ──AppMessage──► pkjs (phone JS: GPS + HTTPS) ──HTTPS──► proxy ──► UTA feeds
```

## Why a proxy exists

UTA's realtime feed identifies a stop only by its position within a trip, not
by stop ID, so "what's next at my stop" requires joining against
`stop_times.txt` (~467k rows). That join is too large to ship on the watch or
run on the phone, so the proxy holds UTA's schedule in memory and answers
small, per-stop queries instead. No UTA API key or account is needed — the
feeds are public.
### A Proxy will be permanently hosted at https://uta.webstas.net for your conveinience.

## Layout

| Path | What it is |
|---|---|
| [`pebble-uta/`](pebble-uta) | The watchapp: C for the watch, PebbleKit JS for the phone side (location, HTTP, settings). |
| [`proxy/`](proxy) | The ASP.NET Core service. See [`proxy/README.md`](proxy/README.md) for endpoints and feed details. |

## Building

**Watch app** (`cd pebble-uta`): requires the Pebble SDK (`pebble-tool`).

```
pebble build
pebble install --emulator emery --logs   # or --phone <ip> for a real watch
```

**Proxy** (`cd proxy`): requires the .NET SDK.

```
dotnet build -c Release
dotnet run -- selftest 40.766 -111.891   # sanity-check feed access, no server
```

A prebuilt proxy binary is a self-contained single-file `.exe` — see
[`proxy/README.md`](proxy/README.md) for the publish command and deployment
options (Windows Service or IIS).

## License

GPL-3.0 — see [LICENSE](LICENSE).
