/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * persistent_config.h  Persistent JSON configuration for bridges and ports
 *
 * Stores the configuration loaded from a JSON file for newly created
 * bridges/ports. Reload replaces the stored configuration only; reapply
 * also applies it to existing bridges/ports.
 */

#ifndef PERSISTENT_CONFIG_H
#define PERSISTENT_CONFIG_H

#include <net/if.h>

#include "list.h"
#include "mstp.h"

#define MSTPD_ETC_CONFIG_FILE "/etc/mstpd/config.json"
#define MSTPD_LIB_CONFIG_FILE "/usr/lib/mstpd/config.json"

typedef struct
{
    struct list_head list; /* anchor in stored_bridge_cfg_t.mstis */
    __u16 mstid;
    __u8 bridge_priority;
} stored_msti_bridge_cfg_t;

typedef struct
{
    struct list_head list; /* anchor in stored_port_cfg_t.mstis */
    __u16 mstid;
    MSTI_PortConfig cfg;
} stored_msti_port_cfg_t;

typedef struct
{
    struct list_head list; /* anchor in stored_bridge_cfg_t.ports */
    char name[IFNAMSIZ];
    bool set_port_priority;
    __u8 port_priority;
    CIST_PortConfig cist_cfg;
    bool set_cist_internal_port_path_cost;
    __u32 cist_admin_internal_port_path_cost;
    struct list_head mstis; /* list of stored_msti_port_cfg_t */
} stored_port_cfg_t;

typedef struct
{
    struct list_head list; /* anchor in the global configuration list */
    char name[IFNAMSIZ];
    bool set_bridge_priority;
    __u8 bridge_priority;
    CIST_BridgeConfig cist_cfg; /* also carries protocol_version */
    bool set_mst_config_id;
    __u16 mst_config_id_revision;
    __u8 mst_config_id_name[CONFIGURATION_NAME_LEN];
    bool set_vid2fid;
    __u16 vid2fid[MAX_VID + 1];
    bool set_fid2mstid;
    __u16 fid2mstid[MAX_FID + 1];
    struct list_head mstis; /* list of stored_msti_bridge_cfg_t */
    struct list_head ports; /* list of stored_port_cfg_t */
} stored_bridge_cfg_t;

const char *persistent_config_resolve(const char *path);

int persistent_config_load(const char *path);

/* Apply the currently stored configuration to a single, newly created
 * bridge resp. port. Does nothing if no configuration is stored for it. */
void persistent_config_apply_to_bridge(bridge_t *br);
void persistent_config_apply_to_port(port_t *prt);

#endif /* PERSISTENT_CONFIG_H */
