# Variable
BUILD ?= debug
SAN   ?=

# Toolchain
AR       := ar
CXX      := g++
CXXFLAGS := -std=c++17 -Wall -Wextra -Wpedantic -I. -pthread
LDLIBS   := -lgtest_main -lgtest

ifeq ($(BUILD),debug)
  CXXFLAGS += -O0 -g
else ifeq ($(BUILD),release)
  CXXFLAGS += -O2 -DNDEBUG
else ifeq ($(BUILD),relwithdebinfo)
  CXXFLAGS += -O2 -g -fno-omit-frame-pointer
else
  $(error Invalid BUILD '$(BUILD)'. Valid parameters are: debug, release, relwithdebinfo)
endif

# Directory
TEST_SRC_DIR := tests
LIB_SRC_DIR  := netlib
ifeq ($(SAN),)
  OUT_DIR  := build/$(BUILD)
else ifeq ($(SAN),address)
  OUT_DIR  := build/$(BUILD)-address
  CXXFLAGS += -fsanitize=address,undefined -fno-omit-frame-pointer
else ifeq ($(SAN), thread)
  OUT_DIR  := build/$(BUILD)-thread
  CXXFLAGS += -fsanitize=thread
else
  $(error Invalid SAN '$(SAN)'. Valid parameters are: address, thread)
endif

OBJ_DIR      := $(OUT_DIR)/obj

# Sources files
TEST_SRCS := $(shell find $(TEST_SRC_DIR) -type f -name '*.cc')
LIB_SRCS  := $(shell find $(LIB_SRC_DIR) -type f -name '*.cc')

# Object comprehension
TEST_OBJS := $(patsubst %.cc,$(OBJ_DIR)/%.o,$(TEST_SRCS))
LIB_OBJS  := $(patsubst %.cc,$(OBJ_DIR)/%.o,$(LIB_SRCS))

# Outputs
TEST_BINS := $(patsubst %.cc,$(OUT_DIR)/%,$(TEST_SRCS))
LIB       := $(OUT_DIR)/lib/libnetlib.a
DEPS      := $(patsubst %.o,%.d,$(TEST_OBJS) $(LIB_OBJS))

# Phony target
.DEFAULT_GOAL := all
all: $(LIB) $(TEST_BINS)

test: $(TEST_BINS)
	@for e in $(TEST_BINS); do $$e || exit 1; done

# Library
$(LIB): $(LIB_OBJS)
	@mkdir -p $(@D)
	@rm -f $@
	$(AR) rcs $@ $^

# Link
$(TEST_BINS): $(OUT_DIR)/%: $(OBJ_DIR)/%.o $(LIB)
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $^ $(LDLIBS) -o $@

# Compile
$(OBJ_DIR)/%.o: %.cc
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) -c -MMD -MP $< -o $@

# Tail
-include $(DEPS)

# Utilities
# 只删除当前配置的输出目录（如 build/debug-address/），其他配置的产物保留
# 防御：OUT_DIR 必须形如 build/xxx，否则拒绝执行，避免变量意外为空时误删
clean:
	@case "$(OUT_DIR)" in build/?*) ;; *) echo "clean: refusing to remove '$(OUT_DIR)'" >&2; exit 1;; esac
	rm -rf $(OUT_DIR)

distclean:
	rm -rf build

.PHONY: all test clean distclean
