#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <signal.h>
#include <netdb.h>
#include <fcntl.h>
#include <arpa/inet.h>

#define MAX_DEST_HOSTS 3
#define MAX_CLIENTS 5
#define BUF_SIZE 100
#define TIMEOUT_SECS 15
#define INET_PORTSTRLEN 6

#define ERR(source) (perror(source),                                 \
                     fprintf(stderr, "%s:%d\n", __FILE__, __LINE__), \
                     exit(EXIT_FAILURE))

volatile sig_atomic_t do_work = 1;

struct host
{
    char *ip;
    char *port;
    int fd;
};

int sethandler(void (*f)(int), int sigNo);
void sigint_handler(int sig);

struct sockaddr_in make_ipv4_address(const char *address, const char *port);
int make_socket(int domain, int type);
int bind_tcp_socket(char *host, char *port);
void set_socket_non_blocking(int fd);

struct host add_new_client(int serverfd);
int connect_socket(char *host, char *port);
ssize_t bulk_write(int fd, char *buf, size_t count);

void server_work(struct host server, struct host dest_hosts[], int dest_host_num);
void add_client_to_server(int serverfd, int *max_fd, struct host clients[], int max_client, fd_set *rfds);
void stdin_to_dest(struct host dest_hosts[], int dest_host_num, fd_set *base_rfds, char *buf, int buf_size);
void check_if_disconnected(struct host *host, fd_set *base_rfds);
void read_from_client(struct host *host, fd_set *base_rfds, char *buf, int buf_size);
void close_client(struct host *host);

void usage(char *name)
{
    fprintf(stderr, "USAGE: %s <host> <port> [<dest_host> <dest_port>]\n", name);
}

int main(int argc, char **argv)
{
    if (sethandler(sigint_handler, SIGINT))
        ERR("sethandler");

    if (sethandler(SIG_IGN, SIGPIPE))
        ERR("sethandler");

    if (argc < 3 || argc % 2 != 1 || argc > 9)
        usage(argv[0]);

    struct host server = {argv[1], argv[2], bind_tcp_socket(argv[1], argv[2])};
    set_socket_non_blocking(server.fd);

    int dest_host_num = 0;
    struct host dest_hosts[MAX_DEST_HOSTS];
    for (int i = 3; i < argc; i += 2)
    {
        int fd = connect_socket(argv[i], argv[i + 1]);
        if (fd != 0)
        {
            dest_hosts[dest_host_num] = (struct host){argv[i], argv[i + 1], fd};
            printf("Connected to %s:%s\n", dest_hosts[dest_host_num].ip, dest_hosts[dest_host_num].port);
            dest_host_num++;
        }
    }

    server_work(server, dest_hosts, dest_host_num);

    for (int i = 0; i < dest_host_num; i++)
        if (dest_hosts[i].fd != 0 && TEMP_FAILURE_RETRY(close(dest_hosts[i].fd)) < 0)
            ERR("close");

    if (TEMP_FAILURE_RETRY(close(server.fd)) < 0)
        ERR("close");
    fprintf(stderr, "Server has terminated.\n");

    return EXIT_SUCCESS;
}

void server_work(struct host server, struct host dest_hosts[], int dest_host_num)
{
    struct host clients[MAX_CLIENTS] = {};
    int retval, max_fd;
    fd_set base_rfds, rfds;
    sigset_t mask, oldmask;
    struct timespec timeout = {};
    char buf[BUF_SIZE];

    FD_ZERO(&base_rfds);
    FD_SET(server.fd, &base_rfds);
    FD_SET(STDIN_FILENO, &base_rfds);
    max_fd = server.fd;

    for (int i = 0; i < dest_host_num; i++)
    {
        FD_SET(dest_hosts[i].fd, &base_rfds);
        if (dest_hosts[i].fd > max_fd)
            max_fd = dest_hosts[i].fd;
    }

    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigprocmask(SIG_BLOCK, &mask, &oldmask);

    while (do_work)
    {
        rfds = base_rfds;
        timeout.tv_sec = TIMEOUT_SECS;

        if ((retval = pselect(max_fd + 1, &rfds, NULL, NULL, &timeout, &oldmask)) > 0)
        {
            if (FD_ISSET(server.fd, &rfds))
                add_client_to_server(server.fd, &max_fd, clients, MAX_CLIENTS, &base_rfds);

            if (FD_ISSET(STDIN_FILENO, &rfds))
                stdin_to_dest(dest_hosts, dest_host_num, &base_rfds, buf, sizeof buf);

            for (int i = 0; i < dest_host_num; i++)
                if (dest_hosts[i].fd != 0 && FD_ISSET(dest_hosts[i].fd, &rfds))
                    check_if_disconnected(&dest_hosts[i], &base_rfds);

            for (int i = 0; i < MAX_CLIENTS; i++)
                if (clients[i].fd != 0 && FD_ISSET(clients[i].fd, &rfds))
                    read_from_client(&clients[i], &base_rfds, buf, sizeof buf);
        }
        else
        {
            if (retval == 0)
            {
                printf("Timeout\n");
                break;
            }
            if (do_work == 0)
                break;
            if (EINTR == errno)
                continue;
            ERR("pselect");
        }
    }
    sigprocmask(SIG_UNBLOCK, &mask, NULL);

    for (int i = 0; i < MAX_CLIENTS; i++)
        if (clients[i].fd != 0)
            close_client(&clients[i]);      
}

void add_client_to_server(int serverfd, int *max_fd, struct host clients[], int max_client, fd_set *base_rfds)
{
    struct host new_client = add_new_client(serverfd);
    if (new_client.fd > 0)
    {
        int i;
        for (i = 0; i < max_client; i++)
        {
            if (clients[i].fd == 0)
            {
                clients[i] = new_client;
                FD_SET(new_client.fd, base_rfds);
                if (new_client.fd > *max_fd)
                    *max_fd = new_client.fd;
                break;
            }
        }

        if (i == max_client)
        {
            char *msg = "Server is full. Try later.\n";
            if (bulk_write(new_client.fd, msg, strlen(msg)) < 0)
                ERR("write");
            close_client(&new_client);
            return;
        }

        char *msg = "Connection established.\n";
        if (bulk_write(new_client.fd, msg, strlen(msg)) < 0)
            ERR("write");

        printf("Client %s:%s connected.\n", new_client.ip, new_client.port);
    }
}

void stdin_to_dest(struct host dest_hosts[], int dest_host_num, fd_set *base_rfds, char *buf, int buf_size)
{
    ssize_t n = read(STDIN_FILENO, buf, BUF_SIZE);
    if (n <= 0)
        ERR("read");

    for (int i = 0; i < dest_host_num; i++)
    {
        if (dest_hosts[i].fd != 0 && bulk_write(dest_hosts[i].fd, buf, n) < n)
        {
            fprintf(stderr, "Error writing to destination host %s:%s\n", dest_hosts[i].ip, dest_hosts[i].port);
            if (EPIPE == errno || ECONNRESET == errno)
            {
                fprintf(stderr, "Destination host %s:%s has disconnected.\n", dest_hosts[i].ip, dest_hosts[i].port);
                FD_CLR(dest_hosts[i].fd, base_rfds);
                if (TEMP_FAILURE_RETRY(close(dest_hosts[i].fd)) < 0)
                    ERR("close");
                dest_hosts[i].fd = 0;
            }
        }
    }
}

void check_if_disconnected(struct host *host, fd_set *base_rfds)
{
    if (TEMP_FAILURE_RETRY(read(host->fd, NULL, 0)) == 0)
    {
        fprintf(stderr, "Dest host %s:%s has disconnected.\n", host->ip, host->port);
        FD_CLR(host->fd, base_rfds);
        if (TEMP_FAILURE_RETRY(close(host->fd)) < 0)
            ERR("close");
        host->fd = 0;
    }
}

void read_from_client(struct host *host, fd_set *base_rfds, char *buf, int buf_size)
{
    ssize_t n = read(host->fd, buf, buf_size);
    if (n > 0)
    {
        if (bulk_write(STDOUT_FILENO, buf, n - 1) < n - 1)
            ERR("write");
        printf(" - %s:%s\n", host->ip, host->port);
    }
    else if (n == 0)
    {
        printf("Client %s:%s has disconnected.\n", host->ip, host->port);
        FD_CLR(host->fd, base_rfds);
        close_client(host);
    }
    else
        ERR("read");
}

void close_client(struct host *host)
{
    if (TEMP_FAILURE_RETRY(close(host->fd)) < 0)
        ERR("close");
    free(host->ip);
    free(host->port);
    host->fd = 0;
}

// Socket connection //

int connect_socket(char *host, char *port)
{
    struct sockaddr_in addr = make_ipv4_address(host, port);
    int socketfd = make_socket(PF_INET, SOCK_STREAM);

    if (connect(socketfd, (struct sockaddr *)&addr, sizeof addr) < 0)
    {
        if (errno != EINTR)
            return 0;
        else
        {
            fd_set wfds;
            int status;
            socklen_t size = sizeof(int);
            FD_ZERO(&wfds);
            FD_SET(socketfd, &wfds);
            if (TEMP_FAILURE_RETRY(select(socketfd + 1, NULL, &wfds, NULL, NULL)) < 0)
                ERR("select");
            if (getsockopt(socketfd, SOL_SOCKET, SO_ERROR, &status, &size) < 0)
                ERR("getsockopt");
            if (0 != status)
                return 0;
        }
    }
    return socketfd;
}

struct host add_new_client(int serverfd)
{
    struct host new_client;
    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(addr);

    if ((new_client.fd = TEMP_FAILURE_RETRY(accept(serverfd, &addr, &addr_len))) < 0)
    {
        if (EAGAIN == errno || EWOULDBLOCK == errno)
        {
            new_client.fd = 0;
            return new_client;
        };
        ERR("accept");
    }

    if (addr.sin_family != AF_INET || addr_len != sizeof(struct sockaddr_in))
    {
        if (TEMP_FAILURE_RETRY(close(new_client.fd)) < 0)
            ERR("close");
        new_client.fd = 0;
        return new_client;
    }

    new_client.ip = malloc(INET_ADDRSTRLEN);
    new_client.port = malloc(INET_PORTSTRLEN);
    if (new_client.ip == NULL || new_client.port == NULL)
        ERR("malloc");

    if (NULL == inet_ntop(addr.sin_family, &addr.sin_addr, new_client.ip, INET_ADDRSTRLEN))
        ERR("inet_ntop");
    if (snprintf(new_client.port, INET_PORTSTRLEN, "%.5d", ntohs(addr.sin_port)) < 0)
        ERR("sprintf");

    return new_client;
}

ssize_t bulk_write(int fd, char *buf, size_t count)
{
    int c;
    size_t len = 0;
    do
    {
        c = TEMP_FAILURE_RETRY(write(fd, buf, count));
        if (c < 0)
            return c;
        buf += c;
        len += c;
        count -= c;
    } while (count > 0);
    return len;
}

// Socket management //

int bind_tcp_socket(char *host, char *port)
{
    struct sockaddr_in addr = make_ipv4_address(host, port);
    int set = 1;
    int socketfd = make_socket(PF_INET, SOCK_STREAM);

    if (setsockopt(socketfd, SOL_SOCKET, SO_REUSEADDR, &set, sizeof set))
        ERR("setsockopt");
    if (bind(socketfd, (struct sockaddr *)&addr, sizeof addr) < 0)
        ERR("bind");
    if (listen(socketfd, MAX_CLIENTS + 1) < 0)
        ERR("listen");

    return socketfd;
}

int make_socket(int domain, int type)
{
    int sock;
    sock = socket(domain, type, 0);
    if (sock < 0)
        ERR("socket");
    return sock;
}

struct sockaddr_in make_ipv4_address(const char *address, const char *port)
{
    int ret;
    struct sockaddr_in addr;
    struct addrinfo *result;
    struct addrinfo hints = {};
    hints.ai_family = AF_INET;

    if ((ret = getaddrinfo(address, port, &hints, &result)) != 0)
    {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(ret));
        exit(EXIT_FAILURE);
    }

    addr = *(struct sockaddr_in *)(result->ai_addr);
    freeaddrinfo(result);
    return addr;
}

void set_socket_non_blocking(int socketfd)
{
    int old_flags;
    if ((old_flags = fcntl(socketfd, F_GETFL, 0)) < 0)
        ERR("fcntl");
    if (fcntl(socketfd, F_SETFL, old_flags | O_NONBLOCK) < 0)
        ERR("fcntl");
}

// Signal handling //

int sethandler(void (*f)(int), int sigNo)
{
    struct sigaction act;
    memset(&act, 0, sizeof(struct sigaction));
    act.sa_handler = f;
    if (-1 == sigaction(sigNo, &act, NULL))
        return -1;
    return 0;
}

void sigint_handler(int sig)
{
    do_work = 0;
    printf("\n");
}