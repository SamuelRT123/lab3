#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#define BUFFER_SIZE 1024
#define PORT 5926

int main() {
    int sockfd;
    struct sockaddr_in server_addr;
    char topic[50], message[512], buffer[BUFFER_SIZE];

    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        perror("Error al crear socket");
        exit(EXIT_FAILURE);
    }

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);
    server_addr.sin_addr.s_addr = inet_addr("IP_BROKER");

    printf("Ingrese el tema del partido (ej: EquipoAvsB): ");
    fgets(topic, 50, stdin);
    topic[strcspn(topic, "\n")] = 0;

    while (1) {
        printf("Mensaje a enviar (ej: Gol minuto 45): ");
        fgets(message, 512, stdin);
        message[strcspn(message, "\n")] = 0;

        snprintf(buffer, BUFFER_SIZE, "PUBLISH %s %s", topic, message);
        sendto(sockfd, buffer, strlen(buffer), 0,
               (struct sockaddr *)&server_addr, sizeof(server_addr));

        if (strcmp(message, "SALIR") == 0)
            break;
    }

    close(sockfd);
    return 0;
}
