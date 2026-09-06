using PebbleUtaProxy;

// `PebbleUtaProxy selftest [lat lon]` — load the feeds once, print a sample
// nearby-departures result, and exit. Handy for verifying feed access.
if (args.Length > 0 && args[0] is "selftest" or "--selftest")
{
    await SelfTest.RunAsync(args);
    return;
}

var builder = WebApplication.CreateBuilder(new WebApplicationOptions
{
    Args = args,
    // A Windows Service starts in C:\Windows\System32; keep config + logs next
    // to the binary. No-op when launched normally.
    ContentRootPath = AppContext.BaseDirectory
});

// Runs as a Windows Service when SCM started the process; no-op otherwise.
builder.Services.AddWindowsService(o => o.ServiceName = "PebbleUTA Proxy");

builder.Services.ConfigureHttpJsonOptions(o =>
    o.SerializerOptions.TypeInfoResolverChain.Insert(0, AppJson.Default));

builder.Services.AddHttpClient("feeds", c =>
{
    c.Timeout = TimeSpan.FromSeconds(30);
    c.DefaultRequestHeaders.UserAgent.ParseAdd("PebbleUtaProxy/1.0 (+github PebbleUTA)");
});

builder.Services.AddSingleton<GtfsStore>();
builder.Services.AddSingleton<RealtimeStore>();
builder.Services.AddSingleton<DeparturesService>();
builder.Services.AddHostedService<FeedWorker>();

var app = builder.Build();

var opt = app.Configuration.GetSection(ProxyOptions.SectionName).Get<ProxyOptions>() ?? new ProxyOptions();

int ClampRadius(string? raw) =>
    Math.Clamp(int.TryParse(raw, out var v) ? v : opt.DefaultRadiusMeters, 100, opt.MaxRadiusMeters);

int Window(string? raw) =>
    Math.Clamp(int.TryParse(raw, out var v) ? v : opt.WindowMinutes, 5, 240);

IResult Err(string message, int status) =>
    Results.Json(new ErrorDto(message), AppJson.Default.ErrorDto, statusCode: status);

app.MapGet("/", () => Results.Json(new InfoDto("PebbleUtaProxy", new[]
{
    "/healthz",
    "/departures?lat={lat}&lon={lon}&radius={m}&stops={n}&window={min}",
    "/stops/{stopId}/departures?window={min}",
    "/detail?trip={tripId}&stop={stopId}"
}), AppJson.Default.InfoDto));

app.MapGet("/healthz", (DeparturesService svc) =>
{
    var h = svc.Health();
    return Results.Json(h, AppJson.Default.HealthResponse, statusCode: h.Status == "ok" ? 200 : 503);
});

app.MapGet("/departures", (DeparturesService svc,
    double? lat, double? lon, string? radius, string? stops, string? window) =>
{
    if (!svc.Ready) return Err("gtfs still loading", 503);
    if (lat is null || lon is null || double.IsNaN(lat.Value) || double.IsNaN(lon.Value))
        return Err("lat and lon are required", 400);

    var maxStops = Math.Clamp(int.TryParse(stops, out var s) ? s : opt.MaxStops, 1, 25);
    var res = svc.QueryByLocation(lat.Value, lon.Value, ClampRadius(radius), maxStops, Window(window));
    return Results.Json(res, AppJson.Default.NearbyResponse);
});

app.MapGet("/stops/{stopId}/departures", (DeparturesService svc, string stopId, string? window) =>
{
    if (!svc.Ready) return Err("gtfs still loading", 503);
    var res = svc.QueryByStop(stopId, Window(window));
    return res is null ? Err("unknown stop", 404) : Results.Json(res, AppJson.Default.NearbyResponse);
});

app.MapGet("/detail", (DeparturesService svc, string? trip, string? stop) =>
{
    if (!svc.Ready) return Err("gtfs still loading", 503);
    if (string.IsNullOrEmpty(trip) || string.IsNullOrEmpty(stop))
        return Err("trip and stop are required", 400);

    var res = svc.Detail(trip, stop);
    return res is null ? Err("unknown trip/stop", 404) : Results.Json(res, AppJson.Default.DetailResponse);
});

app.Run();
