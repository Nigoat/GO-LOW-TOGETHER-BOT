
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <inttypes.h>
#include <concord/discord.h>
#include <concord/log.h>
#include "config.h"
#include "db.h"

void on_verify_button(struct discord *client, const struct discord_interaction *event)
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
    u64snowflake guild_id = event->guild_id;

    char *token = db_create_verify_token(user_id, guild_id);
    if (!token) {
        struct discord_interaction_response params = {
            .type = DISCORD_INTERACTION_CHANNEL_MESSAGE_WITH_SOURCE,
            .data = &(struct discord_interaction_callback_data){
                .content = "❌ Failed to generate verification link. Please try again later.",
                .flags = DISCORD_MESSAGE_EPHEMERAL
            }
        };
        discord_create_interaction_response(client, event->id, event->token, &params, NULL);
        return;
    }

    size_t base_len = strlen(g_config.website_base_url);
    size_t token_len = strlen(token);
    if (base_len + 15 + token_len >= 2048) {
        free(token);
        struct discord_interaction_response params = {
            .type = DISCORD_INTERACTION_CHANNEL_MESSAGE_WITH_SOURCE,
            .data = &(struct discord_interaction_callback_data){
                .content = "❌ Verification URL configuration error. Please contact staff.",
                .flags = DISCORD_MESSAGE_EPHEMERAL
            }
        };
        discord_create_interaction_response(client, event->id, event->token, &params, NULL);
        return;
    }

    char url[2048];
    snprintf(url, sizeof(url), "%s/verify?token=%s", g_config.website_base_url, token);

    char desc[2500];
    snprintf(desc, sizeof(desc),
        "Click the link below to complete your verification and receive your roles:\n\n"
        "👉 **[Click Here to Complete Verification](%s)**\n\n"
        "*(This link is uniquely generated for your account and expires in **15 minutes**)*",
        url);

    struct discord_embed embed = {
        .title = "🔐 Your Personal Verification Link",
        .description = desc,
        .color = 0x2ECC71,
        .footer = &(struct discord_embed_footer){
            .text = "GO LOW TOGETHER • Secure Verification"
        }
    };

    struct discord_interaction_response params = {
        .type = DISCORD_INTERACTION_CHANNEL_MESSAGE_WITH_SOURCE,
        .data = &(struct discord_interaction_callback_data){
            .embeds = &(struct discord_embeds){
                .size = 1,
                .array = &embed
            },
            .flags = DISCORD_MESSAGE_EPHEMERAL
        }
    };
    discord_create_interaction_response(client, event->id, event->token, &params, NULL);

    free(token);
}

static int has_owner_role(const struct discord_guild_member *member)
{
    if (!member || !member->roles) return 0;
    for (int i = 0; i < member->roles->size; i++) {
        if (member->roles->array[i] == g_config.owner_role_id)
            return 1;
    }
    return 0;
}

void on_send_verify_command(struct discord *client, const struct discord_interaction *event)
{
    if (!has_owner_role(event->member)) {
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
            .name = "⚡ Unlocked Roles & Perks",
            .value = "• Access to all programming channels (C, C++, Rust, Zig, Assembly)\n"
                     "• Skill level tags (Beginner, Intermediate, Advanced, Wizard)\n"
                     "• Access to voice rooms and live coding channels\n"
                     "• Clan creation and invitations",
            .Inline = false
        },
        {
            .name = "🔒 Quick & Secure",
            .value = "Clicking the button below generates a private, temporary link valid for 15 minutes.",
            .Inline = false
        }
    };

    struct discord_embed embed = {
        .title = "🛡️ GO LOW TOGETHER — Server Verification",
        .description = "Welcome to **GO LOW TOGETHER**!\n\n"
                       "To gain full access to the server, participate in voice channels, create clans, and select your programming skill roles, please click the button below to verify your account.",
        .color = 0x5865F2,
        .fields = &(struct discord_embed_fields){
            .size = sizeof(fields) / sizeof(fields[0]),
            .array = fields
        },
        .footer = &(struct discord_embed_footer){
            .text = "GO LOW TOGETHER • Automated Security Desk"
        }
    };

    struct discord_component button = {
        .type = DISCORD_COMPONENT_BUTTON,
        .style = DISCORD_BUTTON_SUCCESS,
        .label = "Verify Yourself",
        .custom_id = "bmb_verify"
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

    discord_create_message(client, g_config.verify_channel_id, &msg_params, NULL);

    discord_create_interaction_response(client, event->id, event->token,
        &(struct discord_interaction_response){
            .type = DISCORD_INTERACTION_CHANNEL_MESSAGE_WITH_SOURCE,
            .data = &(struct discord_interaction_callback_data){
                .content = "✅ Beautiful verification message sent successfully!",
                .flags = DISCORD_MESSAGE_EPHEMERAL
            }
        }, NULL);
}

