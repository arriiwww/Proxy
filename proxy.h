#ifndef CACHE_PROXY_PROXY_H
#define CACHE_PROXY_PROXY_H

typedef struct proxy_config {
    int listen_port;
} proxy_config_t;

int proxy_run(const proxy_config_t* cfg);

#endif