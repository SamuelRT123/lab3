// publisher_pubsub_quic_like.c
// Publicador UDP compatible con broker/suscriptor QUIC-like (XOR, encabezado, ACK/NACK)

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

#define BUFFER_SIZE 1500
#define PORT 5928
#define IP_BROKER "127.0.0.1"

#define PROTO_VERSION 1
#define MAGIC 0x51554331u
#define MAX_PAYLOAD (BUFFER_SIZE - 64)
#define RECV_TIMEOUT_SEC 3
#define KEEPALIVE_SEC 10

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
    if ((size_t)n < sizeof(quic_like_header_t)) return -1;

    quic_like_header_t hdr;
    memcpy(&hdr, buffer, sizeof(hdr));

    uint32_t length = ntohl(hdr.length);
    if (sizeof(hdr) + length != (size_t)n || length > payload_cap) return -1;

    if (length > 0) {
        memcpy(payload_buf, buffer + sizeof(hdr), length);
        if (decrypt) xor_cipher(payload_buf, length, key);
    }

    out_hdr->type = hdr.type;
    out_hdr->stream_id = ntohl(hdr.stream_id);
    out_hdr->seq = be64toh(hdr.seq);
    out_hdr->length = length;
    return (int)length;
}

// === Handshake ===
static int do_handshake_get_key(int sock, const struct sockaddr_in *srv, unsigned char *out_key)
{
    const char hello[] = "HELLO_PUB";
    send_pkt(sock, srv, PKT_HELLO, 0, 0, 0, hello, strlen(hello), 0, 0);

    struct sockaddr_in from;
    quic_like_header_t hdr;
    char payload[BUFFER_SIZE];
    int r = recv_pkt(sock, &from, &hdr, payload, sizeof(payload), 0, 0);
    if (r < 0) {
        perror("recv HELLO_REPLY");
        return -1;
    }
    payload[r] = '\0';
    if (strncmp(payload, "KEY:", 4) != 0) return -1;
    long key = strtol(payload + 4, NULL, 10);
    *out_key = (unsigned char)key;
    return 0;
}

// === Main ===
int main(int argc, char **argv)
{
    const char *broker_ip = IP_BROKER;
    int broker_port = PORT;

    if (argc == 3) {
        broker_ip = argv[1];
        broker_port = atoi(argv[2]);
    }

    int sockfd;
    struct sockaddr_in server_addr;

    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        perror("socket");
        exit(EXIT_FAILURE);
    }

    struct timeval timeout = { RECV_TIMEOUT_SEC, 0 };
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(broker_port);
    server_addr.sin_addr.s_addr = inet_addr(broker_ip);

    unsigned char broker_key = 0;
    if (do_handshake_get_key(sockfd, &server_addr, &broker_key) != 0) {
        fprintf(stderr, "Handshake fallido\n");
        close(sockfd);
        return 1;
    }
    printf("Clave recibida del broker: %u\n", broker_key);

    char topic[128];
    printf("Tópico a publicar (ej: EquipoAvsB): ");
    fgets(topic, sizeof(topic), stdin);
    topic[strcspn(topic, "\n")] = 0;
    uint32_t stream_id = djb2_hash(topic);

    uint64_t seq = 1;
    char msg[512];
    while (1) {
        printf("Mensaje a publicar (o SALIR): ");
        if (!fgets(msg, sizeof(msg), stdin)) break;
        msg[strcspn(msg, "\n")] = 0;
        if (strcmp(msg, "SALIR") == 0) break;

        if (send_pkt(sockfd, &server_addr, PKT_DATA, 0, stream_id, seq, msg, strlen(msg), broker_key, 1) == 0)
            printf("Enviado seq=%llu\n", (unsigned long long)seq);
        seq++;
    }

    close(sockfd);
    return 0;
}
