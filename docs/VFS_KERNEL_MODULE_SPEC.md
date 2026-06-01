# AuroraSU VFS 内核模块接口规范文档

> **版本**: v3.0  
> **目标平台**: OnePlus ACE5 (SM8650), Android 14 GKI 6.1, ARM64  
> **通讯方式**: sysfs (状态读取) + pipe (命令下发) + netlink (实时事件流)  
> **许可证**: GPL-2.0  
> **GKI合规**: 仅使用GKI KMI导出符号

---

## 1. 概述

本文档定义了 AuroraSU VFS 子系统中，用户空间管理器与内核 .ko 模块之间的完整通讯协议。内核模块开发者应严格按照本文档实现所有接口，确保与 Android 端代码的兼容性。

### 1.1 架构关系

#### 三通道通讯架构

```
┌──────────────────────────────────────────────────────────────────────────┐
│                     Android 管理器 (用户空间)                              │
│                                                                          │
│  VFSDebugUtil ──→ VFSHookManager ──→ VFSRuleEngine                        │
│       │                  │                  │                           │
│       │          VFSTemplateManager         │                           │
│       │                  │                  │                           │
│       ▼                  ▼                  ▼                           │
│  VFSPersistenceManager (持久化到 /data/adb/ztrosu/*.json)                 │
└──────────┬──────────────────┬──────────────────┬────────────────────────┘
           │                  │                  │
     ┌─────┴─────┐    ┌──────┴──────┐    ┌──────┴──────┐
     │  sysfs通道  │    │  pipe通道   │    │ netlink通道  │
     │ (状态读取)  │    │ (命令下发)  │    │ (事件推送)  │
     └─────┬─────┘    └──────┬──────┘    └──────┬──────┘
           │                  │                  │
           │  /sys/kernel/    │  /dev/aurora_    │  NETLINK_
           │  ztrosu/vfs/*    │  vfs_<8hex>      │  USERSOCK
           │                  │  (mkfifo)       │  (组31)
           │                  │                  │
           ▼                  ▼                  ▼
     ┌──────────────────────────────────────────────────┐
     │                内核 .ko 模块                       │
     │                                                   │
     │  sysfs接口层    pipe命令处理层    netlink事件推送层  │
     │       │              │                  │         │
     │       └──────────────┼──────────────────┘         │
     │                      ▼                            │
     │              VFS Hook & 规则引擎                    │
     │              (共享全局上下文)                        │
     └──────────────────────────────────────────────────┘
```

#### 通道职责划分

| 通道 | 方向 | 用途 | 特点 |
|------|------|------|------|
| **sysfs** | 用户空间 → 内核 (读) / 用户空间 → 内核 (写) | 状态读取、策略配置 | 持久化接口，兼容v2 |
| **pipe** | 用户空间 → 内核 (写) | 命令下发 (add_hook, remove_hook, set_rules等) | 一次性使用，随机命名 |
| **netlink** | 内核 → 用户空间 (推送) | 实时事件流 (VFS操作事件) | 异步推送，多播组 |

#### 通讯方式选择指南

```
需要读取状态/统计/规则?  ──→ sysfs (读取)
需要修改全局策略?        ──→ sysfs (写入) 或 pipe (CMD_SET_POLICY)
需要添加/移除Hook目标?   ──→ pipe (CMD_ADD_HOOK / CMD_REMOVE_HOOK)
需要批量设置规则?        ──→ pipe (CMD_SET_RULES)
需要接收实时VFS事件?     ──→ netlink (订阅事件组)
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

## 3. Pipe 通讯协议 (命令下发)

### 3.1 概述

Pipe通道用于用户空间向内核模块下发一次性命令，替代sysfs中部分写入操作。Pipe采用"创建-使用-销毁"模型，每次通讯创建新的命名管道，通讯完毕立即销毁。

**设计动机**:
- 避免sysfs写入的竞态条件（sysfs写入是全局共享的）
- 支持二进制协议，传输效率更高
- 一次性管道天然防止重放攻击
- 支持更复杂的命令数据结构

### 3.2 管道命名规则

| 属性 | 值 |
|------|---|
| 路径格式 | `/dev/aurora_vfs_<8位随机hex>` |
| 示例 | `/dev/aurora_vfs_a3f7b2c1` |
| 创建方式 | 用户空间调用 `mkfifo()` 创建 |
| 读取方式 | 内核通过 misc设备 或 procfs 接口读取 |
| 生命周期 | 创建 → 写入命令 → 内核读取处理 → 立即 `unlink` 销毁 |
| 权限 | 0600 (仅root可访问) |
| 超时 | 5秒，超时自动销毁 |

**随机命名生成**:
```kotlin
val random = SecureRandom()
val hex = ByteArray(4).also { random.nextBytes(it) }.joinToString("") { "%02x".format(it) }
val pipePath = "/dev/aurora_vfs_$hex"
```

### 3.3 命令协议格式

写入pipe的二进制数据格式 (所有字段小端序):

```c
struct vfs_command {
    __u32 magic;       // 0xAF5F (Aurora VFS magic number)
    __u32 version;     // 2 (协议版本)
    __u32 cmd_type;    // 命令类型 (见下表)
    __u32 cmd_len;     // 命令数据长度 (data[]的字节数)
    __u8  data[];      // 命令数据 (变长，长度由cmd_len指定)
};
```

**协议头大小**: 16字节 (固定)

**内核校验流程**:
1. 读取前16字节作为协议头
2. 校验 `magic == 0xAF5F`，不匹配则丢弃并销毁管道
3. 校验 `version == 2`，不匹配则丢弃并销毁管道
4. 根据 `cmd_len` 读取剩余的 `data[]`
5. 根据 `cmd_type` 分发处理

### 3.4 命令类型定义

```c
#define VFS_CMD_MAGIC          0xAF5F
#define VFS_CMD_VERSION       2

/* 命令类型 */
#define CMD_ADD_HOOK          1   // 添加Hook目标
#define CMD_REMOVE_HOOK       2   // 移除Hook目标
#define CMD_SET_RULES         3   // 批量设置规则
#define CMD_CLEAR_RULES       4   // 清空规则
#define CMD_SET_POLICY        5   // 设置策略
#define CMD_RESET_STATS       6   // 重置统计
#define CMD_QUERY_STATUS      7   // 查询状态 (通过pipe返回)
```

| 命令类型 | 值 | 方向 | data内容 | 说明 |
|---------|---|------|---------|------|
| `CMD_ADD_HOOK` | 1 | 用户→内核 | `cmd_add_hook` | 添加Hook目标 |
| `CMD_REMOVE_HOOK` | 2 | 用户→内核 | `cmd_remove_hook` | 移除Hook目标 |
| `CMD_SET_RULES` | 3 | 用户→内核 | `cmd_set_rules` | 批量设置规则 |
| `CMD_CLEAR_RULES` | 4 | 用户→内核 | 无 (cmd_len=0) | 清空所有规则 |
| `CMD_SET_POLICY` | 5 | 用户→内核 | `cmd_set_policy` | 设置策略参数 |
| `CMD_RESET_STATS` | 6 | 用户→内核 | 无 (cmd_len=0) | 重置统计计数器 |
| `CMD_QUERY_STATUS` | 7 | 内核→用户 | 状态二进制数据 | 查询当前状态 |

### 3.5 CMD_ADD_HOOK 数据格式

```c
struct cmd_add_hook {
    __u8  hook_type;        // 0=PID, 1=PACKAGE
    __u32 identifier_len;   // identifier字符串长度 (不含\0)
    char  identifier[];     // PID字符串或包名 (变长)
    __u32 uid;              // 目标UID
    __u8  hook_mode;        // 0=MONITOR_ONLY, 1=INTERCEPT_READ,
                           // 2=INTERCEPT_WRITE, 3=INTERCEPT_ALL
};
```

**写入示例 (添加PID Hook)**:
```
协议头: magic=0xAF5F, version=2, cmd_type=1, cmd_len=19
数据:   hook_type=0, identifier_len=5, identifier="12345", uid=10086, hook_mode=3
```

**写入示例 (添加PACKAGE Hook)**:
```
协议头: magic=0xAF5F, version=2, cmd_type=1, cmd_len=31
数据:   hook_type=1, identifier_len=16, identifier="com.example.app", uid=10086, hook_mode=1
```

### 3.6 CMD_REMOVE_HOOK 数据格式

```c
struct cmd_remove_hook {
    __u8  hook_type;        // 0=PID, 1=PACKAGE
    __u32 identifier_len;   // identifier字符串长度
    char  identifier[];     // PID字符串或包名
};
```

### 3.7 CMD_SET_RULES 数据格式

```c
struct cmd_set_rules {
    __u32 rule_count;      // 规则数量
    // 紧跟rule_count条规则，每条格式：
    //   __u8  action;       // 0=allow, 1=deny
    //   __u32 path_len;     // 路径长度
    //   char  path[];       // 路径模式 (变长)
    //   __u8  mode_mask;    // bit0=read, bit1=write
};
```

**写入示例 (设置2条规则)**:
```
协议头: magic=0xAF5F, version=2, cmd_type=3, cmd_len=...
数据:   rule_count=2
        规则1: action=1(deny), path_len=14, path="/system/**", mode_mask=0x02(write)
        规则2: action=0(allow), path_len=10, path="/sdcard/**", mode_mask=0x01(read)
```

### 3.8 CMD_SET_POLICY 数据格式

```c
struct cmd_set_policy {
    __u8  enabled;          // 0或1
    __u8  log_level;        // 0-5
    __u8  default_action;   // 0=allow, 1=deny
    __u8  reserved;         // 对齐填充
};
```

### 3.9 Pipe通讯完整流程

```
用户空间                                    内核模块
   │                                          │
   │── 生成8位随机hex ──────────────────────→ │
   │── mkfifo("/dev/aurora_vfs_a3f7b2c1") ──→ │  (创建命名管道)
   │                                          │
   │── 打开pipe写入端 (O_WRONLY) ───────────→ │
   │── 写入 vfs_command 二进制数据 ──────────→ │
   │── 关闭写入端 ──────────────────────────→ │
   │                                          │── 内核检测到pipe有数据
   │                                          │── 读取并解析vfs_command
   │                                          │── 校验magic和version
   │                                          │── 执行对应命令
   │                                          │── 更新全局上下文
   │                                          │
   │── unlink("/dev/aurora_vfs_a3f7b2c1") ──→ │  (销毁管道)
   │                                          │
```

### 3.10 内核Pipe读取实现参考

内核通过misc设备或procfs接口监听pipe:

```c
/* 内核端pipe监听线程伪代码 */
static int pipe_listener_thread(void *data)
{
    while (!kthread_should_stop()) {
        // 扫描 /dev/aurora_vfs_* 命名管道
        // 检测到新管道时打开读取端
        // 读取 vfs_command 协议头
        // 校验 magic == 0xAF5F && version == 2
        // 读取 cmd_len 字节的数据
        // 分发到对应命令处理函数
        // 关闭pipe
    }
    return 0;
}
```

**替代方案**: 通过procfs接口注册pipe路径，内核轮询procfs条目发现新pipe。

---

## 4. Netlink 事件协议 (实时事件流)

### 4.1 概述

Netlink通道用于内核模块向用户空间异步推送VFS操作事件。用户空间通过订阅netlink多播组接收实时事件流。

**设计动机**:
- sysfs的stats接口需要主动轮询，无法实时感知事件
- netlink提供内核到用户空间的高效异步推送机制
- 支持多播，多个用户空间进程可同时订阅
- 标准Linux内核通讯机制，稳定可靠

### 4.2 Netlink 配置

| 属性 | 值 |
|------|---|
| Netlink协议 | `NETLINK_USERSOCK` (自定义协议) |
| 多播组 | 组31 (Aurora VFS事件组) |
| 绑定 | 绑定到特定PID |
| 最大消息大小 | 4096字节 (单条事件) |
| 发送缓冲区 | 16384字节 (内核端) |
| 接收缓冲区 | 65536字节 (用户空间端) |

### 4.3 事件格式

```c
struct vfs_event {
    __u32 magic;       // 0xAF5F (Aurora VFS magic number)
    __u32 event_type;  // 事件类型 (见下表)
    __u32 pid;         // 触发进程PID
    __u32 uid;         // 触发进程UID
    __u32 path_len;    // 文件路径长度 (不含\0)
    __u8  path[];      // 文件路径 (变长)
    __u64 timestamp;   // 纳秒时间戳 (ktime_get_real_ns())
    __u32 result;      // 0=allow, 1=deny
};
```

**事件头大小**: 28字节 (固定) + path_len (变长)

### 4.4 事件类型定义

```c
/* VFS操作事件 (1-9) */
#define EVENT_VFS_OPEN       1    // 文件打开
#define EVENT_VFS_READ       2    // 文件读取
#define EVENT_VFS_WRITE      3    // 文件写入
#define EVENT_VFS_CLOSE      4    // 文件关闭
#define EVENT_VFS_DENY       5    // 访问被拒绝

/* 管理事件 (10-19) */
#define EVENT_HOOK_ADDED     10   // Hook目标已添加
#define EVENT_HOOK_REMOVED   11   // Hook目标已移除
#define EVENT_RULE_CHANGED    12   // 规则已变更
```

| 事件类型 | 值 | 触发时机 | path字段内容 |
|---------|---|---------|-------------|
| `EVENT_VFS_OPEN` | 1 | 被监控进程打开文件 | 文件路径 |
| `EVENT_VFS_READ` | 2 | 被监控进程读取文件 | 文件路径 |
| `EVENT_VFS_WRITE` | 3 | 被监控进程写入文件 | 文件路径 |
| `EVENT_VFS_CLOSE` | 4 | 被监控进程关闭文件 | 文件路径 |
| `EVENT_VFS_DENY` | 5 | 访问被规则/Hook拒绝 | 文件路径 |
| `EVENT_HOOK_ADDED` | 10 | Hook目标添加成功 | 目标标识符 |
| `EVENT_HOOK_REMOVED` | 11 | Hook目标移除成功 | 目标标识符 |
| `EVENT_RULE_CHANGED` | 12 | 规则列表变更 | 空字符串 |

### 4.5 用户空间订阅流程

```kotlin
// Kotlin伪代码 - 订阅netlink事件
val sock = NetlinkSocket()
sock.bind(NETLINK_USERSOCK, pid)
sock.joinGroup(31)  // 加入Aurora VFS事件组

// 接收线程
thread {
    while (running) {
        val event = sock.receive()
        if (event.magic == 0xAF5F) {
            val eventType = event.eventType
            val pid = event.pid
            val uid = event.uid
            val path = event.getPath()
            val timestamp = event.timestamp
            val result = event.result
            // 分发到UI或日志系统
        }
    }
}
```

### 4.6 内核发送实现参考

```c
/* 内核端发送事件伪代码 */
static void send_vfs_event(struct vfs_debug_ctx *ctx,
                           u32 event_type, u32 pid, u32 uid,
                           const char *path, u32 result)
{
    struct sk_buff *skb;
    struct nlmsghdr *nlh;
    struct vfs_event *evt;
    u32 path_len = strlen(path);
    u32 total_len = NLMSG_SPACE(sizeof(struct vfs_event) + path_len);

    skb = alloc_skb(total_len, GFP_ATOMIC);
    if (!skb) return;

    nlh = nlmsg_put(skb, 0, 0, NLMSG_DONE, total_len - NLMSG_HDRLEN, 0);
    evt = (struct vfs_event *)nlmsg_data(nlh);

    evt->magic = VFS_CMD_MAGIC;
    evt->event_type = event_type;
    evt->pid = pid;
    evt->uid = uid;
    evt->path_len = path_len;
    memcpy(evt->path, path, path_len);
    evt->timestamp = ktime_get_real_ns();
    evt->result = result;

    nlmsg_multicast(ctx->nlsk, skb, 0, AURORA_VFS_NL_GROUP, GFP_ATOMIC);
}
```

### 4.7 事件过滤

用户空间可通过以下方式过滤事件:
- 按 `event_type` 过滤: 仅关注特定类型事件
- 按 `pid` 过滤: 仅关注特定进程
- 按 `uid` 过滤: 仅关注特定用户
- 按 `result` 过滤: 仅关注被拒绝的事件

内核端在 `log_level` 控制下决定是否发送事件:

| log_level | 发送的事件类型 |
|-----------|-------------|
| 0 | 无 |
| 1 | 仅 EVENT_VFS_DENY |
| 2 | EVENT_VFS_DENY + EVENT_RULE_CHANGED |
| 3 | 所有VFS操作事件 + 管理事件 |
| 4 | 所有事件 (含详细匹配过程) |
| 5 | 所有事件 (含每条VFS操作) |

---

## 5. 双模式目标选择

### 5.1 概述

用户空间提供两种模式选择被Hook的目标进程/应用，选择后通过pipe通道发送 `CMD_ADD_HOOK` 命令。

### 5.2 模式1 - 运行进程选择

**适用场景**: Hook当前正在运行的进程

**流程**:
1. 用户空间扫描 `/proc/` 目录获取实时PID列表
2. 解析每个PID的 `/proc/<pid>/cmdline` 获取进程名
3. 从 `/proc/<pid>/status` 读取UID
4. 展示列表供用户选择:

```
┌──────────────────────────────────────────┐
│  PID    进程名              UID          │
│  1234   com.example.app    10086         │
│  5678   system_server      1000          │
│  9012   surfaceflinger     1000          │
└──────────────────────────────────────────┘
```

5. 用户选择后，通过pipe发送:
```
CMD_ADD_HOOK(hook_type=PID, identifier="1234", uid=10086, hook_mode=3)
```

**用户空间实现参考**:
```kotlin
fun getRunningProcesses(): List<ProcessInfo> {
    val procs = mutableListOf<ProcessInfo>()
    File("/proc").listFiles()?.filter { it.isDirectory && it.name.all { it.isDigit() } }
        ?.forEach { procDir ->
            val pid = procDir.name.toInt()
            val cmdline = File("${procDir.absolutePath}/cmdline").readText()
            val status = File("${procDir.absolutePath}/status").readLines()
            val uid = status.find { it.startsWith("Uid:") }
                ?.split("\\s+".toRegex())?.get(1)?.toInt() ?: 0
            procs.add(ProcessInfo(pid, cmdline, uid))
        }
    return procs
}
```

### 5.3 模式2 - 已安装应用选择

**适用场景**: Hook已安装的Android应用

**流程**:
1. 通过 `pm list packages -U` 获取应用列表
2. 解析输出获取包名和UID
3. 展示列表供用户选择:

```
┌──────────────────────────────────────────┐
│  包名                      UID           │
│  com.example.app           10086         │
│  com.android.settings      1000          │
│  com.android.chrome        10123         │
└──────────────────────────────────────────┘
```

4. 用户选择后，通过pipe发送:
```
CMD_ADD_HOOK(hook_type=PACKAGE, identifier="com.example.app", uid=10086, hook_mode=3)
```

**内核匹配机制**: 内核无法直接获取包名，因此对于 `PACKAGE` 类型的Hook，内核通过 **UID匹配** 来识别目标进程。当进程的UID与Hook目标UID一致时，视为匹配。

**用户空间实现参考**:
```kotlin
fun getInstalledPackages(): List<PackageInfo> {
    val process = Runtime.getRuntime().exec(arrayOf("pm", "list", "packages", "-U"))
    val output = process.inputStream.bufferedReader().readText()
    return output.lines()
        .filter { it.startsWith("package:") }
        .map { line ->
            val parts = line.substringAfter("package:").split(" ")
            PackageInfo(
                packageName = parts[0],
                uid = parts.getOrNull(1)?.removePrefix("uid:")?.toIntOrNull() ?: 0
            )
        }
}
```

### 5.4 模式对比

| 维度 | 模式1 (运行进程) | 模式2 (已安装应用) |
|------|----------------|------------------|
| 数据来源 | `/proc/` 扫描 | `pm list packages -U` |
| 标识方式 | PID | 包名 |
| 内核匹配 | 直接匹配PID | 通过UID匹配 |
| 持久性 | 进程退出后失效 | 持续有效 (UID不变) |
| 适用场景 | 临时调试 | 长期监控 |

---

## 6. 安全设计

### 6.1 Pipe安全

| 安全措施 | 实现方式 |
|---------|---------|
| 随机命名 | 每次通讯使用不同的8位随机hex命名 |
| 即时销毁 | 通讯完毕立即 `unlink` 销毁管道文件 |
| 权限控制 | pipe权限设为 `0600` (仅root可读写) |
| 超时保护 | 5秒超时，超时自动销毁未使用的管道 |
| Magic校验 | 内核校验 `0xAF5F` magic number |
| 版本校验 | 内核校验协议版本号，防止版本不匹配 |

**超时处理流程**:
```
创建pipe → 启动5秒计时器
    │
    ├── 5秒内收到数据 → 取消计时器 → 正常处理 → 销毁pipe
    │
    └── 5秒超时 → 自动unlink销毁pipe → 记录超时日志
```

### 6.2 Netlink安全

| 安全措施 | 实现方式 |
|---------|---------|
| UID过滤 | 仅允许 `UID=0` (root) 的进程连接 |
| CAP_ACK确认 | 使用 `NETLINK_CAP_ACK` 确认机制 |
| PID绑定 | 绑定到特定PID，防止跨进程窃听 |
| 组隔离 | 使用独立的多播组31，与其他netlink用户隔离 |

**内核端安全检查**:
```c
/* netlink消息接收回调中的安全检查 */
static int aurora_vfs_nl_rcv_msg(struct sk_buff *skb, struct nlmsghdr *nlh)
{
    // 检查发送者UID
    const struct cred *cred = current_cred();
    if (uid_eq(cred->uid, GLOBAL_ROOT_UID)) {
        // 允许root连接
    } else {
        return -EPERM;
    }
    // ... 处理消息
}
```

### 6.3 整体安全模型

```
┌─────────────────────────────────────────────────┐
│                  安全边界                         │
│                                                   │
│  sysfs:  /sys/kernel/ztrosu/vfs/                 │
│    └── 文件系统权限控制 (0644/0444/0200)          │
│                                                   │
│  pipe:   /dev/aurora_vfs_<random>                 │
│    └── 0600权限 + 随机命名 + 即时销毁 + 超时      │
│                                                   │
│  netlink: NETLINK_USERSOCK 组31                   │
│    └── UID=0限制 + PID绑定 + CAP_ACK              │
│                                                   │
│  所有通道:                                         │
│    └── Magic Number (0xAF5F) 校验                  │
│    └── 协议版本校验                                │
└─────────────────────────────────────────────────┘
```

---

## 7. 规则匹配引擎规范

### 7.1 匹配流程

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

### 7.2 路径匹配算法

支持三种通配符:

| 通配符 | 含义 | 示例 |
|--------|------|------|
| `*` | 匹配任意非 `/` 字符序列 | `/data/*` 匹配 `/data/app` 但不匹配 `/data/app/test` |
| `?` | 匹配单个非 `/` 字符 | `/data/a?` 匹配 `/data/app` |
| `**` | 匹配任意字符序列(含 `/`) | `/system/**` 匹配 `/system/bin/su` |

**实现建议**: 使用动态规划 (DP) 算法，`dp[i][j]` 表示 `pattern[0..i-1]` 是否匹配 `path[0..j-1]`。

### 7.3 规则优先级

- 数字越大优先级越高
- 优先级高的规则先匹配
- 第一个匹配的规则决定结果
- 无匹配时使用 `default_action`

### 7.4 预设规则 (内核模块可内置)

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

## 8. 日志输出规范

### 8.1 仅使用 trace_printk

所有日志输出**必须**使用 `trace_printk()`，不使用 `pr_info/pr_err/pr_debug` 等持久化日志。

**原因**: 
- `trace_printk` 输出到 ftrace 缓冲区，非持久化
- 可通过 `/sys/kernel/tracing/trace` 查看
- 不会污染 dmesg/kernel log
- 性能开销低

### 8.2 日志格式

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

### 8.3 日志级别控制

| log_level | 输出内容 |
|-----------|---------|
| 0 | 无输出 |
| 1 | 仅 deny 事件 |
| 2 | deny + 规则匹配失败 |
| 3 | deny + 匹配详情 + Hook事件 |
| 4 | 所有匹配过程 + 统计更新 |
| 5 | 每条VFS操作 (open/read/write/close) |

---

## 9. 内核数据结构定义

### 9.1 头文件常量

```c
#define VFS_DEBUG_VERSION     2       // 接口协议版本
#define VFS_MAX_RULES         64      // 最大规则数
#define VFS_MAX_HOOKS         128     // 最大Hook目标数
#define VFS_MAX_RULE_LEN      256     // 单条规则最大长度
#define VFS_MAX_PATH_LEN      512     // 路径模式最大长度
#define VFS_MAX_PKG_LEN       256     // 包名最大长度

/* v3 新增 - Pipe通讯 */
#define VFS_CMD_MAGIC         0xAF5F  // 命令Magic Number
#define VFS_CMD_VERSION       2       // 命令协议版本
#define VFS_PIPE_TIMEOUT_MS   5000    // Pipe超时 (毫秒)
#define VFS_PIPE_NAME_PREFIX  "aurora_vfs_"  // Pipe名称前缀
#define VFS_PIPE_PATH_LEN     32      // Pipe路径最大长度

/* v3 新增 - Netlink通讯 */
#define AURORA_VFS_NL_FAMILY  NETLINK_USERSOCK  // Netlink协议族
#define AURORA_VFS_NL_GROUP   31      // 多播组号
#define VFS_NL_MAX_MSG_LEN    4096    // 单条事件最大长度
#define VFS_NL_SND_BUF_SIZE   16384   // 内核发送缓冲区
#define VFS_NL_RCV_BUF_SIZE   65536   // 用户空间接收缓冲区
```

### 9.2 核心结构体

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

    /* v3 新增 - Netlink */
    struct sock *nlsk;               // netlink socket
    struct netlink_kernel_cfg nl_cfg; // netlink配置

    /* v3 新增 - Pipe */
    struct task_struct *pipe_thread;  // pipe监听线程
    struct miscdevice pipe_misc;      // misc设备 (pipe读取)
};
```

---

## 10. sysfs 属性表参考

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

## 11. GKI KMI 符号依赖

内核模块仅允许使用以下GKI导出的符号：

### 11.1 必需符号

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
| netlink | `netlink_kernel_create`, `netlink_kernel_release`, `netlink_has_listeners`, `nlmsg_multicast`, `nlmsg_put`, `nlmsg_data`, `nlmsg_space`, `alloc_skb`, `NETLINK_CB`, `netlink_unicast` | netlink通讯 |
| misc | `misc_register`, `misc_deregister` | misc设备注册 (pipe读取) |
| 线程 | `kthread_create`, `kthread_stop`, `kthread_should_stop` | 内核线程 (pipe监听) |

### 11.2 禁止使用

- 任何非GKI导出的OEM私有符号
- `printk`, `pr_info`, `pr_err`, `pr_debug` (用 `trace_printk` 替代)
- 任何需要特定内核配置的符号

---

## 12. Makefile 规范

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

## 13. 模块生命周期

### 13.1 加载 (insmod)

1. 创建 `/sys/kernel/ztrosu/vfs/` 目录
2. 创建所有sysfs属性文件
3. 初始化统计计数器为0
4. 初始化策略: enabled=false, log_level=0, default_action=allow
5. 初始化空规则链表和空Hook链表
6. 创建netlink socket (NETLINK_USERSOCK, 多播组31)
7. 注册misc设备 (用于pipe读取)
8. 启动pipe监听内核线程
9. 输出: `trace_printk("aurora_vfs: module loaded\n");`

### 13.2 运行中

- enabled=false 时: 不进行任何VFS操作监控
- enabled=true 时: 开始监控和拦截

### 13.3 卸载 (rmmod)

1. 设置 enabled=false，停止所有监控
2. 停止pipe监听线程
3. 注销misc设备
4. 释放netlink socket
5. 移除所有sysfs属性
6. 释放kobject
7. 释放所有规则内存
8. 释放所有Hook目标内存
9. 销毁锁
10. 输出: `trace_printk("aurora_vfs: module unloaded\n");`

---

## 14. 与用户空间的完整交互流程

### 14.1 初始化流程

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
   │── 通过pipe发送CMD_SET_RULES ──────→ │  (批量设置规则)
   │   mkfifo → 写入 → unlink           │
   │←── pipe处理完成 ──────────────────│
   │                                     │
   │── 通过pipe发送CMD_ADD_HOOK ───────→ │  (添加Hook目标)
   │   mkfifo → 写入 → unlink           │
   │←── pipe处理完成 ──────────────────│
   │                                     │
   │── 订阅netlink事件组 ─────────────→ │  (开始接收实时事件)
   │←── 事件流开始推送 ───────────────│
```

### 14.2 运行时监控流程

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
       │      │
       │      ▼
       │  通过netlink推送vfs_event ──→ 用户空间
```

---

## 15. 测试验证清单

内核模块开发者完成实现后，应验证以下项目：

### 15.1 sysfs接口测试

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

### 15.2 Pipe通道测试

- [ ] 用户空间 `mkfifo /dev/aurora_vfs_<random>` 成功创建管道
- [ ] 写入正确的 `vfs_command` (magic=0xAF5F, version=2) 后内核成功处理
- [ ] 写入错误magic的命令后内核丢弃并销毁管道
- [ ] 写入错误version的命令后内核丢弃并销毁管道
- [ ] `CMD_ADD_HOOK` (PID类型) 成功添加Hook目标
- [ ] `CMD_ADD_HOOK` (PACKAGE类型) 成功添加Hook目标
- [ ] `CMD_REMOVE_HOOK` 成功移除Hook目标
- [ ] `CMD_SET_RULES` 成功批量设置规则
- [ ] `CMD_CLEAR_RULES` 成功清空规则
- [ ] `CMD_SET_POLICY` 成功设置策略参数
- [ ] `CMD_RESET_STATS` 成功重置统计计数器
- [ ] 通讯完毕后管道被正确 `unlink` 销毁
- [ ] 5秒超时后未使用的管道被自动销毁
- [ ] 管道权限为0600，非root进程无法访问

### 15.3 Netlink通道测试

- [ ] 内核模块加载后创建netlink socket
- [ ] 用户空间root进程成功绑定并加入多播组31
- [ ] 非root进程无法绑定netlink socket
- [ ] VFS操作发生时内核正确推送 `EVENT_VFS_OPEN/READ/WRITE/CLOSE`
- [ ] 访问被拒绝时内核正确推送 `EVENT_VFS_DENY`
- [ ] Hook目标添加后内核推送 `EVENT_HOOK_ADDED`
- [ ] Hook目标移除后内核推送 `EVENT_HOOK_REMOVED`
- [ ] 规则变更后内核推送 `EVENT_RULE_CHANGED`
- [ ] 事件中包含正确的 magic (0xAF5F)
- [ ] 事件中包含正确的 pid, uid, path, timestamp, result
- [ ] log_level=0 时无事件推送
- [ ] 多个用户空间进程可同时订阅事件

### 15.4 功能测试

- [ ] enabled=0 时，不拦截任何操作
- [ ] enabled=1 时，按规则拦截
- [ ] 规则优先级正确
- [ ] glob通配符匹配正确
- [ ] Hook目标PID匹配正确
- [ ] Hook目标UID匹配正确
- [ ] 统计计数器正确更新
- [ ] trace_printk 输出可通过 `/sys/kernel/tracing/trace` 查看
- [ ] rmmod 后无内存泄漏
- [ ] rmmod 后netlink socket正确释放
- [ ] rmmod 后pipe监听线程正确停止

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

### v1 → v2 变更

| 旧版接口 (v1) | 新版接口 (v2) | 变更说明 |
|---------------|---------------|---------|
| `rules` 只支持单行写入 | `rules` 支持多行批量写入 | **必须修复** |
| 无 `hook_targets` | 新增 `hook_targets` | **新增** |
| 无 `hook_list` | 新增 `hook_list` | **新增** |
| 无 `version` | 新增 `version` | **新增** |
| `rules` 格式 `action:path:mode` | 保持不变 | 兼容 |
| `stats` 格式 `key:value` | 保持不变 | 兼容 |

### v2 → v3 变更 (三通道架构)

| 维度 | v2 | v3 | 变更说明 |
|------|----|----|---------|
| 通讯架构 | 纯sysfs | sysfs + pipe + netlink | **架构升级** |
| sysfs接口 | 全部保留 | 全部保留不变 | **完全兼容** |
| 命令下发 | sysfs写入 (hook_targets, rules等) | pipe二进制协议 | **新增通道** (sysfs写入仍可用) |
| 事件推送 | 无 (需轮询stats) | netlink异步推送 | **新增功能** |
| Hook添加 | sysfs写入文本命令 | pipe发送CMD_ADD_HOOK | **新增方式** |
| 规则设置 | sysfs写入文本规则 | pipe发送CMD_SET_RULES | **新增方式** |
| 实时监控 | 无 | netlink事件流 | **新增功能** |
| 安全机制 | 文件系统权限 | pipe随机命名+超时, netlink UID过滤 | **增强** |

**兼容性保证**: v3完全保留v2的所有sysfs接口，现有基于sysfs的用户空间代码无需修改即可继续工作。pipe和netlink是新增的通讯方式，用户空间可选择性地迁移到新通道以获得更好的性能和实时性。
