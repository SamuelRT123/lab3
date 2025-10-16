#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#define MAX_CLIENTS 15
#define BUFFER_SIZE 1024
#define PORT 5926

typedef struct {
    struct sockaddr_in addr;
    socklen_t addr_len;
    char topic[50];
} Subscriber;

Subscriber subscribers[MAX_CLIENTS];
int subscriber_count = 0;

void add_subscriber(struct sockaddr_in addr, socklen_t addr_len, char *topic) {
    if (subscriber_count < MAX_CLIENTS) {
        subscribers[subscriber_count].addr = addr;
        subscribers[subscriber_count].addr_len = addr_len;
        strcpy(subscribers[subscriber_count].topic, topic);
        subscriber_count++;
        printf("Nuevo suscriptor agregado al tema: %s\n", topic);
    } else {
        printf("Máximo número de suscriptores alcanzado.\n");
    }
}

void distribute_message(int sockfd, char *topic, char *message) {
    for (int i = 0; i < subscriber_count; i++) {
        if (strcmp(subscribers[i].topic, topic) == 0) {
            sendto(sockfd, message, strlen(message), 0,
                   (struct sockaddr *)&subscribers[i].addr,
                   subscribers[i].addr_len);
        }
    }
}

int main() {
    int sockfd;
    struct sockaddr_in server_addr, client_addr;
    char buffer[BUFFER_SIZE];
    socklen_t addr_len = sizeof(client_addr);

    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        perror("Error al crear socket");
        exit(EXIT_FAILURE);
    }

    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(PORT);

    if (bind(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("Error al hacer bind");
        exit(EXIT_FAILURE);
    }

    printf("Broker UDP escuchando en puerto %d...\n", PORT);

    while (1) {
        memset(buffer, 0, BUFFER_SIZE);
        recvfrom(sockfd, buffer, BUFFER_SIZE, 0,
                 (struct sockaddr *)&client_addr, &addr_len);

        if (strncmp(buffer, "SUBSCRIBE", 9) == 0) {
            char topic[50];
            sscanf(buffer, "SUBSCRIBE %s", topic);
            add_subscriber(client_addr, addr_len, topic);
        } else if (strncmp(buffer, "PUBLISH", 7) == 0) {
            char topic[50], message[512];
            sscanf(buffer, "PUBLISH %s %[^\n]", topic, message);
            printf("Mensaje recibido para %s: %s\n", topic, message);
            distribute_message(sockfd, topic, message);
        }
    }

    close(sockfd);
    return 0;
}
