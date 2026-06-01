/* SPDX-License-Identifier: GPL-2.0 */
/*
 * phantom_lkm.c - 教学研究用内核模块
 * 研究链表动态摘除操作及sysfs节点注销机制
 *
 * 目标平台: OnePlus ACE5 (SM8650), 内核 6.1.141, Android 14 GKI
 */

#include "phantom_lkm.h"

/* ==================== 全局状态 ==================== */

static struct phantom_state g_state = {
    .kobj = NULL,
    .node_list = LIST_HEAD_INIT(g_state.node_list),
    .list_mutex = __MUTEX_INITIALIZER(g_state.list_mutex),
    .node_count = 0,
    .initialized = false,
};

/* ==================== 链表操作实现 ==================== */

/**
 * phantom_node_create - 创建新节点
 * 使用kzalloc分配内存，确保内存清零
 */
struct phantom_node *phantom_node_create(u32 id, u32 data_len)
{
    struct phantom_node *node;
    void *data_buf = NULL;

    phantom_trace_func();

    /* 参数检查 */
    if (id < PHANTOM_NODE_ID_MIN || id > PHANTOM_NODE_ID_MAX) {
        phantom_trace("invalid node id: %u", id);
        return NULL;
    }

    /* 分配节点结构体内存 */
    node = kzalloc(sizeof(*node), GFP_KERNEL);
    if (!node) {
        phantom_trace("failed to allocate node structure");
        return NULL;
    }

    /* 分配数据缓冲区 */
    if (data_len > 0) {
        data_buf = kzalloc(data_len, GFP_KERNEL);
        if (!data_buf) {
            phantom_trace("failed to allocate data buffer, len=%u", data_len);
            kfree(node);
            return NULL;
        }
    }

    /* 初始化节点 */
    INIT_LIST_HEAD(&node->list);
    node->id = id;
    node->timestamp = ktime_get_ns();
    node->data_len = data_len;
    node->data = data_buf;

    phantom_trace_node(node, "created");

    return node;
}

/**
 * phantom_node_destroy - 销毁节点并释放内存
 * 安全释放节点及其数据缓冲区
 */
void phantom_node_destroy(struct phantom_node *node)
{
    if (!node)
        return;

    phantom_trace_func();
    phantom_trace_node(node, "destroying");

    /* 确保节点已从链表中移除 */
    if (!list_empty(&node->list)) {
        phantom_trace("warning: node still in list, removing");
        list_del_init(&node->list);
    }

    /* 释放数据缓冲区 */
    if (node->data) {
        kfree(node->data);
        node->data = NULL;
    }

    /* 释放节点结构体 */
    kfree(node);
}

/**
 * phantom_node_add - 添加节点到链表
 * 使用互斥锁保护链表操作
 */
int phantom_node_add(struct phantom_node *node)
{
    int ret = 0;

    if (!node)
        return -EINVAL;

    phantom_trace_func();

    mutex_lock(&g_state.list_mutex);

    /* 检查ID是否已存在 */
    struct phantom_node *existing;
    list_for_each_entry(existing, &g_state.node_list, list) {
        if (existing->id == node->id) {
            phantom_trace("node id %u already exists", node->id);
            ret = -EEXIST;
            goto unlock;
        }
    }

    /* 添加到链表尾部 */
    list_add_tail(&node->list, &g_state.node_list);
    g_state.node_count++;

    phantom_trace_node(node, "added to list");
    phantom_trace("total nodes: %u", g_state.node_count);

unlock:
    mutex_unlock(&g_state.list_mutex);
    return ret;
}

/**
 * phantom_node_remove_by_id - 根据ID删除节点
 * 使用list_del_init()确保节点安全移除
 */
int phantom_node_remove_by_id(u32 id)
{
    struct phantom_node *node, *tmp;
    int ret = -ENOENT;

    phantom_trace("removing node id=%u", id);

    mutex_lock(&g_state.list_mutex);

    list_for_each_entry_safe(node, tmp, &g_state.node_list, list) {
        if (node->id == id) {
            /* 使用list_del_init()安全移除节点 */
            list_del_init(&node->list);
            g_state.node_count--;

            phantom_trace_node(node, "removed from list");
            phantom_trace("total nodes: %u", g_state.node_count);

            /* 释放节点内存 */
            phantom_node_destroy(node);

            ret = 0;
            break;
        }
    }

    mutex_unlock(&g_state.list_mutex);
    return ret;
}

/**
 * phantom_node_find_by_id - 根据ID查找节点
 * 内部使用，调用者需持有锁
 */
struct phantom_node *phantom_node_find_by_id(u32 id)
{
    struct phantom_node *node;

    list_for_each_entry(node, &g_state.node_list, list) {
        if (node->id == id)
            return node;
    }

    return NULL;
}

/**
 * phantom_list_clear_all - 清空所有节点
 * 返回删除的节点数量
 */
u32 phantom_list_clear_all(void)
{
    struct phantom_node *node, *tmp;
    u32 count = 0;

    phantom_trace_func();

    mutex_lock(&g_state.list_mutex);

    list_for_each_entry_safe(node, tmp, &g_state.node_list, list) {
        /* 使用list_del_init()安全移除节点 */
        list_del_init(&node->list);
        count++;

        phantom_trace_node(node, "cleared");

        /* 释放节点内存 */
        if (node->data) {
            kfree(node->data);
            node->data = NULL;
        }
        kfree(node);
    }

    g_state.node_count = 0;
    INIT_LIST_HEAD(&g_state.node_list);

    mutex_unlock(&g_state.list_mutex);

    phantom_trace("cleared %u nodes", count);
    return count;
}

/**
 * phantom_list_cleanup - 模块卸载时清理所有节点
 * 研究重点：使用list_del_init()确保节点安全移除
 */
void phantom_list_cleanup(void)
{
    struct phantom_node *node, *tmp;
    u32 count = 0;

    phantom_trace_func();

    /* 遍历并摘除所有节点 */
    mutex_lock(&g_state.list_mutex);

    /*
     * 使用list_for_each_entry_safe进行安全遍历
     * 使用list_del_init()将节点从链表中移除并重新初始化
     * 这是研究重点：确保节点安全移除，list_empty()返回true
     */
    list_for_each_entry_safe(node, tmp, &g_state.node_list, list) {
        /* list_del_init() - 从链表中删除节点并重新初始化list_head */
        list_del_init(&node->list);
        count++;

        phantom_trace_node(node, "detached");

        /* 验证节点已安全移除 */
        if (list_empty(&node->list)) {
            phantom_trace("node %u list_empty confirmed", node->id);
        }

        /* 释放节点资源 */
        if (node->data) {
            kfree(node->data);
            node->data = NULL;
        }
        kfree(node);
    }

    /* 重新初始化链表头 */
    INIT_LIST_HEAD(&g_state.node_list);
    g_state.node_count = 0;

    mutex_unlock(&g_state.list_mutex);

    phantom_trace("cleanup completed, %u nodes freed", count);
}

/* ==================== sysfs属性实现 ==================== */

/* 属性声明 */
static ssize_t status_show(struct kobject *kobj, struct kobj_attribute *attr,
                           char *buf);
static ssize_t node_count_show(struct kobject *kobj, struct kobj_attribute *attr,
                               char *buf);
static ssize_t add_node_store(struct kobject *kobj, struct kobj_attribute *attr,
                              const char *buf, size_t count);
static ssize_t remove_node_store(struct kobject *kobj, struct kobj_attribute *attr,
                                 const char *buf, size_t count);
static ssize_t clear_nodes_store(struct kobject *kobj, struct kobj_attribute *attr,
                                 const char *buf, size_t count);

/* 属性定义 */
static struct kobj_attribute status_attr =
    __ATTR(status, 0444, status_show, NULL);

static struct kobj_attribute node_count_attr =
    __ATTR(node_count, 0444, node_count_show, NULL);

static struct kobj_attribute add_node_attr =
    __ATTR(add_node, 0200, NULL, add_node_store);

static struct kobj_attribute remove_node_attr =
    __ATTR(remove_node, 0200, NULL, remove_node_store);

static struct kobj_attribute clear_nodes_attr =
    __ATTR(clear_nodes, 0200, NULL, clear_nodes_store);

/* 属性组 */
static struct attribute *phantom_attrs[] = {
    &status_attr.attr,
    &node_count_attr.attr,
    &add_node_attr.attr,
    &remove_node_attr.attr,
    &clear_nodes_attr.attr,
    NULL,
};

static struct attribute_group phantom_attr_group = {
    .attrs = phantom_attrs,
};

/**
 * status_show - 显示模块状态
 * 格式: status: <initialized>, version: <version>, nodes: <count>
 */
static ssize_t status_show(struct kobject *kobj, struct kobj_attribute *attr,
                           char *buf)
{
    phantom_trace("status_show called");

    return sprintf(buf, "status: %s\nversion: %s\nnodes: %u\n",
                   g_state.initialized ? "initialized" : "uninitialized",
                   PHANTOM_LKM_VERSION,
                   g_state.node_count);
}

/**
 * node_count_show - 显示链表节点数量
 */
static ssize_t node_count_show(struct kobject *kobj, struct kobj_attribute *attr,
                               char *buf)
{
    phantom_trace("node_count_show called, count=%u", g_state.node_count);

    return sprintf(buf, "%u\n", g_state.node_count);
}

/**
 * add_node_store - 添加新节点
 * 输入格式: "<id> [<data_len>]"
 * 例如: "100" 或 "100 256"
 */
static ssize_t add_node_store(struct kobject *kobj, struct kobj_attribute *attr,
                              const char *buf, size_t count)
{
    u32 id, data_len = 0;
    struct phantom_node *node;
    int ret;

    phantom_trace("add_node_store called, input=%s", buf);

    /* 解析输入 */
    ret = sscanf(buf, "%u %u", &id, &data_len);
    if (ret < 1) {
        phantom_trace("invalid input format");
        return -EINVAL;
    }

    /* 限制数据长度 */
    if (data_len > 4096) {
        phantom_trace("data_len too large: %u", data_len);
        return -EINVAL;
    }

    /* 创建节点 */
    node = phantom_node_create(id, data_len);
    if (!node) {
        phantom_trace("failed to create node %u", id);
        return -ENOMEM;
    }

    /* 添加到链表 */
    ret = phantom_node_add(node);
    if (ret) {
        phantom_trace("failed to add node %u, ret=%d", id, ret);
        phantom_node_destroy(node);
        return ret;
    }

    phantom_trace("node %u added successfully", id);
    return count;
}

/**
 * remove_node_store - 删除指定节点
 * 输入格式: "<id>"
 */
static ssize_t remove_node_store(struct kobject *kobj, struct kobj_attribute *attr,
                                 const char *buf, size_t count)
{
    u32 id;
    int ret;

    phantom_trace("remove_node_store called, input=%s", buf);

    /* 解析输入 */
    ret = sscanf(buf, "%u", &id);
    if (ret != 1) {
        phantom_trace("invalid input format");
        return -EINVAL;
    }

    /* 删除节点 */
    ret = phantom_node_remove_by_id(id);
    if (ret) {
        phantom_trace("node %u not found", id);
        return ret;
    }

    phantom_trace("node %u removed successfully", id);
    return count;
}

/**
 * clear_nodes_store - 清空所有节点
 * 输入: 任意非空内容触发清空
 */
static ssize_t clear_nodes_store(struct kobject *kobj, struct kobj_attribute *attr,
                                 const char *buf, size_t count)
{
    u32 cleared;

    phantom_trace("clear_nodes_store called");

    cleared = phantom_list_clear_all();
    phantom_trace("cleared %u nodes", cleared);

    return count;
}

/**
 * phantom_sysfs_init - 初始化sysfs节点
 * 创建 /sys/kernel/phantom_lkm/ 目录及属性文件
 */
int phantom_sysfs_init(void)
{
    int ret;

    phantom_trace_func();

    /* 在 /sys/kernel/ 下创建目录 */
    g_state.kobj = kobject_create_and_add(PHANTOM_SYSFS_DIR, kernel_kobj);
    if (!g_state.kobj) {
        phantom_trace("failed to create kobject");
        return -ENOMEM;
    }

    /* 创建属性组 */
    ret = sysfs_create_group(g_state.kobj, &phantom_attr_group);
    if (ret) {
        phantom_trace("failed to create sysfs group, ret=%d", ret);
        kobject_put(g_state.kobj);
        g_state.kobj = NULL;
        return ret;
    }

    phantom_trace("sysfs initialized at /sys/kernel/%s", PHANTOM_SYSFS_DIR);
    return 0;
}

/**
 * phantom_sysfs_exit - 注销所有sysfs节点
 * 研究重点：安全注销sysfs节点，避免use-after-free
 */
void phantom_sysfs_exit(void)
{
    phantom_trace_func();

    if (g_state.kobj) {
        /*
         * 先移除属性组，再释放kobject
         * 这是研究重点：确保sysfs节点在kobject释放前已注销
         */
        sysfs_remove_group(g_state.kobj, &phantom_attr_group);
        phantom_trace("sysfs group removed");

        /* 释放kobject引用 */
        kobject_put(g_state.kobj);
        g_state.kobj = NULL;

        phantom_trace("kobject released");
    }
}

/* ==================== 模块生命周期 ==================== */

static int __init phantom_lkm_init(void)
{
    int ret;

    /* 注意：模块加载时使用trace_printk可能看不到输出，
     * 因为tracing可能尚未完全初始化。
     * 这里仅作示例，实际输出可通过/sys/kernel/tracing/trace查看 */

    /* 初始化链表 */
    INIT_LIST_HEAD(&g_state.node_list);
    mutex_init(&g_state.list_mutex);
    g_state.node_count = 0;

    /* 初始化sysfs */
    ret = phantom_sysfs_init();
    if (ret) {
        return ret;
    }

    g_state.initialized = true;

    /* 此输出在模块加载时可能不可见，需通过trace查看 */
    phantom_trace("module initialized, version=%s", PHANTOM_LKM_VERSION);

    return 0;
}

static void __exit phantom_lkm_exit(void)
{
    phantom_trace_func();

    g_state.initialized = false;

    /*
     * 清理顺序很重要：
     * 1. 先注销sysfs节点（防止用户空间继续访问）
     * 2. 再清理链表节点（释放所有内存）
     *
     * 这是研究重点：确保资源释放顺序正确，避免竞态条件
     */

    /* 步骤1: 注销sysfs节点 */
    phantom_sysfs_exit();

    /* 步骤2: 清理链表节点 */
    phantom_list_cleanup();

    /* 步骤3: 销毁互斥锁 */
    mutex_destroy(&g_state.list_mutex);

    phantom_trace("module exited cleanly");
}

module_init(phantom_lkm_init);
module_exit(phantom_lkm_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR(PHANTOM_LKM_AUTHOR);
MODULE_DESCRIPTION(PHANTOM_LKM_DESC);
MODULE_VERSION(PHANTOM_LKM_VERSION);
