// scan_options.h
#ifndef SCAN_OPTIONS_H
#define SCAN_OPTIONS_H

#define BLOCK_SIZE_DEFAULT          4096
#define MIN_REPORT_INTERVAL         1000
#define DEFAULT_SUSPECT_THRESHOLD   100
#define DEFAULT_SUSPECT_RETRIES     10
#define DEFAULT_SUSPECT_INTERVAL    100

#include <stddef.h>

typedef struct {
    const char *device;
    const char *start_str;
    const char *end_str;
    size_t      block_size;
    const char *log_filename;
    int         log_threshold;
    const char *config_file;
    double      sample_ratio;
    int         random_sampling;
    int         wait_factor;
    int         suspect_threshold;
    int         suspect_retries;
    int         suspect_interval;
} ScanOptions;

// 函数声明
int parse_arguments(int argc, char *argv[], ScanOptions *opts);

#endif // SCAN_OPTIONS_H
