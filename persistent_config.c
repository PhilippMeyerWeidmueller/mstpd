/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * persistent_config.c  Persistent JSON configuration for bridges and ports
 */

#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <math.h>
#include <limits.h>
#include <stdint.h>
#include <asm/byteorder.h>

#include <cjson/cJSON.h>

#include "ctl_socket_server.h"
#include "persistent_config.h"
#include "log.h"
#include "driver.h"

static LIST_HEAD(bridge_configs);

static void free_port_config(stored_port_cfg_t *port_cfg)
{
    stored_msti_port_cfg_t *msti_cfg, *tmp;

    list_for_each_entry_safe(msti_cfg, tmp, &port_cfg->mstis, list)
    {
        list_del(&msti_cfg->list);
        free(msti_cfg);
    }
    free(port_cfg);
}

static void free_bridge_config(stored_bridge_cfg_t *br_cfg)
{
    stored_msti_bridge_cfg_t *msti_cfg, *tmp_msti;
    stored_port_cfg_t *port_cfg, *tmp_port;

    list_for_each_entry_safe(msti_cfg, tmp_msti, &br_cfg->mstis, list)
    {
        list_del(&msti_cfg->list);
        free(msti_cfg);
    }
    list_for_each_entry_safe(port_cfg, tmp_port, &br_cfg->ports, list)
    {
        list_del(&port_cfg->list);
        free_port_config(port_cfg);
    }
    free(br_cfg);
}

static void free_config_list(struct list_head *list)
{
    stored_bridge_cfg_t *br_cfg, *tmp;

    list_for_each_entry_safe(br_cfg, tmp, list, list)
    {
        list_del(&br_cfg->list);
        free_bridge_config(br_cfg);
    }
}

static void report_config_default(const char *br_name, const char *port_name,
                                  const char *field)
{
    if(port_name)
        ctl_err_log("Bridge '%s' port '%s': '%s' not set; using default.\n",
                    br_name, port_name, field);
    else
        ctl_err_log("Bridge '%s': '%s' not set; using default.\n",
                    br_name, field);
}

typedef enum { FIELD_ABSENT, FIELD_VALID, FIELD_INVALID } field_result_t;

static bool cjson_is_uint(cJSON *field)
{
    if(!cJSON_IsNumber(field))
        return false;

    double value = field->valuedouble;
    double integer_part;
    return isfinite(value) && value >= 0
        && modf(value, &integer_part) == 0;
}

static field_result_t get_json_uint(cJSON *parent, const char *key,
                                    unsigned int type_max,
                                    unsigned int *out)
{
    cJSON *field = cJSON_GetObjectItemCaseSensitive(parent, key);
    if(!field)
        return FIELD_ABSENT;
    if(!cjson_is_uint(field))
        return FIELD_INVALID;

    double value = field->valuedouble;
    *out = value > type_max ? type_max : (unsigned int)value;
    return FIELD_VALID;
}

static field_result_t get_json_protocol_version(cJSON *parent,
                                                const char *key,
                                                protocol_version_t *out)
{
    cJSON *field = cJSON_GetObjectItemCaseSensitive(parent, key);
    if(!field)
        return FIELD_ABSENT;
    if(!cJSON_IsString(field))
        return FIELD_INVALID;

    const char *value = field->valuestring;
    if(!strcmp(value, "stp"))
        *out = protoSTP;
    else if(!strcmp(value, "rstp"))
        *out = protoRSTP;
    else if(!strcmp(value, "mstp"))
        *out = protoMSTP;
    else
        return FIELD_INVALID;

    return FIELD_VALID;
}

static field_result_t get_json_bool(cJSON *parent, const char *key,
                                    bool *out)
{
    cJSON *field = cJSON_GetObjectItemCaseSensitive(parent, key);
    if(!field)
        return FIELD_ABSENT;
    if(!cJSON_IsBool(field))
        return FIELD_INVALID;

    *out = cJSON_IsTrue(field);
    return FIELD_VALID;
}

static field_result_t get_json_admin_p2p(cJSON *parent, const char *key,
                                         admin_p2p_t *out)
{
    cJSON *field = cJSON_GetObjectItemCaseSensitive(parent, key);
    if(!field)
        return FIELD_ABSENT;
    if(!cJSON_IsString(field))
        return FIELD_INVALID;

    const char *value = field->valuestring;
    if(!strcmp(value, "auto"))
        *out = p2pAuto;
    else if(!strcmp(value, "yes"))
        *out = p2pForceTrue;
    else if(!strcmp(value, "no"))
        *out = p2pForceFalse;
    else
        return FIELD_INVALID;

    return FIELD_VALID;
}

static bool parse_cist_bridge_config(const char *name, cJSON *json,
                                     stored_bridge_cfg_t *br_cfg)
{
    CIST_BridgeConfig *cfg = &br_cfg->cist_cfg;
    unsigned int u;

    switch(get_json_protocol_version(json, "force-protocol-version", &cfg->protocol_version))
    {
        case FIELD_INVALID:
            ERROR("Bridge '%s': 'force-protocol-version' must be one of "
                  "\"stp\", \"rstp\", \"mstp\"", name);
            return false;
        case FIELD_VALID:
            cfg->set_protocol_version = true;
            break;
        case FIELD_ABSENT:
            report_config_default(name, NULL, "force-protocol-version");
            break;
    }

    switch(get_json_uint(json, "bridge-priority", UINT8_MAX, &u))
    {
        case FIELD_INVALID:
            ERROR("Bridge '%s': 'bridge-priority' must be a non-negative integer",
                  name);
            return false;
        case FIELD_VALID:
            br_cfg->bridge_priority = u;
            br_cfg->set_bridge_priority = true;
            break;
        case FIELD_ABSENT:
            report_config_default(name, NULL, "bridge-priority");
            break;
    }

    switch(get_json_uint(json, "bridge-max-age", UINT8_MAX, &u))
    {
        case FIELD_INVALID:
            ERROR("Bridge '%s': 'bridge-max-age' must be a non-negative integer",
                  name);
            return false;
        case FIELD_VALID:
            cfg->bridge_max_age = u;
            cfg->set_bridge_max_age = true;
            break;
        case FIELD_ABSENT:
            report_config_default(name, NULL, "bridge-max-age");
            break;
    }

    switch(get_json_uint(json, "bridge-forward-delay", UINT8_MAX, &u))
    {
        case FIELD_INVALID:
            ERROR("Bridge '%s': 'bridge-forward-delay' must be a "
                "non-negative integer", name);
            return false;
        case FIELD_VALID:
            cfg->bridge_forward_delay = u;
            cfg->set_bridge_forward_delay = true;
            break;
        case FIELD_ABSENT:
            report_config_default(name, NULL, "bridge-forward-delay");
            break;
    }

    switch(get_json_uint(json, "hello-time", UINT8_MAX, &u))
    {
        case FIELD_INVALID:
            ERROR("Bridge '%s': 'hello-time' must be a non-negative integer",
                  name);
            return false;
        case FIELD_VALID:
            cfg->bridge_hello_time = u;
            cfg->set_bridge_hello_time = true;
            break;
        case FIELD_ABSENT:
            report_config_default(name, NULL, "hello-time");
            break;
    }

    switch(get_json_uint(json, "tx-hold-count", UINT_MAX, &u))
    {
        case FIELD_INVALID:
            ERROR("Bridge '%s': 'tx-hold-count' must be a non-negative integer",
                  name);
            return false;
        case FIELD_VALID:
            cfg->tx_hold_count = u;
            cfg->set_tx_hold_count = true;
            break;
        case FIELD_ABSENT:
            report_config_default(name, NULL, "tx-hold-count");
            break;
    }

    switch(get_json_uint(json, "ageing-time", UINT_MAX, &u))
    {
        case FIELD_INVALID:
            ERROR("Bridge '%s': 'ageing-time' must be a "
                "non-negative integer", name);
            return false;
        case FIELD_VALID:
            cfg->bridge_ageing_time = u;
            cfg->set_bridge_ageing_time = true;
            break;
        case FIELD_ABSENT:
            report_config_default(name, NULL, "ageing-time");
            break;
    }

    return true;
}

/* Sets array[idx] = value for every index in the range strings of
 * list_json ("N" or "N-M"), idx bounded to min_index..max_index */
static bool parse_index_list(cJSON *list_json, unsigned int min_index,
                             unsigned int max_index, unsigned int value,
                             __u16 *array)
{
    if(!cJSON_IsArray(list_json))
        return false;

    cJSON *item;
    cJSON_ArrayForEach(item, list_json)
    {
        if(!cJSON_IsString(item))
            return false;

        const char *s = item->valuestring;
        char *end;
        long first = strtol(s, &end, 10);
        if(end == s || first < min_index || first > max_index)
            return false;

        long last = first;
        if(*end == '-')
        {
            char *end2;
            last = strtol(end + 1, &end2, 10);
            if(end2 == end + 1 || *end2 != '\0'
               || last < min_index || last > max_index)
                return false;
        }
        else if(*end != '\0')
            return false;

        if(first > last)
        {
            long tmp = first;
            first = last;
            last = tmp;
        }

        long idx;
        for(idx = first; idx <= last; ++idx)
            array[idx] = (__u16)value;
    }
    return true;
}

static bool parse_vid2fid(const char *name, cJSON *mstp_json,
                          stored_bridge_cfg_t *br_cfg)
{
    cJSON *list_json = cJSON_GetObjectItemCaseSensitive(mstp_json, "vid2fid");
    if(!list_json)
    {
        report_config_default(name, NULL, "mstp.vid2fid");
        return true;
    }
    if(!cJSON_IsArray(list_json))
    {
        ERROR("Bridge '%s': 'mstp.vid2fid' must be an array", name);
        return false;
    }

    cJSON *entry;
    cJSON_ArrayForEach(entry, list_json)
    {
        unsigned int fid;
        if(!cJSON_IsObject(entry)
           || FIELD_VALID != get_json_uint(entry, "fid", UINT16_MAX, &fid)
           || !parse_index_list(cJSON_GetObjectItemCaseSensitive(entry, "vids"),
                                1, MAX_VID, fid, br_cfg->vid2fid))
        {
            ERROR("Bridge '%s': invalid 'mstp.vid2fid' entry", name);
            return false;
        }
    }

    br_cfg->set_vid2fid = true;
    return true;
}

static bool parse_fid2mstid(const char *name, cJSON *mstp_json,
                           stored_bridge_cfg_t *br_cfg)
{
    cJSON *list_json = cJSON_GetObjectItemCaseSensitive(mstp_json, "fid2mstid");
    if(!list_json)
    {
        report_config_default(name, NULL, "mstp.fid2mstid");
        return true;
    }
    if(!cJSON_IsArray(list_json))
    {
        ERROR("Bridge '%s': 'mstp.fid2mstid' must be an array", name);
        return false;
    }

    cJSON *entry;
    cJSON_ArrayForEach(entry, list_json)
    {
        unsigned int mstid;
        if(!cJSON_IsObject(entry)
           || FIELD_VALID != get_json_uint(entry, "mstid", UINT16_MAX,
                                           &mstid)
           || !parse_index_list(cJSON_GetObjectItemCaseSensitive(entry, "fids"),
                                0, MAX_FID, mstid, br_cfg->fid2mstid))
        {
            ERROR("Bridge '%s': invalid 'mstp.fid2mstid' entry", name);
            return false;
        }
    }

    br_cfg->set_fid2mstid = true;
    return true;
}

static bool parse_mst_config_id(const char *name, cJSON *mstp_json,
                                stored_bridge_cfg_t *br_cfg)
{
    cJSON *cfg_json = cJSON_GetObjectItemCaseSensitive(mstp_json, "mst-config-id");
    if(!cfg_json)
    {
        report_config_default(name, NULL, "mstp.mst-config-id");
        return true;
    }

    cJSON *name_json = cJSON_GetObjectItemCaseSensitive(cfg_json, "configuration-name");
    if(!cJSON_IsObject(cfg_json) || !name_json || !cJSON_IsString(name_json))
    {
        ERROR("Bridge '%s': 'mstp.mst-config-id.configuration-name' must be a string",
              name);
        return false;
    }
    /* longer names are truncated silently, like the daemon's own parser.
     * memcpy (not strncpy) avoids a bogus -Wstringop-truncation warning,
     * since the field isn't meant to be null-terminated */
    const char *cfg_name = name_json->valuestring;
    size_t cfg_name_len = strlen(cfg_name);
    if(cfg_name_len > sizeof(br_cfg->mst_config_id_name))
        cfg_name_len = sizeof(br_cfg->mst_config_id_name);
    memcpy(br_cfg->mst_config_id_name, cfg_name, cfg_name_len);

    unsigned int revision = 0;
    switch(get_json_uint(cfg_json, "revision-level", UINT16_MAX, &revision))
    {
        case FIELD_INVALID:
            ERROR("Bridge '%s': 'mstp.mst-config-id.revision-level' must be an "
                  "non-negative integer", name);
            return false;
        case FIELD_VALID:
            break;
        case FIELD_ABSENT:
            report_config_default(name, NULL, "mstp.mst-config-id.revision-level");
            break;
    }
    br_cfg->mst_config_id_revision = revision;

    br_cfg->set_mst_config_id = true;
    return true;
}

static bool parse_msti_list(const char *name, cJSON *mstp_json,
                            stored_bridge_cfg_t *br_cfg)
{
    cJSON *msti_json = cJSON_GetObjectItemCaseSensitive(mstp_json, "msti");
    if(!msti_json)
        return true;
    if(!cJSON_IsObject(msti_json))
    {
        ERROR("Bridge '%s': 'mstp.msti' must be an object", name);
        return false;
    }

    MSTP_BridgeDefaultConfig *defaults = malloc(sizeof(*defaults));
    if(!defaults)
    {
        ERROR("Out of memory");
        return false;
    }
    const __u8 macaddr[ETH_ALEN] = { 0 };
    MSTP_IN_get_bridge_default_config(defaults, macaddr);
    __u8 default_priority = defaults->bridge_priority;
    free(defaults);

    cJSON *value;
    cJSON_ArrayForEach(value, msti_json)
    {
        const char *key = value->string;
        char *end;
        long mstid = strtol(key, &end, 10);
        if(*end != '\0' || mstid < 1 || mstid > MAX_IMPLEMENTATION_MSTIS)
        {
            ERROR("Bridge '%s': invalid MSTID '%s' in 'mstp.msti'", name,
                  key);
            return false;
        }

        unsigned int priority = default_priority;
        if(!cJSON_IsObject(value)
            || FIELD_INVALID ==
                get_json_uint(value, "bridge-priority", UINT8_MAX, &priority))
        {
            ERROR("Bridge '%s': 'mstp.msti.%s' must be an object with an "
                "optional non-negative integer 'bridge-priority'", name, key);
            return false;
        }

        if(!cJSON_GetObjectItemCaseSensitive(value, "bridge-priority"))
        {
            char field[64];
            snprintf(field, sizeof(field), "mstp.msti.%ld.bridge-priority",
                     mstid);
            report_config_default(name, NULL, field);
        }

        stored_msti_bridge_cfg_t *msti_cfg = calloc(1, sizeof(*msti_cfg));
        if(!msti_cfg)
        {
            ERROR("Out of memory");
            return false;
        }
        msti_cfg->mstid = (__u16)mstid;
        msti_cfg->bridge_priority = (__u8)priority;
        list_add_tail(&msti_cfg->list, &br_cfg->mstis);
    }

    return true;
}

static bool parse_mstp_bridge_config(const char *name, cJSON *json,
                                    stored_bridge_cfg_t *br_cfg)
{
    cJSON *mstp_json = cJSON_GetObjectItemCaseSensitive(json, "mstp");
    if(!mstp_json)
    {
        report_config_default(name, NULL, "mstp.max-hops");
        report_config_default(name, NULL, "mstp.mst-config-id");
        report_config_default(name, NULL, "mstp.vid2fid");
        report_config_default(name, NULL, "mstp.fid2mstid");
        return true;
    }
    if(!cJSON_IsObject(mstp_json))
    {
        ERROR("Bridge '%s': 'mstp' must be an object", name);
        return false;
    }

    unsigned int u;
    switch(get_json_uint(mstp_json, "max-hops", UINT8_MAX, &u))
    {
        case FIELD_INVALID:
            ERROR("Bridge '%s': 'mstp.max-hops' must be a non-negative integer",
                  name);
            return false;
        case FIELD_VALID:
            br_cfg->cist_cfg.max_hops = u;
            br_cfg->cist_cfg.set_max_hops = true;
            break;
        case FIELD_ABSENT:
            report_config_default(name, NULL, "mstp.max-hops");
            break;
    }

    return parse_mst_config_id(name, mstp_json, br_cfg)
        && parse_vid2fid(name, mstp_json, br_cfg)
        && parse_fid2mstid(name, mstp_json, br_cfg)
        && parse_msti_list(name, mstp_json, br_cfg);
}

static bool set_cist_bool_field(const char *br_name, const char *port_name,
                                cJSON *json, const char *key,
                                bool *value_field, bool *set_field)
{
    bool b;
    switch(get_json_bool(json, key, &b))
    {
        case FIELD_INVALID:
            ERROR("Bridge '%s' port '%s': '%s' must be a boolean", br_name,
                  port_name, key);
            return false;
        case FIELD_VALID:
            *value_field = b;
            *set_field = true;
            break;
        case FIELD_ABSENT:
            report_config_default(br_name, port_name, key);
            break;
    }
    return true;
}

static bool parse_cist_port_config(const char *br_name, const char *port_name,
                                   cJSON *json, stored_port_cfg_t *port_cfg)
{
    CIST_PortConfig *cfg = &port_cfg->cist_cfg;
    unsigned int u;

    switch(get_json_uint(json, "port-priority", UINT8_MAX, &u))
    {
        case FIELD_INVALID:
            ERROR("Bridge '%s' port '%s': 'port-priority' must be an "
                  "non-negative integer", br_name, port_name);
            return false;
        case FIELD_VALID:
            port_cfg->port_priority = u;
            port_cfg->set_port_priority = true;
            break;
        case FIELD_ABSENT:
            report_config_default(br_name, port_name, "port-priority");
            break;
    }

    switch(get_json_uint(json, "admin-external-cost",
                         UINT32_MAX, &u))
    {
        case FIELD_INVALID:
            ERROR("Bridge '%s' port '%s': "
                  "'admin-external-cost' must be a "
                  "non-negative integer", br_name, port_name);
            return false;
        case FIELD_VALID:
            cfg->admin_external_port_path_cost = u;
            cfg->set_admin_external_port_path_cost = true;
            break;
        case FIELD_ABSENT:
            report_config_default(br_name, port_name,
                                  "admin-external-cost");
            break;
    }

    switch(get_json_admin_p2p(json, "admin-point-to-point", &cfg->admin_p2p))
    {
        case FIELD_INVALID:
            ERROR("Bridge '%s' port '%s': 'admin-point-to-point' must be \"auto\", "
                  "\"yes\" or \"no\"", br_name, port_name);
            return false;
        case FIELD_VALID:
            cfg->set_admin_p2p = true;
            break;
        case FIELD_ABSENT:
            report_config_default(br_name, port_name, "admin-point-to-point");
            break;
    }

    return set_cist_bool_field(br_name, port_name, json, "admin-edge-port",
                               &cfg->admin_edge_port,
                               &cfg->set_admin_edge_port)
        && set_cist_bool_field(br_name, port_name, json, "auto-edge-port",
                              &cfg->auto_edge_port, &cfg->set_auto_edge_port)
        && set_cist_bool_field(br_name, port_name, json, "restricted-role",
                              &cfg->restricted_role,
                              &cfg->set_restricted_role)
        && set_cist_bool_field(br_name, port_name, json, "restricted-TCN",
                              &cfg->restricted_tcn,
                              &cfg->set_restricted_tcn)
        && set_cist_bool_field(br_name, port_name, json, "bpdu-guard-port",
                              &cfg->bpdu_guard_port,
                              &cfg->set_bpdu_guard_port)
        && set_cist_bool_field(br_name, port_name, json, "bpdu-filter-port",
                              &cfg->bpdu_filter_port,
                              &cfg->set_bpdu_filter_port)
        && set_cist_bool_field(br_name, port_name, json, "network-port",
                              &cfg->network_port, &cfg->set_network_port)
        && set_cist_bool_field(br_name, port_name, json, "dont-txmt",
                              &cfg->dont_txmt, &cfg->set_dont_txmt);
}

static bool parse_msti_port_list(const char *br_name, const char *port_name,
                                 cJSON *mstp_json,
                                 stored_port_cfg_t *port_cfg)
{
    cJSON *msti_json = cJSON_GetObjectItemCaseSensitive(mstp_json, "msti");
    if(!msti_json)
        return true;
    if(!cJSON_IsObject(msti_json))
    {
        ERROR("Bridge '%s' port '%s': 'mstp.msti' must be an object",
              br_name, port_name);
        return false;
    }

    MSTP_PortDefaultConfig defaults;
    MSTP_IN_get_port_default_config(&defaults);
    cJSON *value;
    cJSON_ArrayForEach(value, msti_json)
    {
        const char *key = value->string;
        char *end;
        long mstid = strtol(key, &end, 10);
        if(*end != '\0' || mstid < 1 || mstid > MAX_IMPLEMENTATION_MSTIS
           || !cJSON_IsObject(value))
        {
            ERROR("Bridge '%s' port '%s': invalid 'mstp.msti' entry '%s'",
                  br_name, port_name, key);
            return false;
        }

        stored_msti_port_cfg_t *msti_cfg = calloc(1, sizeof(*msti_cfg));
        if(!msti_cfg)
        {
            ERROR("Out of memory");
            return false;
        }
        msti_cfg->mstid = (__u16)mstid;
        msti_cfg->cfg = defaults.msti_cfg;

        unsigned int u;
        char field[64];
        switch(get_json_uint(value, "port-priority", UINT8_MAX, &u))
        {
            case FIELD_INVALID:
                ERROR("Bridge '%s' port '%s': "
                      "'mstp.msti.%s.port-priority' must be a "
                      "non-negative integer", br_name, port_name, key);
                free(msti_cfg);
                return false;
            case FIELD_VALID:
                msti_cfg->cfg.port_priority = u;
                msti_cfg->cfg.set_port_priority = true;
                break;
            case FIELD_ABSENT:
                snprintf(field, sizeof(field), "mstp.msti.%ld.port-priority",
                         mstid);
                report_config_default(br_name, port_name, field);
                break;
        }

        switch(get_json_uint(value, "admin-internal-cost",
                     UINT32_MAX, &u))
        {
            case FIELD_INVALID:
                ERROR("Bridge '%s' port '%s': "
                      "'mstp.msti.%s.admin-internal-cost' must "
                      "be a non-negative integer", br_name, port_name, key);
                free(msti_cfg);
                return false;
            case FIELD_VALID:
                msti_cfg->cfg.admin_internal_port_path_cost = u;
                msti_cfg->cfg.set_admin_internal_port_path_cost = true;
                break;
            case FIELD_ABSENT:
                snprintf(field, sizeof(field),
                         "mstp.msti.%ld.admin-internal-cost", mstid);
                report_config_default(br_name, port_name, field);
                break;
        }

        list_add_tail(&msti_cfg->list, &port_cfg->mstis);
    }

    return true;
}

static bool parse_mstp_port_config(const char *br_name, const char *port_name,
                                   cJSON *json, stored_port_cfg_t *port_cfg)
{
    cJSON *mstp_json = cJSON_GetObjectItemCaseSensitive(json, "mstp");
    if(!mstp_json)
    {
        report_config_default(br_name, port_name,
                              "mstp.cist-admin-internal-cost");
        return true;
    }
    if(!cJSON_IsObject(mstp_json))
    {
        ERROR("Bridge '%s' port '%s': 'mstp' must be an object", br_name,
              port_name);
        return false;
    }

    unsigned int u;
    switch(get_json_uint(mstp_json, "cist-admin-internal-cost",
                         UINT32_MAX, &u))
    {
        case FIELD_INVALID:
            ERROR("Bridge '%s' port '%s': "
                  "'mstp.cist-admin-internal-cost' must be an "
                  "non-negative integer", br_name, port_name);
            return false;
        case FIELD_VALID:
            port_cfg->cist_admin_internal_port_path_cost = u;
            port_cfg->set_cist_internal_port_path_cost = true;
            break;
        case FIELD_ABSENT:
            report_config_default(br_name, port_name,
                                  "mstp.cist-admin-internal-cost");
            break;
    }

    return parse_msti_port_list(br_name, port_name, mstp_json, port_cfg);
}

static bool parse_port_config(const char *br_name, const char *name,
                              cJSON *json, stored_port_cfg_t **out)
{
    stored_port_cfg_t *port_cfg;

    if(strlen(name) >= sizeof(port_cfg->name))
    {
        ERROR("Bridge '%s': port name '%s' is too long", br_name, name);
        return false;
    }
    if(!cJSON_IsObject(json))
    {
        ERROR("Bridge '%s': configuration for port '%s' is not an object",
              br_name, name);
        return false;
    }

    if(!(port_cfg = calloc(1, sizeof(*port_cfg))))
    {
        ERROR("Out of memory");
        return false;
    }
    MSTP_PortDefaultConfig defaults;
    MSTP_IN_get_port_default_config(&defaults);
    port_cfg->cist_cfg = defaults.cist_cfg;
    port_cfg->port_priority = defaults.msti_cfg.port_priority;
    port_cfg->set_port_priority = defaults.msti_cfg.set_port_priority;
    port_cfg->cist_admin_internal_port_path_cost =
        defaults.msti_cfg.admin_internal_port_path_cost;
    port_cfg->set_cist_internal_port_path_cost =
        defaults.msti_cfg.set_admin_internal_port_path_cost;

    strcpy(port_cfg->name, name);
    INIT_LIST_HEAD(&port_cfg->mstis);

    if(!parse_cist_port_config(br_name, name, json, port_cfg)
       || !parse_mstp_port_config(br_name, name, json, port_cfg))
    {
        free_port_config(port_cfg);
        return false;
    }

    *out = port_cfg;
    return true;
}

static bool parse_ports(const char *br_name, cJSON *json,
                        stored_bridge_cfg_t *br_cfg)
{
    cJSON *ports_json = cJSON_GetObjectItemCaseSensitive(json, "ports");
    if(!ports_json)
        return true;
    if(!cJSON_IsObject(ports_json))
    {
        ERROR("Bridge '%s': 'ports' must be an object", br_name);
        return false;
    }

    cJSON *value;
    cJSON_ArrayForEach(value, ports_json)
    {
        const char *name = value->string;
        stored_port_cfg_t *port_cfg;
        if(!parse_port_config(br_name, name, value, &port_cfg))
            return false;
        list_add_tail(&port_cfg->list, &br_cfg->ports);
    }

    return true;
}

static bool parse_bridge_config(const char *name, cJSON *json,
                                stored_bridge_cfg_t **out)
{
    stored_bridge_cfg_t *br_cfg;

    if(strlen(name) >= sizeof(br_cfg->name))
    {
        ERROR("Bridge name '%s' is too long", name);
        return false;
    }
    if(!cJSON_IsObject(json))
    {
        ERROR("Configuration for bridge '%s' is not an object", name);
        return false;
    }

    if(!(br_cfg = calloc(1, sizeof(*br_cfg))))
    {
        ERROR("Out of memory");
        return false;
    }
    MSTP_BridgeDefaultConfig *defaults = malloc(sizeof(*defaults));
    if(!defaults)
    {
        ERROR("Out of memory");
        free(br_cfg);
        return false;
    }
    const __u8 macaddr[ETH_ALEN] = { 0 };
    MSTP_IN_get_bridge_default_config(defaults, macaddr);
    br_cfg->cist_cfg = defaults->cist_cfg;
    br_cfg->bridge_priority = defaults->bridge_priority;
    br_cfg->set_bridge_priority = true;
    memcpy(br_cfg->vid2fid, defaults->vid2fid, sizeof(br_cfg->vid2fid));
    br_cfg->set_vid2fid = true;
    memcpy(br_cfg->fid2mstid, defaults->fid2mstid, sizeof(br_cfg->fid2mstid));
    br_cfg->set_fid2mstid = true;
    free(defaults);

    strcpy(br_cfg->name, name);
    INIT_LIST_HEAD(&br_cfg->mstis);
    INIT_LIST_HEAD(&br_cfg->ports);

    if(!parse_cist_bridge_config(name, json, br_cfg)
       || !parse_mstp_bridge_config(name, json, br_cfg)
       || !parse_ports(name, json, br_cfg))
    {
        free_bridge_config(br_cfg);
        return false;
    }

    *out = br_cfg;
    return true;
}

const char *persistent_config_resolve(const char *path)
{
    if(path && path[0] != '\0')
        return path;
    if(0 == access(MSTPD_ETC_CONFIG_FILE, R_OK))
        return MSTPD_ETC_CONFIG_FILE;
    if(0 == access(MSTPD_LIB_CONFIG_FILE, R_OK))
        return MSTPD_LIB_CONFIG_FILE;
    return NULL;
}

int persistent_config_load(const char *path)
{
    if(0 != access(path, F_OK))
    {
        INFO("No configuration file at %s, using defaults", path);
        return -1;
    }

    FILE *f = fopen(path, "rb");
    if(!f)
    {
        ERROR("Couldn't open configuration file %s: %s", path,
              strerror(errno));
        return -1;
    }

    if(0 != fseek(f, 0, SEEK_END))
    {
        ERROR("Couldn't determine size of configuration file %s", path);
        fclose(f);
        return -1;
    }
    long size = ftell(f);
    if(size < 0 || 0 != fseek(f, 0, SEEK_SET))
    {
        ERROR("Couldn't determine size of configuration file %s", path);
        fclose(f);
        return -1;
    }

    char *buf = malloc((size_t)size + 1);
    if(!buf)
    {
        ERROR("Out of memory");
        fclose(f);
        return -1;
    }
    size_t read_len = fread(buf, 1, (size_t)size, f);
    fclose(f);
    buf[read_len] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if(!root)
    {
        const char *err_ptr = cJSON_GetErrorPtr();
        int line = 1;
        if(err_ptr)
        {
            const char *p;
            for(p = buf; p < err_ptr; ++p)
                if(*p == '\n')
                    ++line;
        }
        ERROR("Couldn't parse configuration file %s: error near line %d",
              path, line);
        free(buf);
        return -1;
    }
    free(buf);

    cJSON *bridges_json = cJSON_GetObjectItemCaseSensitive(root, "bridges");
    if(!bridges_json || !cJSON_IsObject(bridges_json))
    {
        ERROR("Configuration file %s: missing or invalid 'bridges' object",
              path);
        cJSON_Delete(root);
        return -1;
    }

    struct list_head new_configs;
    INIT_LIST_HEAD(&new_configs);

    bool ok = true;
    cJSON *value;
    cJSON_ArrayForEach(value, bridges_json)
    {
        const char *name = value->string;
        stored_bridge_cfg_t *br_cfg;
        if(!parse_bridge_config(name, value, &br_cfg))
        {
            ok = false;
            break;
        }
        list_add_tail(&br_cfg->list, &new_configs);
    }

    cJSON_Delete(root);

    if(!ok)
    {
        free_config_list(&new_configs);
        ERROR("Configuration file %s has errors, keeping previous "
              "configuration", path);
        return -1;
    }

    free_config_list(&bridge_configs);
    list_splice_init(&new_configs, &bridge_configs);

    INFO("Loaded configuration from %s", path);
    return 0;
}

static stored_bridge_cfg_t *find_stored_bridge(const char *name)
{
    stored_bridge_cfg_t *br_cfg;

    list_for_each_entry(br_cfg, &bridge_configs, list)
        if(!strcmp(br_cfg->name, name))
            return br_cfg;
    return NULL;
}

static stored_port_cfg_t *find_stored_port(stored_bridge_cfg_t *br_cfg,
                                           const char *name)
{
    stored_port_cfg_t *port_cfg;

    list_for_each_entry(port_cfg, &br_cfg->ports, list)
        if(!strcmp(port_cfg->name, name))
            return port_cfg;
    return NULL;
}

static tree_t *find_tree(bridge_t *br, __u16 mstid)
{
    tree_t *tree;
    __be16 wanted = __cpu_to_be16(mstid);

    list_for_each_entry(tree, &br->trees, bridge_list)
        if(tree->MSTID == wanted)
            return tree;
    return NULL;
}

static per_tree_port_t *find_ptp(port_t *prt, __u16 mstid)
{
    per_tree_port_t *ptp;
    __be16 wanted = __cpu_to_be16(mstid);

    list_for_each_entry(ptp, &prt->trees, port_list)
        if(ptp->MSTID == wanted)
            return ptp;
    return NULL;
}

void persistent_config_apply_to_bridge(bridge_t *br)
{
    stored_bridge_cfg_t *br_cfg = find_stored_bridge(br->sysdeps.name);
    if(!br_cfg)
        return;

    if(0 != MSTP_IN_set_cist_bridge_config(br, &br_cfg->cist_cfg))
        ERROR_BRNAME(br, "Couldn't apply stored bridge configuration");

    if(br_cfg->set_bridge_priority
       && 0 != MSTP_IN_set_msti_bridge_config(GET_CIST_TREE(br),
                                              br_cfg->bridge_priority))
        ERROR_BRNAME(br, "Couldn't apply stored bridge priority");

    if(br_cfg->set_mst_config_id)
        MSTP_IN_set_mst_config_id(br, br_cfg->mst_config_id_revision,
                                  br_cfg->mst_config_id_name);
    else
    {
        MSTP_BridgeDefaultConfig *defaults = malloc(sizeof(*defaults));
        if(!defaults)
        {
            ERROR_BRNAME(br, "Out of memory");
            return;
        }
        MSTP_IN_get_bridge_default_config(defaults, br->sysdeps.macaddr);
        MSTP_IN_set_mst_config_id(br, defaults->mst_config_id_revision,
                                  defaults->mst_config_id_name);
        free(defaults);
    }

    if(br_cfg->set_vid2fid && !MSTP_IN_set_all_vids2fids(br, br_cfg->vid2fid))
        ERROR_BRNAME(br, "Couldn't apply stored vid2fid table");

    stored_msti_bridge_cfg_t *msti_cfg;
    list_for_each_entry(msti_cfg, &br_cfg->mstis, list)
    {
        tree_t *tree = find_tree(br, msti_cfg->mstid);
        if(!tree)
        {
            if(!driver_create_msti(br, msti_cfg->mstid)
               || !MSTP_IN_create_msti(br, msti_cfg->mstid))
            {
                ERROR_BRNAME(br, "Couldn't create stored MSTI %hu",
                             msti_cfg->mstid);
                continue;
            }
            tree = find_tree(br, msti_cfg->mstid);
        }
        if(tree && 0 != MSTP_IN_set_msti_bridge_config(
                       tree, msti_cfg->bridge_priority))
            ERROR_BRNAME(br, "Couldn't apply stored priority for MSTI %hu",
                        msti_cfg->mstid);
    }

    if(br_cfg->set_fid2mstid
       && !MSTP_IN_set_all_fids2mstids(br, br_cfg->fid2mstid))
        ERROR_BRNAME(br, "Couldn't apply stored fid2mstid table");
}

void persistent_config_apply_to_port(port_t *prt)
{
    stored_bridge_cfg_t *br_cfg =
        find_stored_bridge(prt->bridge->sysdeps.name);
    if(!br_cfg)
        return;

    stored_port_cfg_t *port_cfg = find_stored_port(br_cfg,
                                                   prt->sysdeps.name);
    if(!port_cfg)
        return;

    if(0 != MSTP_IN_set_cist_port_config(prt, &port_cfg->cist_cfg))
        ERROR_PRTNAME(prt, "Couldn't apply stored port configuration");

    if(port_cfg->set_port_priority || port_cfg->set_cist_internal_port_path_cost)
    {
        MSTI_PortConfig cfg = { 0 };
        if(port_cfg->set_port_priority)
        {
            cfg.port_priority = port_cfg->port_priority;
            cfg.set_port_priority = true;
        }
        if(port_cfg->set_cist_internal_port_path_cost)
        {
            cfg.admin_internal_port_path_cost =
                port_cfg->cist_admin_internal_port_path_cost;
            cfg.set_admin_internal_port_path_cost = true;
        }
        if(0 != MSTP_IN_set_msti_port_config(GET_CIST_PTP_FROM_PORT(prt),
                                             &cfg))
            ERROR_PRTNAME(prt, "Couldn't apply stored CIST port priority/"
                         "cost");
    }

    stored_msti_port_cfg_t *msti_cfg;
    list_for_each_entry(msti_cfg, &port_cfg->mstis, list)
    {
        per_tree_port_t *ptp = find_ptp(prt, msti_cfg->mstid);
        if(!ptp)
        {
            ERROR_PRTNAME(prt, "Stored MSTI %hu doesn't exist on this "
                         "bridge", msti_cfg->mstid);
            continue;
        }
        if(0 != MSTP_IN_set_msti_port_config(ptp, &msti_cfg->cfg))
            ERROR_PRTNAME(prt, "Couldn't apply stored port configuration "
                         "for MSTI %hu", msti_cfg->mstid);
    }
}

