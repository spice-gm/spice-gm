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

struct MainChannel final: public RedChannel
{
    MainChannel(RedsState *reds);

    void on_connect(RedClient *client, RedStream *stream, int migration,
                    RedChannelCapabilities *caps) override {};

    // TODO: add refs and release (afrer all clients completed migration in one way or the other?)
    RedsMigSpice mig_target;
    int num_clients_mig_wait;
};

MainChannel *main_channel_new(RedsState *reds);
RedClient *main_channel_get_client_by_link_id(MainChannel *main_chan, uint32_t link_id);
/* This is a 'clone' from the reds.h Channel.link callback to allow passing link_id */
MainChannelClient *main_channel_link(MainChannel *, RedClient *client,
     RedStream *stream, uint32_t link_id, int migration,
     RedChannelCapabilities *caps);
void main_channel_push_mouse_mode(MainChannel *main_chan, SpiceMouseMode current_mode,
                                  int is_client_mouse_allowed);
void main_channel_push_agent_connected(MainChannel *main_chan);
void main_channel_push_agent_disconnected(MainChannel *main_chan);
void main_channel_push_multi_media_time(MainChannel *main_chan, uint32_t time);
/* tell MainChannel we have a new channel ready */
void main_channel_registered_new_channel(MainChannel *main_chan,
                                         RedChannel *channel);

int main_channel_is_connected(MainChannel *main_chan);

/* switch host migration */
void main_channel_migrate_switch(MainChannel *main_chan, RedsMigSpice *mig_target);

/* semi seamless migration */

/* returns the number of clients that we are waiting for their connection.
 * try_seamless = 'true' when the seamless-migration=on in qemu command line */
int main_channel_migrate_connect(MainChannel *main_channel, RedsMigSpice *mig_target,
                                 int try_seamless);
void main_channel_migrate_cancel_wait(MainChannel *main_chan);
const RedsMigSpice* main_channel_get_migration_target(MainChannel *main_chan);
/* returns the number of clients for which SPICE_MSG_MAIN_MIGRATE_END was sent*/
int main_channel_migrate_src_complete(MainChannel *main_chan, int success);
void main_channel_on_migrate_connected(MainChannel *main_channel,
                                       gboolean success, gboolean seamless);

#include "pop-visibility.h"

#endif /* MAIN_CHANNEL_H_ */
