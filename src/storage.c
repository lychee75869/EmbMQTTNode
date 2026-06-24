#include "storage.h"
#include <sqlite3.h>
#include <pthread.h>

static sqlite3 *g_db = NULL;
static pthread_mutex_t g_db_mutex = PTHREAD_MUTEX_INITIALIZER;

#define SQL_CREATE \
    "CREATE TABLE IF NOT EXISTS sensor_data (" \
    "    id INTEGER PRIMARY KEY AUTOINCREMENT," \
    "    client_id TEXT NOT NULL," \
    "    timestamp_ms INTEGER NOT NULL," \
    "    temperature REAL NOT NULL," \
    "    humidity REAL NOT NULL," \
    "    pressure REAL NOT NULL" \
    ");"

int storage_init(const char *db_path)
{
    int rc = sqlite3_open(db_path, &g_db);
    if (rc != SQLITE_OK) {
        LOG_ERROR("sqlite3_open failed: %s", sqlite3_errmsg(g_db));
        return E_IO;
    }

    char *err = NULL;
    rc = sqlite3_exec(g_db, SQL_CREATE, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        LOG_ERROR("sqlite3_exec create table failed: %s", err);
        sqlite3_free(err);
        sqlite3_close(g_db);
        g_db = NULL;
        return E_IO;
    }

    LOG_INFO("storage init ok: %s", db_path);
    return E_OK;
}

int storage_save(const struct sensor_data *data, const char *client_id)
{
    if (!g_db || !data || !client_id) return E_INVAL;

    const char *sql = "INSERT INTO sensor_data (client_id, timestamp_ms, temperature, humidity, pressure) "
                      "VALUES (?, ?, ?, ?, ?);";
    sqlite3_stmt *stmt = NULL;

    pthread_mutex_lock(&g_db_mutex);
    int rc = sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) goto fail;

    sqlite3_bind_text(stmt, 1, client_id, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 2, data->timestamp_ms);
    sqlite3_bind_double(stmt, 3, data->temperature);
    sqlite3_bind_double(stmt, 4, data->humidity);
    sqlite3_bind_double(stmt, 5, data->pressure);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&g_db_mutex);

    if (rc != SQLITE_DONE) {
        LOG_ERROR("storage_save step failed: %d", rc);
        return E_IO;
    }
    return E_OK;

fail:
    if (stmt) sqlite3_finalize(stmt);
    pthread_mutex_unlock(&g_db_mutex);
    LOG_ERROR("storage_save prepare failed: %d", rc);
    return E_IO;
}

int storage_get_pending(struct sensor_data *out, int count)
{
    if (!g_db || !out || count <= 0) return E_INVAL;

    const char *sql = "SELECT timestamp_ms, temperature, humidity, pressure "
                      "FROM sensor_data ORDER BY timestamp_ms ASC LIMIT ?;";
    sqlite3_stmt *stmt = NULL;

    pthread_mutex_lock(&g_db_mutex);
    int rc = sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) goto fail;

    sqlite3_bind_int(stmt, 1, count);

    int n = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW && n < count) {
        out[n].timestamp_ms   = sqlite3_column_int64(stmt, 0);
        out[n].temperature    = sqlite3_column_double(stmt, 1);
        out[n].humidity       = sqlite3_column_double(stmt, 2);
        out[n].pressure       = sqlite3_column_double(stmt, 3);
        n++;
    }
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&g_db_mutex);

    return n;

fail:
    if (stmt) sqlite3_finalize(stmt);
    pthread_mutex_unlock(&g_db_mutex);
    LOG_ERROR("storage_get_pending prepare failed: %d", rc);
    return E_IO;
}

int storage_delete_sent(int64_t timestamp_ms)
{
    if (!g_db) return E_INVAL;

    const char *sql = "DELETE FROM sensor_data WHERE timestamp_ms <= ?;";
    sqlite3_stmt *stmt = NULL;

    pthread_mutex_lock(&g_db_mutex);
    int rc = sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) goto fail;

    sqlite3_bind_int64(stmt, 1, timestamp_ms);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&g_db_mutex);

    if (rc != SQLITE_DONE) {
        LOG_ERROR("storage_delete_sent step failed: %d", rc);
        return E_IO;
    }
    return E_OK;

fail:
    if (stmt) sqlite3_finalize(stmt);
    pthread_mutex_unlock(&g_db_mutex);
    LOG_ERROR("storage_delete_sent prepare failed: %d", rc);
    return E_IO;
}

void storage_close(void)
{
    if (g_db) {
        sqlite3_close(g_db);
        g_db = NULL;
    }
}
