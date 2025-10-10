// subscriber_tcp.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>

#define PORT 8080
#define BUF_SIZE 2048

int main(void) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) { perror("socket"); exit(1); }

    struct sockaddr_in srv = {0};
    srv.sin_family = AF_INET;
    srv.sin_port = htons(PORT);
    // CAMBIA ESTA IP SEGÚN DÓNDE CORRA EL BROKER (127.0.0.1 si es la misma máquina)
    srv.sin_addr.s_addr = inet_addr("127.0.0.1");

    if (connect(sock, (struct sockaddr*)&srv, sizeof(srv)) < 0) { perror("connect"); exit(1); }

    char topic[128], out[256];
    printf("Tema a suscribirse (ej: EquipoAvsB): ");
    if (!fgets(topic, sizeof(topic), stdin)) return 0;
    topic[strcspn(topic, "\n")] = 0;

    snprintf(out, sizeof(out), "SUBSCRIBE %s\n", topic);
    if (send(sock, out, strlen(out), 0) < 0) { perror("send"); return 1; }

    printf("Suscrito a %s. Esperando mensajes...\n", topic);

    char buf[BUF_SIZE];
    for (;;) {
        ssize_t n = recv(sock, buf, sizeof(buf)-1, 0);
        if (n <= 0) { puts("Conexión cerrada."); break; }
        buf[n] = '\0';
        fputs(buf, stdout); // los mensajes ya llegan con '\n'
    }

    close(sock);
    return 0;
}
