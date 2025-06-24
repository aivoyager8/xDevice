# XDevice Build System

CC = gcc
CXX = g++
CFLAGS = -Wall -Wextra -O3 -std=c11 -pthread -fPIC
CXXFLAGS = -Wall -Wextra -O3 -std=c++17 -pthread -fPIC
LDFLAGS = -pthread -lrt -lm

# 目录结构
SRCDIR = src
INCDIR = include
BUILDDIR = build
BINDIR = bin
TESTDIR = tests

# 源文件
STORAGE_SOURCES = $(wildcard $(SRCDIR)/storage/*.c)
NETWORK_SOURCES = $(wildcard $(SRCDIR)/network/*.c)
DISTRIBUTED_SOURCES = $(wildcard $(SRCDIR)/distributed/*.c)
BLOCKDEV_SOURCES = $(wildcard $(SRCDIR)/blockdev/*.c)
COMMON_SOURCES = $(wildcard $(SRCDIR)/common/*.c)
MAIN_SOURCES = $(wildcard $(SRCDIR)/*.c)

ALL_SOURCES = $(STORAGE_SOURCES) $(NETWORK_SOURCES) $(DISTRIBUTED_SOURCES) \
              $(BLOCKDEV_SOURCES) $(COMMON_SOURCES)

# 对象文件
OBJECTS = $(patsubst $(SRCDIR)/%.c,$(BUILDDIR)/%.o,$(ALL_SOURCES))

# 可执行文件
XDEVICE_NODE = $(BINDIR)/xdevice-node
XDEVICE_CLI = $(BINDIR)/xdevice-cli
XDEVICE_TEST = $(BINDIR)/xdevice-test

# 库文件
LIBXDEVICE = $(BINDIR)/libxdevice.so

# 默认目标
all: dirs $(LIBXDEVICE) $(XDEVICE_NODE) $(XDEVICE_CLI)

# 创建目录
dirs:
	@mkdir -p $(BUILDDIR)/storage
	@mkdir -p $(BUILDDIR)/network
	@mkdir -p $(BUILDDIR)/distributed
	@mkdir -p $(BUILDDIR)/blockdev
	@mkdir -p $(BUILDDIR)/common
	@mkdir -p $(BINDIR)

# 编译对象文件
$(BUILDDIR)/%.o: $(SRCDIR)/%.c
	$(CC) $(CFLAGS) -I$(INCDIR) -c $< -o $@

# 编译库文件
$(LIBXDEVICE): $(OBJECTS)
	$(CC) $(LDFLAGS) -shared -o $@ $^

# 编译主程序
$(XDEVICE_NODE): $(SRCDIR)/main/node_main.c $(LIBXDEVICE)
	$(CC) $(CFLAGS) -I$(INCDIR) -L$(BINDIR) $< -lxdevice -o $@

$(XDEVICE_CLI): $(SRCDIR)/main/cli_main.c $(LIBXDEVICE)
	$(CC) $(CFLAGS) -I$(INCDIR) -L$(BINDIR) $< -lxdevice -o $@

# 测试
test: $(XDEVICE_TEST)
	./$(XDEVICE_TEST)

$(XDEVICE_TEST): $(wildcard $(TESTDIR)/*.c) $(LIBXDEVICE)
	$(CC) $(CFLAGS) -I$(INCDIR) -I$(TESTDIR) -L$(BINDIR) $^ -lxdevice -o $@

# 安装依赖
deps:
	@echo "Installing dependencies..."
	# Ubuntu/Debian
	@if command -v apt-get >/dev/null 2>&1; then \
		sudo apt-get update && \
		sudo apt-get install -y build-essential libevent-dev libssl-dev \
		zlib1g-dev liblz4-dev libzstd-dev uuid-dev; \
	fi
	# CentOS/RHEL
	@if command -v yum >/dev/null 2>&1; then \
		sudo yum groupinstall -y "Development Tools" && \
		sudo yum install -y libevent-devel openssl-devel \
		zlib-devel lz4-devel libzstd-devel libuuid-devel; \
	fi

# 清理
clean:
	rm -rf $(BUILDDIR)
	rm -rf $(BINDIR)

# 安装
install: all
	sudo mkdir -p /usr/local/bin
	sudo mkdir -p /usr/local/lib
	sudo mkdir -p /usr/local/include/xdevice
	sudo cp $(BINDIR)/xdevice-* /usr/local/bin/
	sudo cp $(LIBXDEVICE) /usr/local/lib/
	sudo cp -r $(INCDIR)/* /usr/local/include/xdevice/
	sudo ldconfig

# 卸载
uninstall:
	sudo rm -f /usr/local/bin/xdevice-*
	sudo rm -f /usr/local/lib/libxdevice.so
	sudo rm -rf /usr/local/include/xdevice

.PHONY: all dirs test clean install uninstall deps
