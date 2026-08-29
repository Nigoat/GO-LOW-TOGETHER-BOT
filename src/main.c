
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <inttypes.h>
#include <pthread.h>
#include <unistd.h>
#include <concord/discord.h>
#include <concord/log.h>
#include "config.h"
#include "db.h"
#include "verify.h"
#include "tickets.h"
#include "clans.h"
#include "voice.h"

static void *role_poller_thread(void *arg)
{
    struct discord *client = (struct discord *)arg;
    int cleanup_counter = 0;

    while (1) {
        sleep(5);

        pending_role_update_t *updates = NULL;
        int count = 0;
        if (db_get_pending_role_updates(&updates, &count) == 0 && count > 0) {
            for (int i = 0; i < count; i++) {
                u64snowflake user_id = (u64snowflake)strtoull(updates[i].discord_user_id, NULL, 10);
                u64snowflake guild_id = (u64snowflake)strtoull(updates[i].guild_id, NULL, 10);
                u64snowflake role_id = (u64snowflake)strtoull(updates[i].role_id, NULL, 10);

                if (strcmp(updates[i].action, "add") == 0) {
                    discord_add_guild_member_role(client, guild_id, user_id, role_id, NULL, NULL);
                } else if (strcmp(updates[i].action, "remove") == 0) {
                    discord_remove_guild_member_role(client, guild_id, user_id, role_id, NULL, NULL);
                }

                db_mark_role_update_processed(updates[i].id);
            }
            free(updates);
        }

        cleanup_counter++;
        if (cleanup_counter >= 120) {
            cleanup_counter = 0;
            db_cleanup_old_tokens();
            db_cleanup_old_pending_updates();
        }
    }

    return NULL;
}

static void on_ready(struct discord *client, const struct discord_ready *event)
{
    log_info("Logged in as %s", event->user->username);

    db_init(g_config.database_url);
    voice_init();

    u64snowflake app_id = event->application->id;
    u64snowflake guild_id = g_config.guild_id;

    struct discord_create_guild_application_command verify_cmd = {
        .name = "send-verify",
        .description = "Manually send or resend the verification embed message.",
        .type = 1
    };
    discord_create_guild_application_command(client, app_id, guild_id, &verify_cmd, NULL);

    struct discord_create_guild_application_command ticket_cmd = {
        .name = "send-ticket",
        .description = "Manually send or resend the ticket embed message.",
        .type = 1
    };
    discord_create_guild_application_command(client, app_id, guild_id, &ticket_cmd, NULL);

    struct discord_application_command_option clan_create_opts[] = {
        {
            .type = DISCORD_APPLICATION_OPTION_STRING,
            .name = "name",
            .description = "The name of your new clan",
            .required = true
        },
        {
            .type = DISCORD_APPLICATION_OPTION_STRING,
            .name = "description",
            .description = "The description and bio of your clan",
            .required = true
        },
        {
            .type = DISCORD_APPLICATION_OPTION_ATTACHMENT,
            .name = "picture",
            .description = "Optional clan logo/picture (must be under 4MB)",
            .required = false
        }
    };
    struct discord_create_guild_application_command create_clan_cmd = {
        .name = "create-clan",
        .description = "Create a new clan, role, and custom nickname tag.",
        .type = 1,
        .options = &(struct discord_application_command_options){
            .size = sizeof(clan_create_opts) / sizeof(clan_create_opts[0]),
            .array = clan_create_opts
        }
    };
    discord_create_guild_application_command(client, app_id, guild_id, &create_clan_cmd, NULL);

    struct discord_application_command_option clan_invite_opts[] = {
        {
            .type = DISCORD_APPLICATION_OPTION_USER,
            .name = "user",
            .description = "The server member you want to invite to your clan",
            .required = true
        }
    };
    struct discord_create_guild_application_command invite_clan_cmd = {
        .name = "invite-clan",
        .description = "Send a DM invitation to a member to join your clan.",
        .type = 1,
        .options = &(struct discord_application_command_options){
            .size = sizeof(clan_invite_opts) / sizeof(clan_invite_opts[0]),
            .array = clan_invite_opts
        }
    };
    discord_create_guild_application_command(client, app_id, guild_id, &invite_clan_cmd, NULL);

    struct discord_create_guild_application_command voice_priv_cmd = {
        .name = "voice-private",
        .description = "Make your active temporary voice channel private (invite only).",
        .type = 1
    };
    discord_create_guild_application_command(client, app_id, guild_id, &voice_priv_cmd, NULL);

    struct discord_create_guild_application_command voice_pub_cmd = {
        .name = "voice-public",
        .description = "Make your active temporary voice channel public to everyone.",
        .type = 1
    };
    discord_create_guild_application_command(client, app_id, guild_id, &voice_pub_cmd, NULL);

    struct discord_application_command_option voice_user_opts[] = {
        {
            .type = DISCORD_APPLICATION_OPTION_USER,
            .name = "user",
            .description = "Target member",
            .required = true
        }
    };

    struct discord_create_guild_application_command voice_permit_cmd = {
        .name = "voice-permit",
        .description = "Allow a specific member to join your private voice channel.",
        .type = 1,
        .options = &(struct discord_application_command_options){
            .size = sizeof(voice_user_opts) / sizeof(voice_user_opts[0]),
            .array = voice_user_opts
        }
    };
    discord_create_guild_application_command(client, app_id, guild_id, &voice_permit_cmd, NULL);

    struct discord_create_guild_application_command voice_kick_cmd = {
        .name = "voice-kick",
        .description = "Disconnect/kick a member from your temporary voice channel.",
        .type = 1,
        .options = &(struct discord_application_command_options){
            .size = sizeof(voice_user_opts) / sizeof(voice_user_opts[0]),
            .array = voice_user_opts
        }
    };
    discord_create_guild_application_command(client, app_id, guild_id, &voice_kick_cmd, NULL);

    pthread_t poller_tid;
    pthread_create(&poller_tid, NULL, role_poller_thread, client);
    pthread_detach(poller_tid);
}

static void on_interaction(struct discord *client, const struct discord_interaction *event)
{
    if (event->type == DISCORD_INTERACTION_APPLICATION_COMMAND && event->data) {
        if (strcmp(event->data->name, "send-verify") == 0) {
            on_send_verify_command(client, event);
        } else if (strcmp(event->data->name, "send-ticket") == 0) {
            on_send_ticket_command(client, event);
        } else if (strcmp(event->data->name, "create-clan") == 0) {
            on_create_clan_command(client, event);
        } else if (strcmp(event->data->name, "invite-clan") == 0) {
            on_invite_clan_command(client, event);
        } else if (strcmp(event->data->name, "voice-private") == 0) {
            on_voice_private_command(client, event);
        } else if (strcmp(event->data->name, "voice-public") == 0) {
            on_voice_public_command(client, event);
        } else if (strcmp(event->data->name, "voice-permit") == 0) {
            on_voice_permit_command(client, event);
        } else if (strcmp(event->data->name, "voice-kick") == 0) {
            on_voice_kick_command(client, event);
        }
    } else if (event->type == DISCORD_INTERACTION_MESSAGE_COMPONENT) {
        if (event->data && event->data->custom_id) {
            const char *cid = event->data->custom_id;
            if (strcmp(cid, "bmb_verify") == 0) {
                on_verify_button(client, event);
            } else if (strcmp(cid, "bmb_open_ticket") == 0) {
                on_open_ticket_button(client, event);
            } else if (strcmp(cid, "bmb_close_ticket") == 0) {
                on_close_ticket_button(client, event);
            } else if (strncmp(cid, "clan_inv_acc:", 13) == 0 || strncmp(cid, "clan_inv_dec:", 13) == 0) {
                on_clan_invite_button(client, event, cid);
            }
        }
    }
}

int main(void)
{
    config_load();

    struct discord *client = discord_init(g_config.bot_token);
    discord_add_intents(client, DISCORD_GATEWAY_GUILDS | DISCORD_GATEWAY_GUILD_MESSAGES | DISCORD_GATEWAY_GUILD_VOICE_STATES);

    discord_set_on_ready(client, &on_ready);
    discord_set_on_interaction_create(client, &on_interaction);
    discord_set_on_voice_state_update(client, &on_voice_state_update);

    discord_run(client);

    db_cleanup();
    discord_cleanup(client);
    return 0;
}

