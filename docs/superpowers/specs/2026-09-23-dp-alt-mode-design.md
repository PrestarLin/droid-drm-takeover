# DP Alt Mode — 容器桌面经 Type-C DP 输出（安卓内屏不受影响）

状态：实现完成，待真机插线实测。
分支：`dev`。
日期：2026-09-23。

## 1. 架构

目标：不停安卓、不抢 master，让容器侧 KWin + Plasma 画面通过 OnePlus 15 的
Type-C DP Alt Mode 输出到外接显示器；内屏 SurfaceFlinger 照常运行。

与既有 `desk-takeover.sh`（停 composer HAL → kwinwrap 抢 master）的本质区别：

| | desk-takeover（内屏接管） | DP alt mode（外屏输出） |
|---|---|---|
| 安卓状态 | stop 整套图形栈 | **完全不动** |
| 权限路线 | 抢 DRM master + DROP_MASTER | **独占租约** DRM_MODE_LEASE_EXCL |
| kwin 拿 fd | kwinwrap 交接（setuid 桥） | **LD_PRELOAD shim** 拦 open |
| 网络 | 容器接管 WiFi | 不碰 |

三层结构：

1. **内核**（`kernel-patches/`，3 个补丁，仅改 `drivers/gpu/drm` + `include/`）：
   - 0001 铸租：privileged CREATE_LEASE + `DRM_MODE_LEASE_EXCL` 标志 + 租约生命周期
     hotplug uevent。
   - 0002 隐藏：SF（非 lessee）视角里被独占租约的 connector 恒 disconnected、
     resource 列表剔除、plane 不可见 —— SF 的 DP 热插拔状态机完全不受租约方干扰。
   - 0003 防踩：CRTC/atomic 提交路径拒绝改动外方独占租约对象（EBUSY）。
2. **容器侧 helper**（`src/dp-lease-helper.c`，静态）：root 铸租（DP connector +
   空闲 CRTC + planes），daemon 化持有 lessee fd，unix socket + SCM_RIGHTS 分发，
   `revoke` 时触发 hotplug 把 DP 口还给 SF。
3. **容器侧会话**（`scripts/desk-dp-takeover.sh`）：
   helper daemon → `dp-screenctl`（wake_lock/背光/电源键策略）→ 可选 `dp-touchpad`
   → `LD_PRELOAD=kwin-drm-shim.so kwin_wayland --socket=dpdesk` → plasmashell 等。

时序保证：先铸独占租（SF 重查 DP 从此不可见）→ kwin 运行中插线 → 驱动真 HPD →
SF 仍报 disconnected、lessee 见 connected → kwin atomic modeset → 外屏出画。
revoke 顺序相反（先还权再杀 kwin），保证 SF 立即恢复 DP 口。

## 2. 内核补丁（kernel-patches/）

- `0001-drm-privileged-exclusive-lease-minting.patch`
  - uapi：`DRM_MODE_LEASE_EXCL (1u << 30)`（`drm_mode.h`）。
  - `DRM_IOCTL_MODE_CREATE_LEASE/LIST_LESSEES/REVOKE_LEASE` 去掉 `DRM_MASTER`
    ioctl flag，改为函数内检查：current master **或** `CAP_SYS_ADMIN`
    （容器 root 满足；GET_LEASE 保持原 flag）。
  - `struct drm_lease` 增加 `exclusive`；EXCL 时 mint/revoke/lessee destroy 均触发
    sysfs hotplug uevent（在 idr_mutex 释放后触发，锁序安全）。
  - 新 helper `drm_excl_obj_hidden_from(dev, obj_id, file_priv)`（drm_lease.c，
    供 listing/state 路径复用；`drm_file` 前向声明进 drm_internal.h）。
- `0002-drm-hide-foreign-exclusively-leased-objects.patch`
  - `drm_mode_getresources` 三循环剔除外方独占对象；`getconnector` 对非 lessee
    早退 DISCONNECTED 并清零字段；`getplane_res` 同理；page_flip 检查 plane 归属。
  - 语义 = “没有 DP” 对 SF；lessee（`drm_is_current_master_locked` 对 lessee
    返回 true）不受影响。
- `0003-drm-reject-stomping-foreign-exclusive-leases.patch`
  - `setcrtc`/`atomic_commit` 对外方独占对象返回 EBUSY，防 master 侧误踩租约方
    已提交的显示状态。

验证：三个补丁在 cctv18 6.12.23 android16 树顺序 `git apply --check` + `apply`
全过，tree 复原干净；CI `kernel` job 在完整源码 zip 上 `patch -p1` 应用并编
`Image`。

## 3. 用户态组件

| 文件 | 产物 | 运行位置 | 作用 |
|---|---|---|---|
| `src/dp-lease-helper.c` | `bin-static/dp-lease-helper` (-static) | 容器 root | daemon/revoke/status；裸 DRM ioctl 铸租；socket 发 fd |
| `src/kwin-drm-shim.c` | `bin/kwin-drm-shim.so` | 容器（LD_PRELOAD 进 kwin） | 拦 open/open64/openat/openat64 `/dev/dri/card*` → dup 租约 fd |
| `src/dp-screenctl.c` | `bin-static/dp-screenctl` (-static) | 容器 root | SCREEN_OFF 策略：display（wake_lock+背光0+吞电源键+音量转发）/ lock（仅 wake_lock）/ off |
| `src/dp-touchpad.c` | `bin-static/dp-touchpad` (-static) | 容器 root | TOUCHPAD=on：EVIOCGRAB 触摸屏 → INPUT_PROP_DIRECT uinput 单点 MT 克隆 |
| `scripts/desk-dp-takeover.sh` | — | 容器 root | 会话入口（自脱钩 setsid） |
| `scripts/desk-dp-stop.sh` | — | 容器 root | revoke → 杀 DP 桌面栈 → 杀 helper → 开关归位；**禁止 fuser card0** |
| `scripts/dp-touchpad.sh` | — | — | TOUCHPAD 开关薄包装 |

约定：

- socket 路径：env `DP_LEASE_SOCK` → 目录候选 `/data/local/tmp`、`/run`、`/tmp`
  （helper 取首个 W_OK；shim 按同序 connect），文件 `drm-lease.sock`；
  辅助 `dp-lease.pid`（flock 单实例）、`dp-lease.state`（sock/lessee/conn/crtc/planes）。
- socket 协议（SOCK_STREAM 单字节）：`F` 发 lease fd（SCM_RIGHTS）；`R` 执行
  REVOKE_LEASE 后回 `K` 再退出；`revoke` 子命令 socket 失败时读 state 直接
  root ioctl 兜底。
- uinput 设备创建后自扫 `/sys/class/input/*/name` 读 `dev` 属性补
  mknod `/dev/input/eventN`（EEXIST 忽略）；`/dev/uinput` 缺失按
  `/sys/class/misc/uinput/dev` 自建。
- wake_lock 独立 token `dp_takeover`（与既有 `qoderdbg` 无关）；两档 SCREEN_OFF
  都必须持锁 —— system_suspend 会把 Type-C DP 链路断掉。
- shim 纯代理：无 lease 时回退真实 open 并打日志，进程不因此失败。

## 4. 测试策略

1. **编译**（本仓库 CI）：
   - aarch64 job：`make` 全量（含 3 个静态 DP 工具 + shim.so），artifact
     `tools-aarch64` = `bin/` + `bin-static/`。
   - kernel job：完整 cctv18 源码 zip + clang19 工具链，`patch -p1` 应用
     kernel-patches/ 后 `make O=out gki_defconfig Image`，artifact
     `kernel-image`（`Image`，LOCALVERSION `-droid-drm-dp`）。精简路径不启
     RUST/susfs/KSU。
2. **无插线静态自检**（真机，不改系统文件）：
   - `dp-lease-helper status` 显示 lessee/conn/crtc/planes；
   - SF 侧 `drm` 工具（主机环境）确认 DP connector 对 master 恒 disconnected；
   - `desk-dp-takeover.sh` 起 kwin 后 `wayland-info` 自检通过；
   - `desk-dp-stop.sh` 后 SF 重新看见 DP 口（getconnector 状态恢复）。
3. **插线端到端**（待做，设备需带 DP 显示器）：
   - 会话中插入 Type-C DP 线 → 外屏点亮、内屏 SF 无感；
   - `TOUCHPAD=on` 时触屏操作镜像到外屏；
   - `SCREEN_OFF=display` 时内屏黑且不休眠（`dumpsys power` 确认未 suspend）、
     电源键被吞、音量键仍可转发；
   - stop 会话 → DP 口立刻回到安卓侧热插拔管理。
4. **回归红线**：stop 路径绝不 `fuser -k /dev/dri/card0`、绝不 stop 安卓服务、
   绝不碰 WiFi —— 与 `desk-takeover.sh` 的域严格分离。
