#ifndef __KV_CONFIG_H
#define __KV_CONFIG_H

#include <stdint.h>
#include <string>

#include "kv_log.h"
#include "kv_persistent.h"

namespace kv_config
{
    enum class Role
    {
        STANDALONE,
        MASTER,
        SLAVE,
    };

    struct Config
    {
        uint16_t port{8050};
        kv_log::Level log_level{kv_log::Level::INFO};
        kv_persistent::PersistMode persist_mode{kv_persistent::PersistMode::AOF};
        Role role{Role::STANDALONE};

        // Only meaningful when role is SLAVE: the master's RDMA endpoint.
        std::string master_ip;
        uint16_t master_port{20000};
    };

    // Returns 0 on success. A missing file, an unknown section/key, or a bad
    // value is an error: silently ignoring a typo here could start the server
    // in the wrong replication role.
    int load(const char *path, Config &out);

    const char *role_name(Role role);

    const char *persist_mode_name(kv_persistent::PersistMode mode);
} // namespace kv_config

#endif // __KV_CONFIG_H
