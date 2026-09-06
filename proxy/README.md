# PebbleUTA Proxy

A small ASP.NET Core service that turns UTA's open feeds into a compact
"nearby departures" JSON API for the PebbleUTA watch app. No API key, no
account — it reads the same public feeds the watch cannot use directly:

- `https://apps.rideuta.com/tms/gtfs/Static` — GTFS schedule (zip)
- `https://apps.rideuta.com/tms/gtfs/TripUpdate` — GTFS-realtime (protobuf)

The realtime feed identifies each stop only by its position in the trip, so
the proxy joins it to `stop_times.txt` from the schedule feed and serves the
result already resolved.

## Why a proxy

The watch has no GPS or internet — the phone JS does the network calls. But
the join needs `stop_times.txt` (≈467k rows, 22 MB), which is too big to ship
in the watch app or parse on the phone. The proxy holds it in memory once and
answers small queries.

## Endpoints

| Route | Purpose |
|---|---|
| `GET /healthz` | status, feed version, counts, realtime age, vehicle/alert counts |
| `GET /departures?lat=&lon=&radius=&stops=&window=` | nearest stops + next departures + a top-level `alerts` array for the routes/stops shown |
| `GET /stops/{stopId}/departures?window=` | one stop, for pinned favorites (no GPS) |
| `GET /detail?trip=&stop=` | full detail for one departure: on-time delta, vehicle position + distance, the rest of the trip's stops with times, and any alerts |

It reads all three UTA realtime feeds (trip updates, vehicle positions,
alerts). Departures carry `trip_id`, `delay_min`, `wheelchair`, `has_alert`.
UTA does not publish occupancy/crowding.

`radius` metres (default 800, max 16000), `window` minutes (default 90),
`stops` max stops to return (default 8).

Response:

```json
{
  "generated": 1788658298,
  "rt_age_s": 3,
  "stops": [
    { "stop_id": "13105", "name": "200 S / Main St (EB)",
      "lat": 40.765, "lon": -111.891, "distance_m": 117,
      "departures": [
        { "route": "1", "route_long": "SOUTH TEMPLE", "color": "2EB566",
          "headsign": "University Hospital", "departure": 1788658754,
          "realtime": true, "minutes": 8 }
      ] }
  ]
}
```

## Requirements

- .NET SDK 10 to build. To use the .NET 8 LTS runtime instead, change
  `<TargetFramework>` in `PebbleUtaProxy.csproj` to `net8.0`.
- Windows Service host: nothing on the server if you publish self-contained.
- IIS host: the **ASP.NET Core Hosting Bundle** matching the target framework.

## Build and publish

```powershell
cd proxy
dotnet publish -c Release -r win-x64 --self-contained true `
  -p:PublishSingleFile=true -p:EnableCompressionInSingleFile=true `
  -p:PublishTrimmed=true -p:DebugType=none -o publish
```

That is one ~14 MB `PebbleUtaProxy.exe` with the runtime inside it — the
server needs **no .NET install**. Trimming is safe here because JSON goes
through the source-generated `AppJson` context (`src/Json.cs`); do not add
reflection-based serialization without adding it to that context.

Framework-dependent (tiny, but needs the ASP.NET Core Runtime 10 on the box):
`dotnet publish -c Release -o publish`.

Copy `publish\` to the server, e.g. `C:\Services\PebbleUtaProxy`.

## Host as a Windows Service (recommended)

The app self-detects service start and logs to the Application event log. The
listen port comes from `appsettings.json` -> `Urls` (default `:8080`), or from
the `-Port` switch below.

From an **elevated** PowerShell:

```powershell
cd proxy\deploy
.\install-service.ps1 -BinPath 'C:\Services\PebbleUtaProxy' -Port 8080
```

That creates an auto-start service `PebbleUtaProxy` running as
`NT AUTHORITY\LocalService`, sets it to restart on crash, opens inbound TCP
8080 in Windows Firewall, and starts it. Check:

```powershell
curl http://localhost:8080/healthz
```

Remove it with `.\uninstall-service.ps1`.

If `LocalService` hits a permission wall, reinstall with
`-Account 'LocalSystem'`.

## Host under IIS (alternative)

1. Install the ASP.NET Core Hosting Bundle, then `net stop was /y && net start w3svc`.
2. New Application Pool: **No Managed Code**, Start Mode **AlwaysRunning**,
   Idle Time-out **0**, Regular recycle interval **0**.
3. New Site/Application pointing at the publish folder, bound to a LAN port.
4. Optional: the IIS **Application Initialization** feature + `preloadEnabled="true"`
   so the ~10 s GTFS load happens at start, not on the first request.

The AlwaysRunning + idle-0 + no-recycle settings matter: on recycle the
in-memory schedule is dropped and the next request stalls ~10 s reloading. A
Windows Service has none of this lifecycle churn, which is why it is the
recommended host.

## Configuration

`appsettings.json` → `Proxy` section, or environment variables with `PROXY__`
prefix (double underscore), or (IIS) `<environmentVariable>` in `web.config`.

| Key | Default | Notes |
|---|---|---|
| `StaticUrl` | UTA GTFS zip | |
| `TripUpdatesUrl` | UTA GTFS-RT | |
| `AgencyTimeZone` | `America/Denver` | IANA id |
| `RtPollSeconds` | 25 | realtime refresh |
| `StaticRefreshHours` | 24 | schedule reload |
| `DefaultRadiusMeters` / `MaxRadiusMeters` | 800 / 16000 | |
| `MaxStops` | 8 | |
| `MaxDeparturesPerStop` | 6 | |
| `WindowMinutes` | 90 | |

## Verify without deploying

```powershell
dotnet run -- selftest 40.7660 -111.8910
```

Loads both feeds once and prints a sample `/departures` result for that point.
No web server, no listening socket. Tested output for downtown Salt Lake:
4 stops, 24 departures, 14 of them realtime.

## Memory

The schedule index (`stop_times`) holds ≈467k rows in memory — roughly
150–250 MB for the process. Give the app pool room; a 512 MB cap is tight.
