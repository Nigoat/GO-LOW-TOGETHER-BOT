
#ifndef CLANS_H
#define CLANS_H

#include <concord/discord.h>

void on_create_clan_command(struct discord *client, const struct discord_interaction *event);
void on_invite_clan_command(struct discord *client, const struct discord_interaction *event);
void on_clan_invite_button(struct discord *client, const struct discord_interaction *event, const char *custom_id);

#endif

