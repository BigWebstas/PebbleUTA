using System.Text.Json;
using Microsoft.Extensions.Configuration;
using Microsoft.Extensions.Logging;

namespace PebbleUtaProxy;

/// <summary>
/// Loads the GTFS schedule + one realtime snapshot, then prints a nearby
/// departures query. No web server, no listening socket.
/// </summary>
public static class SelfTest
{
    public static async Task RunAsync(string[] args)
    {
        var lat = args.Length > 1 && double.TryParse(args[1], out var la) ? la : 40.7660;
        var lon = args.Length > 2 && double.TryParse(args[2], out var lo) ? lo : -111.8910;

        var config = new ConfigurationBuilder()
            .AddJsonFile("appsettings.json", optional: true)
            .AddEnvironmentVariables()
            .Build();

        using var lf = LoggerFactory.Create(b => b.AddSimpleConsole(o => o.SingleLine = true)
            .SetMinimumLevel(LogLevel.Information));

        var httpFactory = new SimpleHttpClientFactory();
        var gtfs = new GtfsStore(httpFactory, config, lf.CreateLogger<GtfsStore>());
        var rt = new RealtimeStore(lf.CreateLogger<RealtimeStore>());
        var opt = config.GetSection(ProxyOptions.SectionName).Get<ProxyOptions>() ?? new ProxyOptions();

        Console.WriteLine("Loading GTFS schedule...");
        await gtfs.LoadAsync(CancellationToken.None);

        Console.WriteLine("Fetching realtime trip updates...");
        var http = httpFactory.CreateClient("feeds");
        rt.ApplyTripUpdates(await http.GetByteArrayAsync(opt.TripUpdatesUrl));

        var svc = new DeparturesService(gtfs, rt, config, lf.CreateLogger<DeparturesService>());
        var res = svc.QueryByLocation(lat, lon, 900, 4, 90);

        Console.WriteLine();
        Console.WriteLine($"Query {lat},{lon}  rt_age={res.RtAgeS}s  stops={res.Stops.Count}  alerts={res.Alerts.Count}");
        Console.WriteLine(JsonSerializer.Serialize(res, AppJson.Default.NearbyResponse));
    }

    private sealed class SimpleHttpClientFactory : IHttpClientFactory
    {
        public HttpClient CreateClient(string name) => new()
        {
            Timeout = TimeSpan.FromSeconds(30)
        };
    }
}
