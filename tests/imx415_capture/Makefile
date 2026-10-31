CC := aarch64-buildroot-linux-gnu-gcc
CFLAGS := -g -O0 -Wall -Iinc
LDLIBS := -lpthread

TARGET := v4l2_capture

SRC_DIR := src
OBJ_DIR := obj
DEP_DIR := dep
INC_DIR := inc

MAIN_SRC := main.c
SRCS := $(wildcard $(SRC_DIR)/*.c)
ALL_SRCS := $(MAIN_SRC) $(SRCS)

OBJS := $(patsubst %.c,$(OBJ_DIR)/%.o,$(notdir $(ALL_SRCS)))
DEPS := $(patsubst %.o,$(DEP_DIR)/%.d,$(notdir $(OBJS)))

.PHONY: all dirs run clean 

all: dirs $(TARGET)

dirs:
	mkdir -p $(OBJ_DIR) $(DEP_DIR)

$(TARGET): $(OBJS)
	$(CC) $^ -o $@ $(LDLIBS)

$(OBJ_DIR)/%.o: %.c
	$(CC) $(CFLAGS) -MMD -MP -MF $(DEP_DIR)/$*.d -c $< -o $@

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c
	$(CC) $(CFLAGS) -MMD -MP -MF $(DEP_DIR)/$*.d -c $< -o $@

-include $(DEPS)

run: $(TARGET)
	./$(TARGET)

clean:
	rm -rf $(TARGET) $(OBJ_DIR) $(DEP_DIR)