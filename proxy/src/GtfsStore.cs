using System.Globalization;
using System.IO.Compression;
using CsvHelper;
using CsvHelper.Configuration;

namespace PebbleUtaProxy;

public readonly record struct StopInfo(
    string Id, string Name, double Lat, double Lon, string Code, string Desc, bool Wheelchair);

public readonly record struct RouteInfo(
    string Id, string ShortName, string LongName, string Color, string Type, string TypeLabel);

public readonly record struct TripInfo(
    string RouteId, string Headsign, string ServiceId, string DirectionId, bool Wheelchair);

public readonly record struct StopTimeRef(string StopId, int DepSec);

public readonly record struct SchedRef(string TripId, int DepSec, int Seq);

public sealed record CalendarRow(string ServiceId, bool[] Days, string Start, string End);

public sealed record FeedInfo(string Publisher, string Start, string End, string Version);

/// <summary>An immutable parsed copy of the GTFS schedule feed.</summary>
public sealed class GtfsSnapshot
{
    public required DateTimeOffset LoadedAt { get; init; }
    public FeedInfo? FeedInfo { get; init; }

    public required IReadOnlyDictionary<string, StopInfo> Stops { get; init; }
    public required IReadOnlyList<StopInfo> BoardableStops { get; init; }
    public required IReadOnlyDictionary<string, RouteInfo> Routes { get; init; }
    public required IReadOnlyDictionary<string, TripInfo> Trips { get; init; }

    /// <summary>trip_id -&gt; (stop_sequence -&gt; stop + scheduled departure).</summary>
    public required IReadOnlyDictionary<string, Dictionary<int, StopTimeRef>> TripStops { get; init; }

    /// <summary>stop_id -&gt; scheduled departures at that stop, sorted by DepSec.</summary>
    public required IReadOnlyDictionary<string, List<SchedRef>> StopSchedule { get; init; }

    public required IReadOnlyList<CalendarRow> Calendar { get; init; }

    /// <summary>"YYYYMMDD" -&gt; (service_id -&gt; exception_type 1|2).</summary>
    public required IReadOnlyDictionary<string, Dictionary<string, int>> CalendarDates { get; init; }

    public int StopCount => BoardableStops.Count;
    public int RouteCount => Routes.Count;
    public int TripCount => Trips.Count;
}

public sealed class GtfsStore
{
    private volatile GtfsSnapshot? _current;
    public GtfsSnapshot? Current => _current;
    public bool Ready => _current is not null;

    private readonly HttpClient _http;
    private readonly ProxyOptions _opt;
    private readonly ILogger<GtfsStore> _log;

    public GtfsStore(IHttpClientFactory httpFactory, IConfiguration config, ILogger<GtfsStore> log)
    {
        _http = httpFactory.CreateClient("feeds");
        _opt = config.GetSection(ProxyOptions.SectionName).Get<ProxyOptions>() ?? new ProxyOptions();
        _log = log;
    }

    private static readonly string[] DayCols =
        { "sunday", "monday", "tuesday", "wednesday", "thursday", "friday", "saturday" };

    public async Task LoadAsync(CancellationToken ct)
    {
        var sw = System.Diagnostics.Stopwatch.StartNew();
        _log.LogInformation("Downloading GTFS static feed {Url}", _opt.StaticUrl);

        var bytes = await _http.GetByteArrayAsync(_opt.StaticUrl, ct);
        using var zip = new ZipArchive(new MemoryStream(bytes), ZipArchiveMode.Read);

        var intern = new Dictionary<string, string>(StringComparer.Ordinal);
        string I(string? s) => s is null ? "" : (intern.TryGetValue(s, out var v) ? v : (intern[s] = s));

        var feedInfo = ReadFeedInfo(zip);

        // ---- stops ----
        var stops = new Dictionary<string, StopInfo>(StringComparer.Ordinal);
        var boardable = new List<StopInfo>();
        foreach (var r in ReadRows(zip, "stops.txt"))
        {
            var id = I(r.Get("stop_id"));
            var lat = ParseDouble(r.Get("stop_lat"));
            var lon = ParseDouble(r.Get("stop_lon"));
            var info = new StopInfo(id, r.Get("stop_name") ?? id, lat, lon,
                r.Get("stop_code") ?? "", r.Get("stop_desc") ?? "",
                r.Get("wheelchair_boarding") == "1");
            stops[id] = info;
            var locType = r.Get("location_type") ?? "";
            if ((locType is "" or "0") && !double.IsNaN(lat) && !double.IsNaN(lon))
                boardable.Add(info);
        }

        // ---- routes ----
        var routes = new Dictionary<string, RouteInfo>(StringComparer.Ordinal);
        foreach (var r in ReadRows(zip, "routes.txt"))
        {
            var id = I(r.Get("route_id"));
            var type = r.Get("route_type") ?? "";
            routes[id] = new RouteInfo(
                id,
                r.Get("route_short_name") ?? "",
                r.Get("route_long_name") ?? "",
                (r.Get("route_color") ?? "").TrimStart('#').ToUpperInvariant(),
                type,
                LabelForType(type));
        }

        // ---- trips ----
        var trips = new Dictionary<string, TripInfo>(StringComparer.Ordinal);
        foreach (var r in ReadRows(zip, "trips.txt"))
        {
            trips[I(r.Get("trip_id"))] = new TripInfo(
                I(r.Get("route_id")),
                r.Get("trip_headsign") ?? "",
                I(r.Get("service_id")),
                r.Get("direction_id") ?? "",
                r.Get("wheelchair_accessible") == "1");
        }

        // ---- calendar ----
        var calendar = new List<CalendarRow>();
        foreach (var r in ReadRows(zip, "calendar.txt"))
        {
            var days = new bool[7];
            for (var i = 0; i < 7; i++) days[i] = r.Get(DayCols[i]) == "1";
            calendar.Add(new CalendarRow(I(r.Get("service_id")), days,
                r.Get("start_date") ?? "", r.Get("end_date") ?? ""));
        }

        // ---- calendar_dates ----
        var calDates = new Dictionary<string, Dictionary<string, int>>(StringComparer.Ordinal);
        foreach (var r in ReadRows(zip, "calendar_dates.txt"))
        {
            var date = r.Get("date") ?? "";
            if (date.Length == 0) continue;
            if (!calDates.TryGetValue(date, out var m))
                calDates[date] = m = new Dictionary<string, int>(StringComparer.Ordinal);
            m[I(r.Get("service_id"))] = int.TryParse(r.Get("exception_type"), out var et) ? et : 0;
        }

        // ---- stop_times (large; stream) ----
        var tripStops = new Dictionary<string, Dictionary<int, StopTimeRef>>(StringComparer.Ordinal);
        var stopSchedule = new Dictionary<string, List<SchedRef>>(StringComparer.Ordinal);
        var rowCount = 0;
        foreach (var r in ReadRows(zip, "stop_times.txt"))
        {
            rowCount++;
            if (!int.TryParse(r.Get("stop_sequence"), out var seq)) continue;
            var depSec = GtfsTimeToSec(r.Get("departure_time")) ?? GtfsTimeToSec(r.Get("arrival_time"));
            if (depSec is null) continue;

            var tripId = I(r.Get("trip_id"));
            var stopId = I(r.Get("stop_id"));

            if (!tripStops.TryGetValue(tripId, out var seqMap))
                tripStops[tripId] = seqMap = new Dictionary<int, StopTimeRef>();
            seqMap[seq] = new StopTimeRef(stopId, depSec.Value);

            if (!stopSchedule.TryGetValue(stopId, out var list))
                stopSchedule[stopId] = list = new List<SchedRef>();
            list.Add(new SchedRef(tripId, depSec.Value, seq));
        }
        foreach (var list in stopSchedule.Values)
            list.Sort((a, b) => a.DepSec.CompareTo(b.DepSec));

        _current = new GtfsSnapshot
        {
            LoadedAt = DateTimeOffset.UtcNow,
            FeedInfo = feedInfo,
            Stops = stops,
            BoardableStops = boardable,
            Routes = routes,
            Trips = trips,
            TripStops = tripStops,
            StopSchedule = stopSchedule,
            Calendar = calendar,
            CalendarDates = calDates
        };

        _log.LogInformation(
            "GTFS loaded in {Sec:F1}s: {Stops} stops, {Routes} routes, {Trips} trips, {Rows} stop_times",
            sw.Elapsed.TotalSeconds, boardable.Count, routes.Count, trips.Count, rowCount);
    }

    /// <summary>service_ids running on the given "YYYYMMDD".</summary>
    public static HashSet<string> ActiveServices(GtfsSnapshot snap, string ymd)
    {
        var dow = DowOf(ymd);
        var set = new HashSet<string>(StringComparer.Ordinal);
        foreach (var c in snap.Calendar)
            if (string.CompareOrdinal(ymd, c.Start) >= 0 &&
                string.CompareOrdinal(ymd, c.End) <= 0 && c.Days[dow])
                set.Add(c.ServiceId);

        if (snap.CalendarDates.TryGetValue(ymd, out var ex))
            foreach (var (sid, type) in ex)
            {
                if (type == 1) set.Add(sid);
                else if (type == 2) set.Remove(sid);
            }
        return set;
    }

    // ------------------------------------------------------------------ helpers

    private readonly struct Row(Dictionary<string, int> map, string[] fields)
    {
        public string? Get(string col) =>
            map.TryGetValue(col, out var i) && i < fields.Length ? fields[i] : null;
    }

    private static IEnumerable<Row> ReadRows(ZipArchive zip, string name)
    {
        var entry = zip.GetEntry(name);
        if (entry is null) yield break;

        using var reader = new StreamReader(entry.Open());
        var cfg = new CsvConfiguration(CultureInfo.InvariantCulture)
        {
            DetectDelimiter = false,
            TrimOptions = TrimOptions.Trim,
            BadDataFound = null,
            MissingFieldFound = null
        };
        using var csv = new CsvReader(reader, cfg);
        if (!csv.Read() || !csv.ReadHeader()) yield break;

        var header = csv.HeaderRecord ?? Array.Empty<string>();
        var map = new Dictionary<string, int>(StringComparer.OrdinalIgnoreCase);
        for (var i = 0; i < header.Length; i++) map[header[i].Trim().Trim('﻿')] = i;

        while (csv.Read())
        {
            var n = csv.Parser.Count;
            var fields = new string[n];
            for (var i = 0; i < n; i++) fields[i] = csv.GetField(i) ?? "";
            yield return new Row(map, fields);
        }
    }

    private static FeedInfo? ReadFeedInfo(ZipArchive zip)
    {
        foreach (var r in ReadRows(zip, "feed_info.txt"))
            return new FeedInfo(
                r.Get("feed_publisher_name") ?? "",
                r.Get("feed_start_date") ?? "",
                r.Get("feed_end_date") ?? "",
                r.Get("feed_version") ?? "");
        return null;
    }

    private static string LabelForType(string t) => t switch
    {
        "0" or "1" or "2" => "Rail",
        "3" or "11" => "Bus",
        "5" => "Streetcar",
        _ => ""
    };

    private static double ParseDouble(string? s) =>
        double.TryParse(s, NumberStyles.Float, CultureInfo.InvariantCulture, out var d) ? d : double.NaN;

    /// <summary>GTFS "H:MM:SS" (hours may exceed 23) -&gt; seconds after midnight.</summary>
    public static int? GtfsTimeToSec(string? s)
    {
        if (string.IsNullOrWhiteSpace(s)) return null;
        var parts = s.Trim().Split(':');
        if (parts.Length != 3) return null;
        if (int.TryParse(parts[0], out var h) && int.TryParse(parts[1], out var m) &&
            int.TryParse(parts[2], out var sec))
            return h * 3600 + m * 60 + sec;
        return null;
    }

    private static int DowOf(string ymd)
    {
        var d = new DateTime(int.Parse(ymd[..4]), int.Parse(ymd[4..6]), int.Parse(ymd[6..8]),
            0, 0, 0, DateTimeKind.Utc);
        return (int)d.DayOfWeek; // Sunday = 0
    }
}
