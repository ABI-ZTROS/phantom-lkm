#!/bin/bash
#
# OnePlus ACE5 (SM8650/pineapple) 外部模块编译脚本
# 用于编译内核外部模块 (.ko文件)
#

set -e

# 颜色定义
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
NC='\033[0m'

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

log_header() {
    echo -e "${CYAN}============================================${NC}"
    echo -e "${CYAN}$1${NC}"
    echo -e "${CYAN}============================================${NC}"
}

# 获取脚本目录
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ENV_FILE="${SCRIPT_DIR}/env.sh"

# 编译日志
BUILD_LOG="${SCRIPT_DIR}/build_$(date +%Y%m%d_%H%M%S).log"

# 初始化日志
init_log() {
    cat > "$BUILD_LOG" << EOF
OnePlus ACE5 (SM8650/pineapple) 模块编译日志
============================================
开始时间: $(date)
============================================

EOF
}

# 写入日志
write_log() {
    echo "$1" >> "$BUILD_LOG"
}

# 显示帮助信息
show_help() {
    cat << EOF
OnePlus ACE5 (SM8650/pineapple) 外部模块编译脚本

用法:
    $0 [选项] <模块路径>

选项:
    -h, --help          显示此帮助信息
    -c, --clean         清理编译输出
    -j, --jobs N        指定并行编译任务数 (默认: $(nproc))
    -o, --output DIR    指定输出目录
    -v, --verbose       详细输出
    --no-sign           不签名模块
    --strip             剥离模块符号

参数:
    模块路径            外部模块源码目录 (包含Makefile)

示例:
    $0 /path/to/my_module
    $0 -c /path/to/my_module
    $0 -j4 -o /tmp/output /path/to/my_module

环境变量:
    KDIR                内核源码目录 (默认从env.sh加载)
    CROSS_COMPILE       交叉编译器前缀
    CC                  C编译器
    ARCH                目标架构

EOF
}

# 解析参数
MODULE_PATH=""
CLEAN=0
JOBS=$(nproc)
OUTPUT_DIR=""
VERBOSE=0
SIGN_MODULE=1
STRIP_MODULE=0

parse_args() {
    while [[ $# -gt 0 ]]; do
        case $1 in
            -h|--help)
                show_help
                exit 0
                ;;
            -c|--clean)
                CLEAN=1
                shift
                ;;
            -j|--jobs)
                JOBS="$2"
                shift 2
                ;;
            -o|--output)
                OUTPUT_DIR="$2"
                shift 2
                ;;
            -v|--verbose)
                VERBOSE=1
                shift
                ;;
            --no-sign)
                SIGN_MODULE=0
                shift
                ;;
            --strip)
                STRIP_MODULE=1
                shift
                ;;
            -*)
                log_error "未知选项: $1"
                show_help
                exit 1
                ;;
            *)
                if [ -z "$MODULE_PATH" ]; then
                    MODULE_PATH="$1"
                else
                    log_error "多余的参数: $1"
                    exit 1
                fi
                shift
                ;;
        esac
    done

    if [ -z "$MODULE_PATH" ]; then
        log_error "未指定模块路径"
        show_help
        exit 1
    fi

    if [ ! -d "$MODULE_PATH" ]; then
        log_error "模块路径不存在: $MODULE_PATH"
        exit 1
    fi

    MODULE_PATH="$(cd "$MODULE_PATH" && pwd)"

    if [ -z "$OUTPUT_DIR" ]; then
        OUTPUT_DIR="${MODULE_PATH}/out"
    fi

    mkdir -p "$OUTPUT_DIR"
}

# 检查环境
check_environment() {
    log_header "检查编译环境"
    write_log "1. 环境检查"

    # 检查env.sh是否存在
    if [ ! -f "$ENV_FILE" ]; then
        log_error "环境文件不存在: $ENV_FILE"
        log_info "请先运行 ./setup_env.sh"
        exit 1
    fi
    log_success "环境文件存在: env.sh"
    write_log "[PASS] 环境文件存在: env.sh"

    # 加载环境变量
    log_info "加载环境变量..."
    set +e
    source "$ENV_FILE" 2>/dev/null
    set -e

    # 检查关键环境变量
    if [ -z "$KDIR" ]; then
        log_warn "KDIR未设置，尝试使用默认路径"
        export KDIR="${SCRIPT_DIR}/kernel_source/common"
    fi

    if [ ! -d "$KDIR" ]; then
        log_error "内核目录不存在: $KDIR"
        log_info "请确保已运行 ./setup_env.sh 拉取内核源码"
        exit 1
    fi
    log_success "内核目录: $KDIR"
    write_log "[PASS] 内核目录: $KDIR"

    # 检查编译器
    if ! command -v clang &> /dev/null; then
        log_error "Clang未找到，请检查环境配置"
        exit 1
    fi
    log_success "Clang: $(clang --version | head -n1)"
    write_log "[PASS] Clang可用"

    # 检查模块Makefile
    if [ ! -f "${MODULE_PATH}/Makefile" ]; then
        log_error "模块Makefile不存在: ${MODULE_PATH}/Makefile"
        exit 1
    fi
    log_success "模块Makefile存在"
    write_log "[PASS] 模块Makefile存在"

    echo ""
}

# 准备编译环境
prepare_build() {
    log_header "准备编译环境"
    write_log ""
    write_log "2. 准备编译环境"

    cd "$KDIR"

    # 检查是否已有配置文件
    if [ ! -f ".config" ]; then
        log_info "未检测到.config，尝试使用默认配置..."

        # 查找可用的defconfig
        local defconfig=""
        if [ -f "arch/arm64/configs/gki_defconfig" ]; then
            defconfig="gki_defconfig"
        elif [ -f "arch/arm64/configs/defconfig" ]; then
            defconfig="defconfig"
        elif [ -n "$DEFCONFIG" ] && [ -f "arch/arm64/configs/$DEFCONFIG" ]; then
            defconfig="$DEFCONFIG"
        fi

        if [ -n "$defconfig" ]; then
            log_info "使用defconfig: $defconfig"
            make ARCH=arm64 LLVM=1 $defconfig 2>&1 | tee -a "$BUILD_LOG"
        else
            log_warn "未找到defconfig，尝试使用现有配置"
            make ARCH=arm64 LLVM=1 defconfig 2>&1 | tee -a "$BUILD_LOG" || true
        fi
    fi

    # 执行modules_prepare
    if [ ! -f "Module.symvers" ]; then
        log_info "执行 modules_prepare..."
        if make ARCH=arm64 LLVM=1 modules_prepare 2>&1 | tee -a "$BUILD_LOG"; then
            log_success "modules_prepare 完成"
            write_log "[PASS] modules_prepare 完成"
        else
            log_error "modules_prepare 失败"
            write_log "[FAIL] modules_prepare 失败"
            exit 1
        fi
    else
        log_info "Module.symvers已存在，跳过modules_prepare"
    fi

    echo ""
}

# 清理编译输出
clean_build() {
    log_header "清理编译输出"
    write_log ""
    write_log "3. 清理编译输出"

    cd "$MODULE_PATH"

    if [ -f "Makefile" ]; then
        log_info "执行 make clean..."
        make -C "$KDIR" M="$MODULE_PATH" clean 2>&1 | tee -a "$BUILD_LOG" || true
    fi

    # 清理输出目录
    if [ -d "$OUTPUT_DIR" ]; then
        rm -rf "$OUTPUT_DIR"
        mkdir -p "$OUTPUT_DIR"
    fi

    log_success "清理完成"
    write_log "[PASS] 清理完成"

    if [ $CLEAN -eq 1 ]; then
        exit 0
    fi

    echo ""
}

# 编译模块
build_module() {
    log_header "编译模块"
    write_log ""
    write_log "4. 编译模块"

    cd "$MODULE_PATH"

    # 设置编译参数
    local make_args=()
    make_args+=("-C" "$KDIR")
    make_args+=("M=$MODULE_PATH")
    make_args+=("ARCH=arm64")
    make_args+=("LLVM=1")
    make_args+=("-j$JOBS")

    if [ $VERBOSE -eq 1 ]; then
        make_args+=("V=1")
    fi

    # 设置输出目录
    export KBUILD_OUTPUT="$OUTPUT_DIR"

    log_info "开始编译..."
    log_info "模块路径: $MODULE_PATH"
    log_info "内核目录: $KDIR"
    log_info "并行任务: $JOBS"

    write_log "模块路径: $MODULE_PATH"
    write_log "内核目录: $KDIR"
    write_log "并行任务: $JOBS"

    # 执行编译
    if make "${make_args[@]}" 2>&1 | tee -a "$BUILD_LOG"; then
        log_success "编译成功"
        write_log "[PASS] 编译成功"
    else
        log_error "编译失败"
        write_log "[FAIL] 编译失败"
        exit 1
    fi

    echo ""
}

# 查找编译生成的.ko文件
find_modules() {
    log_header "查找编译输出"
    write_log ""
    write_log "5. 查找编译输出"

    local modules=()
    while IFS= read -r -d '' module; do
        modules+=("$module")
    done < <(find "$MODULE_PATH" -name "*.ko" -type f -print0 2>/dev/null)

    if [ ${#modules[@]} -eq 0 ]; then
        log_warn "未找到.ko文件"
        write_log "[WARN] 未找到.ko文件"
        return 1
    fi

    log_success "找到 ${#modules[@]} 个模块文件:"
    write_log "[PASS] 找到 ${#modules[@]} 个模块文件"

    for module in "${modules[@]}"; do
        local size=$(stat -c%s "$module" 2>/dev/null || stat -f%z "$module" 2>/dev/null)
        local size_human=$(numfmt --to=iec $size 2>/dev/null || echo "${size}B")
        log_info "  - $(basename "$module") (${size_human})"
        write_log "  - $module (${size_human})"

        # 复制到输出目录
        cp "$module" "$OUTPUT_DIR/"
    done

    echo ""
}

# 签名模块
sign_modules() {
    if [ $SIGN_MODULE -eq 0 ]; then
        log_info "跳过模块签名"
        return 0
    fi

    log_header "签名模块"
    write_log ""
    write_log "6. 签名模块"

    # 检查是否有签名密钥
    local sign_key="${KDIR}/certs/signing_key.pem"
    local sign_cert="${KDIR}/certs/signing_key.x509"

    if [ ! -f "$sign_key" ] || [ ! -f "$sign_cert" ]; then
        log_warn "签名密钥不存在，跳过签名"
        log_info "密钥位置: ${KDIR}/certs/"
        write_log "[WARN] 签名密钥不存在，跳过签名"
        return 0
    fi

    # 查找需要签名的模块
    local modules=()
    while IFS= read -r -d '' module; do
        modules+=("$module")
    done < <(find "$OUTPUT_DIR" -name "*.ko" -type f -print0 2>/dev/null)

    for module in "${modules[@]}"; do
        log_info "签名: $(basename "$module")"

        # 使用内核脚本签名
        if [ -f "${KDIR}/scripts/sign-file" ]; then
            "${KDIR}/scripts/sign-file" sha256 "$sign_key" "$sign_cert" "$module" 2>&1 | tee -a "$BUILD_LOG"
            log_success "签名完成: $(basename "$module")"
            write_log "[PASS] 签名完成: $(basename "$module")"
        else
            log_warn "sign-file脚本不存在"
            write_log "[WARN] sign-file脚本不存在"
            break
        fi
    done

    echo ""
}

# 剥离模块符号
strip_modules() {
    if [ $STRIP_MODULE -eq 0 ]; then
        return 0
    fi

    log_header "剥离模块符号"
    write_log ""
    write_log "7. 剥离模块符号"

    local modules=()
    while IFS= read -r -d '' module; do
        modules+=("$module")
    done < <(find "$OUTPUT_DIR" -name "*.ko" -type f -print0 2>/dev/null)

    for module in "${modules[@]}"; do
        local before_size=$(stat -c%s "$module" 2>/dev/null || stat -f%z "$module" 2>/dev/null)

        if command -v llvm-strip &> /dev/null; then
            llvm-strip --strip-debug "$module"
        elif command -v ${CROSS_COMPILE}strip &> /dev/null; then
            ${CROSS_COMPILE}strip --strip-debug "$module"
        else
            log_warn "未找到strip工具"
            write_log "[WARN] 未找到strip工具"
            return 0
        fi

        local after_size=$(stat -c%s "$module" 2>/dev/null || stat -f%z "$module" 2>/dev/null)
        local saved=$((before_size - after_size))
        local saved_human=$(numfmt --to=iec $saved 2>/dev/null || echo "${saved}B")

        log_success "剥离完成: $(basename "$module") (节省 $saved_human)"
        write_log "[PASS] 剥离完成: $(basename "$module") (节省 $saved_human)"
    done

    echo ""
}

# 验证模块
verify_modules() {
    log_header "验证模块"
    write_log ""
    write_log "8. 验证模块"

    local modules=()
    while IFS= read -r -d '' module; do
        modules+=("$module")
    done < <(find "$OUTPUT_DIR" -name "*.ko" -type f -print0 2>/dev/null)

    if [ ${#modules[@]} -eq 0 ]; then
        log_warn "输出目录中没有.ko文件"
        write_log "[WARN] 输出目录中没有.ko文件"
        return 1
    fi

    for module in "${modules[@]}"; do
        log_info "验证: $(basename "$module")"

        # 检查文件类型
        local file_info=$(file "$module")
        log_info "  文件类型: $file_info"
        write_log "  文件类型: $file_info"

        # 检查模块信息
        if command -v ${CROSS_COMPILE}modinfo &> /dev/null; then
            local modinfo=$(${CROSS_COMPILE}modinfo "$module" 2>/dev/null | head -n10)
            log_info "  模块信息:"
            echo "$modinfo" | while read line; do
                log_info "    $line"
            done
            write_log "  模块信息:"
            write_log "$modinfo"
        fi

        # 检查依赖
        if command -v ${CROSS_COMPILE}readelf &> /dev/null; then
            local deps=$(${CROSS_COMPILE}readelf -d "$module" 2>/dev/null | grep NEEDED || true)
            if [ -n "$deps" ]; then
                log_info "  依赖库:"
                echo "$deps" | while read line; do
                    log_info "    $line"
                done
            fi
        fi
    done

    echo ""
}

# 生成编译报告
generate_report() {
    log_header "编译报告"
    write_log ""
    write_log "9. 编译报告"

    local modules=()
    while IFS= read -r -d '' module; do
        modules+=("$module")
    done < <(find "$OUTPUT_DIR" -name "*.ko" -type f -print0 2>/dev/null)

    log_success "编译完成!"
    log_info "输出目录: $OUTPUT_DIR"
    log_info "模块数量: ${#modules[@]}"
    log_info "编译日志: $BUILD_LOG"

    write_log "[PASS] 编译完成"
    write_log "输出目录: $OUTPUT_DIR"
    write_log "模块数量: ${#modules[@]}"

    cat >> "$BUILD_LOG" << EOF

============================================
编译报告摘要
============================================
完成时间: $(date)
模块路径: $MODULE_PATH
内核目录: $KDIR
输出目录: $OUTPUT_DIR

编译模块:
EOF

    for module in "${modules[@]}"; do
        local size=$(stat -c%s "$module" 2>/dev/null || stat -f%z "$module" 2>/dev/null)
        local size_human=$(numfmt --to=iec $size 2>/dev/null || echo "${size}B")
        echo "  - $(basename "$module") (${size_human})" >> "$BUILD_LOG"
    done

    cat >> "$BUILD_LOG" << EOF

============================================
EOF

    echo ""
    log_success "编译报告已保存: $BUILD_LOG"
}

# 主函数
main() {
    log_header "OnePlus ACE5 (SM8650/pineapple) 模块编译"

    init_log
    parse_args "$@"
    check_environment
    prepare_build
    clean_build
    build_module
    find_modules
    sign_modules
    strip_modules
    verify_modules
    generate_report

    log_success "全部完成!"
}

# 执行主函数
main "$@"
