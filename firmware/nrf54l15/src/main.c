#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(gw_main, LOG_LEVEL_INF);
int gw_app_run(void);

int main(void)
{
    int err = gw_app_run();
    LOG_ERR("Gateway stopped: %d", err);
    return err;
}
