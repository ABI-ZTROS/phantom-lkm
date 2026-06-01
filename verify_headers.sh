#!/bin/bash
#
# OnePlus ACE5 (SM8650/pineapple) 头文件验证脚本
# 验证内核模块编译所需的头文件和符号导出
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

# 验证报告
REPORT_FILE="${SCRIPT_DIR}/verify_report.txt"
ERRORS=0
WARNS=0

# 初始化报告
init_report() {
    cat > "$REPORT_FILE" << EOF
OnePlus ACE5 (SM8650/pineapple) 头文件验证报告
============================================
生成时间: $(date)
============================================

EOF
}

# 写入报告
write_report() {
    echo "$1" >> "$REPORT_FILE"
}

# 检查环境
check_environment() {
    log_header "检查编译环境"
    write_report "1. 环境检查"
    write_report "----------------------------------------"

    # 检查env.sh是否存在
    if [ ! -f "$ENV_FILE" ]; then
        log_error "环境文件不存在: $ENV_FILE"
        log_info "请先运行 ./setup_env.sh"
        exit 1
    fi
    log_success "环境文件存在: env.sh"
    write_report "[PASS] 环境文件存在: env.sh"

    # 加载环境变量
    log_info "加载环境变量..."
    set +e
    source "$ENV_FILE" 2>/dev/null
    set -e

    # 检查关键环境变量
    local vars=("ARCH" "CROSS_COMPILE" "CC" "CLANG_DIR" "KERNEL_DIR" "KDIR")
    for var in "${vars[@]}"; do
        if [ -z "${!var}" ]; then
            log_warn "环境变量未设置: $var"
            write_report "[WARN] 环境变量未设置: $var"
            WARNS=$((WARNS + 1))
        else
            log_success "环境变量已设置: $var=${!var}"
            write_report "[PASS] 环境变量已设置: $var=${!var}"
        fi
    done

    # 检查Clang
    if command -v clang &> /dev/null; then
        local clang_version=$(clang --version | head -n1)
        log_success "Clang可用: $clang_version"
        write_report "[PASS] Clang可用: $clang_version"
    else
        log_error "Clang未找到"
        write_report "[FAIL] Clang未找到"
        ERRORS=$((ERRORS + 1))
    fi

    echo ""
}

# 检查内核源码目录
check_kernel_source() {
    log_header "检查内核源码"
    write_report ""
    write_report "2. 内核源码检查"
    write_report "----------------------------------------"

    if [ -z "$KERNEL_DIR" ] || [ ! -d "$KERNEL_DIR" ]; then
        log_error "内核源码目录不存在: ${KERNEL_DIR:-未设置}"
        write_report "[FAIL] 内核源码目录不存在"
        ERRORS=$((ERRORS + 1))
        return 1
    fi

    log_success "内核源码目录存在: $KERNEL_DIR"
    write_report "[PASS] 内核源码目录存在: $KERNEL_DIR"

    # 检查常见子目录
    local subdirs=("common" "kernel" "arch" "include" "drivers" "fs" "mm" "net")
    for dir in "${subdirs[@]}"; do
        if [ -d "$KERNEL_DIR/$dir" ]; then
            log_success "子目录存在: $dir"
            write_report "[PASS] 子目录存在: $dir"
        else
            log_warn "子目录不存在: $dir"
            write_report "[WARN] 子目录不存在: $dir"
            WARNS=$((WARNS + 1))
        fi
    done

    # 确定正确的内核源码路径 (GKI通常使用common目录)
    if [ -d "$KERNEL_DIR/common" ]; then
        export KDIR="$KERNEL_DIR/common"
        log_info "使用GKI内核路径: $KDIR"
        write_report "[INFO] 使用GKI内核路径: $KDIR"
    else
        export KDIR="$KERNEL_DIR"
        log_info "使用内核路径: $KDIR"
        write_report "[INFO] 使用内核路径: $KDIR"
    fi

    echo ""
}

# 关键头文件列表
CRITICAL_HEADERS=(
    "linux/module.h"
    "linux/kernel.h"
    "linux/kobject.h"
    "linux/sysfs.h"
    "linux/fs.h"
    "linux/device.h"
    "linux/slab.h"
    "linux/string.h"
    "linux/init.h"
    "linux/moduleparam.h"
    "linux/stat.h"
    "linux/uaccess.h"
    "linux/sched.h"
    "linux/pid.h"
    "linux/cred.h"
    "linux/security.h"
    "linux/selinux.h"
    "linux/xattr.h"
    "linux/dcache.h"
    "linux/path.h"
    "linux/mount.h"
    "linux/nsproxy.h"
    "linux/proc_fs.h"
    "linux/seq_file.h"
    "linux/kallsyms.h"
    "linux/kprobes.h"
    "linux/ftrace.h"
    "linux/syscalls.h"
    "linux/unistd.h"
    "linux/version.h"
    "linux/utsname.h"
    "linux/mm.h"
    "linux/vmalloc.h"
    "linux/ioctl.h"
    "linux/cdev.h"
    "linux/workqueue.h"
    "linux/timer.h"
    "linux/mutex.h"
    "linux/spinlock.h"
    "linux/rwsem.h"
    "linux/list.h"
    "linux/hashtable.h"
    "linux/rbtree.h"
    "linux/radix-tree.h"
    "linux/idr.h"
    "linux/xarray.h"
    "linux/bitmap.h"
    "linux/bitops.h"
    "linux/atomic.h"
    "linux/refcount.h"
    "linux/percpu.h"
    "linux/smp.h"
    "linux/cpumask.h"
    "linux/nodemask.h"
    "linux/topology.h"
    "linux/mmzone.h"
    "linux/gfp.h"
    "linux/highmem.h"
    "linux/pagemap.h"
    "linux/page-flags.h"
    "linux/page_ref.h"
    "linux/mm_types.h"
    "linux/mm_inline.h"
    "linux/swap.h"
    "linux/swapops.h"
    "linux/shmem_fs.h"
    "linux/file.h"
    "linux/fdtable.h"
    "linux/fcntl.h"
    "linux/io.h"
    "linux/ioport.h"
    "linux/resource.h"
    "linux/interrupt.h"
    "linux/irq.h"
    "linux/irqnr.h"
    "linux/irqflags.h"
    "linux/hardirq.h"
    "linux/softirq.h"
    "linux/preempt.h"
    "linux/rcupdate.h"
    "linux/srcu.h"
    "linux/notifier.h"
    "linux/reboot.h"
    "linux/kmod.h"
    "linux/kthread.h"
    "linux/completion.h"
    "linux/wait.h"
    "linux/poll.h"
    "linux/eventfd.h"
    "linux/signalfd.h"
    "linux/aio.h"
    "linux/uio.h"
    "linux/socket.h"
    "linux/sockios.h"
    "linux/net.h"
    "linux/in.h"
    "linux/in6.h"
    "linux/un.h"
    "linux/netdevice.h"
    "linux/skbuff.h"
    "linux/ip.h"
    "linux/ipv6.h"
    "linux/tcp.h"
    "linux/udp.h"
    "linux/icmp.h"
    "linux/icmpv6.h"
    "linux/route.h"
    "linux/netlink.h"
    "linux/rtnetlink.h"
    "linux/genetlink.h"
    "linux/sock_diag.h"
    "linux/inet_diag.h"
    "linux/inet.h"
    "linux/netfilter.h"
    "linux/netfilter_ipv4.h"
    "linux/netfilter_ipv6.h"
    "linux/netfilter_arp.h"
    "linux/netfilter_bridge.h"
    "linux/netfilter_decnet.h"
    "linux/netfilter_netdev.h"
    "linux/dma-mapping.h"
    "linux/dma-direction.h"
    "linux/dma-attrs.h"
    "linux/dma-buf.h"
    "linux/scatterlist.h"
    "linux/kfifo.h"
    "linux/klist.h"
    "linux/kref.h"
    "linux/kset.h"
    "linux/ktype.h"
    "linux/kobj_type.h"
    "linux/kernfs.h"
    "linux/sysfs.h"
    "linux/configfs.h"
    "linux/debugfs.h"
    "linux/tracefs.h"
    "linux/pstore.h"
    "linux/console.h"
    "linux/printk.h"
    "linux/consolemap.h"
    "linux/vt.h"
    "linux/vt_kern.h"
    "linux/tty.h"
    "linux/tty_driver.h"
    "linux/tty_flip.h"
    "linux/tty_ldisc.h"
    "linux/serial.h"
    "linux/serial_core.h"
    "linux/serial_reg.h"
    "linux/input.h"
    "linux/input-event-codes.h"
    "linux/uinput.h"
    "linux/serio.h"
    "linux/serport.h"
    "linux/spi/spi.h"
    "linux/i2c.h"
    "linux/i2c-dev.h"
    "linux/platform_device.h"
    "linux/amba/bus.h"
    "linux/amba/serial.h"
    "linux/clk.h"
    "linux/clk-provider.h"
    "linux/clkdev.h"
    "linux/regulator/consumer.h"
    "linux/regulator/driver.h"
    "linux/regulator/machine.h"
    "linux/gpio.h"
    "linux/gpio/consumer.h"
    "linux/gpio/driver.h"
    "linux/pwm.h"
    "linux/leds.h"
    "linux/power_supply.h"
    "linux/thermal.h"
    "linux/hwmon.h"
    "linux/hwmon-sysfs.h"
    "linux/iio/iio.h"
    "linux/iio/buffer.h"
    "linux/iio/trigger.h"
    "linux/iio/trigger_consumer.h"
    "linux/iio/triggered_buffer.h"
    "linux/mfd/core.h"
    "linux/mfd/syscon.h"
    "linux/of.h"
    "linux/of_device.h"
    "linux/of_platform.h"
    "linux/of_gpio.h"
    "linux/of_irq.h"
    "linux/of_address.h"
    "linux/of_reserved_mem.h"
    "linux/firmware.h"
    "linux/efi.h"
    "linux/acpi.h"
    "linux/pci.h"
    "linux/pci_regs.h"
    "linux/usb.h"
    "linux/usb/ch9.h"
    "linux/usb/gadget.h"
    "linux/usb/otg.h"
    "linux/usb/role.h"
    "linux/usb/typec.h"
    "linux/usb/typec_mux.h"
    "linux/usb/typec_dp.h"
    "linux/usb/typec_tbt.h"
    "linux/phy/phy.h"
    "linux/phy/phy-qcom-ufs.h"
    "linux/mmc/host.h"
    "linux/mmc/card.h"
    "linux/mmc/mmc.h"
    "linux/mmc/sd.h"
    "linux/mmc/sdio.h"
    "linux/mmc/sdio_func.h"
    "linux/mmc/sdio_ids.h"
    "linux/rtc.h"
    "linux/watchdog.h"
    "linux/random.h"
    "linux/uuid.h"
    "linux/crc16.h"
    "linux/crc32.h"
    "linux/crc32c.h"
    "linux/crc7.h"
    "linux/crc8.h"
    "linux/crc-itu-t.h"
    "linux/crc-t10dif.h"
    "linux/libcrc32c.h"
    "linux/hash.h"
    "linux/jhash.h"
    "linux/siphash.h"
    "linux/xxhash.h"
    "linux/zlib.h"
    "linux/zstd.h"
    "linux/lz4.h"
    "linux/buffer_head.h"
    "linux/mpage.h"
    "linux/writeback.h"
    "linux/address_space.h"
    "linux/pagemap.h"
    "linux/swap.h"
    "linux/swapops.h"
    "linux/shmem_fs.h"
    "linux/file.h"
    "linux/fdtable.h"
    "linux/fcntl.h"
    "linux/io.h"
    "linux/ioport.h"
    "linux/resource.h"
    "linux/interrupt.h"
    "linux/irq.h"
    "linux/irqnr.h"
    "linux/irqflags.h"
    "linux/hardirq.h"
    "linux/softirq.h"
    "linux/preempt.h"
    "linux/rcupdate.h"
    "linux/srcu.h"
    "linux/notifier.h"
    "linux/reboot.h"
    "linux/kmod.h"
    "linux/kthread.h"
    "linux/completion.h"
    "linux/wait.h"
    "linux/poll.h"
    "linux/eventfd.h"
    "linux/signalfd.h"
    "linux/aio.h"
    "linux/uio.h"
    "linux/socket.h"
    "linux/sockios.h"
    "linux/net.h"
    "linux/in.h"
    "linux/in6.h"
    "linux/un.h"
    "linux/netdevice.h"
    "linux/skbuff.h"
    "linux/ip.h"
    "linux/ipv6.h"
    "linux/tcp.h"
    "linux/udp.h"
    "linux/icmp.h"
    "linux/icmpv6.h"
    "linux/route.h"
    "linux/netlink.h"
    "linux/rtnetlink.h"
    "linux/genetlink.h"
    "linux/sock_diag.h"
    "linux/inet_diag.h"
    "linux/inet.h"
    "linux/netfilter.h"
    "uapi/linux/major.h"
    "uapi/linux/input.h"
    "uapi/linux/input-event-codes.h"
    "uapi/linux/fb.h"
    "uapi/linux/videodev2.h"
    "uapi/linux/media.h"
    "uapi/linux/v4l2-common.h"
    "uapi/linux/v4l2-controls.h"
    "uapi/linux/v4l2-mediabus.h"
    "uapi/linux/v4l2-subdev.h"
    "uapi/linux/msm_mdp.h"
    "uapi/linux/msm_kgsl.h"
    "uapi/linux/msm_ion.h"
    "uapi/linux/msm_ion_ids.h"
    "uapi/linux/msm_hdcp.h"
    "uapi/linux/msm_drm.h"
    "uapi/linux/msm_dsps.h"
    "uapi/linux/msm_rmnet.h"
    "uapi/linux/rmnet_data.h"
    "uapi/linux/rmnet_ipa_fd_ioctl.h"
    "uapi/linux/msm_ipa.h"
    "uapi/linux/msm_bus.h"
    "uapi/linux/msm_bus_board.h"
    "uapi/linux/msm_audio.h"
    "uapi/linux/msm_audio_aac.h"
    "uapi/linux/msm_audio_amrnb.h"
    "uapi/linux/msm_audio_amrwb.h"
    "uapi/linux/msm_audio_ion.h"
    "uapi/linux/msm_audio_mvs.h"
    "uapi/linux/msm_audio_qcp.h"
    "uapi/linux/msm_audio_sbc.h"
    "uapi/linux/msm_audio_voicememo.h"
    "uapi/linux/msm_audio_wma.h"
    "uapi/linux/msm_audio_wmapro.h"
    "uapi/linux/msm_camera.h"
    "uapi/linux/msm_camsensor_sdk.h"
    "uapi/linux/msm_fd.h"
    "uapi/linux/msm_gestures.h"
    "uapi/linux/msm_isp.h"
    "uapi/linux/msm_jpeg.h"
    "uapi/linux/msm_jpeg_dma.h"
    "uapi/linux/msm_lcd.h"
    "uapi/linux/msm_mdp_ext.h"
    "uapi/linux/msm_rotator.h"
    "uapi/linux/msm_vidc.h"
    "uapi/linux/msm_vpu.h"
    "uapi/linux/videodev2_exynos_camera.h"
    "uapi/linux/smcinvoke.h"
    "uapi/linux/msm_thermal.h"
    "uapi/linux/msm_npu.h"
    "uapi/linux/msm_sensors.h"
    "uapi/linux/msm_dsps.h"
    "uapi/linux/msm_q6venc.h"
    "uapi/linux/msm_q6vdec.h"
    "uapi/linux/msm_thermal_ioctl.h"
    "uapi/linux/msm_audio_calibration.h"
    "uapi/linux/msm_audio_cal_utils.h"
    "uapi/linux/msm_audio_alac.h"
    "uapi/linux/msm_audio_ape.h"
    "uapi/linux/msm_audio_g711.h"
    "uapi/linux/msm_audio_g711_dec.h"
    "uapi/linux/msm_audio_amrwbplus.h"
    "uapi/linux/msm_audio_dolby_dap.h"
    "uapi/linux/msm_audio_dolby_sw.h"
    "uapi/linux/msm_audio_dtshd.h"
    "uapi/linux/msm_audio_wmapro10.h"
    "uapi/linux/msm_audio_alac.h"
    "uapi/linux/msm_audio_ape.h"
    "uapi/linux/msm_audio_g711.h"
    "uapi/linux/msm_audio_g711_dec.h"
    "uapi/linux/msm_audio_amrwbplus.h"
    "uapi/linux/msm_audio_dolby_dap.h"
    "uapi/linux/msm_audio_dolby_sw.h"
    "uapi/linux/msm_audio_dtshd.h"
    "uapi/linux/msm_audio_wmapro10.h"
    "asm-generic/ioctl.h"
    "asm-generic/uaccess.h"
    "asm-generic/cacheflush.h"
    "asm-generic/barrier.h"
    "asm-generic/bitops.h"
    "asm-generic/atomic.h"
    "asm-generic/atomic64.h"
    "asm-generic/cmpxchg.h"
    "asm-generic/cmpxchg-local.h"
    "asm-generic/spinlock.h"
    "asm-generic/rwsem.h"
    "asm-generic/mutex.h"
    "asm-generic/semaphore.h"
    "asm-generic/signal.h"
    "asm-generic/siginfo.h"
    "asm-generic/syscall.h"
    "asm-generic/unistd.h"
    "asm-generic/fcntl.h"
    "asm-generic/stat.h"
    "asm-generic/statfs.h"
    "asm-generic/poll.h"
    "asm-generic/socket.h"
    "asm-generic/sockios.h"
    "asm-generic/termios.h"
    "asm-generic/ioctls.h"
    "asm-generic/termbits.h"
    "asm-generic/swab.h"
    "asm-generic/page.h"
    "asm-generic/memory_model.h"
    "asm-generic/sections.h"
    "asm-generic/percpu.h"
    "asm-generic/topology.h"
    "asm-generic/dma-mapping.h"
    "asm-generic/dma-coherent.h"
    "asm-generic/vga.h"
    "asm-generic/kdebug.h"
    "asm-generic/kmap_types.h"
    "asm-generic/pgtable.h"
    "asm-generic/pgalloc.h"
    "asm-generic/tlb.h"
    "asm-generic/tlbflush.h"
    "asm-generic/mmu.h"
    "asm-generic/mmu_context.h"
    "asm-generic/hugetlb.h"
    "asm-generic/mm_hooks.h"
    "asm-generic/delay.h"
    "asm-generic/timex.h"
    "asm-generic/local.h"
    "asm-generic/local64.h"
    "asm-generic/hardirq.h"
    "asm-generic/softirq_stack.h"
    "asm-generic/preempt.h"
    "asm-generic/switch_to.h"
    "asm-generic/exec.h"
    "asm-generic/signal-defs.h"
    "asm-generic/sigcontext.h"
    "asm-generic/sigframe.h"
    "asm-generic/ucontext.h"
    "asm-generic/fpstate.h"
    "asm-generic/processor.h"
    "asm-generic/proc_fs.h"
    "asm-generic/bugs.h"
    "asm-generic/dma.h"
    "asm-generic/io.h"
    "asm-generic/iomap.h"
    "asm-generic/mmiowb.h"
    "asm-generic/pci_iomap.h"
    "asm-generic/serial.h"
    "asm-generic/rtc.h"
    "asm-generic/module.h"
    "asm-generic/sections.h"
    "asm-generic/emergency-restart.h"
    "asm-generic/kprobes.h"
    "asm-generic/ftrace.h"
    "asm-generic/uprobes.h"
    "asm-generic/perf_event.h"
    "asm-generic/irq_regs.h"
    "asm-generic/checksum.h"
    "asm-generic/extable.h"
)

# 检查头文件
check_headers() {
    log_header "检查关键头文件"
    write_report ""
    write_report "3. 头文件检查"
    write_report "----------------------------------------"

    if [ -z "$KDIR" ] || [ ! -d "$KDIR" ]; then
        log_error "内核目录未设置或不存在"
        write_report "[FAIL] 内核目录未设置"
        ERRORS=$((ERRORS + 1))
        return 1
    fi

    local include_dir="$KDIR/include"
    if [ ! -d "$include_dir" ]; then
        log_error "include目录不存在: $include_dir"
        write_report "[FAIL] include目录不存在"
        ERRORS=$((ERRORS + 1))
        return 1
    fi

    local found=0
    local missing=0

    for header in "${CRITICAL_HEADERS[@]}"; do
        local header_path="$include_dir/$header"
        if [ -f "$header_path" ]; then
            found=$((found + 1))
        else
            missing=$((missing + 1))
            if [ $missing -le 10 ]; then
                log_warn "头文件缺失: $header"
            fi
        fi
    done

    log_info "头文件统计: 找到 $found, 缺失 $missing"
    write_report "[INFO] 头文件统计: 找到 $found, 缺失 $missing"

    if [ $missing -eq 0 ]; then
        log_success "所有关键头文件都存在"
        write_report "[PASS] 所有关键头文件都存在"
    else
        log_warn "部分头文件缺失，但可能不影响基本模块编译"
        write_report "[WARN] 部分头文件缺失"
        WARNS=$((WARNS + 1))
    fi

    echo ""
}

# 执行make modules_prepare
run_modules_prepare() {
    log_header "执行 modules_prepare"
    write_report ""
    write_report "4. modules_prepare 执行"
    write_report "----------------------------------------"

    if [ -z "$KDIR" ] || [ ! -d "$KDIR" ]; then
        log_error "内核目录未设置或不存在"
        write_report "[FAIL] 内核目录未设置"
        ERRORS=$((ERRORS + 1))
        return 1
    fi

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
            make ARCH=arm64 $defconfig 2>&1 | tee -a "$REPORT_FILE"
        else
            log_warn "未找到defconfig，尝试使用现有配置"
            make ARCH=arm64 defconfig 2>&1 | tee -a "$REPORT_FILE" || true
        fi
    fi

    log_info "执行 make modules_prepare..."
    if make ARCH=arm64 LLVM=1 modules_prepare 2>&1 | tee -a "$REPORT_FILE"; then
        log_success "modules_prepare 执行成功"
        write_report "[PASS] modules_prepare 执行成功"
    else
        log_error "modules_prepare 执行失败"
        write_report "[FAIL] modules_prepare 执行失败"
        ERRORS=$((ERRORS + 1))
        return 1
    fi

    echo ""
}

# 检查生成的文件
check_generated_files() {
    log_header "检查生成的文件"
    write_report ""
    write_report "5. 生成文件检查"
    write_report "----------------------------------------"

    cd "$KDIR"

    # 检查Module.symvers
    if [ -f "Module.symvers" ]; then
        local sym_count=$(wc -l < Module.symvers)
        log_success "Module.symvers 存在 (包含 $sym_count 个符号)"
        write_report "[PASS] Module.symvers 存在 (包含 $sym_count 个符号)"
    else
        log_error "Module.symvers 不存在"
        write_report "[FAIL] Module.symvers 不存在"
        ERRORS=$((ERRORS + 1))
    fi

    # 检查模块版本信息
    if [ -f "include/config/kernel.release" ]; then
        local kernel_release=$(cat include/config/kernel.release)
        log_success "内核版本信息: $kernel_release"
        write_report "[PASS] 内核版本信息: $kernel_release"
    else
        log_warn "内核版本信息文件不存在"
        write_report "[WARN] 内核版本信息文件不存在"
        WARNS=$((WARNS + 1))
    fi

    # 检查自动生成的头文件
    local generated_headers=(
        "include/generated/autoconf.h"
        "include/generated/uapi/linux/version.h"
        "include/generated/compile.h"
    )

    for header in "${generated_headers[@]}"; do
        if [ -f "$header" ]; then
            log_success "生成文件存在: $header"
            write_report "[PASS] 生成文件存在: $header"
        else
            log_warn "生成文件缺失: $header"
            write_report "[WARN] 生成文件缺失: $header"
            WARNS=$((WARNS + 1))
        fi
    done

    # 检查scripts目录
    if [ -d "scripts/basic" ] && [ -d "scripts/mod" ]; then
        log_success "scripts目录结构完整"
        write_report "[PASS] scripts目录结构完整"
    else
        log_warn "scripts目录可能不完整"
        write_report "[WARN] scripts目录可能不完整"
        WARNS=$((WARNS + 1))
    fi

    echo ""
}

# 验证编译器
check_compiler() {
    log_header "验证编译器"
    write_report ""
    write_report "6. 编译器验证"
    write_report "----------------------------------------"

    # 检查clang版本
    if command -v clang &> /dev/null; then
        local clang_full=$(clang --version | head -n1)
        log_success "Clang: $clang_full"
        write_report "[PASS] Clang: $clang_full"
    else
        log_error "Clang未找到"
        write_report "[FAIL] Clang未找到"
        ERRORS=$((ERRORS + 1))
    fi

    # 检查其他LLVM工具
    local llvm_tools=("llvm-ar" "llvm-nm" "llvm-objcopy" "llvm-objdump" "llvm-readelf" "llvm-strip")
    for tool in "${llvm_tools[@]}"; do
        if command -v $tool &> /dev/null; then
            log_success "$tool 可用"
            write_report "[PASS] $tool 可用"
        else
            log_warn "$tool 未找到"
            write_report "[WARN] $tool 未找到"
            WARNS=$((WARNS + 1))
        fi
    done

    # 检查交叉编译工具
    if command -v ${CROSS_COMPILE}gcc &> /dev/null; then
        local gcc_version=$(${CROSS_COMPILE}gcc --version | head -n1)
        log_success "交叉编译器: $gcc_version"
        write_report "[PASS] 交叉编译器: $gcc_version"
    else
        log_warn "交叉编译器 ${CROSS_COMPILE}gcc 未找到"
        write_report "[WARN] 交叉编译器未找到"
        WARNS=$((WARNS + 1))
    fi

    echo ""
}

# 生成验证报告
finalize_report() {
    log_header "验证报告摘要"

    cat >> "$REPORT_FILE" << EOF

============================================
验证报告摘要
============================================
验证时间: $(date)
内核目录: ${KDIR:-未设置}
Clang路径: $(which clang 2>/dev/null || echo "未找到")

统计信息:
- 错误数: $ERRORS
- 警告数: $WARNS

结果: $([ $ERRORS -eq 0 ] && echo "通过" || echo "失败")
============================================

EOF

    if [ $ERRORS -eq 0 ]; then
        log_success "验证通过! 发现 $WARNS 个警告"
        log_info "详细报告: $REPORT_FILE"
        write_report "[PASS] 验证通过"
        return 0
    else
        log_error "验证失败! 发现 $ERRORS 个错误, $WARNS 个警告"
        log_info "详细报告: $REPORT_FILE"
        write_report "[FAIL] 验证失败"
        return 1
    fi
}

# 主函数
main() {
    log_header "OnePlus ACE5 (SM8650/pineapple) 头文件验证"

    init_report
    check_environment
    check_kernel_source
    check_headers
    run_modules_prepare
    check_generated_files
    check_compiler
    finalize_report

    exit $([ $ERRORS -eq 0 ] && echo 0 || echo 1)
}

# 执行主函数
main "$@"
