using System.Text.Json.Serialization;

namespace PebbleUtaProxy;

public sealed record ErrorDto(string Error);

public sealed record InfoDto(string Service, IReadOnlyList<string> Endpoints);

/// <summary>
/// Source-generated JSON metadata. Lets the app serialize without reflection,
/// which is what makes a trimmed / self-contained build work.
/// </summary>
[JsonSourceGenerationOptions(
    PropertyNamingPolicy = JsonKnownNamingPolicy.SnakeCaseLower,
    DefaultIgnoreCondition = JsonIgnoreCondition.WhenWritingNull)]
[JsonSerializable(typeof(InfoDto))]
[JsonSerializable(typeof(HealthResponse))]
[JsonSerializable(typeof(NearbyResponse))]
[JsonSerializable(typeof(DetailResponse))]
[JsonSerializable(typeof(ErrorDto))]
public partial class AppJson : JsonSerializerContext;
