// publisher_tcp.c
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

    char topic[128];
    printf("Tema del partido (ej: EquipoAvsB): ");
    if (!fgets(topic, sizeof(topic), stdin)) return 0;
    topic[strcspn(topic, "\n")] = 0;

    // Este publisher puede (opcionalmente) identificarse, pero en este diseño basta con enviar PUBLISH...
    char line[BUF_SIZE], out[BUF_SIZE];
    while (1) {
        printf("Mensaje (ej: Gol minuto 45) o SALIR: ");
        if (!fgets(line, sizeof(line), stdin)) break;
        line[strcspn(line, "\n")] = 0;
        if (strcmp(line, "SALIR") == 0) break;

        snprintf(out, sizeof(out), "PUBLISH %s %s\n", topic, line);
        if (send(sock, out, strlen(out), 0) < 0) { perror("send"); break; }
    }

    close(sock);
    return 0;
}
