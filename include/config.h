
#ifndef CONFIG_H
#define CONFIG_H

#include <concord/discord.h>

struct bmb_config {
    char *bot_token;
    char *database_url;
    char *website_base_url;

    u64snowflake guild_id;
    u64snowflake verify_channel_id;
    u64snowflake ticket_channel_id;
    u64snowflake create_voice_channel_id;
    u64snowflake owner_role_id;
    u64snowflake moderator_role_id;
    u64snowflake streamer_role_id;
    u64snowflake verified_role_id;

    u64snowflake role_c;
    u64snowflake role_cpp;
    u64snowflake role_rust;
    u64snowflake role_zig;
    u64snowflake role_assembly;
    u64snowflake role_other;

    u64snowflake role_beginner;
    u64snowflake role_intermediate;
    u64snowflake role_advanced;
    u64snowflake role_wizard;

    u64snowflake *live_coding_voice_ids;
    int live_coding_voice_count;
};

extern struct bmb_config g_config;

void config_load(void);
u64snowflake config_parse_snowflake(const char *env_name);
u64snowflake config_parse_optional_snowflake(const char *env_name);
u64snowflake *config_parse_snowflake_list(const char *env_name, int *count_out);

#endif

