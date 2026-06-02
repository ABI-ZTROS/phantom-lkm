/* SPDX-License-Identifier: GPL-2.0 */
/*
 * pipe.c - AuroraSU VFS Pipe通讯实现
 * 对齐 VFS_KERNEL_MODULE_SPEC.md v3.0 规范
 *
 * 使用 misc 设备实现 Pipe 命令接收
 */

#include "phantom_lkm.h"
#include <linux/sched.h>    /* current */

/* ==================== 全局变量 ==================== */

static struct vfs_command cmd_buffer;
static DEFINE_MUTEX(pipe_mutex);

/* ==================== 文件操作回调 ==================== */

/**
 * pipe_open - 打开 misc 设备
 */
static int pipe_open(struct inode *inode, struct file *file)
{
    vfs_trace("pipe: opened by pid=%d", current->pid);
    return 0;
}

/**
 * pipe_release - 关闭 misc 设备
 */
static int pipe_release(struct inode *inode, struct file *file)
{
    vfs_trace("pipe: closed by pid=%d", current->pid);
    return 0;
}

/**
 * pipe_read - 读取回调 (暂不支持)
 */
static ssize_t pipe_read(struct file *file, char __user *buf, 
                         size_t count, loff_t *ppos)
{
    /* Pipe 是单向的：用户空间写入，内核读取处理 */
    /* 不支持从内核读取数据 */
    return -EINVAL;
}

/**
 * pipe_write - 写入回调，接收命令
 */
static ssize_t pipe_write(struct file *file, const char __user *buf,
                          size_t count, loff_t *ppos)
{
    struct vfs_command cmd;
    void *data = NULL;
    int ret;

    vfs_trace("pipe: write called, count=%zu", count);

    /* 最小长度检查 */
    if (count < sizeof(struct vfs_command)) {
        vfs_trace("pipe: data too short, need at least %zu bytes", 
                  sizeof(struct vfs_command));
        return -EINVAL;
    }

    /* 拷贝命令头 */
    if (copy_from_user(&cmd, buf, sizeof(cmd))) {
        vfs_trace("pipe: copy_from_user failed");
        return -EFAULT;
    }

    /* 校验 Magic */
    if (cmd.magic != VFS_CMD_MAGIC) {
        vfs_trace("pipe: invalid magic 0x%04X, expected 0x%04X", 
                  cmd.magic, VFS_CMD_MAGIC);
        return -EINVAL;
    }

    /* 校验版本 */
    if (cmd.version != VFS_CMD_VERSION) {
        vfs_trace("pipe: invalid version %u, expected %u", 
                  cmd.version, VFS_CMD_VERSION);
        return -EINVAL;
    }

    /* 校验命令类型 */
    if (cmd.cmd_type < CMD_ADD_HOOK || cmd.cmd_type > CMD_QUERY_STATUS) {
        vfs_trace("pipe: invalid cmd_type %u", cmd.cmd_type);
        return -EINVAL;
    }

    /* 如果有数据，分配内存并拷贝 */
    if (cmd.cmd_len > 0) {
        if (cmd.cmd_len > 4096) {  /* 最大限制 4KB */
            vfs_trace("pipe: cmd_len too large: %u", cmd.cmd_len);
            return -EINVAL;
        }

        data = kzalloc(cmd.cmd_len, GFP_KERNEL);
        if (!data) {
            vfs_trace("pipe: kzalloc failed for data");
            return -ENOMEM;
        }

        if (copy_from_user(data, buf + sizeof(cmd), cmd.cmd_len)) {
            vfs_trace("pipe: copy_from_user data failed");
            kfree(data);
            return -EFAULT;
        }
    }

    /* 处理命令 */
    mutex_lock(&pipe_mutex);
    ret = vfs_pipe_process_command(&cmd, data);
    mutex_unlock(&pipe_mutex);

    /* 释放数据内存 */
    kfree(data);

    if (ret < 0) {
        vfs_trace("pipe: command %u failed, ret=%d", cmd.cmd_type, ret);
        return ret;
    }

    vfs_trace("pipe: command %u processed successfully", cmd.cmd_type);
    return count;
}

/* ==================== 文件操作表 ==================== */

static const struct file_operations pipe_fops = {
    .owner   = THIS_MODULE,
    .open    = pipe_open,
    .release = pipe_release,
    .read    = pipe_read,
    .write   = pipe_write,
};

/* ==================== 接口函数 ==================== */

/**
 * vfs_pipe_init - 初始化 Pipe 通讯
 */
int vfs_pipe_init(void)
{
    int ret;

    /* 初始化 misc 设备 */
    g_ctx.pipe_misc.minor = MISC_DYNAMIC_MINOR;
    g_ctx.pipe_misc.name = AURORA_VFS_NAME;  /* /dev/aurora_vfs */
    g_ctx.pipe_misc.fops = &pipe_fops;
    g_ctx.pipe_misc.mode = 0600;  /* 仅 root 可访问 */

    ret = misc_register(&g_ctx.pipe_misc);
    if (ret) {
        vfs_trace("pipe: misc_register failed, ret=%d", ret);
        return ret;
    }

    vfs_trace("pipe: misc device registered at /dev/%s", AURORA_VFS_NAME);
    return 0;
}

/**
 * vfs_pipe_exit - 注销 Pipe 通讯
 */
void vfs_pipe_exit(void)
{
    misc_deregister(&g_ctx.pipe_misc);
    vfs_trace("pipe: misc device unregistered");
}

/* ==================== 命令处理 ==================== */

/**
 * pipe_cmd_add_hook - 处理 CMD_ADD_HOOK
 */
static int pipe_cmd_add_hook(void *data, size_t len)
{
    struct cmd_add_hook *cmd;
    char *identifier;
    __u32 uid;
    __u8 hook_mode;
    size_t min_len;
    int ret;

    if (!data || len < sizeof(struct cmd_add_hook)) {
        return -EINVAL;
    }

    cmd = (struct cmd_add_hook *)data;
    
    /* 计算最小长度 */
    min_len = sizeof(struct cmd_add_hook) + cmd->identifier_len + sizeof(__u32) + sizeof(__u8);
    if (len < min_len) {
        vfs_trace("pipe: add_hook data too short");
        return -EINVAL;
    }

    /* 提取字段 */
    identifier = (char *)(cmd + 1);
    
    /* 检查 identifier 长度 */
    if (cmd->identifier_len == 0 || cmd->identifier_len > VFS_MAX_PKG_LEN) {
        vfs_trace("pipe: add_hook invalid identifier_len %u", cmd->identifier_len);
        return -EINVAL;
    }

    /* 确保 identifier 以 null 结尾 */
    if (identifier[cmd->identifier_len - 1] != '\0') {
        /* 需要手动添加 null terminator */
        char *id_copy = kzalloc(cmd->identifier_len + 1, GFP_KERNEL);
        if (!id_copy)
            return -ENOMEM;
        memcpy(id_copy, identifier, cmd->identifier_len);
        id_copy[cmd->identifier_len] = '\0';
        
        /* uid 和 hook_mode 在 identifier 之后 */
        memcpy(&uid, identifier + cmd->identifier_len, sizeof(__u32));
        memcpy(&hook_mode, identifier + cmd->identifier_len + sizeof(__u32), sizeof(__u8));
        
        ret = vfs_hook_add(cmd->hook_type, id_copy, uid, hook_mode);
        kfree(id_copy);
    } else {
        /* 已经是 null terminated */
        memcpy(&uid, identifier + cmd->identifier_len, sizeof(__u32));
        memcpy(&hook_mode, identifier + cmd->identifier_len + sizeof(__u32), sizeof(__u8));
        
        ret = vfs_hook_add(cmd->hook_type, identifier, uid, hook_mode);
    }

    if (ret == 0) {
        vfs_trace("pipe: add_hook success, type=%u, uid=%u, mode=%u", 
                  cmd->hook_type, uid, hook_mode);
    }

    return ret;
}

/**
 * pipe_cmd_remove_hook - 处理 CMD_REMOVE_HOOK
 */
static int pipe_cmd_remove_hook(void *data, size_t len)
{
    struct cmd_remove_hook *cmd;
    char *identifier;
    int ret;

    if (!data || len < sizeof(struct cmd_remove_hook)) {
        return -EINVAL;
    }

    cmd = (struct cmd_remove_hook *)data;
    
    if (len < sizeof(struct cmd_remove_hook) + cmd->identifier_len) {
        vfs_trace("pipe: remove_hook data too short");
        return -EINVAL;
    }

    identifier = (char *)(cmd + 1);

    /* 确保 null terminated */
    if (cmd->identifier_len > 0 && identifier[cmd->identifier_len - 1] != '\0') {
        char *id_copy = kzalloc(cmd->identifier_len + 1, GFP_KERNEL);
        if (!id_copy)
            return -ENOMEM;
        memcpy(id_copy, identifier, cmd->identifier_len);
        id_copy[cmd->identifier_len] = '\0';
        
        ret = vfs_hook_remove(cmd->hook_type, id_copy);
        kfree(id_copy);
    } else {
        ret = vfs_hook_remove(cmd->hook_type, identifier);
    }

    if (ret == 0) {
        vfs_trace("pipe: remove_hook success, type=%u", cmd->hook_type);
    }

    return ret;
}

/**
 * pipe_cmd_set_rules - 处理 CMD_SET_RULES
 * 数据格式: rule_count (4字节) + 变长规则数据
 */
static int pipe_cmd_set_rules(void *data, size_t len)
{
    __u32 rule_count;
    char *pos;
    size_t remaining;
    int i, ret;
    int added = 0;

    if (!data || len < sizeof(__u32)) {
        return -EINVAL;
    }

    memcpy(&rule_count, data, sizeof(__u32));
    
    if (rule_count == 0) {
        /* 清空规则 */
        vfs_rules_clear();
        vfs_trace("pipe: set_rules cleared all rules");
        return 0;
    }

    if (rule_count > VFS_MAX_RULES) {
        vfs_trace("pipe: set_rules too many rules %u", rule_count);
        return -EINVAL;
    }

    /* 先清空现有规则 */
    vfs_rules_clear();

    pos = (char *)data + sizeof(__u32);
    remaining = len - sizeof(__u32);

    for (i = 0; i < rule_count && remaining > 0; i++) {
        __u8 action, mode_mask;
        __u32 path_len;
        char *path;
        struct vfs_rule *rule;
        char rule_str[VFS_MAX_RULE_LEN];

        /* 检查剩余数据是否足够 */
        if (remaining < sizeof(__u8) * 2 + sizeof(__u32)) {
            vfs_trace("pipe: set_rules incomplete rule data");
            break;
        }

        /* 解析字段 */
        action = *pos++;
        remaining--;
        
        memcpy(&path_len, pos, sizeof(__u32));
        pos += sizeof(__u32);
        remaining -= sizeof(__u32);
        
        mode_mask = *pos++;
        remaining--;

        if (path_len == 0 || path_len > VFS_MAX_PATH_LEN || path_len > remaining) {
            vfs_trace("pipe: set_rules invalid path_len %u", path_len);
            break;
        }

        path = pos;
        pos += path_len;
        remaining -= path_len;

        /* 构建规则字符串: action:path:mode */
        snprintf(rule_str, sizeof(rule_str), "%s:%.*s:%s",
                 action == 0 ? "allow" : "deny",
                 (int)path_len, path,
                 mode_mask == 0x01 ? "r" : (mode_mask == 0x02 ? "w" : "rw"));

        rule = vfs_rule_parse(rule_str);
        if (rule) {
            ret = vfs_rule_add(rule);
            if (ret == 0) {
                added++;
            } else {
                kfree(rule);
            }
        }
    }

    vfs_trace("pipe: set_rules added %d/%u rules", added, rule_count);
    return (added == rule_count) ? 0 : -EINVAL;
}

/**
 * pipe_cmd_clear_rules - 处理 CMD_CLEAR_RULES
 */
static int pipe_cmd_clear_rules(void)
{
    unsigned int count = vfs_rules_clear();
    vfs_trace("pipe: clear_rules removed %u rules", count);
    return 0;
}

/**
 * pipe_cmd_set_policy - 处理 CMD_SET_POLICY
 */
static int pipe_cmd_set_policy(void *data, size_t len)
{
    struct cmd_set_policy *cmd;

    if (!data || len < sizeof(struct cmd_set_policy)) {
        return -EINVAL;
    }

    cmd = (struct cmd_set_policy *)data;

    /* 验证参数 */
    if (cmd->enabled != 0 && cmd->enabled != 1) {
        vfs_trace("pipe: set_policy invalid enabled %u", cmd->enabled);
        return -EINVAL;
    }
    
    if (cmd->log_level > 5) {
        vfs_trace("pipe: set_policy invalid log_level %u", cmd->log_level);
        return -EINVAL;
    }
    
    if (cmd->default_action != 0 && cmd->default_action != 1) {
        vfs_trace("pipe: set_policy invalid default_action %u", cmd->default_action);
        return -EINVAL;
    }

    /* 应用策略 */
    g_ctx.policy.enabled = cmd->enabled;
    g_ctx.policy.log_level = cmd->log_level;
    g_ctx.policy.default_action = cmd->default_action;

    vfs_trace("pipe: set_policy enabled=%u, log_level=%u, default_action=%u",
              cmd->enabled, cmd->log_level, cmd->default_action);
    return 0;
}

/**
 * pipe_cmd_reset_stats - 处理 CMD_RESET_STATS
 */
static int pipe_cmd_reset_stats(void)
{
    vfs_stats_reset();
    vfs_trace("pipe: reset_stats executed");
    return 0;
}

/**
 * vfs_pipe_process_command - 主命令分发函数
 */
int vfs_pipe_process_command(struct vfs_command *cmd, void *data)
{
    if (!cmd) {
        return -EINVAL;
    }

    vfs_trace("pipe: processing cmd_type=%u, cmd_len=%u", 
              cmd->cmd_type, cmd->cmd_len);

    switch (cmd->cmd_type) {
    case CMD_ADD_HOOK:
        return pipe_cmd_add_hook(data, cmd->cmd_len);
        
    case CMD_REMOVE_HOOK:
        return pipe_cmd_remove_hook(data, cmd->cmd_len);
        
    case CMD_SET_RULES:
        return pipe_cmd_set_rules(data, cmd->cmd_len);
        
    case CMD_CLEAR_RULES:
        return pipe_cmd_clear_rules();
        
    case CMD_SET_POLICY:
        return pipe_cmd_set_policy(data, cmd->cmd_len);
        
    case CMD_RESET_STATS:
        return pipe_cmd_reset_stats();
        
    case CMD_QUERY_STATUS:
        /* 暂不支持，返回空状态 */
        vfs_trace("pipe: QUERY_STATUS not implemented");
        return 0;
        
    default:
        vfs_trace("pipe: unknown cmd_type %u", cmd->cmd_type);
        return -EINVAL;
    }
}
