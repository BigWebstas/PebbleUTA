using System.Globalization;

namespace PebbleUtaProxy;

public sealed class DeparturesService
{
    private readonly GtfsStore _gtfs;
    private readonly RealtimeStore _rt;
    private readonly ProxyOptions _opt;
    private readonly TimeZoneInfo _tz;
    private readonly ILogger<DeparturesService> _log;

    public DeparturesService(
        GtfsStore gtfs, RealtimeStore rt, IConfiguration config, ILogger<DeparturesService> log)
    {
        _gtfs = gtfs;
        _rt = rt;
        _opt = config.GetSection(ProxyOptions.SectionName).Get<ProxyOptions>() ?? new ProxyOptions();
        _log = log;
        try { _tz = TimeZoneInfo.FindSystemTimeZoneById(_opt.AgencyTimeZone); }
        catch (TimeZoneNotFoundException)
        {
            _log.LogWarning("Time zone '{Tz}' not found; falling back to UTC", _opt.AgencyTimeZone);
            _tz = TimeZoneInfo.Utc;
        }
    }

    public bool Ready => _gtfs.Ready;

    // --------------------------------------------------------------- public API

    public NearbyResponse QueryByLocation(double lat, double lon, int radiusM, int maxStops, int windowMin)
    {
        var snap = _gtfs.Current ?? throw new InvalidOperationException("GTFS not loaded");
        var now = DateTimeOffset.UtcNow;

        var near = new List<(StopInfo Stop, int Dist)>();
        foreach (var s in snap.BoardableStops)
        {
            var d = Geo.HaversineMetres(lat, lon, s.Lat, s.Lon);
            if (d <= radiusM) near.Add((s, (int)Math.Round(d)));
        }
        near.Sort((a, b) => a.Dist.CompareTo(b.Dist));

        // Alerts: match against every stop within the radius, the routes
        // actually leaving, and any agency-wide alert.
        var nearStopIds = new HashSet<string>(near.Select(n => n.Stop.Id), StringComparer.Ordinal);

        var stops = new List<StopDto>();
        var routeLabels = new HashSet<string>(StringComparer.Ordinal);
        foreach (var (stop, dist) in near.Take(Math.Max(1, maxStops)))
        {
            var deps = DeparturesForStop(snap, stop.Id, now, windowMin, _opt.MaxDeparturesPerStop);
            if (deps.Count == 0) continue;
            foreach (var d in deps) routeLabels.Add(d.Route);
            stops.Add(ToStopDto(stop, dist, deps));
        }
        return new NearbyResponse(
            now.ToUnixTimeSeconds(), RtAgeSeconds(now),
            MatchAlerts(nearStopIds, routeLabels), stops);
    }

    public NearbyResponse? QueryByStop(string stopId, int windowMin)
    {
        var snap = _gtfs.Current ?? throw new InvalidOperationException("GTFS not loaded");
        if (!snap.Stops.TryGetValue(stopId, out var stop)) return null;

        var now = DateTimeOffset.UtcNow;
        var deps = DeparturesForStop(snap, stopId, now, windowMin, _opt.MaxDeparturesPerStop);
        var stops = new List<StopDto> { ToStopDto(stop, 0, deps) };
        var routeLabels = new HashSet<string>(deps.Select(d => d.Route), StringComparer.Ordinal);
        return new NearbyResponse(
            now.ToUnixTimeSeconds(), RtAgeSeconds(now),
            MatchAlerts(new HashSet<string>(new[] { stopId }, StringComparer.Ordinal), routeLabels),
            stops);
    }

    /// <summary>
    /// Distinct alerts touching one of the given stop ids, one of the given
    /// route labels, or the whole agency (no informed entity).
    /// </summary>
    private List<AlertDto> MatchAlerts(HashSet<string> stopIds, HashSet<string> routeLabels)
    {
        if (_rt.Alerts.Count == 0) return new List<AlertDto>();

        var snap = _gtfs.Current;
        var seen = new HashSet<string>(StringComparer.Ordinal);
        var outp = new List<AlertDto>();
        foreach (var a in _rt.Alerts)
        {
            bool touches;
            if (a.StopIds.Count == 0 && a.RouteIds.Count == 0)
            {
                touches = true; // agency-wide
            }
            else
            {
                touches = a.StopIds.Any(stopIds.Contains);
                if (!touches && snap is not null)
                    foreach (var rid in a.RouteIds)
                        if (snap.Routes.TryGetValue(rid, out var ri) &&
                            routeLabels.Contains(RouteLabel(ri, rid))) { touches = true; break; }
            }

            if (touches && seen.Add(a.Header + "\n" + a.Description))
                outp.Add(new AlertDto(a.Header.Trim(), a.Description.Trim()));
        }
        return outp;
    }

    public DetailResponse? Detail(string tripId, string stopId)
    {
        var snap = _gtfs.Current ?? throw new InvalidOperationException("GTFS not loaded");
        if (!snap.Stops.TryGetValue(stopId, out var stop)) return null;

        var now = DateTimeOffset.UtcNow;
        snap.Trips.TryGetValue(tripId, out var trip);
        var rtTrip = _rt.Trips.GetValueOrDefault(tripId);

        var routeId = !string.IsNullOrEmpty(trip.RouteId) ? trip.RouteId : rtTrip?.RouteId;
        RouteInfo route = default;
        if (routeId is not null) snap.Routes.TryGetValue(routeId, out route);

        var here = TripStopAt(snap, tripId, stopId);   // (seq, depSec)
        if (here is null && rtTrip is null) return null;

        // this departure at this stop
        DateTimeOffset? when = null;
        int? delayMin = null;
        var realtime = false;
        if (rtTrip is not null)
        {
            var su = FindStopUpdate(snap, rtTrip, stopId);
            if (su is { } u)
            {
                when = ResolveRealtimeInstant(snap, rtTrip, u, now);
                var dsec = u.DepDelay ?? u.ArrDelay;
                if (dsec is not null) delayMin = (int)Math.Round(dsec.Value / 60.0);
                realtime = when is not null;
            }
        }
        if (when is null && here is { } h0)
            when = SchedInstant(now, h0.DepSec);

        var minutes = when is null ? 0 : (int)Math.Max(0, Math.Round((when.Value - now).TotalMinutes));

        // following departures of this route at this stop
        var routeLabel = RouteLabel(route, routeId);
        var next = DeparturesForStop(snap, stopId, now, 240, 25)
            .Where(d => d.Route == routeLabel && d.Minutes > minutes)
            .Select(d => d.Minutes)
            .Take(4)
            .ToList();

        // upcoming stops on this trip
        var upcoming = new List<UpcomingStopDto>();
        if (here is { } h && snap.TripStops.TryGetValue(tripId, out var seqMap))
        {
            foreach (var kv in seqMap.Where(k => k.Key > h.Seq).OrderBy(k => k.Key))
            {
                if (!snap.Stops.TryGetValue(kv.Value.StopId, out var us)) continue;
                var (ut, urt) = UpcomingInstant(snap, rtTrip, kv.Key, kv.Value.StopId, kv.Value.DepSec, now);
                var um = (int)Math.Max(0, Math.Round((ut - now).TotalMinutes));
                upcoming.Add(new UpcomingStopDto(us.Name, um, urt));
                if (upcoming.Count >= _opt.DetailUpcomingStops) break;
            }
        }

        // vehicle
        VehicleDto? vehicle = null;
        if (_rt.Vehicles.TryGetValue(tripId, out var v))
        {
            var dist = (int)Math.Round(Geo.HaversineMetres(v.Lat, v.Lon, stop.Lat, stop.Lon));
            int? mins = v.SpeedMps > 0.5f ? (int)Math.Round(dist / v.SpeedMps / 60.0) : null;
            var mph = v.SpeedMps >= 0 ? (int)Math.Round(v.SpeedMps * 2.23694) : 0;
            var bearing = v.Bearing >= 0 ? (int)Math.Round(v.Bearing) % 360 : -1;
            vehicle = new VehicleDto(dist, mins, mph, bearing, Compass8(bearing));
        }

        // alerts
        var alerts = _rt.AlertsFor(routeId, stopId, tripId)
            .Select(a => new AlertDto(a.Header, a.Description))
            .ToList();

        return new DetailResponse(
            now.ToUnixTimeSeconds(), RtAgeSeconds(now),
            routeLabel, route.LongName ?? "", route.Color ?? "",
            (!string.IsNullOrEmpty(trip.Headsign) ? trip.Headsign : route.LongName) ?? "",
            stop.Name, stop.Code, stop.Desc,
            minutes, realtime, delayMin, stop.Wheelchair && trip.Wheelchair,
            next, vehicle, upcoming, alerts);
    }

    public HealthResponse Health()
    {
        var g = _gtfs.Current;
        var now = DateTimeOffset.UtcNow;
        return new HealthResponse(
            Status: g is null ? "loading" : "ok",
            Build: "2026-09-05-scoped-alerts",
            GtfsLoadedAt: g?.LoadedAt.ToString("o"),
            GtfsFeedVersion: g?.FeedInfo?.Version,
            RtFetchedAt: _rt.FetchedAt?.ToString("o"),
            RtAgeS: RtAgeSeconds(now),
            Stops: g?.StopCount ?? 0,
            Routes: g?.RouteCount ?? 0,
            Trips: g?.TripCount ?? 0,
            RtActiveTrips: _rt.Trips.Count,
            RtVehicles: _rt.Vehicles.Count,
            RtAlerts: _rt.Alerts.Count);
    }

    // --------------------------------------------------------------- internals

    private StopDto ToStopDto(StopInfo stop, int dist, IReadOnlyList<DepartureDto> deps) =>
        new(stop.Id, stop.Name, stop.Code, stop.Desc, stop.Lat, stop.Lon, dist, stop.Wheelchair, deps);

    private int RtAgeSeconds(DateTimeOffset now)
    {
        var f = _rt.FetchedAt;
        return f is null ? -1 : (int)Math.Round((now - f.Value).TotalSeconds);
    }

    private List<DepartureDto> DeparturesForStop(
        GtfsSnapshot snap, string stopId, DateTimeOffset now, int windowMin, int cap)
    {
        var horizon = now.AddMinutes(windowMin);
        var floor = now.AddSeconds(-60);
        var rows = new List<(DateTimeOffset When, DepartureDto Row)>();
        var seen = new HashSet<string>(StringComparer.Ordinal);

        foreach (var tu in _rt.Trips.Values)
        {
            if (tu.ScheduleRelationship is 3 or 7) continue; // canceled / deleted
            foreach (var su in tu.Stops)
            {
                var sid = su.StopId;
                if (sid is null &&
                    snap.TripStops.TryGetValue(tu.TripId, out var seqMap) &&
                    su.Seq >= 0 && seqMap.TryGetValue(su.Seq, out var stRef))
                    sid = stRef.StopId;
                if (!string.Equals(sid, stopId, StringComparison.Ordinal)) continue;

                var when = ResolveRealtimeInstant(snap, tu, su, now);
                if (when is null || when < floor || when > horizon) continue;

                var dsec = su.DepDelay ?? su.ArrDelay;
                int? delayMin = dsec is null ? null : (int)Math.Round(dsec.Value / 60.0);
                rows.Add((when.Value, MakeRow(snap, tu.TripId, tu.RouteId, when.Value, now, true, delayMin)));
                seen.Add(tu.TripId);
            }
        }

        if (snap.StopSchedule.TryGetValue(stopId, out var sched) && sched.Count > 0)
        {
            var midnightToday = LocalMidnightUtc(now);
            var midnightYest = midnightToday.AddDays(-1);
            var activeToday = GtfsStore.ActiveServices(snap, Ymd(now));
            var activeYest = GtfsStore.ActiveServices(snap, Ymd(now.AddDays(-1)));

            foreach (var sc in sched)
            {
                if (seen.Contains(sc.TripId)) continue;
                if (!snap.Trips.TryGetValue(sc.TripId, out var trip)) continue;

                DateTimeOffset? when = null;
                if (activeToday.Contains(trip.ServiceId))
                {
                    var t = midnightToday.AddSeconds(sc.DepSec);
                    if (t >= now && t <= horizon) when = t;
                }
                if (when is null && activeYest.Contains(trip.ServiceId))
                {
                    var t = midnightYest.AddSeconds(sc.DepSec);
                    if (t >= now && t <= horizon) when = t;
                }
                if (when is null) continue;

                rows.Add((when.Value, MakeRow(snap, sc.TripId, trip.RouteId, when.Value, now, false, null)));
            }
        }

        rows.Sort((a, b) => a.When.CompareTo(b.When));

        var outp = new List<DepartureDto>(Math.Min(rows.Count, cap));
        var dedupe = new HashSet<string>(StringComparer.Ordinal);
        foreach (var (_, row) in rows)
        {
            if (!dedupe.Add($"{row.Route}|{row.Headsign}|{row.Minutes}")) continue;
            outp.Add(row);
            if (outp.Count >= cap) break;
        }
        return outp;
    }

    private DepartureDto MakeRow(
        GtfsSnapshot snap, string tripId, string? routeIdHint,
        DateTimeOffset when, DateTimeOffset now, bool realtime, int? delayMin)
    {
        snap.Trips.TryGetValue(tripId, out var trip);
        var routeId = !string.IsNullOrEmpty(trip.RouteId) ? trip.RouteId : routeIdHint;

        RouteInfo route = default;
        if (routeId is not null) snap.Routes.TryGetValue(routeId, out route);

        var headsign = !string.IsNullOrEmpty(trip.Headsign) ? trip.Headsign : route.LongName;
        var minutes = (int)Math.Max(0, Math.Round((when - now).TotalMinutes));
        var hasAlert = _rt.AlertsFor(routeId, null, tripId).Count > 0;

        return new DepartureDto(
            Route: RouteLabel(route, routeId),
            RouteLong: route.LongName ?? "",
            Color: route.Color ?? "",
            Headsign: headsign ?? "",
            Departure: when.ToUnixTimeSeconds(),
            Realtime: realtime,
            Minutes: minutes,
            TripId: tripId,
            DelayMin: delayMin,
            Wheelchair: trip.Wheelchair,
            HasAlert: hasAlert);
    }

    private static string RouteLabel(RouteInfo route, string? routeId) =>
        !string.IsNullOrEmpty(route.ShortName) ? route.ShortName
        : !string.IsNullOrEmpty(route.LongName) ? route.LongName
        : !string.IsNullOrEmpty(route.TypeLabel) ? route.TypeLabel
        : routeId ?? "?";

    private static (int Seq, int DepSec)? TripStopAt(GtfsSnapshot snap, string tripId, string stopId)
    {
        if (!snap.TripStops.TryGetValue(tripId, out var seqMap)) return null;
        foreach (var kv in seqMap)
            if (string.Equals(kv.Value.StopId, stopId, StringComparison.Ordinal))
                return (kv.Key, kv.Value.DepSec);
        return null;
    }

    private static RtStopUpdate? FindStopUpdate(GtfsSnapshot snap, RtTrip tu, string stopId)
    {
        foreach (var su in tu.Stops)
        {
            var sid = su.StopId;
            if (sid is null &&
                snap.TripStops.TryGetValue(tu.TripId, out var seqMap) &&
                su.Seq >= 0 && seqMap.TryGetValue(su.Seq, out var stRef))
                sid = stRef.StopId;
            if (string.Equals(sid, stopId, StringComparison.Ordinal)) return su;
        }
        return null;
    }

    private (DateTimeOffset When, bool Realtime) UpcomingInstant(
        GtfsSnapshot snap, RtTrip? rtTrip, int seq, string stopId, int depSec, DateTimeOffset now)
    {
        if (rtTrip is not null)
        {
            foreach (var su in rtTrip.Stops)
            {
                if (su.Seq != seq && !string.Equals(su.StopId, stopId, StringComparison.Ordinal)) continue;
                var t = ResolveRealtimeInstant(snap, rtTrip, su, now);
                if (t is not null) return (t.Value, true);
            }
        }
        return (SchedInstant(now, depSec), false);
    }

    private DateTimeOffset? ResolveRealtimeInstant(
        GtfsSnapshot snap, RtTrip tu, RtStopUpdate su, DateTimeOffset now)
    {
        if (su.DepTime is > 0) return DateTimeOffset.FromUnixTimeSeconds(su.DepTime.Value);
        if (su.ArrTime is > 0) return DateTimeOffset.FromUnixTimeSeconds(su.ArrTime.Value);

        var delay = su.DepDelay ?? su.ArrDelay;
        if (delay is null) return null;
        if (su.Seq < 0 ||
            !snap.TripStops.TryGetValue(tu.TripId, out var seqMap) ||
            !seqMap.TryGetValue(su.Seq, out var stRef))
            return null;
        return LocalMidnightUtc(now).AddSeconds(stRef.DepSec + delay.Value);
    }

    private DateTimeOffset SchedInstant(DateTimeOffset now, int depSec)
    {
        var mid = LocalMidnightUtc(now);
        var cands = new[]
        {
            mid.AddSeconds(depSec),
            mid.AddDays(-1).AddSeconds(depSec),
            mid.AddDays(1).AddSeconds(depSec)
        };
        Array.Sort(cands, (a, b) =>
            Math.Abs((a - now).Ticks).CompareTo(Math.Abs((b - now).Ticks)));
        return cands[0];
    }

    private static string Compass8(int bearing)
    {
        if (bearing < 0) return "";
        string[] pts = { "N", "NE", "E", "SE", "S", "SW", "W", "NW" };
        return pts[(int)Math.Round(bearing / 45.0) % 8];
    }

    private DateTimeOffset LocalMidnightUtc(DateTimeOffset nowUtc)
    {
        var local = TimeZoneInfo.ConvertTime(nowUtc, _tz);
        var midnight = new DateTime(local.Year, local.Month, local.Day, 0, 0, 0, DateTimeKind.Unspecified);
        return new DateTimeOffset(TimeZoneInfo.ConvertTimeToUtc(midnight, _tz), TimeSpan.Zero);
    }

    private string Ymd(DateTimeOffset nowUtc) =>
        TimeZoneInfo.ConvertTime(nowUtc, _tz).ToString("yyyyMMdd", CultureInfo.InvariantCulture);
}

public static class Geo
{
    public static double HaversineMetres(double aLat, double aLon, double bLat, double bLon)
    {
        const double r = 6371000;
        var toRad = Math.PI / 180;
        var dLat = (bLat - aLat) * toRad;
        var dLon = (bLon - aLon) * toRad;
        var h = Math.Sin(dLat / 2) * Math.Sin(dLat / 2) +
                Math.Cos(aLat * toRad) * Math.Cos(bLat * toRad) *
                Math.Sin(dLon / 2) * Math.Sin(dLon / 2);
        return 2 * r * Math.Asin(Math.Sqrt(h));
    }
}
