
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <inttypes.h>
#include <concord/discord.h>
#include <concord/log.h>
#include "config.h"
#include "db.h"
#include "clans.h"

static void format_clan_nickname(const char *clan_name, const char *username, char *out_nick, size_t out_size)
{

    char tag[32];
    snprintf(tag, sizeof(tag), "[%s] ", clan_name);
    size_t tag_len = strlen(tag);

    if (tag_len >= out_size - 1) {
        snprintf(out_nick, out_size, "[%.*s]", (int)(out_size - 3), clan_name);
        return;
    }

    size_t max_name_len = out_size - 1 - tag_len;
    snprintf(out_nick, out_size, "%s%.*s", tag, (int)max_name_len, username ? username : "Member");
}

static void send_user_dm(struct discord *client, u64snowflake user_id,
                         const char *content, const struct discord_embed *embed)
{
    struct discord_create_dm params = {
        .recipient_id = user_id
    };

    struct discord_channel dm_ch = { 0 };
    struct discord_ret_channel ret = {
        .sync = &dm_ch
    };

    if (discord_create_dm(client, &params, &ret) == CCORD_OK && dm_ch.id != 0) {
        struct discord_create_message msg = { 0 };
        if (content) msg.content = (char *)content;
        if (embed) {
            msg.embeds = &(struct discord_embeds){
                .size = 1,
                .array = (struct discord_embed *)embed
            };
        }
        discord_create_message(client, dm_ch.id, &msg, NULL);
    }
}

void on_create_clan_command(struct discord *client, const struct discord_interaction *event)
{
    if (!event->member || !event->member->user) {
        struct discord_interaction_response resp = {
            .type = DISCORD_INTERACTION_CHANNEL_MESSAGE_WITH_SOURCE,
            .data = &(struct discord_interaction_callback_data){
                .content = "❌ Could not identify your user profile.",
                .flags = DISCORD_MESSAGE_EPHEMERAL
            }
        };
        discord_create_interaction_response(client, event->id, event->token, &resp, NULL);
        return;
    }

    u64snowflake user_id = event->member->user->id;
    u64snowflake guild_id = event->guild_id ? event->guild_id : g_config.guild_id;

    clan_t existing_clan;
    if (db_clan_get_by_user(user_id, &existing_clan) == 0) {
        char err_msg[256];
        snprintf(err_msg, sizeof(err_msg),
                 "❌ You are already in the clan **[%s]**! You must leave your current clan before creating a new one.",
                 existing_clan.name);
        struct discord_interaction_response resp = {
            .type = DISCORD_INTERACTION_CHANNEL_MESSAGE_WITH_SOURCE,
            .data = &(struct discord_interaction_callback_data){
                .content = err_msg,
                .flags = DISCORD_MESSAGE_EPHEMERAL
            }
        };
        discord_create_interaction_response(client, event->id, event->token, &resp, NULL);
        return;
    }

    const char *clan_name = NULL;
    const char *clan_desc = NULL;
    const char *picture_url = NULL;
    u64snowflake picture_attachment_id = 0;

    if (event->data && event->data->options) {
        for (int i = 0; i < event->data->options->size; i++) {
            struct discord_application_command_interaction_data_option *opt =
                &event->data->options->array[i];
            if (strcmp(opt->name, "name") == 0 && opt->value) {
                clan_name = opt->value;
            } else if (strcmp(opt->name, "description") == 0 && opt->value) {
                clan_desc = opt->value;
            } else if (strcmp(opt->name, "picture") == 0 && opt->value) {
                picture_attachment_id = (u64snowflake)strtoull(opt->value, NULL, 10);
            }
        }
    }

    if (!clan_name || !*clan_name || !clan_desc || !*clan_desc) {
        struct discord_interaction_response resp = {
            .type = DISCORD_INTERACTION_CHANNEL_MESSAGE_WITH_SOURCE,
            .data = &(struct discord_interaction_callback_data){
                .content = "❌ Clan name and description are required.",
                .flags = DISCORD_MESSAGE_EPHEMERAL
            }
        };
        discord_create_interaction_response(client, event->id, event->token, &resp, NULL);
        return;
    }

    if (picture_attachment_id != 0 && event->data->resolved && event->data->resolved->attachments) {
        for (int i = 0; i < event->data->resolved->attachments->size; i++) {
            struct discord_attachment *att = &event->data->resolved->attachments->array[i];
            if (att->id == picture_attachment_id) {
                if (att->size > 4 * 1024 * 1024) {
                    struct discord_interaction_response resp = {
                        .type = DISCORD_INTERACTION_CHANNEL_MESSAGE_WITH_SOURCE,
                        .data = &(struct discord_interaction_callback_data){
                            .content = "❌ Clan picture must be smaller than 4MB!",
                            .flags = DISCORD_MESSAGE_EPHEMERAL
                        }
                    };
                    discord_create_interaction_response(client, event->id, event->token, &resp, NULL);
                    return;
                }
                picture_url = att->url;
                break;
            }
        }
    }

    clan_t name_check;
    if (db_clan_get_by_name(clan_name, &name_check) == 0) {
        struct discord_interaction_response resp = {
            .type = DISCORD_INTERACTION_CHANNEL_MESSAGE_WITH_SOURCE,
            .data = &(struct discord_interaction_callback_data){
                .content = "❌ A clan with that name already exists! Please choose a different name.",
                .flags = DISCORD_MESSAGE_EPHEMERAL
            }
        };
        discord_create_interaction_response(client, event->id, event->token, &resp, NULL);
        return;
    }

    char role_name[64];
    snprintf(role_name, sizeof(role_name), "[%s]", clan_name);

    struct discord_create_guild_role role_params = {
        .name = role_name,
        .mentionable = true
    };

    struct discord_role created_role = { 0 };
    struct discord_ret_role role_ret = {
        .sync = &created_role
    };

    if (discord_create_guild_role(client, guild_id, &role_params, &role_ret) != CCORD_OK || created_role.id == 0) {
        struct discord_interaction_response resp = {
            .type = DISCORD_INTERACTION_CHANNEL_MESSAGE_WITH_SOURCE,
            .data = &(struct discord_interaction_callback_data){
                .content = "❌ Failed to create Discord role for your clan. Please check bot permissions.",
                .flags = DISCORD_MESSAGE_EPHEMERAL
            }
        };
        discord_create_interaction_response(client, event->id, event->token, &resp, NULL);
        return;
    }

    int clan_id = 0;
    if (db_clan_create(guild_id, clan_name, clan_desc, picture_url, user_id, created_role.id, &clan_id) != 0) {
        discord_delete_guild_role(client, guild_id, created_role.id, NULL);
        struct discord_interaction_response resp = {
            .type = DISCORD_INTERACTION_CHANNEL_MESSAGE_WITH_SOURCE,
            .data = &(struct discord_interaction_callback_data){
                .content = "❌ Failed to register clan in the database. Please try again.",
                .flags = DISCORD_MESSAGE_EPHEMERAL
            }
        };
        discord_create_interaction_response(client, event->id, event->token, &resp, NULL);
        return;
    }

    discord_add_guild_member_role(client, guild_id, user_id, created_role.id, NULL, NULL);

    const char *username = event->member->user->username;
    if (event->member->nick && *event->member->nick) {
        username = event->member->nick;
    }
    char new_nick[33];
    format_clan_nickname(clan_name, username, new_nick, sizeof(new_nick));

    struct discord_modify_guild_member mod_member = {
        .nick = new_nick
    };
    discord_modify_guild_member(client, guild_id, user_id, &mod_member, NULL);

    char leader_mention[64];
    snprintf(leader_mention, sizeof(leader_mention), "<@%" PRIu64 ">", user_id);

    char role_mention[64];
    snprintf(role_mention, sizeof(role_mention), "<@&%" PRIu64 ">", created_role.id);

    char tag_str[64];
    snprintf(tag_str, sizeof(tag_str), "`[%s]`", clan_name);

    struct discord_embed_field fields[] = {
        { .name = "👑 Clan Leader", .value = leader_mention, ._inline = true },
        { .name = "🏷️ Clan Role", .value = role_mention, ._inline = true },
        { .name = "👥 Clan Tag", .value = tag_str, ._inline = true },
        { .name = "📜 Description", .value = (char *)clan_desc, ._inline = false }
    };

    struct discord_embed embed = {
        .title = "⚔️ Clan Successfully Created!",
        .description = "A new clan has been founded in Bare Metal Builders!",
        .color = 0xE67E22,
        .fields = &(struct discord_embed_fields){
            .size = sizeof(fields) / sizeof(fields[0]),
            .array = fields
        },
        .footer = &(struct discord_embed_footer){
            .text = "Bare Metal Builders • Clan System"
        }
    };

    if (picture_url && *picture_url) {
        embed.thumbnail = &(struct discord_embed_thumbnail){
            .url = (char *)picture_url
        };
    }

    struct discord_interaction_response resp = {
        .type = DISCORD_INTERACTION_CHANNEL_MESSAGE_WITH_SOURCE,
        .data = &(struct discord_interaction_callback_data){
            .embeds = &(struct discord_embeds){
                .size = 1,
                .array = &embed
            }
        }
    };
    discord_create_interaction_response(client, event->id, event->token, &resp, NULL);
}

void on_invite_clan_command(struct discord *client, const struct discord_interaction *event)
{
    if (!event->member || !event->member->user) {
        struct discord_interaction_response resp = {
            .type = DISCORD_INTERACTION_CHANNEL_MESSAGE_WITH_SOURCE,
            .data = &(struct discord_interaction_callback_data){
                .content = "❌ Could not identify your user profile.",
                .flags = DISCORD_MESSAGE_EPHEMERAL
            }
        };
        discord_create_interaction_response(client, event->id, event->token, &resp, NULL);
        return;
    }

    u64snowflake inviter_id = event->member->user->id;
    u64snowflake guild_id = event->guild_id ? event->guild_id : g_config.guild_id;

    clan_t clan;
    if (db_clan_get_by_user(inviter_id, &clan) != 0) {
        struct discord_interaction_response resp = {
            .type = DISCORD_INTERACTION_CHANNEL_MESSAGE_WITH_SOURCE,
            .data = &(struct discord_interaction_callback_data){
                .content = "❌ You must belong to a clan to invite others! Use `/create-clan` to start your own.",
                .flags = DISCORD_MESSAGE_EPHEMERAL
            }
        };
        discord_create_interaction_response(client, event->id, event->token, &resp, NULL);
        return;
    }

    u64snowflake target_id = 0;
    if (event->data && event->data->options) {
        for (int i = 0; i < event->data->options->size; i++) {
            struct discord_application_command_interaction_data_option *opt =
                &event->data->options->array[i];
            if (strcmp(opt->name, "user") == 0 && opt->value) {
                target_id = (u64snowflake)strtoull(opt->value, NULL, 10);
            }
        }
    }

    if (target_id == 0 || target_id == inviter_id) {
        struct discord_interaction_response resp = {
            .type = DISCORD_INTERACTION_CHANNEL_MESSAGE_WITH_SOURCE,
            .data = &(struct discord_interaction_callback_data){
                .content = "❌ Please specify a valid server member to invite (you cannot invite yourself).",
                .flags = DISCORD_MESSAGE_EPHEMERAL
            }
        };
        discord_create_interaction_response(client, event->id, event->token, &resp, NULL);
        return;
    }

    clan_t target_clan;
    if (db_clan_get_by_user(target_id, &target_clan) == 0) {
        char err_msg[256];
        snprintf(err_msg, sizeof(err_msg),
                 "❌ That user is already a member of the clan **[%s]**!",
                 target_clan.name);
        struct discord_interaction_response resp = {
            .type = DISCORD_INTERACTION_CHANNEL_MESSAGE_WITH_SOURCE,
            .data = &(struct discord_interaction_callback_data){
                .content = err_msg,
                .flags = DISCORD_MESSAGE_EPHEMERAL
            }
        };
        discord_create_interaction_response(client, event->id, event->token, &resp, NULL);
        return;
    }

    int invite_id = 0;
    if (db_clan_create_invite(clan.id, guild_id, inviter_id, target_id, &invite_id) != 0) {
        struct discord_interaction_response resp = {
            .type = DISCORD_INTERACTION_CHANNEL_MESSAGE_WITH_SOURCE,
            .data = &(struct discord_interaction_callback_data){
                .content = "❌ Failed to generate clan invitation in database.",
                .flags = DISCORD_MESSAGE_EPHEMERAL
            }
        };
        discord_create_interaction_response(client, event->id, event->token, &resp, NULL);
        return;
    }

    struct discord_create_dm dm_params = {
        .recipient_id = target_id
    };
    struct discord_channel dm_ch = { 0 };
    struct discord_ret_channel dm_ret = {
        .sync = &dm_ch
    };

    if (discord_create_dm(client, &dm_params, &dm_ret) != CCORD_OK || dm_ch.id == 0) {
        struct discord_interaction_response resp = {
            .type = DISCORD_INTERACTION_CHANNEL_MESSAGE_WITH_SOURCE,
            .data = &(struct discord_interaction_callback_data){
                .content = "❌ Could not open Direct Messages with that user. They might have DMs disabled.",
                .flags = DISCORD_MESSAGE_EPHEMERAL
            }
        };
        discord_create_interaction_response(client, event->id, event->token, &resp, NULL);
        return;
    }

    char inv_desc[1024];
    snprintf(inv_desc, sizeof(inv_desc),
             "**<@%" PRIu64 ">** has invited you to join the clan **[%s]**!\n\n"
             "**Description:**\n%s\n\n"
             "Would you like to accept this invitation?",
             inviter_id, clan.name, clan.description);

    char leader_mention[64];
    snprintf(leader_mention, sizeof(leader_mention), "<@%s>", clan.owner_id);

    struct discord_embed_field dm_fields[] = {
        { .name = "👑 Clan Leader", .value = leader_mention, ._inline = true },
        { .name = "🏷️ Clan Tag", .value = clan.name, ._inline = true }
    };

    struct discord_embed dm_embed = {
        .title = "⚔️ You Have Been Invited to Join a Clan!",
        .description = inv_desc,
        .color = 0x9B59B6,
        .fields = &(struct discord_embed_fields){
            .size = sizeof(dm_fields) / sizeof(dm_fields[0]),
            .array = dm_fields
        },
        .footer = &(struct discord_embed_footer){
            .text = "Bare Metal Builders • Clan Invitation"
        }
    };

    if (clan.picture_url[0]) {
        dm_embed.thumbnail = &(struct discord_embed_thumbnail){
            .url = clan.picture_url
        };
    }

    char acc_id[64], dec_id[64];
    snprintf(acc_id, sizeof(acc_id), "clan_inv_acc:%d", invite_id);
    snprintf(dec_id, sizeof(dec_id), "clan_inv_dec:%d", invite_id);

    struct discord_component buttons[] = {
        {
            .type = DISCORD_COMPONENT_BUTTON,
            .style = DISCORD_BUTTON_SUCCESS,
            .label = "✅ Accept Invitation",
            .custom_id = acc_id
        },
        {
            .type = DISCORD_COMPONENT_BUTTON,
            .style = DISCORD_BUTTON_DANGER,
            .label = "❌ Decline",
            .custom_id = dec_id
        }
    };

    struct discord_component action_row = {
        .type = DISCORD_COMPONENT_ACTION_ROW,
        .components = &(struct discord_components){
            .size = sizeof(buttons) / sizeof(buttons[0]),
            .array = buttons
        }
    };

    struct discord_create_message dm_msg = {
        .embeds = &(struct discord_embeds){
            .size = 1,
            .array = &dm_embed
        },
        .components = &(struct discord_components){
            .size = 1,
            .array = &action_row
        }
    };

    discord_create_message(client, dm_ch.id, &dm_msg, NULL);

    char success_feedback[256];
    snprintf(success_feedback, sizeof(success_feedback),
             "✅ Clan invitation sent to <@%" PRIu64 "> via Direct Message!", target_id);

    struct discord_interaction_response resp = {
        .type = DISCORD_INTERACTION_CHANNEL_MESSAGE_WITH_SOURCE,
        .data = &(struct discord_interaction_callback_data){
            .content = success_feedback,
            .flags = DISCORD_MESSAGE_EPHEMERAL
        }
    };
    discord_create_interaction_response(client, event->id, event->token, &resp, NULL);
}

void on_clan_invite_button(struct discord *client, const struct discord_interaction *event, const char *custom_id)
{
    int is_accept = (strncmp(custom_id, "clan_inv_acc:", 13) == 0);
    const char *id_ptr = custom_id + (is_accept ? 13 : 13);
    int invite_id = atoi(id_ptr);

    if (invite_id <= 0) return;

    clan_invite_t invite;
    if (db_clan_get_invite(invite_id, &invite) != 0) {
        struct discord_interaction_response resp = {
            .type = DISCORD_INTERACTION_CHANNEL_MESSAGE_WITH_SOURCE,
            .data = &(struct discord_interaction_callback_data){
                .content = "❌ Invitation not found or expired.",
                .flags = DISCORD_MESSAGE_EPHEMERAL
            }
        };
        discord_create_interaction_response(client, event->id, event->token, &resp, NULL);
        return;
    }

    if (strcmp(invite.status, "pending") != 0) {
        char status_msg[128];
        snprintf(status_msg, sizeof(status_msg), "❌ This invitation was already %s.", invite.status);
        struct discord_interaction_response resp = {
            .type = DISCORD_INTERACTION_CHANNEL_MESSAGE_WITH_SOURCE,
            .data = &(struct discord_interaction_callback_data){
                .content = status_msg,
                .flags = DISCORD_MESSAGE_EPHEMERAL
            }
        };
        discord_create_interaction_response(client, event->id, event->token, &resp, NULL);
        return;
    }

    u64snowflake user_id = event->user ? event->user->id : (event->member && event->member->user ? event->member->user->id : 0);
    u64snowflake target_id = (u64snowflake)strtoull(invite.target_id, NULL, 10);

    if (user_id != target_id) {
        struct discord_interaction_response resp = {
            .type = DISCORD_INTERACTION_CHANNEL_MESSAGE_WITH_SOURCE,
            .data = &(struct discord_interaction_callback_data){
                .content = "❌ This invitation was not addressed to you.",
                .flags = DISCORD_MESSAGE_EPHEMERAL
            }
        };
        discord_create_interaction_response(client, event->id, event->token, &resp, NULL);
        return;
    }

    clan_t clan;
    if (db_clan_get_by_id(invite.clan_id, &clan) != 0) {
        struct discord_interaction_response resp = {
            .type = DISCORD_INTERACTION_CHANNEL_MESSAGE_WITH_SOURCE,
            .data = &(struct discord_interaction_callback_data){
                .content = "❌ Clan no longer exists.",
                .flags = DISCORD_MESSAGE_EPHEMERAL
            }
        };
        discord_create_interaction_response(client, event->id, event->token, &resp, NULL);
        return;
    }

    u64snowflake guild_id = (u64snowflake)strtoull(clan.guild_id, NULL, 10);
    u64snowflake inviter_id = (u64snowflake)strtoull(invite.inviter_id, NULL, 10);
    u64snowflake owner_id = (u64snowflake)strtoull(clan.owner_id, NULL, 10);
    u64snowflake role_id = (u64snowflake)strtoull(clan.role_id, NULL, 10);

    if (is_accept) {
        db_clan_update_invite_status(invite_id, "accepted");
        db_clan_add_member(clan.id, target_id, guild_id);

        discord_add_guild_member_role(client, guild_id, target_id, role_id, NULL, NULL);

        const char *username = event->user ? event->user->username : "Member";
        char new_nick[33];
        format_clan_nickname(clan.name, username, new_nick, sizeof(new_nick));

        struct discord_modify_guild_member mod_member = {
            .nick = new_nick
        };
        discord_modify_guild_member(client, guild_id, target_id, &mod_member, NULL);

        char join_msg[512];
        snprintf(join_msg, sizeof(join_msg),
                 "🎉 **Welcome to [%s]!**\n\nYou have joined the clan, received the clan role, and your nickname has been updated to `%s`.",
                 clan.name, new_nick);

        struct discord_interaction_response resp = {
            .type = DISCORD_INTERACTION_UPDATE_MESSAGE,
            .data = &(struct discord_interaction_callback_data){
                .content = join_msg,
                .components = &(struct discord_components){ .size = 0 }
            }
        };
        discord_create_interaction_response(client, event->id, event->token, &resp, NULL);

        char inviter_notif[256];
        snprintf(inviter_notif, sizeof(inviter_notif),
                 "🎉 <@%" PRIu64 "> has accepted your invitation and joined the clan **[%s]**!",
                 target_id, clan.name);
        send_user_dm(client, inviter_id, inviter_notif, NULL);

        if (inviter_id != owner_id) {
            char owner_notif[256];
            snprintf(owner_notif, sizeof(owner_notif),
                     "🎉 <@%" PRIu64 "> has joined your clan **[%s]**! (Invited by <@%" PRIu64 ">)",
                     target_id, clan.name, inviter_id);
            send_user_dm(client, owner_id, owner_notif, NULL);
        }
    } else {
        db_clan_update_invite_status(invite_id, "declined");

        char dec_msg[256];
        snprintf(dec_msg, sizeof(dec_msg), "❌ You have declined the invitation to join **[%s]**.", clan.name);

        struct discord_interaction_response resp = {
            .type = DISCORD_INTERACTION_UPDATE_MESSAGE,
            .data = &(struct discord_interaction_callback_data){
                .content = dec_msg,
                .components = &(struct discord_components){ .size = 0 }
            }
        };
        discord_create_interaction_response(client, event->id, event->token, &resp, NULL);

        char inviter_notif[256];
        snprintf(inviter_notif, sizeof(inviter_notif),
                 "❌ <@%" PRIu64 "> did not want to join the clan **[%s]**.",
                 target_id, clan.name);
        send_user_dm(client, inviter_id, inviter_notif, NULL);

        if (inviter_id != owner_id) {
            char owner_notif[256];
            snprintf(owner_notif, sizeof(owner_notif),
                     "❌ <@%" PRIu64 "> did not want to join the clan **[%s]**.",
                     target_id, clan.name);
            send_user_dm(client, owner_id, owner_notif, NULL);
        }
    }
}

