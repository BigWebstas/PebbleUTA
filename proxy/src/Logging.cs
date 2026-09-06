using System.Collections.Concurrent;
using System.Globalization;
using System.Net;
using System.Net.Sockets;
using System.Text;

namespace PebbleUtaProxy;

// File + syslog logging for the proxy. Both outputs are optional and driven by
// the "Logging:File" / "Logging:Syslog" sections of appsettings.json. A single
// background thread drains a bounded queue to both sinks, so nothing on the
// request path ever blocks on disk or the network.

public sealed class FileLogOptions
{
    /// <summary>Write a daily log file. Default true.</summary>
    public bool Enabled { get; set; } = true;

    /// <summary>Folder for the log files. Relative paths are next to the exe.</summary>
    public string Directory { get; set; } = "logs";

    /// <summary>Delete files older than this many days. 0 = keep forever.</summary>
    public int RetainedDays { get; set; } = 14;

    /// <summary>File name prefix; the date and ".log" are appended.</summary>
    public string FilePrefix { get; set; } = "proxy-";
}

public enum SyslogProtocol { Udp, Tcp }

public enum SyslogFormat { Rfc5424, Rfc3164 }

public sealed class SyslogOptions
{
    /// <summary>Forward log lines to a syslog server. Default false.</summary>
    public bool Enabled { get; set; }

    /// <summary>Syslog server IP address or host name.</summary>
    public string Host { get; set; } = "";

    /// <summary>Syslog server port. 514 is the standard.</summary>
    public int Port { get; set; } = 514;

    public SyslogProtocol Protocol { get; set; } = SyslogProtocol.Udp;

    public SyslogFormat Format { get; set; } = SyslogFormat.Rfc5424;

    /// <summary>Syslog facility number. 16 = local0.</summary>
    public int Facility { get; set; } = 16;

    /// <summary>APP-NAME / tag reported to the server.</summary>
    public string AppName { get; set; } = "PebbleUtaProxy";
}

internal readonly record struct LogEntry(
    DateTimeOffset Time, LogLevel Level, string Category, int EventId,
    string Message, string? Exception);

internal static class Severity
{
    // Console-style 3-letter label for the file.
    public static string Label(LogLevel l) => l switch
    {
        LogLevel.Trace => "TRC",
        LogLevel.Debug => "DBG",
        LogLevel.Information => "INF",
        LogLevel.Warning => "WRN",
        LogLevel.Error => "ERR",
        LogLevel.Critical => "CRT",
        _ => "OFF",
    };

    // RFC 5424 severity number.
    public static int Number(LogLevel l) => l switch
    {
        LogLevel.Critical => 2,
        LogLevel.Error => 3,
        LogLevel.Warning => 4,
        LogLevel.Information => 6,
        _ => 7,
    };
}

public sealed class PebbleUtaLoggerProvider : ILoggerProvider
{
    private readonly BlockingCollection<LogEntry> _queue = new(8192);
    private readonly Thread _worker;
    private readonly FileSink? _file;
    private readonly SyslogSink? _syslog;
    private long _dropped;

    public PebbleUtaLoggerProvider(FileLogOptions file, SyslogOptions syslog)
    {
        if (file.Enabled)
            _file = new FileSink(file);
        if (syslog.Enabled && !string.IsNullOrWhiteSpace(syslog.Host))
            _syslog = new SyslogSink(syslog);

        _worker = new Thread(Run) { IsBackground = true, Name = "pebbleuta-log" };
        _worker.Start();
    }

    /// <summary>True when at least one sink is configured.</summary>
    public bool Active => _file is not null || _syslog is not null;

    public ILogger CreateLogger(string categoryName) => new Writer(this, categoryName);

    internal void Enqueue(in LogEntry e)
    {
        if (_queue.IsAddingCompleted || !_queue.TryAdd(e))
            Interlocked.Increment(ref _dropped);
    }

    private void Run()
    {
        foreach (var e in _queue.GetConsumingEnumerable())
        {
            try { _file?.Write(e); } catch { /* never let logging crash the service */ }
            try { _syslog?.Send(e); } catch { }
        }
    }

    public void Dispose()
    {
        _queue.CompleteAdding();
        try { _worker.Join(TimeSpan.FromSeconds(3)); } catch { }
        var dropped = Interlocked.Read(ref _dropped);
        if (dropped > 0 && _file is not null)
            try { _file.Write(new LogEntry(DateTimeOffset.Now, LogLevel.Warning,
                "PebbleUtaProxy.Logging", 0, $"{dropped} log line(s) dropped (queue full)", null)); }
            catch { }
        _file?.Dispose();
        _syslog?.Dispose();
        _queue.Dispose();
    }

    private sealed class Writer(PebbleUtaLoggerProvider provider, string category) : ILogger
    {
        public IDisposable? BeginScope<TState>(TState state) where TState : notnull => null;

        // Level filtering is already applied by the logging framework
        // (Logging:LogLevel) before the call reaches here.
        public bool IsEnabled(LogLevel logLevel) => logLevel != LogLevel.None;

        public void Log<TState>(LogLevel logLevel, EventId eventId, TState state,
            Exception? exception, Func<TState, Exception?, string> formatter)
        {
            if (logLevel == LogLevel.None)
                return;
            var message = formatter(state, exception);
            if (string.IsNullOrEmpty(message) && exception is null)
                return;
            provider.Enqueue(new LogEntry(
                DateTimeOffset.Now, logLevel, category, eventId.Id, message, exception?.ToString()));
        }
    }
}

internal sealed class FileSink : IDisposable
{
    private readonly string _dir;
    private readonly int _retainDays;
    private readonly string _prefix;
    private StreamWriter? _writer;
    private DateOnly _openFor;

    public FileSink(FileLogOptions o)
    {
        _dir = Path.IsPathRooted(o.Directory)
            ? o.Directory
            : Path.Combine(AppContext.BaseDirectory, o.Directory);
        _retainDays = o.RetainedDays;
        _prefix = string.IsNullOrWhiteSpace(o.FilePrefix) ? "proxy-" : o.FilePrefix;
        System.IO.Directory.CreateDirectory(_dir);
        Prune();
    }

    public void Write(in LogEntry e)
    {
        var day = DateOnly.FromDateTime(e.Time.LocalDateTime);
        if (_writer is null || day != _openFor)
        {
            _writer?.Dispose();
            var path = Path.Combine(_dir, $"{_prefix}{day:yyyy-MM-dd}.log");
            _writer = new StreamWriter(new FileStream(
                path, FileMode.Append, FileAccess.Write, FileShare.ReadWrite))
            { AutoFlush = true };
            _openFor = day;
            Prune();
        }

        _writer.Write(e.Time.ToString("yyyy-MM-dd HH:mm:ss.fff zzz", CultureInfo.InvariantCulture));
        _writer.Write(" [");
        _writer.Write(Severity.Label(e.Level));
        _writer.Write("] ");
        _writer.Write(e.Category);
        if (e.EventId != 0)
        {
            _writer.Write('[');
            _writer.Write(e.EventId);
            _writer.Write(']');
        }
        _writer.Write(": ");
        _writer.WriteLine(e.Message);
        if (e.Exception is not null)
            _writer.WriteLine(e.Exception);
    }

    private void Prune()
    {
        if (_retainDays <= 0)
            return;
        var cutoff = DateTime.Today.AddDays(-_retainDays);
        try
        {
            foreach (var f in System.IO.Directory.EnumerateFiles(_dir, $"{_prefix}*.log"))
            {
                var name = Path.GetFileNameWithoutExtension(f);
                if (name.Length >= 10 && DateTime.TryParseExact(
                        name[^10..], "yyyy-MM-dd", CultureInfo.InvariantCulture,
                        DateTimeStyles.None, out var d) && d.Date < cutoff)
                {
                    File.Delete(f);
                }
            }
        }
        catch { /* best effort */ }
    }

    public void Dispose() => _writer?.Dispose();
}

internal sealed class SyslogSink : IDisposable
{
    private const int MaxMessageBytes = 2000;
    private static readonly byte[] Bom = [0xEF, 0xBB, 0xBF];
    private static readonly byte[] Lf = [(byte)'\n'];

    private readonly SyslogOptions _o;
    private readonly string _host;
    private readonly int _pri0;      // facility * 8, add severity per message
    private readonly object _gate = new();
    private Socket? _sock;
    private EndPoint? _endpoint;

    public SyslogSink(SyslogOptions o)
    {
        _o = o;
        _host = Environment.MachineName;
        _pri0 = (o.Facility & 0x1F) * 8;
    }

    public void Send(in LogEntry e)
    {
        var text = _o.Format == SyslogFormat.Rfc3164 ? Format3164(e) : Format5424(e);
        var bytes = Encoding.UTF8.GetBytes(text);
        if (bytes.Length > MaxMessageBytes)
            Array.Resize(ref bytes, MaxMessageBytes);

        lock (_gate)
        {
            try
            {
                EnsureSocket();
                if (_sock is null)
                    return;
                if (_o.Protocol == SyslogProtocol.Tcp)
                {
                    _sock.Send(bytes);
                    _sock.Send(Lf);   // RFC 6587 non-transparent framing
                }
                else
                {
                    _sock.SendTo(bytes, _endpoint!);
                }
            }
            catch
            {
                // Drop this line and force a reconnect on the next one.
                _sock?.Dispose();
                _sock = null;
            }
        }
    }

    private void EnsureSocket()
    {
        if (_sock is not null)
            return;

        var ip = ResolveHost();
        _endpoint = new IPEndPoint(ip, _o.Port);

        if (_o.Protocol == SyslogProtocol.Tcp)
        {
            var s = new Socket(ip.AddressFamily, SocketType.Stream, ProtocolType.Tcp)
            { SendTimeout = 2000 };
            s.Connect(_endpoint);
            _sock = s;
        }
        else
        {
            _sock = new Socket(ip.AddressFamily, SocketType.Dgram, ProtocolType.Udp);
        }
    }

    private IPAddress ResolveHost() =>
        IPAddress.TryParse(_o.Host, out var literal)
            ? literal
            : Array.Find(Dns.GetHostAddresses(_o.Host),
                a => a.AddressFamily == AddressFamily.InterNetwork)
              ?? Dns.GetHostAddresses(_o.Host)[0];

    private string Format5424(in LogEntry e)
    {
        var pri = _pri0 + Severity.Number(e.Level);
        var ts = e.Time.ToString("yyyy-MM-ddTHH:mm:ss.ffK", CultureInfo.InvariantCulture);
        var msg = OneLine(e);
        // <PRI>1 TIMESTAMP HOST APP PROCID MSGID SD BOM MSG
        return $"<{pri}>1 {ts} {_host} {_o.AppName} {Environment.ProcessId} - - " +
               Encoding.UTF8.GetString(Bom) + msg;
    }

    private string Format3164(in LogEntry e)
    {
        var pri = _pri0 + Severity.Number(e.Level);
        var ts = e.Time.ToString("MMM", CultureInfo.InvariantCulture) + " " +
                 e.Time.Day.ToString(CultureInfo.InvariantCulture).PadLeft(2) + " " +
                 e.Time.ToString("HH:mm:ss", CultureInfo.InvariantCulture);
        // <PRI>TIMESTAMP HOST TAG[PID]: MSG
        return $"<{pri}>{ts} {_host} {_o.AppName}[{Environment.ProcessId}]: {OneLine(e)}";
    }

    private static string OneLine(in LogEntry e)
    {
        var sb = new StringBuilder();
        sb.Append(e.Category);
        if (e.EventId != 0)
            sb.Append('[').Append(e.EventId).Append(']');
        sb.Append(": ").Append(e.Message);
        if (e.Exception is not null)
            sb.Append(" | ").Append(e.Exception);
        return sb.ToString().Replace("\r", " ").Replace("\n", " ");
    }

    public void Dispose()
    {
        lock (_gate)
        {
            _sock?.Dispose();
            _sock = null;
        }
    }
}

public static class PebbleUtaLoggingExtensions
{
    /// <summary>
    /// Wire up the file + syslog log sinks from configuration. Reads
    /// "Logging:File" and "Logging:Syslog". A no-op when neither is enabled.
    /// </summary>
    public static ILoggingBuilder AddPebbleUtaSinks(
        this ILoggingBuilder builder, IConfiguration config)
    {
        var file = config.GetSection("Logging:File").Get<FileLogOptions>() ?? new FileLogOptions();
        var syslog = config.GetSection("Logging:Syslog").Get<SyslogOptions>() ?? new SyslogOptions();

        var provider = new PebbleUtaLoggerProvider(file, syslog);
        if (provider.Active)
            builder.AddProvider(provider);
        else
            provider.Dispose();

        return builder;
    }
}
