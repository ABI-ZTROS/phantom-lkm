/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ioctl.c - AuroraSU VFS ioctl 通讯实现
 * 对齐 VFS_KERNEL_MODULE_SPEC.md v3.0 规范
 *
 * 提供 misc 设备 /dev/aurora_vfs_ioctl 作为 PRIMARY 通讯接口
 */

#include "phantom_lkm.h"
#include <linux/sched.h>        /* current */
#include <linux/ioctl.h>        /* ioctl 宏 */

/* ==================== 全局变量 ==================== */

static struct miscdevice ioctl_misc;
static DEFINE_MUTEX(ioctl_mutex);

/* 外部引用 events.c 中的事件环形缓冲区 */
extern struct event_record {
    __u32 magic;
    __u32 event_type;
    __u32 pid;
    __u32 uid;
    __u32 path_len;
    char  path[512];
    __u64 timestamp;
    __u32 result;
} event_ring[];

extern atomic_t event_head;
extern spinlock_t event_lock;
#define EVENT_RING_SIZE 64

/* ==================== 辅助函数 ==================== */

/**
 * check_root_access - 检查调用者是否为 root
 */
static inline int check_root_access(void)
{
    if (!capable(CAP_SYS_ADMIN))
        return -EPERM;
    return 0;
}

/* ==================== ioctl 命令处理 ==================== */

/**
 * ioc_get_version - 获取模块版本
 */
static long ioc_get_version(struct aurora_ioc_version __user *uver)
{
    struct aurora_ioc_version ver;

    memset(&ver, 0, sizeof(ver));
    ver.version = (u32)AURORA_VFS_VERSION[0] - '0';
    strscpy(ver.name, AURORA_VFS_NAME, sizeof(ver.name));

    if (copy_to_user(uver, &ver, sizeof(ver)))
        return -EFAULT;

    return 0;
}

/**
 * ioc_get_stats - 获取统计信息
 */
static long ioc_get_stats(struct aurora_ioc_stats __user *ustats)
{
    struct aurora_ioc_stats stats;

    memset(&stats, 0, sizeof(stats));
    stats.open_count   = atomic64_read(&g_ctx.stats.open_count);
    stats.read_count   = atomic64_read(&g_ctx.stats.read_count);
    stats.write_count  = atomic64_read(&g_ctx.stats.write_count);
    stats.close_count  = atomic64_read(&g_ctx.stats.close_count);
    stats.denied_count = atomic64_read(&g_ctx.stats.denied_count);
    stats.last_updated = g_ctx.stats.last_updated;

    if (copy_to_user(ustats, &stats, sizeof(stats)))
        return -EFAULT;

    return 0;
}

/**
 * ioc_get_policy - 获取当前策略
 */
static long ioc_get_policy(struct aurora_ioc_policy __user *upol)
{
    struct aurora_ioc_policy pol;

    memset(&pol, 0, sizeof(pol));
    pol.enabled        = READ_ONCE(g_ctx.policy.enabled) ? 1 : 0;
    pol.log_level      = (u8)g_ctx.policy.log_level;
    pol.default_action = (g_ctx.policy.default_action == VFS_ACTION_DENY) ? 1 : 0;

    if (copy_to_user(upol, &pol, sizeof(pol)))
        return -EFAULT;

    return 0;
}

/**
 * ioc_set_policy - 设置策略
 */
static long ioc_set_policy(struct aurora_ioc_policy __user *upol)
{
    struct aurora_ioc_policy pol;

    if (copy_from_user(&pol, upol, sizeof(pol)))
        return -EFAULT;

    if (pol.enabled != 0 && pol.enabled != 1)
        return -EINVAL;
    if (pol.log_level > 5)
        return -EINVAL;
    if (pol.default_action != 0 && pol.default_action != 1)
        return -EINVAL;

    WRITE_ONCE(g_ctx.policy.enabled, pol.enabled);
    WRITE_ONCE(g_ctx.policy.log_level, pol.log_level);
    g_ctx.policy.default_action = pol.default_action ? VFS_ACTION_DENY : VFS_ACTION_ALLOW;

    vfs_trace("ioctl: policy set enabled=%u, log_level=%u, default_action=%u",
              pol.enabled, pol.log_level, pol.default_action);
    return 0;
}

/**
 * ioc_get_rules - 读取所有规则到用户空间
 */
static long ioc_get_rules(struct aurora_ioc_rules __user *urules)
{
    struct aurora_ioc_rules krules;
    struct vfs_rule *rule;
    struct aurora_ioc_rule *kbuf = NULL;
    __u32 count = 0;
    __u32 max_count;
    int ret = 0;

    /* 先拷贝头部获取 max_count */
    if (copy_from_user(&krules, urules, sizeof(struct aurora_ioc_rules)))
        return -EFAULT;

    max_count = krules.max_count;
    if (max_count == 0 || max_count > VFS_MAX_RULES)
        max_count = VFS_MAX_RULES;

    kbuf = kzalloc(sizeof(struct aurora_ioc_rule) * max_count, GFP_KERNEL);
    if (!kbuf)
        return -ENOMEM;

    mutex_lock(&g_ctx.rules_mutex);

    list_for_each_entry(rule, &g_ctx.rules, list) {
        if (!rule->enabled)
            continue;
        if (count >= max_count)
            break;

        kbuf[count].priority = (__u32)rule->priority;
        kbuf[count].action   = (rule->action == VFS_ACTION_DENY) ? 1 : 0;
        kbuf[count].mode_mask = (__u8)rule->mode_mask;
        strscpy(kbuf[count].path_pattern, rule->path_pattern,
                sizeof(kbuf[count].path_pattern));
        count++;
    }

    mutex_unlock(&g_ctx.rules_mutex);

    /* 写回 count */
    krules.count = count;
    if (copy_to_user(urules, &krules, sizeof(struct aurora_ioc_rules))) {
        ret = -EFAULT;
        goto out;
    }

    /* 写回规则数组 */
    if (count > 0) {
        if (copy_to_user(urules->rules, kbuf,
                         sizeof(struct aurora_ioc_rule) * count)) {
            ret = -EFAULT;
            goto out;
        }
    }

out:
    kfree(kbuf);
    return ret;
}

/**
 * ioc_set_rules - 从用户空间批量设置规则
 */
static long ioc_set_rules(struct aurora_ioc_rules __user *urules)
{
    struct aurora_ioc_rules krules;
    struct aurora_ioc_rule *kbuf = NULL;
    int i;
    int ret = 0;
    int added = 0;

    if (copy_from_user(&krules, urules, sizeof(struct aurora_ioc_rules)))
        return -EFAULT;

    if (krules.count > VFS_MAX_RULES)
        return -EINVAL;

    if (krules.count == 0) {
        vfs_rules_clear();
        return 0;
    }

    kbuf = kzalloc(sizeof(struct aurora_ioc_rule) * krules.count, GFP_KERNEL);
    if (!kbuf)
        return -ENOMEM;

    if (copy_from_user(kbuf, urules->rules,
                       sizeof(struct aurora_ioc_rule) * krules.count)) {
        kfree(kbuf);
        return -EFAULT;
    }

    /* 先清空现有规则 */
    vfs_rules_clear();

    for (i = 0; i < krules.count; i++) {
        struct vfs_rule *rule;
        char rule_str[VFS_MAX_RULE_LEN];
        const char *mode_str;

        mode_str = (kbuf[i].mode_mask == VFS_OP_READ) ? "r" :
                   (kbuf[i].mode_mask == VFS_OP_WRITE) ? "w" : "rw";

        snprintf(rule_str, sizeof(rule_str), "%s:%s:%s",
                 kbuf[i].action ? "deny" : "allow",
                 kbuf[i].path_pattern, mode_str);

        rule = vfs_rule_parse(rule_str);
        if (rule) {
            rule->priority = kbuf[i].priority;
            if (vfs_rule_add(rule) == 0) {
                added++;
            } else {
                kfree(rule);
            }
        }
    }

    vfs_trace("ioctl: set_rules added %d/%u rules", added, krules.count);
    kfree(kbuf);

    return (added == krules.count) ? 0 : -EINVAL;
}

/**
 * ioc_clear_rules - 清空所有规则
 */
static long ioc_clear_rules(void)
{
    unsigned int count = vfs_rules_clear();
    vfs_trace("ioctl: clear_rules removed %u rules", count);
    return 0;
}

/**
 * ioc_get_hooks - 读取所有 Hook 目标
 */
static long ioc_get_hooks(struct aurora_ioc_hooks __user *uhooks)
{
    struct aurora_ioc_hooks khooks;
    struct vfs_hook_target *hook;
    struct aurora_ioc_hook *kbuf = NULL;
    __u32 count = 0;
    __u32 max_count;
    int ret = 0;

    if (copy_from_user(&khooks, uhooks, sizeof(struct aurora_ioc_hooks)))
        return -EFAULT;

    max_count = khooks.max_count;
    if (max_count == 0 || max_count > VFS_MAX_HOOKS)
        max_count = VFS_MAX_HOOKS;

    kbuf = kzalloc(sizeof(struct aurora_ioc_hook) * max_count, GFP_KERNEL);
    if (!kbuf)
        return -ENOMEM;

    mutex_lock(&g_ctx.hooks_mutex);

    list_for_each_entry(hook, &g_ctx.hooks, list) {
        if (!hook->enabled)
            continue;
        if (count >= max_count)
            break;

        kbuf[count].type    = (__u8)hook->type;
        kbuf[count].mode    = (__u8)hook->mode;
        kbuf[count].uid     = hook->uid;
        kbuf[count].enabled = hook->enabled ? 1 : 0;

        if (hook->type == VFS_HOOK_PID) {
            kbuf[count].pid = hook->pid;
        } else {
            strscpy(kbuf[count].package_name, hook->package_name,
                    sizeof(kbuf[count].package_name));
        }
        count++;
    }

    mutex_unlock(&g_ctx.hooks_mutex);

    khooks.count = count;
    if (copy_to_user(uhooks, &khooks, sizeof(struct aurora_ioc_hooks))) {
        ret = -EFAULT;
        goto out;
    }

    if (count > 0) {
        if (copy_to_user(uhooks->hooks, kbuf,
                         sizeof(struct aurora_ioc_hook) * count)) {
            ret = -EFAULT;
            goto out;
        }
    }

out:
    kfree(kbuf);
    return ret;
}

/**
 * ioc_add_hook - 添加 Hook 目标
 */
static long ioc_add_hook(struct aurora_ioc_hook __user *uhook)
{
    struct aurora_ioc_hook khook;
    char identifier[32];
    int ret;

    if (copy_from_user(&khook, uhook, sizeof(khook)))
        return -EFAULT;

    if (khook.type != VFS_HOOK_PID && khook.type != VFS_HOOK_PACKAGE)
        return -EINVAL;

    if (khook.type == VFS_HOOK_PID) {
        snprintf(identifier, sizeof(identifier), "%u", khook.pid);
    } else {
        strscpy(identifier, khook.package_name, sizeof(identifier));
    }

    ret = vfs_hook_add(khook.type, identifier, khook.uid, khook.mode);
    if (ret == 0) {
        vfs_trace("ioctl: add_hook success, type=%u, id=%s, uid=%u, mode=%u",
                  khook.type, identifier, khook.uid, khook.mode);
    }
    return ret;
}

/**
 * ioc_remove_hook - 移除 Hook 目标
 */
static long ioc_remove_hook(struct aurora_ioc_hook __user *uhook)
{
    struct aurora_ioc_hook khook;
    char identifier[32];
    int ret;

    if (copy_from_user(&khook, uhook, sizeof(khook)))
        return -EFAULT;

    if (khook.type != VFS_HOOK_PID && khook.type != VFS_HOOK_PACKAGE)
        return -EINVAL;

    if (khook.type == VFS_HOOK_PID) {
        snprintf(identifier, sizeof(identifier), "%u", khook.pid);
    } else {
        strscpy(identifier, khook.package_name, sizeof(identifier));
    }

    ret = vfs_hook_remove(khook.type, identifier);
    if (ret == 0) {
        vfs_trace("ioctl: remove_hook success, type=%u, id=%s", khook.type, identifier);
    }
    return ret;
}

/**
 * ioc_reset_stats - 重置统计
 */
static long ioc_reset_stats(void)
{
    vfs_stats_reset();
    vfs_trace("ioctl: reset_stats executed");
    return 0;
}

/**
 * ioc_get_events - 读取事件环形缓冲区
 */
static long ioc_get_events(struct aurora_ioc_events __user *uevents)
{
    struct aurora_ioc_events kevents;
    struct aurora_ioc_event *kbuf = NULL;
    __u32 count = 0;
    __u32 max_count;
    int i;
    int ret = 0;
    unsigned long flags;

    if (copy_from_user(&kevents, uevents, sizeof(struct aurora_ioc_events)))
        return -EFAULT;

    max_count = kevents.max_count;
    if (max_count == 0 || max_count > EVENT_RING_SIZE)
        max_count = EVENT_RING_SIZE;

    kbuf = kzalloc(sizeof(struct aurora_ioc_event) * max_count, GFP_KERNEL);
    if (!kbuf)
        return -ENOMEM;

    spin_lock_irqsave(&event_lock, flags);

    for (i = 0; i < EVENT_RING_SIZE && count < max_count; i++) {
        int idx = (atomic_read(&event_head) - i + EVENT_RING_SIZE) % EVENT_RING_SIZE;
        struct event_record *rec = &event_ring[idx];

        if (rec->magic != VFS_CMD_MAGIC)
            continue;

        kbuf[count].magic      = rec->magic;
        kbuf[count].event_type = rec->event_type;
        kbuf[count].pid        = rec->pid;
        kbuf[count].uid        = rec->uid;
        kbuf[count].path_len   = rec->path_len;
        kbuf[count].timestamp  = rec->timestamp;
        kbuf[count].result     = rec->result;

        if (rec->path_len > 0 && rec->path_len < VFS_MAX_PATH_LEN) {
            memcpy(kbuf[count].path, rec->path, rec->path_len);
            kbuf[count].path[rec->path_len] = '\0';
        }
        count++;
    }

    spin_unlock_irqrestore(&event_lock, flags);

    kevents.count = count;
    if (copy_to_user(uevents, &kevents, sizeof(struct aurora_ioc_events))) {
        ret = -EFAULT;
        goto out;
    }

    if (count > 0) {
        if (copy_to_user(uevents->events, kbuf,
                         sizeof(struct aurora_ioc_event) * count)) {
            ret = -EFAULT;
            goto out;
        }
    }

out:
    kfree(kbuf);
    return ret;
}

/* ==================== 文件操作回调 ==================== */

/**
 * ioctl_open - 打开 ioctl 设备
 */
static int ioctl_open(struct inode *inode, struct file *file)
{
    int ret = check_root_access();
    if (ret)
        return ret;

    vfs_trace("ioctl: opened by pid=%d", current->pid);
    return 0;
}

/**
 * ioctl_release - 关闭 ioctl 设备
 */
static int ioctl_release(struct inode *inode, struct file *file)
{
    vfs_trace("ioctl: closed by pid=%d", current->pid);
    return 0;
}

/**
 * aurora_vfs_ioctl - ioctl 主处理函数
 */
static long aurora_vfs_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    long ret;
    void __user *argp = (void __user *)arg;

    /* 验证 magic */
    if (_IOC_TYPE(cmd) != AURORA_VFS_IOC_MAGIC) {
        vfs_trace("ioctl: invalid magic 0x%02X", _IOC_TYPE(cmd));
        return -ENOTTY;
    }

    /* 验证命令号范围 */
    if (_IOC_NR(cmd) > AURORA_VFS_IOC_MAXNR) {
        vfs_trace("ioctl: invalid cmd nr %u", _IOC_NR(cmd));
        return -ENOTTY;
    }

    /* 验证参数指针方向 (有数据交换的命令) */
    if (_IOC_DIR(cmd) & (_IOC_READ | _IOC_WRITE)) {
        if (!argp)
            return -EINVAL;
    }

    mutex_lock(&ioctl_mutex);

    switch (cmd) {
    case AURORA_VFS_GET_VERSION:
        ret = ioc_get_version(argp);
        break;

    case AURORA_VFS_GET_STATS:
        ret = ioc_get_stats(argp);
        break;

    case AURORA_VFS_GET_POLICY:
        ret = ioc_get_policy(argp);
        break;

    case AURORA_VFS_SET_POLICY:
        ret = ioc_set_policy(argp);
        break;

    case AURORA_VFS_GET_RULES:
        ret = ioc_get_rules(argp);
        break;

    case AURORA_VFS_SET_RULES:
        ret = ioc_set_rules(argp);
        break;

    case AURORA_VFS_CLEAR_RULES:
        ret = ioc_clear_rules();
        break;

    case AURORA_VFS_GET_HOOKS:
        ret = ioc_get_hooks(argp);
        break;

    case AURORA_VFS_ADD_HOOK:
        ret = ioc_add_hook(argp);
        break;

    case AURORA_VFS_REMOVE_HOOK:
        ret = ioc_remove_hook(argp);
        break;

    case AURORA_VFS_RESET_STATS:
        ret = ioc_reset_stats();
        break;

    case AURORA_VFS_GET_EVENTS:
        ret = ioc_get_events(argp);
        break;

    default:
        vfs_trace("ioctl: unknown cmd 0x%08X", cmd);
        ret = -ENOTTY;
        break;
    }

    mutex_unlock(&ioctl_mutex);

    if (ret < 0 && ret != -ENOTTY) {
        vfs_trace_level(2, "ioctl: cmd 0x%08X failed, ret=%ld", cmd, ret);
    }

    return ret;
}

#ifdef CONFIG_COMPAT
/**
 * aurora_vfs_compat_ioctl - 32位兼容模式 ioctl
 */
static long aurora_vfs_compat_ioctl(struct file *file, unsigned int cmd,
                                     unsigned long arg)
{
    /* 所有数据结构均为固定大小，无指针，可直接复用主 ioctl */
    return aurora_vfs_ioctl(file, cmd, arg);
}
#endif

/* ==================== 文件操作表 ==================== */

static const struct file_operations ioctl_fops = {
    .owner          = THIS_MODULE,
    .open           = ioctl_open,
    .release        = ioctl_release,
    .unlocked_ioctl = aurora_vfs_ioctl,
#ifdef CONFIG_COMPAT
    .compat_ioctl   = aurora_vfs_compat_ioctl,
#endif
};

/* ==================== 接口函数 ==================== */

/**
 * vfs_ioctl_init - 初始化 ioctl 通讯
 */
int vfs_ioctl_init(void)
{
    int ret;

    memset(&ioctl_misc, 0, sizeof(ioctl_misc));
    ioctl_misc.minor = MISC_DYNAMIC_MINOR;
    ioctl_misc.name  = AURORA_VFS_IOCTL_NAME;  /* /dev/aurora_vfs_ioctl */
    ioctl_misc.fops  = &ioctl_fops;
    ioctl_misc.mode  = 0600;  /* 仅 root 可访问 */

    ret = misc_register(&ioctl_misc);
    if (ret) {
        pr_err("[aurora_vfs] ioctl: misc_register failed, ret=%d\n", ret);
        return ret;
    }

    pr_info("[aurora_vfs] ioctl: misc device registered at /dev/%s\n",
            AURORA_VFS_IOCTL_NAME);
    return 0;
}

/**
 * vfs_ioctl_exit - 注销 ioctl 通讯
 */
void vfs_ioctl_exit(void)
{
    misc_deregister(&ioctl_misc);
    pr_info("[aurora_vfs] ioctl: misc device unregistered\n");
}
