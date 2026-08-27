// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file syscall_router_session.c
 * @brief Syscall router session domain (airy_sys_session_* implementation and routing).
 */

// @owner: team-B
#include "syscall_router.h"
#include "syscall_router_internal.h"

/**
  * @brief Route session-management syscalls
 */
char *route_session_methods(const char *method, cJSON *params, cJSON *request_id)
{
    cJSON *result = NULL;
    airy_err_t err = AIRY_SUCCESS;

    if (strcmp(method, "airy_sys_session_create") == 0) {
        cJSON *metadata = cJSON_GetObjectItem(params, "metadata");
        char *out_session_id = NULL;
        const char *meta_str = metadata ? cJSON_PrintUnformatted(metadata) : NULL;

        err = airy_sys_session_create(meta_str, &out_session_id);

        if (meta_str)
            AIRY_FREE((void *)meta_str);

        if (err == AIRY_SUCCESS && out_session_id) {
            result = cJSON_CreateObject();
            cJSON_AddStringToObject(result, "session_id", out_session_id);
            AIRY_FREE(out_session_id);
        }
    } else if (strcmp(method, "airy_sys_session_get") == 0) {
        cJSON *session_id = cJSON_GetObjectItem(params, "session_id");

        if (!session_id || !cJSON_IsString(session_id)) {
            return jsonrpc_create_error_response(request_id, -32602,
                                                 "Invalid params: session_id required", NULL);
        }

        char *out_info = NULL;
        err = airy_sys_session_get(session_id->valuestring, &out_info);

        if (err == AIRY_SUCCESS && out_info) {
            result = cJSON_Parse(out_info);
            AIRY_FREE(out_info);
        }
    } else if (strcmp(method, "airy_sys_session_close") == 0) {
        cJSON *session_id = cJSON_GetObjectItem(params, "session_id");

        if (!session_id || !cJSON_IsString(session_id)) {
            return jsonrpc_create_error_response(request_id, -32602,
                                                 "Invalid params: session_id required", NULL);
        }

        err = airy_sys_session_close(session_id->valuestring);
        if (err == AIRY_SUCCESS) {
            result = cJSON_CreateObject();
            cJSON_AddBoolToObject(result, "closed", true);
        }
    } else if (strcmp(method, "airy_sys_session_list") == 0) {
        char **sessions = NULL;
        size_t session_count = 0;
        err = airy_sys_session_list(&sessions, &session_count);

        if (err == AIRY_SUCCESS && sessions) {
            result = cJSON_CreateArray();
            for (size_t i = 0; i < session_count && sessions[i]; i++) {
                cJSON_AddItemToArray(result, cJSON_CreateString(sessions[i]));
                AIRY_FREE(sessions[i]);
            }
            AIRY_FREE(sessions);
        }
    }

    if (err != AIRY_SUCCESS) {
        cJSON_Delete(result);
        char err_msg[64];
        snprintf(err_msg, sizeof(err_msg), "System call failed: %d", err);
        return jsonrpc_create_error_response(request_id, -32000, err_msg, NULL);
    }

    return jsonrpc_create_success_response(request_id, result);
}

airy_err_t airy_sys_session_create(const char *metadata, char **out_session_id)
{
    if (!out_session_id)
        return AIRY_ERR_INVALID_PARAM;

    RUNTIME_LOCK();
    if (g_runtime.session_count >= g_max_sessions) {
        RUNTIME_UNLOCK();
        return AIRY_ERR_OUT_OF_MEMORY;
    }

    session_entry_t *sess = &g_runtime.sessions[g_runtime.session_count++];
    sess->session_id = AIRY_STRDUP(generate_uuid());
    sess->metadata = metadata ? AIRY_STRDUP(metadata) : NULL;
    sess->created_at = time(NULL);
    sess->last_accessed = sess->created_at;
    *out_session_id = AIRY_STRDUP(sess->session_id);
    ht_insert(&g_runtime.session_index, sess->session_id, g_runtime.session_count - 1);
    RUNTIME_UNLOCK();
    return AIRY_OK;
}

airy_err_t airy_sys_session_get(const char *session_id, char **out_info)
{
    if (!session_id || !out_info)
        return AIRY_ERR_INVALID_PARAM;

    RUNTIME_LOCK();
    ssize_t idx = ht_lookup(&g_runtime.session_index, session_id);
    if (idx >= 0 && (size_t)idx < g_runtime.session_count) {
        g_runtime.sessions[idx].last_accessed = time(NULL);
        cJSON *info = cJSON_CreateObject();
        cJSON_AddStringToObject(info, "session_id", session_id);
        cJSON_AddStringToObject(info, "metadata",
                                g_runtime.sessions[idx].metadata ?
                                    g_runtime.sessions[idx].metadata :
                                    "");
        cJSON_AddNumberToObject(info, "age_seconds",
                                (double)(time(NULL) - g_runtime.sessions[idx].created_at));
        *out_info = cJSON_PrintUnformatted(info);
        cJSON_Delete(info);
        RUNTIME_UNLOCK();
        return AIRY_OK;
    }
    RUNTIME_UNLOCK();
    *out_info = NULL;
    return AIRY_ERR_NOT_FOUND;
}

airy_err_t airy_sys_session_close(const char *session_id)
{
    if (!session_id)
        return AIRY_ERR_INVALID_PARAM;

    RUNTIME_LOCK();
    ssize_t idx = ht_lookup(&g_runtime.session_index, session_id);
    if (idx >= 0 && (size_t)idx < g_runtime.session_count) {
        AIRY_FREE(g_runtime.sessions[idx].session_id);
        AIRY_FREE(g_runtime.sessions[idx].metadata);
        __builtin_memmove(&g_runtime.sessions[idx], &g_runtime.sessions[idx + 1],
                          (g_runtime.session_count - idx - 1) * sizeof(session_entry_t));
        g_runtime.session_count--;

        /* P0: after the left-shift, indices after idx change for all sessions, but
          * session_index still holds the old indices, so later session_get/close
          * would hit the wrong session (OOB/UAF). Rebuild session_index after deletion. */
        ht_destroy(&g_runtime.session_index);
        if (ht_init(&g_runtime.session_index, g_max_sessions * 2) != 0) {
            airy_err_push_ex(AIRY_ERR_OUT_OF_MEMORY, __FILE__, __LINE__, __func__,
                             "session_index rebuild failed");
            RUNTIME_UNLOCK();
            return AIRY_ERR_OUT_OF_MEMORY;
        }
        for (size_t i = 0; i < g_runtime.session_count; i++) {
            ht_insert(&g_runtime.session_index, g_runtime.sessions[i].session_id, i);
        }
        RUNTIME_UNLOCK();
        return AIRY_OK;
    }
    RUNTIME_UNLOCK();
    return AIRY_ERR_NOT_FOUND;
}

airy_err_t airy_sys_session_list(char ***sessions, size_t *count)
{
    if (!sessions || !count)
        return AIRY_ERR_INVALID_PARAM;

    RUNTIME_LOCK();
    *sessions = (char **)AIRY_CALLOC(g_runtime.session_count, sizeof(char *));
    if (!*sessions && g_runtime.session_count > 0) {
        RUNTIME_UNLOCK();
        return AIRY_ERR_OUT_OF_MEMORY;
    }

    for (size_t i = 0; i < g_runtime.session_count; i++) {
        (*sessions)[i] = AIRY_STRDUP(g_runtime.sessions[i].session_id);
    }
    *count = g_runtime.session_count;
    RUNTIME_UNLOCK();
    return AIRY_OK;
}
