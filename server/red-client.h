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

#ifndef RED_CLIENT_H_
#define RED_CLIENT_H_

#include "main-channel-client.h"
#include "safe-list.hpp"

#include "push-visibility.h"

RedClient *red_client_new(RedsState *reds, int migrated);

class RedClient final
{
public:
    SPICE_CXX_GLIB_ALLOCATOR

    RedClient(RedsState *reds, bool migrated);
protected:
    ~RedClient();

public:
    void ref() { g_atomic_int_inc(&_ref); }
    void unref() { if (g_atomic_int_dec_and_test(&_ref)) delete this; }

    /*
     * disconnects all the client's channels (should be called from the client's thread)
     */
    void destroy();

    gboolean add_channel(RedChannelClient *rcc, char **error);
    static void remove_channel(RedChannelClient *rcc);

    MainChannelClient *get_main();

    /* called when the migration handshake results in seamless migration (dst side).
     * By default we assume semi-seamless */
    void set_migration_seamless();
    void semi_seamless_migrate_complete(); /* dst side */
    gboolean seamless_migration_done_for_channel();
    /* TRUE if the migration is seamless and there are still channels that wait from migration data.
     * Or, during semi-seamless migration, and the main channel still waits for MIGRATE_END
     * from the client.
     * Note: Call it only from the main thread */
    int during_migrate_at_target();

    void migrate();

    gboolean is_disconnecting();
    void set_disconnecting();
    RedsState* get_server();

private:
    RedChannelClient *get_channel(int type, int id);

    RedsState *const reds;
    red::safe_list<red::shared_ptr<RedChannelClient>> channels;
    red::shared_ptr<MainChannelClient> mcc;
    pthread_mutex_t lock; // different channels can be in different threads

    pthread_t thread_id;

    int disconnecting;
    /* Note that while semi-seamless migration is conducted by the main thread, seamless migration
     * involves all channels, and thus the related variables can be accessed from different
     * threads */
    /* if seamless=TRUE, migration_target is turned off when all
     * the clients received their migration data. Otherwise (semi-seamless),
     * it is turned off, when red_client_semi_seamless_migrate_complete
     * is called */
    int during_target_migrate;
    int seamless_migrate;
    int num_migrated_channels; /* for seamless - number of channels that wait for migrate data*/

    gint _ref = 1;
};

#include "pop-visibility.h"

#endif /* RED_CLIENT_H_ */
