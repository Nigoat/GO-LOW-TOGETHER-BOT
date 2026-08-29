
#ifndef DB_H
#define DB_H

#include <postgresql/libpq-fe.h>
#include <concord/discord.h>

typedef struct {
    int id;
    char discord_user_id[32];
    char guild_id[32];
    char role_id[32];
    char action[8];
} pending_role_update_t;

typedef struct {
    int id;
    char guild_id[32];
    char name[64];
    char description[512];
    char picture_url[512];
    char owner_id[32];
    char role_id[32];
} clan_t;

typedef struct {
    int id;
    int clan_id;
    char guild_id[32];
    char inviter_id[32];
    char target_id[32];
    char status[16];
} clan_invite_t;

typedef struct {
    char channel_id[32];
    char guild_id[32];
    char owner_id[32];
    int is_private;
} temp_voice_t;

void db_init(const char *connection_string);
void db_cleanup(void);
PGconn *db_get_thread_connection(void);

char *db_create_verify_token(u64snowflake discord_user_id, u64snowflake guild_id);
int db_validate_verify_token(const char *token, u64snowflake *discord_user_id, u64snowflake *guild_id);
int db_mark_verify_token_used(const char *token);

int db_get_pending_role_updates(pending_role_update_t **updates, int *count);
int db_mark_role_update_processed(int id);

void db_cleanup_old_tokens(void);
void db_cleanup_old_pending_updates(void);

int db_clan_create(u64snowflake guild_id, const char *name, const char *description,
                   const char *picture_url, u64snowflake owner_id, u64snowflake role_id, int *clan_id_out);
int db_clan_get_by_user(u64snowflake user_id, clan_t *clan_out);
int db_clan_get_by_id(int clan_id, clan_t *clan_out);
int db_clan_get_by_name(const char *name, clan_t *clan_out);
int db_clan_add_member(int clan_id, u64snowflake user_id, u64snowflake guild_id);
int db_clan_create_invite(int clan_id, u64snowflake guild_id, u64snowflake inviter_id,
                          u64snowflake target_id, int *invite_id_out);
int db_clan_get_invite(int invite_id, clan_invite_t *invite_out);
int db_clan_update_invite_status(int invite_id, const char *status);

int db_temp_voice_register(u64snowflake channel_id, u64snowflake guild_id, u64snowflake owner_id);
int db_temp_voice_get(u64snowflake channel_id, temp_voice_t *voice_out);
int db_temp_voice_update_owner(u64snowflake channel_id, u64snowflake new_owner_id);
int db_temp_voice_set_privacy(u64snowflake channel_id, int is_private);
int db_temp_voice_delete(u64snowflake channel_id);

#endif

