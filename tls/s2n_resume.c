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
#include <math.h>
#include <sys/param.h>

#include <s2n.h>

#include "error/s2n_errno.h"
#include "stuffer/s2n_stuffer.h"
#include "utils/s2n_safety.h"
#include "utils/s2n_blob.h"
#include "utils/s2n_random.h"
#include "utils/s2n_set.h"

#include "tls/s2n_cipher_suites.h"
#include "tls/s2n_connection.h"
#include "tls/s2n_resume.h"
#include "tls/s2n_crypto.h"
#include "tls/s2n_tls.h"

int s2n_allowed_to_cache_connection(struct s2n_connection *conn)
{
    /* We're unable to cache connections with a Client Cert since we currently don't serialize the Client Cert,
     * which means that callers won't have access to the Client's Cert if the connection is resumed. */
    if (s2n_connection_is_client_auth_enabled(conn) > 0) {
        return 0;
    }

    struct s2n_config *config = conn->config;

    POSIX_ENSURE_REF(config);
    return config->use_session_cache;
}

static int s2n_tls12_serialize_resumption_state(struct s2n_connection *conn, struct s2n_stuffer *to)
{
    POSIX_ENSURE_REF(conn);
    POSIX_ENSURE_REF(to);

    uint64_t now;

    S2N_ERROR_IF(s2n_stuffer_space_remaining(to) < S2N_TLS12_STATE_SIZE_IN_BYTES, S2N_ERR_STUFFER_IS_FULL);

    /* Get the time */
    POSIX_GUARD(conn->config->wall_clock(conn->config->sys_clock_ctx, &now));

    /* Write the entry */
    POSIX_GUARD(s2n_stuffer_write_uint8(to, S2N_TLS12_SERIALIZED_FORMAT_VERSION));
    POSIX_GUARD(s2n_stuffer_write_uint8(to, conn->actual_protocol_version));
    POSIX_GUARD(s2n_stuffer_write_bytes(to, conn->secure.cipher_suite->iana_value, S2N_TLS_CIPHER_SUITE_LEN));
    POSIX_GUARD(s2n_stuffer_write_uint64(to, now));
    POSIX_GUARD(s2n_stuffer_write_bytes(to, conn->secrets.master_secret, S2N_TLS_SECRET_LEN));
    POSIX_GUARD(s2n_stuffer_write_uint8(to, conn->ems_negotiated));

    return S2N_SUCCESS;
}

static S2N_RESULT s2n_tls13_serialize_keying_material_expiration(struct s2n_connection *conn,
        uint64_t now, struct s2n_stuffer *out)
{
    RESULT_ENSURE_REF(conn);
    RESULT_ENSURE_REF(out);

    if (conn->mode != S2N_SERVER) {
        return S2N_RESULT_OK;
    }

    uint64_t expiration_timestamp = now + (conn->server_keying_material_lifetime * (uint64_t) ONE_SEC_IN_NANOS);

    struct s2n_psk *chosen_psk = conn->psk_params.chosen_psk;
    if (chosen_psk && chosen_psk->type == S2N_PSK_TYPE_RESUMPTION) {
        expiration_timestamp = MIN(chosen_psk->keying_material_expiration, expiration_timestamp);
    }

    RESULT_GUARD_POSIX(s2n_stuffer_write_uint64(out, expiration_timestamp));
    return S2N_RESULT_OK;
}

static S2N_RESULT s2n_tls13_serialize_resumption_state(struct s2n_connection *conn, struct s2n_stuffer *out)
{
    RESULT_ENSURE_REF(conn);
    RESULT_ENSURE_REF(out);

    uint64_t current_time = 0;
    struct s2n_ticket_fields *ticket_fields = &conn->tls13_ticket_fields;

    /* Get the time */
    RESULT_GUARD_POSIX(conn->config->wall_clock(conn->config->sys_clock_ctx, &current_time));

    RESULT_GUARD_POSIX(s2n_stuffer_write_uint8(out, S2N_TLS13_SERIALIZED_FORMAT_VERSION));
    RESULT_GUARD_POSIX(s2n_stuffer_write_uint8(out, conn->actual_protocol_version));
    RESULT_GUARD_POSIX(s2n_stuffer_write_bytes(out, conn->secure.cipher_suite->iana_value, S2N_TLS_CIPHER_SUITE_LEN));
    RESULT_GUARD_POSIX(s2n_stuffer_write_uint64(out, current_time));
    RESULT_GUARD_POSIX(s2n_stuffer_write_uint32(out, ticket_fields->ticket_age_add));
    RESULT_ENSURE_LTE(ticket_fields->session_secret.size, UINT8_MAX);
    RESULT_GUARD_POSIX(s2n_stuffer_write_uint8(out, ticket_fields->session_secret.size));
    RESULT_GUARD_POSIX(s2n_stuffer_write_bytes(out, ticket_fields->session_secret.data, ticket_fields->session_secret.size));
    RESULT_GUARD(s2n_tls13_serialize_keying_material_expiration(conn, current_time, out));

    uint32_t server_max_early_data = 0;
    RESULT_GUARD(s2n_early_data_get_server_max_size(conn, &server_max_early_data));
    RESULT_GUARD_POSIX(s2n_stuffer_write_uint32(out, server_max_early_data));
    if (server_max_early_data > 0) {
        uint8_t application_protocol_len = strlen(conn->application_protocol);
        RESULT_GUARD_POSIX(s2n_stuffer_write_uint8(out, application_protocol_len));
        RESULT_GUARD_POSIX(s2n_stuffer_write_bytes(out, (uint8_t *) conn->application_protocol, application_protocol_len));
        RESULT_GUARD_POSIX(s2n_stuffer_write_uint16(out, conn->server_early_data_context.size));
        RESULT_GUARD_POSIX(s2n_stuffer_write(out, &conn->server_early_data_context));
    }

    return S2N_RESULT_OK;
}

static S2N_RESULT s2n_serialize_resumption_state(struct s2n_connection *conn, struct s2n_stuffer *out)
{
    if(conn->actual_protocol_version < S2N_TLS13) {
        RESULT_GUARD_POSIX(s2n_tls12_serialize_resumption_state(conn, out));
    } else {
        RESULT_GUARD(s2n_tls13_serialize_resumption_state(conn, out));
    }
    return S2N_RESULT_OK;
}

static int s2n_tls12_deserialize_resumption_state(struct s2n_connection *conn, struct s2n_stuffer *from)
{
    uint8_t protocol_version = 0;
    uint8_t cipher_suite[S2N_TLS_CIPHER_SUITE_LEN] = { 0 };

    S2N_ERROR_IF(s2n_stuffer_data_available(from) < S2N_TLS12_STATE_SIZE_IN_BYTES - sizeof(uint8_t), S2N_ERR_STUFFER_OUT_OF_DATA);

    POSIX_GUARD(s2n_stuffer_read_uint8(from, &protocol_version));
    S2N_ERROR_IF(protocol_version != conn->actual_protocol_version, S2N_ERR_INVALID_SERIALIZED_SESSION_STATE);

    POSIX_GUARD(s2n_stuffer_read_bytes(from, cipher_suite, S2N_TLS_CIPHER_SUITE_LEN));
    S2N_ERROR_IF(memcmp(conn->secure.cipher_suite->iana_value, cipher_suite, S2N_TLS_CIPHER_SUITE_LEN), S2N_ERR_INVALID_SERIALIZED_SESSION_STATE);

    uint64_t now;
    POSIX_GUARD(conn->config->wall_clock(conn->config->sys_clock_ctx, &now));

    uint64_t then;
    POSIX_GUARD(s2n_stuffer_read_uint64(from, &then));
    S2N_ERROR_IF(then > now, S2N_ERR_INVALID_SERIALIZED_SESSION_STATE);
    S2N_ERROR_IF(now - then > conn->config->session_state_lifetime_in_nanos, S2N_ERR_INVALID_SERIALIZED_SESSION_STATE);

    POSIX_GUARD(s2n_stuffer_read_bytes(from, conn->secrets.master_secret, S2N_TLS_SECRET_LEN));

    /* TODO: https://github.com/aws/s2n-tls/issues/2990 */
    if (S2N_IN_TEST && s2n_stuffer_data_available(from)) {
        uint8_t ems_negotiated = 0;
        POSIX_GUARD(s2n_stuffer_read_uint8(from, &ems_negotiated));

        /**
         *= https://tools.ietf.org/rfc/rfc7627#section-5.3
         *# If the original session did not use the "extended_master_secret"
         *# extension but the new ClientHello contains the extension, then the
         *# server MUST NOT perform the abbreviated handshake.  Instead, it
         *# SHOULD continue with a full handshake (as described in
         *# Section 5.2) to negotiate a new session.
         *#
         *# If the original session used the "extended_master_secret"
         *# extension but the new ClientHello does not contain it, the server
         *# MUST abort the abbreviated handshake.
         **/
        if (conn->ems_negotiated != ems_negotiated) {
            /* The session ticket needs to have the same EMS state as the current session. If it doesn't
             * have the same state, the current session takes the state of the session ticket and errors.
             * If the deserialization process errors, we will use this state in a few extra checks
             * to determine if we can fallback to a full handshake.
             */
            conn->ems_negotiated = ems_negotiated;
            POSIX_BAIL(S2N_ERR_INVALID_SERIALIZED_SESSION_STATE);
        }
    }

    return S2N_SUCCESS;
}

static int s2n_client_serialize_resumption_state(struct s2n_connection *conn, struct s2n_stuffer *to)
{
    /* Serialize session ticket */
   if (conn->config->use_tickets && conn->client_ticket.size > 0) {
       POSIX_GUARD(s2n_stuffer_write_uint8(to, S2N_STATE_WITH_SESSION_TICKET));
       POSIX_GUARD(s2n_stuffer_write_uint16(to, conn->client_ticket.size));
       POSIX_GUARD(s2n_stuffer_write(to, &conn->client_ticket));
   } else {
       /* Serialize session id */
       POSIX_ENSURE_LT(conn->actual_protocol_version, S2N_TLS13);
       POSIX_GUARD(s2n_stuffer_write_uint8(to, S2N_STATE_WITH_SESSION_ID));
       POSIX_GUARD(s2n_stuffer_write_uint8(to, conn->session_id_len));
       POSIX_GUARD(s2n_stuffer_write_bytes(to, conn->session_id, conn->session_id_len));
   }

    /* Serialize session state */
    POSIX_GUARD_RESULT(s2n_serialize_resumption_state(conn, to));

    return 0;
}

static S2N_RESULT s2n_tls12_client_deserialize_session_state(struct s2n_connection *conn, struct s2n_stuffer *from)
{   
    RESULT_ENSURE_REF(conn);
    RESULT_ENSURE_REF(from);

    RESULT_GUARD_POSIX(s2n_stuffer_read_uint8(from, &conn->actual_protocol_version));

    uint8_t *cipher_suite_wire = s2n_stuffer_raw_read(from, S2N_TLS_CIPHER_SUITE_LEN);
    RESULT_ENSURE_REF(cipher_suite_wire);
    RESULT_GUARD_POSIX(s2n_set_cipher_as_client(conn, cipher_suite_wire));

    uint64_t then = 0;
    RESULT_GUARD_POSIX(s2n_stuffer_read_uint64(from, &then));

    RESULT_GUARD_POSIX(s2n_stuffer_read_bytes(from, conn->secrets.master_secret, S2N_TLS_SECRET_LEN));

    /* TODO: https://github.com/aws/s2n-tls/issues/2990 */
    if (S2N_IN_TEST && s2n_stuffer_data_available(from)) {
        uint8_t ems_negotiated = 0;
        RESULT_GUARD_POSIX(s2n_stuffer_read_uint8(from, &ems_negotiated));
        conn->ems_negotiated = ems_negotiated;
    }
    return S2N_RESULT_OK;
}

static S2N_RESULT s2n_validate_ticket_age(uint64_t current_time, uint64_t ticket_issue_time)
{
    RESULT_ENSURE(current_time >= ticket_issue_time, S2N_ERR_INVALID_SESSION_TICKET);
    uint64_t ticket_age_in_nanos = current_time - ticket_issue_time;
    uint64_t ticket_age_in_sec = ticket_age_in_nanos / ONE_SEC_IN_NANOS;
    RESULT_ENSURE(ticket_age_in_sec <= ONE_WEEK_IN_SEC, S2N_ERR_INVALID_SESSION_TICKET);
    return S2N_RESULT_OK;
}

static S2N_RESULT s2n_tls13_deserialize_session_state(struct s2n_connection *conn, struct s2n_blob *psk_identity, struct s2n_stuffer *from)
{
    RESULT_ENSURE_REF(conn);
    RESULT_ENSURE_REF(psk_identity);
    RESULT_ENSURE_REF(from);

    DEFER_CLEANUP(struct s2n_psk psk = { 0 }, s2n_psk_wipe);
    RESULT_GUARD(s2n_psk_init(&psk, S2N_PSK_TYPE_RESUMPTION));
    RESULT_GUARD_POSIX(s2n_psk_set_identity(&psk, psk_identity->data, psk_identity->size));

    uint8_t protocol_version = 0;
    RESULT_GUARD_POSIX(s2n_stuffer_read_uint8(from, &protocol_version));
    RESULT_ENSURE_GTE(protocol_version, S2N_TLS13);

    uint8_t iana_id[S2N_TLS_CIPHER_SUITE_LEN] = { 0 };
    RESULT_GUARD_POSIX(s2n_stuffer_read_bytes(from, iana_id, S2N_TLS_CIPHER_SUITE_LEN));
    struct s2n_cipher_suite *cipher_suite = NULL;
    RESULT_GUARD(s2n_cipher_suite_from_iana(iana_id, &cipher_suite));
    RESULT_ENSURE_REF(cipher_suite);
    psk.hmac_alg = cipher_suite->prf_alg;

    RESULT_GUARD_POSIX(s2n_stuffer_read_uint64(from, &psk.ticket_issue_time));

    /**
     *= https://tools.ietf.org/rfc/rfc8446#section-4.6.1
     *# Clients MUST NOT cache
     *# tickets for longer than 7 days, regardless of the ticket_lifetime,
     *# and MAY delete tickets earlier based on local policy.
     */
    uint64_t current_time = 0;
    RESULT_GUARD_POSIX(conn->config->wall_clock(conn->config->sys_clock_ctx, &current_time));
    RESULT_GUARD(s2n_validate_ticket_age(current_time, psk.ticket_issue_time));

    RESULT_GUARD_POSIX(s2n_stuffer_read_uint32(from, &psk.ticket_age_add));

    uint8_t secret_len = 0;
    RESULT_GUARD_POSIX(s2n_stuffer_read_uint8(from, &secret_len));
    RESULT_ENSURE_LTE(secret_len, S2N_TLS_SECRET_LEN);
    uint8_t *secret_data = s2n_stuffer_raw_read(from, secret_len);
    RESULT_ENSURE_REF(secret_data);
    RESULT_GUARD_POSIX(s2n_psk_set_secret(&psk, secret_data, secret_len));

    if (conn->mode == S2N_SERVER) {
        RESULT_GUARD_POSIX(s2n_stuffer_read_uint64(from, &psk.keying_material_expiration));
        RESULT_ENSURE(psk.keying_material_expiration > current_time, S2N_ERR_KEYING_MATERIAL_EXPIRED);
    }

    uint32_t max_early_data_size = 0;
    RESULT_GUARD_POSIX(s2n_stuffer_read_uint32(from, &max_early_data_size));
    if (max_early_data_size > 0) {
        RESULT_GUARD_POSIX(s2n_psk_configure_early_data(&psk, max_early_data_size,
                iana_id[0], iana_id[1]));

        uint8_t app_proto_size = 0;
        RESULT_GUARD_POSIX(s2n_stuffer_read_uint8(from, &app_proto_size));
        uint8_t *app_proto_data = s2n_stuffer_raw_read(from, app_proto_size);
        RESULT_ENSURE_REF(app_proto_data);
        RESULT_GUARD_POSIX(s2n_psk_set_application_protocol(&psk, app_proto_data, app_proto_size));

        uint16_t early_data_context_size = 0;
        RESULT_GUARD_POSIX(s2n_stuffer_read_uint16(from, &early_data_context_size));
        uint8_t *early_data_context_data = s2n_stuffer_raw_read(from, early_data_context_size);
        RESULT_ENSURE_REF(early_data_context_data);
        RESULT_GUARD_POSIX(s2n_psk_set_early_data_context(&psk, early_data_context_data, early_data_context_size));
    }

    /* Make sure that this connection is configured for resumption PSKs, not external PSKs */
    RESULT_GUARD(s2n_connection_set_psk_type(conn, S2N_PSK_TYPE_RESUMPTION));
    /* Remove all previously-set PSKs. To keep the session ticket API behavior consistent
     * across protocol versions, we currently only support setting a single resumption PSK. */
    RESULT_GUARD(s2n_psk_parameters_wipe(&conn->psk_params));
    RESULT_GUARD_POSIX(s2n_connection_append_psk(conn, &psk));

    return S2N_RESULT_OK;
}

static S2N_RESULT s2n_deserialize_resumption_state(struct s2n_connection *conn, struct s2n_blob *psk_identity, struct s2n_stuffer *from)
{
    RESULT_ENSURE_REF(conn);
    RESULT_ENSURE_REF(from);

    uint8_t format = 0;
    RESULT_GUARD_POSIX(s2n_stuffer_read_uint8(from, &format));

    if (format == S2N_TLS12_SERIALIZED_FORMAT_VERSION) {
        if (conn->mode == S2N_SERVER) {
            RESULT_GUARD_POSIX(s2n_tls12_deserialize_resumption_state(conn, from));
        } else {
            RESULT_GUARD(s2n_tls12_client_deserialize_session_state(conn, from));
        }
    } else if (format == S2N_TLS13_SERIALIZED_FORMAT_VERSION) {
        RESULT_GUARD(s2n_tls13_deserialize_session_state(conn, psk_identity, from));
        if (conn->mode == S2N_CLIENT) {
            /* Free the client_ticket after setting a psk on the connection.
             * This prevents s2n_connection_get_session from returning a TLS1.3
             * ticket before a ticket has been received from the server. */
            RESULT_GUARD_POSIX(s2n_free(&conn->client_ticket));
        }
    } else {
        RESULT_BAIL(S2N_ERR_INVALID_SERIALIZED_SESSION_STATE);
    }
    return S2N_RESULT_OK;
}

static int s2n_client_deserialize_with_session_id(struct s2n_connection *conn, struct s2n_stuffer *from)
{
    uint8_t session_id_len;
    POSIX_GUARD(s2n_stuffer_read_uint8(from, &session_id_len));

    if (session_id_len == 0 || session_id_len > S2N_TLS_SESSION_ID_MAX_LEN
        || session_id_len > s2n_stuffer_data_available(from)) {
        POSIX_BAIL(S2N_ERR_INVALID_SERIALIZED_SESSION_STATE);
    }

    conn->session_id_len = session_id_len;
    POSIX_GUARD(s2n_stuffer_read_bytes(from, conn->session_id, session_id_len));

    POSIX_GUARD_RESULT(s2n_deserialize_resumption_state(conn, NULL, from));

    return 0;
}

static int s2n_client_deserialize_with_session_ticket(struct s2n_connection *conn, struct s2n_stuffer *from)
{
    uint16_t session_ticket_len;
    POSIX_GUARD(s2n_stuffer_read_uint16(from, &session_ticket_len));

    if (session_ticket_len == 0 || session_ticket_len > s2n_stuffer_data_available(from)) {
        POSIX_BAIL(S2N_ERR_INVALID_SERIALIZED_SESSION_STATE);
    }

    POSIX_GUARD(s2n_realloc(&conn->client_ticket, session_ticket_len));
    POSIX_GUARD(s2n_stuffer_read(from, &conn->client_ticket));

    POSIX_GUARD_RESULT(s2n_deserialize_resumption_state(conn, &conn->client_ticket, from));

    return 0;
}

static int s2n_client_deserialize_resumption_state(struct s2n_connection *conn, struct s2n_stuffer *from)
{
    uint8_t format;
    POSIX_GUARD(s2n_stuffer_read_uint8(from, &format));

    switch (format) {
    case S2N_STATE_WITH_SESSION_ID:
        POSIX_GUARD(s2n_client_deserialize_with_session_id(conn, from));
        break;
    case S2N_STATE_WITH_SESSION_TICKET:
        POSIX_GUARD(s2n_client_deserialize_with_session_ticket(conn, from));
        break;
    default:
        POSIX_BAIL(S2N_ERR_INVALID_SERIALIZED_SESSION_STATE);
    }

    return 0;
}

int s2n_resume_from_cache(struct s2n_connection *conn)
{
    S2N_ERROR_IF(conn->session_id_len == 0, S2N_ERR_SESSION_ID_TOO_SHORT);
    S2N_ERROR_IF(conn->session_id_len > S2N_TLS_SESSION_ID_MAX_LEN, S2N_ERR_SESSION_ID_TOO_LONG);

    uint8_t data[S2N_TLS12_TICKET_SIZE_IN_BYTES] = { 0 };
    struct s2n_blob entry = {0};
    POSIX_GUARD(s2n_blob_init(&entry, data, S2N_TLS12_TICKET_SIZE_IN_BYTES));
    uint64_t size = entry.size;
    int result = conn->config->cache_retrieve(conn, conn->config->cache_retrieve_data, conn->session_id, conn->session_id_len, entry.data, &size);
    if (result == S2N_CALLBACK_BLOCKED) {
        POSIX_BAIL(S2N_ERR_ASYNC_BLOCKED);
    }
    POSIX_GUARD(result);

    S2N_ERROR_IF(size != entry.size, S2N_ERR_SIZE_MISMATCH);

    struct s2n_stuffer from = {0};
    POSIX_GUARD(s2n_stuffer_init(&from, &entry));
    POSIX_GUARD(s2n_stuffer_write(&from, &entry));
    POSIX_GUARD(s2n_decrypt_session_cache(conn, &from));

    return 0;
}

int s2n_store_to_cache(struct s2n_connection *conn)
{
    uint8_t data[S2N_TLS12_TICKET_SIZE_IN_BYTES] = { 0 };
    struct s2n_blob entry = {0};
    POSIX_GUARD(s2n_blob_init(&entry, data, S2N_TLS12_TICKET_SIZE_IN_BYTES));
    struct s2n_stuffer to = {0};

    /* session_id_len should always be >0 since either the Client provided a SessionId or the Server generated a new
     * one for the Client */
    S2N_ERROR_IF(conn->session_id_len == 0, S2N_ERR_SESSION_ID_TOO_SHORT);
    S2N_ERROR_IF(conn->session_id_len > S2N_TLS_SESSION_ID_MAX_LEN, S2N_ERR_SESSION_ID_TOO_LONG);

    POSIX_GUARD(s2n_stuffer_init(&to, &entry));
    POSIX_GUARD(s2n_encrypt_session_cache(conn, &to));

    /* Store to the cache */
    conn->config->cache_store(conn, conn->config->cache_store_data, S2N_TLS_SESSION_CACHE_TTL, conn->session_id, conn->session_id_len, entry.data, entry.size);

    return 0;
}

int s2n_connection_set_session(struct s2n_connection *conn, const uint8_t *session, size_t length)
{
    POSIX_ENSURE_REF(conn);
    POSIX_ENSURE_REF(session);

    DEFER_CLEANUP(struct s2n_blob session_data = {0}, s2n_free);
    POSIX_GUARD(s2n_alloc(&session_data, length));
    memcpy(session_data.data, session, length);

    struct s2n_stuffer from = {0};
    POSIX_GUARD(s2n_stuffer_init(&from, &session_data));
    POSIX_GUARD(s2n_stuffer_write(&from, &session_data));
    POSIX_GUARD(s2n_client_deserialize_resumption_state(conn, &from));
    return 0;
}

int s2n_connection_get_session(struct s2n_connection *conn, uint8_t *session, size_t max_length)
{
    POSIX_ENSURE_REF(conn);
    POSIX_ENSURE_REF(session);

    int len = s2n_connection_get_session_length(conn);

    if (len == 0) {
        return 0;
    }

    S2N_ERROR_IF(len > max_length, S2N_ERR_SERIALIZED_SESSION_STATE_TOO_LONG);

    struct s2n_blob serialized_data = {0};
    POSIX_GUARD(s2n_blob_init(&serialized_data, session, len));
    POSIX_GUARD(s2n_blob_zero(&serialized_data));

    struct s2n_stuffer to = {0};
    POSIX_GUARD(s2n_stuffer_init(&to, &serialized_data));
    POSIX_GUARD(s2n_client_serialize_resumption_state(conn, &to));

    return len;
}

int s2n_connection_get_session_ticket_lifetime_hint(struct s2n_connection *conn)
{
    POSIX_ENSURE_REF(conn);
    S2N_ERROR_IF(!(conn->config->use_tickets && conn->client_ticket.size > 0), S2N_ERR_SESSION_TICKET_NOT_SUPPORTED);

    /* Session resumption using session ticket */
    return conn->ticket_lifetime_hint;
}

S2N_RESULT s2n_connection_get_session_state_size(struct s2n_connection *conn, size_t *state_size)
{
    RESULT_ENSURE_REF(conn);
    RESULT_ENSURE_REF(state_size);

    if (conn->actual_protocol_version < S2N_TLS13) {
        *state_size = S2N_TLS12_STATE_SIZE_IN_BYTES;
        return S2N_RESULT_OK;
    }

    *state_size = S2N_TLS13_FIXED_STATE_SIZE;

    uint8_t secret_size = 0;
    RESULT_ENSURE_REF(conn->secure.cipher_suite);
    RESULT_GUARD_POSIX(s2n_hmac_digest_size(conn->secure.cipher_suite->prf_alg, &secret_size));
    *state_size += secret_size;

    uint32_t server_max_early_data = 0;
    RESULT_GUARD(s2n_early_data_get_server_max_size(conn, &server_max_early_data));
    if (server_max_early_data > 0) {
        *state_size += S2N_TLS13_FIXED_EARLY_DATA_STATE_SIZE
                + strlen(conn->application_protocol)
                + conn->server_early_data_context.size;
    }

    return S2N_RESULT_OK;
}

static S2N_RESULT s2n_connection_get_session_length_impl(struct s2n_connection *conn, size_t *length)
{
    RESULT_ENSURE_REF(conn);
    RESULT_ENSURE_REF(conn->config);
    RESULT_ENSURE_REF(length);
    *length = 0;

    if (conn->config->use_tickets && conn->client_ticket.size > 0) {
        size_t session_state_size = 0;
        RESULT_GUARD(s2n_connection_get_session_state_size(conn, &session_state_size));
        *length = S2N_STATE_FORMAT_LEN + S2N_SESSION_TICKET_SIZE_LEN + conn->client_ticket.size + session_state_size;
    } else if (conn->session_id_len > 0 && conn->actual_protocol_version < S2N_TLS13) {
        *length = S2N_STATE_FORMAT_LEN + sizeof(conn->session_id_len) + conn->session_id_len + S2N_TLS12_STATE_SIZE_IN_BYTES;
    }
    return S2N_RESULT_OK;
}

int s2n_connection_get_session_length(struct s2n_connection *conn)
{
    size_t length = 0;
    if (s2n_result_is_ok(s2n_connection_get_session_length_impl(conn, &length))) {
        return length;
    }
    return 0;
}

int s2n_connection_is_session_resumed(struct s2n_connection *conn)
{
    return conn && IS_RESUMPTION_HANDSHAKE(conn)
        && (conn->actual_protocol_version < S2N_TLS13 || conn->psk_params.type == S2N_PSK_TYPE_RESUMPTION);
}

int s2n_connection_is_ocsp_stapled(struct s2n_connection *conn)
{
    POSIX_ENSURE_REF(conn);

    if (conn->actual_protocol_version >= S2N_TLS13) {
        return (s2n_server_can_send_ocsp(conn) || s2n_server_sent_ocsp(conn));
    } else {
        return IS_OCSP_STAPLED(conn);
    }
}

int s2n_config_is_encrypt_decrypt_key_available(struct s2n_config *config)
{
    uint64_t now;
    struct s2n_ticket_key *ticket_key = NULL;
    POSIX_GUARD(config->wall_clock(config->sys_clock_ctx, &now));
    POSIX_ENSURE_REF(config->ticket_keys);

    uint32_t ticket_keys_len = 0;
    POSIX_GUARD_RESULT(s2n_set_len(config->ticket_keys, &ticket_keys_len));

    for (uint32_t i = ticket_keys_len; i > 0; i--) {
        uint32_t idx = i - 1;
        POSIX_GUARD_RESULT(s2n_set_get(config->ticket_keys, idx, (void **)&ticket_key));
        uint64_t key_intro_time = ticket_key->intro_timestamp;

        if (key_intro_time < now
                && now < key_intro_time + config->encrypt_decrypt_key_lifetime_in_nanos) {
            return 1;
        }
    }

    return 0;
}

/* This function is used in s2n_get_ticket_encrypt_decrypt_key to compute the weight
 * of the keys and to choose a single key from all of the encrypt-decrypt keys.
 * Higher the weight of the key, higher the probability of being picked.
 */
int s2n_compute_weight_of_encrypt_decrypt_keys(struct s2n_config *config,
                                               uint8_t *encrypt_decrypt_keys_index,
                                               uint8_t num_encrypt_decrypt_keys,
                                               uint64_t now)
{
    double total_weight = 0;
    struct s2n_ticket_key_weight ticket_keys_weight[S2N_MAX_TICKET_KEYS];
    struct s2n_ticket_key *ticket_key = NULL;

    /* Compute weight of encrypt-decrypt keys */
    for (int i = 0; i < num_encrypt_decrypt_keys; i++) {
        POSIX_GUARD_RESULT(s2n_set_get(config->ticket_keys, encrypt_decrypt_keys_index[i], (void **)&ticket_key));

        uint64_t key_intro_time = ticket_key->intro_timestamp;
        uint64_t key_encryption_peak_time = key_intro_time + (config->encrypt_decrypt_key_lifetime_in_nanos / 2);

        /* The % of encryption using this key is linearly increasing */
        if (now < key_encryption_peak_time) {
            ticket_keys_weight[i].key_weight = now - key_intro_time;
        } else {
            /* The % of encryption using this key is linearly decreasing */
            ticket_keys_weight[i].key_weight = (config->encrypt_decrypt_key_lifetime_in_nanos / 2) - (now - key_encryption_peak_time);
        }

        ticket_keys_weight[i].key_index = encrypt_decrypt_keys_index[i];
        total_weight += ticket_keys_weight[i].key_weight;
    }

    /* Pick a random number in [0, 1). Using 53 bits (IEEE 754 double-precision floats). */
    uint64_t random_int = 0;
    POSIX_GUARD_RESULT(s2n_public_random(pow(2, 53), &random_int));
    double random = (double)random_int / (double)pow(2, 53);

    /* Compute cumulative weight of encrypt-decrypt keys */
    for (int i = 0; i < num_encrypt_decrypt_keys; i++) {
        ticket_keys_weight[i].key_weight = ticket_keys_weight[i].key_weight / total_weight;

        if (i > 0) {
            ticket_keys_weight[i].key_weight += ticket_keys_weight[i - 1].key_weight;
        }

        if (ticket_keys_weight[i].key_weight > random) {
            return ticket_keys_weight[i].key_index;
        }
    }

    POSIX_BAIL(S2N_ERR_ENCRYPT_DECRYPT_KEY_SELECTION_FAILED);
}

/* This function is used in s2n_encrypt_session_ticket in order for s2n to
 * choose a key in encrypt-decrypt state from all of the keys added to config
 */
struct s2n_ticket_key *s2n_get_ticket_encrypt_decrypt_key(struct s2n_config *config)
{
    uint8_t num_encrypt_decrypt_keys = 0;
    uint8_t encrypt_decrypt_keys_index[S2N_MAX_TICKET_KEYS] = { 0 };
    struct s2n_ticket_key *ticket_key = NULL;

    uint64_t now;
    PTR_GUARD_POSIX(config->wall_clock(config->sys_clock_ctx, &now));
    PTR_ENSURE_REF(config->ticket_keys);

    uint32_t ticket_keys_len = 0;
    PTR_GUARD_RESULT(s2n_set_len(config->ticket_keys, &ticket_keys_len));

    for (uint32_t i = ticket_keys_len; i > 0; i--) {
        uint32_t idx = i - 1;
        PTR_GUARD_RESULT(s2n_set_get(config->ticket_keys, idx, (void **)&ticket_key));
        uint64_t key_intro_time = ticket_key->intro_timestamp;

        if (key_intro_time < now
                && now < key_intro_time + config->encrypt_decrypt_key_lifetime_in_nanos) {
            encrypt_decrypt_keys_index[num_encrypt_decrypt_keys] = idx;
            num_encrypt_decrypt_keys++;
        }
    }

    if (num_encrypt_decrypt_keys == 0) {
        PTR_BAIL(S2N_ERR_NO_TICKET_ENCRYPT_DECRYPT_KEY);
    }

    if (num_encrypt_decrypt_keys == 1) {
        PTR_GUARD_RESULT(s2n_set_get(config->ticket_keys, encrypt_decrypt_keys_index[0], (void **)&ticket_key));
        return ticket_key;
    }

    int8_t idx;
    PTR_GUARD_POSIX(idx = s2n_compute_weight_of_encrypt_decrypt_keys(config, encrypt_decrypt_keys_index, num_encrypt_decrypt_keys, now));

    PTR_GUARD_RESULT(s2n_set_get(config->ticket_keys, idx, (void **)&ticket_key));
    return ticket_key;
}

/* This function is used in s2n_decrypt_session_ticket in order for s2n to
 * find the matching key that was used for encryption.
 */
struct s2n_ticket_key *s2n_find_ticket_key(struct s2n_config *config, const uint8_t *name)
{
    uint64_t now;
    struct s2n_ticket_key *ticket_key = NULL;
    PTR_GUARD_POSIX(config->wall_clock(config->sys_clock_ctx, &now));
    PTR_ENSURE_REF(config->ticket_keys);

    uint32_t ticket_keys_len = 0;
    PTR_GUARD_RESULT(s2n_set_len(config->ticket_keys, &ticket_keys_len));

    for (uint32_t i = 0; i < ticket_keys_len; i++) {
        PTR_GUARD_RESULT(s2n_set_get(config->ticket_keys, i, (void **)&ticket_key));

        if (memcmp(ticket_key->key_name, name, S2N_TICKET_KEY_NAME_LEN) == 0) {

            /* Check to see if the key has expired */
            if (now >= ticket_key->intro_timestamp +
                                config->encrypt_decrypt_key_lifetime_in_nanos + config->decrypt_key_lifetime_in_nanos) {
                s2n_config_wipe_expired_ticket_crypto_keys(config, i);

                return NULL;
            }

            return ticket_key;
        }
    }

    return NULL;
}

int s2n_encrypt_session_ticket(struct s2n_connection *conn, struct s2n_stuffer *to)
{
    struct s2n_ticket_key *key;
    struct s2n_session_key aes_ticket_key = {0};
    struct s2n_blob aes_key_blob = {0};

    uint8_t iv_data[S2N_TLS_GCM_IV_LEN] = { 0 };
    struct s2n_blob iv = {0};
    POSIX_GUARD(s2n_blob_init(&iv, iv_data, sizeof(iv_data)));

    uint8_t aad_data[S2N_TICKET_AAD_LEN] = { 0 };
    struct s2n_blob aad_blob = {0};
    POSIX_GUARD(s2n_blob_init(&aad_blob, aad_data, sizeof(aad_data)));
    struct s2n_stuffer aad = {0};

    key = s2n_get_ticket_encrypt_decrypt_key(conn->config);

    /* No keys loaded by the user or the keys are either in decrypt-only or expired state */
    S2N_ERROR_IF(!key, S2N_ERR_NO_TICKET_ENCRYPT_DECRYPT_KEY);

    POSIX_GUARD(s2n_stuffer_write_bytes(to, key->key_name, S2N_TICKET_KEY_NAME_LEN));

    POSIX_GUARD_RESULT(s2n_get_public_random_data(&iv));
    POSIX_GUARD(s2n_stuffer_write(to, &iv));

    POSIX_GUARD(s2n_blob_init(&aes_key_blob, key->aes_key, S2N_AES256_KEY_LEN));
    POSIX_GUARD(s2n_session_key_alloc(&aes_ticket_key));
    POSIX_GUARD(s2n_aes256_gcm.init(&aes_ticket_key));
    POSIX_GUARD(s2n_aes256_gcm.set_encryption_key(&aes_ticket_key, &aes_key_blob));

    POSIX_GUARD(s2n_stuffer_init(&aad, &aad_blob));
    POSIX_GUARD(s2n_stuffer_write_bytes(&aad, key->implicit_aad, S2N_TICKET_AAD_IMPLICIT_LEN));
    POSIX_GUARD(s2n_stuffer_write_bytes(&aad, key->key_name, S2N_TICKET_KEY_NAME_LEN));

    uint32_t plaintext_header_size = s2n_stuffer_data_available(to);
    POSIX_GUARD_RESULT(s2n_serialize_resumption_state(conn, to));
    POSIX_GUARD(s2n_stuffer_skip_write(to, S2N_TLS_GCM_TAG_LEN));

    struct s2n_blob state_blob = { 0 };
    struct s2n_stuffer copy_for_encryption = *to;
    POSIX_GUARD(s2n_stuffer_skip_read(&copy_for_encryption, plaintext_header_size));
    uint32_t state_blob_size = s2n_stuffer_data_available(&copy_for_encryption);
    uint8_t *state_blob_data = s2n_stuffer_raw_read(&copy_for_encryption, state_blob_size);
    POSIX_ENSURE_REF(state_blob_data);
    POSIX_GUARD(s2n_blob_init(&state_blob, state_blob_data, state_blob_size));

    POSIX_GUARD(s2n_aes256_gcm.io.aead.encrypt(&aes_ticket_key, &iv, &aad_blob, &state_blob, &state_blob));

    POSIX_GUARD(s2n_aes256_gcm.destroy_key(&aes_ticket_key));
    POSIX_GUARD(s2n_session_key_free(&aes_ticket_key));

    return 0;
}

int s2n_decrypt_session_ticket(struct s2n_connection *conn, struct s2n_stuffer *from)
{
    struct s2n_ticket_key *key;
    DEFER_CLEANUP(struct s2n_session_key aes_ticket_key = {0}, s2n_session_key_free);
    struct s2n_blob aes_key_blob = {0};

    uint8_t key_name[S2N_TICKET_KEY_NAME_LEN];

    uint8_t iv_data[S2N_TLS_GCM_IV_LEN] = { 0 };
    struct s2n_blob iv = { 0 };
    POSIX_GUARD(s2n_blob_init(&iv, iv_data, sizeof(iv_data)));

    uint8_t aad_data[S2N_TICKET_AAD_LEN] = { 0 };
    struct s2n_blob aad_blob = {0};
    POSIX_GUARD(s2n_blob_init(&aad_blob, aad_data, sizeof(aad_data)));
    struct s2n_stuffer aad = {0};

    POSIX_GUARD(s2n_stuffer_read_bytes(from, key_name, S2N_TICKET_KEY_NAME_LEN));

    key = s2n_find_ticket_key(conn->config, key_name);

    /* Key has expired; do full handshake with New Session Ticket (NST) */
    S2N_ERROR_IF(!key, S2N_ERR_KEY_USED_IN_SESSION_TICKET_NOT_FOUND);

    POSIX_GUARD(s2n_stuffer_read(from, &iv));

    s2n_blob_init(&aes_key_blob, key->aes_key, S2N_AES256_KEY_LEN);
    POSIX_GUARD(s2n_session_key_alloc(&aes_ticket_key));
    POSIX_GUARD(s2n_aes256_gcm.init(&aes_ticket_key));
    POSIX_GUARD(s2n_aes256_gcm.set_decryption_key(&aes_ticket_key, &aes_key_blob));

    POSIX_GUARD(s2n_stuffer_init(&aad, &aad_blob));
    POSIX_GUARD(s2n_stuffer_write_bytes(&aad, key->implicit_aad, S2N_TICKET_AAD_IMPLICIT_LEN));
    POSIX_GUARD(s2n_stuffer_write_bytes(&aad, key->key_name, S2N_TICKET_KEY_NAME_LEN));

    struct s2n_blob en_blob = { 0 };
    uint32_t en_blob_size = s2n_stuffer_data_available(from);
    uint8_t *en_blob_data = s2n_stuffer_raw_read(from, en_blob_size);
    POSIX_ENSURE_REF(en_blob_data);
    POSIX_GUARD(s2n_blob_init(&en_blob, en_blob_data, en_blob_size));
    POSIX_GUARD(s2n_aes256_gcm.io.aead.decrypt(&aes_ticket_key, &iv, &aad_blob, &en_blob, &en_blob));    

    struct s2n_blob state_blob = { 0 };
    uint32_t state_blob_size = en_blob_size - S2N_TLS_GCM_TAG_LEN;
    POSIX_GUARD(s2n_blob_init(&state_blob, en_blob.data, state_blob_size));
    struct s2n_stuffer state_stuffer = { 0 };
    POSIX_GUARD(s2n_stuffer_init(&state_stuffer, &state_blob));
    POSIX_GUARD(s2n_stuffer_skip_write(&state_stuffer, state_blob_size));
    POSIX_GUARD_RESULT(s2n_deserialize_resumption_state(conn, &from->blob, &state_stuffer));

    uint64_t now;
    POSIX_GUARD(conn->config->wall_clock(conn->config->sys_clock_ctx, &now));

    /* If the key is in decrypt-only state, then a new key is assigned
     * for the ticket.
     */
    if (now >= key->intro_timestamp + conn->config->encrypt_decrypt_key_lifetime_in_nanos) {
        /* Check if a key in encrypt-decrypt state is available */
        if (s2n_config_is_encrypt_decrypt_key_available(conn->config) == 1) {
            conn->session_ticket_status = S2N_NEW_TICKET;
            POSIX_GUARD_RESULT(s2n_handshake_type_set_tls12_flag(conn, WITH_SESSION_TICKET));
            return S2N_SUCCESS;
        }
    }
    return S2N_SUCCESS;
}

int s2n_encrypt_session_cache(struct s2n_connection *conn, struct s2n_stuffer *to)
{
    return s2n_encrypt_session_ticket(conn, to);
}

int s2n_decrypt_session_cache(struct s2n_connection *conn, struct s2n_stuffer *from)
{
    struct s2n_ticket_key *key;
    struct s2n_session_key aes_ticket_key = {0};
    struct s2n_blob aes_key_blob = {0};

    uint8_t key_name[S2N_TICKET_KEY_NAME_LEN] = {0};

    uint8_t iv_data[S2N_TLS_GCM_IV_LEN] = { 0 };
    struct s2n_blob iv = {0};
    POSIX_GUARD(s2n_blob_init(&iv, iv_data, sizeof(iv_data)));

    uint8_t aad_data[S2N_TICKET_AAD_LEN] = { 0 };
    struct s2n_blob aad_blob = {0};
    POSIX_GUARD(s2n_blob_init(&aad_blob, aad_data, sizeof(aad_data)));
    struct s2n_stuffer aad = {0};

    uint8_t s_data[S2N_TLS12_STATE_SIZE_IN_BYTES] = { 0 };
    struct s2n_blob state_blob = {0};
    POSIX_GUARD(s2n_blob_init(&state_blob, s_data, sizeof(s_data)));
    struct s2n_stuffer state = {0};

    uint8_t en_data[S2N_TLS12_STATE_SIZE_IN_BYTES + S2N_TLS_GCM_TAG_LEN] = {0};
    struct s2n_blob en_blob = {0};
    POSIX_GUARD(s2n_blob_init(&en_blob, en_data, sizeof(en_data)));

    POSIX_GUARD(s2n_stuffer_read_bytes(from, key_name, S2N_TICKET_KEY_NAME_LEN));

    key = s2n_find_ticket_key(conn->config, key_name);

    /* Key has expired; do full handshake with New Session Ticket (NST) */
    S2N_ERROR_IF(!key, S2N_ERR_KEY_USED_IN_SESSION_TICKET_NOT_FOUND);

    POSIX_GUARD(s2n_stuffer_read(from, &iv));

    s2n_blob_init(&aes_key_blob, key->aes_key, S2N_AES256_KEY_LEN);
    POSIX_GUARD(s2n_session_key_alloc(&aes_ticket_key));
    POSIX_GUARD(s2n_aes256_gcm.init(&aes_ticket_key));
    POSIX_GUARD(s2n_aes256_gcm.set_decryption_key(&aes_ticket_key, &aes_key_blob));

    POSIX_GUARD(s2n_stuffer_init(&aad, &aad_blob));
    POSIX_GUARD(s2n_stuffer_write_bytes(&aad, key->implicit_aad, S2N_TICKET_AAD_IMPLICIT_LEN));
    POSIX_GUARD(s2n_stuffer_write_bytes(&aad, key->key_name, S2N_TICKET_KEY_NAME_LEN));

    POSIX_GUARD(s2n_stuffer_read(from, &en_blob));

    POSIX_GUARD(s2n_aes256_gcm.io.aead.decrypt(&aes_ticket_key, &iv, &aad_blob, &en_blob, &en_blob));

    POSIX_GUARD(s2n_stuffer_init(&state, &state_blob));
    POSIX_GUARD(s2n_stuffer_write_bytes(&state, en_data, S2N_TLS12_STATE_SIZE_IN_BYTES));

    POSIX_GUARD_RESULT(s2n_deserialize_resumption_state(conn, NULL, &state));

    POSIX_GUARD(s2n_aes256_gcm.destroy_key(&aes_ticket_key));
    POSIX_GUARD(s2n_session_key_free(&aes_ticket_key));

    return 0;
}

/* This function is used to remove all or just one expired key from server config */
int s2n_config_wipe_expired_ticket_crypto_keys(struct s2n_config *config, int8_t expired_key_index)
{
    int num_of_expired_keys = 0;
    int expired_keys_index[S2N_MAX_TICKET_KEYS];
    struct s2n_ticket_key *ticket_key = NULL;

    if (expired_key_index != -1) {
        expired_keys_index[num_of_expired_keys] = expired_key_index;
        num_of_expired_keys++;

        goto end;
    }

    uint64_t now;
    POSIX_GUARD(config->wall_clock(config->sys_clock_ctx, &now));
    POSIX_ENSURE_REF(config->ticket_keys);

    uint32_t ticket_keys_len = 0;
    POSIX_GUARD_RESULT(s2n_set_len(config->ticket_keys, &ticket_keys_len));

    for (uint32_t i = 0; i < ticket_keys_len; i++) {
        POSIX_GUARD_RESULT(s2n_set_get(config->ticket_keys, i, (void **)&ticket_key));
        if (now >= ticket_key->intro_timestamp +
                   config->encrypt_decrypt_key_lifetime_in_nanos + config->decrypt_key_lifetime_in_nanos) {
            expired_keys_index[num_of_expired_keys] = i;
            num_of_expired_keys++;
        }
    }

end:
    for (int j = 0; j < num_of_expired_keys; j++) {
        POSIX_GUARD_RESULT(s2n_set_remove(config->ticket_keys, expired_keys_index[j] - j));
    }

    return 0;
}


int s2n_config_store_ticket_key(struct s2n_config *config, struct s2n_ticket_key *key)
{
    /* Keys are stored from oldest to newest */
    POSIX_GUARD_RESULT(s2n_set_add(config->ticket_keys, key));
    return S2N_SUCCESS;
}

int s2n_config_set_initial_ticket_count(struct s2n_config *config, uint8_t num)
{
    POSIX_ENSURE_REF(config);

    config->initial_tickets_to_send = num;
    POSIX_GUARD(s2n_config_set_session_tickets_onoff(config, true));

    return S2N_SUCCESS;
}

int s2n_connection_add_new_tickets_to_send(struct s2n_connection *conn, uint8_t num) {
    POSIX_ENSURE_REF(conn);
    POSIX_GUARD_RESULT(s2n_psk_validate_keying_material(conn));

    uint32_t out = conn->tickets_to_send + num;
    POSIX_ENSURE(out <= UINT16_MAX, S2N_ERR_INTEGER_OVERFLOW);
    conn->tickets_to_send = out;

    return S2N_SUCCESS;
}

int s2n_connection_set_server_keying_material_lifetime(struct s2n_connection *conn, uint32_t lifetime_in_secs)
{
    POSIX_ENSURE_REF(conn);
    conn->server_keying_material_lifetime = lifetime_in_secs;
    return S2N_SUCCESS;
}

int s2n_config_set_session_ticket_cb(struct s2n_config *config, s2n_session_ticket_fn callback, void *ctx)
{
    POSIX_ENSURE_MUT(config);

    config->session_ticket_cb = callback;
    config->session_ticket_ctx = ctx;
    return S2N_SUCCESS;
}

int s2n_session_ticket_get_data_len(struct s2n_session_ticket *ticket, size_t *data_len)
{
    POSIX_ENSURE_REF(ticket);
    POSIX_ENSURE_MUT(data_len);

    *data_len = ticket->ticket_data.size;
    return S2N_SUCCESS;
}

int s2n_session_ticket_get_data(struct s2n_session_ticket *ticket, size_t max_data_len, uint8_t *data)
{
    POSIX_ENSURE_REF(ticket);
    POSIX_ENSURE_MUT(data);

    POSIX_ENSURE(ticket->ticket_data.size <= max_data_len, S2N_ERR_SERIALIZED_SESSION_STATE_TOO_LONG);
    POSIX_CHECKED_MEMCPY(data, ticket->ticket_data.data, ticket->ticket_data.size);

    return S2N_SUCCESS;
}

int s2n_session_ticket_get_lifetime(struct s2n_session_ticket *ticket, uint32_t *session_lifetime)
{
    POSIX_ENSURE_REF(ticket);
    POSIX_ENSURE_REF(session_lifetime);

    *session_lifetime = ticket->session_lifetime;

    return S2N_SUCCESS;
}
