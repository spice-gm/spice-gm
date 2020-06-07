/*
   Copyright (C) 2009 Red Hat, Inc.

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

#ifndef MAIN_CHANNEL_H_
#define MAIN_CHANNEL_H_

#include <stdint.h>
#include <spice/vd_agent.h>
#include <common/marshaller.h>

#include "red-channel.h"

#include "push-visibility.h"

// TODO: Defines used to calculate receive buffer size, and also by reds.c
// other options: is to make a reds_main_consts.h, to duplicate defines.
#define REDS_AGENT_WINDOW_SIZE 10
#define REDS_NUM_INTERNAL_AGENT_MESSAGES 1

struct RedsMigSpice {
    char *host;
    char *cert_subject;
    int port;
    int sport;
};

struct MainChannel;

red::shared_ptr<MainChannel> main_channel_new(RedsState *reds);

/* This is a 'clone' from the reds.h Channel.link callback to allow passing link_id */
MainChannelClient *main_channel_link(MainChannel *, RedClient *client,
     RedStream *stream, uint32_t link_id, int migration,
     RedChannelCapabilities *caps);

struct MainChannel final: public RedChannel
{
    RedClient *get_client_by_link_id(uint32_t link_id);
    void push_mouse_mode(SpiceMouseMode current_mode, int is_client_mouse_allowed);
    void push_agent_connected();
    void push_agent_disconnected();
    void push_multi_media_time(uint32_t time);
    /* tell MainChannel we have a new channel ready */
    void registered_new_channel(RedChannel *channel);

    /* switch host migration */
    void migrate_switch(RedsMigSpice *mig_target);

    /* semi seamless migration */

    /* returns the number of clients that we are waiting for their connection.
     * try_seamless = 'true' when the seamless-migration=on in qemu command line */
    int migrate_connect(RedsMigSpice *mig_target, int try_seamless);
    void migrate_cancel_wait();
    const RedsMigSpice* get_migration_target();
    /* returns the number of clients for which SPICE_MSG_MAIN_MIGRATE_END was sent*/
    int migrate_src_complete(int success);
    void on_migrate_connected(gboolean success, gboolean seamless);

    MainChannel(RedsState *reds);

    void on_connect(RedClient *client, RedStream *stream, int migration,
                    RedChannelCapabilities *caps) override {};

    // TODO: add refs and release (after all clients completed migration in one way or the other?)
    RedsMigSpice mig_target;
    int num_clients_mig_wait;
};

#include "pop-visibility.h"

#endif /* MAIN_CHANNEL_H_ */
