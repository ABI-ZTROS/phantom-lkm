# AuroraSU VFS 内核模块接口规范文档

> **版本**: v2.0  
> **目标平台**: OnePlus ACE5 (SM8650), Android 14 GKI 6.1, ARM64  
> **通讯方式**: sysfs (主) + procfs (辅助)  
> **许可证**: GPL-2.0  
> **GKI合规**: 仅使用GKI KMI导出符号

---

## 1. 概述

本文档定义了 AuroraSU VFS 子系统中，用户空间管理器与内核 .ko 模块之间的完整通讯协议。内核模块开发者应严格按照本文档实现所有接口，确保与 Android 端代码的兼容性。

### 1.1 架构关系

```
┌─────────────────────────────────────────────────────────────────┐
│                    Android 管理器 (用户空间)                       │
│                                                                 │
│  VFSDebugUtil ──→ VFSHookManager ──→ VFSRuleEngine               │
│       │                  │                  │                   │
│       │          VFSTemplateManager         │                   │
│       │                  │                  │                   │
│       ▼                  ▼                  ▼                   │
│  VFSPersistenceManager (持久化到 /data/adb/ztrosu/*.json)        │
└──────────────────────────┬──────────────────────────────────────┘
                           │
              ┌────────────┼────────────┐
              ▼            ▼            ▼
         sysfs接口     Shell命令    procfs读取
    /sys/kernel/ztrosu/vfs/*   (标准Linux)
```

### 1.2 后端检测优先级

用户空间按以下顺序检测后端，命中即停止：

| 优先级 | 后端类型 | 检测路径 | 说明 |
|--------|---------|---------|------|
| 1 | KERNEL_SYSFS | `/sys/kernel/ztrosu/vfs` 目录是否存在 | **内核模块需创建此目录** |
| 2 | KERNEL_DEBUGFS | `/sys/kernel/debug/ztrosu/vfs` 目录是否存在 | 可选实现 |
| 3 | USERSPACE | `/data/adb/ksu/vfs_monitor` 目录是否存在 | 用户空间守护进程 |
| 4 | MOCK | 以上均不存在 | 返回模拟数据，无实际功能 |

---

## 2. sysfs 接口规范

### 2.1 接口总览

所有接口位于 `/sys/kernel/ztrosu/vfs/` 目录下。

| 属性文件 | 权限 | 读写方向 | 数据格式 | 说明 |
|---------|------|---------|---------|------|
| `stats` | 0444 | 只读 | key:value 文本 | VFS操作统计 |
| `stats_reset` | 0200 | 只写 | 纯文本 | 重置统计计数器 |
| `enabled` | 0644 | 读写 | `"0"` / `"1"` | 全局开关 |
| `log_level` | 0644 | 读写 | 数字字符串 | 日志级别 0-5 |
| `default_action` | 0644 | 读写 | `"allow"` / `"deny"` | 默认动作 |
| `rules` | 0644 | 读写 | 多行文本 | 访问规则列表 |
| `rules_clear` | 0200 | 只写 | 任意内容 | 清空所有规则 |
| `hook_targets` | 0644 | 读写 | 命令协议 | Hook目标管理 |
| `hook_list` | 0444 | 只读 | 多行文本 | 当前Hook目标列表 |
| `version` | 0444 | 只读 | 纯文本 | 模块版本号 |

---

### 2.2 stats (只读)

**路径**: `/sys/kernel/ztrosu/vfs/stats`  
**权限**: 0444  
**格式**: 每行一个 `key: value` 对

**读取输出示例**:
```
open: 1234
read: 5678
write: 901
close: 1230
denied: 5
last_updated: 1717200000
```

**字段说明**:

| 字段 | 类型 | 说明 |
|------|------|------|
| `open` | u64 | 文件打开次数 |
| `read` | u64 | 文件读取次数 |
| `write` | u64 | 文件写入次数 |
| `close` | u64 | 文件关闭次数 |
| `denied` | u64 | 被拒绝的访问次数 |
| `last_updated` | u64 | 最后更新时间戳 (Unix epoch秒) |

**用户空间解析逻辑**:
```kotlin
content.lines().forEach { line ->
    val trimmed = line.trim()
    when {
        trimmed.startsWith("open:") -> openCount = trimmed.substringAfter(":").trim().toLongOrNull() ?: 0L
        trimmed.startsWith("read:") -> readCount = ...
        trimmed.startsWith("write:") -> writeCount = ...
        trimmed.startsWith("close:") -> closeCount = ...
        trimmed.startsWith("denied:") -> deniedCount = ...
    }
}
```

**内核实现要求**:
- 使用 `atomic64_t` 保证计数器线程安全
- `last_updated` 使用 `ktime_get_real_seconds()`
- show回调返回 `snprintf(buf, PAGE_SIZE, ...)` 的字节数

---

### 2.3 stats_reset (只写)

**路径**: `/sys/kernel/ztrosu/vfs/stats_reset`  
**权限**: 0200  
**格式**: 写入任意内容即触发重置

**写入示例**: `1`

**内核行为**: 接收到任意写入后，将所有计数器归零，更新 `last_updated`。

---

### 2.4 enabled (读写)

**路径**: `/sys/kernel/ztrosu/vfs/enabled`  
**权限**: 0644

**读取输出**: `0` 或 `1`  
**写入输入**: `0` 或 `1`

**内核行为**:
- 读取: 返回当前启用状态
- 写入: 使用 `kstrtobool()` 解析，设置全局启用标志
- `enabled=0` 时，所有VFS监控和拦截功能停止，统计计数也停止
- `enabled=1` 时，恢复所有功能

---

### 2.5 log_level (读写)

**路径**: `/sys/kernel/ztrosu/vfs/log_level`  
**权限**: 0644

**读取输出**: `0` ~ `5`  
**写入输入**: `0` ~ `5`

**日志级别定义**:

| 级别 | 名称 | 说明 |
|------|------|------|
| 0 | NONE | 不输出任何日志 |
| 1 | ERROR | 仅输出错误（拒绝访问等） |
| 2 | WARNING | 输出警告和错误 |
| 3 | INFO | 输出常规信息（规则匹配等） |
| 4 | DEBUG | 输出调试信息（详细匹配过程） |
| 5 | VERBOSE | 输出所有信息（包括每条VFS操作） |

**内核行为**:
- 写入超出范围的值返回 `-EINVAL`
- 仅使用 `trace_printk()` 输出，不使用 `pr_info/pr_err` 等持久化日志
- 通过 `/sys/kernel/tracing/trace` 查看输出

---

### 2.6 default_action (读写)

**路径**: `/sys/kernel/ztrosu/vfs/default_action`  
**权限**: 0644

**读取输出**: `allow` 或 `deny`  
**写入输入**: `allow` 或 `deny`

**内核行为**:
- 当没有规则匹配时，使用此默认动作
- 写入非 `allow`/`deny` 的值返回 `-EINVAL`

---

### 2.7 rules (读写)

**路径**: `/sys/kernel/ztrosu/vfs/rules`  
**权限**: 0644

**读取输出格式**: 每行一条规则
```
deny:/data/data/*/databases/:rw
allow:/sdcard/:r
deny:/system/:w
```

**写入输入格式**: 每行一条规则，支持多行批量写入
```
deny:/data/data/*/databases/:rw
allow:/sdcard/:r
deny:/system/:w
```

**规则格式**: `action:path_pattern:mode`

| 字段 | 取值 | 说明 |
|------|------|------|
| `action` | `allow` / `deny` | 允许或拒绝 |
| `path_pattern` | glob模式 | 支持 `*`, `?`, `**` |
| `mode` | `r` / `w` / `rw` | 读/写/读写 |

**⚠️ 关键实现要求**:
- 写入时必须**逐行解析**，支持一次写入多条规则
- 每行以 `\n` 分隔
- 忽略空行
- 最大规则数: 64 (`VFS_MAX_RULES`)
- 单条规则最大长度: 256 (`VFS_MAX_RULE_LEN`)
- 路径模式最大长度: 512 (`VFS_MAX_PATH_LEN`)
- 超出限制返回 `-ENOSPC` 或 `-EINVAL`

**内核写入解析伪代码**:
```c
// rules_store 回调
// buf 中可能包含多行规则，必须逐行处理
char *line, *buf_copy;
buf_copy = kstrndup(buf, count, GFP_KERNEL);
line = buf_copy;
while ((line = strsep(&buf_copy, "\n")) != NULL) {
    if (strlen(line) == 0) continue;  // 跳过空行
    vfs_debug_add_rule(line);          // 解析并添加单条规则
}
kfree(buf_copy);
return count;
```

---

### 2.8 rules_clear (只写)

**路径**: `/sys/kernel/ztrosu/vfs/rules_clear`  
**权限**: 0200

**写入**: 任意内容即触发清空所有规则

---

### 2.9 hook_targets (读写) ⭐ 新增接口

**路径**: `/sys/kernel/ztrosu/vfs/hook_targets`  
**权限**: 0644

**用途**: 管理被Hook的进程/应用目标

#### 2.9.1 写入格式 (添加Hook)

```
add:<type>:<identifier>:<uid>:<mode>
```

| 字段 | 说明 | 示例值 |
|------|------|--------|
| `type` | `PID` 或 `PACKAGE` | `PID` |
| `identifier` | PID数字或包名 | `12345` 或 `com.example.app` |
| `uid` | 目标的UID (十进制) | `10086` |
| `mode` | Hook模式 (见下表) | `INTERCEPT_ALL` |

**Hook模式定义**:

| 模式 | 值 | 说明 |
|------|---|------|
| `MONITOR_ONLY` | 0 | 仅监控，不拦截任何操作 |
| `INTERCEPT_READ` | 1 | 拦截读操作 (O_RDONLY, O_RDWR) |
| `INTERCEPT_WRITE` | 2 | 拦截写操作 (O_WRONLY, O_RDWR, O_CREAT) |
| `INTERCEPT_ALL` | 3 | 拦截所有文件操作 |

**写入示例**:
```
add:PID:12345:10086:MONITOR_ONLY
add:PACKAGE:com.example.app:10086:INTERCEPT_ALL
```

#### 2.9.2 写入格式 (移除Hook)

```
remove:<type>:<identifier>
```

**写入示例**:
```
remove:PID:12345
remove:PACKAGE:com.example.app
```

#### 2.9.3 读取格式

每行一个Hook目标:
```
PID:12345:10086:MONITOR_ONLY:1
PACKAGE:com.example.app:10086:INTERCEPT_ALL:1
```

格式: `type:identifier:uid:mode:enabled`

| 字段 | 说明 |
|------|------|
| `type` | `PID` 或 `PACKAGE` |
| `identifier` | PID数字或包名 |
| `uid` | UID |
| `mode` | Hook模式名称 |
| `enabled` | `1`=启用, `0`=禁用 |

#### 2.9.4 内核数据结构

```c
#define VFS_MAX_HOOKS 128
#define VFS_MAX_PKG_LEN 256

enum vfs_hook_mode {
    VFS_HOOK_MONITOR_ONLY = 0,
    VFS_HOOK_INTERCEPT_READ = 1,
    VFS_HOOK_INTERCEPT_WRITE = 2,
    VFS_HOOK_INTERCEPT_ALL = 3,
};

enum vfs_hook_type {
    VFS_HOOK_PID = 0,
    VFS_HOOK_PACKAGE = 1,
};

struct vfs_hook_target {
    struct list_head list;
    enum vfs_hook_type type;
    union {
        pid_t pid;
        char package_name[VFS_MAX_PKG_LEN];
    };
    uid_t uid;
    enum vfs_hook_mode mode;
    bool enabled;
};
```

#### 2.9.5 Hook匹配逻辑

当VFS操作发生时，内核应：
1. 获取当前进程的PID和UID
2. 遍历Hook目标列表
3. 对于 `PID` 类型：直接匹配PID
4. 对于 `PACKAGE` 类型：匹配UID（因为内核无法直接获取包名）
5. 如果匹配到目标，根据 `mode` 决定是否拦截
6. 如果未匹配到任何目标，按规则引擎处理

---

### 2.10 hook_list (只读) ⭐ 新增接口

**路径**: `/sys/kernel/ztrosu/vfs/hook_list`  
**权限**: 0444

**用途**: 获取当前所有Hook目标的详细信息

**读取输出格式**:
```
# ID    TYPE     IDENTIFIER          UID    MODE           ENABLED
0001   PID      12345               10086  MONITOR_ONLY   yes
0002   PACKAGE  com.example.app     10086  INTERCEPT_ALL  yes
```

---

### 2.11 version (只读)

**路径**: `/sys/kernel/ztrosu/vfs/version`  
**权限**: 0444

**读取输出**: `2` (整数，表示接口协议版本号)

---

## 3. 规则匹配引擎规范

### 3.1 匹配流程

```
VFS操作发生
    │
    ▼
enabled == true ? ──No──→ 放行 (返回0)
    │Yes
    ▼
当前进程是否在Hook目标列表中? ──No──→ 进入规则引擎
    │Yes                              │
    ▼                                 ▼
根据Hook mode决定动作          按优先级遍历规则列表
    │                                 │
    ▼                                 ▼
返回结果                     规则匹配?
                                  │Yes      │No
                                  ▼         ▼
                            返回规则动作  返回default_action
```

### 3.2 路径匹配算法

支持三种通配符:

| 通配符 | 含义 | 示例 |
|--------|------|------|
| `*` | 匹配任意非 `/` 字符序列 | `/data/*` 匹配 `/data/app` 但不匹配 `/data/app/test` |
| `?` | 匹配单个非 `/` 字符 | `/data/a?` 匹配 `/data/app` |
| `**` | 匹配任意字符序列(含 `/`) | `/system/**` 匹配 `/system/bin/su` |

**实现建议**: 使用动态规划 (DP) 算法，`dp[i][j]` 表示 `pattern[0..i-1]` 是否匹配 `path[0..j-1]`。

### 3.3 规则优先级

- 数字越大优先级越高
- 优先级高的规则先匹配
- 第一个匹配的规则决定结果
- 无匹配时使用 `default_action`

### 3.4 预设规则 (内核模块可内置)

| 优先级 | 动作 | 路径模式 | 操作 | 说明 |
|--------|------|---------|------|------|
| 100 | deny | `/system/**` | write | 保护系统分区 |
| 100 | deny | `/vendor/**` | write | 保护vendor分区 |
| 100 | deny | `/product/**` | write | 保护product分区 |
| 95 | deny | `/sys/fs/selinux/**` | read,write | 保护SELinux |
| 90 | deny | `/data/data/*` | read,write | 保护应用私有数据 |
| 80 | log | `/data/adb/ksu/**` | read,write | 记录KSU访问 |
| 50 | allow | `/sdcard/**` | read,open | 允许公共存储读取 |
| 30 | log | `/proc/**` | read | 记录/proc读取 |

---

## 4. 日志输出规范

### 4.1 仅使用 trace_printk

所有日志输出**必须**使用 `trace_printk()`，不使用 `pr_info/pr_err/pr_debug` 等持久化日志。

**原因**: 
- `trace_printk` 输出到 ftrace 缓冲区，非持久化
- 可通过 `/sys/kernel/tracing/trace` 查看
- 不会污染 dmesg/kernel log
- 性能开销低

### 4.2 日志格式

```
trace_printk("vfs: <event> <details>\n");
```

**示例**:
```c
trace_printk("vfs: deny path=%s pid=%d uid=%d rule=%s\n", 
             path, current->pid, current_uid, rule_name);
trace_printk("vfs: hook_add type=PID pid=%d mode=%d\n", pid, mode);
trace_printk("vfs: rule_add action=%s path=%s mode=%d\n", 
             action_str, path_pattern, mode_mask);
```

### 4.3 日志级别控制

| log_level | 输出内容 |
|-----------|---------|
| 0 | 无输出 |
| 1 | 仅 deny 事件 |
| 2 | deny + 规则匹配失败 |
| 3 | deny + 匹配详情 + Hook事件 |
| 4 | 所有匹配过程 + 统计更新 |
| 5 | 每条VFS操作 (open/read/write/close) |

---

## 5. 内核数据结构定义

### 5.1 头文件常量

```c
#define VFS_DEBUG_VERSION     2       // 接口协议版本
#define VFS_MAX_RULES         64      // 最大规则数
#define VFS_MAX_HOOKS         128     // 最大Hook目标数
#define VFS_MAX_RULE_LEN      256     // 单条规则最大长度
#define VFS_MAX_PATH_LEN      512     // 路径模式最大长度
#define VFS_MAX_PKG_LEN       256     // 包名最大长度
```

### 5.2 核心结构体

```c
/* 规则结构 */
struct vfs_rule {
    struct list_head list;
    int priority;                    // 优先级 (数字越大越先匹配)
    enum vfs_action action;           // ALLOW / DENY
    char path_pattern[VFS_MAX_PATH_LEN];
    unsigned int mode_mask;           // bit0=read, bit1=write
    uid_t uid_filter;                 // UID过滤 (0表示不限制)
    bool enabled;
};

/* Hook目标结构 */
struct vfs_hook_target {
    struct list_head list;
    enum vfs_hook_type type;          // PID / PACKAGE
    union {
        pid_t pid;
        char package_name[VFS_MAX_PKG_LEN];
    };
    uid_t uid;
    enum vfs_hook_mode mode;
    bool enabled;
};

/* 统计结构 */
struct vfs_stats {
    atomic64_t open_count;
    atomic64_t read_count;
    atomic64_t write_count;
    atomic64_t close_count;
    atomic64_t denied_count;
    u64 last_updated;
};

/* 全局策略 */
struct vfs_policy {
    bool enabled;
    unsigned int log_level;
    enum vfs_action default_action;
};

/* 全局上下文 */
struct vfs_debug_ctx {
    struct vfs_stats stats;
    struct vfs_policy policy;
    struct list_head rules;           // 规则链表
    struct list_head hooks;           // Hook目标链表
    unsigned int rules_count;
    unsigned int hooks_count;
    spinlock_t lock;
    bool initialized;
};
```

---

## 6. sysfs 属性表参考

```c
static struct attribute *vfs_attrs[] = {
    &stats_attr.attr,              // 0444 RO
    &stats_reset_attr.attr,       // 0200 WO
    &enabled_attr.attr,            // 0644 RW
    &log_level_attr.attr,         // 0644 RW
    &default_action_attr.attr,    // 0644 RW
    &rules_attr.attr,             // 0644 RW
    &rules_clear_attr.attr,       // 0200 WO
    &hook_targets_attr.attr,      // 0644 RW  ⭐ 新增
    &hook_list_attr.attr,         // 0444 RO  ⭐ 新增
    &version_attr.attr,           // 0444 RO  ⭐ 新增
    NULL,
};

static struct attribute_group vfs_attr_group = {
    .attrs = vfs_attrs,
};
```

---

## 7. GKI KMI 符号依赖

内核模块仅允许使用以下GKI导出的符号：

### 7.1 必需符号

| 类别 | 符号 | 用途 |
|------|------|------|
| 内存 | `kzalloc`, `kfree`, `kstrdup`, `kstrndup` | 内存分配 |
| 链表 | `list_add_tail`, `list_del_init`, `list_empty`, `list_for_each_entry_safe`, `INIT_LIST_HEAD` | 链表操作 |
| 同步 | `mutex_init`, `mutex_lock`, `mutex_unlock`, `mutex_destroy`, `spin_lock_init`, `spin_lock`, `spin_unlock` | 同步原语 |
| 原子 | `atomic64_inc`, `atomic64_set`, `atomic64_read` | 原子计数器 |
| sysfs | `kobject_create_and_add`, `kobject_put`, `sysfs_create_group`, `sysfs_remove_group`, `kernel_kobj` | sysfs接口 |
| 字符串 | `strncmp`, `strcmp`, `strlen`, `strsep`, `strchr`, `snprintf`, `sscanf` | 字符串处理 |
| 转换 | `kstrtobool`, `kstrtouint`, `kstrtoint` | 字符串转数值 |
| 时间 | `ktime_get_real_seconds`, `ktime_get_ns` | 时间获取 |
| 进程 | `current` (宏) | 当前进程信息 |
| VFS | `filp_open`, `filp_close`, `fget`, `fput`, `vfs_read`, `vfs_write` | 文件操作 |
| trace | `trace_printk` | 非持久化日志 |

### 7.2 禁止使用

- 任何非GKI导出的OEM私有符号
- `printk`, `pr_info`, `pr_err`, `pr_debug` (用 `trace_printk` 替代)
- 任何需要特定内核配置的符号

---

## 8. Makefile 规范

```makefile
MODULE_NAME := aurora_vfs

obj-m += $(MODULE_NAME).o

KDIR ?= /lib/modules/$(shell uname -r)/build
ARCH ?= arm64
CROSS_COMPILE ?=

ccflags-y += -Wall -Wextra -Wno-unused-parameter
ccflags-y += -O2 -g

all:
	$(MAKE) -C $(KDIR) M=$(PWD) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	$(RM) -f *.o *.ko *.mod.c *.mod *.order *.symvers .*.cmd

.PHONY: all clean
```

**编译命令**:
```bash
make KDIR=/path/to/gki-headers ARCH=arm64 \
     CROSS_COMPILE=aarch64-linux-gnu- \
     CC=clang
```

---

## 9. 模块生命周期

### 9.1 加载 (insmod)

1. 创建 `/sys/kernel/ztrosu/vfs/` 目录
2. 创建所有sysfs属性文件
3. 初始化统计计数器为0
4. 初始化策略: enabled=false, log_level=0, default_action=allow
5. 初始化空规则链表和空Hook链表
6. 输出: `trace_printk("aurora_vfs: module loaded\n");`

### 9.2 运行中

- enabled=false 时: 不进行任何VFS操作监控
- enabled=true 时: 开始监控和拦截

### 9.3 卸载 (rmmod)

1. 设置 enabled=false，停止所有监控
2. 移除所有sysfs属性
3. 释放kobject
4. 释放所有规则内存
5. 释放所有Hook目标内存
6. 销毁锁
7. 输出: `trace_printk("aurora_vfs: module unloaded\n");`

---

## 10. 与用户空间的完整交互流程

### 10.1 初始化流程

```
用户空间                              内核模块
   │                                     │
   │── 检测 /sys/kernel/ztrosu/vfs ───→ │  (目录存在 = 模块已加载)
   │←── 目录存在 ──────────────────────│
   │                                     │
   │── 读取 version ──────────────────→ │
   │←── "2" ──────────────────────────│
   │                                     │
   │── 读取 enabled ──────────────────→ │
   │←── "0" ──────────────────────────│
   │                                     │
   │── 写入 enabled "1" ──────────────→ │  (启用VFS监控)
   │←── 成功 ──────────────────────────│
   │                                     │
   │── 加载持久化配置 ────────────────→ │
   │   (从 /data/adb/ztrosu/*.json)     │
   │                                     │
   │── 写入 rules (多行) ────────────→ │  (批量写入规则)
   │←── 成功 ──────────────────────────│
   │                                     │
   │── 写入 hook_targets ─────────────→ │  (添加Hook目标)
   │←── 成功 ──────────────────────────│
```

### 10.2 运行时监控流程

```
进程执行文件操作
       │
       ▼
内核模块拦截
       │
       ├── enabled? ──No──→ 放行
       │      │Yes
       │      ▼
       ├── 进程在Hook列表中? ──No──→ 规则引擎匹配
       │      │Yes                    │
       │      ▼                       ▼
       │  根据Hook mode处理      规则匹配?
       │      │                  │Yes    │No
       │      ▼                  ▼       ▼
       │  拦截/放行/记录    返回规则动作  default_action
       │      │                  │       │
       │      ▼                  ▼       ▼
       │  更新统计计数器      更新统计   更新统计
       │      │
       │      ▼
       │  trace_printk (如果log_level允许)
```

---

## 11. 测试验证清单

内核模块开发者完成实现后，应验证以下项目：

### 11.1 接口测试

- [ ] `/sys/kernel/ztrosu/vfs/` 目录存在
- [ ] `cat stats` 返回正确格式
- [ ] `echo 1 > enabled` 成功
- [ ] `cat enabled` 返回 `1`
- [ ] `echo 3 > log_level` 成功
- [ ] `echo deny > default_action` 成功
- [ ] `echo -e "deny:/system/**:w\nallow:/sdcard/**:r" > rules` 成功
- [ ] `cat rules` 返回写入的规则
- [ ] `echo 1 > rules_clear` 成功
- [ ] `cat rules` 返回空
- [ ] `echo "add:PID:1234:10086:MONITOR_ONLY" > hook_targets` 成功
- [ ] `cat hook_list` 包含添加的目标
- [ ] `echo "remove:PID:1234" > hook_targets` 成功
- [ ] `cat hook_list` 不包含已移除的目标
- [ ] `cat version` 返回 `2`

### 11.2 功能测试

- [ ] enabled=0 时，不拦截任何操作
- [ ] enabled=1 时，按规则拦截
- [ ] 规则优先级正确
- [ ] glob通配符匹配正确
- [ ] Hook目标PID匹配正确
- [ ] Hook目标UID匹配正确
- [ ] 统计计数器正确更新
- [ ] trace_printk 输出可通过 `/sys/kernel/tracing/trace` 查看
- [ ] rmmod 后无内存泄漏

---

## 附录A: 用户空间配置文件格式 (参考)

以下文件由用户空间管理，内核模块不需要处理，但了解格式有助于理解整体设计。

### /data/adb/ztrosu/vfs_hooks.json
```json
{
  "version": 1,
  "lastModified": 1717200000,
  "targets": [
    {
      "id": "uuid-string",
      "type": "PID",
      "identifier": "12345",
      "uid": 10086,
      "mode": "MONITOR_ONLY",
      "enabled": true,
      "createdAt": 1717200000
    }
  ]
}
```

### /data/adb/ztrosu/vfs_rules.json
```json
[
  {
    "id": "rule-001",
    "action": "DENY",
    "pathPattern": "/system/**",
    "opTypes": ["WRITE"],
    "priority": 100,
    "enabled": true
  }
]
```

---

## 附录B: 与旧版接口的兼容性

| 旧版接口 (v1) | 新版接口 (v2) | 变更说明 |
|---------------|---------------|---------|
| `rules` 只支持单行写入 | `rules` 支持多行批量写入 | **必须修复** |
| 无 `hook_targets` | 新增 `hook_targets` | **新增** |
| 无 `hook_list` | 新增 `hook_list` | **新增** |
| 无 `version` | 新增 `version` | **新增** |
| `rules` 格式 `action:path:mode` | 保持不变 | 兼容 |
| `stats` 格式 `key:value` | 保持不变 | 兼容 |
