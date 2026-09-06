namespace PebbleUtaProxy;

/// <summary>
/// Owns the feed lifecycle: load the GTFS schedule on startup, refresh it
/// periodically, and poll the GTFS-realtime trip updates on a short interval.
/// </summary>
public sealed class FeedWorker : BackgroundService
{
    private readonly GtfsStore _gtfs;
    private readonly RealtimeStore _rt;
    private readonly IHttpClientFactory _httpFactory;
    private readonly ProxyOptions _opt;
    private readonly ILogger<FeedWorker> _log;

    public FeedWorker(
        GtfsStore gtfs, RealtimeStore rt, IHttpClientFactory httpFactory,
        IConfiguration config, ILogger<FeedWorker> log)
    {
        _gtfs = gtfs;
        _rt = rt;
        _httpFactory = httpFactory;
        _opt = config.GetSection(ProxyOptions.SectionName).Get<ProxyOptions>() ?? new ProxyOptions();
        _log = log;
    }

    protected override async Task ExecuteAsync(CancellationToken ct)
    {
        await LoadStaticWithRetry(ct);

        var http = _httpFactory.CreateClient("feeds");
        var lastStaticRefresh = DateTimeOffset.UtcNow;
        var refreshEvery = TimeSpan.FromHours(Math.Max(1, _opt.StaticRefreshHours));
        var pollEvery = TimeSpan.FromSeconds(Math.Max(5, _opt.RtPollSeconds));

        while (!ct.IsCancellationRequested)
        {
            await PollFeed(http, _opt.TripUpdatesUrl, _rt.ApplyTripUpdates, "trip updates", ct);
            await PollFeed(http, _opt.VehiclePositionsUrl, _rt.ApplyVehicles, "vehicle positions", ct);
            await PollFeed(http, _opt.AlertsUrl, _rt.ApplyAlerts, "alerts", ct);
            if (ct.IsCancellationRequested) break;

            if (DateTimeOffset.UtcNow - lastStaticRefresh >= refreshEvery)
            {
                try
                {
                    await _gtfs.LoadAsync(ct);
                    lastStaticRefresh = DateTimeOffset.UtcNow;
                }
                catch (OperationCanceledException) { break; }
                catch (Exception ex)
                {
                    _log.LogWarning(ex, "Scheduled GTFS refresh failed; keeping previous copy");
                }
            }

            try { await Task.Delay(pollEvery, ct); }
            catch (OperationCanceledException) { break; }
        }
    }

    private async Task PollFeed(
        HttpClient http, string url, Action<byte[]> apply, string label, CancellationToken ct)
    {
        try
        {
            var bytes = await http.GetByteArrayAsync(url, ct);
            apply(bytes);
        }
        catch (OperationCanceledException) { /* shutting down */ }
        catch (Exception ex)
        {
            _log.LogWarning(ex, "{Label} poll failed", label);
        }
    }

    private async Task LoadStaticWithRetry(CancellationToken ct)
    {
        var attempt = 0;
        while (!ct.IsCancellationRequested)
        {
            try
            {
                await _gtfs.LoadAsync(ct);
                return;
            }
            catch (OperationCanceledException) { return; }
            catch (Exception ex)
            {
                attempt++;
                var wait = TimeSpan.FromSeconds(Math.Min(300, 15 * attempt));
                _log.LogError(ex, "GTFS load failed (attempt {Attempt}); retrying in {Wait}s",
                    attempt, wait.TotalSeconds);
                try { await Task.Delay(wait, ct); }
                catch (OperationCanceledException) { return; }
            }
        }
    }
}
