// disk_collector.c
#include "disk_collector.h"
#include "device_info.h" // This includes the new MAX_FULL_PATH_LEN
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>
#include <unistd.h>
#include <sys/sysmacros.h>
#include <libgen.h>
#include <ctype.h>
#include <stdarg.h> // Needed for va_list in get_string_from_output


// Path max and buffer len defines (now primarily driven by device_info.h)
// Remove the specific PATH_MAX and MAX_BUFFER_LEN defines here if they clash
// with device_info.h, or ensure they are consistent.
// Given MAX_FULL_PATH_LEN is in device_info.h, we can remove the #ifndef PATH_MAX block
// and just use MAX_FULL_PATH_LEN directly.
// #ifndef PATH_MAX
// #define PATH_MAX 4096 // This define is less critical here, as we use MAX_FULL_PATH_LEN
// #endif
#define MAX_BUFFER_LEN 128 // This is for general line parsing, keep it as is.

// ... rest of the functions ...

// No changes needed in the snprintf calls themselves, as they already use sizeof(path_buf)
// The increase in MAX_FULL_PATH_LEN in device_info.h will automatically resolve the warnings.
