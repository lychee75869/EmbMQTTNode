/*
 * storage.c
 * 本地 SQLite 缓存模块实现
 * 支持断网续传：离线时缓存传感器数据，网络恢复后自动补发
 * 线程安全（pthread_mutex）
 *
 * v1.2.4（P0-4 修复）：
 *   - sensor_data 表增加 source 列（local / modbus），含旧库 ALTER TABLE migration
 *   - storage_save 增加 source 入参，成功后回填 data->id（SQLite 自增主键）
 *   - storage_get_pending 返回 id + source，供补发线程精确删除与选择 topic
 *   - 新增 storage_delete_by_id：按主键精确删单条（发布成功才删，失败保留重试）
 *   - storage_delete_sent 保留为 legacy 接口，main.c 不再调用
 */
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
    "    pressure REAL NOT NULL," \
    "    source TEXT NOT NULL DEFAULT 'local'" \
    ");"

/* v1.2.4 之前旧库的表没有 source 列，需要 ALTER TABLE 补列 */
#define SQL_MIGRATE_ADD_SOURCE \
    "ALTER TABLE sensor_data ADD COLUMN source TEXT NOT NULL DEFAULT 'local'"

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

    /* ── migration：旧库表无 source 列时补列 ──
     * 通过 prepare 一句引用 source 的 SELECT 探测列是否存在
     * （LIMIT 0 不产生任何 IO），不存在则 ALTER TABLE 补列。 */
    sqlite3_stmt *ck = NULL;
    int has_source = 0;
    if (sqlite3_prepare_v2(g_db, "SELECT source FROM sensor_data LIMIT 0",
                           -1, &ck, NULL) == SQLITE_OK) {
        has_source = 1;
        sqlite3_finalize(ck);
    }
    if (!has_source) {
        rc = sqlite3_exec(g_db, SQL_MIGRATE_ADD_SOURCE, NULL, NULL, &err);
        if (rc != SQLITE_OK) {
            LOG_ERROR("migrate add source column failed: %s", err);
            sqlite3_free(err);
            sqlite3_close(g_db);
            g_db = NULL;
            return E_IO;
        }
        LOG_INFO("storage migrated: added source column to sensor_data");
    }

    LOG_INFO("storage init ok: %s", db_path);
    return E_OK;
}

int storage_save(const struct sensor_data *data, sensor_source_t source,
                 const char *client_id)
{
    if (!g_db || !data || !client_id) return E_INVAL;

    const char *sql = "INSERT INTO sensor_data "
                      "(client_id, timestamp_ms, temperature, humidity, pressure, source) "
                      "VALUES (?, ?, ?, ?, ?, ?);";
    sqlite3_stmt *stmt = NULL;

    pthread_mutex_lock(&g_db_mutex);
    int rc = sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) goto fail;

    sqlite3_bind_text(stmt, 1, client_id, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 2, data->timestamp_ms);
    sqlite3_bind_double(stmt, 3, data->temperature);
    sqlite3_bind_double(stmt, 4, data->humidity);
    sqlite3_bind_double(stmt, 5, data->pressure);
    sqlite3_bind_text(stmt, 6,
                      (source == SOURCE_MODBUS) ? "modbus" : "local",
                      -1, SQLITE_STATIC);

    rc = sqlite3_step(stmt);
    if (rc == SQLITE_DONE) {
        /* 回填自增主键，供调用方后续按 id 精确删除 */
        /* 注：入参为 const 指针，仅此处写回 id 字段（调用方均传非 const 栈对象） */
        ((struct sensor_data *)data)->id = sqlite3_last_insert_rowid(g_db);
    }
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

    const char *sql = "SELECT id, timestamp_ms, temperature, humidity, pressure, source "
                      "FROM sensor_data ORDER BY timestamp_ms ASC, id ASC LIMIT ?;";
    sqlite3_stmt *stmt = NULL;

    pthread_mutex_lock(&g_db_mutex);
    int rc = sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) goto fail;

    sqlite3_bind_int(stmt, 1, count);

    int n = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW && n < count) {
        out[n].id            = sqlite3_column_int64(stmt, 0);
        out[n].timestamp_ms  = sqlite3_column_int64(stmt, 1);
        out[n].temperature   = sqlite3_column_double(stmt, 2);
        out[n].humidity      = sqlite3_column_double(stmt, 3);
        out[n].pressure      = sqlite3_column_double(stmt, 4);
        const unsigned char *src = sqlite3_column_text(stmt, 5);
        out[n].source = (src && strcmp((const char *)src, "modbus") == 0)
                            ? SOURCE_MODBUS : SOURCE_LOCAL;
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

int storage_delete_by_id(int64_t id)
{
    if (!g_db || id <= 0) return E_INVAL;

    const char *sql = "DELETE FROM sensor_data WHERE id = ?;";
    sqlite3_stmt *stmt = NULL;

    pthread_mutex_lock(&g_db_mutex);
    int rc = sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) goto fail;

    sqlite3_bind_int64(stmt, 1, id);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&g_db_mutex);

    if (rc != SQLITE_DONE) {
        LOG_ERROR("storage_delete_by_id step failed: %d", rc);
        return E_IO;
    }
    return E_OK;

fail:
    if (stmt) sqlite3_finalize(stmt);
    pthread_mutex_unlock(&g_db_mutex);
    LOG_ERROR("storage_delete_by_id prepare failed: %d", rc);
    return E_IO;
}

/*
 * legacy：main.c 不再使用（无条件按时间戳删除会把发布失败的数据一并删掉，
 * 即 P0-4 bug）。保留仅供测试 / 全表清理等管理命令调用。
 */
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
