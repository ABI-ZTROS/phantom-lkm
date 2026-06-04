/* SPDX-License-Identifier: GPL-2.0 */
/*
 * security_audit.c - AuroraSU 系统安全审计模块
 *
 * 功能:
 * 1. Shell 脚本执行统计 - 追踪 sh/bash/mksh 执行次数、来源、参数
 * 2. 设备分区保护 - 监控关键分区完整性
 */

#include "phantom_lkm.h"
#include <linux/security.h>
#include <linux/cred.h>
#include <linux/fs.h>
#include <linux/namei.h>
#include <linux/dcache.h>
#include <linux/uaccess.h>
#include <linux/time.h>
#include <linux/sched/signal.h>

/* ==================== 常量定义 ==================== */

#define AUDIT_MAX_SHELL_ENTRIES     256     /* 最大Shell执行记录数 */
#define AUDIT_MAX_PATH_LEN          512
#define AUDIT_MAX_CMDLINE_LEN        1024
#define AUDIT_MAX_PARTITIONS         16      /* 最大监控分区数 */
#define AUDIT_HASH_LEN               64      /* SHA256 hex长度 */

/* 受保护的分区列表 */
static const char *protected_partitions[] = {
    "/system",
    "/vendor",
    "/product",
    "/odm",
    "/boot",
    "/dtb",
    "/vbmeta",
    "/recovery",
    NULL
};

/* Shell 解释器列表 */
static const char *shell_interpreters[] = {
    "sh",
    "bash",
    "mksh",
    "zsh",
    "dash",
    "/system/bin/sh",
    "/system/bin/bash",
    "/system/bin/mksh",
    "/system/bin/dash",
    "/vendor/bin/sh",
    "/data/data/com.termux/files/usr/bin/sh",
    "/data/data/com.termux/files/usr/bin/bash",
    NULL
};

/* ==================== 数据结构 ==================== */

/* Shell 执行记录 */
struct shell_exec_entry {
    struct list_head    list;
    pid_t               caller_pid;         /* 调用者 PID */
    uid_t               caller_uid;         /* 调用者 UID */
    pid_t               shell_pid;          /* Shell 进程 PID */
    char                interpreter[128];    /* 解释器路径 */
    char                script_path[AUDIT_MAX_PATH_LEN];  /* 脚本路径 */
    char                caller_name[128];   /* 调用者进程名 */
    u64                 timestamp;          /* 执行时间戳 */
    bool                is_interactive;     /* 是否交互式 */
};

/* Shell 执行统计 */
struct shell_exec_stats {
    atomic64_t          total_exec_count;       /* 总执行次数 */
    atomic64_t          script_exec_count;       /* 脚本执行次数 */
    atomic64_t          interactive_count;       /* 交互式执行次数 */
    atomic64_t          denied_count;            /* 被拒绝次数 */
    u64                 last_exec_timestamp;      /* 最后执行时间 */
    char                last_interpreter[128];   /* 最后使用的解释器 */
    char                last_caller[128];         /* 最后的调用者 */
};

/* 分区监控条目 */
struct partition_monitor {
    char                mount_point[128];     /* 挂载点 */
    char                device_path[256];     /* 设备路径 */
    char                expected_hash[AUDIT_HASH_LEN]; /* 基准哈希 */
    char                current_hash[AUDIT_HASH_LEN];  /* 当前哈希 */
    bool                is_protected;         /* 是否启用保护 */
    bool                is_modified;          /* 是否被修改 */
    u64                 last_check_time;      /* 最后检查时间 */
    atomic64_t          modification_count;   /* 修改次数 */
};

/* 分区保护策略 */
struct partition_policy {
    bool                enabled;              /* 分区保护总开关 */
    bool                auto_reject;          /* 自动拒绝写入 */
    bool                alert_only;           /* 仅告警不拦截 */
    unsigned int        check_interval_sec;   /* 检查间隔（秒） */
};

/* ==================== 全局状态 ==================== */

static struct shell_exec_stats shell_stats;
static struct list_head        shell_exec_list;
static DEFINE_SPINLOCK(shell_list_lock);
static unsigned int            shell_list_count;

static struct partition_monitor    partitions[AUDIT_MAX_PARTITIONS];
static unsigned int                partition_count;
static struct partition_policy     part_policy;
static DEFINE_MUTEX(part_mutex);

/* sysfs kobject */
static struct kobject *kobj_audit;

/* ==================== Shell 执行追踪 ==================== */

/**
 * is_shell_interpreter - 检查是否是 Shell 解释器
 */
static bool is_shell_interpreter(const char *path)
{
    int i;
    if (!path)
        return false;

    for (i = 0; shell_interpreters[i] != NULL; i++) {
        if (strcmp(path, shell_interpreters[i]) == 0)
            return true;
    }

    /* 检查路径结尾 */
    if (strstr(path, "/sh") || strstr(path, "/bash") ||
        strstr(path, "/mksh") || strstr(path, "/zsh") ||
        strstr(path, "/dash"))
        return true;

    return false;
}

/**
 * shell_audit_record_exec - 记录一次 Shell 执行
 * @caller_pid: 调用者PID
 * @caller_uid: 调用者UID
 * @interpreter: 解释器路径
 * @script_path: 脚本路径（交互式为空）
 * @is_interactive: 是否交互式
 */
void shell_audit_record_exec(pid_t caller_pid, uid_t caller_uid,
                              const char *interpreter,
                              const char *script_path,
                              bool is_interactive)
{
    struct shell_exec_entry *entry;
    unsigned long flags;

    /* 更新统计 */
    atomic64_inc(&shell_stats.total_exec_count);
    if (is_interactive)
        atomic64_inc(&shell_stats.interactive_count);
    else
        atomic64_inc(&shell_stats.script_exec_count);

    shell_stats.last_exec_timestamp = ktime_get_real_seconds();
    if (interpreter)
        strscpy(shell_stats.last_interpreter, interpreter,
                sizeof(shell_stats.last_interpreter));

    /* 检查日志级别 */
    if (g_ctx.policy.log_level < 2)
        return;

    /* 分配记录 */
    entry = kzalloc(sizeof(*entry), GFP_ATOMIC);
    if (!entry)
        return;

    entry->caller_pid = caller_pid;
    entry->caller_uid = caller_uid;
    entry->shell_pid = current->pid;
    entry->timestamp = ktime_get_real_seconds();
    entry->is_interactive = is_interactive;

    if (interpreter)
        strscpy(entry->interpreter, interpreter, sizeof(entry->interpreter));
    if (script_path)
        strscpy(entry->script_path, script_path, sizeof(entry->script_path));

    /* 获取调用者进程名 */
    if (current->parent)
        strscpy(entry->caller_name, current->parent->comm,
                sizeof(entry->caller_name));

    /* 加入链表（FIFO，超限删除最老的） */
    spin_lock_irqsave(&shell_list_lock, flags);
    list_add_tail(&entry->list, &shell_exec_list);
    shell_list_count++;
    while (shell_list_count > AUDIT_MAX_SHELL_ENTRIES) {
        struct shell_exec_entry *oldest =
            list_first_entry(&shell_exec_list, struct shell_exec_entry, list);
        list_del(&oldest->list);
        kfree(oldest);
        shell_list_count--;
    }
    spin_unlock_irqrestore(&shell_list_lock, flags);

    vfs_trace_level(2, "shell_exec: caller=%d uid=%d interp=%s script=%s",
                    caller_pid, caller_uid,
                    interpreter ? interpreter : "?",
                    script_path ? script_path : "(interactive)");
}

/**
 * shell_audit_get_stats - 获取Shell执行统计
 */
int shell_audit_get_stats(char *buf, size_t size)
{
    int len = 0;

    len += scnprintf(buf + len, size - len,
        "total: %lld\n"
        "scripts: %lld\n"
        "interactive: %lld\n"
        "denied: %lld\n"
        "last_interpreter: %s\n"
        "last_caller: %s\n"
        "last_timestamp: %llu\n",
        atomic64_read(&shell_stats.total_exec_count),
        atomic64_read(&shell_stats.script_exec_count),
        atomic64_read(&shell_stats.interactive_count),
        atomic64_read(&shell_stats.denied_count),
        shell_stats.last_interpreter,
        shell_stats.last_caller,
        shell_stats.last_exec_timestamp);

    return len;
}

/**
 * shell_audit_get_recent - 获取最近的Shell执行记录
 */
int shell_audit_get_recent(char *buf, size_t size, int count)
{
    struct shell_exec_entry *entry;
    unsigned long flags;
    int len = 0;
    int printed = 0;

    spin_lock_irqsave(&shell_list_lock, flags);

    /* 从最新到最老遍历 */
    list_for_each_entry_reverse(entry, &shell_exec_list, list) {
        if (printed >= count)
            break;

        len += scnprintf(buf + len, size - len,
            "%s:%d:%d:%s:%s:%s:%llu\n",
            entry->interpreter,
            entry->caller_pid,
            entry->caller_uid,
            entry->caller_name,
            entry->is_interactive ? "-" : entry->script_path,
            entry->is_interactive ? "interactive" : "script",
            entry->timestamp);
        printed++;
    }

    spin_unlock_irqrestore(&shell_list_lock, flags);

    return len;
}

/**
 * shell_audit_clear - 清空Shell执行记录
 */
void shell_audit_clear(void)
{
    struct shell_exec_entry *entry, *tmp;
    unsigned long flags;

    spin_lock_irqsave(&shell_list_lock, flags);
    list_for_each_entry_safe(entry, tmp, &shell_exec_list, list) {
        list_del(&entry->list);
        kfree(entry);
    }
    shell_list_count = 0;
    spin_unlock_irqrestore(&shell_list_lock, flags);

    vfs_trace("shell audit cleared");
}

/* ==================== 分区保护 ==================== */

/**
 * partition_is_protected - 检查路径是否在受保护分区中
 */
bool partition_is_protected(const char *path)
{
    int i;

    if (!part_policy.enabled || !path)
        return false;

    for (i = 0; i < partition_count; i++) {
        if (partitions[i].is_protected &&
            strncmp(path, partitions[i].mount_point,
                    strlen(partitions[i].mount_point)) == 0)
            return true;
    }

    /* 检查默认保护列表 */
    for (i = 0; protected_partitions[i] != NULL; i++) {
        if (strncmp(path, protected_partitions[i],
                    strlen(protected_partitions[i])) == 0)
            return true;
    }

    return false;
}

/**
 * partition_check_write - 检查对受保护分区的写操作
 * @path: 目标路径
 * @pid: 写入进程PID
 * @uid: 写入进程UID
 * @return: true=允许写入, false=拒绝写入
 */
bool partition_check_write(const char *path, pid_t pid, uid_t uid)
{
    int i;

    if (!part_policy.enabled)
        return true;

    if (!partition_is_protected(path))
        return true;

    /* root (uid=0) 总是允许 */
    if (uid == 0)
        return true;

    /* 记录修改 */
    for (i = 0; i < partition_count; i++) {
        if (partitions[i].is_protected &&
            strncmp(path, partitions[i].mount_point,
                    strlen(partitions[i].mount_point)) == 0) {
            atomic64_inc(&partitions[i].modification_count);
            partitions[i].is_modified = true;
            partitions[i].last_check_time = ktime_get_real_seconds();
        }
    }

    if (part_policy.auto_reject) {
        vfs_trace_level(1, "PARTITION PROTECT: DENIED write to %s by pid=%d uid=%d",
                        path, pid, uid);
        atomic64_inc(&shell_stats.denied_count);
        return false;
    }

    if (part_policy.alert_only) {
        vfs_trace_level(2, "PARTITION ALERT: write to %s by pid=%d uid=%d",
                        path, pid, uid);
    }

    return true;
}

/**
 * partition_get_status - 获取分区保护状态
 */
int partition_get_status(char *buf, size_t size)
{
    int len = 0;
    int i;

    len += scnprintf(buf + len, size - len,
        "enabled: %d\n"
        "auto_reject: %d\n"
        "alert_only: %d\n"
        "check_interval: %u\n"
        "---\n",
        part_policy.enabled ? 1 : 0,
        part_policy.auto_reject ? 1 : 0,
        part_policy.alert_only ? 1 : 0,
        part_policy.check_interval_sec);

    for (i = 0; i < partition_count; i++) {
        len += scnprintf(buf + len, size - len,
            "%s: protected=%d modified=%d mods=%lld\n",
            partitions[i].mount_point,
            partitions[i].is_protected ? 1 : 0,
            partitions[i].is_modified ? 1 : 0,
            atomic64_read(&partitions[i].modification_count));
    }

    return len;
}

/**
 * partition_set_policy - 设置分区保护策略
 * @input: 策略字符串 "enabled:auto_reject:alert_only:interval"
 */
int partition_set_policy(const char *input)
{
    int enabled, auto_reject, alert_only, interval;

    if (sscanf(input, "%d:%d:%d:%u", &enabled, &auto_reject,
               &alert_only, &interval) != 4)
        return -EINVAL;

    mutex_lock(&part_mutex);
    part_policy.enabled = enabled ? true : false;
    part_policy.auto_reject = auto_reject ? true : false;
    part_policy.alert_only = alert_only ? true : false;
    part_policy.check_interval_sec = interval;
    mutex_unlock(&part_mutex);

    vfs_trace("partition policy updated: enabled=%d reject=%d alert=%d",
              enabled, auto_reject, alert_only);

    return 0;
}

/**
 * partition_reset_modification - 重置分区修改标记
 */
void partition_reset_modification(void)
{
    int i;

    mutex_lock(&part_mutex);
    for (i = 0; i < partition_count; i++) {
        partitions[i].is_modified = false;
        atomic64_set(&partitions[i].modification_count, 0);
    }
    mutex_unlock(&part_mutex);

    vfs_trace("partition modification flags reset");
}

/* ==================== sysfs 接口 ==================== */

/* shell_stats: 显示Shell执行统计 */
static ssize_t shell_stats_show(struct kobject *kobj,
                                 struct kobj_attribute *attr, char *buf)
{
    return shell_audit_get_stats(buf, PAGE_SIZE);
}

/* shell_recent: 显示最近的Shell执行记录 */
static ssize_t shell_recent_show(struct kobject *kobj,
                                  struct kobj_attribute *attr, char *buf)
{
    return shell_audit_get_recent(buf, PAGE_SIZE, 20);
}

/* shell_clear: 清空Shell执行记录 (写入触发) */
static ssize_t shell_clear_store(struct kobject *kobj,
                                  struct kobj_attribute *attr,
                                  const char *buf, size_t count)
{
    shell_audit_clear();
    return count;
}

/* partition_status: 显示分区保护状态 */
static ssize_t partition_status_show(struct kobject *kobj,
                                       struct kobj_attribute *attr, char *buf)
{
    return partition_get_status(buf, PAGE_SIZE);
}

/* partition_policy: 设置分区保护策略 */
static ssize_t partition_policy_store(struct kobject *kobj,
                                        struct kobj_attribute *attr,
                                        const char *buf, size_t count)
{
    if (partition_set_policy(buf) == 0)
        return count;
    return -EINVAL;
}

/* partition_reset: 重置分区修改标记 (写入触发) */
static ssize_t partition_reset_store(struct kobject *kobj,
                                       struct kobj_attribute *attr,
                                       const char *buf, size_t count)
{
    partition_reset_modification();
    return count;
}

/* sysfs 属性定义 */
static struct kobj_attribute shell_stats_attr =
    __ATTR(shell_stats, 0444, shell_stats_show, NULL);

static struct kobj_attribute shell_recent_attr =
    __ATTR(shell_recent, 0444, shell_recent_show, NULL);

static struct kobj_attribute shell_clear_attr =
    __ATTR(shell_clear, 0200, NULL, shell_clear_store);

static struct kobj_attribute partition_status_attr =
    __ATTR(partition_status, 0444, partition_status_show, NULL);

static struct kobj_attribute partition_policy_attr =
    __ATTR(partition_policy, 0644, partition_status_show, partition_policy_store);

static struct kobj_attribute partition_reset_attr =
    __ATTR(partition_reset, 0200, NULL, partition_reset_store);

static struct attribute *audit_attrs[] = {
    &shell_stats_attr.attr,
    &shell_recent_attr.attr,
    &shell_clear_attr.attr,
    &partition_status_attr.attr,
    &partition_policy_attr.attr,
    &partition_reset_attr.attr,
    NULL
};

static struct attribute_group audit_attr_group = {
    .attrs = audit_attrs,
};

/* ==================== 初始化/退出 ==================== */

/**
 * security_audit_init - 初始化安全审计模块
 */
int security_audit_init(void)
{
    int ret, i;

    /* 初始化 Shell 统计 */
    atomic64_set(&shell_stats.total_exec_count, 0);
    atomic64_set(&shell_stats.script_exec_count, 0);
    atomic64_set(&shell_stats.interactive_count, 0);
    atomic64_set(&shell_stats.denied_count, 0);
    shell_stats.last_exec_timestamp = 0;
    memset(shell_stats.last_interpreter, 0, sizeof(shell_stats.last_interpreter));
    memset(shell_stats.last_caller, 0, sizeof(shell_stats.last_caller));

    /* 初始化 Shell 执行记录链表 */
    INIT_LIST_HEAD(&shell_exec_list);
    shell_list_count = 0;

    /* 初始化分区监控 */
    partition_count = 0;
    for (i = 0; protected_partitions[i] != NULL && i < AUDIT_MAX_PARTITIONS; i++) {
        strscpy(partitions[i].mount_point, protected_partitions[i],
                sizeof(partitions[i].mount_point));
        partitions[i].is_protected = true;
        partitions[i].is_modified = false;
        atomic64_set(&partitions[i].modification_count, 0);
        partition_count++;
    }

    /* 默认策略：启用保护，仅告警不拦截 */
    part_policy.enabled = true;
    part_policy.auto_reject = false;
    part_policy.alert_only = true;
    part_policy.check_interval_sec = 300;

    /* 创建 sysfs 目录: /sys/kernel/ztrosu/audit/ */
    kobj_audit = kobject_create_and_add("audit", g_ctx.kobj_root);
    if (!kobj_audit) {
        vfs_trace("ERROR: failed to create audit kobject");
        return -ENOMEM;
    }

    ret = sysfs_create_group(kobj_audit, &audit_attr_group);
    if (ret) {
        vfs_trace("ERROR: failed to create audit sysfs group: %d", ret);
        kobject_put(kobj_audit);
        return ret;
    }

    vfs_trace("security audit initialized: %d partitions monitored", partition_count);
    return 0;
}

/**
 * security_audit_exit - 退出安全审计模块
 */
void security_audit_exit(void)
{
    struct shell_exec_entry *entry, *tmp;
    unsigned long flags;

    /* 清理 Shell 执行记录 */
    spin_lock_irqsave(&shell_list_lock, flags);
    list_for_each_entry_safe(entry, tmp, &shell_exec_list, list) {
        list_del(&entry->list);
        kfree(entry);
    }
    shell_list_count = 0;
    spin_unlock_irqrestore(&shell_list_lock, flags);

    /* 注销 sysfs */
    if (kobj_audit) {
        sysfs_remove_group(kobj_audit, &audit_attr_group);
        kobject_put(kobj_audit);
        kobj_audit = NULL;
    }

    vfs_trace("security audit exited");
}
