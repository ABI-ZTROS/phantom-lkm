/* SPDX-License-Identifier: GPL-2.0 */
/*
 * phantom_lkm.h - 教学研究用内核模块头文件
 * 研究链表动态摘除操作及sysfs节点注销机制
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
#include <linux/types.h>
#include <linux/trace_printk.h>

/* 模块信息 */
#define PHANTOM_LKM_NAME        "phantom_lkm"
#define PHANTOM_LKM_VERSION     "1.0.0"
#define PHANTOM_LKM_AUTHOR      "AuroraSU Research"
#define PHANTOM_LKM_DESC        "链表动态摘除与sysfs节点管理教学研究模块"

/* sysfs目录名称 */
#define PHANTOM_SYSFS_DIR       "phantom_lkm"

/* 节点ID范围 */
#define PHANTOM_NODE_ID_MIN     1
#define PHANTOM_NODE_ID_MAX     65535

/* 自定义数据结构 - 链表节点 */
struct phantom_node {
    struct list_head    list;       /* 链表节点 */
    u32                 id;         /* 节点唯一标识 */
    u64                 timestamp;  /* 创建时间戳 */
    u32                 data_len;   /* 数据长度 */
    void                *data;      /* 动态分配的数据缓冲区 */
};

/* 模块全局状态结构 */
struct phantom_state {
    struct kobject      *kobj;          /* sysfs根kobject */
    struct list_head    node_list;      /* 链表头 */
    struct mutex        list_mutex;     /* 链表操作互斥锁 */
    u32                 node_count;     /* 当前节点数量 */
    bool                initialized;    /* 模块初始化标志 */
};

/* 属性组结构前向声明 */
struct phantom_attr_group;

/* ==================== 链表操作接口 ==================== */

/**
 * phantom_node_create - 创建新节点
 * @id: 节点ID
 * @data_len: 数据缓冲区大小
 * @return: 成功返回节点指针，失败返回NULL
 */
struct phantom_node *phantom_node_create(u32 id, u32 data_len);

/**
 * phantom_node_destroy - 销毁节点并释放内存
 * @node: 要销毁的节点
 */
void phantom_node_destroy(struct phantom_node *node);

/**
 * phantom_node_add - 添加节点到链表
 * @node: 要添加的节点
 * @return: 成功返回0，失败返回错误码
 */
int phantom_node_add(struct phantom_node *node);

/**
 * phantom_node_remove_by_id - 根据ID删除节点
 * @id: 要删除的节点ID
 * @return: 成功返回0，未找到返回-ENOENT
 */
int phantom_node_remove_by_id(u32 id);

/**
 * phantom_node_find_by_id - 根据ID查找节点
 * @id: 要查找的节点ID
 * @return: 找到返回节点指针，未找到返回NULL
 */
struct phantom_node *phantom_node_find_by_id(u32 id);

/**
 * phantom_list_clear_all - 清空所有节点
 * @return: 删除的节点数量
 */
u32 phantom_list_clear_all(void);

/**
 * phantom_list_cleanup - 模块卸载时清理所有节点
 * 使用list_del_init()确保节点安全移除
 */
void phantom_list_cleanup(void);

/* ==================== sysfs属性接口 ==================== */

/**
 * phantom_sysfs_init - 初始化sysfs节点
 * @return: 成功返回0，失败返回错误码
 */
int phantom_sysfs_init(void);

/**
 * phantom_sysfs_exit - 注销所有sysfs节点
 */
void phantom_sysfs_exit(void);

/* ==================== 调试输出宏 ==================== */

/* 仅使用trace_printk进行非持久化输出 */
#define phantom_trace(fmt, ...) \
    trace_printk("[phantom_lkm] " fmt "\n", ##__VA_ARGS__)

#define phantom_trace_func() \
    trace_printk("[phantom_lkm] %s\n", __func__)

#define phantom_trace_node(node, action) \
    trace_printk("[phantom_lkm] %s node: id=%u, ts=%llu, data_len=%u\n", \
                 action, (node)->id, (node)->timestamp, (node)->data_len)

#endif /* _PHANTOM_LKM_H_ */
