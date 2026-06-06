#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <string.h>
#include <netdb.h>

// the objective isnt to build prod grade server
// i just wanna learn how http works under the hood -> raw TCP sockets + manual HTTP parsing

#define PORT    "8080"
#define BACKLOG 10
#define MAX_LEN 4096  // max size of incoming HTTP request

// read_file() -> reads entire file into heap buffer
// caller must free() the returned pointer
// sets *file_size to number of bytes read
// returns NULL on error
char *read_file(const char *path, long *file_size) {
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        perror("fopen");
        return NULL;
    }

    // get file size
    fseek(f, 0, SEEK_END);
    *file_size = ftell(f);
    fseek(f, 0, SEEK_SET);  // rewind to start before reading

    // allocate heap buffer -> no VLA, safe for large files
    char *buf = malloc(*file_size + 1);
    if (buf == NULL) {
        perror("malloc");
        fclose(f);
        return NULL;
    }

    // fread() -> read file_size bytes into buf
    // returns number of bytes actually read -> check for short read
    size_t n_read = fread(buf, 1, *file_size, f);
    if (n_read != (size_t)*file_size) {
        fprintf(stderr, "fread: short read\n");
        free(buf);
        fclose(f);
        return NULL;
    }

    buf[*file_size] = '\0';  // null-terminate -> safe to use as string
    fclose(f);
    return buf;
}

// setup_server() -> socket + bind + listen in one place
// returns sockfd >= 0 on success, -1 on error
int setup_server(const char *port) {
    struct addrinfo hints, *res;
    int sockfd;

    memset(&hints, 0, sizeof hints);
    hints.ai_family   = AF_UNSPEC;    // AF_INET -> IPv4, AF_INET6 -> IPv6, AF_UNSPEC -> either
    hints.ai_socktype = SOCK_STREAM;  // SOCK_STREAM -> TCP, SOCK_DGRAM -> UDP
    hints.ai_flags    = AI_PASSIVE;   // fill in local IP for me

    if (getaddrinfo(NULL, port, &hints, &res) != 0) {
        perror("getaddrinfo");
        return -1;
    }

    // socket() -> kernel allocates socket structure
    //          -> creates send/receive buffers
    //          -> returns file descriptor (sockfd >= 0) or -1 on error
    sockfd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sockfd == -1) {
        perror("socket");
        freeaddrinfo(res);
        return -1;
    }

    // SO_REUSEADDR -> allows reuse of port immediately after crash/restart
    // without this -> bind() fails with "Address already in use" for ~60s
    int yes = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);

    // bind() -> attach socket to port
    // on success -> zero returned, -1 on error
    if (bind(sockfd, res->ai_addr, res->ai_addrlen) == -1) {
        perror("bind");
        freeaddrinfo(res);
        close(sockfd);
        return -1;
    }

    // res no longer needed after bind() -> free the linked list
    freeaddrinfo(res);

    // listen() -> mark socket as passive (waiting for connections, not making them)
    // backlog -> number of connections allowed in incoming queue
    // listen() returns -1 on error
    if (listen(sockfd, BACKLOG) == -1) {
        perror("listen");
        close(sockfd);
        return -1;
    }

    return sockfd;
}

// send_all() -> TCP may not send all bytes in one call
// loop until entire buffer is sent
// returns -1 on error, 0 on success
int send_all(int fd, const char *buf, size_t len) {
    size_t total = 0;
    while (total < len) {
        int n = send(fd, buf + total, len - total, 0);
        if (n == -1) {
            perror("send");
            return -1;
        }
        total += n;
    }
    return 0;
}

// handle_client() -> recv request, parse method, send response
void handle_client(int new_sockfd) {
    char http_request[MAX_LEN];

    // recv() -> read incoming data into http_request buffer
    // MAX_LEN - 1 -> leave room for null terminator
    // returns number of bytes received, -1 on error
    int bytes_received = recv(new_sockfd, http_request, MAX_LEN - 1, 0);
    if (bytes_received == -1) {
        perror("recv");
        return;
    }
    http_request[bytes_received] = '\0';  // null-terminate -> safe strncmp

    // strncmp() -> compare first 3 bytes only
    // returns 0 on match (not 1!)
    if (strncmp(http_request, "GET", 3) == 0) {
        printf("Received GET request\n");

        // read index.html into heap buffer
        long file_size;
        char *body = read_file("index.html", &file_size);
        if (body == NULL) {
            // send 500 if file read fails
            const char *err = "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 0\r\n\r\n";
            send_all(new_sockfd, err, strlen(err));
            return;
        }

        // build HTTP response
        // header + blank line (\r\n) + body
        // 256 -> header headroom on top of file size
        size_t response_size = file_size + 256;
        char *response = malloc(response_size);
        if (response == NULL) {
            perror("malloc");
            free(body);
            return;
        }

        snprintf(response, response_size,
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/html\r\n"
            "Content-Length: %ld\r\n"
            "Connection: close\r\n"
            "\r\n"           // blank line -> separates headers from body (required)
            "%s",
            file_size, body
        );

        free(body);  // body copied into response -> safe to free

        int result = send_all(new_sockfd, response, strlen(response));
        if (result == 0)
            printf("Response sent (%zu bytes)\n", strlen(response));

        free(response);

    } else {
        // method not GET -> 405 Method Not Allowed
        const char *r = "HTTP/1.1 405 Method Not Allowed\r\nContent-Length: 0\r\n\r\n";
        send_all(new_sockfd, r, strlen(r));
    }
}

int main(void) {
    struct sockaddr_storage their_addr;  // big enough for IPv4 or IPv6 client address
    socklen_t addr_size;
    int sockfd, new_sockfd;

    sockfd = setup_server(PORT);
    if (sockfd == -1) return 1;

    printf("server is listening on port %s\n", PORT);

    // accept() -> blocks until client connects
    // returns new_sockfd -> new fd just for this client
    // sockfd -> stays open, keeps accepting new clients (front door)
    // new_sockfd -> private channel for this one client (read/write here)
    while (1) {
        addr_size  = sizeof their_addr;
        new_sockfd = accept(sockfd, (struct sockaddr *)&their_addr, &addr_size);
        if (new_sockfd == -1) {
            perror("accept");
            continue;  // don't die on one bad accept -> keep looping
        }

        handle_client(new_sockfd);
        close(new_sockfd);  // done with this client -> close private channel
    }

    close(sockfd);  // unreachable in infinite loop but good habit
    return 0;
}