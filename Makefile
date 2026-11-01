CC := /root/rk3568/rk3568_linux_sdk/buildroot/output/rockchip_atk_dlrk3568/host/bin/aarch64-buildroot-linux-gnu-gcc
SYSROOT := /root/rk3568/rk3568_linux_sdk/buildroot/output/rockchip_atk_dlrk3568/host/aarch64-buildroot-linux-gnu/sysroot
CFLAGS := -g -O0 -Wall -Iinc -Iutils -I$(SYSROOT)/usr/include -I$(SYSROOT)/usr/include/rockchip -I$(SYSROOT)/usr/include/rga
LDLIBS := -lpthread -lrockchip_mpp -fsanitize=address -L$(SYSROOT)/usr/lib -lrga -lavformat -lavcodec -lavutil

TARGET := rkCam

SRC_DIR := src
UTILS_DIR := utils
OBJ_DIR := obj
DEP_DIR := dep

SRCS := $(wildcard $(SRC_DIR)/*.c) $(wildcard $(UTILS_DIR)/*.c)
OBJS := $(patsubst %.c,$(OBJ_DIR)/%.o,$(notdir $(SRCS)))
DEPS := $(patsubst %.o,$(DEP_DIR)/%.d,$(notdir $(OBJS)))

.PHONY: all dirs run clean

all: dirs $(TARGET)

dirs:
	mkdir -p $(OBJ_DIR) $(DEP_DIR)

$(TARGET): $(OBJS)
	$(CC) $^ -o $@ $(LDLIBS)

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c
	$(CC) $(CFLAGS) -MMD -MP -MF $(DEP_DIR)/$*.d -c $< -o $@

$(OBJ_DIR)/%.o: $(UTILS_DIR)/%.c
	$(CC) $(CFLAGS) -MMD -MP -MF $(DEP_DIR)/$*.d -c $< -o $@

-include $(DEPS)

run: $(TARGET)
	./$(TARGET) -r /root/imx415.h264

clean:
	rm -rf $(TARGET) $(OBJ_DIR) $(DEP_DIR)