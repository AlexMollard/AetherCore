using System.Buffers.Binary;
using System.Net;
using System.Net.WebSockets;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Threading.Channels;

var builder = WebApplication.CreateBuilder(args);
var config = builder.Configuration;
string token = config["Gateway:Token"] ?? throw new InvalidOperationException("Gateway:Token is required");
if (string.IsNullOrWhiteSpace(token)) throw new InvalidOperationException("Gateway:Token is required");
string[] origins = (config["Gateway:Origins"] ?? "").Split(',', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries);
if (origins.Length == 0) throw new InvalidOperationException("Gateway:Origins is required");
bool allowPublic = bool.TryParse(config["Gateway:AllowPublic"], out bool publicValue) && publicValue;
string listen = config["Gateway:Listen"] ?? "http://127.0.0.1:8788";
if (!Uri.TryCreate(listen, UriKind.Absolute, out var listenUri) ||
    (listenUri.Scheme != "http" && listenUri.Scheme != "https") ||
    (!allowPublic && !(listenUri.Host.Equals("localhost", StringComparison.OrdinalIgnoreCase) ||
        (IPAddress.TryParse(listenUri.Host, out var ip) && IPAddress.IsLoopback(ip)))))
    throw new InvalidOperationException("Non-loopback listen requires Gateway:AllowPublic=true");
int minPort = int.Parse(config["Gateway:UdpMin"] ?? "27000");
int maxPort = int.Parse(config["Gateway:UdpMax"] ?? "27100");
if (minPort < 1 || maxPort > 65535 || minPort > maxPort) throw new InvalidOperationException("Invalid UDP port range");
var targets = new Dictionary<string, (string Host, ushort Port)>(StringComparer.Ordinal);
foreach (var entry in config.GetSection("Gateway:Targets").GetChildren())
{
    string host = entry["Host"] ?? throw new InvalidOperationException("Target host required");
    if (!ushort.TryParse(entry["Port"], out ushort port) || port == 0 || host.Length == 0)
        throw new InvalidOperationException("Invalid target " + entry.Key);
    var address = Dns.GetHostAddresses(host).FirstOrDefault(a => a.AddressFamily == System.Net.Sockets.AddressFamily.InterNetwork)
        ?? throw new InvalidOperationException("Target has no IPv4 address: " + entry.Key);
    targets.Add(entry.Key, (address.ToString(), port));
}
if (Native.Initialize() != 0) throw new InvalidOperationException("enet_initialize failed");
builder.WebHost.UseUrls(listen);
var app = builder.Build();
app.UseWebSockets();
var rooms = new Dictionary<string, ushort>(StringComparer.Ordinal);
var sync = new object();
var slots = new SemaphoreSlim(128);
app.Map("/net", async context =>
{
    if (!context.WebSockets.IsWebSocketRequest || !origins.Contains(context.Request.Headers.Origin.ToString(), StringComparer.Ordinal))
    { context.Response.StatusCode = 403; return; }
    if (!await slots.WaitAsync(0)) { context.Response.StatusCode = 503; return; }
    WebSocket? socket = null;
    try
    {
        socket = await context.WebSockets.AcceptWebSocketAsync();
        using var lifetime = CancellationTokenSource.CreateLinkedTokenSource(context.RequestAborted, app.Lifetime.ApplicationStopping);
        lifetime.CancelAfter(TimeSpan.FromSeconds(10));
        var first = await Protocol.Receive(socket, 4096, lifetime.Token);
        if (first.Type != WebSocketMessageType.Text) throw new InvalidDataException("First frame must be JSON");
        var open = JsonSerializer.Deserialize<OpenRequest>(first.Data, new JsonSerializerOptions { PropertyNameCaseInsensitive = false })
            ?? throw new InvalidDataException("Invalid handshake");
        if (open.version != 1 || (open.role != "host" && open.role != "client") || open.maxPeers is < 1 or > 64 ||
            open.port is < 0 or > 65535 || open.token is null || !SecretEquals(token, open.token) ||
            (open.room is { Length: > 64 }) || (open.target is { Length: > 64 }))
            throw new InvalidDataException("Invalid handshake");
        bool host = open.role == "host";
        bool hasRoom = !string.IsNullOrWhiteSpace(open.room);
        bool hasTarget = !string.IsNullOrWhiteSpace(open.target);
        if ((host && hasTarget) || (!host && hasRoom == hasTarget)) throw new InvalidDataException("Invalid room or target");
        IntPtr native = IntPtr.Zero;
        string? ownedRoom = null;
        try
        {
            if (host)
            {
                if (open.port != 0 && (open.port < minPort || open.port > maxPort)) throw new InvalidDataException("UDP port outside allowed range");
                lock (sync)
                {
                    if (hasRoom && rooms.ContainsKey(open.room!)) throw new InvalidDataException("Room exists");
                    int start = open.port == 0 ? minPort : open.port;
                    int end = open.port == 0 ? maxPort : open.port;
                    for (int p = start; p <= end; p++)
                    {
                        native = Native.Host((ushort)p, open.maxPeers, allowPublic ? 1 : 0);
                        if (native == IntPtr.Zero) continue;
                        if (hasRoom) { rooms.Add(open.room!, (ushort)p); ownedRoom = open.room; }
                        break;
                    }
                }
            }
            else
            {
                (string Host, ushort Port) endpoint;
                if (hasRoom)
                {
                    lock (sync)
                    {
                        if (!rooms.TryGetValue(open.room!, out ushort port)) throw new InvalidDataException("Room not found");
                        endpoint = ("127.0.0.1", port);
                        native = Native.Connect(endpoint.Host, endpoint.Port);
                    }
                }
                else
                {
                    if (!targets.TryGetValue(open.target!, out endpoint)) throw new InvalidDataException("Unknown target alias");
                    if (open.port != endpoint.Port) throw new InvalidDataException("Target port does not match configured alias");
                    native = Native.Connect(endpoint.Host, endpoint.Port);
                }
            }
            if (native == IntPtr.Zero) throw new InvalidDataException("ENet host creation failed");
            await socket.SendAsync(Encoding.UTF8.GetBytes("{\"version\":1,\"kind\":\"ready\"}"),
                WebSocketMessageType.Text, true, lifetime.Token);
            lifetime.CancelAfter(Timeout.InfiniteTimeSpan);
            await Protocol.Run(socket, native, lifetime, host);
        }
        finally
        {
            lock (sync)
            {
                if (ownedRoom is not null) rooms.Remove(ownedRoom);
                if (native != IntPtr.Zero) Native.Destroy(native);
            }
        }
    }
    catch (Exception e) when (e is InvalidDataException or WebSocketException or OperationCanceledException or JsonException or IOException)
    {
        app.Logger.LogDebug("Gateway session closed: {Reason}", e.Message);
        if (socket?.State == WebSocketState.Open)
        {
            using var closeTimeout = new CancellationTokenSource(TimeSpan.FromSeconds(1));
            try { await socket.CloseOutputAsync(WebSocketCloseStatus.PolicyViolation, "Gateway session rejected or closed", closeTimeout.Token); }
            catch (Exception closeError) when (closeError is WebSocketException or OperationCanceledException) { }
        }
    }
    finally { socket?.Dispose(); slots.Release(); }
});
try { await app.RunAsync(); }
finally { Native.Deinitialize(); }

static bool SecretEquals(string expected, string supplied)
{
    byte[] a = SHA256.HashData(Encoding.UTF8.GetBytes(expected));
    byte[] b = SHA256.HashData(Encoding.UTF8.GetBytes(supplied));
    return CryptographicOperations.FixedTimeEquals(a, b);
}

sealed class OpenRequest
{
    public int version { get; set; }
    public string? role { get; set; }
    public int port { get; set; }
    public int maxPeers { get; set; }
    public string? room { get; set; }
    public string? target { get; set; }
    public string? token { get; set; }
}

static class Protocol
{
    const int MaxPayload = 65536;
    const int MaxMessage = MaxPayload + 8;
    // Sixteen maximum-sized messages per direction give each queue a 1 MiB payload budget.
    static Channel<byte[]> Queue() => Channel.CreateBounded<byte[]>(new BoundedChannelOptions(15)
    { SingleReader = true, SingleWriter = true, FullMode = BoundedChannelFullMode.Wait });

    public static async Task<(WebSocketMessageType Type, byte[] Data)> Receive(WebSocket socket, int limit, CancellationToken ct)
    {
        byte[] buffer = new byte[limit + 1];
        int count = 0;
        WebSocketReceiveResult result;
        do
        {
            result = await socket.ReceiveAsync(new ArraySegment<byte>(buffer, count, buffer.Length - count), ct);
            if (result.MessageType == WebSocketMessageType.Close) throw new WebSocketException("Peer closed");
            count += result.Count;
            if (count > limit || (count == buffer.Length && !result.EndOfMessage)) throw new InvalidDataException("Message too large");
        } while (!result.EndOfMessage);
        Array.Resize(ref buffer, count);
        return (result.MessageType, buffer);
    }

    public static async Task Run(WebSocket socket, IntPtr native, CancellationTokenSource lifetime, bool host)
    {
        var incoming = Queue(); var outgoing = Queue();
        CancellationToken ct = lifetime.Token;
        async Task ReadLoop()
        {
            try
            {
                while (true)
                {
                    var message = await Receive(socket, MaxMessage, ct);
                    if (message.Type != WebSocketMessageType.Binary) throw new InvalidDataException("Expected binary message");
                    await incoming.Writer.WriteAsync(message.Data, ct);
                }
            }
            catch (WebSocketException) { /* Drain frames already received before shutdown. */ }
            finally { incoming.Writer.TryComplete(); }
        }
        async Task WriteLoop()
        {
            try
            {
                await foreach (var message in outgoing.Reader.ReadAllAsync(ct))
                    await socket.SendAsync(message, WebSocketMessageType.Binary, true, ct);
            }
            finally { if (socket.State == WebSocketState.Open) lifetime.Cancel(); }
        }
        Task reader = ReadLoop(), writer = WriteLoop();
        byte[] payload = new byte[MaxPayload];
        bool connected = host;
        long connectStarted = System.Diagnostics.Stopwatch.GetTimestamp();
        try
        {
            while (!ct.IsCancellationRequested)
            {
                if (!connected && System.Diagnostics.Stopwatch.GetElapsedTime(connectStarted) >= TimeSpan.FromSeconds(15))
                    throw new InvalidDataException("ENet connection timed out");
                int budget = 64;
                while (budget-- > 0 && incoming.Reader.TryRead(out byte[]? message))
                {
                    if (message.Length < 8 || message[0] != 1 || message[2] > 2 || (message[3] & ~1) != 0)
                        throw new InvalidDataException("Invalid frame");
                    byte kind = message[1], channel = message[2], flags = message[3];
                    uint peer = BinaryPrimitives.ReadUInt32LittleEndian(message.AsSpan(4));
                    int length = message.Length - 8;
                    if (kind == 3)
                    {
                        if (Native.Send(native, peer, channel, flags & 1, message.AsSpan(8).ToArray(), length) != 0)
                            throw new InvalidDataException("Invalid peer or send failed");
                    }
                    else if (kind == 4 && length == 0)
                    {
                        if (Native.DisconnectPeer(native, peer) != 0) throw new InvalidDataException("Invalid peer");
                    }
                    else if (kind == 5 && length == 0)
                    {
                        Native.Flush(native);
                        return;
                    }
                    else throw new InvalidDataException("Invalid message kind");
                }
                if (incoming.Reader.Completion.IsCompleted)
                {
                    Native.Flush(native);
                    await reader; // Preserve non-close protocol failures after draining.
                    return;
                }
                for (int i = 0; i < 64; i++)
                {
                    int result = Native.Poll(native, out byte kind, out byte channel, out byte flags, out uint peer, payload, out int length);
                    if (result < 0) throw new InvalidDataException("ENet service failed");
                    if (result == 0) break;
                    if (channel > 2) throw new InvalidDataException("Invalid ENet channel");
                    if (kind == 1) connected = true;
                    byte[] frame = new byte[8 + length];
                    frame[0] = 1; frame[1] = kind; frame[2] = channel; frame[3] = flags;
                    BinaryPrimitives.WriteUInt32LittleEndian(frame.AsSpan(4), peer);
                    payload.AsSpan(0, length).CopyTo(frame.AsSpan(8));
                    await outgoing.Writer.WriteAsync(frame, ct);
                }
                Native.Flush(native);
                await Task.Delay(5, ct);
            }
        }
        finally
        {
            lifetime.Cancel();
            try { await Task.WhenAll(reader, writer); } catch (Exception e) when (e is OperationCanceledException or WebSocketException or InvalidDataException) { }
        }
    }
}

static class Native
{
    const string Library = "aether_net_gateway";
    [DllImport(Library, EntryPoint = "gateway_initialize", CallingConvention = CallingConvention.Cdecl)] public static extern int Initialize();
    [DllImport(Library, EntryPoint = "gateway_deinitialize", CallingConvention = CallingConvention.Cdecl)] public static extern void Deinitialize();
    [DllImport(Library, EntryPoint = "gateway_host", CallingConvention = CallingConvention.Cdecl)] public static extern IntPtr Host(ushort port, int peers, int publicBind);
    [DllImport(Library, EntryPoint = "gateway_connect", CallingConvention = CallingConvention.Cdecl)] public static extern IntPtr Connect([MarshalAs(UnmanagedType.LPUTF8Str)] string host, ushort port);
    [DllImport(Library, EntryPoint = "gateway_poll", CallingConvention = CallingConvention.Cdecl)] public static extern int Poll(IntPtr host, out byte kind, out byte channel, out byte flags, out uint peer, [Out] byte[] payload, out int length);
    [DllImport(Library, EntryPoint = "gateway_send", CallingConvention = CallingConvention.Cdecl)] public static extern int Send(IntPtr host, uint peer, byte channel, int reliable, byte[] data, int length);
    [DllImport(Library, EntryPoint = "gateway_disconnect_peer", CallingConvention = CallingConvention.Cdecl)] public static extern int DisconnectPeer(IntPtr host, uint peer);
    [DllImport(Library, EntryPoint = "gateway_flush", CallingConvention = CallingConvention.Cdecl)] public static extern void Flush(IntPtr host);
    [DllImport(Library, EntryPoint = "gateway_destroy", CallingConvention = CallingConvention.Cdecl)] public static extern void Destroy(IntPtr host);
}
