
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <inttypes.h>
#include <concord/discord.h>
#include <concord/log.h>
#include "config.h"

static int has_staff_role(const struct discord_guild_member *member)
{
    if (!member || !member->roles) return 0;
    for (int i = 0; i < member->roles->size; i++) {
        u64snowflake role = member->roles->array[i];
        if (role == g_config.owner_role_id || role == g_config.moderator_role_id)
            return 1;
    }
    return 0;
}

static void done_ticket_created(struct discord *client,
    struct discord_response *resp,
    const struct discord_channel *channel)
{
    (void)resp;
    if (!channel) {
        log_error("Failed to create ticket thread");
        return;
    }

    u64snowflake user_id = 0;
    if (channel->name && strncmp(channel->name, "ticket-", 7) == 0) {
        user_id = (u64snowflake)strtoull(channel->name + 7, NULL, 10);
    }

    log_info("Created ticket thread %" PRIu64 " for user %" PRIu64,
        channel->id, user_id);

    if (user_id != 0) {
        discord_add_thread_member(client, channel->id, user_id, NULL, NULL);
    }

    struct discord_embed_field fields[] = {
        {
            .name = "🔒 Privacy Notice",
            .value = "This thread is private and only visible to you and the server staff.",
            .Inline = false
        }
    };

    struct discord_embed embed = {
        .title = "🎫 Support Ticket Opened",
        .description = "Welcome to your support ticket! Please state your inquiry or issue with as much detail as possible. A staff member will be with you shortly.",
        .color = 0x3498DB,
        .fields = &(struct discord_embed_fields){
            .size = sizeof(fields) / sizeof(fields[0]),
            .array = fields
        },
        .footer = &(struct discord_embed_footer){
            .text = "Bare Metal Builders • Support Desk"
        }
    };

    struct discord_component close_button = {
        .type = DISCORD_COMPONENT_BUTTON,
        .style = DISCORD_BUTTON_DANGER,
        .label = "🔒 Close Ticket",
        .custom_id = "bmb_close_ticket"
    };

    struct discord_component action_row = {
        .type = DISCORD_COMPONENT_ACTION_ROW,
        .components = &(struct discord_components){
            .size = 1,
            .array = &close_button
        }
    };

    struct discord_create_message msg_params = {
        .embeds = &(struct discord_embeds){
            .size = 1,
            .array = &embed
        },
        .components = &(struct discord_components){
            .size = 1,
            .array = &action_row
        }
    };
    discord_create_message(client, channel->id, &msg_params, NULL);
}

void on_open_ticket_button(struct discord *client, const struct discord_interaction *event)
{
    if (!event->member || !event->member->user) {
        struct discord_interaction_response params = {
            .type = DISCORD_INTERACTION_CHANNEL_MESSAGE_WITH_SOURCE,
            .data = &(struct discord_interaction_callback_data){
                .content = "❌ Could not identify you. Please try again.",
                .flags = DISCORD_MESSAGE_EPHEMERAL
            }
        };
        discord_create_interaction_response(client, event->id, event->token, &params, NULL);
        return;
    }

    u64snowflake user_id = event->member->user->id;

    char thread_name[128];
    snprintf(thread_name, sizeof(thread_name), "ticket-%" PRIu64, user_id);

    struct discord_start_thread_without_message params = {
        .name = thread_name,
        .auto_archive_duration = 1440,
        .type = DISCORD_CHANNEL_GUILD_PRIVATE_THREAD
    };

    struct discord_ret_channel ret = {
        .done = &done_ticket_created,
    };
    discord_start_thread_without_message(client, g_config.ticket_channel_id, &params, &ret);

    discord_create_interaction_response(client, event->id, event->token,
        &(struct discord_interaction_response){
            .type = DISCORD_INTERACTION_CHANNEL_MESSAGE_WITH_SOURCE,
            .data = &(struct discord_interaction_callback_data){
                .content = "📩 Your ticket is being created! Please check the newly created thread in a few moments.",
                .flags = DISCORD_MESSAGE_EPHEMERAL
            }
        }, NULL);
}

void on_close_ticket_button(struct discord *client, const struct discord_interaction *event)
{
    if (!has_staff_role(event->member)) {
        struct discord_interaction_response params = {
            .type = DISCORD_INTERACTION_CHANNEL_MESSAGE_WITH_SOURCE,
            .data = &(struct discord_interaction_callback_data){
                .content = "❌ Only staff members can close tickets.",
                .flags = DISCORD_MESSAGE_EPHEMERAL
            }
        };
        discord_create_interaction_response(client, event->id, event->token, &params, NULL);
        return;
    }

    discord_create_interaction_response(client, event->id, event->token,
        &(struct discord_interaction_response){
            .type = DISCORD_INTERACTION_CHANNEL_MESSAGE_WITH_SOURCE,
            .data = &(struct discord_interaction_callback_data){
                .content = "🔒 Ticket closed. Thank you for contacting Bare Metal Builders support.",
                .flags = DISCORD_MESSAGE_EPHEMERAL
            }
        }, NULL);
}

void on_send_ticket_command(struct discord *client, const struct discord_interaction *event)
{
    if (!has_staff_role(event->member)) {
        struct discord_interaction_response params = {
            .type = DISCORD_INTERACTION_CHANNEL_MESSAGE_WITH_SOURCE,
            .data = &(struct discord_interaction_callback_data){
                .content = "❌ You do not have permission to use this command.",
                .flags = DISCORD_MESSAGE_EPHEMERAL
            }
        };
        discord_create_interaction_response(client, event->id, event->token, &params, NULL);
        return;
    }

    struct discord_embed_field fields[] = {
        {
            .name = "📌 Support Guidelines",
            .value = "• State your question, bug report, or inquiry clearly.\n"
                     "• Please be patient while staff reviews your ticket.\n"
                     "• Keep all communication civil and constructive.",
            .Inline = false
        }
    };

    struct discord_embed embed = {
        .title = "🎫 Bare Metal Builders — Support & Inquiry Desk",
        .description = "Need help from our team, want to report an issue, or discuss partnerships and projects?\n\n"
                       "Click the button below to open a private support ticket.",
        .color = 0xF1C40F,
        .fields = &(struct discord_embed_fields){
            .size = sizeof(fields) / sizeof(fields[0]),
            .array = fields
        },
        .footer = &(struct discord_embed_footer){
            .text = "Bare Metal Builders • Support Center"
        }
    };

    struct discord_component button = {
        .type = DISCORD_COMPONENT_BUTTON,
        .style = DISCORD_BUTTON_PRIMARY,
        .label = "📩 Open a Ticket",
        .custom_id = "bmb_open_ticket"
    };

    struct discord_component action_row = {
        .type = DISCORD_COMPONENT_ACTION_ROW,
        .components = &(struct discord_components){
            .size = 1,
            .array = &button
        }
    };

    struct discord_create_message msg_params = {
        .embeds = &(struct discord_embeds){
            .size = 1,
            .array = &embed
        },
        .components = &(struct discord_components){
            .size = 1,
            .array = &action_row
        }
    };

    discord_create_message(client, g_config.ticket_channel_id, &msg_params, NULL);

    discord_create_interaction_response(client, event->id, event->token,
        &(struct discord_interaction_response){
            .type = DISCORD_INTERACTION_CHANNEL_MESSAGE_WITH_SOURCE,
            .data = &(struct discord_interaction_callback_data){
                .content = "✅ Beautiful ticket message sent successfully!",
                .flags = DISCORD_MESSAGE_EPHEMERAL
            }
        }, NULL);
}

