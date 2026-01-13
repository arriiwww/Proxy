#include "proxy.h"
#include "logger.h"

int main(void) {
    logger_init(LOG_DEBUG);

    proxy_config_t cfg;
    cfg.listen_port = 80;

    log_info("starting proxy on port %d", cfg.listen_port);

    int rc = proxy_run(&cfg);
    if (rc != 0) log_error("proxy failed with code %d", rc);

    logger_finalize();
    return rc;
}