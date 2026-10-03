#pragma once
#include <stdbool.h>
#include <stddef.h>

// Build the official device authorization URL with the user code prefilled.
bool kuku_baidu_auth_link(const char *code, char *url, size_t cap);
