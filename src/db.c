
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>
#include <postgresql/libpq-fe.h>
#include <concord/log.h>
#include "db.h"

static PGconn *g_main_conn = NULL;
static char g_conn_string[2048] = {0};
static pthread_mutex_t g_db_mutex = PTHREAD_MUTEX_INITIALIZER;

static PGconn *db_connect(const char *conninfo)
{
    PGconn *conn = PQconnectdb(conninfo);
    if (PQstatus(conn) != CONNECTION_OK) {
        log_error("Database connection failed");
        PQfinish(conn);
        return NULL;
    }
    return conn;
}

static PGconn *db_ensure_connection(PGconn *conn)
{
    if (!conn || PQstatus(conn) != CONNECTION_OK) {
        if (conn) PQfinish(conn);
        if (!g_conn_string[0]) return NULL;
        conn = db_connect(g_conn_string);
        g_main_conn = conn;
        if (!conn) return NULL;
    }
    return conn;
}

void db_init(const char *connection_string)
{
    size_t len = strlen(connection_string);
    if (len >= sizeof(g_conn_string)) {
        log_fatal("Database connection string too long");
        return;
    }
    memcpy(g_conn_string, connection_string, len + 1);
    g_main_conn = db_connect(g_conn_string);
    if (!g_main_conn) {
        log_fatal("Failed to connect to database");
        return;
    }

    const char *schema_sql =
        "CREATE TABLE IF NOT EXISTS verify_tokens ("
        "  id SERIAL PRIMARY KEY,"
        "  token VARCHAR(64) UNIQUE NOT NULL,"
        "  discord_user_id VARCHAR(32) NOT NULL,"
        "  guild_id VARCHAR(32) NOT NULL,"
        "  used BOOLEAN DEFAULT FALSE,"
        "  created_at TIMESTAMP DEFAULT NOW(),"
        "  expires_at TIMESTAMP NOT NULL"
        ");"
        "CREATE TABLE IF NOT EXISTS pending_role_updates ("
        "  id SERIAL PRIMARY KEY,"
        "  discord_user_id VARCHAR(32) NOT NULL,"
        "  guild_id VARCHAR(32) NOT NULL,"
        "  role_id VARCHAR(32) NOT NULL,"
        "  action VARCHAR(8) NOT NULL,"
        "  processed BOOLEAN DEFAULT FALSE,"
        "  created_at TIMESTAMP DEFAULT NOW()"
        ");"
        "CREATE TABLE IF NOT EXISTS clans ("
        "  id SERIAL PRIMARY KEY,"
        "  guild_id VARCHAR(32) NOT NULL,"
        "  name VARCHAR(64) UNIQUE NOT NULL,"
        "  description TEXT NOT NULL,"
        "  picture_url TEXT,"
        "  owner_id VARCHAR(32) NOT NULL,"
        "  role_id VARCHAR(32) NOT NULL,"
        "  created_at TIMESTAMP DEFAULT NOW()"
        ");"
        "CREATE TABLE IF NOT EXISTS clan_members ("
        "  id SERIAL PRIMARY KEY,"
        "  clan_id INT REFERENCES clans(id) ON DELETE CASCADE,"
        "  user_id VARCHAR(32) UNIQUE NOT NULL,"
        "  guild_id VARCHAR(32) NOT NULL,"
        "  joined_at TIMESTAMP DEFAULT NOW()"
        ");"
        "CREATE TABLE IF NOT EXISTS clan_invites ("
        "  id SERIAL PRIMARY KEY,"
        "  clan_id INT REFERENCES clans(id) ON DELETE CASCADE,"
        "  guild_id VARCHAR(32) NOT NULL,"
        "  inviter_id VARCHAR(32) NOT NULL,"
        "  target_id VARCHAR(32) NOT NULL,"
        "  status VARCHAR(16) DEFAULT 'pending',"
        "  created_at TIMESTAMP DEFAULT NOW()"
        ");"
        "CREATE TABLE IF NOT EXISTS temp_voice_channels ("
        "  channel_id VARCHAR(32) PRIMARY KEY,"
        "  guild_id VARCHAR(32) NOT NULL,"
        "  owner_id VARCHAR(32) NOT NULL,"
        "  is_private INT DEFAULT 0,"
        "  created_at TIMESTAMP DEFAULT NOW()"
        ");";

    PGresult *res = PQexec(g_main_conn, schema_sql);
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        log_warn("Schema initialization notice/warning: %s", PQerrorMessage(g_main_conn));
    }
    PQclear(res);

    log_info("Database connection established and schema verified");
}

void db_cleanup(void)
{
    if (g_main_conn) {
        PQfinish(g_main_conn);
        g_main_conn = NULL;
    }
}

PGconn *db_get_thread_connection(void)
{
    if (!g_conn_string[0]) return NULL;
    PGconn *conn = db_connect(g_conn_string);
    return conn;
}

static void generate_random_hex(char *buf, int bytes)
{
    unsigned char rand_bytes[32];
    FILE *f = fopen("/dev/urandom", "rb");
    if (f) {
        size_t nread = fread(rand_bytes, 1, (size_t)bytes, f);
        fclose(f);
        if ((int)nread != bytes) {
            log_error("Failed to read enough random bytes");
            memset(buf, '0', (size_t)(bytes * 2));
            buf[bytes * 2] = '\0';
            return;
        }
    } else {
        srand((unsigned)time(NULL));
        for (int i = 0; i < bytes; i++)
            rand_bytes[i] = (unsigned char)(rand() & 0xFF);
    }
    for (int i = 0; i < bytes; i++)
        snprintf(buf + (i * 2), 3, "%02x", rand_bytes[i]);
    buf[bytes * 2] = '\0';
}

char *db_create_verify_token(u64snowflake discord_user_id, u64snowflake guild_id)
{
    pthread_mutex_lock(&g_db_mutex);
    g_main_conn = db_ensure_connection(g_main_conn);
    if (!g_main_conn) {
        pthread_mutex_unlock(&g_db_mutex);
        return NULL;
    }
    char token[65];
    generate_random_hex(token, 32);
    char user_str[32], guild_str[32];
    snprintf(user_str, sizeof(user_str), "%llu", (unsigned long long)discord_user_id);
    snprintf(guild_str, sizeof(guild_str), "%llu", (unsigned long long)guild_id);
    const char *query =
        "INSERT INTO verify_tokens (token, discord_user_id, guild_id, expires_at) "
        "VALUES ($1, $2, $3, NOW() + INTERVAL '15 minutes')";
    const char *params[3] = { token, user_str, guild_str };
    int lengths[3] = { (int)strlen(token), (int)strlen(user_str), (int)strlen(guild_str) };
    int formats[3] = { 0, 0, 0 };
    PGresult *res = PQexecParams(g_main_conn, query, 3, NULL, params, lengths, formats, 0);
    int ok = (PQresultStatus(res) == PGRES_COMMAND_OK);
    PQclear(res);
    pthread_mutex_unlock(&g_db_mutex);
    if (!ok) return NULL;
    return strdup(token);
}

int db_validate_verify_token(const char *token, u64snowflake *discord_user_id, u64snowflake *guild_id)
{
    if (!token || strlen(token) != 64) return -1;
    pthread_mutex_lock(&g_db_mutex);
    g_main_conn = db_ensure_connection(g_main_conn);
    if (!g_main_conn) {
        pthread_mutex_unlock(&g_db_mutex);
        return -1;
    }
    const char *query =
        "SELECT discord_user_id, guild_id FROM verify_tokens "
        "WHERE token = $1 AND used = FALSE AND expires_at > NOW()";
    const char *params[1] = { token };
    int lengths[1] = { 64 };
    int formats[1] = { 0 };
    PGresult *res = PQexecParams(g_main_conn, query, 1, NULL, params, lengths, formats, 0);
    int ok = (PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) > 0);
    if (ok) {
        *discord_user_id = (u64snowflake)strtoull(PQgetvalue(res, 0, 0), NULL, 10);
        *guild_id = (u64snowflake)strtoull(PQgetvalue(res, 0, 1), NULL, 10);
    }
    PQclear(res);
    pthread_mutex_unlock(&g_db_mutex);
    return ok ? 0 : -1;
}

int db_mark_verify_token_used(const char *token)
{
    if (!token || strlen(token) != 64) return -1;
    pthread_mutex_lock(&g_db_mutex);
    g_main_conn = db_ensure_connection(g_main_conn);
    if (!g_main_conn) {
        pthread_mutex_unlock(&g_db_mutex);
        return -1;
    }
    const char *query = "DELETE FROM verify_tokens WHERE token = $1";
    const char *params[1] = { token };
    int lengths[1] = { 64 };
    int formats[1] = { 0 };
    PGresult *res = PQexecParams(g_main_conn, query, 1, NULL, params, lengths, formats, 0);
    int ok = (PQresultStatus(res) == PGRES_COMMAND_OK);
    PQclear(res);
    pthread_mutex_unlock(&g_db_mutex);
    return ok ? 0 : -1;
}

int db_get_pending_role_updates(pending_role_update_t **updates, int *count)
{
    PGconn *conn = db_get_thread_connection();
    if (!conn) return -1;
    const char *query =
        "SELECT id, discord_user_id, guild_id, role_id, action "
        "FROM pending_role_updates WHERE processed = FALSE ORDER BY created_at";
    PGresult *res = PQexec(conn, query);
    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        PQclear(res);
        PQfinish(conn);
        return -1;
    }
    int n = PQntuples(res);
    if (n == 0) {
        *updates = NULL;
        *count = 0;
        PQclear(res);
        PQfinish(conn);
        return 0;
    }
    *updates = malloc(n * sizeof(pending_role_update_t));
    if (!*updates) {
        PQclear(res);
        PQfinish(conn);
        return -1;
    }
    *count = n;
    for (int i = 0; i < n; i++) {
        (*updates)[i].id = atoi(PQgetvalue(res, i, 0));
        memset((*updates)[i].discord_user_id, 0, sizeof((*updates)[i].discord_user_id));
        memset((*updates)[i].guild_id, 0, sizeof((*updates)[i].guild_id));
        memset((*updates)[i].role_id, 0, sizeof((*updates)[i].role_id));
        memset((*updates)[i].action, 0, sizeof((*updates)[i].action));
        strncpy((*updates)[i].discord_user_id, PQgetvalue(res, i, 1), sizeof((*updates)[i].discord_user_id) - 1);
        strncpy((*updates)[i].guild_id, PQgetvalue(res, i, 2), sizeof((*updates)[i].guild_id) - 1);
        strncpy((*updates)[i].role_id, PQgetvalue(res, i, 3), sizeof((*updates)[i].role_id) - 1);
        strncpy((*updates)[i].action, PQgetvalue(res, i, 4), sizeof((*updates)[i].action) - 1);
    }
    PQclear(res);
    PQfinish(conn);
    return 0;
}

int db_mark_role_update_processed(int id)
{
    PGconn *conn = db_get_thread_connection();
    if (!conn) return -1;
    char id_str[16];
    snprintf(id_str, sizeof(id_str), "%d", id);
    const char *query = "DELETE FROM pending_role_updates WHERE id = $1";
    const char *params[1] = { id_str };
    int lengths[1] = { (int)strlen(id_str) };
    int formats[1] = { 0 };
    PGresult *res = PQexecParams(conn, query, 1, NULL, params, lengths, formats, 0);
    int ok = (PQresultStatus(res) == PGRES_COMMAND_OK);
    PQclear(res);
    PQfinish(conn);
    return ok ? 0 : -1;
}

void db_cleanup_old_tokens(void)
{
    pthread_mutex_lock(&g_db_mutex);
    g_main_conn = db_ensure_connection(g_main_conn);
    if (!g_main_conn) {
        pthread_mutex_unlock(&g_db_mutex);
        return;
    }
    const char *query =
        "DELETE FROM verify_tokens "
        "WHERE used = TRUE OR expires_at <= NOW()";
    PGresult *res = PQexec(g_main_conn, query);
    if (PQresultStatus(res) != PGRES_COMMAND_OK)
        log_error("db_cleanup_old_tokens failed");
    PQclear(res);
    pthread_mutex_unlock(&g_db_mutex);
}

void db_cleanup_old_pending_updates(void)
{
    PGconn *conn = db_get_thread_connection();
    if (!conn) return;
    const char *query =
        "DELETE FROM pending_role_updates "
        "WHERE processed = TRUE OR created_at < NOW() - INTERVAL '1 day'";
    PGresult *res = PQexec(conn, query);
    if (PQresultStatus(res) != PGRES_COMMAND_OK)
        log_error("db_cleanup_old_pending_updates failed");
    PQclear(res);
    PQfinish(conn);
}

int db_clan_create(u64snowflake guild_id, const char *name, const char *description,
                   const char *picture_url, u64snowflake owner_id, u64snowflake role_id, int *clan_id_out)
{
    pthread_mutex_lock(&g_db_mutex);
    g_main_conn = db_ensure_connection(g_main_conn);
    if (!g_main_conn) {
        pthread_mutex_unlock(&g_db_mutex);
        return -1;
    }

    char guild_str[32], owner_str[32], role_str[32];
    snprintf(guild_str, sizeof(guild_str), "%llu", (unsigned long long)guild_id);
    snprintf(owner_str, sizeof(owner_str), "%llu", (unsigned long long)owner_id);
    snprintf(role_str, sizeof(role_str), "%llu", (unsigned long long)role_id);

    const char *query =
        "INSERT INTO clans (guild_id, name, description, picture_url, owner_id, role_id) "
        "VALUES ($1, $2, $3, $4, $5, $6) RETURNING id";
    const char *params[6] = { guild_str, name, description, picture_url ? picture_url : "", owner_str, role_str };
    int lengths[6] = { (int)strlen(guild_str), (int)strlen(name), (int)strlen(description),
                       picture_url ? (int)strlen(picture_url) : 0, (int)strlen(owner_str), (int)strlen(role_str) };
    int formats[6] = { 0, 0, 0, 0, 0, 0 };

    PGresult *res = PQexecParams(g_main_conn, query, 6, NULL, params, lengths, formats, 0);
    if (PQresultStatus(res) != PGRES_TUPLES_OK || PQntuples(res) == 0) {
        log_error("db_clan_create failed: %s", PQerrorMessage(g_main_conn));
        PQclear(res);
        pthread_mutex_unlock(&g_db_mutex);
        return -1;
    }

    int clan_id = atoi(PQgetvalue(res, 0, 0));
    PQclear(res);

    char clan_id_str[16];
    snprintf(clan_id_str, sizeof(clan_id_str), "%d", clan_id);
    const char *mem_query = "INSERT INTO clan_members (clan_id, user_id, guild_id) VALUES ($1, $2, $3)";
    const char *mem_params[3] = { clan_id_str, owner_str, guild_str };
    int mem_lengths[3] = { (int)strlen(clan_id_str), (int)strlen(owner_str), (int)strlen(guild_str) };
    int mem_formats[3] = { 0, 0, 0 };
    PGresult *mem_res = PQexecParams(g_main_conn, mem_query, 3, NULL, mem_params, mem_lengths, mem_formats, 0);
    PQclear(mem_res);

    pthread_mutex_unlock(&g_db_mutex);

    if (clan_id_out) *clan_id_out = clan_id;
    return 0;
}

int db_clan_get_by_user(u64snowflake user_id, clan_t *clan_out)
{
    pthread_mutex_lock(&g_db_mutex);
    g_main_conn = db_ensure_connection(g_main_conn);
    if (!g_main_conn) {
        pthread_mutex_unlock(&g_db_mutex);
        return -1;
    }

    char user_str[32];
    snprintf(user_str, sizeof(user_str), "%llu", (unsigned long long)user_id);

    const char *query =
        "SELECT c.id, c.guild_id, c.name, c.description, COALESCE(c.picture_url, ''), c.owner_id, c.role_id "
        "FROM clans c JOIN clan_members m ON c.id = m.clan_id WHERE m.user_id = $1";
    const char *params[1] = { user_str };
    int lengths[1] = { (int)strlen(user_str) };
    int formats[1] = { 0 };

    PGresult *res = PQexecParams(g_main_conn, query, 1, NULL, params, lengths, formats, 0);
    int found = (PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) > 0);
    if (found && clan_out) {
        memset(clan_out, 0, sizeof(*clan_out));
        clan_out->id = atoi(PQgetvalue(res, 0, 0));
        strncpy(clan_out->guild_id, PQgetvalue(res, 0, 1), sizeof(clan_out->guild_id) - 1);
        strncpy(clan_out->name, PQgetvalue(res, 0, 2), sizeof(clan_out->name) - 1);
        strncpy(clan_out->description, PQgetvalue(res, 0, 3), sizeof(clan_out->description) - 1);
        strncpy(clan_out->picture_url, PQgetvalue(res, 0, 4), sizeof(clan_out->picture_url) - 1);
        strncpy(clan_out->owner_id, PQgetvalue(res, 0, 5), sizeof(clan_out->owner_id) - 1);
        strncpy(clan_out->role_id, PQgetvalue(res, 0, 6), sizeof(clan_out->role_id) - 1);
    }
    PQclear(res);
    pthread_mutex_unlock(&g_db_mutex);
    return found ? 0 : -1;
}

int db_clan_get_by_id(int clan_id, clan_t *clan_out)
{
    pthread_mutex_lock(&g_db_mutex);
    g_main_conn = db_ensure_connection(g_main_conn);
    if (!g_main_conn) {
        pthread_mutex_unlock(&g_db_mutex);
        return -1;
    }

    char id_str[16];
    snprintf(id_str, sizeof(id_str), "%d", clan_id);

    const char *query =
        "SELECT id, guild_id, name, description, COALESCE(picture_url, ''), owner_id, role_id "
        "FROM clans WHERE id = $1";
    const char *params[1] = { id_str };
    int lengths[1] = { (int)strlen(id_str) };
    int formats[1] = { 0 };

    PGresult *res = PQexecParams(g_main_conn, query, 1, NULL, params, lengths, formats, 0);
    int found = (PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) > 0);
    if (found && clan_out) {
        memset(clan_out, 0, sizeof(*clan_out));
        clan_out->id = atoi(PQgetvalue(res, 0, 0));
        strncpy(clan_out->guild_id, PQgetvalue(res, 0, 1), sizeof(clan_out->guild_id) - 1);
        strncpy(clan_out->name, PQgetvalue(res, 0, 2), sizeof(clan_out->name) - 1);
        strncpy(clan_out->description, PQgetvalue(res, 0, 3), sizeof(clan_out->description) - 1);
        strncpy(clan_out->picture_url, PQgetvalue(res, 0, 4), sizeof(clan_out->picture_url) - 1);
        strncpy(clan_out->owner_id, PQgetvalue(res, 0, 5), sizeof(clan_out->owner_id) - 1);
        strncpy(clan_out->role_id, PQgetvalue(res, 0, 6), sizeof(clan_out->role_id) - 1);
    }
    PQclear(res);
    pthread_mutex_unlock(&g_db_mutex);
    return found ? 0 : -1;
}

int db_clan_get_by_name(const char *name, clan_t *clan_out)
{
    pthread_mutex_lock(&g_db_mutex);
    g_main_conn = db_ensure_connection(g_main_conn);
    if (!g_main_conn) {
        pthread_mutex_unlock(&g_db_mutex);
        return -1;
    }

    const char *query =
        "SELECT id, guild_id, name, description, COALESCE(picture_url, ''), owner_id, role_id "
        "FROM clans WHERE LOWER(name) = LOWER($1)";
    const char *params[1] = { name };
    int lengths[1] = { (int)strlen(name) };
    int formats[1] = { 0 };

    PGresult *res = PQexecParams(g_main_conn, query, 1, NULL, params, lengths, formats, 0);
    int found = (PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) > 0);
    if (found && clan_out) {
        memset(clan_out, 0, sizeof(*clan_out));
        clan_out->id = atoi(PQgetvalue(res, 0, 0));
        strncpy(clan_out->guild_id, PQgetvalue(res, 0, 1), sizeof(clan_out->guild_id) - 1);
        strncpy(clan_out->name, PQgetvalue(res, 0, 2), sizeof(clan_out->name) - 1);
        strncpy(clan_out->description, PQgetvalue(res, 0, 3), sizeof(clan_out->description) - 1);
        strncpy(clan_out->picture_url, PQgetvalue(res, 0, 4), sizeof(clan_out->picture_url) - 1);
        strncpy(clan_out->owner_id, PQgetvalue(res, 0, 5), sizeof(clan_out->owner_id) - 1);
        strncpy(clan_out->role_id, PQgetvalue(res, 0, 6), sizeof(clan_out->role_id) - 1);
    }
    PQclear(res);
    pthread_mutex_unlock(&g_db_mutex);
    return found ? 0 : -1;
}

int db_clan_add_member(int clan_id, u64snowflake user_id, u64snowflake guild_id)
{
    pthread_mutex_lock(&g_db_mutex);
    g_main_conn = db_ensure_connection(g_main_conn);
    if (!g_main_conn) {
        pthread_mutex_unlock(&g_db_mutex);
        return -1;
    }

    char clan_id_str[16], user_str[32], guild_str[32];
    snprintf(clan_id_str, sizeof(clan_id_str), "%d", clan_id);
    snprintf(user_str, sizeof(user_str), "%llu", (unsigned long long)user_id);
    snprintf(guild_str, sizeof(guild_str), "%llu", (unsigned long long)guild_id);

    const char *query =
        "INSERT INTO clan_members (clan_id, user_id, guild_id) VALUES ($1, $2, $3) "
        "ON CONFLICT (user_id) DO UPDATE SET clan_id = EXCLUDED.clan_id";
    const char *params[3] = { clan_id_str, user_str, guild_str };
    int lengths[3] = { (int)strlen(clan_id_str), (int)strlen(user_str), (int)strlen(guild_str) };
    int formats[3] = { 0, 0, 0 };

    PGresult *res = PQexecParams(g_main_conn, query, 3, NULL, params, lengths, formats, 0);
    int ok = (PQresultStatus(res) == PGRES_COMMAND_OK);
    PQclear(res);
    pthread_mutex_unlock(&g_db_mutex);
    return ok ? 0 : -1;
}

int db_clan_create_invite(int clan_id, u64snowflake guild_id, u64snowflake inviter_id,
                          u64snowflake target_id, int *invite_id_out)
{
    pthread_mutex_lock(&g_db_mutex);
    g_main_conn = db_ensure_connection(g_main_conn);
    if (!g_main_conn) {
        pthread_mutex_unlock(&g_db_mutex);
        return -1;
    }

    char clan_id_str[16], guild_str[32], inviter_str[32], target_str[32];
    snprintf(clan_id_str, sizeof(clan_id_str), "%d", clan_id);
    snprintf(guild_str, sizeof(guild_str), "%llu", (unsigned long long)guild_id);
    snprintf(inviter_str, sizeof(inviter_str), "%llu", (unsigned long long)inviter_id);
    snprintf(target_str, sizeof(target_str), "%llu", (unsigned long long)target_id);

    const char *query =
        "INSERT INTO clan_invites (clan_id, guild_id, inviter_id, target_id, status) "
        "VALUES ($1, $2, $3, $4, 'pending') RETURNING id";
    const char *params[4] = { clan_id_str, guild_str, inviter_str, target_str };
    int lengths[4] = { (int)strlen(clan_id_str), (int)strlen(guild_str), (int)strlen(inviter_str), (int)strlen(target_str) };
    int formats[4] = { 0, 0, 0, 0 };

    PGresult *res = PQexecParams(g_main_conn, query, 4, NULL, params, lengths, formats, 0);
    int ok = (PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) > 0);
    if (ok && invite_id_out) {
        *invite_id_out = atoi(PQgetvalue(res, 0, 0));
    }
    PQclear(res);
    pthread_mutex_unlock(&g_db_mutex);
    return ok ? 0 : -1;
}

int db_clan_get_invite(int invite_id, clan_invite_t *invite_out)
{
    pthread_mutex_lock(&g_db_mutex);
    g_main_conn = db_ensure_connection(g_main_conn);
    if (!g_main_conn) {
        pthread_mutex_unlock(&g_db_mutex);
        return -1;
    }

    char id_str[16];
    snprintf(id_str, sizeof(id_str), "%d", invite_id);

    const char *query =
        "SELECT id, clan_id, guild_id, inviter_id, target_id, status "
        "FROM clan_invites WHERE id = $1";
    const char *params[1] = { id_str };
    int lengths[1] = { (int)strlen(id_str) };
    int formats[1] = { 0 };

    PGresult *res = PQexecParams(g_main_conn, query, 1, NULL, params, lengths, formats, 0);
    int found = (PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) > 0);
    if (found && invite_out) {
        memset(invite_out, 0, sizeof(*invite_out));
        invite_out->id = atoi(PQgetvalue(res, 0, 0));
        invite_out->clan_id = atoi(PQgetvalue(res, 0, 1));
        strncpy(invite_out->guild_id, PQgetvalue(res, 0, 2), sizeof(invite_out->guild_id) - 1);
        strncpy(invite_out->inviter_id, PQgetvalue(res, 0, 3), sizeof(invite_out->inviter_id) - 1);
        strncpy(invite_out->target_id, PQgetvalue(res, 0, 4), sizeof(invite_out->target_id) - 1);
        strncpy(invite_out->status, PQgetvalue(res, 0, 5), sizeof(invite_out->status) - 1);
    }
    PQclear(res);
    pthread_mutex_unlock(&g_db_mutex);
    return found ? 0 : -1;
}

int db_clan_update_invite_status(int invite_id, const char *status)
{
    pthread_mutex_lock(&g_db_mutex);
    g_main_conn = db_ensure_connection(g_main_conn);
    if (!g_main_conn) {
        pthread_mutex_unlock(&g_db_mutex);
        return -1;
    }

    char id_str[16];
    snprintf(id_str, sizeof(id_str), "%d", invite_id);

    const char *query = "UPDATE clan_invites SET status = $1 WHERE id = $2";
    const char *params[2] = { status, id_str };
    int lengths[2] = { (int)strlen(status), (int)strlen(id_str) };
    int formats[2] = { 0, 0 };

    PGresult *res = PQexecParams(g_main_conn, query, 2, NULL, params, lengths, formats, 0);
    int ok = (PQresultStatus(res) == PGRES_COMMAND_OK);
    PQclear(res);
    pthread_mutex_unlock(&g_db_mutex);
    return ok ? 0 : -1;
}

int db_temp_voice_register(u64snowflake channel_id, u64snowflake guild_id, u64snowflake owner_id)
{
    pthread_mutex_lock(&g_db_mutex);
    g_main_conn = db_ensure_connection(g_main_conn);
    if (!g_main_conn) {
        pthread_mutex_unlock(&g_db_mutex);
        return -1;
    }

    char ch_str[32], g_str[32], o_str[32];
    snprintf(ch_str, sizeof(ch_str), "%llu", (unsigned long long)channel_id);
    snprintf(g_str, sizeof(g_str), "%llu", (unsigned long long)guild_id);
    snprintf(o_str, sizeof(o_str), "%llu", (unsigned long long)owner_id);

    const char *query =
        "INSERT INTO temp_voice_channels (channel_id, guild_id, owner_id, is_private) "
        "VALUES ($1, $2, $3, 0) ON CONFLICT (channel_id) DO UPDATE SET owner_id = EXCLUDED.owner_id, is_private = EXCLUDED.is_private";
    const char *params[3] = { ch_str, g_str, o_str };
    int lengths[3] = { (int)strlen(ch_str), (int)strlen(g_str), (int)strlen(o_str) };
    int formats[3] = { 0, 0, 0 };

    PGresult *res = PQexecParams(g_main_conn, query, 3, NULL, params, lengths, formats, 0);
    int ok = (PQresultStatus(res) == PGRES_COMMAND_OK);
    PQclear(res);
    pthread_mutex_unlock(&g_db_mutex);
    return ok ? 0 : -1;
}

int db_temp_voice_get(u64snowflake channel_id, temp_voice_t *voice_out)
{
    pthread_mutex_lock(&g_db_mutex);
    g_main_conn = db_ensure_connection(g_main_conn);
    if (!g_main_conn) {
        pthread_mutex_unlock(&g_db_mutex);
        return -1;
    }

    char ch_str[32];
    snprintf(ch_str, sizeof(ch_str), "%llu", (unsigned long long)channel_id);

    const char *query = "SELECT channel_id, guild_id, owner_id, is_private FROM temp_voice_channels WHERE channel_id = $1";
    const char *params[1] = { ch_str };
    int lengths[1] = { (int)strlen(ch_str) };
    int formats[1] = { 0 };

    PGresult *res = PQexecParams(g_main_conn, query, 1, NULL, params, lengths, formats, 0);
    int found = (PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) > 0);
    if (found && voice_out) {
        memset(voice_out, 0, sizeof(*voice_out));
        strncpy(voice_out->channel_id, PQgetvalue(res, 0, 0), sizeof(voice_out->channel_id) - 1);
        strncpy(voice_out->guild_id, PQgetvalue(res, 0, 1), sizeof(voice_out->guild_id) - 1);
        strncpy(voice_out->owner_id, PQgetvalue(res, 0, 2), sizeof(voice_out->owner_id) - 1);
        voice_out->is_private = atoi(PQgetvalue(res, 0, 3));
    }
    PQclear(res);
    pthread_mutex_unlock(&g_db_mutex);
    return found ? 0 : -1;
}

int db_temp_voice_update_owner(u64snowflake channel_id, u64snowflake new_owner_id)
{
    pthread_mutex_lock(&g_db_mutex);
    g_main_conn = db_ensure_connection(g_main_conn);
    if (!g_main_conn) {
        pthread_mutex_unlock(&g_db_mutex);
        return -1;
    }

    char ch_str[32], o_str[32];
    snprintf(ch_str, sizeof(ch_str), "%llu", (unsigned long long)channel_id);
    snprintf(o_str, sizeof(o_str), "%llu", (unsigned long long)new_owner_id);

    const char *query = "UPDATE temp_voice_channels SET owner_id = $1 WHERE channel_id = $2";
    const char *params[2] = { o_str, ch_str };
    int lengths[2] = { (int)strlen(o_str), (int)strlen(ch_str) };
    int formats[2] = { 0, 0 };

    PGresult *res = PQexecParams(g_main_conn, query, 2, NULL, params, lengths, formats, 0);
    int ok = (PQresultStatus(res) == PGRES_COMMAND_OK);
    PQclear(res);
    pthread_mutex_unlock(&g_db_mutex);
    return ok ? 0 : -1;
}

int db_temp_voice_set_privacy(u64snowflake channel_id, int is_private)
{
    pthread_mutex_lock(&g_db_mutex);
    g_main_conn = db_ensure_connection(g_main_conn);
    if (!g_main_conn) {
        pthread_mutex_unlock(&g_db_mutex);
        return -1;
    }

    char ch_str[32], priv_str[8];
    snprintf(ch_str, sizeof(ch_str), "%llu", (unsigned long long)channel_id);
    snprintf(priv_str, sizeof(priv_str), "%d", is_private);

    const char *query = "UPDATE temp_voice_channels SET is_private = $1 WHERE channel_id = $2";
    const char *params[2] = { priv_str, ch_str };
    int lengths[2] = { (int)strlen(priv_str), (int)strlen(ch_str) };
    int formats[2] = { 0, 0 };

    PGresult *res = PQexecParams(g_main_conn, query, 2, NULL, params, lengths, formats, 0);
    int ok = (PQresultStatus(res) == PGRES_COMMAND_OK);
    PQclear(res);
    pthread_mutex_unlock(&g_db_mutex);
    return ok ? 0 : -1;
}

int db_temp_voice_delete(u64snowflake channel_id)
{
    pthread_mutex_lock(&g_db_mutex);
    g_main_conn = db_ensure_connection(g_main_conn);
    if (!g_main_conn) {
        pthread_mutex_unlock(&g_db_mutex);
        return -1;
    }

    char ch_str[32];
    snprintf(ch_str, sizeof(ch_str), "%llu", (unsigned long long)channel_id);

    const char *query = "DELETE FROM temp_voice_channels WHERE channel_id = $1";
    const char *params[1] = { ch_str };
    int lengths[1] = { (int)strlen(ch_str) };
    int formats[1] = { 0 };

    PGresult *res = PQexecParams(g_main_conn, query, 1, NULL, params, lengths, formats, 0);
    int ok = (PQresultStatus(res) == PGRES_COMMAND_OK);
    PQclear(res);
    pthread_mutex_unlock(&g_db_mutex);
    return ok ? 0 : -1;
}

