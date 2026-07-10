#pragma once

#include <stdbool.h>
#include <stddef.h>

void device_registry_init(void);

void device_registry_get_device_id(char *buf, size_t len);

bool device_registry_get_api_token(char *buf, size_t len);

bool device_registry_ensure_authenticated(const char *upload_url,
                                          const char *fallback_api_key,
                                          const char *registration_secret,
                                          char *status,
                                          size_t status_len);
