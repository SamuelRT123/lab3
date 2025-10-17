#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <errno.h>

#define BUFFER_SIZE 1500
#define PORT 5928
#define IP_BROKER "127.0.0.1"

#define PROTO_VERSION 1
#define MAGIC 0x51554331u
#define MAX_PAYLOAD (BUFFER_SIZE - 64)
#define RECV_TIMEOUT_SEC 5

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

static uint32_t djb2_hash(const char *s) {
    uint32_t h = 5381u;
    int c;
    while ((c = (unsigned char)*s++)) h = ((h << 5) + h) + (uint32_t)c;
    if (h == 0) h = 1;
    return h;
}

static void xor_cipher(char *data, size_t len, unsigned char key) {
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
    ssize_t n = recvfrom(sock, buffer, sizeof(buffer), 0, (struct sockaddr*)from, &flen);
    if (n < 0) return -1;
    if ((size_t)n < sizeof(quic_like_header_t)) return -1;

    quic_like_header_t hdr;
    memcpy(&hdr, buffer, sizeof(hdr));
    if (ntohl(hdr.magic) != MAGIC || hdr.version != PROTO_VERSION) return -1;

    uint32_t length = ntohl(hdr.length);
    if (sizeof(hdr) + length != (size_t)n || length > payload_cap) return -1;

    if (length > 0) {
        memcpy(payload_buf, buffer + sizeof(hdr), length);
        if (decrypt) xor_cipher(payload_buf, length, key);
    }

    out_hdr->type = hdr.type;
    out_hdr->flags = hdr.flags;
    out_hdr->stream_id = ntohl(hdr.stream_id);
    out_hdr->seq = be64toh(hdr.seq);
    out_hdr->length = length;
    return (int)length;
}

// === Handshake: obtener clave XOR del broker ===
static int do_handshake_get_key(int sock, const struct sockaddr_in *srv, unsigned char *out_key)
{
    const char hello[] = "HELLO_SUB";
    if (send_pkt(sock, srv, PKT_HELLO, 0, 0, 0, hello, (uint32_t)strlen(hello), 0, 0) != 0)
        return -1;

    struct sockaddr_in from;
    quic_like_header_t hdr;
    char payload[BUFFER_SIZE];
    int r = recv_pkt(sock, &from, &hdr, payload, sizeof(payload), 0, 0);
    if (r < 0 || hdr.type != PKT_HELLO_REPLY) return -1;

    payload[r] = '\0';
    if (strncmp(payload, "KEY:", 4) != 0) return -1;
    long key = strtol(payload + 4, NULL, 10);
    if (key <= 0 || key > 255) return -1;
    *out_key = (unsigned char)key;
    return 0;
}

int main(int argc, char **argv)
{
    const char *broker_ip = IP_BROKER;
    int broker_port = PORT;
    if (argc == 3) { broker_ip = argv[1]; broker_port = atoi(argv[2]); }

    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) { perror("socket"); return 1; }

    // timeout de lectura
    struct timeval tv = { RECV_TIMEOUT_SEC, 0 };
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in srv;
    memset(&srv, 0, sizeof(srv));
    srv.sin_family = AF_INET;
    srv.sin_port = htons(broker_port);
    srv.sin_addr.s_addr = inet_addr(broker_ip);

    // 1) handshake -> KEY:n
    unsigned char key = 0;
    if (do_handshake_get_key(sockfd, &srv, &key) != 0) {
        fprintf(stderr, "Handshake fallido\n");
        return 1;
    }
    printf("Clave recibida: %u\n", key);

    // 2) pedir tópico y suscribirse (SUB:<stream_id>) cifrado
    char topic[128];
    printf("Tema a suscribirse (ej: EquipoAvsB): ");
    if (!fgets(topic, sizeof(topic), stdin)) return 0;
    topic[strcspn(topic, "\n")] = 0;
    uint32_t stream_id = djb2_hash(topic);

    char submsg[64];
    snprintf(submsg, sizeof(submsg), "SUB:%u", stream_id);
    if (send_pkt(sockfd, &srv, PKT_SUBSCRIBE, 0, stream_id, 0,
                 submsg, (uint32_t)strlen(submsg), key, 1) != 0) {
        fprintf(stderr, "Error enviando SUBSCRIBE\n");
        return 1;
    }
    printf("Suscrito a stream_id=%u\n", stream_id);

    // 3) recibir DATA + NACK en caso de huecos
    uint64_t next_expected = 1;

    for (;;) {
        struct sockaddr_in from;
        quic_like_header_t hdr;
        char payload[MAX_PAYLOAD];
        int r = recv_pkt(sockfd, &from, &hdr, payload, sizeof(payload), key,
                         /*decrypt*/ (hdr.type == PKT_DATA || hdr.type == PKT_ACK ||
                                      hdr.type == PKT_NACK || hdr.type == PKT_PING || hdr.type == PKT_PONG));
        if (r < 0) {
            if (errno == EWOULDBLOCK || errno == EAGAIN) continue;
            perror("recv");
            continue;
        }

        switch (hdr.type) {
            case PKT_DATA: {
                // manejar huecos
                if (hdr.seq > next_expected) {
                    // pedir retransmisión
                    char nack[64];
                    snprintf(nack, sizeof(nack), "NACK:%llu-%llu",
                             (unsigned long long)next_expected,
                             (unsigned long long)(hdr.seq - 1));
                    (void)send_pkt(sockfd, &srv, PKT_NACK, 0, hdr.stream_id, 0,
                                   nack, (uint32_t)strlen(nack), key, 1);
                }
                // mostrar mensaje
                printf("[seq=%llu] %.*s\n",
                       (unsigned long long)hdr.seq, r, payload);
                next_expected = hdr.seq + 1;

                // (opcional) mandar ACK
                // char ack[32]; snprintf(ack, sizeof(ack), "ACK:%llu", (unsigned long long)hdr.seq);
                // (void)send_pkt(sockfd, &srv, PKT_ACK, 0, hdr.stream_id, hdr.seq, ack, (uint32_t)strlen(ack), key, 1);
                break;
            }
            case PKT_ACK:
                // opcional: stats
                break;
            case PKT_PING: {
                const char pong[] = "PONG";
                (void)send_pkt(sockfd, &srv, PKT_PONG, 0, 0, 0, pong, (uint32_t)strlen(pong), key, 1);
                break;
            }
            default:
                // ignora otros
                break;
        }
    }

    close(sockfd);
    return 0;
}
