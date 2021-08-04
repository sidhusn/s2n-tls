/*
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License").
 * You may not use this file except in compliance with the License.
 * A copy of the License is located at
 *
 *  http://aws.amazon.com/apache2.0
 *
 * or in the "license" file accompanying this file. This file is distributed
 * on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either
 * express or implied. See the License for the specific language governing
 * permissions and limitations under the License.
 */

#pragma once

#include <stdint.h>
#include <s2n.h>

#define GUARD_EXIT_NULL(x)                                 \
    do {                                                   \
        if (x == NULL) {                                   \
            fprintf(stderr, "NULL pointer encountered\n"); \
            exit(1);                                       \
        }                                                  \
    } while (0)

#define GUARD_EXIT(x, msg)  \
  do {                      \
    if ((x) < 0) {          \
      print_s2n_error(msg); \
      exit(1);              \
    }                       \
  } while (0)

#define GUARD_RETURN(x, msg) \
  do {                       \
    if ((x) < 0) {           \
      print_s2n_error(msg);  \
      return -1;             \
    }                        \
  } while (0)

#define S2N_MAX_PSK_LIST_LENGTH 10
#define MAX_KEY_LEN 32
#define MAX_VAL_LEN 255

struct session_cache_entry {
    uint8_t key[MAX_KEY_LEN];
    uint8_t key_len;
    uint8_t value[MAX_VAL_LEN];
    uint8_t value_len;
};

struct session_cache_entry session_cache[256];

struct verify_data {
    const char *trusted_host;
};

void print_s2n_error(const char *app_error);
int echo(struct s2n_connection *conn, int sockfd, bool *stop_echo);
int wait_for_event(int fd, s2n_blocked_status blocked);
int negotiate(struct s2n_connection *conn, int sockfd);
int early_data_recv(struct s2n_connection *conn);
int early_data_send(struct s2n_connection *conn, uint8_t *data, uint32_t len);
void print_connection_data(struct s2n_connection *conn, bool session_resumed);
void psk_early_data(struct s2n_connection *conn, bool session_resumed);
int https(struct s2n_connection *conn, uint32_t bench);
int key_log_callback(void *ctx, struct s2n_connection *conn, uint8_t *logline, size_t len);

int cache_store_callback(struct s2n_connection *conn, void *ctx, uint64_t ttl, const void *key, uint64_t key_size, const void *value, uint64_t value_size);
int cache_retrieve_callback(struct s2n_connection *conn, void *ctx, const void *key, uint64_t key_size, void *value, uint64_t *value_size);
int cache_delete_callback(struct s2n_connection *conn, void *ctx, const void *key, uint64_t key_size);

char *load_file_to_cstring(const char *path);
int s2n_str_hex_to_bytes(const unsigned char *hex, uint8_t *out_bytes, uint32_t max_out_bytes_len);
int s2n_setup_external_psk_list(struct s2n_connection *conn, char *psk_optarg_list[S2N_MAX_PSK_LIST_LENGTH], size_t psk_list_len);
uint8_t unsafe_verify_host(const char *host_name, size_t host_name_len, void *data);