
#ifndef VERIFY_H
#define VERIFY_H

#include <concord/discord.h>

void on_verify_button(struct discord *client, const struct discord_interaction *event);
void on_send_verify_command(struct discord *client, const struct discord_interaction *event);

#endif

