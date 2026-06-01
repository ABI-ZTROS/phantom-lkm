#!/bin/bash
#
# OnePlus ACE5 (SM8650/pineapple) ARM64 Clang内核编译环境配置脚本
# 适用于Ubuntu 22.04
# 内核版本: 6.1.141 (Android 14 GKI)
#

set -e

# 颜色定义
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# 日志函数
log_info() {
    echo -e "${BLUE}[INFO]${NC} $1"
}

log_success() {
    echo -e "${GREEN}[SUCCESS]${NC} $1"
}

log_warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

# 错误处理函数
error_exit() {
    log_error "$1"
    exit 1
}

# 重试函数
retry_command() {
    local max_attempts=$1
    local delay=$2
    local command="${@:3}"
    local attempt=1

    while [ $attempt -le $max_attempts ]; do
        log_info "执行: $command (尝试 $attempt/$max_attempts)"
        if eval "$command"; then
            return 0
        fi
        log_warn "命令失败，等待 ${delay} 秒后重试..."
        sleep $delay
        attempt=$((attempt + 1))
    done

    error_exit "命令在 $max_attempts 次尝试后仍然失败: $command"
}

# 检查root权限
check_root() {
    if [ "$EUID" -ne 0 ]; then
        log_warn "某些操作需要root权限，建议在需要时使用sudo"
    fi
}

# 获取脚本所在目录
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORK_DIR="${SCRIPT_DIR}"
KERNEL_DIR="${WORK_DIR}/kernel_source"
TOOLCHAIN_DIR="${WORK_DIR}/toolchain"

# 创建目录结构
log_info "创建工作目录..."
mkdir -p "$KERNEL_DIR"
mkdir -p "$TOOLCHAIN_DIR"
log_success "工作目录创建完成"

# 系统依赖列表
DEPENDENCIES=(
    "build-essential"
    "bc"
    "bison"
    "flex"
    "libssl-dev"
    "libelf-dev"
    "python3"
    "python3-pip"
    "git"
    "curl"
    "wget"
    "unzip"
    "cpio"
    "rsync"
    "jq"
    "file"
    "ccache"
    "libncurses5-dev"
    "libncursesw5-dev"
    "lz4"
    "zstd"
    "device-tree-compiler"
    "lzop"
    "libarchive-tools"
)

# 安装依赖
install_dependencies() {
    log_info "更新软件包列表..."
    retry_command 3 5 "sudo apt-get update"

    log_info "安装编译依赖..."
    for pkg in "${DEPENDENCIES[@]}"; do
        if ! dpkg -l | grep -q "^ii  $pkg "; then
            log_info "安装 $pkg..."
            retry_command 3 5 "sudo apt-get install -y $pkg"
        else
            log_info "$pkg 已安装，跳过"
        fi
    done

    log_success "所有依赖安装完成"
}

# 安装repo工具
install_repo() {
    log_info "安装repo工具..."
    if ! command -v repo &> /dev/null; then
        retry_command 3 5 "curl https://storage.googleapis.com/git-repo-downloads/repo > /tmp/repo"
        chmod a+x /tmp/repo
        sudo mv /tmp/repo /usr/local/bin/repo
        log_success "repo工具安装完成"
    else
        log_info "repo工具已存在"
    fi
}

# 下载Android Clang工具链
download_clang() {
    log_info "下载Android Clang工具链..."

    # 推荐的Clang版本 (Android 14 GKI)
    CLANG_VERSION="r450784e"  # Android 14推荐版本
    CLANG_URL="https://android.googlesource.com/platform/prebuilts/clang/host/linux-x86/+archive/refs/heads/main/clang-${CLANG_VERSION}.tar.gz"

    local clang_dir="${TOOLCHAIN_DIR}/clang-${CLANG_VERSION}"

    if [ -d "$clang_dir" ] && [ -f "$clang_dir/bin/clang" ]; then
        log_info "Clang ${CLANG_VERSION} 已存在，跳过下载"
    else
        log_info "下载 Clang ${CLANG_VERSION}..."
        mkdir -p "$clang_dir"
        retry_command 3 10 "wget -q --show-progress -O /tmp/clang-${CLANG_VERSION}.tar.gz ${CLANG_URL}"

        log_info "解压Clang工具链..."
        tar -xzf /tmp/clang-${CLANG_VERSION}.tar.gz -C "$clang_dir" --strip-components=1 || {
            # 尝试备用下载源
            log_warn "主下载源失败，尝试备用源..."
            retry_command 3 10 "wget -q --show-progress -O /tmp/clang-${CLANG_VERSION}.tar.gz https://github.com/AOSP-mirror/platform_prebuilts_clang_host_linux-x86/releases/download/android-14.0.0_r1/clang-${CLANG_VERSION}.tar.gz"
            tar -xzf /tmp/clang-${CLANG_VERSION}.tar.gz -C "$clang_dir" --strip-components=1
        }

        rm -f /tmp/clang-${CLANG_VERSION}.tar.gz
        log_success "Clang工具链下载完成"
    fi

    # 验证Clang
    if [ ! -f "$clang_dir/bin/clang" ]; then
        error_exit "Clang安装验证失败"
    fi

    echo "$clang_dir"
}

# 下载预构建工具链 (包含binutils)
download_prebuilts() {
    log_info "下载Android预构建工具链..."

    local prebuilt_dir="${TOOLCHAIN_DIR}/prebuilts"
    mkdir -p "$prebuilt_dir"

    # 下载aarch64-linux-android工具链
    local gcc_url="https://android.googlesource.com/platform/prebuilts/gcc/linux-x86/aarch64/aarch64-linux-android-4.9/+archive/refs/heads/main.tar.gz"
    local gcc_dir="${prebuilt_dir}/aarch64-linux-android-4.9"

    if [ ! -d "$gcc_dir" ]; then
        log_info "下载aarch64-linux-android工具链..."
        retry_command 3 10 "wget -q --show-progress -O /tmp/aarch64-gcc.tar.gz ${gcc_url}"
        mkdir -p "$gcc_dir"
        tar -xzf /tmp/aarch64-gcc.tar.gz -C "$gcc_dir"
        rm -f /tmp/aarch64-gcc.tar.gz
        log_success "aarch64工具链下载完成"
    else
        log_info "aarch64工具链已存在"
    fi

    echo "$prebuilt_dir"
}

# 拉取内核源码
fetch_kernel_source() {
    log_info "拉取OnePlus ACE5内核源码..."

    local manifest_url="https://github.com/OnePlusOSS/kernel_manifest.git"
    local manifest_branch="oneplus/sm8650"
    local manifest_file="oneplus_ace5.xml"

    cd "$KERNEL_DIR"

    if [ -d ".repo" ]; then
        log_info "检测到已有repo，执行sync更新..."
        retry_command 3 10 "repo sync -c -j$(nproc)"
    else
        log_info "初始化repo..."
        retry_command 3 10 "repo init -u ${manifest_url} -b ${manifest_branch} -m ${manifest_file} --depth=1"

        log_info "同步源码 (这可能需要一段时间)..."
        retry_command 3 10 "repo sync -c -j$(nproc) --no-tags --no-clone-bundle"
    fi

    log_success "内核源码拉取完成"
}

# 生成环境变量文件
generate_env_file() {
    log_info "生成环境变量配置文件..."

    local clang_dir="$1"
    local prebuilt_dir="$2"

    cat > "${WORK_DIR}/env.sh" << 'EOF'
#!/bin/bash
#
# OnePlus ACE5 (SM8650/pineapple) 内核编译环境变量
# 使用方法: source ./env.sh
#

# 基础目录
export WORK_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export KERNEL_DIR="${WORK_DIR}/kernel_source"
export TOOLCHAIN_DIR="${WORK_DIR}/toolchain"

# Clang工具链配置
export CLANG_VERSION="r450784e"
export CLANG_DIR="${TOOLCHAIN_DIR}/clang-${CLANG_VERSION}"
export PATH="${CLANG_DIR}/bin:${PATH}"

# GCC工具链配置 (用于链接器和部分工具)
export GCC_DIR="${TOOLCHAIN_DIR}/prebuilts/aarch64-linux-android-4.9"
export PATH="${GCC_DIR}/bin:${PATH}"

# 架构设置
export ARCH=arm64
export SUBARCH=arm64

# 交叉编译器设置
export CROSS_COMPILE=aarch64-linux-android-
export CROSS_COMPILE_ARM32=arm-linux-androideabi-

# Clang编译器设置
export CC="clang"
export CXX="clang++"
export AR="llvm-ar"
export NM="llvm-nm"
export OBJCOPY="llvm-objcopy"
export OBJDUMP="llvm-objdump"
export READELF="llvm-readelf"
export OBJSIZE="llvm-size"
export STRIP="llvm-strip"
export LLVM_AR="llvm-ar"
export LLVM_NM="llvm-nm"

# GKI编译标志
export LLVM=1
export LLVM_IAS=1

# 内核配置
export DEFCONFIG="gki_defconfig"
export DEVICE_DEFCONFIG="oneplus_ace5_defconfig"

# 编译选项
export KBUILD_BUILD_USER="android"
export KBUILD_BUILD_HOST="localhost"
export KBUILD_BUILD_TIMESTAMP="$(date)"

# ccache加速
export CCACHE_EXEC="$(which ccache)"
export USE_CCACHE=1
export CCACHE_DIR="${WORK_DIR}/.ccache"
export CCACHE_MAXSIZE="50G"

# 输出目录
export OUT_DIR="${WORK_DIR}/out"
export DIST_DIR="${OUT_DIR}/dist"

# 模块编译目录
export KDIR="${KERNEL_DIR}/common"

# 添加到PATH
export PATH="${OUT_DIR}:${PATH}"

echo "============================================"
echo "OnePlus ACE5 内核编译环境已加载"
echo "============================================"
echo "ARCH:           ${ARCH}"
echo "CROSS_COMPILE:  ${CROSS_COMPILE}"
echo "CC:             ${CC}"
echo "CLANG_DIR:      ${CLANG_DIR}"
echo "KERNEL_DIR:     ${KERNEL_DIR}"
echo "KDIR:           ${KDIR}"
echo "============================================"
EOF

    chmod +x "${WORK_DIR}/env.sh"
    log_success "环境变量文件已生成: ${WORK_DIR}/env.sh"
}

# 生成README文档
generate_readme() {
    log_info "生成README文档..."

    cat > "${WORK_DIR}/README.md" << 'EOF'
# OnePlus ACE5 (SM8650/pineapple) 内核编译环境

## 设备信息
- 设备: OnePlus ACE5
- SoC: SM8650 (代号pineapple)
- 内核版本: 6.1.141 (Android 14 GKI)
- 架构: ARM64

## 源码信息
- Manifest仓库: https://github.com/OnePlusOSS/kernel_manifest.git
- 分支: oneplus/sm8650
- Manifest文件: oneplus_ace5.xml

## 目录结构
```
kernel_lkm/
├── env.sh              # 环境变量配置文件
├── setup_env.sh        # 环境配置脚本
├── verify_headers.sh   # 头文件验证脚本
├── build_module.sh     # 模块编译脚本
├── kernel_source/      # 内核源码目录
├── toolchain/          # 工具链目录
│   ├── clang-r450784e/ # Clang工具链
│   └── prebuilts/      # 预构建工具
└── out/                # 编译输出目录
```

## 使用方法

### 1. 配置环境
```bash
cd /workspace/AuroraSU/kernel_lkm
./setup_env.sh
```

### 2. 加载环境变量
```bash
source ./env.sh
```

### 3. 验证头文件
```bash
./verify_headers.sh
```

### 4. 编译外部模块
```bash
./build_module.sh /path/to/your/module
```

## 环境变量说明

| 变量 | 说明 |
|------|------|
| ARCH | 目标架构 (arm64) |
| CROSS_COMPILE | 交叉编译前缀 |
| CC | C编译器 (clang) |
| CLANG_DIR | Clang工具链路径 |
| KERNEL_DIR | 内核源码根目录 |
| KDIR | 模块编译使用的内核目录 |
| LLVM | 使用LLVM工具链 |
| LLVM_IAS | 使用LLVM集成汇编器 |

## 编译选项

### 编译内核
```bash
source ./env.sh
cd $KERNEL_DIR/common
make $DEFCONFIG
make -j$(nproc)
```

### 编译模块
```bash
source ./env.sh
cd /path/to/module
make KDIR=$KDIR
```

## 注意事项

1. 确保有足够的磁盘空间 (建议至少50GB)
2. 需要稳定的网络连接下载源码和工具链
3. 首次编译建议使用ccache加速
4. 部分操作需要sudo权限

## 故障排除

### 如果repo sync失败
```bash
cd kernel_source
repo sync -c -j4 --fail-fast
```

### 如果Clang下载失败
```bash
# 手动下载并解压到toolchain/clang-r450784e/
```

### 清理编译环境
```bash
make clean
make mrproper
```
EOF

    log_success "README文档已生成"
}

# 主函数
main() {
    log_info "============================================"
    log_info "OnePlus ACE5 (SM8650) 内核编译环境配置"
    log_info "============================================"
    log_info "工作目录: $WORK_DIR"
    log_info "内核目录: $KERNEL_DIR"
    log_info "工具链目录: $TOOLCHAIN_DIR"
    log_info "============================================"

    check_root

    # 安装依赖
    install_dependencies

    # 安装repo
    install_repo

    # 下载工具链
    local clang_dir=$(download_clang)
    local prebuilt_dir=$(download_prebuilts)

    # 拉取内核源码
    fetch_kernel_source

    # 生成环境文件
    generate_env_file "$clang_dir" "$prebuilt_dir"

    # 生成文档
    generate_readme

    log_info "============================================"
    log_success "环境配置完成!"
    log_info "============================================"
    log_info "使用方法:"
    log_info "1. source ./env.sh        # 加载环境变量"
    log_info "2. ./verify_headers.sh    # 验证头文件"
    log_info "3. ./build_module.sh      # 编译模块"
    log_info "============================================"
    log_info "内核源码位置: $KERNEL_DIR"
    log_info "工具链位置: $TOOLCHAIN_DIR"
    log_info "============================================"
}

# 执行主函数
main "$@"
