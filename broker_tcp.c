// broker_tcp.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/select.h>
#include <sys/socket.h>


//Número del puerto donde esta escuchando
//FD_SETSIZE es una constante del sistema Linux=1024
//Buffer del broker
//Tamaño máximo de la longitud del tema.
#define PORT 5927
#define MAX_CLIENTS  FD_SETSIZE
#define BUF_SIZE     2048
#define TOPIC_SIZE   64


//Definir un enum para tener claridad en que es cada cliente conectado al broker, un pub o un sub.
typedef enum { ROLE_UNKNOWN=0, ROLE_SUB, ROLE_PUB } Role;

//Struct para representar la información de un cliente conectado al broker.
// Cada conexión aceptada con accept() devuelve un fd único, 
//     que se usa en send() y recv() para enviar/recibir datos del cliente.

typedef struct {
    int   fd;
    Role  role;
    char  topic[TOPIC_SIZE];
} Client;

static Client clients[MAX_CLIENTS];


// //Como hay clientes limitados, cada vez que uno se descontecta o genera error, hay que borrarlo
// static → solo es visible dentro del mismo archivo.
// i → índice del cliente dentro del arreglo global clients[].
// fd_set *allset → conjunto de descriptores de archivo (sockets) que select() está vigilando.
// int *maxfd → puntero al valor del descriptor más alto en uso (necesario para select()).

static void remove_client(int i, fd_set *allset, int *maxfd) {
    if (clients[i].fd >= 0) {
        close(clients[i].fd);
        clients[i].fd = -1;
        clients[i].role = ROLE_UNKNOWN;
        clients[i].topic[0] = '\0';
    }
    // recalcular maxfd
    *maxfd = -1;
    for (int k = 0; k < MAX_CLIENTS; ++k)
        if (clients[k].fd > *maxfd) *maxfd = clients[k].fd;
}

//const char *topic → nombre del tema al que pertenece el mensaje.
//const char *msg → el mensaje que se quiere enviar a todos los clientes suscritos a ese topic.

// Itera sobre todos los clientes para ver cuales estan suscritos al tema y están activos (fd >= 0), en orden:
// Es valido, es un suscriptor y el tema coincide.
//Envia el mensaje al suscriptor con send() y el descriptor del socket correspondiente. Vuelve a iterar().
static void broadcast_to_topic(const char *topic, const char *msg) {
    for (int i = 0; i < MAX_CLIENTS; ++i) {
        if (clients[i].fd >= 0 && clients[i].role == ROLE_SUB && strcmp(clients[i].topic, topic) == 0) {
            send(clients[i].fd, msg, strlen(msg), 0);
        }
    }
}

//Identidica si es un publicador o un suscriptor, los crea, formatea los mensajes y los envía.
//Ver los otros archivos de TCP para corrobarar consistencia PUBLISH y SUBSCRIBE
static void handle_line(int idx, char *line) {
    // trim \r\n
    size_t n = strlen(line);
    while (n && (line[n-1]=='\n' || line[n-1]=='\r')) line[--n]='\0';

    // Si line es igual a "SUBSCRIBE", crea ese suscriptor y le asigna todos sus atributos.
    if (strncmp(line, "SUBSCRIBE ", 10) == 0) {
        clients[idx].role = ROLE_SUB;
        strncpy(clients[idx].topic, line + 10, TOPIC_SIZE-1);
        clients[idx].topic[TOPIC_SIZE-1] = '\0';
        char ok[128];
        snprintf(ok, sizeof(ok), "OK SUBSCRIBED %s\n", clients[idx].topic);
        //Confirma la conexion al cliente.
        send(clients[idx].fd, ok, strlen(ok), 0);
        fprintf(stdout, "[Broker] SUB: fd=%d topic=%s\n", clients[idx].fd, clients[idx].topic);

    } else if (strncmp(line, "PUBLISH ", 8) == 0) {
        // formato: PUBLISH <topic> <message...>
        char topic[TOPIC_SIZE] = {0};
        const char *p = line + 8;
        // Leer tema (token hasta espacio)
        int tlen = 0;
        while (*p && *p!=' ' && tlen < TOPIC_SIZE-1) topic[tlen++] = *p++;
        topic[tlen] = '\0';
        while (*p == ' ') ++p; // Saltar espacios
        const char *msg = p;
        fprintf(stdout, "[Broker] PUB: topic=%s msg=%s\n", topic, msg);
        // reenviar sólo el mensaje plano
        char out[BUF_SIZE];
        snprintf(out, sizeof(out), "%s\n", msg);
        broadcast_to_topic(topic, out);
    } else {
        const char *err = "ERR Unknown command\n";
        send(clients[idx].fd, err, strlen(err), 0);
    }
}

int main(void) {
    // init de clients
    for (int i = 0; i < MAX_CLIENTS; ++i) clients[i].fd = -1;

    //Se crea el socket TCP.
    int listenfd = socket(AF_INET, SOCK_STREAM, 0);
    if (listenfd < 0) { 
        perror("socket"); exit(1); 
    }

    // Habilita la reutilización del puerto/dirección para el socket de escucha.
    // Por si el broker falla y las conexiones ya existentes quedan en TIME_WAIT.
    // Esto permite reiniciar el broker sin esperar a que se libere el puerto.
    /*
        Firma y parámetros de setsockopt():

        int setsockopt(int sockfd, int level, int optname, const void *optval, socklen_t optlen);

        - sockfd  : descriptor del socket al que se le aplica la opción.
        - level   : “capa”/protocolo de la opción (ej.: SOL_SOCKET, IPPROTO_TCP, IPPROTO_IP, IPPROTO_IPV6).
        - optname : nombre de la opción dentro de ese nivel (ej.: SO_REUSEADDR, TCP_NODELAY, SO_RCVTIMEO).
        - optval  : puntero al valor a establecer (p. ej., int=1 para habilitar).
        - optlen  : tamaño en bytes del dato apuntado por optval (ej.: sizeof(int), sizeof(struct timeval)).
        - return  : 0 en éxito; -1 en error y errno indica la causa.
        */
    int yes = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));


    // Se repite la misma estructura que en los otros archivos TCP, en este caso la IP debe ser la misma donde corre el broker.
    struct sockaddr_in srv = {0};
    srv.sin_family = AF_INET;
    srv.sin_port   = htons(PORT);
    srv.sin_addr.s_addr = INADDR_ANY;

    // bind(): asocia el socket a una dirección local (IP, puerto).
    // Reserva ip_port para que el kernel sepa a donde van los datos entrantes.
    if (bind(listenfd, (struct sockaddr*)&srv, sizeof(srv)) < 0) { 
        perror("bind"); exit(1); 
    }

    if (listen(listenfd, 64) < 0) { 
        perror("listen"); exit(1); 
    }

    //Declarar 2 conjuntos de descriptores de archivo (fd_set).
    // allset → conjunto maestro que mantiene todos los fds activos (escucha + clientes
    // rset  → conjunto temporal que se pasa a select() cada vez (porque select() lo modifica).
    fd_set allset, rset;
    
    //Ninguna conexión aceptada aún.
    FD_ZERO(&allset);

    // Agrega el fd del socket de escucha (listenfd) al conjunto maestro.
    // Así select() podrá avisar cuando haya nuevas conexiones pendientes (accept()).
    FD_SET(listenfd, &allset);
    
    int maxfd = listenfd;

    // select() necesita saber el mayor fd que estás vigilando (se pasa como maxfd+1).
    // Arranca siendo listenfd; cada vez que aceptes un nuevo cliente, si su fd es mayor, actualiza maxfd.

    printf("Broker TCP escuchando en puerto %d...\n", PORT);

    char buf[BUF_SIZE];

    for (;;) {
        rset = allset;


        /*
        Firma general de select():

        int select(
            int nfds,           // 1 + valor del descriptor MÁS ALTO que vigilas
            fd_set *readfds,    // conjunto a vigilar por "listo para leer"   (puede ser NULL)
            fd_set *writefds,   // conjunto a vigilar por "listo para escribir" (puede ser NULL)
            fd_set *exceptfds,  // conjunto a vigilar por condiciones excepcionales (puede ser NULL)
            struct timeval *timeout // cuánto esperar: NULL = infinito; {0,0} = no bloquear; o un tiempo
        );

        Retorna: 
        >0  número de descriptores "listos" en los conjuntos (se MODIFICAN in-place),
        0  si expiró el timeout sin eventos,
        -1  en error (errno establece la causa, p.ej. EINTR si se interrumpió por señal).
        */
        int nready = select(maxfd + 1, &rset, NULL, NULL, NULL);
        if (nready < 0) { 
            perror("select"); 
            continue; 
        }

        
        if (FD_ISSET(listenfd, &rset)) {
            //Hay conexiones pendientes en el socket de escucha.

            // Estructuras para guardar la info del cliente que se conecta.
            struct sockaddr_in cli;  //IPv4 del cliente
            socklen_t clilen = sizeof(cli);
            // Aceptar la conexión pendiente.
            int connfd = accept(listenfd, (struct sockaddr*)&cli, &clilen);
            if (connfd >= 0) {
                // Añadir a los clientes
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

            //listenfd ya fue atendido arriba en acept().
            if (i == listenfd) continue;
            
            //Encontrar idx del cliente con fd=i
            int idx = -1;
            for (int k = 0; k < MAX_CLIENTS; ++k) if (clients[k].fd == i) { idx = k; break; }
            //Ya fue gestionado el cliente.
            if (idx < 0) continue;

            if (FD_ISSET(i, &rset)) {
                // esta listo para lectura, se lee con recv()
                ssize_t n = recv(i, buf, sizeof(buf)-1, 0);
                if (n <= 0) {
                    //Error, toca sacarlo del conjunto y eliminar el cliente.
                    FD_CLR(i, &allset);
                    remove_client(idx, &allset, &maxfd);
                } else {
                    buf[n] = '\0';
                    // procesar por líneas (pueden venir varias)
                    char *saveptr = NULL;
                    //strtok_r= split
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
