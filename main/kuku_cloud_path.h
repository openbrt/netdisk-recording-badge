#pragma once

#include <stdbool.h>
#include <stddef.h>

#define KUKU_CLOUD_ROOT "/apps/网盘录音工牌"
#define KUKU_CLOUD_PATH_MAX 256
#define KUKU_CLOUD_UNDATED "undated"

// Derive the cloud destination from the immutable recording-start filename,
// never from upload time. Legacy sequence names go to undated.
bool kuku_cloud_recording_path(const char *name, char *out, size_t cap);
// Confine navigation to the application directory. Buffers must not alias.
bool kuku_cloud_child_path(const char *dir, const char *name, char *out, size_t cap);
bool kuku_cloud_parent_path(const char *dir, char *out, size_t cap);
