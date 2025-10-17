// broker_pubsub_quic_like.c
// Broker UDP con encabezado "tipo QUIC", XOR básico, suscripciones y retransmisión (NACK/ACK)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <errno.h>
#include <time.h>
#include <fcntl.h>

#define BUFFER_SIZE 1500
#define PORT 5928
#define IP_BIND "0.0.0.0"

#define MAX_CLIENTS 256
#define MAX_STREAMS 512
#define MAX_PAYLOAD (BUFFER_SIZE - 64)
#define RECV_TIMEOUT_SEC 1
#define BROKER_KEY 173        // Clave XOR compartida (1..255)
#define HISTORY_DEPTH 512     // Cuántos mensajes por stream guardamos para retransmisión
#define KEEPALIVE_CLIENT_S 60 // Si no vemos a un cliente en tanto tiempo, lo purgamos

// ====== Protocolo ======
#define PROTO_VERSION 1
#define MAGIC 0x51554331u /* "QUC1" */

typedef enum {
    PKT_HELLO = 1,
    PKT_HELLO_REPLY = 2,
    PKT_SUBSCRIBE = 3,
    PKT_DATA = 4,
    PKT_ACK = 5,
    PKT_NACK = 6,
    PKT_PING = 7,
    PKT_PONG = 8
} pkt_type_t;

#define F_END_STREAM 0x01

#pragma pack(push, 1)
typedef struct {
    uint32_t magic;
    uint8_t  version;
    uint8_t  type;
    uint8_t  flags;
    uint8_t  reserved;
    uint32_t stream_id;
    uint64_t seq;
    uint32_t length;
} quic_like_header_t;
#pragma pack(pop)

// ====== Utilidades ======
static uint32_t djb2_hash(const char *s) {
    uint32_t h = 5381u;
    int c;
    while ((c = (unsigned char)*s++))
        h = ((h << 5) + h) + (uint32_t)c;
    if (h == 0) h = 1;
    return h;
}

void xor_cipher(char *data, size_t len, unsigned char key) {
    for (size_t i = 0; i < len; i++) data[i] ^= key;
}

static int send_pkt(int sock, const struct sockaddr_in *addr,
                    pkt_type_t type, uint8_t flags, uint32_t stream_id,
                    uint64_t seq, const void *payload, uint32_t length,
                    unsigned char key, int encrypt)
{
    if (length > MAX_PAYLOAD) return -1;

    char buffer[BUFFER_SIZE];
    quic_like_header_t hdr;

    hdr.magic = htonl(MAGIC);
    hdr.version = PROTO_VERSION;
    hdr.type = (uint8_t)type;
    hdr.flags = flags;
    hdr.reserved = 0;
    hdr.stream_id = htonl(stream_id);
    hdr.seq = htobe64(seq);
    hdr.length = htonl(length);

    memcpy(buffer, &hdr, sizeof(hdr));
    if (length > 0 && payload) {
        memcpy(buffer + sizeof(hdr), payload, length);
        if (encrypt) xor_cipher(buffer + sizeof(hdr), length, key);
    }

    size_t total = sizeof(hdr) + length;
    ssize_t sent = sendto(sock, buffer, total, 0,
                          (const struct sockaddr*)addr, sizeof(*addr));
    return (sent == (ssize_t)total) ? 0 : -1;
}

static int recv_pkt(int sock, struct sockaddr_in *from, quic_like_header_t *out_hdr,
                    char *payload_buf, size_t payload_cap, unsigned char key, int decrypt)
{
    char buffer[BUFFER_SIZE];
    socklen_t flen = sizeof(*from);
    ssize_t n = recvfrom(sock, buffer, sizeof(buffer), 0,
                         (struct sockaddr*)from, &flen);
    if (n < 0) return -1;
    if ((size_t)n < sizeof(quic_like_header_t)) {
        errno = EPROTO;
        return -1;
    }

    quic_like_header_t hdr;
    memcpy(&hdr, buffer, sizeof(hdr));

    if (ntohl(hdr.magic) != MAGIC || hdr.version != PROTO_VERSION) {
        errno = EPROTO;
        return -1;
    }

    uint32_t length = ntohl(hdr.length);
    if (sizeof(hdr) + length != (size_t)n || length > payload_cap) {
        errno = EMSGSIZE;
        return -1;
    }

    if (length > 0) {
        memcpy(payload_buf, buffer + sizeof(hdr), length);
        if (decrypt) xor_cipher(payload_buf, length, key);
    }

    out_hdr->magic = MAGIC;
    out_hdr->version = hdr.version;
    out_hdr->type = hdr.type;
    out_hdr->flags = hdr.flags;
    out_hdr->reserved = 0;
    out_hdr->stream_id = ntohl(hdr.stream_id);
    out_hdr->seq = be64toh(hdr.seq);
    out_hdr->length = length;

    return (int)length;
}

// ====== Modelo de datos ======
typedef struct {
    uint64_t seq;
    uint32_t stream_id;
    uint16_t len;
    uint8_t  flags;
    char     data[MAX_PAYLOAD];
} msg_record_t;

typedef struct {
    uint32_t stream_id;
    uint64_t next_seq;            // siguiente seq a emitir
    msg_record_t history[HISTORY_DEPTH];
    size_t head;                  // apunta al próximo slot donde escribir
    size_t size;                  // cuántos válidos hay (<= HISTORY_DEPTH)
} stream_state_t;

typedef struct {
    struct sockaddr_in addr;
    uint32_t streams[8];          // hasta 8 suscripciones por cliente (simple)
    size_t   n_streams;
    time_t   last_seen;
    int      active;
} client_t;

static client_t clients[MAX_CLIENTS];
static stream_state_t streams[MAX_STREAMS];
static size_t n_streams = 0;

// Buscar/crear stream
static stream_state_t* get_stream(uint32_t sid) {
    for (size_t i = 0; i < n_streams; ++i) {
        if (streams[i].stream_id == sid) return &streams[i];
    }
    if (n_streams >= MAX_STREAMS) return NULL;
    streams[n_streams].stream_id = sid;
    streams[n_streams].next_seq = 1;
    streams[n_streams].head = 0;
    streams[n_streams].size = 0;
    return &streams[n_streams++];
}

// Comparar sockaddr_in (IP:PUERTO)
static int same_addr(const struct sockaddr_in *a, const struct sockaddr_in *b) {
    return a->sin_family == b->sin_family &&
           a->sin_port == b->sin_port &&
           a->sin_addr.s_addr == b->sin_addr.s_addr;
}

// Buscar/crear cliente
static client_t* get_client(const struct sockaddr_in *addr) {
    for (size_t i = 0; i < MAX_CLIENTS; ++i) {
        if (clients[i].active && same_addr(&clients[i].addr, addr)) {
            return &clients[i];
        }
    }
    for (size_t i = 0; i < MAX_CLIENTS; ++i) {
        if (!clients[i].active) {
            clients[i].active = 1;
            clients[i].addr = *addr;
            clients[i].n_streams = 0;
            clients[i].last_seen = time(NULL);
            return &clients[i];
        }
    }
    return NULL;
}

static int client_subscribe(client_t *cl, uint32_t sid) {
    if (!cl) return -1;
    for (size_t i = 0; i < cl->n_streams; ++i) {
        if (cl->streams[i] == sid) return 0; // ya suscrito
    }
    if (cl->n_streams >= 8) return -1;
    cl->streams[cl->n_streams++] = sid;
    cl->last_seen = time(NULL);
    return 0;
}

static int client_is_subscribed(const client_t *cl, uint32_t sid) {
    for (size_t i = 0; i < cl->n_streams; ++i)
        if (cl->streams[i] == sid) return 1;
    return 0;
}

static void purge_inactive_clients(void) {
    time_t now = time(NULL);
    for (size_t i = 0; i < MAX_CLIENTS; ++i) {
        if (clients[i].active && (now - clients[i].last_seen > KEEPALIVE_CLIENT_S)) {
            clients[i].active = 0;
        }
    }
}

// Guardar mensaje en historial del stream
static void stream_store(stream_state_t *st, uint64_t seq,
                         const char *data, uint16_t len, uint8_t flags)
{
    size_t idx = st->head;
    st->history[idx].seq = seq;
    st->history[idx].stream_id = st->stream_id;
    st->history[idx].len = len;
    st->history[idx].flags = flags;
    memcpy(st->history[idx].data, data, len);

    st->head = (st->head + 1) % HISTORY_DEPTH;
    if (st->size < HISTORY_DEPTH) st->size++;
}

// Reenviar rango [from,to] a un cliente si está en buffer
static void resend_range(int sock, client_t *cl, stream_state_t *st,
                         uint64_t from_seq, uint64_t to_seq)
{
    if (!st || !cl) return;
    // Buscar mínimos y máximos en buffer
    if (st->size == 0) return;
    // Reconstruir ventana de historial
    // El más antiguo está en (head - size + HISTORY_DEPTH) % HISTORY_DEPTH
    size_t oldest = (st->head + HISTORY_DEPTH - st->size) % HISTORY_DEPTH;
    uint64_t min_seq = st->history[oldest].seq;
    uint64_t max_seq = st->history[(st->head + HISTORY_DEPTH - 1) % HISTORY_DEPTH].seq;

    if (to_seq < min_seq || from_seq > max_seq) {
        // Fuera de ventana; no podemos reenviar
        return;
    }

    if (from_seq < min_seq) from_seq = min_seq;
    if (to_seq > max_seq) to_seq = max_seq;

    for (uint64_t s = from_seq; s <= to_seq; ++s) {
        // Buscar secuencia s en el buffer linealmente (HISTORY_DEPTH acotado)
        for (size_t i = 0; i < st->size; ++i) {
            size_t idx = (oldest + i) % HISTORY_DEPTH;
            if (st->history[idx].seq == s) {
                (void)send_pkt(sock, &cl->addr, PKT_DATA,
                               st->history[idx].flags,
                               st->history[idx].stream_id,
                               st->history[idx].seq,
                               st->history[idx].data,
                               st->history[idx].len,
