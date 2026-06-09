/* SPDX-License-Identifier: GPL-2.0 */
/*
 * phantom_lkm.c - AuroraSU VFS 内核模块
 * 对齐 VFS_KERNEL_MODULE_SPEC.md v3.0 规范
 *
 * 目标平台: OnePlus ACE5 (SM8650), 内核 6.1.141, Android 14 GKI
 */

#include "phantom_lkm.h"

/* ==================== 全局上下文 ==================== */

struct vfs_debug_ctx g_ctx;

/* LSM Hook 状态 */
enum lsm_hook_method g_lsm_method = LSM_HOOK_NONE;
bool g_lsm_hooks_registered = false;

/* ==================== 统计计数器实现 ==================== */

void vfs_stats_init(void)
{
    atomic64_set(&g_ctx.stats.open_count, 0);
    atomic64_set(&g_ctx.stats.read_count, 0);
    atomic64_set(&g_ctx.stats.write_count, 0);
    atomic64_set(&g_ctx.stats.close_count, 0);
    atomic64_set(&g_ctx.stats.denied_count, 0);
    g_ctx.stats.last_updated = ktime_get_real_seconds();
}

void vfs_stats_reset(void)
{
    vfs_stats_init();
    vfs_trace("stats reset");
}

/* 操作类型: 0=open, 1=read, 2=write, 3=close, 4=denied */
void vfs_stats_update(int op_type)
{
    switch (op_type) {
    case 0:
        atomic64_inc(&g_ctx.stats.open_count);
        break;
    case 1:
        atomic64_inc(&g_ctx.stats.read_count);
        break;
    case 2:
        atomic64_inc(&g_ctx.stats.write_count);
        break;
    case 3:
        atomic64_inc(&g_ctx.stats.close_count);
        break;
    case 4:
        atomic64_inc(&g_ctx.stats.denied_count);
        break;
    }
    g_ctx.stats.last_updated = ktime_get_real_seconds();
}

/* 内核安全的整数解析（替代 atoi，失败返回 0） */
static int safe_atoi(const char *str)
{
    int result = 0;
    kstrtoint(str, 10, &result);
    return result;
}

int vfs_stats_get_string(char *buf, size_t size)
{
    return snprintf(buf, size,
        "open: %lld\n"
        "read: %lld\n"
        "write: %lld\n"
        "close: %lld\n"
        "denied: %lld\n"
        "last_updated: %lld\n",
        atomic64_read(&g_ctx.stats.open_count),
        atomic64_read(&g_ctx.stats.read_count),
        atomic64_read(&g_ctx.stats.write_count),
        atomic64_read(&g_ctx.stats.close_count),
        atomic64_read(&g_ctx.stats.denied_count),
        g_ctx.stats.last_updated);
}

/* ==================== 规则引擎实现 ==================== */

/* glob匹配 - 支持 * ? ** */
static bool glob_match(const char *pattern, const char *path)
{
    /* 简化实现: 仅支持 * 和 ** */
    const char *p = pattern, *s = path;
    
    while (*p && *s) {
        if (*p == '*') {
            /* 检查是否是 ** (匹配任意字符含/) */
            if (p[1] == '*') {
                p += 2;
                /* ** 匹配剩余所有字符 */
                if (!*p) return true;
                /* 递归匹配 */
                while (*s) {
                    if (glob_match(p, s)) return true;
                    s++;
                }
                return false;
            } else {
                /* * 匹配非/字符 */
                p++;
                while (*s && *s != '/') {
                    if (glob_match(p, s)) return true;
                    s++;
                }
                continue;
            }
        } else if (*p == '?') {
            /* ? 匹配单个非/字符 */
            p++;
            if (*s == '/') return false;
            s++;
        } else if (*p == *s) {
            p++;
            s++;
        } else {
            return false;
        }
    }
    
    /* 处理末尾 * */
    while (*p == '*') p++;
    
    return !*p && !*s;
}

struct vfs_rule *vfs_rule_parse(const char *rule_str)
{
    struct vfs_rule *rule;
    char *buf, *action_str, *path_str, *mode_str;
    int ret;
    
    if (!rule_str || strlen(rule_str) > VFS_MAX_RULE_LEN)
        return NULL;
    
    rule = kzalloc(sizeof(*rule), GFP_KERNEL);
    if (!rule)
        return NULL;
    
    buf = kstrndup(rule_str, VFS_MAX_RULE_LEN, GFP_KERNEL);
    if (!buf) {
        kfree(rule);
        return NULL;
    }
    
    /* 解析格式: action:path_pattern:mode */
    action_str = buf;
    path_str = strchr(buf, ':');
    if (!path_str) {
        kfree(buf);
        kfree(rule);
        return NULL;
    }
    *path_str++ = '\0';
    
    mode_str = strchr(path_str, ':');
    if (!mode_str) {
        kfree(buf);
        kfree(rule);
        return NULL;
    }
    *mode_str++ = '\0';
    
    /* 解析action */
    if (strcmp(action_str, "allow") == 0)
        rule->action = VFS_ACTION_ALLOW;
    else if (strcmp(action_str, "deny") == 0)
        rule->action = VFS_ACTION_DENY;
    else {
        kfree(buf);
        kfree(rule);
        return NULL;
    }
    
    /* 解析path_pattern */
    if (strlen(path_str) > VFS_MAX_PATH_LEN) {
        kfree(buf);
        kfree(rule);
        return NULL;
    }
    strncpy(rule->path_pattern, path_str, VFS_MAX_PATH_LEN - 1);
    
    /* 解析mode */
    rule->mode_mask = 0;
    if (strchr(mode_str, 'r') || strchr(mode_str, 'R'))
        rule->mode_mask |= VFS_OP_READ;
    if (strchr(mode_str, 'w') || strchr(mode_str, 'W'))
        rule->mode_mask |= VFS_OP_WRITE;
    
    /* 默认值 */
    rule->priority = 50;
    rule->uid_filter = 0;
    rule->enabled = true;
    
    INIT_LIST_HEAD(&rule->list);
    
    kfree(buf);
    return rule;
}

bool vfs_rule_match(struct vfs_rule *rule, const char *path, unsigned int mode_mask)
{
    if (!rule || !rule->enabled || !path)
        return false;
    
    /* 检查操作类型 */
    if ((rule->mode_mask & mode_mask) != mode_mask)
        return false;
    
    /* 检查路径匹配 */
    return glob_match(rule->path_pattern, path);
}

int vfs_rule_add(struct vfs_rule *rule)
{
    if (!rule)
        return -EINVAL;
    
    mutex_lock(&g_ctx.rules_mutex);
    
    if (g_ctx.rules_count >= VFS_MAX_RULES) {
        mutex_unlock(&g_ctx.rules_mutex);
        return -ENOSPC;
    }
    
    /* 按优先级插入 */
    struct vfs_rule *pos;
    list_for_each_entry(pos, &g_ctx.rules, list) {
        if (rule->priority > pos->priority) {
            list_add_tail(&rule->list, &pos->list);
            g_ctx.rules_count++;
            mutex_unlock(&g_ctx.rules_mutex);
            vfs_trace("rule added: %s:%s", 
                      rule->action == VFS_ACTION_ALLOW ? "allow" : "deny",
                      rule->path_pattern);
            return 0;
        }
    }
    
    /* 添加到末尾 */
    list_add_tail(&rule->list, &g_ctx.rules);
    g_ctx.rules_count++;
    
    mutex_unlock(&g_ctx.rules_mutex);
    vfs_trace("rule added: %s:%s", 
              rule->action == VFS_ACTION_ALLOW ? "allow" : "deny",
              rule->path_pattern);
    return 0;
}

void vfs_rule_remove(struct vfs_rule *rule)
{
    if (!rule)
        return;
    
    mutex_lock(&g_ctx.rules_mutex);
    list_del_init(&rule->list);
    g_ctx.rules_count--;
    mutex_unlock(&g_ctx.rules_mutex);
    
    kfree(rule);
    vfs_trace("rule removed");
}

unsigned int vfs_rules_clear(void)
{
    struct vfs_rule *rule, *tmp;
    unsigned int count = 0;
    
    mutex_lock(&g_ctx.rules_mutex);
    
    list_for_each_entry_safe(rule, tmp, &g_ctx.rules, list) {
        list_del_init(&rule->list);
        kfree(rule);
        count++;
    }
    
    g_ctx.rules_count = 0;
    INIT_LIST_HEAD(&g_ctx.rules);
    
    mutex_unlock(&g_ctx.rules_mutex);
    
    vfs_trace("rules cleared: %u", count);
    return count;
}

enum vfs_action vfs_rules_check(const char *path, unsigned int mode_mask)
{
    struct vfs_rule *rule;
    
    mutex_lock(&g_ctx.rules_mutex);
    
    list_for_each_entry(rule, &g_ctx.rules, list) {
        if (vfs_rule_match(rule, path, mode_mask)) {
            enum vfs_action action = rule->action;
            mutex_unlock(&g_ctx.rules_mutex);
            vfs_trace_level(3, "rule matched: %s -> %s", path,
                           action == VFS_ACTION_ALLOW ? "allow" : "deny");
            return action;
        }
    }
    
    mutex_unlock(&g_ctx.rules_mutex);
    
    /* 无匹配，返回默认动作 */
    return g_ctx.policy.default_action;
}

/* ==================== Hook管理实现 ==================== */

int vfs_hook_add(enum vfs_hook_type type, const char *identifier,
                 uid_t uid, enum vfs_hook_mode mode)
{
    struct vfs_hook_target *hook;
    
    if (!identifier)
        return -EINVAL;
    
    if (type == VFS_HOOK_PID && strlen(identifier) > 16)
        return -EINVAL;
    
    if (type == VFS_HOOK_PACKAGE && strlen(identifier) > VFS_MAX_PKG_LEN)
        return -EINVAL;
    
    mutex_lock(&g_ctx.hooks_mutex);
    
    if (g_ctx.hooks_count >= VFS_MAX_HOOKS) {
        mutex_unlock(&g_ctx.hooks_mutex);
        return -ENOSPC;
    }
    
    /* 检查是否已存在 */
    struct vfs_hook_target *existing;
    list_for_each_entry(existing, &g_ctx.hooks, list) {
        if (existing->type == type) {
            if (type == VFS_HOOK_PID && existing->pid == safe_atoi(identifier)) {
                mutex_unlock(&g_ctx.hooks_mutex);
                return -EEXIST;
            }
            if (type == VFS_HOOK_PACKAGE && 
                strcmp(existing->package_name, identifier) == 0) {
                mutex_unlock(&g_ctx.hooks_mutex);
                return -EEXIST;
            }
        }
    }
    
    hook = kzalloc(sizeof(*hook), GFP_KERNEL);
    if (!hook) {
        mutex_unlock(&g_ctx.hooks_mutex);
        return -ENOMEM;
    }
    
    hook->type = type;
    hook->uid = uid;
    hook->mode = mode;
    hook->enabled = true;
    
    if (type == VFS_HOOK_PID) {
        hook->pid = safe_atoi(identifier);
    } else {
        strncpy(hook->package_name, identifier, VFS_MAX_PKG_LEN - 1);
    }
    
    INIT_LIST_HEAD(&hook->list);
    list_add_tail(&hook->list, &g_ctx.hooks);
    g_ctx.hooks_count++;
    
    mutex_unlock(&g_ctx.hooks_mutex);
    
    vfs_trace("hook added: type=%d, id=%s, uid=%u, mode=%d",
              type, identifier, uid, mode);
    return 0;
}

int vfs_hook_remove(enum vfs_hook_type type, const char *identifier)
{
    struct vfs_hook_target *hook, *tmp;
    
    mutex_lock(&g_ctx.hooks_mutex);
    
    list_for_each_entry_safe(hook, tmp, &g_ctx.hooks, list) {
        if (hook->type == type) {
            bool match = false;
            if (type == VFS_HOOK_PID && hook->pid == safe_atoi(identifier))
                match = true;
            if (type == VFS_HOOK_PACKAGE && 
                strcmp(hook->package_name, identifier) == 0)
                match = true;
            
            if (match) {
                list_del_init(&hook->list);
                g_ctx.hooks_count--;
                kfree(hook);
                mutex_unlock(&g_ctx.hooks_mutex);
                vfs_trace("hook removed: type=%d, id=%s", type, identifier);
                return 0;
            }
        }
    }
    
    mutex_unlock(&g_ctx.hooks_mutex);
    return -ENOENT;
}

struct vfs_hook_target *vfs_hook_check(pid_t pid, uid_t uid, unsigned int *mode_mask)
{
    struct vfs_hook_target *hook;
    
    mutex_lock(&g_ctx.hooks_mutex);
    
    list_for_each_entry(hook, &g_ctx.hooks, list) {
        if (!hook->enabled)
            continue;
        
        if (hook->type == VFS_HOOK_PID && hook->pid == pid) {
            *mode_mask = hook->mode;
            mutex_unlock(&g_ctx.hooks_mutex);
            return hook;
        }
        
        if (hook->type == VFS_HOOK_PACKAGE && hook->uid == uid) {
            *mode_mask = hook->mode;
            mutex_unlock(&g_ctx.hooks_mutex);
            return hook;
        }
    }
    
    mutex_unlock(&g_ctx.hooks_mutex);
    return NULL;
}

unsigned int vfs_hooks_clear(void)
{
    struct vfs_hook_target *hook, *tmp;
    unsigned int count = 0;
    
    mutex_lock(&g_ctx.hooks_mutex);
    
    list_for_each_entry_safe(hook, tmp, &g_ctx.hooks, list) {
        list_del_init(&hook->list);
        kfree(hook);
        count++;
    }
    
    g_ctx.hooks_count = 0;
    INIT_LIST_HEAD(&g_ctx.hooks);
    
    mutex_unlock(&g_ctx.hooks_mutex);
    
    vfs_trace("hooks cleared: %u", count);
    return count;
}

/* ==================== sysfs属性实现 ==================== */

/* stats (0444) */
static ssize_t stats_show(struct kobject *kobj, struct kobj_attribute *attr,
                          char *buf)
{
    return vfs_stats_get_string(buf, PAGE_SIZE);
}

/* stats_reset (0200) */
static ssize_t stats_reset_store(struct kobject *kobj, struct kobj_attribute *attr,
                                 const char *buf, size_t count)
{
    vfs_stats_reset();
    return count;
}

/* enabled (0644) */
static ssize_t enabled_show(struct kobject *kobj, struct kobj_attribute *attr,
                            char *buf)
{
    return sprintf(buf, "%d\n", g_ctx.policy.enabled ? 1 : 0);
}

static ssize_t enabled_store(struct kobject *kobj, struct kobj_attribute *attr,
                             const char *buf, size_t count)
{
    bool val;
    int ret = kstrtobool(buf, &val);
    if (ret)
        return ret;
    
    g_ctx.policy.enabled = val;
    vfs_trace("enabled set to %d", val);
    return count;
}

/* log_level (0644) */
static ssize_t log_level_show(struct kobject *kobj, struct kobj_attribute *attr,
                              char *buf)
{
    return sprintf(buf, "%u\n", g_ctx.policy.log_level);
}

static ssize_t log_level_store(struct kobject *kobj, struct kobj_attribute *attr,
                               const char *buf, size_t count)
{
    unsigned int val;
    int ret = kstrtouint(buf, 10, &val);
    if (ret)
        return ret;
    
    if (val > 5)
        return -EINVAL;
    
    g_ctx.policy.log_level = val;
    vfs_trace("log_level set to %u", val);
    return count;
}

/* default_action (0644) */
static ssize_t default_action_show(struct kobject *kobj, struct kobj_attribute *attr,
                                   char *buf)
{
    return sprintf(buf, "%s\n",
                   g_ctx.policy.default_action == VFS_ACTION_ALLOW ? "allow" : "deny");
}

static ssize_t default_action_store(struct kobject *kobj, struct kobj_attribute *attr,
                                    const char *buf, size_t count)
{
    if (strncmp(buf, "allow", 5) == 0)
        g_ctx.policy.default_action = VFS_ACTION_ALLOW;
    else if (strncmp(buf, "deny", 4) == 0)
        g_ctx.policy.default_action = VFS_ACTION_DENY;
    else
        return -EINVAL;
    
    vfs_trace("default_action set to %s",
              g_ctx.policy.default_action == VFS_ACTION_ALLOW ? "allow" : "deny");
    return count;
}

/* rules (0644) */
static ssize_t rules_show(struct kobject *kobj, struct kobj_attribute *attr,
                          char *buf)
{
    struct vfs_rule *rule;
    int len = 0;
    
    mutex_lock(&g_ctx.rules_mutex);
    
    list_for_each_entry(rule, &g_ctx.rules, list) {
        if (rule->enabled) {
            char mode_str[4] = "";
            if (rule->mode_mask & VFS_OP_READ) strcat(mode_str, "r");
            if (rule->mode_mask & VFS_OP_WRITE) strcat(mode_str, "w");
            
            len += sprintf(buf + len, "%s:%s:%s\n",
                          rule->action == VFS_ACTION_ALLOW ? "allow" : "deny",
                          rule->path_pattern,
                          mode_str);
            
            if (len >= PAGE_SIZE - 100)
                break;
        }
    }
    
    mutex_unlock(&g_ctx.rules_mutex);
    return len;
}

static ssize_t rules_store(struct kobject *kobj, struct kobj_attribute *attr,
                           const char *buf, size_t count)
{
    char *buf_copy, *line;
    int added = 0;
    
    buf_copy = kstrndup(buf, count, GFP_KERNEL);
    if (!buf_copy)
        return -ENOMEM;
    
    line = buf_copy;
    while (line) {
        char *next = strchr(line, '\n');
        if (next) *next++ = '\0';
        
        /* 跳过空行 */
        if (strlen(line) > 0) {
            struct vfs_rule *rule = vfs_rule_parse(line);
            if (rule) {
                int ret = vfs_rule_add(rule);
                if (ret == 0)
                    added++;
                else
                    kfree(rule);
            }
        }
        
        line = next;
    }
    
    kfree(buf_copy);
    vfs_trace("rules added: %d", added);
    return count;
}

/* rules_clear (0200) */
static ssize_t rules_clear_store(struct kobject *kobj, struct kobj_attribute *attr,
                                 const char *buf, size_t count)
{
    vfs_rules_clear();
    return count;
}

/* hook_targets (0644) */
static ssize_t hook_targets_show(struct kobject *kobj, struct kobj_attribute *attr,
                                 char *buf)
{
    struct vfs_hook_target *hook;
    int len = 0;
    
    mutex_lock(&g_ctx.hooks_mutex);
    
    list_for_each_entry(hook, &g_ctx.hooks, list) {
        const char *type_str = hook->type == VFS_HOOK_PID ? "PID" : "PACKAGE";
        const char *id_str = hook->type == VFS_HOOK_PID ? 
            kasprintf(GFP_KERNEL, "%d", hook->pid) : hook->package_name;
        const char *mode_str;
        
        switch (hook->mode) {
        case VFS_HOOK_MONITOR_ONLY: mode_str = "MONITOR_ONLY"; break;
        case VFS_HOOK_INTERCEPT_READ: mode_str = "INTERCEPT_READ"; break;
        case VFS_HOOK_INTERCEPT_WRITE: mode_str = "INTERCEPT_WRITE"; break;
        case VFS_HOOK_INTERCEPT_ALL: mode_str = "INTERCEPT_ALL"; break;
        default: mode_str = "UNKNOWN"; break;
        }
        
        len += sprintf(buf + len, "%s:%s:%u:%s:%d\n",
                      type_str, id_str, hook->uid, mode_str, hook->enabled ? 1 : 0);
        
        if (hook->type == VFS_HOOK_PID)
            kfree(id_str);
        
        if (len >= PAGE_SIZE - 100)
            break;
    }
    
    mutex_unlock(&g_ctx.hooks_mutex);
    return len;
}

static ssize_t hook_targets_store(struct kobject *kobj, struct kobj_attribute *attr,
                                  const char *buf, size_t count)
{
    char *buf_copy, *cmd, *type_str, *identifier, *uid_str, *mode_str;
    int ret = 0;
    
    buf_copy = kstrndup(buf, count, GFP_KERNEL);
    if (!buf_copy)
        return -ENOMEM;
    
    cmd = buf_copy;
    
    /* 解析格式: add:TYPE:identifier:uid:mode 或 remove:TYPE:identifier */
    if (strncmp(cmd, "add:", 4) == 0) {
        cmd += 4;
        type_str = cmd;
        identifier = strchr(cmd, ':');
        if (!identifier) { kfree(buf_copy); return -EINVAL; }
        *identifier++ = '\0';
        uid_str = strchr(identifier, ':');
        if (!uid_str) { kfree(buf_copy); return -EINVAL; }
        *uid_str++ = '\0';
        mode_str = strchr(uid_str, ':');
        if (!mode_str) { kfree(buf_copy); return -EINVAL; }
        *mode_str++ = '\0';
        
        enum vfs_hook_type type = strcmp(type_str, "PID") == 0 ? 
            VFS_HOOK_PID : VFS_HOOK_PACKAGE;
        uid_t uid = safe_atoi(uid_str);
        enum vfs_hook_mode mode;
        
        if (strcmp(mode_str, "MONITOR_ONLY") == 0) mode = VFS_HOOK_MONITOR_ONLY;
        else if (strcmp(mode_str, "INTERCEPT_READ") == 0) mode = VFS_HOOK_INTERCEPT_READ;
        else if (strcmp(mode_str, "INTERCEPT_WRITE") == 0) mode = VFS_HOOK_INTERCEPT_WRITE;
        else if (strcmp(mode_str, "INTERCEPT_ALL") == 0) mode = VFS_HOOK_INTERCEPT_ALL;
        else { kfree(buf_copy); return -EINVAL; }
        
        ret = vfs_hook_add(type, identifier, uid, mode);
    } else if (strncmp(cmd, "remove:", 7) == 0) {
        cmd += 7;
        type_str = cmd;
        identifier = strchr(cmd, ':');
        if (!identifier) { kfree(buf_copy); return -EINVAL; }
        *identifier++ = '\0';
        
        enum vfs_hook_type type = strcmp(type_str, "PID") == 0 ?
            VFS_HOOK_PID : VFS_HOOK_PACKAGE;
        
        ret = vfs_hook_remove(type, identifier);
    } else {
        kfree(buf_copy);
        return -EINVAL;
    }
    
    kfree(buf_copy);
    
    if (ret)
        return ret;
    
    return count;
}

/* hook_list (0444) - 冒号分隔格式，与用户层 getHookList() 兼容 */
static ssize_t hook_list_show(struct kobject *kobj, struct kobj_attribute *attr,
                              char *buf)
{
    struct vfs_hook_target *hook;
    int len = 0;
    
    mutex_lock(&g_ctx.hooks_mutex);
    
    list_for_each_entry(hook, &g_ctx.hooks, list) {
        const char *type_str = hook->type == VFS_HOOK_PID ? "PID" : "PACKAGE";
        const char *id_str = hook->type == VFS_HOOK_PID ?
            kasprintf(GFP_KERNEL, "%d", hook->pid) : hook->package_name;
        const char *mode_str;
        
        switch (hook->mode) {
        case VFS_HOOK_MONITOR_ONLY: mode_str = "MONITOR_ONLY"; break;
        case VFS_HOOK_INTERCEPT_READ: mode_str = "INTERCEPT_READ"; break;
        case VFS_HOOK_INTERCEPT_WRITE: mode_str = "INTERCEPT_WRITE"; break;
        case VFS_HOOK_INTERCEPT_ALL: mode_str = "INTERCEPT_ALL"; break;
        default: mode_str = "UNKNOWN"; break;
        }
        
        len += sprintf(buf + len, "%s:%s:%u:%s:%d\n",
                      type_str, id_str, hook->uid, mode_str,
                      hook->enabled ? 1 : 0);
        
        if (hook->type == VFS_HOOK_PID)
            kfree(id_str);
        
        if (len >= PAGE_SIZE - 100)
            break;
    }
    
    mutex_unlock(&g_ctx.hooks_mutex);
    return len;
}

/* version (0444) */
static ssize_t version_show(struct kobject *kobj, struct kobj_attribute *attr,
                            char *buf)
{
    return sprintf(buf, "%s\n", AURORA_VFS_VERSION);
}

/* ==================== sysfs属性定义 ==================== */

static struct kobj_attribute stats_attr = __ATTR(stats, 0444, stats_show, NULL);
static struct kobj_attribute stats_reset_attr = __ATTR(stats_reset, 0200, NULL, stats_reset_store);
static struct kobj_attribute enabled_attr = __ATTR(enabled, 0644, enabled_show, enabled_store);
static struct kobj_attribute log_level_attr = __ATTR(log_level, 0644, log_level_show, log_level_store);
static struct kobj_attribute default_action_attr = __ATTR(default_action, 0644, default_action_show, default_action_store);
static struct kobj_attribute rules_attr = __ATTR(rules, 0644, rules_show, rules_store);
static struct kobj_attribute rules_clear_attr = __ATTR(rules_clear, 0200, NULL, rules_clear_store);
static struct kobj_attribute hook_targets_attr = __ATTR(hook_targets, 0644, hook_targets_show, hook_targets_store);
static struct kobj_attribute hook_list_attr = __ATTR(hook_list, 0444, hook_list_show, NULL);
static struct kobj_attribute version_attr = __ATTR(version, 0444, version_show, NULL);

static struct attribute *vfs_attrs[] = {
    &stats_attr.attr,
    &stats_reset_attr.attr,
    &enabled_attr.attr,
    &log_level_attr.attr,
    &default_action_attr.attr,
    &rules_attr.attr,
    &rules_clear_attr.attr,
    &hook_targets_attr.attr,
    &hook_list_attr.attr,
    &version_attr.attr,
    NULL,
};

static struct attribute_group vfs_attr_group = {
    .attrs = vfs_attrs,
};

/* ==================== sysfs初始化 ==================== */

int vfs_sysfs_init(void)
{
    int ret;
    
    /* 创建 /sys/kernel/ztrosu 目录 */
    g_ctx.kobj_root = kobject_create_and_add(AURORA_SYSFS_ROOT, kernel_kobj);
    if (!g_ctx.kobj_root) {
        vfs_trace("failed to create ztrosu kobject");
        return -ENOMEM;
    }
    
    /* 创建 /sys/kernel/ztrosu/vfs 目录 */
    g_ctx.kobj_vfs = kobject_create_and_add(AURORA_SYSFS_DIR, g_ctx.kobj_root);
    if (!g_ctx.kobj_vfs) {
        vfs_trace("failed to create vfs kobject");
        kobject_put(g_ctx.kobj_root);
        g_ctx.kobj_root = NULL;
        return -ENOMEM;
    }
    
    /* 创建属性组 */
    ret = sysfs_create_group(g_ctx.kobj_vfs, &vfs_attr_group);
    if (ret) {
        vfs_trace("failed to create sysfs group: %d", ret);
        kobject_put(g_ctx.kobj_vfs);
        g_ctx.kobj_vfs = NULL;
        kobject_put(g_ctx.kobj_root);
        g_ctx.kobj_root = NULL;
        return ret;
    }
    
    vfs_trace("sysfs initialized at /sys/kernel/%s/%s", 
              AURORA_SYSFS_ROOT, AURORA_SYSFS_DIR);
    return 0;
}

void vfs_sysfs_exit(void)
{
    if (g_ctx.kobj_vfs) {
        sysfs_remove_group(g_ctx.kobj_vfs, &vfs_attr_group);
        kobject_put(g_ctx.kobj_vfs);
        g_ctx.kobj_vfs = NULL;
    }
    
    if (g_ctx.kobj_root) {
        kobject_put(g_ctx.kobj_root);
        g_ctx.kobj_root = NULL;
    }
    
    vfs_trace("sysfs exited");
}

/* ==================== LSM Hook 实现 ==================== */

/* 前向声明 */
static int aurora_security_file_open(struct file *file);
static int aurora_security_file_permission(struct file *file, int mask);
static int aurora_security_bprm_check(struct linux_binprm *bprm);

/* 安全Hook列表 (LSM框架) */
static struct security_hook_list aurora_hooks[] __lsm_ro_after_init = {
    LSM_HOOK_INIT(file_open, aurora_security_file_open),
    LSM_HOOK_INIT(file_permission, aurora_security_file_permission),
    LSM_HOOK_INIT(bprm_check_security, aurora_security_bprm_check),
};

/* 辅助函数：从 file 结构获取绝对路径 */
static int aurora_get_path_from_file(struct file *file, char *buf, size_t buflen)
{
    struct path *path;
    char *pathname = NULL;

    if (!file || !buf || buflen == 0)
        return -EINVAL;

    path = &file->f_path;
    pathname = d_path(path, buf, buflen);
    if (IS_ERR(pathname)) {
        return PTR_ERR(pathname);
    }

    /* d_path 可能返回 buf 中间位置，需要移动到开头 */
    if (pathname != buf) {
        size_t len = strlen(pathname);
        if (len >= buflen)
            len = buflen - 1;
        memmove(buf, pathname, len);
        buf[len] = '\0';
    }

    return 0;
}

/* 辅助函数：判断 mask 是否为写操作 */
static bool aurora_mask_is_write(int mask)
{
    return (mask & (MAY_WRITE | MAY_APPEND)) != 0;
}

/* 辅助函数：判断 mask 是否为读操作 */
static bool aurora_mask_is_read(int mask)
{
    return (mask & (MAY_READ | MAY_EXEC)) != 0;
}

/*
 * aurora_security_file_open - LSM file_open hook
 * 拦截文件打开操作，连接:
 *   - vfs_rules_check()   规则检查
 *   - vfs_hook_check()    Hook目标检查
 *   - vfs_stats_update()  统计更新
 *   - spoof_intercept_read() 身份伪装读拦截
 */
static int aurora_security_file_open(struct file *file)
{
    char path_buf[VFS_MAX_PATH_LEN];
    enum vfs_action action;
    struct vfs_hook_target *hook;
    unsigned int hook_mode = 0;
    pid_t pid = current->pid;
    uid_t uid = __kuid_val(current_uid());
    int ret = 0;

    if (!g_ctx.policy.enabled)
        return 0;

    /* 获取文件路径 */
    if (aurora_get_path_from_file(file, path_buf, sizeof(path_buf)) != 0)
        return 0;

    /* 检查当前进程是否在Hook列表中 */
    hook = vfs_hook_check(pid, uid, &hook_mode);

    /* 规则引擎检查 */
    action = vfs_rules_check(path_buf, VFS_OP_READ | VFS_OP_WRITE);

    if (action == VFS_ACTION_DENY) {
        vfs_stats_update(4); /* denied */
        vfs_netlink_send_event(EVENT_VFS_DENY, pid, uid, path_buf, 1);
        vfs_trace_level(1, "LSM DENY open: %s pid=%d uid=%d", path_buf, pid, uid);
        return -EPERM;
    }

    /* 身份伪装拦截 (读操作) */
    if (hook && (hook_mode & VFS_HOOK_INTERCEPT_READ)) {
        char spoof_buf[256];
        int spoof_len = spoof_intercept_read(path_buf, spoof_buf, sizeof(spoof_buf));
        if (spoof_len > 0) {
            vfs_trace_level(2, "LSM SPOOF read: %s pid=%d", path_buf, pid);
            /* 注意：这里仅记录，实际spoof在read路径处理 */
        }
    }

    /* 更新统计 */
    vfs_stats_update(0); /* open */
    vfs_netlink_send_event(EVENT_VFS_OPEN, pid, uid, path_buf, 0);

    vfs_trace_level(4, "LSM ALLOW open: %s pid=%d uid=%d", path_buf, pid, uid);
    return 0;
}

/*
 * aurora_security_file_permission - LSM file_permission hook
 * 拦截文件读写权限检查，连接:
 *   - vfs_rules_check()       规则检查
 *   - vfs_hook_check()        Hook目标检查
 *   - vfs_stats_update()      统计更新
 *   - partition_check_write() 受保护分区写检查
 */
static int aurora_security_file_permission(struct file *file, int mask)
{
    char path_buf[VFS_MAX_PATH_LEN];
    enum vfs_action action;
    struct vfs_hook_target *hook;
    unsigned int hook_mode = 0;
    pid_t pid = current->pid;
    uid_t uid = __kuid_val(current_uid());
    unsigned int mode_mask = 0;

    if (!g_ctx.policy.enabled)
        return 0;

    /* 获取文件路径 */
    if (aurora_get_path_from_file(file, path_buf, sizeof(path_buf)) != 0)
        return 0;

    /* 判断操作类型 */
    if (aurora_mask_is_read(mask))
        mode_mask |= VFS_OP_READ;
    if (aurora_mask_is_write(mask))
        mode_mask |= VFS_OP_WRITE;

    if (mode_mask == 0)
        return 0;

    /* 检查当前进程是否在Hook列表中 */
    hook = vfs_hook_check(pid, uid, &hook_mode);

    /* 规则引擎检查 */
    action = vfs_rules_check(path_buf, mode_mask);
    if (action == VFS_ACTION_DENY) {
        vfs_stats_update(4); /* denied */
        vfs_netlink_send_event(EVENT_VFS_DENY, pid, uid, path_buf, 1);
        vfs_trace_level(1, "LSM DENY %s: %s pid=%d uid=%d",
                        (mode_mask & VFS_OP_WRITE) ? "write" : "read",
                        path_buf, pid, uid);
        return -EPERM;
    }

    /* 受保护分区写检查 */
    if ((mode_mask & VFS_OP_WRITE) && !partition_check_write(path_buf, pid, uid)) {
        vfs_stats_update(4); /* denied */
        vfs_netlink_send_event(EVENT_VFS_DENY, pid, uid, path_buf, 1);
        vfs_trace_level(1, "LSM PARTITION DENY write: %s pid=%d uid=%d", path_buf, pid, uid);
        return -EPERM;
    }

    /* 更新统计 */
    if (mode_mask & VFS_OP_READ) {
        vfs_stats_update(1); /* read */
        vfs_netlink_send_event(EVENT_VFS_READ, pid, uid, path_buf, 0);
    }
    if (mode_mask & VFS_OP_WRITE) {
        vfs_stats_update(2); /* write */
        vfs_netlink_send_event(EVENT_VFS_WRITE, pid, uid, path_buf, 0);
    }

    vfs_trace_level(4, "LSM ALLOW %s: %s pid=%d uid=%d",
                    (mode_mask & VFS_OP_WRITE) ? "write" : "read",
                    path_buf, pid, uid);
    return 0;
}

/*
 * aurora_security_bprm_check - LSM bprm_check_security hook
 * 拦截 execve 执行，连接:
 *   - anti_brick_check_exec()    防格机检查
 *   - shell_audit_record_exec()  Shell执行审计
 */
static int aurora_security_bprm_check(struct linux_binprm *bprm)
{
    pid_t pid = current->pid;
    uid_t uid = __kuid_val(current_uid());
    int ret;
    const char *interp = NULL;

    if (!g_ctx.policy.enabled)
        return 0;

    /* 防格机检查 */
    ret = anti_brick_check_exec(bprm);
    if (ret != 0) {
        vfs_trace_level(1, "LSM ANTI_BRICK DENY exec pid=%d uid=%d", pid, uid);
        return ret;
    }

    /* Shell 执行审计 */
    if (bprm->file && bprm->file->f_path.dentry) {
        interp = bprm->file->f_path.dentry->d_name.name;
    }

    if (interp) {
        bool is_shell = (strstr(interp, "sh") != NULL ||
                         strstr(interp, "bash") != NULL ||
                         strstr(interp, "mksh") != NULL ||
                         strstr(interp, "zsh") != NULL ||
                         strstr(interp, "dash") != NULL);
        if (is_shell) {
            shell_audit_record_exec(pid, uid, interp, NULL, true);
            vfs_trace_level(2, "LSM SHELL AUDIT: %s pid=%d uid=%d", interp, pid, uid);
        }
    }

    vfs_trace_level(4, "LSM ALLOW exec pid=%d uid=%d", pid, uid);
    return 0;
}

/* ==================== kprobe 回退实现 ==================== */

#ifdef CONFIG_KPROBES

/* kprobe 用于 security_file_open */
static int kp_file_open_pre(struct kprobe *p, struct pt_regs *regs)
{
    struct file *file;

#if defined(CONFIG_X86_64)
    file = (struct file *)regs->di;
#elif defined(CONFIG_ARM64)
    file = (struct file *)regs->regs[0];
#else
    return 0;
#endif

    return aurora_security_file_open(file);
}

/* kprobe 用于 security_file_permission */
static int kp_file_perm_pre(struct kprobe *p, struct pt_regs *regs)
{
    struct file *file;
    int mask;

#if defined(CONFIG_X86_64)
    file = (struct file *)regs->di;
    mask = (int)regs->si;
#elif defined(CONFIG_ARM64)
    file = (struct file *)regs->regs[0];
    mask = (int)regs->regs[1];
#else
    return 0;
#endif

    return aurora_security_file_permission(file, mask);
}

/* kprobe 用于 security_bprm_check */
static int kp_bprm_check_pre(struct kprobe *p, struct pt_regs *regs)
{
    struct linux_binprm *bprm;

#if defined(CONFIG_X86_64)
    bprm = (struct linux_binprm *)regs->di;
#elif defined(CONFIG_ARM64)
    bprm = (struct linux_binprm *)regs->regs[0];
#else
    return 0;
#endif

    return aurora_security_bprm_check(bprm);
}

static struct kprobe kp_file_open = {
    .symbol_name = "security_file_open",
    .pre_handler = kp_file_open_pre,
};

static struct kprobe kp_file_perm = {
    .symbol_name = "security_file_permission",
    .pre_handler = kp_file_perm_pre,
};

static struct kprobe kp_bprm_check = {
    .symbol_name = "security_bprm_check",
    .pre_handler = kp_bprm_check_pre,
};

#endif /* CONFIG_KPROBES */

/*
 * aurora_lsm_hooks_init - 注册 LSM hooks
 * 优先尝试原生 LSM 框架，失败则回退到 kprobe
 */
int aurora_lsm_hooks_init(void)
{
    int ret = 0;

    vfs_trace("LSM hooks init: trying native LSM...");

    /* 尝试原生 LSM 注册 */
#if defined(CONFIG_SECURITY) && defined(security_add_hooks)
    {
        /* 内核 6.1+ 使用 security_add_hooks */
        security_add_hooks(aurora_hooks, ARRAY_SIZE(aurora_hooks), "aurora");
        g_lsm_method = LSM_HOOK_NATIVE;
        g_lsm_hooks_registered = true;
        vfs_trace("LSM hooks registered via native security_add_hooks");
        return 0;
    }
#else
    vfs_trace("Native LSM not available, falling back to kprobe");
#endif

    /* 回退到 kprobe */
#ifdef CONFIG_KPROBES
    g_lsm_method = LSM_HOOK_KPROBE;

    ret = register_kprobe(&kp_file_open);
    if (ret < 0) {
        vfs_trace("kprobe register failed for file_open: %d", ret);
        g_lsm_method = LSM_HOOK_NONE;
        return ret;
    }

    ret = register_kprobe(&kp_file_perm);
    if (ret < 0) {
        vfs_trace("kprobe register failed for file_permission: %d", ret);
        unregister_kprobe(&kp_file_open);
        g_lsm_method = LSM_HOOK_NONE;
        return ret;
    }

    ret = register_kprobe(&kp_bprm_check);
    if (ret < 0) {
        vfs_trace("kprobe register failed for bprm_check: %d", ret);
        unregister_kprobe(&kp_file_perm);
        unregister_kprobe(&kp_file_open);
        g_lsm_method = LSM_HOOK_NONE;
        return ret;
    }

    g_lsm_hooks_registered = true;
    vfs_trace("LSM hooks registered via kprobe fallback");
    return 0;
#else
    vfs_trace("ERROR: Neither LSM nor kprobe available, hooks not registered!");
    g_lsm_method = LSM_HOOK_NONE;
    return -ENODEV;
#endif
}

/*
 * aurora_lsm_hooks_exit - 注销 LSM hooks
 */
void aurora_lsm_hooks_exit(void)
{
    if (!g_lsm_hooks_registered)
        return;

    switch (g_lsm_method) {
    case LSM_HOOK_NATIVE:
        /* 原生 LSM 目前内核没有 unregister API，
         * 但模块卸载时会自动清理
         */
        vfs_trace("LSM native hooks: module unload will clean up");
        break;

    case LSM_HOOK_KPROBE:
#ifdef CONFIG_KPROBES
        unregister_kprobe(&kp_file_open);
        unregister_kprobe(&kp_file_perm);
        unregister_kprobe(&kp_bprm_check);
        vfs_trace("LSM kprobe hooks unregistered");
#endif
        break;

    default:
        break;
    }

    g_lsm_hooks_registered = false;
    g_lsm_method = LSM_HOOK_NONE;
}

/* ==================== 模块生命周期 ==================== */

static int __init aurora_vfs_init(void)
{
    int ret;
    
    vfs_trace_func();
    
    /* 初始化全局上下文 */
    memset(&g_ctx, 0, sizeof(g_ctx));
    
    /* 初始化链表 */
    INIT_LIST_HEAD(&g_ctx.rules);
    INIT_LIST_HEAD(&g_ctx.hooks);
    
    /* 初始化锁 */
    mutex_init(&g_ctx.rules_mutex);
    mutex_init(&g_ctx.hooks_mutex);
    spin_lock_init(&g_ctx.stats_lock);
    
    /* 初始化统计 */
    vfs_stats_init();
    
    /* 初始化策略 */
    g_ctx.policy.enabled = false;
    g_ctx.policy.log_level = 0;
    g_ctx.policy.default_action = VFS_ACTION_ALLOW;
    
    /* 初始化sysfs */
    ret = vfs_sysfs_init();
    if (ret) {
        mutex_destroy(&g_ctx.rules_mutex);
        mutex_destroy(&g_ctx.hooks_mutex);
        return ret;
    }
    
    /* 初始化 Pipe 通讯 */
    ret = vfs_pipe_init();
    if (ret) {
        vfs_sysfs_exit();
        mutex_destroy(&g_ctx.rules_mutex);
        mutex_destroy(&g_ctx.hooks_mutex);
        return ret;
    }

    /* 初始化安全审计模块 */
    ret = security_audit_init();
    if (ret) {
        vfs_trace("WARNING: security audit init failed (%d), continuing", ret);
        /* 非致命错误，继续运行 */
    }
    
    /* 初始化 Netlink 事件推送 */
    ret = vfs_netlink_init();
    if (ret) {
        vfs_pipe_exit();
        vfs_sysfs_exit();
        mutex_destroy(&g_ctx.rules_mutex);
        mutex_destroy(&g_ctx.hooks_mutex);
        return ret;
    }

    /* 初始化防格机模块 */
    ret = anti_brick_init();
    if (ret) {
        vfs_trace("WARNING: anti-brick init failed (%d), continuing", ret);
    }

    /* 初始化身份伪装模块 */
    ret = identity_spoof_init();
    if (ret) {
        vfs_trace("WARNING: identity spoof init failed (%d), continuing", ret);
    }

    /* 初始化 VFS 事件节点 */
    ret = vfs_events_init();
    if (ret) {
        vfs_trace("WARNING: events init failed (%d), continuing", ret);
    }

    /* 注册 LSM Hooks (必须在其他模块初始化之后) */
    ret = aurora_lsm_hooks_init();
    if (ret) {
        vfs_trace("ERROR: LSM hooks registration failed (%d)", ret);
        /* LSM 注册失败不阻止模块加载，但记录错误 */
    }

    g_ctx.initialized = true;

    vfs_trace("module loaded, version=%s, lsm_method=%d", AURORA_VFS_VERSION, g_lsm_method);
    return 0;
}

static void __exit aurora_vfs_exit(void)
{
    vfs_trace_func();

    g_ctx.initialized = false;

    /* 先注销 LSM Hooks (停止拦截) */
    aurora_lsm_hooks_exit();

    /* 注销 Netlink 事件推送 */
    vfs_netlink_exit();

    /* 注销防格机 */
    anti_brick_exit();

    /* 注销身份伪装 */
    identity_spoof_exit();

    /* 注销 VFS 事件节点 */
    vfs_events_exit();

    /* 注销 Pipe 通讯 */
    vfs_pipe_exit();

    /* 注销安全审计 */
    security_audit_exit();

    /* 注销sysfs */
    vfs_sysfs_exit();

    /* 清理规则 */
    vfs_rules_clear();

    /* 清理Hook */
    vfs_hooks_clear();

    /* 销毁锁 */
    mutex_destroy(&g_ctx.rules_mutex);
    mutex_destroy(&g_ctx.hooks_mutex);

    vfs_trace("module unloaded");
}

module_init(aurora_vfs_init);
module_exit(aurora_vfs_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR(AURORA_VFS_AUTHOR);
MODULE_DESCRIPTION(AURORA_VFS_DESC);
MODULE_VERSION(AURORA_VFS_VERSION);