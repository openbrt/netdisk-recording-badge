#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

// Build a user-facing recording filename. Valid wall-clock time is rendered in
// China Standard Time; duplicate=0 is the base name and duplicate>0 appends a
// numeric suffix. Without valid time, the persistent sequence is used.
bool kuku_rec_filename_build(time_t epoch, uint32_t sequence, unsigned duplicate,
                             char *out, size_t cap);
