
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <inttypes.h>
#include <pthread.h>
#include <concord/discord.h>
#include <concord/log.h>
#include "config.h"
#include "db.h"
#include "voice.h"

#define MAX_TRACKED_CHANNELS 128
#define MAX_CHANNEL_MEMBERS 64

#define PERM_CONNECT (1ULL << 20)
#define PERM_VIEW_CHANNEL (1ULL << 10)

typedef struct {
    u64snowflake channel_id;
    u64snowflake guild_id;
    u64snowflake owner_id;
    int is_private;
    u64snowflake members[MAX_CHANNEL_MEMBERS];
    int member_count;
} tracked_voice_t;

static tracked_voice_t g_tracked_channels[MAX_TRACKED_CHANNELS];
static int g_tracked_count = 0;
static pthread_mutex_t g_voice_mutex = PTHREAD_MUTEX_INITIALIZER;

void voice_init(void)
{
    pthread_mutex_lock(&g_voice_mutex);
    memset(g_tracked_channels, 0, sizeof(g_tracked_channels));
    g_tracked_count = 0;
    pthread_mutex_unlock(&g_voice_mutex);
}

static int is_create_voice_channel(struct discord *client, u64snowflake channel_id)
{
    if (channel_id == 0) return 0;
    if (g_config.create_voice_channel_id != 0 && channel_id == g_config.create_voice_channel_id)
        return 1;

    struct discord_channel ch = { 0 };
    struct discord_ret_channel ret = { .sync = &ch };
    if (discord_get_channel(client, channel_id, &ret) == CCORD_OK && ch.name) {
        if (strcasecmp(ch.name, "create your voice") == 0 ||
            strcasecmp(ch.name, "create-your-voice") == 0 ||
            strcasecmp(ch.name, "create voice") == 0) {
            return 1;
        }
    }
    return 0;
}

static tracked_voice_t *find_tracked_channel_locked(u64snowflake channel_id)
{
    for (int i = 0; i < g_tracked_count; i++) {
        if (g_tracked_channels[i].channel_id == channel_id) {
            return &g_tracked_channels[i];
        }
    }
    return NULL;
}

static tracked_voice_t *find_user_channel_locked(u64snowflake user_id)
{
    for (int i = 0; i < g_tracked_count; i++) {
        for (int j = 0; j < g_tracked_channels[i].member_count; j++) {
            if (g_tracked_channels[i].members[j] == user_id) {
                return &g_tracked_channels[i];
            }
        }
    }
    return NULL;
}

static tracked_voice_t *find_owned_channel_locked(u64snowflake owner_id)
{
    for (int i = 0; i < g_tracked_count; i++) {
        if (g_tracked_channels[i].owner_id == owner_id) {
            return &g_tracked_channels[i];
        }
    }
    return NULL;
}

static void add_member_to_tracked_locked(tracked_voice_t *tv, u64snowflake user_id)
{
    for (int i = 0; i < tv->member_count; i++) {
        if (tv->members[i] == user_id) return;
    }
    if (tv->member_count < MAX_CHANNEL_MEMBERS) {
        tv->members[tv->member_count++] = user_id;
    }
}

static int remove_member_from_tracked_locked(tracked_voice_t *tv, u64snowflake user_id)
{
    for (int i = 0; i < tv->member_count; i++) {
        if (tv->members[i] == user_id) {
            for (int j = i; j < tv->member_count - 1; j++) {
                tv->members[j] = tv->members[j + 1];
            }
            tv->member_count--;
            return 1;
        }
    }
    return 0;
}

static void remove_tracked_channel_locked(int index)
{
    if (index < 0 || index >= g_tracked_count) return;
    for (int i = index; i < g_tracked_count - 1; i++) {
        g_tracked_channels[i] = g_tracked_channels[i + 1];
    }
    g_tracked_count--;
}

void on_voice_state_update(struct discord *client, const struct discord_voice_state *event)
{
    if (!event || !event->user_id) return;

    u64snowflake user_id = event->user_id;
    u64snowflake guild_id = event->guild_id ? event->guild_id : g_config.guild_id;
    u64snowflake joined_channel_id = event->channel_id;

    pthread_mutex_lock(&g_voice_mutex);

    tracked_voice_t *prev_tv = find_user_channel_locked(user_id);
    if (prev_tv && prev_tv->channel_id != joined_channel_id) {
        u64snowflake left_channel_id = prev_tv->channel_id;
        u64snowflake owner_id = prev_tv->owner_id;

        remove_member_from_tracked_locked(prev_tv, user_id);

        if (prev_tv->member_count == 0) {

            log_info("Temp voice channel %" PRIu64 " is empty, deleting", left_channel_id);

            int idx = (int)(prev_tv - g_tracked_channels);
            remove_tracked_channel_locked(idx);

            pthread_mutex_unlock(&g_voice_mutex);
            discord_delete_channel(client, left_channel_id, NULL);
            db_temp_voice_delete(left_channel_id);
            pthread_mutex_lock(&g_voice_mutex);
        } else if (user_id == owner_id) {

            int r = rand() % prev_tv->member_count;
            u64snowflake new_owner_id = prev_tv->members[r];
            prev_tv->owner_id = new_owner_id;

            log_info("Owner left temp voice %" PRIu64 ", transferring ownership to %" PRIu64,
                     left_channel_id, new_owner_id);

            pthread_mutex_unlock(&g_voice_mutex);

            db_temp_voice_update_owner(left_channel_id, new_owner_id);

            char notif[256];
            snprintf(notif, sizeof(notif),
                     "👑 The previous owner left the channel. Ownership has been transferred to <@%" PRIu64 ">!",
                     new_owner_id);

            struct discord_create_message msg = {
                .content = notif
            };
            discord_create_message(client, left_channel_id, &msg, NULL);

            pthread_mutex_lock(&g_voice_mutex);
        }
    }

    if (joined_channel_id != 0) {
        tracked_voice_t *joined_tv = find_tracked_channel_locked(joined_channel_id);
        if (joined_tv) {
            add_member_to_tracked_locked(joined_tv, user_id);
        }
    }

    pthread_mutex_unlock(&g_voice_mutex);

    if (joined_channel_id != 0 && is_create_voice_channel(client, joined_channel_id)) {
        const char *username = (event->member && event->member->user) ?
            event->member->user->username : "User";

        char ch_name[100];
        snprintf(ch_name, sizeof(ch_name), "🔊 %s's Room", username);

        struct discord_create_guild_channel params = {
            .name = ch_name,
            .type = DISCORD_CHANNEL_GUILD_VOICE
        };

        struct discord_channel created_ch = { 0 };
        struct discord_ret_channel ret = {
            .sync = &created_ch
        };

        if (discord_create_guild_channel(client, guild_id, &params, &ret) == CCORD_OK && created_ch.id != 0) {
            u64snowflake new_ch_id = created_ch.id;
            log_info("Created temp voice channel %" PRIu64 " for user %" PRIu64, new_ch_id, user_id);

            db_temp_voice_register(new_ch_id, guild_id, user_id);

            pthread_mutex_lock(&g_voice_mutex);
            if (g_tracked_count < MAX_TRACKED_CHANNELS) {
                tracked_voice_t *tv = &g_tracked_channels[g_tracked_count++];
                memset(tv, 0, sizeof(*tv));
                tv->channel_id = new_ch_id;
                tv->guild_id = guild_id;
                tv->owner_id = user_id;
                tv->is_private = 0;
                tv->members[0] = user_id;
                tv->member_count = 1;
            }
            pthread_mutex_unlock(&g_voice_mutex);

            struct discord_modify_guild_member mod = {
                .channel_id = new_ch_id
            };
            discord_modify_guild_member(client, guild_id, user_id, &mod, NULL);

            struct discord_embed_field fields[] = {
                { .name = "🔒 `/voice-private`", .value = "Lock your voice channel to invite-only.", ._inline = true },
                { .name = "🔓 `/voice-public`", .value = "Unlock your voice channel for everyone.", ._inline = true },
                { .name = "✅ `/voice-permit @user`", .value = "Allow a specific member to join.", ._inline = false },
                { .name = "👢 `/voice-kick @user`", .value = "Disconnect a user from your channel.", ._inline = false }
            };

            struct discord_embed embed = {
                .title = "🔊 Your Voice Channel is Ready!",
                .description = "You are the owner of this voice channel. When everyone leaves, it will automatically be deleted.",
                .color = 0x2ECC71,
                .fields = &(struct discord_embed_fields){
                    .size = sizeof(fields) / sizeof(fields[0]),
                    .array = fields
                },
                .footer = &(struct discord_embed_footer){
                    .text = "Bare Metal Builders • Dynamic Voice"
                }
            };

            struct discord_create_message msg = {
                .embeds = &(struct discord_embeds){
                    .size = 1,
                    .array = &embed
                }
            };
            discord_create_message(client, new_ch_id, &msg, NULL);
        }
    }
}

void on_voice_private_command(struct discord *client, const struct discord_interaction *event)
{
    if (!event->member || !event->member->user) return;
    u64snowflake user_id = event->member->user->id;

    pthread_mutex_lock(&g_voice_mutex);
    tracked_voice_t *tv = find_owned_channel_locked(user_id);
    u64snowflake ch_id = tv ? tv->channel_id : 0;
    u64snowflake guild_id = tv ? tv->guild_id : (event->guild_id ? event->guild_id : g_config.guild_id);
    if (tv) tv->is_private = 1;
    pthread_mutex_unlock(&g_voice_mutex);

    if (ch_id == 0) {
        struct discord_interaction_response resp = {
            .type = DISCORD_INTERACTION_CHANNEL_MESSAGE_WITH_SOURCE,
            .data = &(struct discord_interaction_callback_data){
                .content = "❌ You are not the owner of any active temporary voice channel.",
                .flags = DISCORD_MESSAGE_EPHEMERAL
            }
        };
        discord_create_interaction_response(client, event->id, event->token, &resp, NULL);
        return;
    }

    struct discord_edit_channel_permissions params_everyone = {
        .deny = PERM_CONNECT,
        .type = 0
    };
    discord_edit_channel_permissions(client, ch_id, guild_id, &params_everyone, NULL);

    struct discord_edit_channel_permissions params_owner = {
        .allow = PERM_CONNECT | PERM_VIEW_CHANNEL,
        .type = 1
    };
    discord_edit_channel_permissions(client, ch_id, user_id, &params_owner, NULL);

    db_temp_voice_set_privacy(ch_id, 1);

    struct discord_interaction_response resp = {
        .type = DISCORD_INTERACTION_CHANNEL_MESSAGE_WITH_SOURCE,
        .data = &(struct discord_interaction_callback_data){
            .content = "🔒 Your voice channel is now **private**! Use `/voice-permit @user` to allow friends to join.",
            .flags = DISCORD_MESSAGE_EPHEMERAL
        }
    };
    discord_create_interaction_response(client, event->id, event->token, &resp, NULL);
}

void on_voice_public_command(struct discord *client, const struct discord_interaction *event)
{
    if (!event->member || !event->member->user) return;
    u64snowflake user_id = event->member->user->id;

    pthread_mutex_lock(&g_voice_mutex);
    tracked_voice_t *tv = find_owned_channel_locked(user_id);
    u64snowflake ch_id = tv ? tv->channel_id : 0;
    u64snowflake guild_id = tv ? tv->guild_id : (event->guild_id ? event->guild_id : g_config.guild_id);
    if (tv) tv->is_private = 0;
    pthread_mutex_unlock(&g_voice_mutex);

    if (ch_id == 0) {
        struct discord_interaction_response resp = {
            .type = DISCORD_INTERACTION_CHANNEL_MESSAGE_WITH_SOURCE,
            .data = &(struct discord_interaction_callback_data){
                .content = "❌ You are not the owner of any active temporary voice channel.",
                .flags = DISCORD_MESSAGE_EPHEMERAL
            }
        };
        discord_create_interaction_response(client, event->id, event->token, &resp, NULL);
        return;
    }

    discord_delete_channel_permission(client, ch_id, guild_id, NULL);

    db_temp_voice_set_privacy(ch_id, 0);

    struct discord_interaction_response resp = {
        .type = DISCORD_INTERACTION_CHANNEL_MESSAGE_WITH_SOURCE,
        .data = &(struct discord_interaction_callback_data){
            .content = "🔓 Your voice channel is now **public**! Anyone in the server can join.",
            .flags = DISCORD_MESSAGE_EPHEMERAL
        }
    };
    discord_create_interaction_response(client, event->id, event->token, &resp, NULL);
}

void on_voice_permit_command(struct discord *client, const struct discord_interaction *event)
{
    if (!event->member || !event->member->user) return;
    u64snowflake user_id = event->member->user->id;

    pthread_mutex_lock(&g_voice_mutex);
    tracked_voice_t *tv = find_owned_channel_locked(user_id);
    u64snowflake ch_id = tv ? tv->channel_id : 0;
    pthread_mutex_unlock(&g_voice_mutex);

    if (ch_id == 0) {
        struct discord_interaction_response resp = {
            .type = DISCORD_INTERACTION_CHANNEL_MESSAGE_WITH_SOURCE,
            .data = &(struct discord_interaction_callback_data){
                .content = "❌ You are not the owner of any active temporary voice channel.",
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

    if (target_id == 0) {
        struct discord_interaction_response resp = {
            .type = DISCORD_INTERACTION_CHANNEL_MESSAGE_WITH_SOURCE,
            .data = &(struct discord_interaction_callback_data){
                .content = "❌ Please specify a valid member to permit.",
                .flags = DISCORD_MESSAGE_EPHEMERAL
            }
        };
        discord_create_interaction_response(client, event->id, event->token, &resp, NULL);
        return;
    }

    struct discord_edit_channel_permissions params = {
        .allow = PERM_CONNECT | PERM_VIEW_CHANNEL,
        .type = 1
    };
    discord_edit_channel_permissions(client, ch_id, target_id, &params, NULL);

    char msg[256];
    snprintf(msg, sizeof(msg), "✅ Permitted <@%" PRIu64 "> to join your voice channel.", target_id);

    struct discord_interaction_response resp = {
        .type = DISCORD_INTERACTION_CHANNEL_MESSAGE_WITH_SOURCE,
        .data = &(struct discord_interaction_callback_data){
            .content = msg,
            .flags = DISCORD_MESSAGE_EPHEMERAL
        }
    };
    discord_create_interaction_response(client, event->id, event->token, &resp, NULL);
}

void on_voice_kick_command(struct discord *client, const struct discord_interaction *event)
{
    if (!event->member || !event->member->user) return;
    u64snowflake user_id = event->member->user->id;

    pthread_mutex_lock(&g_voice_mutex);
    tracked_voice_t *tv = find_owned_channel_locked(user_id);
    u64snowflake ch_id = tv ? tv->channel_id : 0;
    u64snowflake guild_id = tv ? tv->guild_id : (event->guild_id ? event->guild_id : g_config.guild_id);
    pthread_mutex_unlock(&g_voice_mutex);

    if (ch_id == 0) {
        struct discord_interaction_response resp = {
            .type = DISCORD_INTERACTION_CHANNEL_MESSAGE_WITH_SOURCE,
            .data = &(struct discord_interaction_callback_data){
                .content = "❌ You are not the owner of any active temporary voice channel.",
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

    if (target_id == 0 || target_id == user_id) {
        struct discord_interaction_response resp = {
            .type = DISCORD_INTERACTION_CHANNEL_MESSAGE_WITH_SOURCE,
            .data = &(struct discord_interaction_callback_data){
                .content = "❌ Please specify a valid member to kick (you cannot kick yourself).",
                .flags = DISCORD_MESSAGE_EPHEMERAL
            }
        };
        discord_create_interaction_response(client, event->id, event->token, &resp, NULL);
        return;
    }

    struct discord_modify_guild_member mod = {
        .channel_id = 0
    };
    discord_modify_guild_member(client, guild_id, target_id, &mod, NULL);

    struct discord_edit_channel_permissions params = {
        .deny = PERM_CONNECT,
        .type = 1
    };
    discord_edit_channel_permissions(client, ch_id, target_id, &params, NULL);

    char msg[256];
    snprintf(msg, sizeof(msg), "👢 Disconnected <@%" PRIu64 "> from your voice channel.", target_id);

    struct discord_interaction_response resp = {
        .type = DISCORD_INTERACTION_CHANNEL_MESSAGE_WITH_SOURCE,
        .data = &(struct discord_interaction_callback_data){
            .content = msg,
            .flags = DISCORD_MESSAGE_EPHEMERAL
        }
    };
    discord_create_interaction_response(client, event->id, event->token, &resp, NULL);
}

