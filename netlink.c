/* SPDX-License-Identifier: GPL-2.0 */
/*
 * netlink.c - AuroraSU VFS Netlink 事件推送实现
 * 对齐 VFS_KERNEL_MODULE_SPEC.md v3.0 规范
 *
 * 使用 NETLINK_USERSOCK 协议族，多播组31
 * 向用户空间异步推送 VFS 操作事件
 */

#include "phantom_lkm.h"
#include <linux/cred.h>       /* current_cred, uid_eq, GLOBAL_ROOT_UID */

/* ==================== Netlink 接收回调 ==================== */

/**
 * aurora_vfs_nl_rcv_msg - Netlink 消息接收回调
 *
 * 目前内核模块只推送事件，不处理用户空间发来的 Netlink 消息。
 * 但需要实现此回调以支持安全检查和 CAP_ACK 确认。
 */
static int aurora_vfs_nl_rcv_msg(struct sk_buff *skb)
{
    struct nlmsghdr *nlh;
    const struct cred *cred;

    if (!skb) {
        vfs_trace("netlink: received NULL skb");
        return -EINVAL;
    }

    nlh = nlmsg_hdr(skb);
    if (!nlh) {
        vfs_trace("netlink: received NULL nlh");
        return -EINVAL;
    }

    /* 安全检查：仅允许 root (UID=0) 连接 */
    cred = current_cred();
    if (!uid_eq(cred->uid, GLOBAL_ROOT_UID)) {
        vfs_trace("netlink: rejected message from non-root uid=%u",
                  __kuid_val(cred->uid));
        return -EPERM;
    }

    /* 目前不处理用户空间发来的命令，仅确认接收 */
    vfs_trace("netlink: received message from root, type=%u, len=%u",
              nlh->nlmsg_type, nlh->nlmsg_len);

    return 0;
}

/* ==================== Netlink 配置 ==================== */

static struct netlink_kernel_cfg nl_cfg = {
    .input  = aurora_vfs_nl_rcv_msg,
    .groups = AURORA_VFS_NL_GROUP,  /* 多播组31 */
};

/* ==================== 接口函数 ==================== */

/**
 * vfs_netlink_init - 创建 Netlink socket
 */
int vfs_netlink_init(void)
{
    struct net *net;

    /* 使用 init_net 命名空间 */
    g_ctx.nlsk = netlink_kernel_create(&init_net,
                                        AURORA_VFS_NL_FAMILY,
                                        &nl_cfg);
    if (!g_ctx.nlsk) {
        vfs_trace("netlink: failed to create netlink socket");
        return -ENOMEM;
    }

    vfs_trace("netlink: socket created, family=%d, group=%d",
              AURORA_VFS_NL_FAMILY, AURORA_VFS_NL_GROUP);
    return 0;
}

/**
 * vfs_netlink_exit - 释放 Netlink socket
 */
void vfs_netlink_exit(void)
{
    if (g_ctx.nlsk) {
        netlink_kernel_release(g_ctx.nlsk);
        g_ctx.nlsk = NULL;
        vfs_trace("netlink: socket released");
    }
}

/**
 * vfs_netlink_send_event - 发送 VFS 事件到用户空间
 *
 * 根据 log_level 过滤事件：
 *   0 = 不发送任何事件
 *   1 = 仅发送 EVENT_VFS_DENY
 *   2 = EVENT_VFS_DENY + 管理事件
 *   3 = 所有 VFS 操作事件 + 管理事件
 *   4-5 = 所有事件
 */
void vfs_netlink_send_event(u32 event_type, u32 pid, u32 uid,
                            const char *path, u32 result)
{
    struct sk_buff *skb;
    struct nlmsghdr *nlh;
    void *data;
    u32 path_len = 0;
    u32 payload_len;
    u32 total_len;
    __u64 timestamp;
    __u32 result_val;

    /* log_level 过滤 */
    if (g_ctx.policy.log_level == 0) {
        return;
    }

    switch (g_ctx.policy.log_level) {
    case 1:
        /* 仅 DENY 事件 */
        if (event_type != EVENT_VFS_DENY)
            return;
        break;
    case 2:
        /* DENY + 管理事件 */
        if (event_type != EVENT_VFS_DENY &&
            event_type != EVENT_HOOK_ADDED &&
            event_type != EVENT_HOOK_REMOVED &&
            event_type != EVENT_RULE_CHANGED)
            return;
        break;
    case 3:
    case 4:
    case 5:
        /* 所有事件 */
        break;
    default:
        return;
    }

    /* 检查是否有监听者 */
    if (!g_ctx.nlsk || !netlink_has_listeners(g_ctx.nlsk, AURORA_VFS_NL_GROUP)) {
        return;
    }

    /* 计算路径长度 */
    if (path) {
        path_len = (__u32)strlen(path);
        if (path_len > VFS_NL_MAX_MSG_LEN)
            path_len = VFS_NL_MAX_MSG_LEN;
    }

    /*
     * payload 布局:
     *   struct vfs_event 固定部分 (20字节: magic + type + pid + uid + path_len)
     *   + path[] (path_len 字节)
     *   + __u64 timestamp (8字节)
     *   + __u32 result (4字节)
     */
    payload_len = sizeof(struct vfs_event) + path_len + sizeof(__u64) + sizeof(__u32);
    total_len = NLMSG_SPACE(payload_len);

    /* 分配 skb */
    skb = alloc_skb(total_len, GFP_ATOMIC);
    if (!skb) {
        vfs_trace("netlink: alloc_skb failed, len=%u", total_len);
        return;
    }

    /* 构建 netlink 消息头 */
    nlh = nlmsg_put(skb, 0, 0, NLMSG_DONE, payload_len, 0);
    if (!nlh) {
        vfs_trace("netlink: nlmsg_put failed");
        dev_kfree_skb_any(skb);
        return;
    }

    data = nlmsg_data(nlh);

    /* 填充事件结构体 */
    /* magic */
    memcpy(data, &(__u32){VFS_CMD_MAGIC}, sizeof(__u32));
    data += sizeof(__u32);

    /* event_type */
    memcpy(data, &event_type, sizeof(__u32));
    data += sizeof(__u32);

    /* pid */
    memcpy(data, &pid, sizeof(__u32));
    data += sizeof(__u32);

    /* uid */
    memcpy(data, &uid, sizeof(__u32));
    data += sizeof(__u32);

    /* path_len */
    memcpy(data, &path_len, sizeof(__u32));
    data += sizeof(__u32);

    /* path (变长) */
    if (path_len > 0 && path) {
        memcpy(data, path, path_len);
    }
    data += path_len;

    /* timestamp (纳秒) */
    timestamp = ktime_get_real_ns();
    memcpy(data, &timestamp, sizeof(__u64));
    data += sizeof(__u64);

    /* result */
    result_val = result;
    memcpy(data, &result_val, sizeof(__u32));

    /* 多播发送 */
    netlink_multicast(g_ctx.nlsk, skb, 0, AURORA_VFS_NL_GROUP, GFP_ATOMIC);

    vfs_trace("netlink: sent event type=%u pid=%u uid=%u path=%s result=%u",
              event_type, pid, uid, path ? path : "(none)", result);
}
