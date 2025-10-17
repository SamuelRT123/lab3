// suscriptor_pubsub_quic_like.c
// Suscriptor UDP con encabezado "tipo QUIC", XOR básico y retransmisión (NACK/ACK)

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

// ====== Config ======
#define PROTO_VERSION 1
#define MAGIC 0x51554331u /* "QUC1" */
#define MAX_PAYLOAD (BUFFER_SIZE - 64)
#define RECV_TIMEOUT_SEC 3
#define KEEPALIVE_SEC 10

// ====== Tipos de paquete (muy simples) ======
typedef enum {
    PKT_HELLO = 1,       // Cliente -> Broker: "HELLO_SUB"
    PKT_HELLO_REPLY = 2, // Broker -> Cliente: "KEY:<num>"
    PKT_SUBSCRIBE = 3,   // Cliente -> Broker: subscribe(topic_hash)
    PKT_DATA = 4,        // Broker -> Cliente: datos de aplicación
    PKT_ACK = 5,         // Cliente -> Broker: ack(seq)
    PKT_NACK = 6,        // Cliente -> Broker: nack rango [from,to]
    PKT_PING = 7,        // Cliente -> Broker: keepalive
    PKT_PONG = 8         // Broker -> Cliente: respuesta keepalive
} pkt_type_t;

// ====== Flags básicos ======
#define F_END_STREAM 0x01

// ====== Encabezado inspirado en QUIC (compacto) ======
#pragma pack(push, 1)
typedef struct {
    uint32_t magic;        // "QUC1"
    uint8_t  version;      // PROTO_VERSION
    uint8_t  type;         // pkt_type_t
    uint8_t  flags;        // F_END_STREAM etc.
    uint8_t  reserved;     // padding
    uint32_t stream_id;    // hash del tópico (tópico = flujo)
    uint64_t seq;          // número de secuencia del DATA
    uint32_t length;       // bytes válidos en payload (después de XOR si aplica)
} quic_like_header_t;
#pragma pack(pop)

// ====== Utilidades ======
static uint32_t djb2_hash(const char *s) {
    uint32_t h = 5381u;
    int c;
    while ((c = (unsigned char)*s++))
        h = ((h << 5) + h) + (uint32_t)c;
    if (h == 0) h = 1; // evitar 0
    return h;
}

// Cifrar / descifrar con XOR (in-place)
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

    // Normalizamos endianness para el caller:
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

// ====== Handshake: obtiene la clave del broker ======
static int do_handshake_get_key(int sock, const struct sockaddr_in *srv, unsigned char *out_key)
{
    const char hello[] = "HELLO_SUB";
    if (send_pkt(sock, srv, PKT_HELLO, 0, 0, 0, hello, (uint32_t)strlen(hello), 0, 0) != 0) {
        perror("send HELLO");
        return -1;
    }

    struct sockaddr_in from;
    quic_like_header_t hdr;
    char payload[BUFFER_SIZE];
    int r = recv_pkt(sock, &from, &hdr, payload, sizeof(payload), 0, 0);
    if (r < 0) {
        perror("recv HELLO_REPLY");
        return -1;
    }
    if (hdr.type != PKT_HELLO_REPLY) {
        fprintf(stderr, "Handshake inesperado: tipo=%u\n", hdr.type);
        return -1;
    }

    payload[r] = '\0';
    // Esperamos "KEY:<number>"
    if (strncmp(payload, "KEY:", 4) != 0) {
        fprintf(stderr, "Formato de clave invalido: %s\n", payload);
        return -1;
    }
    long key = strtol(payload + 4, NULL, 10);
    if (key < 1 || key > 255) {
        fprintf(stderr, "Clave fuera de rango (1..255)\n");
        return -1;
    }
    *out_key = (unsigned char)key;
    return 0;
}

// ====== Suscripción por tópico ======
static int send_subscribe(int sock, const struct sockaddr_in *srv,
                          uint32_t stream_id, unsigned char key)
{
    char submsg[64];
    snprintf(submsg, sizeof(submsg), "SUB:%u", stream_id);
    return send_pkt(sock, srv, PKT_SUBSCRIBE, 0, stream_id, 0, submsg, (uint32_t)strlen(submsg), key, 1);
}

// ====== ACK / NACK ======
static int send_ack(int sock, const struct sockaddr_in *srv,
                    uint32_t stream_id, uint64_t seq, unsigned char key)
{
    char msg[64];
    int n = snprintf(msg, sizeof(msg), "ACK:%llu", (unsigned long long)seq);
    return send_pkt(sock, srv, PKT_ACK, 0, stream_id, seq, msg, (uint32_t)n, key, 1);
}

static int send_nack(int sock, const struct sockaddr_in *srv,
                     uint32_t stream_id, uint64_t from_seq, uint64_t to_seq, unsigned char key)
{
    char msg[96];
    int n = snprintf(msg, sizeof(msg), "NACK:%llu-%llu",
                     (unsigned long long)from_seq, (unsigned long long)to_seq);
    return send_pkt(sock, srv, PKT_NACK, 0, stream_id, from_seq, msg, (uint32_t)n, key, 1);
}

static int send_ping(int sock, const struct sockaddr_in *srv, unsigned char key)
{
    const char ping[] = "PING";
    return send_pkt(sock, srv, PKT_PING, 0, 0, 0, ping, (uint32_t)strlen(ping), key, 1);
}

// ====== Main (suscriptor) ======
int main(void)
{
    int sockfd;
    struct sockaddr_in server_addr;

    // Crear socket UDP
    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        perror("Error al crear socket");
        exit(EXIT_FAILURE);
    }

    // Timeout de recepción
    struct timeval timeout = { RECV_TIMEOUT_SEC, 0 };
    if (setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0) {
        perror("setsockopt SO_RCVTIMEO");
        close(sockfd);
        return 1;
    }

    // Dirección del broker
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);
    server_addr.sin_addr.s_addr = inet_addr(IP_BROKER);

    // ——— Handshake para obtener clave ———
    unsigned char broker_key = 0;
    if (do_handshake_get_key(sockfd, &server_addr, &broker_key) != 0) {
        fprintf(stderr, "Fallo handshake con broker\n");
        close(sockfd);
        return 1;
    }
    printf("Clave del broker recibida: %u\n", broker_key);

    // ——— Elegir tópico y calcular stream_id ———
    char topic[128];
    printf("Ingrese el tópico a suscribirse (ej: EquipoAvsB): ");
    if (!fgets(topic, sizeof(topic), stdin)) {
        fprintf(stderr, "No se pudo leer tópico\n");
        close(sockfd);
        return 1;
    }
    topic[strcspn(topic, "\n")] = 0;
    if (topic[0] == '\0') {
        fprintf(stderr, "Tópico vacío\n");
        close(sockfd);
        return 1;
    }

    uint32_t stream_id = djb2_hash(topic);
    if (send_subscribe(sockfd, &server_addr, stream_id, broker_key) != 0) {
        perror("send SUBSCRIBE");
        close(sockfd);
        return 1;
    }
    printf("Suscrito a '%s' (stream_id=%u)\n", topic, stream_id);

    // ——— Bucle de recepción ———
    uint64_t expected_seq = 1; // esperamos empezar en 1
    time_t last_keepalive = time(NULL);

    for (;;) {
        quic_like_header_t hdr;
        struct sockaddr_in from;
        char payload[MAX_PAYLOAD];

        int r = recv_pkt(sockfd, &from, &hdr, payload, sizeof(payload), broker_key, 1);
        if (r < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // Timeout: enviamos keepalive/ping
                time_t now = time(NULL);
                if (now - last_keepalive >= KEEPALIVE_SEC) {
                    (void)send_ping(sockfd, &server_addr, broker_key);
                    last_keepalive = now;
                }
                // Continuar esperando
                continue;
            } else {
                perror("recv_pkt");
                break;
            }
        }

        // Procesar según tipo
        switch (hdr.type) {
            case PKT_DATA: {
                if (hdr.stream_id != stream_id) {
                    // Datos de otro stream: ignorar o manejar múltiples
                    // Aquí los ignoramos para simplificar
                    break;
                }

                uint64_t seq = hdr.seq;

                if (seq > expected_seq) {
                    // Perdimos paquetes: pedir retransmisión
                    fprintf(stderr, "Faltan paquetes: esperado=%llu recibido=%llu -> solicito NACK [%llu,%llu]\n",
                            (unsigned long long)expected_seq,
                            (unsigned long long)seq,
                            (unsigned long long)expected_seq,
                            (unsigned long long)(seq - 1));
                    (void)send_nack(sockfd, &server_addr, stream_id, expected_seq, seq - 1, broker_key);
                    // NOTA: no avanzamos expected_seq aún
                }

                if (seq == expected_seq) {
                    // Mostrar mensaje de aplicación
                    // (payload ya viene XOR-descifrado)
                    // Se asume texto UTF-8, agregamos '\0' para imprimir.
                    payload[r] = '\0';
                    printf("[DATA seq=%llu] %s\n", (unsigned long long)seq, payload);

                    expected_seq = seq + 1;

                    // ACK al broker
                    (void)send_ack(sockfd, &server_addr, stream_id, seq, broker_key);

                    // Si se marcó fin de stream, podríamos salir
                    if (hdr.flags & F_END_STREAM) {
                        printf("Fin del stream recibido. Saliendo.\n");
                        goto out;
                    }
                } else if (seq < expected_seq) {
                    // Duplicado (posible retransmisión): re-ACK opcional
                    (void)send_ack(sockfd, &server_addr, stream_id, seq, broker_key);
                }
                break;
            }

            case PKT_PONG:
                // Mantener viva la conexión
                // (no hacemos nada, solo feedback del broker)
                break;

            case PKT_HELLO_REPLY:
                // Respuesta tardía/duplicada del handshake; ignorar.
                break;

            case PKT_ACK:
                // ACK del broker a algo que enviamos (SUB/PING), opcional manejar
                break;

            case PKT_NACK:
                // El broker no debería mandar NACK al suscriptor en este esquema.
                break;

            default:
                // Paquete desconocido: ignorar
                break;
        }
    }

out:
    close(sockfd);
    return 0;
}
