/*
   Copyright (C) 2011 Red Hat, Inc.

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

   Red Hat Authors:
        hdegoede@redhat.com
*/

#ifndef AGENT_MSG_FILTER_H_
#define AGENT_MSG_FILTER_H_

#include <inttypes.h>
#include <glib.h>

/* Possible return values for agent_msg_filter_process_data */
typedef enum {
    AGENT_MSG_FILTER_OK,
    AGENT_MSG_FILTER_DISCARD,
    AGENT_MSG_FILTER_PROTO_ERROR,
    AGENT_MSG_FILTER_MONITORS_CONFIG,
} AgentMsgFilterResult;

typedef struct AgentMsgFilter {
    // bytes of message we still need to read
    int msg_data_to_read;
    // status of current message, we need to store in case the same message is split into multiple
    // chunks
    AgentMsgFilterResult result;
    gboolean copy_paste_enabled;
    gboolean file_xfer_enabled;
    // device should pass monitor information to reds instead of passing to agent,
    // used for messages from the guest to the agent
    gboolean use_client_monitors_config;
    // discard all messages, used for example when device is disabled to discard
    // pending data
    gboolean discard_all;
} AgentMsgFilter;

void agent_msg_filter_init(AgentMsgFilter *filter,
                           gboolean copy_paste, gboolean file_xfer,
                           gboolean use_client_monitors_config,
                           gboolean discard_all);
void agent_msg_filter_config(AgentMsgFilter *filter,
                             gboolean copy_paste, gboolean file_xfer,
                             gboolean use_client_monitors_config);
AgentMsgFilterResult agent_msg_filter_process_data(AgentMsgFilter *filter,
                                                   const uint8_t *data, uint32_t len);

#endif /* AGENT_MSG_FILTER_H_ */
