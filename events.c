/* SPDX-License-Identifier: GPL-2.0 */
/*
 * events.c - AuroraSU VFS 事件节点
 *
 * 提供二进制事件流供用户层轮询
 * sysfs 路径: /sys/kernel/ztrosu/vfs/events
 *
 * 事件协议 (little-endian):
 *   Magic     : __u32 (0xAF5F)
 *   EventType : __u32
 *   PID       : __u32
 *   UID       : __u32
 *   PathLen   : __u32
 *   Path      : char[PathLen]
 *   Timestamp : __u64
 *   Result    : __u32
 */

#include "phantom_lkm.h"
#include <linux/slab.h>

/* ==================== 常量定义 ==================== */

#define EVENT_RING_SIZE       64      /* 环形缓冲区大小 */
#define EVENT_MAX_PATH_LEN   512     /* 单条事件路径最大长度 */

/* ==================== 数据结构 ==================== */

/* 单条事件记录 */
struct event_record {
    __u32 magic;
    __u32 event_type;
    __u32 pid;
    __u32 uid;
    __u32 path_len;
    char  path[EVENT_MAX_PATH_LEN];
    __u64 timestamp;
    __u32 result;
};

/* 事件环形缓冲区 */
static struct event_record event_ring[EVENT_RING_SIZE];
static atomic_t event_head;
static spinlock_t event_lock;

/* ==================== 事件写入 ==================== */

/**
 * vfs_event_push - 推送一条事件到环形缓冲区
 * 同时通过 Netlink 发送
 */
void vfs_event_push(__u32 event_type, __u32 pid, __u32 uid,
                    const char *path, __u32 result)
{
    unsigned long flags;
    int head;

    if (!g_ctx.initialized)
        return;

    /* 通过 Netlink 发送事件 */
    vfs_netlink_send_event(event_type, pid, uid, path, result);

    /* 写入环形缓冲区 */
    spin_lock_irqsave(&event_lock, flags);
    head = atomic_inc_return(&event_head) % EVENT_RING_SIZE;

    {
        struct event_record *rec = &event_ring[head];

        memset(rec, 0, sizeof(*rec));
        rec->magic = VFS_CMD_MAGIC;
        rec->event_type = event_type;
        rec->pid = pid;
        rec->uid = uid;
        rec->timestamp = ktime_get_real_ns();
        rec->result = result;

        if (path) {
            size_t len = strlen(path);
            if (len > EVENT_MAX_PATH_LEN - 1)
                len = EVENT_MAX_PATH_LEN - 1;
            memcpy(rec->path, path, len);
            rec->path_len = (__u32)len;
        }
    }

    spin_unlock_irqrestore(&event_lock, flags);
}

/* ==================== sysfs events 属性 ==================== */

/**
 * events_show - 输出二进制事件流
 *
 * 从最新到最旧遍历环形缓冲区，序列化为二进制格式
 * 与用户层 VFSNetlinkListener.parseEventStream() 兼容
 */
static ssize_t events_show(struct kobject *kobj,
                            struct kobj_attribute *attr, char *buf)
{
    int i;
    ssize_t len = 0;
    unsigned long flags;

    spin_lock_irqsave(&event_lock, flags);

    /* 从最新到最旧 */
    for (i = 0; i < EVENT_RING_SIZE; i++) {
        int idx = (atomic_read(&event_head) - i + EVENT_RING_SIZE) %
                  EVENT_RING_SIZE;
        struct event_record *rec = &event_ring[idx];

        /* 跳过空记录 */
        if (rec->magic != VFS_CMD_MAGIC)
            continue;

        /* 检查缓冲区空间 */
        /* 每条事件: 5*4 + path_len + 8 + 4 = 32 + path_len */
        if (len + 32 + rec->path_len > PAGE_SIZE)
            break;

        /* 序列化 (little-endian, 直接内存拷贝) */
        memcpy(buf + len, &rec->magic, sizeof(__u32));
        len += sizeof(__u32);
        memcpy(buf + len, &rec->event_type, sizeof(__u32));
        len += sizeof(__u32);
        memcpy(buf + len, &rec->pid, sizeof(__u32));
        len += sizeof(__u32);
        memcpy(buf + len, &rec->uid, sizeof(__u32));
        len += sizeof(__u32);
        memcpy(buf + len, &rec->path_len, sizeof(__u32));
        len += sizeof(__u32);
        if (rec->path_len > 0) {
            memcpy(buf + len, rec->path, rec->path_len);
            len += rec->path_len;
        }
        memcpy(buf + len, &rec->timestamp, sizeof(__u64));
        len += sizeof(__u64);
        memcpy(buf + len, &rec->result, sizeof(__u32));
        len += sizeof(__u32);
    }

    spin_unlock_irqrestore(&event_lock, flags);
    return len;
}

static struct kobj_attribute events_attr =
    __ATTR(events, 0444, events_show, NULL);

/* ==================== 初始化 / 退出 ==================== */

int vfs_events_init(void)
{
    int ret;

    memset(event_ring, 0, sizeof(event_ring));
    atomic_set(&event_head, 0);
    spin_lock_init(&event_lock);

    /* 在 /sys/kernel/ztrosu/vfs/ 下创建 events 属性 */
    ret = sysfs_create_file(g_ctx.kobj_vfs, &events_attr.attr);
    if (ret) {
        pr_err("[aurora_vfs] failed to create events sysfs: %d\n", ret);
        return ret;
    }

    pr_info("[aurora_vfs] events node initialized\n");
    return 0;
}

void vfs_events_exit(void)
{
    sysfs_remove_file(g_ctx.kobj_vfs, &events_attr.attr);
    pr_info("[aurora_vfs] events node removed\n");
}
