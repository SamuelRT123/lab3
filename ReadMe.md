# Laboratorio 3: ANÁLISIS DE CAPA DE TRANSPORTE Y SOCKETS

## Grupo 7:

David Quiroga 202310820 <br>
Nicolas Gonzalez 202310041 <br>
Samuel Rodríguez Torres 202310140

El propósito de este archivo ReadMe.md es explicar los detalles relacionados con la implementación del patrón de arquitectura publish/subscriber con sockets para poder analizar y ver el funcionamiento de los protocolos de la capa de transporte TCP y UDP. Para esto, se hizo codigo en C que nos permitiriera interactuar con elementos de bajo nivel de la máquina, con la excepción de no poder alterar el kerner propio de la máquina donde se hace el handshake, el control de flujo y el control de congestión.

Adicionalmente, se encuentra una sección del protocolo QUIC, el cual se explica en su respectiva sección y es parte del bono propuesto por el laboratorio, por lo que no tiene apartado en el documento PDF de análisis.

# PUB/SUB
El patrón de arquitectura publish/subscriber es un patrón usado para manejar una comunicación asincrona entre escenarios donde la comunicación se maneja unicamente en 1 dirección. Este patrón nace a raiz de la necesidad de desacoplar responsabilidades en ambas partes e incrementar la escalabilidad y el desempeño del sistema. <br>
Para esto, se necesita de un broker que haga de intermedio en la comunicación; recibe todos los mensajes producidos por los publicadores y sabe rediriglos a los suscriptores correspondientes. <br>
En este orden de ideas, cada protocolo cuenta con la implementación de 3 archivos, 
- 1.Publisher: Correspondiente al publicador que genera los mensajes.
- 2.Broker: Corresponde al broker de mensajeria encargado de distribuir los mensajes a los suscriptores que estén registrados en el tema de cada mensaje. 
- 3.Subscriber: Corresponde al suscriptor de cada tema que espera recibir los mensajes generados solo para su tema distribuido por el broker.

# Librerías
Únicamente se utilizaron librerias nativas de C para hacer el socket, las cuales fueron:

- stdio.h: <br>
Aporta funciones de entrada/salida estándar para imprimir al terminal y construir cadenas.

- stdlib.h <br>
Utilidades generales: En los archivos se usa para tener exit y terminar el programa con un código de estado y error (de stdio.h) para mostrar errores legibles.

- string.h <br>
Manejo de cadenas. Aquí se usan strlen (para saber el tamaño de lo que vamos a enviar) y strcspn (para eliminar el salto de línea \n que deja fgets al leer el tema).

- unistd.h <br>
Funciones POSIX para interactual a bajo nivel con el sistema, como close/read/write para el descriptor del socket.

- arpa/inet.h <br>
Tipos y utilidades para redes: struct sockaddr_in, htons (convertir puerto a “network byte order”) e inet_addr (convertir una IP en texto, p. ej. "127.0.0.1", a formato binario).

## Compilación
```bash
gcc subscriber_tcp.c -o subscriber_tcp
```

### Ejecución:
```
./subscriber_tcp
```

# TCP

## subscriber_tcp.c

- [Archivo Documentado](https://github.com/LabsRedes/Laboratorio-3/blob/main/subscriber_tcp.c) 


#### Explicación

Este programa implementa un cliente publicador TCP que se conecta a un broker para recibir mensajes de un tema específico.  


#### Creación del socket

Primero se crea un socket TCP usando IPv4 a través de <sys/socket.h>.  
El socket se configura con la familia de direcciones AF_INET y el tipo SOCK_STREAM, que corresponde a una conexión TCP.  
Si la creación falla, el programa muestra un error y termina.


#### Conexión al broker
Se prepara una estructura sockaddr_in que contiene la dirección IP y el puerto del broker.  
Luego, el cliente intenta conectarse mediante la función connect, lo que inicia el handshake TCP con el broker.  
Si la conexión es rechazada o falla, se muestra el mensaje de error correspondiente.


#### Suscripción a un tema
El usuario ingresa el nombre del tema al que desea suscribirse.  
El programa construye un mensaje en el formato SUBSCRIBE tema y lo envía al broker a través del socket.  
Si el envío falla, se imprime un error y el programa termina.


#### Recepción de mensajes
Una vez suscrito, el cliente entra en un bucle infinito donde espera mensajes del broker.  
Cada vez que recibe datos, los muestra por pantalla sin modificarlos.  
Si el broker cierra la conexión o ocurre un error, el cliente sale del bucle y finaliza.


#### Cierre de la conexión
Al finalizar la comunicación, el socket se cierra correctamente y el programa termina su ejecución limpiamente.


#### Resumen general
- Crea un socket TCP y se conecta al broker.  
- Envía un mensaje de suscripción con el tema deseado.  
- Permanece recibiendo y mostrando mensajes en tiempo real.  
- Finaliza cuando el broker cierra la conexión o hay un error.  


## publisher_tcp.c
- [Archivo Documentado](https://github.com/LabsRedes/Laboratorio-3/blob/main/publisher_tcp.c) 


#### Explicación

Este programa implementa un cliente suscriptor TCP que se conecta a un broker para recibir mensajes de un tema específico.  


#### Creación del socket

Primero se crea un socket TCP usando IPv4 a través de <sys/socket.h>.  
El socket se configura con la familia de direcciones AF_INET y el tipo SOCK_STREAM, que corresponde a una conexión TCP.  
Si la creación falla, el programa muestra un error y termina.


#### Conexión al broker
Se prepara una estructura sockaddr_in que contiene la dirección IP y el puerto del broker.  
Luego, el cliente intenta conectarse mediante la función connect, lo que inicia el handshake TCP con el broker.  
Si la conexión es rechazada o falla, se muestra el mensaje de error correspondiente.


### Publicación en un tema

El usuario ingresa el nombre del tema (por ejemplo, EquipoAvsB).
Luego, puede escribir mensajes asociados a ese tema, como "Gol minuto 45".
Cada mensaje se envía al broker en el formato:
```
PUBLISH <tema> <mensaje>
```

El envío se realiza mediante la función send().
Si ocurre un error al enviar, el programa lo reporta y finaliza la conexión.

### Finalización

El usuario puede escribir SALIR para terminar el programa.
Al salir, el socket se cierra ordenadamente con close().


#### Resumen general
- Crea un socket TCP y se conecta al broker.  
- Publica un tema.
- Permanece recibiendo entradas del usuario sobre el tema ingresado.
- Finaliza cuando el broker cierra la conexión o hay un error. (SALIR)


## broker_tcp.c
- [Archivo Documentado](https://github.com/LabsRedes/Laboratorio-3/blob/main/broker_tcp.c) 

El broker escucha en el **puerto 5927** y maneja múltiples clientes usando **select()**  y un arreglo fijo de conexiones.

### Cómo funciona

#### 1. Inicialización de estado
Se limpia la tabla global de clientes y se definen constantes como:
- PORT = 5927  
- BUF_SIZE = 2048  
- TOPIC_SIZE = 64  

Se crean los conjuntos de descriptores:
- fd_set allset (maestro)
- fd_set rset (temporal)

#### 2. Creación y preparación del socket de escucha
- Se crea el socket TCP con socket(AF_INET, SOCK_STREAM, 0).  
- Se habilita SO_REUSEADDR mediante setsockopt().  
- Se configura la estructura sockaddr_in con INADDR_ANY y htons(PORT).  
- Se asocia con bind() y se pone en escucha con listen().  
- Se agrega el socket al conjunto maestro allset y se actualiza el maxfd.

#### 3. Bucle principal con select()
- Se copia allset en rset y se llama a select(maxfd + 1, &rset, NULL, NULL, NULL).  
- Si el socket de escucha está listo, se acepta la nueva conexión con accept().  
- El cliente se registra con su descriptor y se marca como ROLE_UNKNOWN hasta recibir un comando.


#### 4. Lectura de datos de clientes
- Para cada descriptor listo, se usa recv().  
- Si devuelve 0 o error, se desconecta al cliente.  
- Si llegan datos, se separan por líneas (strtok_r) y se procesan con handle_line().

#### 5. Protocolo de texto
- **SUBSCRIBE tema**  
  Cambia el rol a suscriptor y guarda el tema.  
  El broker responde: OK SUBSCRIBED tema.

- **PUBLISH tema mensaje**  
  El broker reenvía el mensaje a todos los suscriptores de ese tema.

- **Comando desconocido**  
  El broker responde: ERR Unknown command.

#### 6. Difusión por tema
broadcast_to_topic() recorre todos los clientes y envía el mensaje a quienes estén suscritos al tema indicado.

# UDP

## publisher_udp.c
- [Archivo Documentado](https://github.com/LabsRedes/Laboratorio-3/blob/main/publisher_udp.c) 

Este programa implementa un **publicador UDP** que envía mensajes a un servidor o broker (por ejemplo, un sistema de suscripción tipo pub/sub).  
Cada mensaje enviado incluye un **tema (topic)** y un **contenido (mensaje)**.  
El programa corre en modo interactivo desde consola, pidiendo al usuario el tema y luego los mensajes a publicar.

Está diseñado para conectarse a un broker UDP en el puerto 5926, con dirección IP 127.0.0.1.


### Cómo funciona

#### 1. Inicialización
Se definen constantes:
- BUFFER_SIZE = 1024  
- PORT = 5926  

Se declaran buffers:
- topic (50 caracteres)  
- message (512 caracteres)  
- buffer (para concatenar comando completo a enviar)


#### 2. Creación del socket
Se crea un socket UDP mediante la llamada socket(AF_INET, SOCK_DGRAM, 0).  
Si falla, se muestra un mensaje de error con perror() y se termina la ejecución.

#### 3. Configuración del destino
Se configura la estructura sockaddr_in con:
- sin_family = AF_INET  
- sin_port = htons(PORT)  
- sin_addr.s_addr = inet_addr("127.0.0.1")  

Esto apunta al servidor UDP local que recibe las publicaciones.


#### 4. Entrada de datos del usuario
El programa solicita al usuario:

1. Tema del partido o evento (por ejemplo: EquipoAvsB).  
2. Luego, en un bucle infinito, pide:
   - Un mensaje (por ejemplo: Gol minuto 45).  
   - El mensaje se empaqueta con el formato PUBLISH tema mensaje.

#### 5. Envío de datos
El comando formateado se envía al broker mediante la función sendto().  
Ejemplo de paquete enviado:

PUBLISH EquipoAvsB Gol minuto 45

El envío se repite indefinidamente hasta que el usuario escriba SALIR, lo que rompe el bucle y cierra el socket.

#### 6. Finalización
Cuando el usuario escribe SALIR, el programa sale del bucle principal.  
Se llama a close(sockfd) para liberar el descriptor de socket antes de finalizar.

## subscriber_udp.c
- [Archivo Documentado](https://github.com/LabsRedes/Laboratorio-3/blob/main/subscriber_udp.c) 

### Explicación

Este programa implementa un **suscriptor UDP** que se conecta a un broker o servidor de mensajes y **escucha las publicaciones** enviadas en un tema específico.  
Utiliza el puerto **5926** y permite al usuario ingresar el **nombre del tema (topic)** al cual desea suscribirse.

Una vez suscrito, el programa entra en un bucle infinito que recibe y muestra todos los mensajes publicados en ese tema.


### Cómo funciona

#### 1. Inicialización
Se definen las constantes:
- BUFFER_SIZE = 1024  
- PORT = 5926  

Se declaran las estructuras:
- sockaddr_in server_addr (dirección del broker o servidor UDP)  
- sockaddr_in local_addr (dirección local del suscriptor)  
- buffer (para almacenar mensajes)  
- topic (para guardar el nombre del tema)


#### 2. Creación del socket
Se crea un socket **UDP** mediante la llamada socket(AF_INET, SOCK_DGRAM, 0).  
Si la creación falla, se muestra un mensaje de error y el programa termina.

#### 3. Configuración de la dirección local
- local_addr.sin_family = AF_INET
- local_addr.sin_port = htons(0) → asigna **un puerto aleatorio disponible**  
- local_addr.sin_addr.s_addr = INADDR_ANY → escucha en cualquier interfaz de red disponible  
- Se asocia con bind() para poder recibir mensajes.


#### 4. Configuración de la dirección del broker
- server_addr.sin_family = AF_INET
- server_addr.sin_port = htons(PORT)
- server_addr.sin_addr.s_addr = inet_addr("IP_BROKER")

Aquí, "IP_BROKER" debe reemplazarse por la dirección IP del servidor o broker UDP al que el suscriptor desea conectarse.


#### 5. Suscripción a un tema
El programa pide al usuario que ingrese un tema (por ejemplo: EquipoAvsB).  
Luego construye un mensaje con el formato:

SUBSCRIBE tema

Y lo envía al broker mediante la función sendto().  
Ejemplo de mensaje enviado:

SUBSCRIBE BocaVsRiver

El servidor usa este dato para registrar al suscriptor en el tema indicado.

#### 6. Recepción de mensajes
Una vez suscrito, el programa imprime en pantalla:

Suscrito al tema: BocaVsRiver  
Esperando mensajes...

Luego entra en un bucle infinito para estar atento a recibir los mensajes que distribuya el broker.

## broker_udp.c

### Descripción breve

Este programa implementa un **broker UDP** que actúa como intermediario entre **publicadores (publishers)** y **suscriptores (subscribers)**.  
Su función principal es recibir comandos por UDP y distribuir mensajes a los suscriptores correspondientes según el tema (topic).

El sistema soporta hasta **15 suscriptores simultáneos** y usa el puerto **5926**.


### Cómo funciona

#### 1. Inicialización
Se definen las constantes:
- MAX_CLIENTS = 15  
- BUFFER_SIZE = 1024  
- PORT = 5926  

Se declara una estructura para los suscriptores:

- **Subscriber**
  - addr: dirección del cliente (estructura sockaddr_in)
  - addr_len: tamaño de la dirección
  - topic: tema al que el cliente está suscrito  


#### 2. Creación del socket y configuración del servidor
- Se crea un socket UDP.
- Se configura la dirección del servidor con:
  - sin_family = AF_INET  
  - sin_addr.s_addr = INADDR_ANY  
  - sin_port = htons(PORT)  
- Se asocia al puerto con bind() para empezar a escuchar mensajes UDP entrantes.  

Si ocurre un error en socket() o bind(), el programa muestra un mensaje y termina.

El broker queda activo mostrando:
Broker UDP escuchando en puerto 5926...

#### 3. Bucle principal (recepción de comandos)
El programa entra en un bucle infinito para manejar mensajes entrantes:

Cada datagrama recibido puede contener dos tipos de comandos:

1. **SUBSCRIBE tema**  
   - El broker extrae el nombre del tema con sscanf(buffer, "SUBSCRIBE %s", topic).  
   - Llama a add_subscriber() para registrar al cliente junto con su dirección.  
   - Si el límite de suscriptores se alcanza, se muestra un aviso en consola.

2. **PUBLISH tema mensaje**  
   - El broker extrae el tema y el contenido del mensaje con sscanf(buffer, "PUBLISH %s %[^\n]", topic, message).  
   - Muestra el mensaje en pantalla:  
     Mensaje recibido para tema: mensaje  
   - Llama a distribute_message() para reenviar el mensaje a todos los suscriptores del mismo tema.


#### 4. Registro de suscriptores
La función add_subscriber() agrega un nuevo suscriptor al arreglo global si hay espacio disponible:

void add_subscriber(struct sockaddr_in addr, socklen_t addr_len, char *topic)

- Guarda la dirección IP y puerto del suscriptor.
- Almacena el nombre del tema.  
- Incrementa el contador global subscriber_count.  
- Imprime un mensaje en consola.

#### 5. Distribución de mensajes
La función distribute_message() reenvía el contenido publicado a todos los suscriptores que estén registrados en el mismo tema.

Por cada suscriptor en el arreglo:
- Compara el tema recibido con el tema almacenado (strcmp(subscribers[i].topic, topic)).
- Si coincide, envía el mensaje mediante sendto() al cliente correspondiente.

#### 6. Finalización
El broker no tiene condición de salida; se ejecuta indefinidamente.  
Antes de salir, se cierra el socket con close(sockfd) para liberar los recursos.

# Librerias

Como se pudo haber notado al leer todo el readme explicativo de TCP y UDP, las funciones que se usarón mas relevantes fueron de la libreria <sys/socket.h>:

### 1. socket()
**Función:** Crea un endpoint de comunicación (socket) y devuelve un descriptor.  
**Uso:** socket(AF_INET, SOCK_DGRAM, 0)  
**Internamente:**  
El kernel reserva una estructura de socket, crea colas de envío y recepción, y devuelve un identificador (file descriptor).  
Es como abrir un “buzón” para recibir mensajes UDP.


### 2. bind()
**Función:** Asocia el socket con una dirección IP y un puerto local.  
**Uso:** bind(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr))  
**Internamente:**  
El kernel vincula el puerto UDP (5926) al socket, de modo que los datagramas que lleguen a ese puerto se redirigen a este proceso.  
Es como registrar el buzón en una dirección postal.


### 3. recvfrom()
**Función:** Recibe datagramas UDP desde la red.  
**Uso:** recvfrom(sockfd, buffer, BUFFER_SIZE, 0, (struct sockaddr *)&client_addr, &addr_len)  
**Internamente:**  
El proceso se bloquea hasta que llega un paquete UDP.  
El kernel copia el contenido del datagrama del buffer del kernel al buffer de usuario y devuelve la dirección del remitente.  
Actúa como “abrir una carta” del buzón.

### 4. sendto()
**Función:** Envía datagramas UDP a una dirección destino.  
**Uso:** sendto(sockfd, message, strlen(message), 0, (struct sockaddr *)&dest, sizeof(dest))  
**Internamente:**  
El kernel toma los datos del buffer del usuario, les agrega las cabeceras IP y UDP, y los pasa a la pila de red para su envío.  
UDP no garantiza entrega ni orden, simplemente lanza el paquete.

### 5. htons() y inet_addr()
**Función:**  
- htons(): convierte números (como el puerto) del orden de bytes del host al orden de bytes de red (big-endian).  
- inet_addr(): convierte una dirección IP en formato texto (ej. "127.0.0.1") a formato binario (usable por la red).

**Qué pasa por debajo:**
- Estas funciones no hacen llamadas al sistema: son utilidades de conversión.  
- Preparan los valores para que el kernel pueda interpretarlos correctamente al enviar o recibir paquetes IP.


### 6. close()
**Función:** Cierra el socket y libera los recursos asociados.  
**Uso:** close(sockfd)  
**Internamente:**  
El kernel destruye la estructura del socket, libera el puerto y descarta cualquier dato pendiente.


# QUIC

# Bibliografia:
https://www.ibm.com/docs/es/i/7.6.0?topic=functions-strtok-r-tokenize-string-restartable

https://www.geeksforgeeks.org/cpp/strtok-strtok_r-functions-c-examples/