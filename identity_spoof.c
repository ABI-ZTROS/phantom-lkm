/* SPDX-License-Identifier: GPL-2.0 */
/*
 * identity_spoof.c - AuroraSU 设备身份伪装模块
 *
 * 功能：
 * 1. 按应用/包名伪装设备身份标识
 * 2. 支持随机码生成和实时更换
 * 3. 多层伪装：MAC/Android ID/Build.SERIAL/IMEI等
 */

#include "phantom_lkm.h"
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/random.h>
#include <linux/uaccess.h>
#include <linux/fs.h>
#include <linux/proc_fs.h>

/* ==================== 常量定义 ==================== */

#define SPOOF_MAX_RULES         64
#define SPOOF_MAX_PACKAGE_LEN   128
#define SPOOF_MAX_VALUE_LEN     256
#define SPOOF_MAX_PATH_LEN      512

/* 身份标识类型 */
enum spoof_id_type {
    SPOOF_MAC_WIFI = 0,         /* WiFi MAC */
    SPOOF_MAC_BT,               /* 蓝牙 MAC */
    SPOOF_ANDROID_ID,           /* Settings.Secure.ANDROID_ID */
    SPOOF_BUILD_SERIAL,         /* Build.SERIAL */
    SPOOF_IMEI,                 /* IMEI */
    SPOOF_IMSI,                 /* IMSI */
    SPOOF_AD_ID,                /* Google Advertising ID */
    SPOOF_GSF_ID,               /* Google Services Framework ID */
    SPOOF_WIDEWINE_ID,          /* Widevine DRM ID */
    SPOOF_FINGERPRINT,          /* Build.FINGERPRINT */
    SPOOF_ID_COUNT
};

/* 伪装策略 */
enum spoof_strategy {
    SPOOF_FIXED = 0,            /* 固定值 */
    SPOOF_RANDOM,               /* 随机生成 */
    SPOOF_RANDOM_PER_APP,       /* 每应用随机 */
    SPOOF_ROTATE,               /* 定时轮换 */
};

/* 拦截路径类型 */
enum spoof_path_type {
    SPOOF_PATH_SYSFS = 0,       /* /sys/... 路径 */
    SPOOF_PATH_PROC,            /* /proc/... 路径 */
    SPOOF_PATH_FILE,            /* 普通文件 */
};

/* ==================== 数据结构 ==================== */

/* 单条伪装规则 */
struct spoof_rule {
    struct list_head    list;
    int                 id;                     /* 规则ID */
    char                package_name[SPOOF_MAX_PACKAGE_LEN]; /* 目标包名 */
    enum spoof_id_type  id_type;                /* 标识类型 */
    enum spoof_strategy strategy;               /* 伪装策略 */
    char                fake_value[SPOOF_MAX_VALUE_LEN]; /* 固定伪装值 */
    char                current_value[SPOOF_MAX_VALUE_LEN]; /* 当前值 */
    u64                 last_rotate_time;       /* 最后轮换时间 */
    u32                 rotate_interval_sec;    /* 轮换间隔 */
    bool                enabled;                /* 是否启用 */
};

/* 身份标识元数据 */
struct spoof_id_meta {
    enum spoof_id_type  type;
    const char          *name;                  /* 标识名称 */
    const char          *description;           /* 描述 */
    /* 可拦截的文件路径列表 */
    const char          *paths[8];
};

/* 全局状态 */
static struct list_head         spoof_rules_list;
static DEFINE_MUTEX(spoof_rules_mutex);
static atomic_t                 spoof_next_id;
static bool                     spoof_enabled = true;

/* sysfs kobject */
static struct kobject *kobj_spoof;

/* 身份标识元数据表 */
static const struct spoof_id_meta spoof_metas[] = {
    [SPOOF_MAC_WIFI] = {
        .type = SPOOF_MAC_WIFI,
        .name = "MAC_WIFI",
        .description = "WiFi MAC Address",
        .paths = {
            "/sys/class/net/wlan0/address",
            "/sys/class/net/wifi0/address",
            "/sys/class/net/eth0/address",
            NULL
        }
    },
    [SPOOF_MAC_BT] = {
        .type = SPOOF_MAC_BT,
        .name = "MAC_BT",
        .description = "Bluetooth MAC Address",
        .paths = {
            "/sys/class/bluetooth/hci0/address",
            "/data/misc/bluedroid/bt_config.conf",
            NULL
        }
    },
    [SPOOF_ANDROID_ID] = {
        .type = SPOOF_ANDROID_ID,
        .name = "ANDROID_ID",
        .description = "Android Settings Secure ID",
        .paths = {
            "/data/system/users/0/settings_secure.xml",
            NULL
        }
    },
    [SPOOF_BUILD_SERIAL] = {
        .type = SPOOF_BUILD_SERIAL,
        .name = "BUILD_SERIAL",
        .description = "Build Serial Number",
        .paths = {
            "/sys/class/android_usb/android0/iSerial",
            "/proc/cpuinfo",
            NULL
        }
    },
    [SPOOF_IMEI] = {
        .type = SPOOF_IMEI,
        .name = "IMEI",
        .description = "Device IMEI",
        .paths = {
            NULL
        }
    },
    [SPOOF_IMSI] = {
        .type = SPOOF_IMSI,
        .name = "IMSI",
        .description = "SIM IMSI",
        .paths = {
            NULL
        }
    },
    [SPOOF_AD_ID] = {
        .type = SPOOF_AD_ID,
        .name = "AD_ID",
        .description = "Google Advertising ID",
        .paths = {
            "/data/data/com.google.android.gms/shared_prefs/AdvertisingIdClient.xml",
            NULL
        }
    },
    [SPOOF_GSF_ID] = {
        .type = SPOOF_GSF_ID,
        .name = "GSF_ID",
        .description = "Google Services Framework ID",
        .paths = {
            NULL
        }
    },
    [SPOOF_WIDEWINE_ID] = {
        .type = SPOOF_WIDEWINE_ID,
        .name = "WIDEWINE",
        .description = "Widevine DRM ID",
        .paths = {
            NULL
        }
    },
    [SPOOF_FINGERPRINT] = {
        .type = SPOOF_FINGERPRINT,
        .name = "FINGERPRINT",
        .description = "Build Fingerprint",
        .paths = {
            "/system/build.prop",
            NULL
        }
    },
};

/* ==================== 随机值生成 ==================== */

/**
 * generate_random_mac - 生成随机MAC地址
 */
static void generate_random_mac(char *buf, size_t len)
{
    u8 mac[6];
    get_random_bytes(mac, 6);
    /* 设置本地管理位，避免与真实MAC冲突 */
    mac[0] = (mac[0] & 0xfc) | 0x02;
    scnprintf(buf, len, "%02x:%02x:%02x:%02x:%02x:%02x",
              mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

/**
 * generate_random_android_id - 生成随机Android ID
 */
static void generate_random_android_id(char *buf, size_t len)
{
    u8 id[8];
    get_random_bytes(id, 8);
    scnprintf(buf, len, "%02x%02x%02x%02x%02x%02x%02x%02x",
              id[0], id[1], id[2], id[3], id[4], id[5], id[6], id[7]);
}

/**
 * generate_random_imei - 生成随机IMEI
 */
static void generate_random_imei(char *buf, size_t len)
{
    int i;
    u8 digit;
    char imei[16];

    /* IMEI: 15位数字 */
    get_random_bytes(&digit, 1);
    imei[0] = '3'; /* 通常以35开头 */
    imei[1] = '5';

    for (i = 2; i < 14; i++) {
        get_random_bytes(&digit, 1);
        imei[i] = '0' + (digit % 10);
    }

    /* Luhn校验位（简化：随机生成） */
    get_random_bytes(&digit, 1);
    imei[14] = '0' + (digit % 10);
    imei[15] = '\0';

    strscpy(buf, imei, len);
}

/**
 * generate_random_serial - 生成随机序列号
 */
static void generate_random_serial(char *buf, size_t len)
{
    u8 bytes[8];
    int i;
    get_random_bytes(bytes, 8);
    for (i = 0; i < 8; i++) {
        bytes[i] = 'A' + (bytes[i] % 26);
    }
    scnprintf(buf, len, "%c%c%c%c%c%c%c%c",
              bytes[0], bytes[1], bytes[2], bytes[3],
              bytes[4], bytes[5], bytes[6], bytes[7]);
}

/**
 * generate_random_ad_id - 生成随机广告ID
 */
static void generate_random_ad_id(char *buf, size_t len)
{
    u8 bytes[16];
    get_random_bytes(bytes, 16);
    scnprintf(buf, len,
              "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
              bytes[0], bytes[1], bytes[2], bytes[3],
              bytes[4], bytes[5], bytes[6], bytes[7],
              bytes[8], bytes[9], bytes[10], bytes[11],
              bytes[12], bytes[13], bytes[14], bytes[15]);
}

/**
 * generate_fake_value - 根据类型生成伪装值
 */
static void generate_fake_value(enum spoof_id_type type, char *buf, size_t len)
{
    switch (type) {
    case SPOOF_MAC_WIFI:
    case SPOOF_MAC_BT:
        generate_random_mac(buf, len);
        break;
    case SPOOF_ANDROID_ID:
        generate_random_android_id(buf, len);
        break;
    case SPOOF_BUILD_SERIAL:
        generate_random_serial(buf, len);
        break;
    case SPOOF_IMEI:
        generate_random_imei(buf, len);
        break;
    case SPOOF_AD_ID:
        generate_random_ad_id(buf, len);
        break;
    default:
        /* 其他类型用随机16进制 */
        {
            u8 bytes[16];
            get_random_bytes(bytes, 16);
            scnprintf(buf, len,
                      "%02x%02x%02x%02x%02x%02x%02x%02x",
                      bytes[0], bytes[1], bytes[2], bytes[3],
                      bytes[4], bytes[5], bytes[6], bytes[7]);
        }
        break;
    }
}

/* ==================== 规则管理 ==================== */

/**
 * spoof_find_rule - 查找规则
 */
static struct spoof_rule *spoof_find_rule(int id)
{
    struct spoof_rule *rule;

    mutex_lock(&spoof_rules_mutex);
    list_for_each_entry(rule, &spoof_rules_list, list) {
        if (rule->id == id) {
            mutex_unlock(&spoof_rules_mutex);
            return rule;
        }
    }
    mutex_unlock(&spoof_rules_mutex);

    return NULL;
}

/**
 * spoof_find_rule_by_package - 按包名和类型查找规则
 */
static struct spoof_rule *spoof_find_rule_by_package(
    const char *package_name, enum spoof_id_type type)
{
    struct spoof_rule *rule;

    mutex_lock(&spoof_rules_mutex);
    list_for_each_entry(rule, &spoof_rules_list, list) {
        if (rule->enabled &&
            strcmp(rule->package_name, package_name) == 0 &&
            rule->id_type == type) {
            mutex_unlock(&spoof_rules_mutex);
            return rule;
        }
    }
    mutex_unlock(&spoof_rules_mutex);

    return NULL;
}

/**
 * spoof_add_rule - 添加伪装规则
 */
int spoof_add_rule(const char *package_name, enum spoof_id_type type,
                    enum spoof_strategy strategy, const char *fake_value,
                    u32 rotate_interval_sec)
{
    struct spoof_rule *rule;

    if (!package_name || strlen(package_name) >= SPOOF_MAX_PACKAGE_LEN)
        return -EINVAL;

    if (type >= SPOOF_ID_COUNT)
        return -EINVAL;

    /* 检查是否已存在 */
    rule = spoof_find_rule_by_package(package_name, type);
    if (rule) {
        /* 更新现有规则 */
        mutex_lock(&spoof_rules_mutex);
        rule->strategy = strategy;
        if (fake_value)
            strscpy(rule->fake_value, fake_value, sizeof(rule->fake_value));
        rule->rotate_interval_sec = rotate_interval_sec;
        rule->last_rotate_time = ktime_get_real_seconds();
        rule->enabled = true;

        /* 生成当前值 */
        if (strategy == SPOOF_FIXED && fake_value) {
            strscpy(rule->current_value, fake_value, sizeof(rule->current_value));
        } else {
            generate_fake_value(type, rule->current_value,
                                sizeof(rule->current_value));
        }
        mutex_unlock(&spoof_rules_mutex);

        vfs_trace("spoof rule updated: id=%d pkg=%s type=%s",
                  rule->id, package_name, spoof_metas[type].name);
        return rule->id;
    }

    /* 创建新规则 */
    rule = kzalloc(sizeof(*rule), GFP_KERNEL);
    if (!rule)
        return -ENOMEM;

    rule->id = atomic_inc_return(&spoof_next_id);
    strscpy(rule->package_name, package_name, sizeof(rule->package_name));
    rule->id_type = type;
    rule->strategy = strategy;
    if (fake_value)
        strscpy(rule->fake_value, fake_value, sizeof(rule->fake_value));
    rule->rotate_interval_sec = rotate_interval_sec;
    rule->last_rotate_time = ktime_get_real_seconds();
    rule->enabled = true;

    /* 生成初始值 */
    if (strategy == SPOOF_FIXED && fake_value) {
        strscpy(rule->current_value, fake_value, sizeof(rule->current_value));
    } else {
        generate_fake_value(type, rule->current_value,
                            sizeof(rule->current_value));
    }

    mutex_lock(&spoof_rules_mutex);
    list_add_tail(&rule->list, &spoof_rules_list);
    mutex_unlock(&spoof_rules_mutex);

    vfs_trace("spoof rule added: id=%d pkg=%s type=%s strategy=%d",
              rule->id, package_name, spoof_metas[type].name, strategy);

    return rule->id;
}

/**
 * spoof_remove_rule - 删除规则
 */
int spoof_remove_rule(int id)
{
    struct spoof_rule *rule;

    mutex_lock(&spoof_rules_mutex);
    list_for_each_entry(rule, &spoof_rules_list, list) {
        if (rule->id == id) {
            list_del(&rule->list);
            mutex_unlock(&spoof_rules_mutex);
            kfree(rule);
            vfs_trace("spoof rule removed: id=%d", id);
            return 0;
        }
    }
    mutex_unlock(&spoof_rules_mutex);

    return -ENOENT;
}

/**
 * spoof_rotate_value - 轮换伪装值
 */
void spoof_rotate_value(struct spoof_rule *rule)
{
    if (!rule || rule->strategy != SPOOF_ROTATE)
        return;

    generate_fake_value(rule->id_type, rule->current_value,
                        sizeof(rule->current_value));
    rule->last_rotate_time = ktime_get_real_seconds();

    vfs_trace("spoof value rotated: id=%d pkg=%s new=%s",
              rule->id, rule->package_name, rule->current_value);
}

/**
 * spoof_check_rotate - 检查是否需要轮换
 */
void spoof_check_rotate(void)
{
    struct spoof_rule *rule;
    u64 now = ktime_get_real_seconds();

    mutex_lock(&spoof_rules_mutex);
    list_for_each_entry(rule, &spoof_rules_list, list) {
        if (rule->enabled && rule->strategy == SPOOF_ROTATE &&
            rule->rotate_interval_sec > 0 &&
            (now - rule->last_rotate_time) >= rule->rotate_interval_sec) {
            mutex_unlock(&spoof_rules_mutex);
            spoof_rotate_value(rule);
            mutex_lock(&spoof_rules_mutex);
        }
    }
    mutex_unlock(&spoof_rules_mutex);
}

/* ==================== 拦截核心 ==================== */

/**
 * spoof_match_path - 检查路径是否匹配某个标识类型
 */
static enum spoof_id_type spoof_match_path(const char *path)
{
    int i, j;

    for (i = 0; i < SPOOF_ID_COUNT; i++) {
        for (j = 0; j < 8 && spoof_metas[i].paths[j]; j++) {
            if (strcmp(path, spoof_metas[i].paths[j]) == 0)
                return i;
        }
    }

    return SPOOF_ID_COUNT; /* 不匹配 */
}

/**
 * spoof_get_current_uid - 获取当前进程的UID（用于匹配包名）
 *
 * 简化实现：通过 /proc/[pid]/cmdline 获取包名
 * 实际实现可能需要更复杂的包名解析
 */
static int spoof_get_package_name(char *buf, size_t len)
{
    struct task_struct *task = current;
    const char *comm = task->comm;

    /* 简化：使用进程名作为包名标识 */
    /* 实际应该通过 Binder 或 /proc/[pid]/cmdline 解析 */
    strscpy(buf, comm, len);
    return 0;
}

/**
 * spoof_intercept_read - 拦截读取操作
 * @path: 文件路径
 * @buf: 输出缓冲区
 * @len: 缓冲区长度
 * @return: >0=返回伪装值, 0=不拦截, <0=错误
 *
 * 此函数在 VFS read 路径中被调用
 */
int spoof_intercept_read(const char *path, char *buf, size_t len)
{
    enum spoof_id_type id_type;
    struct spoof_rule *rule;
    char package_name[SPOOF_MAX_PACKAGE_LEN];

    if (!spoof_enabled)
        return 0;

    /* 检查路径是否匹配 */
    id_type = spoof_match_path(path);
    if (id_type >= SPOOF_ID_COUNT)
        return 0;

    /* 获取当前包名 */
    if (spoof_get_package_name(package_name, sizeof(package_name)) != 0)
        return 0;

    /* 查找规则 */
    rule = spoof_find_rule_by_package(package_name, id_type);
    if (!rule)
        return 0;

    /* 检查是否需要轮换 */
    if (rule->strategy == SPOOF_ROTATE && rule->rotate_interval_sec > 0) {
        u64 now = ktime_get_real_seconds();
        if ((now - rule->last_rotate_time) >= rule->rotate_interval_sec) {
            spoof_rotate_value(rule);
        }
    }

    /* 返回伪装值 */
    strscpy(buf, rule->current_value, len);

    vfs_trace_level(3, "spoof: %s -> %s for %s",
                    path, rule->current_value, package_name);

    return strlen(buf);
}

/* ==================== sysfs 接口 ==================== */

/* spoof/rules - 显示所有规则 */
static ssize_t spoof_rules_show(struct kobject *kobj,
                                 struct kobj_attribute *attr, char *buf)
{
    struct spoof_rule *rule;
    int len = 0;

    len += scnprintf(buf + len, PAGE_SIZE - len,
        "id:package:type:strategy:value:enabled\n");

    mutex_lock(&spoof_rules_mutex);
    list_for_each_entry(rule, &spoof_rules_list, list) {
        len += scnprintf(buf + len, PAGE_SIZE - len,
            "%d:%s:%s:%d:%s:%d\n",
            rule->id,
            rule->package_name,
            spoof_metas[rule->id_type].name,
            rule->strategy,
            rule->current_value,
            rule->enabled ? 1 : 0);
    }
    mutex_unlock(&spoof_rules_mutex);

    return len;
}

/* spoof/add - 添加规则 (写入 "package:type:strategy:value:interval") */
static ssize_t spoof_add_store(struct kobject *kobj,
                                struct kobj_attribute *attr,
                                const char *buf, size_t count)
{
    char package[SPOOF_MAX_PACKAGE_LEN];
    char type_str[32];
    char value[SPOOF_MAX_VALUE_LEN];
    int strategy, type, interval;

    if (sscanf(buf, "%127[^:]:%31[^:]:%d:%255[^:]:%d",
               package, type_str, &strategy, value, &interval) < 4)
        return -EINVAL;

    /* 解析类型 */
    for (type = 0; type < SPOOF_ID_COUNT; type++) {
        if (strcmp(type_str, spoof_metas[type].name) == 0)
            break;
    }
    if (type >= SPOOF_ID_COUNT)
        return -EINVAL;

    if (spoof_add_rule(package, type, strategy, value, interval) < 0)
        return -EINVAL;

    return count;
}

/* spoof/remove - 删除规则 (写入 "id") */
static ssize_t spoof_remove_store(struct kobject *kobj,
                                   struct kobj_attribute *attr,
                                   const char *buf, size_t count)
{
    int id;
    if (kstrtoint(buf, 10, &id) != 0)
        return -EINVAL;

    if (spoof_remove_rule(id) != 0)
        return -ENOENT;

    return count;
}

/* spoof/enabled - 总开关 */
static ssize_t spoof_enabled_show(struct kobject *kobj,
                                   struct kobj_attribute *attr, char *buf)
{
    return scnprintf(buf, PAGE_SIZE, "%d\n", spoof_enabled ? 1 : 0);
}

static ssize_t spoof_enabled_store(struct kobject *kobj,
                                    struct kobj_attribute *attr,
                                    const char *buf, size_t count)
{
    int val;
    if (kstrtoint(buf, 10, &val) != 0)
        return -EINVAL;
    spoof_enabled = val ? true : false;
    return count;
}

/* spoof/rotate - 手动轮换所有 ROTATE 规则 (写入触发) */
static ssize_t spoof_rotate_store(struct kobject *kobj,
                                   struct kobj_attribute *attr,
                                   const char *buf, size_t count)
{
    struct spoof_rule *rule;

    mutex_lock(&spoof_rules_mutex);
    list_for_each_entry(rule, &spoof_rules_list, list) {
        if (rule->enabled && rule->strategy == SPOOF_ROTATE) {
            mutex_unlock(&spoof_rules_mutex);
            spoof_rotate_value(rule);
            mutex_lock(&spoof_rules_mutex);
        }
    }
    mutex_unlock(&spoof_rules_mutex);

    vfs_trace("spoof: manual rotate triggered");
    return count;
}

/* sysfs 属性 */
static struct kobj_attribute spoof_rules_attr =
    __ATTR(rules, 0444, spoof_rules_show, NULL);

static struct kobj_attribute spoof_add_attr =
    __ATTR(add, 0200, NULL, spoof_add_store);

static struct kobj_attribute spoof_remove_attr =
    __ATTR(remove, 0200, NULL, spoof_remove_store);

static struct kobj_attribute spoof_enabled_attr =
    __ATTR(enabled, 0644, spoof_enabled_show, spoof_enabled_store);

static struct kobj_attribute spoof_rotate_attr =
    __ATTR(rotate, 0200, NULL, spoof_rotate_store);

static struct attribute *spoof_attrs[] = {
    &spoof_rules_attr.attr,
    &spoof_add_attr.attr,
    &spoof_remove_attr.attr,
    &spoof_enabled_attr.attr,
    &spoof_rotate_attr.attr,
    NULL
};

static struct attribute_group spoof_attr_group = {
    .attrs = spoof_attrs,
};

/* ==================== 初始化/退出 ==================== */

/**
 * identity_spoof_init - 初始化身份伪装模块
 */
int identity_spoof_init(void)
{
    int ret;

    INIT_LIST_HEAD(&spoof_rules_list);
    atomic_set(&spoof_next_id, 0);
    spoof_enabled = true;

    /* 创建 sysfs */
    kobj_spoof = kobject_create_and_add("spoof", g_ctx.kobj_root);
    if (!kobj_spoof) {
        vfs_trace("identity_spoof: failed to create kobject");
        return -ENOMEM;
    }

    ret = sysfs_create_group(kobj_spoof, &spoof_attr_group);
    if (ret) {
        vfs_trace("identity_spoof: failed to create sysfs: %d", ret);
        kobject_put(kobj_spoof);
        return ret;
    }

    vfs_trace("identity_spoof: initialized");
    return 0;
}

/**
 * identity_spoof_exit - 退出身份伪装模块
 */
void identity_spoof_exit(void)
{
    struct spoof_rule *rule, *tmp;

    /* 清理所有规则 */
    mutex_lock(&spoof_rules_mutex);
    list_for_each_entry_safe(rule, tmp, &spoof_rules_list, list) {
        list_del(&rule->list);
        kfree(rule);
    }
    mutex_unlock(&spoof_rules_mutex);

    /* 注销 sysfs */
    if (kobj_spoof) {
        sysfs_remove_group(kobj_spoof, &spoof_attr_group);
        kobject_put(kobj_spoof);
        kobj_spoof = NULL;
    }

    vfs_trace("identity_spoof: exited");
}
