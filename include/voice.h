
#ifndef VOICE_H
#define VOICE_H

#include <concord/discord.h>

void voice_init(void);
void on_voice_state_update(struct discord *client, const struct discord_voice_state *event);

void on_voice_private_command(struct discord *client, const struct discord_interaction *event);
void on_voice_public_command(struct discord *client, const struct discord_interaction *event);
void on_voice_permit_command(struct discord *client, const struct discord_interaction *event);
void on_voice_kick_command(struct discord *client, const struct discord_interaction *event);

#endif

