#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <arpa/inet.h>  // htons(), INADDR_ANY
#include <sys/socket.h> // socket(), bind(), listen(), accept()
#include <sys/epoll.h>  // epoll_create1(), epoll_ctl(), epoll_wait()

#define PORT 4000
#define MAX_EVENTS 64
#define BUF_SIZE 4096

// Put a file descriptor into non-blocking mode
int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

int main(void) {
    // IPv4 TCP socket
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == -1) {
        perror("socket failed");
        return EXIT_FAILURE;
    }

    // Allow instant restart without "Address already in use" (TIME_WAIT) errors
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // Bind to 0.0.0.0:4000
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(PORT); // Host-to-Network byte order (big-endian)

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
        perror("Bind failed.");
        close(server_fd);
        return EXIT_FAILURE;
    }

    // Start listening (kernel queues.up to 128 pending TCP handshakes)
    if (listen(server_fd, 128) == -1) {
        perror("listen failed");
        close(server_fd);
        return EXIT_FAILURE;
    }

    // Make the listening socket non-blocking
    if (set_nonblocking(server_fd) == -1) {
        perror("set_nonblocking server_fd failed.");
        close(server_fd);
        return EXIT_FAILURE;
    }

    // Creating the epoll instance in the kernel
    int epfd = epoll_create1(0);
    if (epfd == -1) {
        perror("epoll_create1 failed.");
        close(server_fd);
        return EXIT_FAILURE;
    }

    // Register server_fd to wake us up on incoming connections
    struct epoll_event ev = {0};
    ev.events = EPOLLIN;
    ev.data.fd = server_fd;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, server_fd, &ev) == -1) {
        perror("epoll_ctl ADD server_fd failed");
        close(epfd);
        close(server_fd);
        return EXIT_FAILURE;
    }

    printf("Epoll HTTP server listening on http://localhost:%d\n", PORT);

    const char *response = 
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: 14\r\n"
        "Connection: close\r\n"
        "r\n"
        "Mini UV built with C.\n";

    struct epoll_event events[MAX_EVENTS];

    // The Event Loop (this is what libuv runs inside Node.js)
    while (1) {
        // Blocks until at least one registered FD is ready for I/O
        int n_ready = epoll_wait(epfd, events, MAX_EVENTS, -1);
        if (n_ready == -1) {
            if (errno == EINTR) continue; // Interrupted by signal, retry
            perror("epoll_wait failed");
            break;
        }

        for (int i = 0; i < n_ready; i++) {
            int fd = events[i].data.fd;

            if (fd == server_fd) {
                // Case A: One or more new clients are trying to connect
                while (1) {
                    int client_fd = accept(server_fd, NULL, NULL);
                    if (client_fd == -1) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            break; // No more pending connections right now
                        }
                        perror("accept failed");
                        break;
                    }

                    set_nonblocking(client_fd);

                    struct epoll_event client_ev = {0};
                    client_ev.events = EPOLLIN;
                    client_ev.data.fd = client_fd;
                    epoll_ctl(epfd, EPOLL_CTL_ADD, client_fd, &client_ev);
                    printf("[+] Accepted client fd=%d\n", client_fd);
                }
            } else {
                // Case B: An existing client socket has data ready to read
                char buf[BUF_SIZE];
                // Blocks here until the client actually sends bytes
                ssize_t n = read(fd, buf, sizeof(buf) - 1);

                if (n > 0) {
                    buf[n] = '\0';
                    printf("[<] Received %zd bytes (fd=%d)n", n, fd);
                    write(fd, response, strlen(response));
                }

                // Clean up: remove from epoll and close the socket
                epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
                close(fd);
                printf("[-] Closed client fd=%d\n", fd);
            }
        }  
    }

    close(epfd);
    close(server_fd);
    return EXIT_SUCCESS;
}