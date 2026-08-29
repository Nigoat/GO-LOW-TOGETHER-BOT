
#ifndef TICKETS_H
#define TICKETS_H

#include <concord/discord.h>

void on_open_ticket_button(struct discord *client, const struct discord_interaction *event);
void on_close_ticket_button(struct discord *client, const struct discord_interaction *event);
void on_send_ticket_command(struct discord *client, const struct discord_interaction *event);

#endif

