using System;
using System.Text;

namespace AetherCore;

/// <summary>A scene-owned connection to a dedicated application server, independent of Net replication.</summary>
/// <remarks>Call Connect from OnAttach or OnUpdate. It starts an asynchronous attempt; call Poll each update.
/// Browser connections use the published gateway configuration when gateway arguments are empty.
/// Messages contain at most 65536 UTF-8 bytes. Dispose in OnDetach; scene teardown also closes the socket.</remarks>
public sealed class ServerConnection : IDisposable
{
    private const int MaxMessageBytes = 64 * 1024;
    private static readonly UTF8Encoding Utf8 = new(false, true);
    private ulong _handle;
    private bool _disposed;
    private string _error = "";

    public bool Connect(string host, int port, string gatewayUrl = "", string gatewayToken = "", string targetAlias = "")
    {
        if (_disposed)
        {
            return false;
        }
        _error = "";
        if (_handle == 0)
        {
            _handle = Native.aether_server_connection_create();
        }
        if (_handle == 0)
        {
            _error = "Connection requires a live OnAttach/OnUpdate callback and an available connection slot (maximum 64).";
            return false;
        }
        return Native.aether_server_connection_connect(_handle, host ?? "", port,
            gatewayUrl ?? "", gatewayToken ?? "", targetAlias ?? "") != 0;
    }

    public void Poll()
    {
        if (!_disposed && _handle != 0)
        {
            Native.aether_server_connection_poll(_handle);
        }
    }

    public unsafe bool Send(string message, bool reliable = true)
    {
        if (_disposed)
        {
            return false;
        }
        _error = "";
        if (message is null || message.Length == 0 || message.Length > MaxMessageBytes || message.Contains('\0'))
        {
            _error = "Message must contain valid UTF-8 without NUL and be at most 65536 bytes.";
            return false;
        }
        byte[] bytes;
        try
        {
            if (Utf8.GetByteCount(message) > MaxMessageBytes)
            {
                _error = "Message exceeds 65536 UTF-8 bytes.";
                return false;
            }
            bytes = Utf8.GetBytes(message);
        }
        catch (EncoderFallbackException)
        {
            _error = "Message contains invalid Unicode.";
            return false;
        }
        if (_handle == 0)
        {
            _error = "Server is not connected.";
            return false;
        }
        fixed (byte* buffer = bytes)
        {
            return Native.aether_server_connection_send(_handle, buffer, bytes.Length, reliable ? 1 : 0) != 0;
        }
    }

    public unsafe bool TryReceive(out string message)
    {
        message = "";
        if (_disposed || _handle == 0)
        {
            return false;
        }
        int size = Native.aether_server_connection_receive(_handle, null, 0);
        if (size <= 0 || size > MaxMessageBytes)
        {
            return false;
        }
        byte[] bytes = new byte[size];
        fixed (byte* buffer = bytes)
        {
            if (Native.aether_server_connection_receive(_handle, buffer, bytes.Length) != size)
            {
                return false;
            }
        }
        message = Utf8.GetString(bytes);
        return true;
    }

    public void Disconnect()
    {
        if (!_disposed && _handle != 0)
        {
            Native.aether_server_connection_disconnect(_handle);
        }
    }

    public bool IsConnected => !_disposed && _handle != 0 && Native.aether_server_connection_is_connected(_handle) != 0;

    public unsafe string LastError
    {
        get
        {
            if (_disposed)
            {
                return "Server connection is disposed.";
            }
            if (_error.Length != 0 || _handle == 0)
            {
                return _error;
            }
            int size = Native.aether_server_connection_last_error(_handle, null, 0);
            if (size <= 0 || size > MaxMessageBytes)
            {
                return "";
            }
            byte[] bytes = new byte[size];
            fixed (byte* buffer = bytes)
            {
                int written = Native.aether_server_connection_last_error(_handle, buffer, bytes.Length);
                return written > 0 ? Utf8.GetString(bytes, 0, written) : "";
            }
        }
    }

    public void Dispose()
    {
        if (_disposed)
        {
            return;
        }
        if (_handle != 0)
        {
            Native.aether_server_connection_release(_handle);
            _handle = 0;
        }
        _disposed = true;
    }
}
