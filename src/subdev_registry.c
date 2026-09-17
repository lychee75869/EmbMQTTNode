/*
 * subdev_registry.c
 * 子设备注册表实现（docs/12 §5.1/§5.2）。
 *
 * 文件格式（INI 风格，与 config.c 一致：key = value，#/; 注释，[段] 跳过）：
 *   subdevice_N = <data_source>,<source_key>,<device_id>,<name>[,<service_id>]
 *     data_source: sensor | modbus（预留 ble）
 *     source_key : sensor→sensor_type(sht30/ads1115/mock)；modbus→slave_id(1-247)
 *     device_id  : 平台分配的子设备 deviceID（≤256）
 *     service_id : 可选，缺省 SensorData
 *
 * 校验规则（fail-closed，非法整条丢弃 + WARN）：
 *   - 条数 > SUBDEVICE_MAX(16) → 超出丢弃 + WARN
 *   - data_source ∉ {sensor, modbus} → 丢弃 + WARN
 *   - sensor: source_key 必须是合法 sensor_type；modbus: 1..247 整数 → 否则丢弃
 *   - device_id 非空、≤256、不含空格与 $ # +（MQTT 通配符）以及
 *     JSON 逸出字符 `"` `\` 与裸控制字符（T05 审计补）→ 否则丢弃
 *   - device_id 全表唯一；(data_source, source_key) 全表唯一 → 重复丢弃
 *   - service_id / name 含非法字符（同上集合）→ 回落缺省 + WARN
 *   - 文件不存在 / 0 条 → WARN + 空表（不 crash）
 */
#include "subdev_registry.h"

/* ─── 内部状态（加载后运行期只读，无需锁）────────────────── */
static struct subdev_entry g_entries[SUBDEVICE_MAX];
static int                 g_count = 0;

#define SUBDEV_SERVICE_ID_DEFAULT "SensorData"

/* trim：与 config.c 同款（去首尾空格/制表符，去行尾换行） */
static char *trim(char *str)
{
    char *end;
    while (*str == ' ' || *str == '\t') str++;
    if (*str == 0) return str;
    end = str + strlen(str) - 1;
    while (end > str && (*end == ' ' || *end == '\t' ||
                         *end == '\n' || *end == '\r'))
        end--;
    end[1] = '\0';
    return str;
}

/* 合法 sensor_type（大小写按现有命名：小写） */
static int valid_sensor_type(const char *t)
{
    return strcmp(t, "sht30") == 0 ||
           strcmp(t, "ads1115") == 0 ||
           strcmp(t, "mock") == 0;
}

/* device_id / service_id 的非法字符集：
 *   - 空白 与 MQTT 通配符分隔符 $ # +
 *   - JSON 字符串逸出字符 `"` `\` 与任意裸控制字符（< 0x20）
 *     （T05 审计补：device_id/service_id 会被插值进 $oc JSON payload） */
static int has_illegal_char(const char *s)
{
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        unsigned char c = *p;
        if (c < 0x20)                          /* 裸控制字符 */
            return 1;
        if (c == ' ' || c == '$' || c == '#' || c == '+')
            return 1;
        if (c == '"' || c == '\\')             /* JSON 逸出 */
            return 1;
    }
    return 0;
}

/* ─── 公开 API ─────────────────────────────────────────── */

int subdev_load(const char *path, struct subdev_entry *out, int max)
{
    g_count = 0;

    if (!path) {
        LOG_WARN("subdev: null path, empty registry");
        return 0;
    }

    FILE *fp = fopen(path, "r");
    if (!fp) {
        LOG_WARN("subdev: cannot open '%s': %s "
                 "(empty registry, subdevice feature degraded)",
                 path, strerror(errno));
        return 0;
    }

    int cap = (max < SUBDEVICE_MAX) ? max : SUBDEVICE_MAX;
    if (cap < 0) cap = 0;

    /* 行/值缓冲：需容纳最长条目
     * ds(15)+skey(63)+did(256)+name(63)+sid(31)+分隔符 ≈ 435，留裕量。 */
    char line[768];
    while (fgets(line, sizeof(line), fp)) {
        char *p = trim(line);
        if (*p == '\0' || *p == '#' || *p == ';') continue;
        if (*p == '[') continue;   /* 段标记：跳过 */

        char key[64]   = {0};
        char value[640] = {0};
        if (sscanf(p, "%63[^=]=%639[^\n]", key, value) != 2) continue;

        char *k = trim(key);
        char *v = trim(value);

        /* 只处理 subdevice_N 行 */
        if (strncmp(k, "subdevice_", 10) != 0) continue;

        /* 解析五段（service_id 可选） */
        char ds[16]                       = {0};
        char skey[64]                     = {0};
        char did[SUBDEV_DEVICE_ID_LEN + 8] = {0};
        char name[64]                     = {0};
        char sid[32]                      = {0};

        int matched = sscanf(v, "%15[^,],%63[^,],%259[^,],%63[^,],%31[^\n]",
                             ds, skey, did, name, sid);
        if (matched < 4) {
            LOG_WARN("subdev: '%s' invalid format, need "
                     "data_source,source_key,device_id,name[,service_id] "
                     "— entry dropped", k);
            continue;
        }

        struct subdev_entry e;
        memset(&e, 0, sizeof(e));

        /* ── data_source ── */
        if (strcmp(ds, "sensor") == 0) {
            e.src = SUBDEV_SENSOR;
        } else if (strcmp(ds, "modbus") == 0) {
            e.src = SUBDEV_MODBUS;
        } else {
            LOG_WARN("subdev: '%s' unknown data_source '%s' "
                     "(sensor|modbus), entry dropped", k, ds);
            continue;
        }

        /* ── source_key ── */
        if (e.src == SUBDEV_SENSOR) {
            if (!valid_sensor_type(skey)) {
                LOG_WARN("subdev: '%s' unknown sensor_type '%s' "
                         "(sht30|ads1115|mock), entry dropped", k, skey);
                continue;
            }
            e.slave_id = 0;
            strncpy(e.sensor_type, skey, sizeof(e.sensor_type) - 1);
        } else {
            char *endp = NULL;
            long sid_v = strtol(skey, &endp, 10);
            if (!endp || *endp != '\0' || sid_v < 1 || sid_v > 247) {
                LOG_WARN("subdev: '%s' modbus slave_id '%s' invalid "
                         "(need 1-247), entry dropped", k, skey);
                continue;
            }
            e.slave_id = (int)sid_v;
            e.sensor_type[0] = '\0';
        }

        /* ── device_id ── */
        size_t dlen = strlen(did);
        if (dlen == 0 || dlen > 256) {
            LOG_WARN("subdev: '%s' device_id length %zu invalid (1-256), "
                     "entry dropped", k, dlen);
            continue;
        }
        if (has_illegal_char(did)) {
            LOG_WARN("subdev: '%s' device_id contains illegal char "
                     "(space/$/#/+/\"/\\\\ or control), entry dropped", k);
            continue;
        }
        strncpy(e.device_id, did, sizeof(e.device_id) - 1);

        /* device_id 全表唯一 */
        int dup = 0;
        for (int i = 0; i < g_count; i++) {
            if (strcmp(g_entries[i].device_id, e.device_id) == 0) {
                dup = 1;
                break;
            }
        }
        if (dup) {
            LOG_WARN("subdev: '%s' device_id '%s' duplicated, entry dropped",
                     k, e.device_id);
            continue;
        }

        /* (data_source, source_key) 全表唯一 */
        dup = 0;
        for (int i = 0; i < g_count; i++) {
            if (g_entries[i].src != e.src) continue;
            if (e.src == SUBDEV_SENSOR) {
                if (strcmp(g_entries[i].sensor_type, e.sensor_type) == 0) {
                    dup = 1;
                    break;
                }
            } else {
                if (g_entries[i].slave_id == e.slave_id) {
                    dup = 1;
                    break;
                }
            }
        }
        if (dup) {
            LOG_WARN("subdev: '%s' duplicate (data_source, source_key), "
                     "entry dropped", k);
            continue;
        }

        /* ── name（展示名，日志/注册 payload 用；空或含非法字符则回落 device_id） ── */
        if (name[0] != '\0' && !has_illegal_char(name))
            strncpy(e.name, name, sizeof(e.name) - 1);
        else
            strncpy(e.name, did, sizeof(e.name) - 1);

        /* ── service_id（可选，缺省 SensorData；非法字符回落 + WARN） ── */
        if (matched >= 5 && sid[0] != '\0') {
            if (has_illegal_char(sid)) {
                LOG_WARN("subdev: '%s' service_id '%s' has illegal char, "
                         "falling back to '%s'", k, sid,
                         SUBDEV_SERVICE_ID_DEFAULT);
                strncpy(e.service_id, SUBDEV_SERVICE_ID_DEFAULT,
                        sizeof(e.service_id) - 1);
            } else {
                strncpy(e.service_id, sid, sizeof(e.service_id) - 1);
            }
        } else {
            strncpy(e.service_id, SUBDEV_SERVICE_ID_DEFAULT,
                    sizeof(e.service_id) - 1);
        }

        /* ── 容量检查（> SUBDEVICE_MAX 超出丢弃 + WARN） ── */
        if (g_count >= cap) {
            LOG_WARN("subdev: too many subdevices, max=%d, '%s' dropped",
                     cap, k);
            continue;
        }

        g_entries[g_count++] = e;
    }

    fclose(fp);

    if (g_count == 0) {
        LOG_WARN("subdev: no valid subdevice entries in '%s' "
                 "(gateway-only mode, subdevice feature degraded)", path);
    } else {
        LOG_INFO("subdev: loaded %d subdevice(s) from %s", g_count, path);
        for (int i = 0; i < g_count; i++) {
            const struct subdev_entry *e = &g_entries[i];
            if (e->src == SUBDEV_SENSOR)
                LOG_INFO("  subdev[%d]: sensor/%s -> %s (%s, svc=%s)",
                         i, e->sensor_type, e->device_id,
                         e->name, e->service_id);
            else
                LOG_INFO("  subdev[%d]: modbus/%d -> %s (%s, svc=%s)",
                         i, e->slave_id, e->device_id, e->name, e->service_id);
        }
    }

    /* 可选拷贝到调用方缓冲 */
    if (out && g_count > 0)
        memcpy(out, g_entries, (size_t)g_count * sizeof(struct subdev_entry));

    return g_count;
}

/* ─── 只读枚举（T04）───────────────────────────────────── */

int subdev_count(void)
{
    return g_count;
}

const struct subdev_entry *subdev_at(int idx)
{
    if (idx < 0 || idx >= g_count)
        return NULL;
    return &g_entries[idx];
}

/* ─── 查询 ─────────────────────────────────────────────── */

const struct subdev_entry *subdev_find_sensor(const char *sensor_type)
{
    if (!sensor_type) return NULL;
    for (int i = 0; i < g_count; i++) {
        if (g_entries[i].src == SUBDEV_SENSOR &&
            strcmp(g_entries[i].sensor_type, sensor_type) == 0)
            return &g_entries[i];
    }
    return NULL;
}

const struct subdev_entry *subdev_find_modbus(int slave_id)
{
    for (int i = 0; i < g_count; i++) {
        if (g_entries[i].src == SUBDEV_MODBUS &&
            g_entries[i].slave_id == slave_id)
            return &g_entries[i];
    }
    return NULL;
}
