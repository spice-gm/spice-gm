/* -*- Mode: C; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
   Copyright (C) 2009-2015 Red Hat, Inc.

   This library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; either
   version 2.1 of the License, or (at your option) any later version.

   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with this library; if not, see <http://www.gnu.org/licenses/>.
*/

#ifndef REDS_PRIVATE_H_
#define REDS_PRIVATE_H_

#include <spice/protocol.h>
#include <spice/stats.h>

#include "main-dispatcher.h"
#include "main-channel.h"
#include "inputs-channel.h"
#include "stat-file.h"
#include "red-record-qxl.h"
#include "safe-list.hpp"

#define MIGRATE_TIMEOUT (MSEC_PER_SEC * 10)
#define MM_TIME_DELTA 400 /*ms*/

typedef struct TicketAuthentication {
    char password[SPICE_MAX_PASSWORD_LENGTH];
    time_t expiration_time;
} TicketAuthentication;

typedef struct TicketInfo {
    RSA *rsa;
    int rsa_size;
    BIGNUM *bn;
    SpiceLinkEncryptedTicket encrypted_ticket;
} TicketInfo;

typedef struct MonitorMode {
    uint32_t x_res;
    uint32_t y_res;
} MonitorMode;

typedef struct RedsMigPendingLink {
    SpiceLinkMess *link_msg;
    RedStream *stream;
} RedsMigPendingLink;

typedef struct RedsMigTargetClient {
    RedClient *client;
    GList *pending_links;
} RedsMigTargetClient;

typedef struct ChannelSecurityOptions ChannelSecurityOptions;

typedef struct RedSSLParameters {
    char keyfile_password[256];
    char certs_file[256];
    char private_key_file[256];
    char ca_certificate_file[256];
    char dh_key_file[256];
    char ciphersuite[256];
} RedSSLParameters;

typedef struct RedCharDeviceVDIPort RedCharDeviceVDIPort;
typedef struct RedServerConfig RedServerConfig;

struct RedsState {
    SPICE_CXX_GLIB_ALLOCATOR

    RedServerConfig *config;
    int listen_socket;
    int secure_listen_socket;
    SpiceWatch *listen_watch;
    SpiceWatch *secure_listen_watch;
    red::shared_ptr<RedCharDeviceVDIPort> agent_dev;
    int pending_mouse_event;
    bool pending_device_display_info_message;
    red::safe_list<RedClient*> clients;
    red::shared_ptr<MainChannel> main_channel;
    red::shared_ptr<InputsChannel> inputs_channel;

    int mig_wait_connect; /* src waits for clients to establish connection to dest
                             (before migration starts) */
    int mig_wait_disconnect; /* src waits for clients to disconnect (after migration completes) */
    std::forward_list<RedClient*, red::Mallocator<RedClient*>>
        mig_wait_disconnect_clients;/* List of RedsMigWaitDisconnectClient. Holds the clients
                                         which the src waits for their disconnection */


    int mig_inprogress;
    int expect_migrate;
    int src_do_seamless_migrate; /* per migration. Updated after the migration handshake
                                    between the 2 servers */
    int dst_do_seamless_migrate; /* per migration. Updated after the migration handshake
                                    between the 2 servers */
    GList *mig_target_clients;

    red::safe_list<red::shared_ptr<RedChannel>> channels;
    SpiceMouseMode mouse_mode;
    int is_client_mouse_allowed;
    int dispatcher_allows_client_mouse;
    MonitorMode monitor_mode;
    SpiceTimer *mig_timer;

    int vm_running;
    red::safe_list<red::shared_ptr<RedCharDevice>> char_devices;
    int seamless_migration_enabled; /* command line arg */

    SSL_CTX *ctx;

#ifdef RED_STATISTICS
    RedStatFile *stat_file;
#endif
    int allow_multiple_clients;
    bool late_initialization_done;

    /* Intermediate state for on going monitors config message from a single
     * client, being passed to the guest */
    SpiceBuffer client_monitors_config;

    int mm_time_enabled;
    uint32_t mm_time_latency;

    SpiceCharDeviceInstance *vdagent;
    SpiceMigrateInstance *migration_interface;

    SpiceCoreInterfaceInternal core;
    red::safe_list<QXLInstance*> qxl_instances; // XXX owning
    red::shared_ptr<MainDispatcher> main_dispatcher;
    RedRecord *record;
};

#define FOREACH_QXL_INSTANCE(_reds, _qxl) \
    for (auto _qxl: _reds->qxl_instances)

#endif /* REDS_PRIVATE_H_ */
