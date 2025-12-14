#include "src/common.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

int main(int argc, char **argv) {
    Config config;
    if (!parse_args(argc, argv, &config)) {
        return 1;
    }

    Logger logger;
    init_logger(&logger, config.output_path);
    log_message(&logger, "Старт симуляции, режим: %s", config.mode);

    int rc = 0;
    if (strcmp(config.mode, MODE_SEMAPHORE) == 0) {
        rc = run_semaphore_mode(&config, &logger);
    } else {
        rc = run_condition_mode(&config, &logger);
    }

    log_message(&logger, "Завершение симуляции, код %d", rc);
    close_logger(&logger);
    return rc;
}

