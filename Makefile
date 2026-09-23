CC      ?= gcc
CFLAGS  += -O2 -g -Wall -Wno-unused-parameter
PKG_CFLAGS := $(shell pkg-config --cflags libdrm wayland-client 2>/dev/null)
DRM_LIBS := $(shell pkg-config --libs libdrm)
WL_LIBS  := $(shell pkg-config --libs wayland-client)

SRC := src
BIN := bin
BLD := build
BST := bin-static

# 纯 DRM/ioctl 工具
DRM_TOOLS := kwinwrap drmatomic touchdraw setprop setbright connprops crtcstate \
             informats masterprobe planecrtc rawprobe kwinprobe atombisect \
             stageprobe replicate
UDEV_TOOLS := udevprobe udevmatch
# 需要 wayland-client + 随仓库分发的协议桩(免 wayland-scanner)
WAYLAND_TOOLS := touchtest touchinj
# DP alt mode: 容器侧 root 工具，静态链接（裸 ioctl，无外部依赖）
DP_STATIC_TOOLS := dp-lease-helper dp-screenctl dp-touchpad
# DP alt mode: LD_PRELOAD 拦 /dev/dri/card* → 换成租约 fd（glibc shared）
DP_SHIM := kwin-drm-shim.so

PROT_OBJS := $(BLD)/xdg-shell-protocol.o $(BLD)/fake-input-protocol.o

all: $(addprefix $(BIN)/,$(DRM_TOOLS)) $(addprefix $(BIN)/,$(UDEV_TOOLS)) \
     $(addprefix $(BIN)/,$(WAYLAND_TOOLS)) $(BIN)/atomicspy.so \
     $(BIN)/$(DP_SHIM) $(addprefix $(BST)/,$(DP_STATIC_TOOLS))

$(BIN) $(BLD) $(BST):
	mkdir -p $@

$(BLD)/%.o: $(SRC)/%.c | $(BLD)
	$(CC) $(CFLAGS) $(PKG_CFLAGS) -fPIC -c $< -o $@

$(BIN)/%: $(SRC)/%.c | $(BIN)
	$(CC) $(CFLAGS) $(PKG_CFLAGS) $< -o $@ $(DRM_LIBS)

$(BIN)/udevprobe $(BIN)/udevmatch: $(BIN)/%: $(SRC)/%.c | $(BIN)
	$(CC) $(CFLAGS) $< -o $@ -ludev

$(addprefix $(BIN)/,$(WAYLAND_TOOLS)): $(BIN)/%: $(SRC)/%.c $(PROT_OBJS) | $(BIN)
	$(CC) $(CFLAGS) $(PKG_CFLAGS) $< $(PROT_OBJS) -o $@ $(WL_LIBS)

$(BIN)/atomicspy.so: $(SRC)/atomic-spy.c | $(BIN)
	$(CC) $(CFLAGS) $(PKG_CFLAGS) -shared -fPIC $< -o $@ $(DRM_LIBS) -ldl

$(BIN)/$(DP_SHIM): $(SRC)/kwin-drm-shim.c | $(BIN)
	$(CC) $(CFLAGS) -shared -fPIC $< -o $@ -ldl

# 裸 DRM/input ioctl，-static；Android(bionic) 与容器(glibc) 双方皆可跑
$(addprefix $(BST)/,$(DP_STATIC_TOOLS)): $(BST)/%: $(SRC)/%.c | $(BST)
	$(CC) $(CFLAGS) -static $< -o $@

clean:
	rm -rf $(BLD)
	rm -f $(addprefix $(BIN)/,$(DRM_TOOLS) $(UDEV_TOOLS) $(WAYLAND_TOOLS)) $(BIN)/atomicspy.so
	rm -f $(BIN)/$(DP_SHIM) $(addprefix $(BST)/,$(DP_STATIC_TOOLS))

.PHONY: all clean
