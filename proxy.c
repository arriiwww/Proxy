#include "proxy.h"
#include "logger.h"
#include <errno.h>
#include <netdb.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

typedef struct client_ctx {
    int client_fd;
} client_ctx_t;

static ssize_t send_all(int fd, const void* buf, size_t len) {
    const char* p = buf;
    size_t left = len;

    while (left > 0) {
        ssize_t n = send(fd, p, left, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) break;
        p += n;
        left -= (size_t)n;
    }
    return (ssize_t)(len - left);
}

static char* memmem_simple(char* haystack, size_t haystack_len, const char* needle, size_t needle_len) {
    if (needle_len == 0 || haystack_len < needle_len) return NULL;

    for (size_t i = 0; i + needle_len <= haystack_len; ++i)
        if (memcmp(haystack + i, needle, needle_len) == 0) return haystack + i;
    return NULL;
}

static char* find_host_header(char* buf, size_t len) {
    const char* needle = "Host:";
    size_t nlen = strlen(needle);

    char* p = memmem_simple(buf, len, needle, nlen);
    if (!p) return NULL;

    p += nlen;
    while (p < buf + len && (*p == ' ' || *p == '\t')) p++;
    return p;
}

static int create_listen_socket(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        log_error("socket failed: %s", strerror(errno));
        return -1;
    }

    int opt = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        log_error("setsockopt SO_REUSEADDR failed: %s", strerror(errno));
        close(fd);
        return -1;
    }

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);

    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        log_error("bind on port %d failed: %s", port, strerror(errno));
        close(fd);
        return -1;
    }

    if (listen(fd, 64) < 0) {
        log_error("listen failed: %s", strerror(errno));
        close(fd);
        return -1;
    }

    return fd;
}

static int connect_to_origin(const char* host, int port) {
    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);

    struct addrinfo hints = {0};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = AI_ADDRCONFIG;

    struct addrinfo* res = NULL;
    int rc = getaddrinfo(host, port_str, &hints, &res);
    if (rc != 0) {
        log_error("getaddrinfo(%s:%s) failed: %s", host, port_str, gai_strerror(rc));
        return -1;
    }

    int fd = -1;
    int last_errno = 0;

    for (struct addrinfo* rp = res; rp != NULL; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0) {
            last_errno = errno;
            continue;
        }

        if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) {
            freeaddrinfo(res);
            log_debug("connected to origin %s:%d, fd=%d", host, port, fd);
            return fd;
        }

        last_errno = errno;
        close(fd);
        fd = -1;
    }

    freeaddrinfo(res);
    log_error("connect_to_origin(%s:%d) failed: %s", host, port, strerror(last_errno));
    return -1;
}

static void handle_client(void* arg) {
    client_ctx_t* ctx = arg;
    int cfd = ctx->client_fd;
    free(ctx);

    log_debug("client fd=%d: handler started", cfd);

    char buf[8192];
    size_t used = 0;

    while (1) {
        ssize_t n = recv(cfd, buf + used, sizeof(buf) - used, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            log_error("client fd=%d: recv failed: %s", cfd, strerror(errno));
            close(cfd);
            return;
        }
        if (n == 0) {
            log_info("client fd=%d: closed connection before headers", cfd);
            close(cfd);
            return;
        }

        used += (size_t)n;

        if (memmem_simple(buf, used, "\r\n\r\n", 4) != NULL) break;

        if (used == sizeof(buf)) {
            log_error("client fd=%d: headers too long", cfd);
            close(cfd);
            return;
        }
    }

    char method[16], raw_uri[1024], version[16];
    if (sscanf(buf, "%15s %1023s %15s", method, raw_uri, version) != 3) {
        log_error("client fd=%d: failed to parse request line", cfd);
        close(cfd);
        return;
    }

    log_debug("client fd=%d: method=%s raw_uri=%s version=%s", cfd, method, raw_uri, version);

    // Извлекаем host и uri правильно
    char host[1024] = {0};
    char uri[1024] = "/";  // default

    if (strncmp(raw_uri, "http://", 7) == 0) {
        // Proxy-style: абсолютный URI
        char* p = raw_uri + 7;
        char* host_end = strchr(p, '/');
        if (!host_end) host_end = p + strlen(p);  // нет path

        size_t host_len = host_end - p;
        if (host_len >= sizeof(host)) host_len = sizeof(host) - 1;
        strncpy(host, p, host_len);
        host[host_len] = '\0';

        if (*host_end == '/') strcpy(uri, host_end);
        else strcpy(uri, "/");

        log_debug("client fd=%d: proxy-style request, extracted host=%s uri=%s", cfd, host, uri);
    } else {
        // Обычный request: относительный URI
        snprintf(uri, sizeof(uri), "%s", raw_uri);


        char* host_start = find_host_header(buf, used);
        if (!host_start) {
            log_error("client fd=%d: no Host header in non-proxy request", cfd);
            close(cfd);
            return;
        }

        char* host_end = memchr(host_start, '\r', used - (host_start - buf));
        if (!host_end) host_end = buf + used;
        size_t host_len = host_end - host_start;
        if (host_len >= sizeof(host)) host_len = sizeof(host) - 1;
        strncpy(host, host_start, host_len);
        host[host_len] = '\0';

        log_debug("client fd=%d: non-proxy request, host=%s uri=%s", cfd, host, uri);
    }

    // Защита от self-loop
    if (strcmp(host, "localhost") == 0 || strcmp(host, "127.0.0.1") == 0) {
        log_error("client fd=%d: self-referential request to localhost, rejecting", cfd);
        const char* resp = "HTTP/1.0 403 Forbidden\r\nConnection: close\r\nContent-Length: 0\r\n\r\n";
        send_all(cfd, resp, strlen(resp));
        close(cfd);
        return;
    }

    int ofd = connect_to_origin(host, 80);
    if (ofd < 0) {
        const char* resp =
            "HTTP/1.0 502 Bad Gateway\r\n"
            "Connection: close\r\n"
            "Content-Length: 0\r\n"
            "\r\n";

        (void)send_all(cfd, resp, strlen(resp));
        close(cfd);
        return;
    }

    log_info("client fd=%d: connected to origin %s:80, origin fd=%d", cfd, host, ofd);

    char req[4096];
    int req_len = snprintf(req, sizeof(req),
                           "GET %s HTTP/1.0\r\n"
                           "Host: %s\r\n"
                           "Connection: close\r\n"
                           "\r\n",
                           uri, host);
    if (req_len <= 0 || req_len >= (int)sizeof(req)) {
        log_error("client fd=%d: failed to build HTTP/1.0 request", cfd);
        close(ofd);
        close(cfd);
        return;
    }

    log_debug("client fd=%d: sending request to origin:\n%.*s", cfd, req_len, req);

    if (send_all(ofd, req, (size_t)req_len) < 0) {
        log_error("client fd=%d: send_all to origin failed: %s", cfd, strerror(errno));
        close(ofd);
        close(cfd);
        return;
    }

    char serv_buf[8192];
    size_t total_from_origin = 0;
    while (1) {
        ssize_t n = recv(ofd, serv_buf, sizeof(serv_buf), 0);
        if (n < 0) {
            if (errno == EINTR) continue;

            if (errno == ECONNRESET) {
                if (total_from_origin > 0) {
                    log_debug("client fd=%d: origin reset after %zu bytes, treat as complete", cfd, total_from_origin);
                } else {
                    log_error("client fd=%d: origin reset before any data", cfd);
                }
                break;
            }

            log_error("client fd=%d: recv from origin failed: %s", cfd, strerror(errno));
            break;
        }
        if (n == 0) {
            log_debug("client fd=%d: origin closed connection", cfd);
            break;
        }

        total_from_origin += (size_t)n;

        if (send_all(cfd, serv_buf, (size_t)n) < 0) {
            if (errno == EPIPE || errno == ECONNRESET) {
                log_info("client fd=%d: closed connection while writing", cfd);
            } else {
                log_error("client fd=%d: send_all to client failed: %s", cfd, strerror(errno));
            }
            break;
        }
    }

    close(ofd);
    close(cfd);
    log_debug("client fd=%d: handler finished", cfd);
}

int proxy_run(const proxy_config_t* cfg) {
    signal(SIGPIPE, SIG_IGN);

    int lfd = create_listen_socket(cfg->listen_port);
    if (lfd < 0) {
        log_error("failed to create listen socket on port %d", cfg->listen_port);
        return 1;
    }

    log_info("proxy listening on port %d", cfg->listen_port);

    while (1) {
        struct sockaddr_in cli_addr;
        socklen_t cli_len = sizeof(cli_addr);
        int cfd = accept(lfd, (struct sockaddr*)&cli_addr, &cli_len);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            log_error("accept failed: %s", strerror(errno));
            break;
        }

        log_debug("accepted client fd=%d", cfd);

        client_ctx_t* ctx = malloc(sizeof(*ctx));
        if (!ctx) {
            log_error("malloc client_ctx failed");
            close(cfd);
            continue;
        }

        ctx->client_fd = cfd;

        pthread_t tid;
        int err = pthread_create(&tid, NULL, (void* (*)(void*))handle_client, ctx);
        if (err != 0) {
            log_error("pthread_create failed for client fd=%d: %s", cfd, strerror(err));
            close(cfd);
            free(ctx);
            continue;
        }

        pthread_detach(tid);
    }

    close(lfd);
    log_info("proxy stopped");
    return 0;
}