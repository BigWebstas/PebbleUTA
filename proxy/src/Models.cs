namespace PebbleUtaProxy;

/// <summary>One upcoming departure at a stop.</summary>
public sealed record DepartureDto(
    string Route,
    string RouteLong,
    string Color,
    string Headsign,
    long Departure,     // unix seconds
    bool Realtime,
    int Minutes,
    string TripId,
    int? DelayMin,      // + late / - early, null if unknown
    bool Wheelchair,
    bool HasAlert);

public sealed record StopDto(
    string StopId,
    string Name,
    string Code,        // number on the stop sign
    string Desc,        // cross streets
    double Lat,
    double Lon,
    int DistanceM,
    bool Wheelchair,
    IReadOnlyList<DepartureDto> Departures);

public sealed record NearbyResponse(
    long Generated,
    int RtAgeS,
    IReadOnlyList<AlertDto> Alerts,     // distinct alerts touching the returned stops/routes
    IReadOnlyList<StopDto> Stops);

public sealed record HealthResponse(
    string Status,
    string Build,
    string? GtfsLoadedAt,
    string? GtfsFeedVersion,
    string? RtFetchedAt,
    int RtAgeS,
    int Stops,
    int Routes,
    int Trips,
    int RtActiveTrips,
    int RtVehicles,
    int RtAlerts);

// --------------------------------------------------------------------- detail

public sealed record AlertDto(string Header, string Description);

public sealed record VehicleDto(
    int DistanceM,       // vehicle -> this stop
    int? MinutesAway,    // rough, from distance / speed
    int SpeedMph,
    int Bearing,         // degrees, -1 unknown
    string Compass);     // "N", "SW", ... "" unknown

public sealed record UpcomingStopDto(string Name, int Minutes, bool Realtime);

public sealed record DetailResponse(
    long Generated,
    int RtAgeS,
    string Route,
    string RouteLong,
    string Color,
    string Headsign,
    string StopName,
    string StopCode,
    string StopDesc,
    int Minutes,
    bool Realtime,
    int? DelayMin,
    bool Wheelchair,
    IReadOnlyList<int> NextMinutes,       // following departures of this route here
    VehicleDto? Vehicle,
    IReadOnlyList<UpcomingStopDto> Upcoming,
    IReadOnlyList<AlertDto> Alerts);
