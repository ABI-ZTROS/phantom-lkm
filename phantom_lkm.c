/* SPDX-License-Identifier: GPL-2.0 */
/*
 * phantom_lkm.c - AuroraSU VFS 内核模块
 * 对齐 VFS_KERNEL_MODULE_SPEC.md v3.0 规范
 *
 * 目标平台: OnePlus ACE5 (SM8650), 内核 6.1.141, Android 14 GKI
 */

#include "phantom_lkm.h"

/* ==================== 全局上下文 ==================== */

static struct vfs_debug_ctx g_ctx;

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
    
    g_ctx.initialized = true;
    
    vfs_trace("module loaded, version=%s", AURORA_VFS_VERSION);
    return 0;
}

static void __exit aurora_vfs_exit(void)
{
    vfs_trace_func();
    
    g_ctx.initialized = false;
    
    /* 注销 Pipe 通讯 */
    vfs_pipe_exit();
    
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