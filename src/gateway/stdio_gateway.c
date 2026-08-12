// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/*
 * @file stdio_gateway.c
 * @brief Stdio gateway implementation - local process communication protocol.
 *
 * Implements the stdio communication protocol and talks to the kernel
 * through the syscall interface. The gateway only translates protocols and
 * contains no business logic.
 */

// @owner: team-B
#include "stdio_gateway.h"

#include "../utils/gateway_rpc_handler.h"
#include "../utils/gateway_utils.h"
#include "../utils/jsonrpc.h"
#include "../utils/syscall_router.h"
#include "logging.h"
#ifdef AIRY_HAS_CJSON
#include <cjson/cJSON.h>

#include <cjson_helpers.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "atomic_compat.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#define STDIN_FILENO 0
#else
#include <sys/select.h>
#include <unistd.h>
#endif

#include "airy_memory.h"

/*
 * time_ns() migrated to gateway_utils.h (gateway_time_ns);
 * portable_sleep() migrated to gateway_utils.h (gateway_sleep).
 */

/**
  * @brief Stdio gateway internal structure
 */
typedef struct stdio_gateway {
    void *handler_adapter;
    gateway_internal_handler_t handler;
    void *handler_data;

    atomic_bool running;

    atomic_uint_fast64_t commands_total;
    atomic_uint_fast64_t commands_failed;
    atomic_uint_fast64_t bytes_received;
    atomic_uint_fast64_t bytes_sent;

    char *input_buffer;
    size_t input_buffer_size;
    size_t input_buffer_pos;
} stdio_gateway_t;

/**
  * @brief Display help information
  * @return Help string (caller frees)
 */
static char *show_help(void)
{
    return AIRY_STRDUP("AgentRT Stdio Gateway - Available Commands:\n"
                       "  help                     - Show this help\n"
                       "  rpc <json-rpc>           - Execute JSON-RPC call\n"
                       "  stats                    - Show gateway statistics\n"
                       "  exit                     - Exit gateway\n"
                       "\n"
                       "JSON-RPC Methods:\n"
                       "  airy_sys_task_submit    - Submit a task\n"
                       "  airy_sys_task_query     - Query task status\n"
                       "  airy_sys_memory_search  - Search memory\n"
                       "  airy_sys_session_create - Create session\n"
                       "  airy_sys_session_list   - List sessions\n"
                       "  airy_sys_telemetry_metrics - Get metrics\n");
}

/**
  * @brief Handle a JSON-RPC request (via the unified RPC handler)
 *
  * Unified request handling via gateway_rpc_handle_request(),
  * removing duplication with the HTTP/WS gateways.
 *
 * @param gateway Gateway instance
 * @param json_str JSON string
  * @return Response string
 */
static char *handle_jsonrpc(stdio_gateway_t *gateway, const char *json_str)
{

    CJSON_PARSE_GUARD(request, json_str,
                      { return jsonrpc_create_error_response(NULL, -32700, "Parse error", NULL); });

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-function-type"
    rpc_result_t result =
        gateway_rpc_handle_request(request,
                                   (int (*)(const char *, char **, void *))gateway->handler,
                                   gateway->handler_data);
#pragma GCC diagnostic pop

    if (result.error_code != 0 || !result.response_json) {
        char *error_resp = result.response_json ?
                               result.response_json :
                               jsonrpc_create_error_response(NULL, -32603, "Internal error", NULL);
        if (result.response_json)
            result.response_json = NULL;
        gateway_rpc_free(&result);

        return error_resp;
    }

    char *success_resp = result.response_json;
    result.response_json = NULL;
    gateway_rpc_free(&result);

    return success_resp;
}

/**
 * @brief Process a command
 * @param gateway Gateway instance
  * @param input Input string
  * @return Response string
 */
static char *process_command(stdio_gateway_t *gateway, const char *input)
{
    if (!input || strlen(input) == 0) {
        return AIRY_STRDUP("");
    }

    char *trimmed = AIRY_STRDUP(input);
    char *start = trimmed;
    while (*start == ' ' || *start == '\t')
        start++;
    char *end = start + strlen(start) - 1;
    while (end > start && (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r'))
        end--;
    *(end + 1) = '\0';

    if (strlen(start) == 0) {
        AIRY_FREE(trimmed);
        return AIRY_STRDUP("");
    }

    char *response = NULL;

    if (strcmp(start, "help") == 0 || strcmp(start, "?") == 0) {
        response = show_help();
    } else if (strcmp(start, "stats") == 0) {
        cJSON *stats = cJSON_CreateObject();
        cJSON_AddNumberToObject(stats, "commands_total",
                                (double)atomic_load(&gateway->commands_total));
        cJSON_AddNumberToObject(stats, "commands_failed",
                                (double)atomic_load(&gateway->commands_failed));
        cJSON_AddNumberToObject(stats, "bytes_received",
                                (double)atomic_load(&gateway->bytes_received));
        cJSON_AddNumberToObject(stats, "bytes_sent", (double)atomic_load(&gateway->bytes_sent));
        response = cJSON_Print(stats);
        cJSON_Delete(stats);
    } else if (strcmp(start, "exit") == 0 || strcmp(start, "quit") == 0) {
        atomic_store(&gateway->running, false);
        response = AIRY_STRDUP("Gateway shutting down...\n");
    } else if (strncmp(start, "rpc ", 4) == 0) {
        const char *json_str = start + 4;
        response = handle_jsonrpc(gateway, json_str);
    } else {
        response = AIRY_STRDUP("Unknown command. Type 'help' for available commands.\n");
    }

    AIRY_FREE(trimmed);
    return response;
}

static airy_err_t stdio_gateway_start(void *gateway_impl)
{
    stdio_gateway_t *gateway = (stdio_gateway_t *)gateway_impl;

    gateway->input_buffer_pos = 0;
    atomic_store(&gateway->running, true);

    AIRY_LOG_INFO("AgentRT Stdio Gateway started. Type 'help' for available commands.");
    AIRY_LOG_DEBUG("> ");

    while (atomic_load(&gateway->running)) {
#ifdef _WIN32
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(STDIN_FILENO, &read_fds);

        struct timeval timeout;
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;

        int ret = select(1, &read_fds, NULL, NULL, &timeout);
#else
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(STDIN_FILENO, &read_fds);

        struct timeval timeout;
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;

        int ret = select(STDIN_FILENO + 1, &read_fds, NULL, NULL, &timeout);
#endif

        if (ret > 0) {
            char buffer[1024];
            if (fgets(buffer, sizeof(buffer), stdin) != NULL) {
                size_t input_len = strlen(buffer);
                atomic_fetch_add(&gateway->bytes_received, input_len);

                if (gateway->input_buffer_pos + input_len < gateway->input_buffer_size) {
                    AIRY_MEMCPY(gateway->input_buffer + gateway->input_buffer_pos, buffer,
                                input_len);
                    gateway->input_buffer_pos += input_len;

                    char *newline = memchr(gateway->input_buffer, '\n', gateway->input_buffer_pos);
                    if (newline) {
                        *newline = '\0';
                        char *command_line = AIRY_STRDUP(gateway->input_buffer);
                        gateway->input_buffer_pos -= (newline + 1 - gateway->input_buffer);
                        __builtin_memmove(gateway->input_buffer, newline + 1,
                                          gateway->input_buffer_pos);

                        char *response = process_command(gateway, command_line);
                        AIRY_FREE(command_line);

                        if (response) {
                            AIRY_LOG_INFO("%s\n", response);
                            atomic_fetch_add(&gateway->bytes_sent, strlen(response));
                            atomic_fetch_add(&gateway->commands_total, 1);
                            AIRY_FREE(response);
                        }

                        if (atomic_load(&gateway->running)) {
                            AIRY_LOG_DEBUG("> ");
                        }
                    }
                }
            } else {
                /* stdin hit EOF or a read error (e.g. redirected to /dev/null or closed).
                 * select still returns immediately because the fd reads as always-ready,
                 * so continuing would busy-loop at 100% CPU. With no input source left,
                 * exit the loop to stop this transport. */
                if (feof(stdin) || ferror(stdin)) {
                    atomic_store(&gateway->running, false);
                }
            }
        }
    }

    AIRY_LOG_INFO("Gateway stopped.");
    return AIRY_SUCCESS;
}

static void stdio_gateway_stop(void *gateway_impl)
{
    stdio_gateway_t *gateway = (stdio_gateway_t *)gateway_impl;
    atomic_store(&gateway->running, false);
}

static void stdio_gateway_destroy(void *gateway_impl)
{
    stdio_gateway_t *gateway = (stdio_gateway_t *)gateway_impl;
    if (!gateway)
        return;

    stdio_gateway_stop(gateway);

    if (gateway->handler_adapter) {
        AIRY_FREE(gateway->handler_adapter);
        gateway->handler_adapter = NULL;
    }
    gateway->handler = NULL;
    gateway->handler_data = NULL;

    AIRY_FREE(gateway->input_buffer);
    gateway->input_buffer = NULL;
    gateway->input_buffer_size = 0;

    AIRY_FREE(gateway);
}

static const char *stdio_gateway_get_name(void *gateway_impl __attribute__((unused)))
{
    return "Stdio Gateway";
}

static bool stdio_gateway_is_running(void *gateway_impl)
{
    stdio_gateway_t *gateway = (stdio_gateway_t *)gateway_impl;
    if (!gateway)
        return false;
    return atomic_load(&gateway->running);
}

static airy_err_t stdio_gateway_get_stats(void *gateway_impl, char **out_json)
{
    stdio_gateway_t *gateway = (stdio_gateway_t *)gateway_impl;
    if (!gateway || !out_json)
        return AIRY_EINVAL;

    cJSON *stats = cJSON_CreateObject();
    if (!stats)
        return AIRY_ENOMEM;
    cJSON_AddNumberToObject(stats, "commands_total", (double)atomic_load(&gateway->commands_total));
    cJSON_AddNumberToObject(stats, "commands_failed",
                            (double)atomic_load(&gateway->commands_failed));
    cJSON_AddNumberToObject(stats, "bytes_received", (double)atomic_load(&gateway->bytes_received));
    cJSON_AddNumberToObject(stats, "bytes_sent", (double)atomic_load(&gateway->bytes_sent));

    char *json_str = cJSON_Print(stats);
    cJSON_Delete(stats);

    if (!json_str)
        return AIRY_ENOMEM;
    *out_json = json_str;
    return AIRY_SUCCESS;
}

/**
  * @brief Set the request handler callback
 */
static airy_err_t stdio_gateway_set_handler(void *gateway_impl, gateway_internal_handler_t handler,
                                            void *user_data)
{
    stdio_gateway_t *gateway = (stdio_gateway_t *)gateway_impl;
    if (!gateway)
        return AIRY_EINVAL;

    if (gateway->handler_adapter) {
        AIRY_FREE(gateway->handler_adapter);
        gateway->handler_adapter = NULL;
    }

    gateway->handler = handler;
    gateway->handler_data = user_data;

    return AIRY_SUCCESS;
}

static const gateway_ops_t stdio_gateway_ops = {.start = stdio_gateway_start,
                                                .stop = stdio_gateway_stop,
                                                .destroy = stdio_gateway_destroy,
                                                .get_name = stdio_gateway_get_name,
                                                .get_stats = stdio_gateway_get_stats,
                                                .is_running = stdio_gateway_is_running,
                                                .set_handler = stdio_gateway_set_handler};

gateway_t *stdio_gateway_create(void)
{
    stdio_gateway_t *gateway = AIRY_CALLOC(1, sizeof(stdio_gateway_t));
    if (!gateway) {
        return NULL;
    }

    gateway->handler_adapter = NULL;
    gateway->handler = NULL;
    gateway->handler_data = NULL;

    const char *env_bs = getenv("AIRY_STDIO_BUFFER_SIZE");
    gateway->input_buffer_size = env_bs ? (size_t)strtoul(env_bs, NULL, 10) : 8192;
    if (gateway->input_buffer_size < 1024)
        gateway->input_buffer_size = 1024;
    if (gateway->input_buffer_size > 1048576)
        gateway->input_buffer_size = 1048576;
    gateway->input_buffer = (char *)AIRY_MALLOC(gateway->input_buffer_size);
    if (!gateway->input_buffer) {
        AIRY_FREE(gateway);
        return NULL;
    }
    gateway->input_buffer_pos = 0;

    atomic_init(&gateway->running, false);
    atomic_init(&gateway->commands_total, 0);
    atomic_init(&gateway->commands_failed, 0);
    atomic_init(&gateway->bytes_received, 0);
    atomic_init(&gateway->bytes_sent, 0);

    gateway_t *gw = AIRY_MALLOC(sizeof(gateway_t));
    if (!gw) {

        AIRY_FREE(gateway->input_buffer);
        AIRY_FREE(gateway);
        return NULL;
    }

    gw->ops = &stdio_gateway_ops;
    gw->impl = gateway;
    gw->type = GATEWAY_TYPE_STDIO;
    gw->public_handler = NULL;
    gw->public_handler_data = NULL;

    return gw;
}
