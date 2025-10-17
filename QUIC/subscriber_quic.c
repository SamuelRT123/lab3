#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#define BUFFER_SIZE 1024
#define PORT 5928
#define IP_BROKER "127.0.0.1"

int main() {
    int sockfd;
    struct sockaddr_in server_addr, local_addr;
    char topic[50], buffer[BUFFER_SIZE];
    socklen_t addr_len = sizeof(server_addr);

    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        perror("Error al crear socket");
        exit(EXIT_FAILURE);
    }

    local_addr.sin_family = AF_INET;
    local_addr.sin_port = htons(0);  // puerto aleatorio
    local_addr.sin_addr.s_addr = INADDR_ANY;
    bind(sockfd, (struct sockaddr *)&local_addr, sizeof(local_addr));

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);
    server_addr.sin_addr.s_addr = inet_addr(IP_BROKER);

    printf("Tema a suscribirse (ej: EquipoAvsB): ");
    fgets(topic, 50, stdin);
    topic[strcspn(topic, "\n")] = 0;

    snprintf(buffer, BUFFER_SIZE, "SUBSCRIBE %s", topic);
    sendto(sockfd, buffer, strlen(buffer), 0,
           (struct sockaddr *)&server_addr, sizeof(server_addr));

    printf("Suscrito al tema: %s\nEsperando mensajes...\n", topic);

    while (1) {

        //Esta línea limpia el contenido del buffer antes de recibir nuevos datos.
        memset(buffer, 0, BUFFER_SIZE);
        recvfrom(sockfd, buffer, BUFFER_SIZE, 0, NULL, NULL);
        printf("[Mensaje recibido] %s\n", buffer);
    }

    close(sockfd);
    return 0;
}
