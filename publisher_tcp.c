// publisher_tcp.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>

//Definir el puerto donde está el broker y el tamaño del buffer s
#define PORT 5927
#define BUF_SIZE 2048

int main(void) {

    //-----------------CREAR EL SOCKET TCP-----------------

    //Se crea el socket TCP con IPv4
    //IPv4 = AF_INET
    //TCP = SOCK_STREAM
    // 0 = protocolo por defecto (TCP para SOCK_STREAM), pero también puede ser IPPROTO_TCP
    // Viene de: <sys/socket.h>
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) { 
        perror("socket"); 
        exit(1); 
    }

//-----------------CONECTAR AL BROKER-----------------

    //Este bloque usa: <netinet/in.h>

    // La estructura sockaddr_in es:
    //struct in_addr {    Dirección IPv4
    //  uint32_t s_addr;}; IP en binario

    //struct sockaddr_in {    // Dirección IPv4 para sockets
    //     sa_family_t    sin_family;   // Familia de direcciones (Ej.AF_INET)
    //     in_port_t      sin_port;     // Puerto en orden de red (htons)
    //     struct in_addr sin_addr;     // IP v4 (struct in_addr)
    //     unsigned char  sin_zero[8];  // Relleno/padding (no se usa)
    // };
    struct sockaddr_in srv = {0};
    srv.sin_family = AF_INET;
    srv.sin_port = htons(PORT);
    
    srv.sin_addr.s_addr = inet_addr("IP_BROKER");

    //int connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen);
    
    // sockfd: descriptor de socket creado con socket().
    // addr: dirección del peer, en este caso el broker;
    // addrlen: tamaño real de la estructura

    // (struct sockaddr*)&srv, (tipo)dato, en este caso es el cast al tipo de dato 
    //sockaddr que es un struct y se convierte el punto a srv.

    //Inicia el Handshake TCP con el broker
    if (connect(sock, (struct sockaddr*)&srv, sizeof(srv)) < 0) { 
        //Hubo un error, perror imprimiría. connect: Connection refused
        perror("connect"); 
        exit(1); 
    }

    //-----------------PUBLICAR MENSAJES A UN TEMA-----------------

    // Variables locales: topic para el texto del tema a publicar
    // line para el contenido del mensaje
    // y out para construir el mensaje final a enviar
    char topic[128];
    printf("Tema del partido (ej: EquipoAvsB): ");

    // fgets es una función de la biblioteca estándar de C que se utiliza para leer una cadena de caracteres de un flujo (stdin)
    // char *fgets (char *string, int n, FILE *stream); en <stdio.h>
    if (!fgets(topic, sizeof(topic), stdin)) return 0;
    // índice del primer '\n' para reemplazarlo por '\0' (o deja el '\0' final tal cual si no había \n).
    topic[strcspn(topic, "\n")] = 0;

    char line[BUF_SIZE], out[BUF_SIZE];

    // Bucle infinito para leer mensajes desde stdin y enviarlos al broker, pudo ser for (;;) tambien
    while (1) {
        printf("Mensaje (ej: Gol minuto 45) o SALIR: ");
        if (!fgets(line, sizeof(line), stdin)) break;

        // Reemplaza el salto de línea por terminador nulo
        line[strcspn(line, "\n")] = 0;

        if (strcmp(line, "SALIR") == 0) break;

        // snprintf escribe en dst como lo haría printf, pero a lo sumo dst_size-1 caracteres,
        // y si dst_size > 0 siempre termina en '\0'.
        // No desborda el búfer
        snprintf(out, sizeof(out), "PUBLISH %s %s\n", topic, line);

        //send envía datos a través del socket creado con descriptor sock.
        // Con TCP, send solo pone datos en el buffer del kernel; no garantiza que el peer ya los recibió.
        // La garantía y los reintentos por pérdida de ACKs los hace TCP en el kernel.
        if (send(sock, out, strlen(out), 0) < 0) { 
            perror("send"); 
            break; 
            }
    }

    close(sock);
    return 0;
}
