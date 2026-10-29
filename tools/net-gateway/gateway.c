#include <enet/enet.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define API __declspec(dllexport)
#else
#define API __attribute__((visibility("default")))
#endif

typedef struct GatewayHost {
    ENetHost *host;
    ENetPeer *server;
    uint32_t next_id;
    size_t queued_bytes;
    int is_host;
} GatewayHost;

static void ENET_CALLBACK packet_freed(ENetPacket *packet) {
    GatewayHost *g = (GatewayHost *)packet->userData;
    g->queued_bytes -= packet->dataLength + 128;
}

API int gateway_initialize(void) { return enet_initialize(); }
API void gateway_deinitialize(void) { enet_deinitialize(); }

API GatewayHost *gateway_host(uint16_t port, int max_peers, int public_bind) {
    ENetAddress address = { ENET_HOST_ANY, port };
    if (!public_bind && enet_address_set_host_ip(&address, "127.0.0.1") != 0) return NULL;
    GatewayHost *g = (GatewayHost *)calloc(1, sizeof(*g));
    if (!g) return NULL;
    g->host = enet_host_create(&address, (size_t)max_peers, 3, 0, 0);
    if (!g->host) { free(g); return NULL; }
    g->host->maximumPacketSize = 65536;
    g->host->maximumWaitingData = 1048576;
    g->is_host = 1;
    g->next_id = 1;
    return g;
}

API GatewayHost *gateway_connect(const char *hostname, uint16_t port) {
    ENetAddress address = {0};
    if (enet_address_set_host_ip(&address, hostname) != 0) return NULL;
    address.port = port;
    GatewayHost *g = (GatewayHost *)calloc(1, sizeof(*g));
    if (!g) return NULL;
    g->host = enet_host_create(NULL, 1, 3, 0, 0);
    if (!g->host) { free(g); return NULL; }
    g->host->maximumPacketSize = 65536;
    g->host->maximumWaitingData = 1048576;
    g->server = enet_host_connect(g->host, &address, 3, 0);
    if (!g->server) { enet_host_destroy(g->host); free(g); return NULL; }
    return g;
}

/* Returns 1 for an event, 0 for none, -1 on service error. */
API int gateway_poll(GatewayHost *g, uint8_t *kind, uint8_t *channel,
                     uint8_t *flags, uint32_t *peer, uint8_t *data, int *length) {
    ENetEvent event;
    int result = enet_host_service(g->host, &event, 0);
    if (result <= 0) return result;
    *channel = 0; *flags = 0; *length = 0;
    if (event.type == ENET_EVENT_TYPE_CONNECT) {
        uint32_t id = g->is_host ? g->next_id++ : 0;
        if (g->is_host && id == 0) { enet_peer_disconnect_now(event.peer, 0); return 0; }
        event.peer->data = (void *)(uintptr_t)id;
        *peer = id; *kind = 1;
    } else if (event.type == ENET_EVENT_TYPE_DISCONNECT) {
        *peer = (uint32_t)(uintptr_t)event.peer->data;
        event.peer->data = NULL;
        if (event.peer == g->server) g->server = NULL;
        *kind = 2;
    } else if (event.type == ENET_EVENT_TYPE_RECEIVE) {
        *peer = (uint32_t)(uintptr_t)event.peer->data;
        *kind = 3; *channel = event.channelID;
        *flags = (event.packet->flags & ENET_PACKET_FLAG_RELIABLE) ? 1 : 0;
        if (event.packet->dataLength > 65536) { enet_packet_destroy(event.packet); return -1; }
        *length = (int)event.packet->dataLength;
        memcpy(data, event.packet->data, event.packet->dataLength);
        enet_packet_destroy(event.packet);
    } else return 0;
    return 1;
}

static ENetPeer *find_peer(GatewayHost *g, uint32_t id) {
    if (!g->is_host) return id == 0 ? g->server : NULL;
    for (size_t i = 0; i < g->host->peerCount; ++i) {
        ENetPeer *p = &g->host->peers[i];
        if (p->state == ENET_PEER_STATE_CONNECTED && (uint32_t)(uintptr_t)p->data == id && id != 0) return p;
    }
    return NULL;
}
API int gateway_send(GatewayHost *g, uint32_t id, uint8_t channel, int reliable, const uint8_t *data, int length) {
    ENetPeer *peer = find_peer(g, id);
    if (!peer || channel > 2 || length < 0 || length > 65536 ||
        g->queued_bytes + (size_t)length + 128 > 1048576) return -1;
    ENetPacket *packet = enet_packet_create(data, (size_t)length,
        reliable ? ENET_PACKET_FLAG_RELIABLE : ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT);
    if (!packet) return -1;
    g->queued_bytes += (size_t)length + 128;
    packet->userData = g;
    packet->freeCallback = packet_freed;
    if (enet_peer_send(peer, channel, packet) != 0) { enet_packet_destroy(packet); return -1; }
    return 0;
}
API int gateway_disconnect_peer(GatewayHost *g, uint32_t id) {
    ENetPeer *peer = find_peer(g, id);
    if (!peer) return -1;
    enet_peer_disconnect_later(peer, 0);
    return 0;
}
API void gateway_flush(GatewayHost *g) { enet_host_flush(g->host); }
API void gateway_destroy(GatewayHost *g) {
    if (!g) return;
    /* Match NetworkSubsystem: a host does not send immediate disconnects
       behind its final packets, which can cause ENet to discard them. */
    if (!g->is_host && g->server)
        enet_peer_disconnect_now(g->server, 0);
    enet_host_destroy(g->host);
    free(g);
}
