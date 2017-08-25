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

#include <glib-object.h>

#include "main-channel-client.h"

G_BEGIN_DECLS

SPICE_DECLARE_TYPE(RedClient, red_client, CLIENT);
#define RED_TYPE_CLIENT red_client_get_type()

RedClient *red_client_new(RedsState *reds, int migrated);

/*
 * disconnects all the client's channels (should be called from the client's thread)
 */
void red_client_destroy(RedClient *client);

gboolean red_client_add_channel(RedClient *client, RedChannelClient *rcc, GError **error);
void red_client_remove_channel(RedChannelClient *rcc);

MainChannelClient *red_client_get_main(RedClient *client);

/* called when the migration handshake results in seamless migration (dst side).
 * By default we assume semi-seamless */
void red_client_set_migration_seamless(RedClient *client);
void red_client_semi_seamless_migrate_complete(RedClient *client); /* dst side */
gboolean red_client_seamless_migration_done_for_channel(RedClient *client);
/* TRUE if the migration is seamless and there are still channels that wait from migration data.
 * Or, during semi-seamless migration, and the main channel still waits for MIGRATE_END
 * from the client.
 * Note: Call it only from the main thread */
int red_client_during_migrate_at_target(RedClient *client);

void red_client_migrate(RedClient *client);

gboolean red_client_is_disconnecting(RedClient *client);
void red_client_set_disconnecting(RedClient *client);
RedsState* red_client_get_server(RedClient *client);

G_END_DECLS

#endif /* RED_CLIENT_H_ */
