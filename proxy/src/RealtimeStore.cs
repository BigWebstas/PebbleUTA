using TransitRealtime;

namespace PebbleUtaProxy;

public readonly record struct RtStopUpdate(
    int Seq, string? StopId,
    long? ArrTime, int? ArrDelay,
    long? DepTime, int? DepDelay,
    int ScheduleRelationship);

public sealed record RtTrip(
    string TripId, string? RouteId, int ScheduleRelationship, IReadOnlyList<RtStopUpdate> Stops);

public sealed record RtVehicle(
    string TripId, double Lat, double Lon, float Bearing, float SpeedMps, long Timestamp, string? Label);

public sealed record RtAlert(
    string Id, string Header, string Description,
    IReadOnlyList<string> RouteIds, IReadOnlyList<string> StopIds, IReadOnlyList<string> TripIds);

/// <summary>
/// Latest parsed GTFS-realtime. The three feeds (trip updates, vehicle
/// positions, alerts) refresh independently and are read together.
/// </summary>
public sealed class RealtimeStore
{
    private volatile IReadOnlyDictionary<string, RtTrip> _trips =
        new Dictionary<string, RtTrip>();
    private volatile IReadOnlyDictionary<string, RtVehicle> _vehicles =
        new Dictionary<string, RtVehicle>();
    private volatile IReadOnlyList<RtAlert> _alerts = Array.Empty<RtAlert>();

    private long _fetchedAtMs;

    private readonly ILogger<RealtimeStore> _log;
    public RealtimeStore(ILogger<RealtimeStore> log) => _log = log;

    public IReadOnlyDictionary<string, RtTrip> Trips => _trips;
    public IReadOnlyDictionary<string, RtVehicle> Vehicles => _vehicles;
    public IReadOnlyList<RtAlert> Alerts => _alerts;
    public DateTimeOffset? FetchedAt
    {
        get
        {
            var ms = Interlocked.Read(ref _fetchedAtMs);
            return ms == 0 ? null : DateTimeOffset.FromUnixTimeMilliseconds(ms);
        }
    }
    public bool HasData => _trips.Count > 0;

    private void MarkFetched() =>
        Interlocked.Exchange(ref _fetchedAtMs, DateTimeOffset.UtcNow.ToUnixTimeMilliseconds());

    // ------------------------------------------------------------ trip updates

    public void ApplyTripUpdates(byte[] protobuf)
    {
        var feed = FeedMessage.Parser.ParseFrom(protobuf);
        var map = new Dictionary<string, RtTrip>(StringComparer.Ordinal);

        foreach (var entity in feed.Entity)
        {
            var tu = entity.TripUpdate;
            if (tu is null) continue;

            var trip = tu.Trip;
            var tripId = trip?.HasTripId == true ? trip.TripId : null;
            if (string.IsNullOrEmpty(tripId)) continue;

            // UTA does not always populate route_id in the trip descriptor; it
            // encodes "<trip_id>_<route_id>" as the entity id, so fall back to that.
            string? routeId = trip!.HasRouteId ? trip.RouteId : null;
            if (string.IsNullOrEmpty(routeId) && entity.Id.Contains('_'))
                routeId = entity.Id[(entity.Id.IndexOf('_') + 1)..];

            var stops = new List<RtStopUpdate>(tu.StopTimeUpdate.Count);
            foreach (var stu in tu.StopTimeUpdate)
            {
                stops.Add(new RtStopUpdate(
                    Seq: stu.HasStopSequence ? (int)stu.StopSequence : -1,
                    StopId: stu.HasStopId ? stu.StopId : null,
                    ArrTime: stu.Arrival is { HasTime: true } a ? a.Time : null,
                    ArrDelay: stu.Arrival is { HasDelay: true } ad ? ad.Delay : null,
                    DepTime: stu.Departure is { HasTime: true } d ? d.Time : null,
                    DepDelay: stu.Departure is { HasDelay: true } dd ? dd.Delay : null,
                    ScheduleRelationship: (int)stu.ScheduleRelationship));
            }

            map[tripId] = new RtTrip(tripId, routeId, (int)trip.ScheduleRelationship, stops);
        }

        _trips = map;
        MarkFetched();
        _log.LogDebug("trip updates: {N} trips", map.Count);
    }

    // -------------------------------------------------------- vehicle positions

    public void ApplyVehicles(byte[] protobuf)
    {
        var feed = FeedMessage.Parser.ParseFrom(protobuf);
        var map = new Dictionary<string, RtVehicle>(StringComparer.Ordinal);

        foreach (var entity in feed.Entity)
        {
            var v = entity.Vehicle;
            if (v?.Position is null) continue;

            var tripId = v.Trip?.HasTripId == true ? v.Trip.TripId : null;
            if (string.IsNullOrEmpty(tripId)) continue;

            map[tripId] = new RtVehicle(
                tripId,
                v.Position.Latitude,
                v.Position.Longitude,
                v.Position.HasBearing ? v.Position.Bearing : -1f,
                v.Position.HasSpeed ? v.Position.Speed : -1f,
                v.HasTimestamp ? (long)v.Timestamp : 0,
                v.Vehicle?.Label);
        }

        _vehicles = map;
        MarkFetched();
        _log.LogDebug("vehicle positions: {N}", map.Count);
    }

    // ------------------------------------------------------------------ alerts

    public void ApplyAlerts(byte[] protobuf)
    {
        var feed = FeedMessage.Parser.ParseFrom(protobuf);
        var list = new List<RtAlert>();

        foreach (var entity in feed.Entity)
        {
            var a = entity.Alert;
            if (a is null) continue;

            var header = FirstText(a.HeaderText);
            var desc = FirstText(a.DescriptionText);
            if (header.Length == 0 && desc.Length == 0) continue;

            var routes = new List<string>();
            var stops = new List<string>();
            var trips = new List<string>();
            foreach (var ie in a.InformedEntity)
            {
                if (!string.IsNullOrEmpty(ie.RouteId)) routes.Add(ie.RouteId);
                if (!string.IsNullOrEmpty(ie.StopId)) stops.Add(ie.StopId);
                if (ie.Trip is { HasTripId: true }) trips.Add(ie.Trip.TripId);
            }

            list.Add(new RtAlert(entity.Id, header, desc, routes, stops, trips));
        }

        _alerts = list;
        MarkFetched();
        _log.LogDebug("alerts: {N}", list.Count);
    }

    private static string FirstText(TranslatedString? ts)
    {
        if (ts is null || ts.Translation.Count == 0) return "";
        foreach (var t in ts.Translation)
            if (t.Language is null or "en" or "en-US") return t.Text ?? "";
        return ts.Translation[0].Text ?? "";
    }

    /// <summary>Alerts touching any of the given route / stop / trip ids.</summary>
    public List<RtAlert> AlertsFor(string? routeId, string? stopId, string? tripId)
    {
        var hits = new List<RtAlert>();
        foreach (var a in _alerts)
        {
            if ((routeId != null && a.RouteIds.Contains(routeId)) ||
                (stopId != null && a.StopIds.Contains(stopId)) ||
                (tripId != null && a.TripIds.Contains(tripId)))
                hits.Add(a);
        }
        return hits;
    }
}
