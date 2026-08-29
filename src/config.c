
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <concord/log.h>
#include "config.h"

struct bmb_config g_config;

u64snowflake config_parse_snowflake(const char *env_name)
{
    const char *val = getenv(env_name);
    if (!val || !*val) {
        log_fatal("Missing required environment variable: %s", env_name);
        return 0;
    }
    return (u64snowflake)strtoull(val, NULL, 10);
}

u64snowflake config_parse_optional_snowflake(const char *env_name)
{
    const char *val = getenv(env_name);
    if (!val || !*val) {
        return 0;
    }
    return (u64snowflake)strtoull(val, NULL, 10);
}

static char *config_require_env(const char *env_name)
{
    const char *val = getenv(env_name);
    if (!val || !*val) {
        log_fatal("Missing required environment variable: %s", env_name);
        return NULL;
    }
    return strdup(val);
}

u64snowflake *config_parse_snowflake_list(const char *env_name, int *count_out)
{
    const char *val = getenv(env_name);
    if (!val || !*val) {
        *count_out = 0;
        return NULL;
    }
    int capacity = 8;
    int count = 0;
    u64snowflake *ids = malloc(capacity * sizeof(u64snowflake));
    if (!ids) {
        *count_out = 0;
        return NULL;
    }
    char *copy = strdup(val);
    if (!copy) {
        free(ids);
        *count_out = 0;
        return NULL;
    }
    char *token = strtok(copy, ",");
    while (token) {
        while (*token == ' ') token++;
        if (count >= capacity) {
            capacity *= 2;
            u64snowflake *new_ids = realloc(ids, capacity * sizeof(u64snowflake));
            if (!new_ids) {
                free(ids);
                free(copy);
                *count_out = count;
                return NULL;
            }
            ids = new_ids;
        }
        ids[count++] = (u64snowflake)strtoull(token, NULL, 10);
        token = strtok(NULL, ",");
    }
    free(copy);
    *count_out = count;
    return ids;
}

void config_load(void)
{
    g_config.bot_token = config_require_env("BOT_TOKEN");
    g_config.database_url = config_require_env("DATABASE_URL");
    g_config.website_base_url = config_require_env("WEBSITE_BASE_URL");

    g_config.guild_id = config_parse_snowflake("GUILD_ID");
    g_config.verify_channel_id = config_parse_snowflake("VERIFY_CHANNEL_ID");
    g_config.ticket_channel_id = config_parse_snowflake("TICKET_CHANNEL_ID");
    g_config.create_voice_channel_id = config_parse_optional_snowflake("CREATE_VOICE_CHANNEL_ID");
    g_config.owner_role_id = config_parse_snowflake("OWNER_ROLE_ID");
    g_config.moderator_role_id = config_parse_snowflake("MODERATOR_ROLE_ID");
    g_config.streamer_role_id = config_parse_snowflake("STREAMER_ROLE_ID");
    g_config.verified_role_id = config_parse_snowflake("VERIFIED_ROLE_ID");

    g_config.role_c = config_parse_snowflake("ROLE_C");
    g_config.role_cpp = config_parse_snowflake("ROLE_CPP");
    g_config.role_rust = config_parse_snowflake("ROLE_RUST");
    g_config.role_zig = config_parse_snowflake("ROLE_ZIG");
    g_config.role_assembly = config_parse_snowflake("ROLE_ASSEMBLY");

    g_config.role_beginner = config_parse_snowflake("ROLE_BEGINNER");
    g_config.role_intermediate = config_parse_snowflake("ROLE_INTERMEDIATE");
    g_config.role_advanced = config_parse_snowflake("ROLE_ADVANCED");
    g_config.role_wizard = config_parse_snowflake("ROLE_WIZARD");

    g_config.live_coding_voice_ids = config_parse_snowflake_list(
        "LIVE_CODING_VOICE_CHANNEL_IDS", &g_config.live_coding_voice_count);

    log_info("Configuration loaded successfully");
}

