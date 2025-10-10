// broker_tcp.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/select.h>

#define PORT 8080
#define MAX_CLIENTS  FD_SETSIZE
#define BUF_SIZE     2048
#define TOPIC_SIZE   64

typedef enum { ROLE_UNKNOWN=0, ROLE_SUB, ROLE_PUB } Role;

typedef struct {
    int   fd;
    Role  role;
    char  topic[TOPIC_SIZE];
} Client;

static Client clients[MAX_CLIENTS];

static void remove_client(int i, fd_set *allset, int *maxfd) {
    if (clients[i].fd >= 0) {
        close(clients[i].fd);
        clients[i].fd = -1;
        clients[i].role = ROLE_UNKNOWN;
        clients[i].topic[0] = '\0';
    }
    // recompute maxfd
    *maxfd = -1;
    for (int k = 0; k < MAX_CLIENTS; ++k)
        if (clients[k].fd > *maxfd) *maxfd = clients[k].fd;
}

static void broadcast_to_topic(const char *topic, const char *msg) {
    for (int i = 0; i < MAX_CLIENTS; ++i) {
        if (clients[i].fd >= 0 && clients[i].role == ROLE_SUB &&
            strcmp(clients[i].topic, topic) == 0) {
            send(clients[i].fd, msg, strlen(msg), 0);
        }
    }
}

static void handle_line(int idx, char *line) {
    // trim trailing \r\n
    size_t n = strlen(line);
    while (n && (line[n-1]=='\n' || line[n-1]=='\r')) line[--n]='\0';

    if (strncmp(line, "SUBSCRIBE ", 10) == 0) {
        clients[idx].role = ROLE_SUB;
        strncpy(clients[idx].topic, line + 10, TOPIC_SIZE-1);
        clients[idx].topic[TOPIC_SIZE-1] = '\0';
        char ok[128];
        snprintf(ok, sizeof(ok), "OK SUBSCRIBED %s\n", clients[idx].topic);
        send(clients[idx].fd, ok, strlen(ok), 0);
        fprintf(stdout, "[Broker] SUB: fd=%d topic=%s\n", clients[idx].fd, clients[idx].topic);
    } else if (strncmp(line, "PUBLISH ", 8) == 0) {
        // format: PUBLISH <topic> <message...>
        char topic[TOPIC_SIZE] = {0};
        const char *p = line + 8;
        // read topic (token hasta espacio)
        int tlen = 0;
        while (*p && *p!=' ' && tlen < TOPIC_SIZE-1) topic[tlen++] = *p++;
        topic[tlen] = '\0';
        while (*p == ' ') ++p; // skip spaces
        const char *msg = p;
        fprintf(stdout, "[Broker] PUB: topic=%s msg=%s\n", topic, msg);
        // reenviar sólo el mensaje "plano"
        char out[BUF_SIZE];
        snprintf(out, sizeof(out), "%s\n", msg);
        broadcast_to_topic(topic, out);
    } else {
        const char *err = "ERR Unknown command\n";
        send(clients[idx].fd, err, strlen(err), 0);
    }
}

int main(void) {
    // init clients
    for (int i = 0; i < MAX_CLIENTS; ++i) clients[i].fd = -1;

    int listenfd = socket(AF_INET, SOCK_STREAM, 0);
    if (listenfd < 0) { perror("socket"); exit(1); }

    int yes = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in srv = {0};
    srv.sin_family = AF_INET;
    srv.sin_port   = htons(PORT);
    srv.sin_addr.s_addr = INADDR_ANY; // o fija tu IP local con inet_pton si quieres

    if (bind(listenfd, (struct sockaddr*)&srv, sizeof(srv)) < 0) { perror("bind"); exit(1); }
    if (listen(listenfd, 64) < 0) { perror("listen"); exit(1); }

    fd_set allset, rset;
    FD_ZERO(&allset);
    FD_SET(listenfd, &allset);
    int maxfd = listenfd;

    printf("Broker TCP escuchando en puerto %d...\n", PORT);

    char buf[BUF_SIZE];

    for (;;) {
        rset = allset;
        int nready = select(maxfd + 1, &rset, NULL, NULL, NULL);
        if (nready < 0) { perror("select"); continue; }

        if (FD_ISSET(listenfd, &rset)) {
            struct sockaddr_in cli; socklen_t clilen = sizeof(cli);
            int connfd = accept(listenfd, (struct sockaddr*)&cli, &clilen);
            if (connfd >= 0) {
                // add to clients
                int placed = 0;
                for (int i = 0; i < MAX_CLIENTS; ++i) {
                    if (clients[i].fd < 0) {
                        clients[i].fd = connfd;
                        clients[i].role = ROLE_UNKNOWN;
                        clients[i].topic[0] = '\0';
                        FD_SET(connfd, &allset);
                        if (connfd > maxfd) maxfd = connfd;
                        placed = 1;
                        break;
                    }
                }
                if (!placed) {
                    const char *full = "ERR Server full\n";
                    send(connfd, full, strlen(full), 0);
                    close(connfd);
                }
            }
            if (--nready <= 0) continue;
        }

        for (int i = 0; i <= maxfd; ++i) {
            if (i == listenfd) continue;
            // find client index
            int idx = -1;
            for (int k = 0; k < MAX_CLIENTS; ++k) if (clients[k].fd == i) { idx = k; break; }
            if (idx < 0) continue;

            if (FD_ISSET(i, &rset)) {
                ssize_t n = recv(i, buf, sizeof(buf)-1, 0);
                if (n <= 0) {
                    FD_CLR(i, &allset);
                    remove_client(idx, &allset, &maxfd);
                } else {
                    buf[n] = '\0';
                    // procesar por líneas (pueden venir varias)
                    char *saveptr = NULL;
                    char *line = strtok_r(buf, "\n", &saveptr);
                    while (line) {
                        handle_line(idx, line);
                        line = strtok_r(NULL, "\n", &saveptr);
                    }
                }
            }
        }
    }
    return 0;
}
