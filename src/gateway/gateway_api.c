// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

// @owner: team-B
#include "error.h"
#include "gateway_internal.h"
#include "logging.h"
#include "airy_memory.h"

void gw_handler_bind(gateway_handler_slot_t *slot, gateway_internal_handler_t handler,
                     void *user_data)
{
    if (!slot)
        return;
    slot->handler = handler;
    slot->data = user_data;
}

int gw_handler_adapt(const char *request_json, char **response_json, void *user_data)
{
    gateway_handler_adapter_t *adapter = (gateway_handler_adapter_t *)user_data;
    if (!adapter || !adapter->internal_handler) {
        *response_json = NULL;
        airy_err_push_ex(AIRY_ERR_NULL_POINTER, __FILE__, __LINE__, __func__,
                         "adapter or internal handler is NULL");
        return AIRY_ERR_NULL_POINTER;
    }
    char *resp = adapter->internal_handler((void *)request_json, adapter->internal_data);
    if (resp) {
        *response_json = resp;
        return 0;
    }
    *response_json = NULL;
    airy_err_push_ex(AIRY_ERR_NULL_POINTER, __FILE__, __LINE__, __func__,
                     "internal handler returned NULL");
    return AIRY_ERR_NULL_POINTER;
}

void gateway_destroy(gateway_t *gw)
{
    if (!gw)
        return;
    if (gw->ops && gw->ops->destroy) {
        gw->ops->destroy(gw->impl);
    }
    AIRY_FREE(gw);
}

int gateway_start(gateway_t *gw)
{
    AIRY_CHECK(gw != NULL, AIRY_ERR_NULL_POINTER, "gw is NULL");
    AIRY_LOG_INFO("gateway_start: starting gateway (type=%d)", gw->type);
    int err = 0;
    if (gw->ops && gw->ops->start) {
        err = gw->ops->start(gw->impl);
    }
    if (err == 0) {
        AIRY_LOG_INFO("gateway_start: gateway started successfully");
    } else {
        AIRY_LOG_ERROR("gateway_start: start failed, err=%d", err);
    }
    return err;
}
