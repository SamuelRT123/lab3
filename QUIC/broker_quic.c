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
                               BROKER_KEY, 1);
                break;
            }
        }
    }
}

// Publicar a todos los clientes suscritos a stream_id
static void publish_to_subscribers(int sock, uint32_t stream_id,
                                   const char *msg, uint16_t len, uint8_t flags)
{
    stream_state_t *st = get_stream(stream_id);
    if (!st) return;

    uint64_t seq = st->next_seq++;
    // Guardar en buffer para posibles retransmisiones
    stream_store(st, seq, msg, len, flags);

    for (size_t i = 0; i < MAX_CLIENTS; ++i) {
        if (!clients[i].active) continue;
        if (!client_is_subscribed(&clients[i], stream_id)) continue;
        (void)send_pkt(sock, &clients[i].addr, PKT_DATA, flags, stream_id,
                       seq, msg, len, BROKER_KEY, 1);
    }
}

// ====== Broker main ======
int main(void)
{
    int sockfd;
    struct sockaddr_in srv;

    memset(clients, 0, sizeof(clients));
    memset(streams, 0, sizeof(streams));
    n_streams = 0;

    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        perror("socket");
        return 1;
    }

    int reuse = 1;
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        perror("setsockopt SO_REUSEADDR");
    }

    memset(&srv, 0, sizeof(srv));
    srv.sin_family = AF_INET;
    srv.sin_port = htons(PORT);
    srv.sin_addr.s_addr = inet_addr(IP_BIND);

    if (bind(sockfd, (struct sockaddr *)&srv, sizeof(srv)) < 0) {
        perror("bind");
        close(sockfd);
        return 1;
    }

    // Hacer stdin no bloqueante para poder usar select()
    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);

    printf("Broker escuchando en %s:%d (clave XOR=%d)\n", IP_BIND, PORT, BROKER_KEY);
    printf("Formato de publicación por stdin:  topic|mensaje\n");
    printf("Ejemplo:  EquipoAvsB|Gol minuto 45\n");

    fd_set rfds;
    for (;;) {
        FD_ZERO(&rfds);
        FD_SET(sockfd, &rfds);
        FD_SET(STDIN_FILENO, &rfds);
        int maxfd = (sockfd > STDIN_FILENO ? sockfd : STDIN_FILENO);

        struct timeval tv = { RECV_TIMEOUT_SEC, 0 };
        int rv = select(maxfd + 1, &rfds, NULL, NULL, &tv);
        if (rv < 0) {
            if (errno == EINTR) continue;
            perror("select");
            break;
        }

        // Entrada de red
        if (FD_ISSET(sockfd, &rfds)) {
            struct sockaddr_in from;
            quic_like_header_t hdr;
            char payload[MAX_PAYLOAD];

            // Intentamos recibir sin desencriptar primero para ver el tipo:
            int r = recv_pkt(sockfd, &from, &hdr, payload, sizeof(payload), 0, 0);
            if (r >= 0) {
                // Si es SUBSCRIBE/ACK/NACK/PKT_PING, el payload venía cifrado
                int needs_decrypt = (hdr.type == PKT_SUBSCRIBE ||
                                     hdr.type == PKT_ACK ||
                                     hdr.type == PKT_NACK ||
                                     hdr.type == PKT_PING);
                if (needs_decrypt) {
                    // volvemos a leer el mismo datagrama? no: ya lo consumimos.
                    // Solución: Interpretar con lo que tenemos: lo recibimos "en claro".
                    // Re-leemos correctamente: truco -> ya leímos, así que en vez de re-leer,
                    // repetimos proceso manual: como hicimos recv con decrypt=0,
                    // desencriptamos el buffer local (payload) aquí:
                    xor_cipher(payload, hdr.length, BROKER_KEY);
                }

                client_t *cl = get_client(&from);
                if (cl) cl->last_seen = time(NULL);

                switch (hdr.type) {
                    case PKT_HELLO: {
                        const char reply[16];
                        // Enviar PKT_HELLO_REPLY en claro con KEY:n
                        // No ciframos el payload del HELLO_REPLY:
                        char msg[32];
                        snprintf(msg, sizeof(msg), "KEY:%d", BROKER_KEY);
                        (void)send_pkt(sockfd, &from, PKT_HELLO_REPLY, 0, 0, 0,
                                       msg, (uint32_t)strlen(msg), 0, 0);
                        break;
                    }

                    case PKT_SUBSCRIBE: {
                        // payload: "SUB:<stream_id>"
                        payload[r] = '\0';
                        if (strncmp(payload, "SUB:", 4) == 0) {
                            uint32_t sid = (uint32_t)strtoul(payload + 4, NULL, 10);
                            (void)get_stream(sid);
                            if (client_subscribe(cl, sid) == 0) {
                                // ACK opcional de suscripción
                                const char ok[] = "SUB_OK";
                                (void)send_pkt(sockfd, &from, PKT_ACK, 0, sid, 0,
                                               ok, (uint32_t)strlen(ok), BROKER_KEY, 1);
                                fprintf(stderr, "Cliente suscrito a stream_id=%u\n", sid);
                            }
                        }
                        break;
                    }

                    case PKT_ACK: {
                        // payload: "ACK:<seq>"
                        // Podemos registrar stats o ignorar
                        break;
                    }

                    case PKT_NACK: {
                        // payload: "NACK:<from>-<to>"
                        payload[r] = '\0';
                        uint64_t a = 0, b = 0;
                        if (sscanf(payload, "NACK:%llu-%llu",
                                   (unsigned long long*)&a,
                                   (unsigned long long*)&b) == 2) {
                            // Reenviar rango al cliente para hdr.stream_id
                            stream_state_t *st = get_stream(hdr.stream_id);
                            resend_range(sockfd, cl, st, a, b);
                        }
                        break;
                    }

                    case PKT_PING: {
                        const char pong[] = "PONG";
                        (void)send_pkt(sockfd, &from, PKT_PONG, 0, 0, 0,
                                       pong, (uint32_t)strlen(pong), BROKER_KEY, 1);
                        break;
                    }

                    case PKT_PONG:
                        // No se espera desde el suscriptor
                        break;

                    case PKT_DATA:
                        // El broker no debería recibir DATA del suscriptor en este esquema
                        break;

                    default:
                        break;
                }
            }
        }

        // Entrada por stdin (publicación)
        if (FD_ISSET(STDIN_FILENO, &rfds)) {
            char line[2048];
            ssize_t n = read(STDIN_FILENO, line, sizeof(line) - 1);
            if (n > 0) {
                line[n] = '\0';
                // Quitar \n finales
                char *p = line;
                while (n > 0 && (line[n-1] == '\n' || line[n-1] == '\r')) {
                    line[n-1] = '\0';
                    n--;
                }
                if (n == 0) continue;

                // Formato: topic|mensaje
                char *bar = strchr(line, '|');
                if (!bar) {
                    // Si no hay '|', tomamos un topic por defecto "default"
                    const char *topic = "default";
                    uint32_t sid = djb2_hash(topic);
                    uint8_t flags = 0;
                    publish_to_subscribers(sockfd, sid, line, (uint16_t)strlen(line), flags);
                } else {
                    *bar = '\0';
                    const char *topic = line;
                    const char *msg = bar + 1;
                    uint32_t sid = djb2_hash(topic);
                    uint8_t flags = 0;
                    publish_to_subscribers(sockfd, sid, msg, (uint16_t)strlen(msg), flags);
                }
            }
        }

        // Mantenimiento
        purge_inactive_clients();
    }

    close(sockfd);
    return 0;
}
