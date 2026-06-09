/* SPDX-License-Identifier: GPL-2.0 */
/*
 * phantom_lkm.h - AuroraSU VFS 内核模块头文件
 * 对齐 VFS_KERNEL_MODULE_SPEC.md v3.0 规范
 *
 * 目标平台: OnePlus ACE5 (SM8650), 内核 6.1.141, Android 14 GKI
 */

#ifndef _PHANTOM_LKM_H_
#define _PHANTOM_LKM_H_

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/atomic.h>
#include <linux/ktime.h>
#include <linux/miscdevice.h>   /* miscdevice */
#include <linux/fs.h>           /* file_operations */
#include <linux/uaccess.h>      /* copy_from_user */
#include <net/sock.h>           /* netlink socket */
#include <linux/netlink.h>      /* netlink messages */
#include <linux/skbuff.h>       /* sk_buff */

/* LSM / Security 头文件 */
#include <linux/security.h>
#include <linux/cred.h>
#include <linux/binfmts.h>
#include <linux/dcache.h>
#include <linux/namei.h>

/* kprobe 回退支持 */
#include <linux/kprobes.h>
#include <linux/kallsyms.h>

/* trace_printk 头文件（Android GKI内核中通常不存在）
 * 直接使用 pr_debug 替代
 */
#include <linux/printk.h>

/* ==================== 模块信息 ==================== */

#define AURORA_VFS_NAME        "aurora_vfs"
#define AURORA_VFS_VERSION     "3"     /* v3.0 协议版本 */
#define AURORA_VFS_AUTHOR      "AuroraSU Research"
#define AURORA_VFS_DESC        "AuroraSU VFS Debug Kernel Module"

/* sysfs 目录路径: /sys/kernel/ztrosu/vfs/ */
#define AURORA_SYSFS_ROOT      "ztrosu"
#define AURORA_SYSFS_DIR       "vfs"

/* ==================== 常量定义 ==================== */

#define VFS_MAX_RULES          64      /* 最大规则数 */
#define VFS_MAX_HOOKS          128     /* 最大Hook目标数 */
#define VFS_MAX_RULE_LEN       256     /* 单条规则最大长度 */
#define VFS_MAX_PATH_LEN       512     /* 路径模式最大长度 */
#define VFS_MAX_PKG_LEN        256     /* 包名最大长度 */

/* v3 新增 - Pipe通讯 */
#define VFS_CMD_MAGIC          0xAF5F  /* 命令Magic Number */
#define VFS_CMD_VERSION        2       /* 命令协议版本 */
#define VFS_PIPE_TIMEOUT_MS    5000    /* Pipe超时 (毫秒) */
#define VFS_PIPE_NAME_PREFIX   "aurora_vfs_"
#define VFS_PIPE_PATH_LEN      32

/* Pipe命令类型 */
#define CMD_ADD_HOOK           1   /* 添加Hook目标 */
#define CMD_REMOVE_HOOK        2   /* 移除Hook目标 */
#define CMD_SET_RULES          3   /* 批量设置规则 */
#define CMD_CLEAR_RULES        4   /* 清空规则 */
#define CMD_SET_POLICY         5   /* 设置策略 */
#define CMD_RESET_STATS        6   /* 重置统计 */
#define CMD_QUERY_STATUS       7   /* 查询状态 */

/* v3 新增 - Netlink通讯 */
#define AURORA_VFS_NL_FAMILY   NETLINK_USERSOCK
#define AURORA_VFS_NL_GROUP    31      /* 多播组号 */
#define VFS_NL_MAX_MSG_LEN     4096    /* 单条事件最大长度 */
#define VFS_NL_TX_BUF_SIZE     16384   /* 内核发送缓冲区 */

/* Netlink 事件类型 */
#define EVENT_VFS_OPEN         1    /* 文件打开 */
#define EVENT_VFS_READ         2    /* 文件读取 */
#define EVENT_VFS_WRITE        3    /* 文件写入 */
#define EVENT_VFS_CLOSE        4    /* 文件关闭 */
#define EVENT_VFS_DENY         5    /* 访问被拒绝 */
#define EVENT_HOOK_ADDED       10   /* Hook目标已添加 */
#define EVENT_HOOK_REMOVED     11   /* Hook目标已移除 */
#define EVENT_RULE_CHANGED     12   /* 规则已变更 */

/* Netlink 事件结构体 (28字节固定头 + 变长path) */
struct vfs_event {
    __u32 magic;       /* 0xAF5F */
    __u32 event_type;  /* 事件类型 */
    __u32 pid;         /* 触发进程PID */
    __u32 uid;         /* 触发进程UID */
    __u32 path_len;    /* 文件路径长度 (不含\0) */
    __u8  path[];      /* 文件路径 (变长) */
    /* 后面紧跟: __u64 timestamp, __u32 result */
};

/* ==================== 枚举定义 ==================== */

/* 动作类型 */
enum vfs_action {
    VFS_ACTION_ALLOW = 0,
    VFS_ACTION_DENY = 1,
};

/* Hook类型 */
enum vfs_hook_type {
    VFS_HOOK_PID = 0,
    VFS_HOOK_PACKAGE = 1,
};

/* Hook模式 */
enum vfs_hook_mode {
    VFS_HOOK_MONITOR_ONLY = 0,     /* 仅监控 */
    VFS_HOOK_INTERCEPT_READ = 1,   /* 拦截读 */
    VFS_HOOK_INTERCEPT_WRITE = 2,  /* 拦截写 */
    VFS_HOOK_INTERCEPT_ALL = 3,    /* 拦截全部 */
};

/* 操作类型掩码 */
#define VFS_OP_READ    0x01    /* bit0: 读 */
#define VFS_OP_WRITE   0x02    /* bit1: 写 */

/* ==================== 数据结构 ==================== */

/* 规则结构 */
struct vfs_rule {
    struct list_head    list;
    int                 priority;           /* 优先级 (数字越大越先匹配) */
    enum vfs_action     action;             /* ALLOW / DENY */
    char                path_pattern[VFS_MAX_PATH_LEN];
    unsigned int        mode_mask;          /* 操作类型掩码 */
    uid_t               uid_filter;         /* UID过滤 (0=不限制) */
    bool                enabled;
};

/* Hook目标结构 */
struct vfs_hook_target {
    struct list_head    list;
    enum vfs_hook_type  type;               /* PID / PACKAGE */
    union {
        pid_t           pid;
        char            package_name[VFS_MAX_PKG_LEN];
    };
    uid_t               uid;
    enum vfs_hook_mode  mode;
    bool                enabled;
};

/* 统计结构 */
struct vfs_stats {
    atomic64_t          open_count;
    atomic64_t          read_count;
    atomic64_t          write_count;
    atomic64_t          close_count;
    atomic64_t          denied_count;
    u64                 last_updated;
};

/* 全局策略 */
struct vfs_policy {
    bool                enabled;            /* 全局开关 */
    unsigned int        log_level;          /* 日志级别 0-5 */
    enum vfs_action     default_action;     /* 默认动作 */
};

/* ==================== Pipe命令数据结构 ==================== */

/* 命令协议头 (16字节) */
struct vfs_command {
    __u32 magic;       /* 0xAF5F */
    __u32 version;     /* 2 */
    __u32 cmd_type;    /* 命令类型 */
    __u32 cmd_len;     /* 数据长度 */
    /* 后面紧跟 data[] */
};

/* CMD_ADD_HOOK 数据 */
struct cmd_add_hook {
    __u8  hook_type;        /* 0=PID, 1=PACKAGE */
    __u32 identifier_len;   /* identifier字符串长度 */
    /* 后面紧跟 identifier[] (变长) */
    /* 然后是 uid (__u32) */
    /* 然后是 hook_mode (__u8) */
};

/* CMD_REMOVE_HOOK 数据 */
struct cmd_remove_hook {
    __u8  hook_type;        /* 0=PID, 1=PACKAGE */
    __u32 identifier_len;   /* identifier字符串长度 */
    /* 后面紧跟 identifier[] (变长) */
};

/* CMD_SET_POLICY 数据 */
struct cmd_set_policy {
    __u8  enabled;          /* 0或1 */
    __u8  log_level;        /* 0-5 */
    __u8  default_action;   /* 0=allow, 1=deny */
    __u8  reserved;         /* 对齐填充 */
};

/* 全局上下文 */
struct vfs_debug_ctx {
    /* sysfs */
    struct kobject      *kobj_root;         /* /sys/kernel/ztrosu */
    struct kobject      *kobj_vfs;          /* /sys/kernel/ztrosu/vfs */
    
    /* 数据 */
    struct vfs_stats    stats;
    struct vfs_policy   policy;
    struct list_head    rules;              /* 规则链表 */
    struct list_head    hooks;              /* Hook目标链表 */
    unsigned int        rules_count;
    unsigned int        hooks_count;
    
    /* 同步 */
    struct mutex        rules_mutex;        /* 规则链表锁 */
    struct mutex        hooks_mutex;        /* Hook链表锁 */
    spinlock_t          stats_lock;         /* 统计更新锁 */
    
    /* 状态 */
    bool                initialized;
    
    /* v3 新增 - Netlink (阶段3实现) */
    struct sock         *nlsk;
    
    /* v3 新增 - Pipe (阶段2实现) */
    struct miscdevice   pipe_misc;          /* misc设备 */
};

/* ==================== LSM Hook 状态 ==================== */

enum lsm_hook_method {
    LSM_HOOK_NONE = 0,
    LSM_HOOK_NATIVE = 1,   /* 原生 security_add_hooks */
    LSM_HOOK_KPROBE = 2,   /* kprobe 动态挂钩 */
};

extern enum lsm_hook_method g_lsm_method;
extern bool g_lsm_hooks_registered;

/* ==================== 全局上下文 ==================== */

extern struct vfs_debug_ctx g_ctx;

/* ==================== 规则引擎接口 ==================== */

/**
 * vfs_rule_parse - 解析规则字符串
 * @rule_str: 规则字符串，格式 "action:path_pattern:mode"
 * @return: 成功返回规则指针，失败返回NULL
 */
struct vfs_rule *vfs_rule_parse(const char *rule_str);

/**
 * vfs_rule_match - 检查路径是否匹配规则
 * @rule: 规则指针
 * @path: 文件路径
 * @mode_mask: 操作类型掩码
 * @return: 匹配返回true
 */
bool vfs_rule_match(struct vfs_rule *rule, const char *path, unsigned int mode_mask);

/**
 * vfs_rule_add - 添加规则到链表
 * @rule: 规则指针
 * @return: 成功返回0
 */
int vfs_rule_add(struct vfs_rule *rule);

/**
 * vfs_rule_remove - 移除规则
 * @rule: 规则指针
 */
void vfs_rule_remove(struct vfs_rule *rule);

/**
 * vfs_rules_clear - 清空所有规则
 * @return: 清空的规则数量
 */
unsigned int vfs_rules_clear(void);

/**
 * vfs_rules_check - 检查路径是否允许访问
 * @path: 文件路径
 * @mode_mask: 操作类型掩码
 * @return: VFS_ACTION_ALLOW 或 VFS_ACTION_DENY
 */
enum vfs_action vfs_rules_check(const char *path, unsigned int mode_mask);

/* ==================== Hook管理接口 ==================== */

/**
 * vfs_hook_add - 添加Hook目标
 * @type: Hook类型 (PID/PACKAGE)
 * @identifier: PID字符串或包名
 * @uid: 目标UID
 * @mode: Hook模式
 * @return: 成功返回0
 */
int vfs_hook_add(enum vfs_hook_type type, const char *identifier, 
                 uid_t uid, enum vfs_hook_mode mode);

/**
 * vfs_hook_remove - 移除Hook目标
 * @type: Hook类型
 * @identifier: PID字符串或包名
 * @return: 成功返回0
 */
int vfs_hook_remove(enum vfs_hook_type type, const char *identifier);

/**
 * vfs_hook_check - 检查当前进程是否在Hook列表中
 * @pid: 进程PID
 * @uid: 进程UID
 * @mode_mask: 操作类型掩码 (用于返回匹配的mode)
 * @return: 匹配返回Hook指针，未匹配返回NULL
 */
struct vfs_hook_target *vfs_hook_check(pid_t pid, uid_t uid, unsigned int *mode_mask);

/**
 * vfs_hooks_clear - 清空所有Hook目标
 * @return: 清空的目标数量
 */
unsigned int vfs_hooks_clear(void);

/* ==================== 统计接口 ==================== */

/**
 * vfs_stats_init - 初始化统计计数器
 */
void vfs_stats_init(void);

/**
 * vfs_stats_reset - 重置统计计数器
 */
void vfs_stats_reset(void);

/**
 * vfs_stats_update - 更新统计计数器
 * @op_type: 操作类型 (open/read/write/close/denied)
 */
void vfs_stats_update(int op_type);

/**
 * vfs_stats_get_string - 获取统计信息字符串
 * @buf: 输出缓冲区
 * @size: 缓冲区大小
 * @return: 写入的字节数
 */
int vfs_stats_get_string(char *buf, size_t size);

/* ==================== sysfs接口 ==================== */

/**
 * vfs_sysfs_init - 初始化sysfs接口
 * @return: 成功返回0
 */
int vfs_sysfs_init(void);

/**
 * vfs_sysfs_exit - 注销sysfs接口
 */
void vfs_sysfs_exit(void);

/* ==================== Pipe接口 ==================== */

/**
 * vfs_pipe_init - 初始化Pipe通讯 (misc设备)
 * @return: 成功返回0
 */
int vfs_pipe_init(void);

/**
 * vfs_pipe_exit - 注销Pipe通讯
 */
void vfs_pipe_exit(void);

/**
 * vfs_pipe_process_command - 处理Pipe命令
 * @cmd: 命令结构体指针
 * @data: 命令数据指针
 * @return: 成功返回0
 */
int vfs_pipe_process_command(struct vfs_command *cmd, void *data);

/* ==================== Netlink接口 ==================== */

/**
 * vfs_netlink_init - 初始化Netlink通讯
 * @return: 成功返回0
 */
int vfs_netlink_init(void);

/**
 * vfs_netlink_exit - 注销Netlink通讯
 */
void vfs_netlink_exit(void);

/**
 * vfs_netlink_send_event - 发送VFS事件
 * @event_type: 事件类型 (EVENT_VFS_OPEN 等)
 * @pid: 触发进程PID
 * @uid: 触发进程UID
 * @path: 文件路径
 * @result: 0=allow, 1=deny
 */
void vfs_netlink_send_event(u32 event_type, u32 pid, u32 uid,
                            const char *path, u32 result);

/* ==================== 安全审计接口 ==================== */

/**
 * security_audit_init - 初始化安全审计模块
 * @return: 成功返回0
 */
int security_audit_init(void);

/**
 * security_audit_exit - 退出安全审计模块
 */
void security_audit_exit(void);

/**
 * shell_audit_record_exec - 记录Shell执行事件
 * @caller_pid: 调用者PID
 * @caller_uid: 调用者UID
 * @interpreter: 解释器路径
 * @script_path: 脚本路径（交互式为NULL）
 * @is_interactive: 是否交互式
 */
void shell_audit_record_exec(pid_t caller_pid, uid_t caller_uid,
                              const char *interpreter,
                              const char *script_path,
                              bool is_interactive);

/**
 * partition_is_protected - 检查路径是否在受保护分区
 * @path: 文件路径
 * @return: true=受保护
 */
bool partition_is_protected(const char *path);

/**
 * partition_check_write - 检查对受保护分区的写操作
 * @path: 目标路径
 * @pid: 写入进程PID
 * @uid: 写入进程UID
 * @return: true=允许, false=拒绝
 */
bool partition_check_write(const char *path, pid_t pid, uid_t uid);

/* ==================== 防格机接口 ==================== */

struct linux_binprm;

/**
 * anti_brick_init - 初始化防格机模块
 * @return: 成功返回0
 */
int anti_brick_init(void);

/**
 * anti_brick_exit - 退出防格机模块
 */
void anti_brick_exit(void);

/**
 * anti_brick_check_exec - 检查 exec 调用是否需要拦截
 * @bprm: binprm 结构
 * @return: 0=允许执行, -EPERM=拒绝执行
 */
int anti_brick_check_exec(struct linux_binprm *bprm);

/* ==================== LSM Hook 接口 ==================== */

/**
 * aurora_lsm_hooks_init - 注册 LSM hooks
 * @return: 成功返回0
 */
int aurora_lsm_hooks_init(void);

/**
 * aurora_lsm_hooks_exit - 注销 LSM hooks
 */
void aurora_lsm_hooks_exit(void);

/* ==================== 身份伪装接口 ==================== */

/**
 * identity_spoof_init - 初始化身份伪装模块
 * @return: 成功返回0
 */
int identity_spoof_init(void);

/**
 * identity_spoof_exit - 退出身份伪装模块
 */
void identity_spoof_exit(void);

/* 注意: SPOOF_* 常量在 identity_spoof.c 中以 enum 定义 */

/* 前向声明 identity_spoof.c 中的枚举类型 */
enum spoof_id_type;
enum spoof_strategy;

/**
 * spoof_add_rule - 添加伪装规则
 * @package_name: 目标包名
 * @type: 标识类型
 * @strategy: 伪装策略
 * @fake_value: 固定伪装值（可为NULL）
 * @rotate_interval_sec: 轮换间隔（秒）
 * @return: 规则ID，<0=错误
 */
int spoof_add_rule(const char *package_name, enum spoof_id_type type,
                    enum spoof_strategy strategy, const char *fake_value,
                    unsigned int rotate_interval_sec);

/**
 * spoof_remove_rule - 删除伪装规则
 * @id: 规则ID
 * @return: 0=成功
 */
int spoof_remove_rule(int id);

/**
 * spoof_intercept_read - 拦截读取操作
 * @path: 文件路径
 * @buf: 输出缓冲区
 * @len: 缓冲区长度
 * @return: >0=返回伪装值, 0=不拦截
 */
int spoof_intercept_read(const char *path, char *buf, size_t len);

/* ==================== VFS 事件节点接口 ==================== */

/**
 * vfs_events_init - 初始化VFS事件节点
 * @return: 成功返回0
 */
int vfs_events_init(void);

/**
 * vfs_events_exit - 注销VFS事件节点
 */
void vfs_events_exit(void);

/**
 * vfs_event_push - 推送事件到环形缓冲区 + Netlink
 * @event_type: 事件类型 (EVENT_VFS_OPEN 等)
 * @pid: 触发进程PID
 * @uid: 触发进程UID
 * @path: 文件路径
 * @result: 0=allow, 1=deny
 */
void vfs_event_push(__u32 event_type, __u32 pid, __u32 uid,
                    const char *path, __u32 result);

/* ==================== 调试输出宏 ==================== */

#define vfs_trace(fmt, ...) \
    pr_debug("[aurora_vfs] " fmt "\n", ##__VA_ARGS__)

#define vfs_trace_func() \
    pr_debug("[aurora_vfs] %s\n", __func__)

#define vfs_trace_level(level, fmt, ...) \
    do { \
        if (g_ctx.policy.log_level >= level) \
            pr_debug("[aurora_vfs] " fmt "\n", ##__VA_ARGS__); \
    } while (0)

#endif /* _PHANTOM_LKM_H_ */