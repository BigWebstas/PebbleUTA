namespace PebbleUtaProxy;

/// <summary>Bound from the "Proxy" section of appsettings.json / environment.</summary>
public sealed class ProxyOptions
{
    public const string SectionName = "Proxy";

    public string StaticUrl { get; set; } = "https://apps.rideuta.com/tms/gtfs/Static";
    public string TripUpdatesUrl { get; set; } = "https://apps.rideuta.com/tms/gtfs/TripUpdate";
    public string VehiclePositionsUrl { get; set; } = "https://apps.rideuta.com/tms/gtfs/Vehicle";
    public string AlertsUrl { get; set; } = "https://apps.rideuta.com/tms/gtfs/Alert";

    /// <summary>IANA id, e.g. "America/Denver". Resolved via TimeZoneInfo (ICU).</summary>
    public string AgencyTimeZone { get; set; } = "America/Denver";

    public int RtPollSeconds { get; set; } = 25;
    public int StaticRefreshHours { get; set; } = 24;

    public int DefaultRadiusMeters { get; set; } = 800;
    public int MaxRadiusMeters { get; set; } = 16000;
    public int MaxStops { get; set; } = 8;
    public int MaxDeparturesPerStop { get; set; } = 6;
    public int WindowMinutes { get; set; } = 90;
    public int DetailUpcomingStops { get; set; } = 8;
}
