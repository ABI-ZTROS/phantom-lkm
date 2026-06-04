/* SPDX-License-Identifier: GPL-2.0 */
/*
 * anti_brick.c - AuroraSU 防格机保护模块
 *
 * 功能：拦截 rm -rf /、dd if=/dev/zero of=/dev/block/... 等高危命令
 * 机制：挂起进程 → 通知用户层 → 等待确认 → 恢复或终止进程
 */

#include "phantom_lkm.h"
#include <linux/sched.h>
#include <linux/signal.h>
#include <linux/pid.h>
#include <linux/wait.h>
#include <linux/kthread.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/binfmts.h>
#include <linux/uaccess.h>
#include <linux/cred.h>
#include <linux/mm.h>

/* ==================== 常量定义 ==================== */

#define AB_MAX_PENDING      16      /* 最大挂起请求数 */
#define AB_CMDLINE_MAX      2048    /* 命令行最大长度 */
#define AB_TIMEOUT_MS       30000   /* 默认超时 30秒 */
#define AB_REASON_MAX       256     /* 拦截原因最大长度 */

/* 高危命令类型 */
enum ab_risk_type {
    AB_RISK_RM_RF_ROOT = 1,     /* rm -rf / 或 rm -rf /* */
    AB_RISK_DD_BLOCK,           /* dd 写入块设备 */
    AB_RISK_DD_ZERO,            /* dd if=/dev/zero */
    AB_RISK_MKFS,               /* mkfs 格式化 */
    AB_RISK_FDISK,              /* fdisk 分区操作 */
    AB_RISK_PARTED,             /* parted 分区操作 */
    AB_RISK_FLASH,              /* flash 刷写 */
    AB_RISK_FASTBOOT,           /* fastboot 命令 */
    AB_RISK_RECOVERY,           /* 恢复出厂设置 */
};

/* 挂起进程状态 */
enum ab_pending_state {
    AB_STATE_PENDING = 0,       /* 等待确认 */
    AB_STATE_ALLOWED = 1,       /* 用户允许 */
    AB_STATE_DENIED = 2,        /* 用户拒绝 */
    AB_STATE_TIMEOUT = 3,       /* 超时 */
};

/* ==================== 数据结构 ==================== */

struct ab_pending_request {
    struct list_head    list;
    int                 id;                 /* 请求ID */
    pid_t               pid;                /* 被挂起进程PID */
    pid_t               tgid;               /* 进程组ID */
    uid_t               uid;                /* 执行者UID */
    char                cmdline[AB_CMDLINE_MAX];  /* 完整命令行 */
    char                exe_path[256];      /* 可执行文件路径 */
    enum ab_risk_type   risk_type;          /* 风险类型 */
    char                reason[AB_REASON_MAX]; /* 拦截原因 */
    enum ab_pending_state state;            /* 当前状态 */
    u64                 timestamp;          /* 挂起时间戳 */
    struct task_struct  *task;              /* 被挂起任务指针 */
    wait_queue_head_t   waitq;              /* 等待队列 */
    bool                completed;          /* 是否已完成 */
};

struct ab_stats {
    atomic64_t          intercepted_count;  /* 拦截次数 */
    atomic64_t          allowed_count;      /* 允许次数 */
    atomic64_t          denied_count;       /* 拒绝次数 */
    atomic64_t          timeout_count;      /* 超时次数 */
    u64                 last_intercept_time; /* 最后拦截时间 */
};

/* ==================== 全局状态 ==================== */

static struct list_head         ab_pending_list;
static DEFINE_SPINLOCK(ab_list_lock);
static unsigned int             ab_pending_count;
static atomic_t                 ab_next_id;
static struct ab_stats          ab_stats;
static bool                     ab_enabled = true;
static unsigned int             ab_timeout_ms = AB_TIMEOUT_MS;

/* sysfs kobject */
static struct kobject *kobj_antibrick;

/* 工作线程 */
static struct task_struct *ab_worker_task;

/* ==================== 高危命令检测 ==================== */

/**
 * ab_detect_risk - 检测命令是否为高危操作
 * @cmdline: 完整命令行
 * @exe: 可执行文件路径
 * @return: 风险类型，0=无风险
 */
static enum ab_risk_type ab_detect_risk(const char *cmdline, const char *exe)
{
    const char *base = strrchr(exe, '/');
    if (base)
        base++;
    else
        base = exe;

    /* 检测 rm -rf / 或 rm -rf /* */
    if (strcmp(base, "rm") == 0) {
        if (strstr(cmdline, "-rf /") || strstr(cmdline, "-rf /*") ||
            strstr(cmdline, "--recursive") && strstr(cmdline, " /") &&
            (strstr(cmdline, "-f") || strstr(cmdline, "--force"))) {
            return AB_RISK_RM_RF_ROOT;
        }
    }

    /* 检测 dd 写入块设备 */
    if (strcmp(base, "dd") == 0) {
        /* dd if=/dev/zero of=/dev/block/... */
        if (strstr(cmdline, "of=/dev/block/") ||
            strstr(cmdline, "of=/dev/mmcblk") ||
            strstr(cmdline, "of=/dev/sd") ||
            strstr(cmdline, "of=/dev/nvme") ||
            strstr(cmdline, "of=/dev/mtd")) {
            if (strstr(cmdline, "if=/dev/zero") ||
                strstr(cmdline, "if=/dev/urandom"))
                return AB_RISK_DD_ZERO;
            return AB_RISK_DD_BLOCK;
        }
    }

    /* 检测 mkfs */
    if (strncmp(base, "mkfs.", 5) == 0 || strcmp(base, "mkfs") == 0) {
        if (strstr(cmdline, "/dev/block/") ||
            strstr(cmdline, "/dev/mmcblk"))
            return AB_RISK_MKFS;
    }

    /* 检测 fdisk/parted */
    if (strcmp(base, "fdisk") == 0 || strcmp(base, "parted") == 0) {
        if (strstr(cmdline, "/dev/block/") ||
            strstr(cmdline, "/dev/mmcblk"))
            return AB_RISK_FDISK;
    }

    /* 检测 flash/fastboot */
    if (strcmp(base, "flash") == 0 || strcmp(base, "fastboot") == 0) {
        if (strstr(cmdline, "flash") || strstr(cmdline, "erase"))
            return AB_RISK_FLASH;
    }

    /* 检测 recovery 恢复出厂 */
    if (strstr(cmdline, "--wipe_data") ||
        strstr(cmdline, "factory_reset") ||
        strstr(cmdline, "wipe_all")) {
        return AB_RISK_RECOVERY;
    }

    return 0;
}

/**
 * ab_get_risk_reason - 获取风险类型的描述
 */
static const char *ab_get_risk_reason(enum ab_risk_type type)
{
    switch (type) {
    case AB_RISK_RM_RF_ROOT:
        return "检测到 rm -rf / 命令，将删除整个文件系统";
    case AB_RISK_DD_BLOCK:
        return "检测到 dd 写入块设备，可能覆盖分区数据";
    case AB_RISK_DD_ZERO:
        return "检测到 dd 用零填充块设备，将擦除所有数据";
    case AB_RISK_MKFS:
        return "检测到格式化命令，将删除分区文件系统";
    case AB_RISK_FDISK:
        return "检测到分区操作命令，可能修改分区表";
    case AB_RISK_PARTED:
        return "检测到分区操作命令，可能修改分区表";
    case AB_RISK_FLASH:
        return "检测到刷写命令，可能覆盖系统分区";
    case AB_RISK_FASTBOOT:
        return "检测到 fastboot 刷写命令";
    case AB_RISK_RECOVERY:
        return "检测到恢复出厂设置命令，将清除用户数据";
    default:
        return "未知高危操作";
    }
}

/**
 * ab_get_risk_name - 获取风险类型名称
 */
static const char *ab_get_risk_name(enum ab_risk_type type)
{
    switch (type) {
    case AB_RISK_RM_RF_ROOT: return "RM_RF_ROOT";
    case AB_RISK_DD_BLOCK:   return "DD_BLOCK";
    case AB_RISK_DD_ZERO:    return "DD_ZERO";
    case AB_RISK_MKFS:       return "MKFS";
    case AB_RISK_FDISK:      return "FDISK";
    case AB_RISK_PARTED:     return "PARTED";
    case AB_RISK_FLASH:      return "FLASH";
    case AB_RISK_FASTBOOT:   return "FASTBOOT";
    case AB_RISK_RECOVERY:   return "RECOVERY";
    default:                 return "UNKNOWN";
    }
}

/* ==================== 进程控制 ==================== */

/**
 * ab_suspend_task - 挂起进程（发送 SIGSTOP）
 */
static int ab_suspend_task(struct task_struct *task)
{
    int ret;

    if (!task)
        return -EINVAL;

    /* 增加引用计数防止进程退出 */
    get_task_struct(task);

    /* 发送 SIGSTOP 挂起进程 */
    ret = send_sig_info(SIGSTOP, SEND_SIG_PRIV, task);
    if (ret < 0) {
        vfs_trace("anti_brick: failed to stop pid=%d, ret=%d", task->pid, ret);
        put_task_struct(task);
        return ret;
    }

    vfs_trace("anti_brick: suspended pid=%d", task->pid);
    return 0;
}

/**
 * ab_resume_task - 恢复进程（发送 SIGCONT）
 */
static int ab_resume_task(struct task_struct *task)
{
    int ret;

    if (!task)
        return -EINVAL;

    ret = send_sig_info(SIGCONT, SEND_SIG_PRIV, task);
    if (ret < 0) {
        vfs_trace("anti_brick: failed to resume pid=%d, ret=%d", task->pid, ret);
        return ret;
    }

    vfs_trace("anti_brick: resumed pid=%d", task->pid);
    put_task_struct(task);
    return 0;
}

/**
 * ab_kill_task - 终止进程（发送 SIGKILL）
 */
static int ab_kill_task(struct task_struct *task)
{
    int ret;

    if (!task)
        return -EINVAL;

    ret = send_sig_info(SIGKILL, SEND_SIG_PRIV, task);
    if (ret < 0) {
        vfs_trace("anti_brick: failed to kill pid=%d, ret=%d", task->pid, ret);
        return ret;
    }

    vfs_trace("anti_brick: killed pid=%d", task->pid);
    put_task_struct(task);
    return 0;
}

/* ==================== 请求管理 ==================== */

/**
 * ab_create_request - 创建挂起请求
 */
static struct ab_pending_request *ab_create_request(
    pid_t pid, pid_t tgid, uid_t uid,
    const char *cmdline, const char *exe_path,
    enum ab_risk_type risk_type,
    struct task_struct *task)
{
    struct ab_pending_request *req;

    req = kzalloc(sizeof(*req), GFP_KERNEL);
    if (!req)
        return NULL;

    req->id = atomic_inc_return(&ab_next_id);
    req->pid = pid;
    req->tgid = tgid;
    req->uid = uid;
    strscpy(req->cmdline, cmdline, sizeof(req->cmdline));
    strscpy(req->exe_path, exe_path, sizeof(req->exe_path));
    req->risk_type = risk_type;
    strscpy(req->reason, ab_get_risk_reason(risk_type), sizeof(req->reason));
    req->state = AB_STATE_PENDING;
    req->timestamp = ktime_get_real_seconds();
    req->task = task;
    init_waitqueue_head(&req->waitq);
    req->completed = false;

    return req;
}

/**
 * ab_add_request - 添加请求到列表
 */
static void ab_add_request(struct ab_pending_request *req)
{
    unsigned long flags;

    spin_lock_irqsave(&ab_list_lock, flags);

    /* FIFO 管理，超限删除最老的 */
    while (ab_pending_count >= AB_MAX_PENDING) {
        struct ab_pending_request *oldest =
            list_first_entry(&ab_pending_list, struct ab_pending_request, list);
        list_del(&oldest->list);
        ab_pending_count--;
        /* 如果还在挂起，恢复它 */
        if (oldest->state == AB_STATE_PENDING && oldest->task) {
            ab_resume_task(oldest->task);
        }
        kfree(oldest);
    }

    list_add_tail(&req->list, &ab_pending_list);
    ab_pending_count++;
    spin_unlock_irqrestore(&ab_list_lock, flags);
}

/**
 * ab_find_request - 按ID查找请求
 */
static struct ab_pending_request *ab_find_request(int id)
{
    struct ab_pending_request *req, *found = NULL;
    unsigned long flags;

    spin_lock_irqsave(&ab_list_lock, flags);
    list_for_each_entry(req, &ab_pending_list, list) {
        if (req->id == id) {
            found = req;
            break;
        }
    }
    spin_unlock_irqrestore(&ab_list_lock, flags);

    return found;
}

/**
 * ab_set_request_state - 设置请求状态并唤醒等待
 */
static void ab_set_request_state(struct ab_pending_request *req,
                                  enum ab_pending_state state)
{
    unsigned long flags;

    spin_lock_irqsave(&ab_list_lock, flags);
    req->state = state;
    req->completed = true;
    wake_up_all(&req->waitq);
    spin_unlock_irqrestore(&ab_list_lock, flags);
}

/**
 * ab_remove_request - 从列表移除请求
 */
static void ab_remove_request(struct ab_pending_request *req)
{
    unsigned long flags;

    spin_lock_irqsave(&ab_list_lock, flags);
    list_del(&req->list);
    ab_pending_count--;
    spin_unlock_irqrestore(&ab_list_lock, flags);

    kfree(req);
}

/* ==================== 核心拦截逻辑 ==================== */

/**
 * anti_brick_check_exec - 检查 exec 调用是否需要拦截
 * @bprm: binprm 结构
 * @return: 0=允许执行, -EPERM=拒绝执行
 *
 * 此函数在 execve 路径中被调用
 */
int anti_brick_check_exec(struct linux_binprm *bprm)
{
    enum ab_risk_type risk;
    struct ab_pending_request *req;
    struct task_struct *task = current;
    pid_t pid = task->pid;
    pid_t tgid = task->tgid;
    uid_t uid = __kuid_val(current_uid());
    char cmdline[AB_CMDLINE_MAX] = {0};
    char exe_path[256] = {0};
    int ret;
    long timeout_jiffies;

    if (!ab_enabled)
        return 0;

    /* 获取可执行文件路径 */
    if (bprm->file && bprm->file->f_path.dentry) {
        strscpy(exe_path, bprm->file->f_path.dentry->d_name.name,
                sizeof(exe_path));
    }

    /* 获取命令行参数 */
    if (bprm->arg_start && bprm->arg_end > bprm->arg_start) {
        int len = bprm->arg_end - bprm->arg_start;
        if (len > AB_CMDLINE_MAX - 1)
            len = AB_CMDLINE_MAX - 1;
        if (access_process_vm(task, bprm->arg_start, cmdline, len, 0) > 0) {
            /* 将参数分隔符替换为空格 */
            int i;
            for (i = 0; i < len; i++) {
                if (cmdline[i] == '\0')
                    cmdline[i] = ' ';
            }
            cmdline[len] = '\0';
        }
    }

    /* 检测高危命令 */
    risk = ab_detect_risk(cmdline, exe_path);
    if (!risk)
        return 0;

    /* 更新统计 */
    atomic64_inc(&ab_stats.intercepted_count);
    ab_stats.last_intercept_time = ktime_get_real_seconds();

    vfs_trace("anti_brick: INTERCEPTED pid=%d cmd=%s risk=%s",
              pid, exe_path, ab_get_risk_name(risk));

    /* 创建挂起请求 */
    req = ab_create_request(pid, tgid, uid, cmdline, exe_path, risk, task);
    if (!req) {
        vfs_trace("anti_brick: failed to create request, allowing execution");
        return 0;
    }

    /* 挂起进程 */
    ret = ab_suspend_task(task);
    if (ret < 0) {
        vfs_trace("anti_brick: failed to suspend, killing pid=%d", pid);
        ab_kill_task(task);
        kfree(req);
        return -EPERM;
    }

    /* 添加到列表 */
    ab_add_request(req);

    /* 发送 Netlink 通知用户层 */
    vfs_netlink_send_event(100 + risk, pid, uid, cmdline, 0);

    /* 等待用户确认（带超时） */
    timeout_jiffies = msecs_to_jiffies(ab_timeout_ms);
    ret = wait_event_interruptible_timeout(
        req->waitq,
        req->completed,
        timeout_jiffies);

    if (ret == 0) {
        /* 超时 */
        vfs_trace("anti_brick: TIMEOUT pid=%d, denying", pid);
        req->state = AB_STATE_TIMEOUT;
        atomic64_inc(&ab_stats.timeout_count);
        ab_kill_task(task);
        ab_remove_request(req);
        return -EPERM;
    }

    if (ret < 0) {
        /* 被信号中断 */
        vfs_trace("anti_brick: interrupted pid=%d, denying", pid);
        req->state = AB_STATE_DENIED;
        atomic64_inc(&ab_stats.denied_count);
        ab_kill_task(task);
        ab_remove_request(req);
        return -EPERM;
    }

    /* 用户已响应 */
    switch (req->state) {
    case AB_STATE_ALLOWED:
        vfs_trace("anti_brick: ALLOWED pid=%d", pid);
        atomic64_inc(&ab_stats.allowed_count);
        ab_resume_task(task);
        ab_remove_request(req);
        return 0;

    case AB_STATE_DENIED:
    default:
        vfs_trace("anti_brick: DENIED pid=%d", pid);
        atomic64_inc(&ab_stats.denied_count);
        ab_kill_task(task);
        ab_remove_request(req);
        return -EPERM;
    }
}

/* ==================== sysfs 接口 ==================== */

/* antibrick/pending - 显示挂起请求列表 */
static ssize_t ab_pending_show(struct kobject *kobj,
                                struct kobj_attribute *attr, char *buf)
{
    struct ab_pending_request *req;
    unsigned long flags;
    int len = 0;

    len += scnprintf(buf + len, PAGE_SIZE - len,
        "id:pid:uid:state:risk:cmdline\n");

    spin_lock_irqsave(&ab_list_lock, flags);
    list_for_each_entry(req, &ab_pending_list, list) {
        len += scnprintf(buf + len, PAGE_SIZE - len,
            "%d:%d:%d:%s:%s:%s\n",
            req->id, req->pid, req->uid,
            req->state == AB_STATE_PENDING ? "PENDING" :
            req->state == AB_STATE_ALLOWED ? "ALLOWED" :
            req->state == AB_STATE_DENIED ? "DENIED" : "TIMEOUT",
            ab_get_risk_name(req->risk_type),
            req->cmdline);
    }
    spin_unlock_irqrestore(&ab_list_lock, flags);

    return len;
}

/* antibrick/allow - 允许指定ID的请求 (写入 "id") */
static ssize_t ab_allow_store(struct kobject *kobj,
                               struct kobj_attribute *attr,
                               const char *buf, size_t count)
{
    int id;
    struct ab_pending_request *req;

    if (kstrtoint(buf, 10, &id) != 0)
        return -EINVAL;

    req = ab_find_request(id);
    if (!req)
        return -ENOENT;

    ab_set_request_state(req, AB_STATE_ALLOWED);
    return count;
}

/* antibrick/deny - 拒绝指定ID的请求 (写入 "id") */
static ssize_t ab_deny_store(struct kobject *kobj,
                              struct kobj_attribute *attr,
                              const char *buf, size_t count)
{
    int id;
    struct ab_pending_request *req;

    if (kstrtoint(buf, 10, &id) != 0)
        return -EINVAL;

    req = ab_find_request(id);
    if (!req)
        return -ENOENT;

    ab_set_request_state(req, AB_STATE_DENIED);
    return count;
}

/* antibrick/stats - 显示统计 */
static ssize_t ab_stats_show(struct kobject *kobj,
                              struct kobj_attribute *attr, char *buf)
{
    return scnprintf(buf, PAGE_SIZE,
        "intercepted: %lld\n"
        "allowed: %lld\n"
        "denied: %lld\n"
        "timeout: %lld\n"
        "pending: %u\n"
        "last_intercept: %llu\n",
        atomic64_read(&ab_stats.intercepted_count),
        atomic64_read(&ab_stats.allowed_count),
        atomic64_read(&ab_stats.denied_count),
        atomic64_read(&ab_stats.timeout_count),
        ab_pending_count,
        ab_stats.last_intercept_time);
}

/* antibrick/enabled - 开关 */
static ssize_t ab_enabled_show(struct kobject *kobj,
                                struct kobj_attribute *attr, char *buf)
{
    return scnprintf(buf, PAGE_SIZE, "%d\n", ab_enabled ? 1 : 0);
}

static ssize_t ab_enabled_store(struct kobject *kobj,
                                 struct kobj_attribute *attr,
                                 const char *buf, size_t count)
{
    int val;
    if (kstrtoint(buf, 10, &val) != 0)
        return -EINVAL;
    ab_enabled = val ? true : false;
    return count;
}

/* antibrick/timeout - 超时设置 */
static ssize_t ab_timeout_show(struct kobject *kobj,
                                struct kobj_attribute *attr, char *buf)
{
    return scnprintf(buf, PAGE_SIZE, "%u\n", ab_timeout_ms);
}

static ssize_t ab_timeout_store(struct kobject *kobj,
                                 struct kobj_attribute *attr,
                                 const char *buf, size_t count)
{
    unsigned int val;
    if (kstrtouint(buf, 10, &val) != 0)
        return -EINVAL;
    if (val < 1000 || val > 300000)
        return -EINVAL;  /* 1秒到5分钟 */
    ab_timeout_ms = val;
    return count;
}

/* antibrick/active - 模块是否活跃（只读，用户层检测用） */
static ssize_t ab_active_show(struct kobject *kobj,
                               struct kobj_attribute *attr, char *buf)
{
    return scnprintf(buf, PAGE_SIZE, "1\n");
}

/* sysfs 属性 */
static struct kobj_attribute ab_pending_attr =
    __ATTR(pending, 0444, ab_pending_show, NULL);

static struct kobj_attribute ab_allow_attr =
    __ATTR(allow, 0200, NULL, ab_allow_store);

static struct kobj_attribute ab_deny_attr =
    __ATTR(deny, 0200, NULL, ab_deny_store);

static struct kobj_attribute ab_stats_attr =
    __ATTR(stats, 0444, ab_stats_show, NULL);

static struct kobj_attribute ab_enabled_attr =
    __ATTR(enabled, 0644, ab_enabled_show, ab_enabled_store);

static struct kobj_attribute ab_timeout_attr =
    __ATTR(timeout, 0644, ab_timeout_show, ab_timeout_store);

static struct kobj_attribute ab_active_attr =
    __ATTR(active, 0444, ab_active_show, NULL);

static struct attribute *ab_attrs[] = {
    &ab_pending_attr.attr,
    &ab_allow_attr.attr,
    &ab_deny_attr.attr,
    &ab_stats_attr.attr,
    &ab_enabled_attr.attr,
    &ab_timeout_attr.attr,
    &ab_active_attr.attr,
    NULL
};

static struct attribute_group ab_attr_group = {
    .attrs = ab_attrs,
};

/* ==================== 初始化/退出 ==================== */

/**
 * anti_brick_init - 初始化防格机模块
 */
int anti_brick_init(void)
{
    int ret;

    /* 初始化链表 */
    INIT_LIST_HEAD(&ab_pending_list);
    ab_pending_count = 0;
    atomic_set(&ab_next_id, 0);

    /* 初始化统计 */
    atomic64_set(&ab_stats.intercepted_count, 0);
    atomic64_set(&ab_stats.allowed_count, 0);
    atomic64_set(&ab_stats.denied_count, 0);
    atomic64_set(&ab_stats.timeout_count, 0);
    ab_stats.last_intercept_time = 0;

    /* 创建 sysfs */
    kobj_antibrick = kobject_create_and_add("antibrick", g_ctx.kobj_root);
    if (!kobj_antibrick) {
        vfs_trace("anti_brick: failed to create kobject");
        return -ENOMEM;
    }

    ret = sysfs_create_group(kobj_antibrick, &ab_attr_group);
    if (ret) {
        vfs_trace("anti_brick: failed to create sysfs group: %d", ret);
        kobject_put(kobj_antibrick);
        return ret;
    }

    vfs_trace("anti_brick: initialized, timeout=%ums", ab_timeout_ms);
    return 0;
}

/**
 * anti_brick_exit - 退出防格机模块
 */
void anti_brick_exit(void)
{
    struct ab_pending_request *req, *tmp;
    unsigned long flags;

    /* 恢复所有挂起的进程 */
    spin_lock_irqsave(&ab_list_lock, flags);
    list_for_each_entry_safe(req, tmp, &ab_pending_list, list) {
        if (req->state == AB_STATE_PENDING && req->task) {
            ab_resume_task(req->task);
        }
        list_del(&req->list);
        kfree(req);
    }
    ab_pending_count = 0;
    spin_unlock_irqrestore(&ab_list_lock, flags);

    /* 注销 sysfs */
    if (kobj_antibrick) {
        sysfs_remove_group(kobj_antibrick, &ab_attr_group);
        kobject_put(kobj_antibrick);
        kobj_antibrick = NULL;
    }

    vfs_trace("anti_brick: exited");
}
