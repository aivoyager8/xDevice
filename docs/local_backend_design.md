# 本地文件存储后端实现

## 1. 本地文件后端设计概述

### 1.1 支持的存储类型

本地文件后端支持多种存储类型，以适应不同的性能和可靠性需求：

1. **普通文件 (Regular Files)**：标准文件系统文件，支持缓存
2. **内存映射文件 (Memory-Mapped Files)**：高性能随机访问
3. **直接I/O文件 (Direct I/O)**：绕过页面缓存，降低延迟
4. **内存文件系统 (TMPFS)**：基于内存的高速存储
5. **块设备 (Block Device)**：直接访问块设备
6. **稀疏文件 (Sparse Files)**：节省磁盘空间的大文件
7. **压缩文件 (Compressed Files)**：启用透明压缩

### 1.2 本地文件接口实现

```c
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <linux/fs.h>
#ifdef HAVE_LIBAIO
#include <libaio.h>
#endif

// 本地文件存储类型
typedef enum {
    LOCAL_STORAGE_REGULAR = 1,     // 普通文件
    LOCAL_STORAGE_MMAP = 2,        // 内存映射文件  
    LOCAL_STORAGE_DIRECT = 3,      // 直接I/O
    LOCAL_STORAGE_TMPFS = 4,       // 内存文件系统
    LOCAL_STORAGE_BLOCK = 5,       // 块设备
    LOCAL_STORAGE_SPARSE = 6,      // 稀疏文件
    LOCAL_STORAGE_COMPRESSED = 7,  // 压缩文件
    LOCAL_STORAGE_HYBRID = 8       // 混合模式(热数据内存,冷数据磁盘)
} local_storage_type_t;

// 本地文件私有数据结构
typedef struct {
    // 基本配置
    char base_directory[256];      // 基础目录
    local_storage_type_t type;     // 存储类型
    mode_t file_permissions;       // 文件权限
    mode_t dir_permissions;        // 目录权限
    
    // I/O配置
    struct {
        bool use_direct_io;        // 使用直接I/O
        bool use_sync_writes;      // 同步写入
        bool use_mmap;             // 使用内存映射
        size_t mmap_size;          // 映射大小
        int open_flags;            // 打开标志
        size_t io_alignment;       // I/O对齐
    } io_config;
    
    // 缓存配置
    struct {
        bool enable_page_cache;    // 页面缓存
        bool enable_read_cache;    // 读缓存
        bool enable_write_cache;   // 写缓存
        size_t cache_size;         // 缓存大小
        uint32_t cache_ttl;        // 缓存TTL
        struct local_cache *cache_instance; // 缓存实例
    } cache_config;
    
    // 异步I/O
    struct {
        bool use_aio;              // 使用异步I/O
        io_context_t aio_ctx;      // AIO上下文
        struct iocb *iocb_pool;    // IOCB池
        uint32_t max_events;       // 最大事件数
        pthread_t aio_thread;      // AIO处理线程
        bool aio_thread_running;   // AIO线程状态
    } aio_config;
    
    // 文件管理
    struct {
        pthread_mutex_t fd_cache_lock; // 文件描述符缓存锁
        struct fd_cache_entry *fd_cache; // 文件描述符缓存
        uint32_t max_open_files;   // 最大打开文件数
        bool auto_create_dirs;     // 自动创建目录
        bool preallocate_files;    // 预分配文件
    } file_mgmt;
    
    // 预分配管理
    struct {
        size_t default_file_size;  // 默认文件大小
        size_t extent_size;        // 扩展大小
        bool use_fallocate;        // 使用fallocate
        bool use_posix_fallocate;  // 使用posix_fallocate
    } prealloc_config;
    
    // 压缩支持
    struct {
        bool enable_compression;   // 启用压缩
        int compression_level;     // 压缩级别
        enum {
            COMPRESS_NONE,
            COMPRESS_LZ4,
            COMPRESS_ZSTD,
            COMPRESS_SNAPPY
        } compression_type;
    } compression;
    
    // 统计信息
    storage_stats_t stats;         // 性能统计
    pthread_mutex_t stats_lock;    // 统计锁
    
} local_private_data_t;

// 文件描述符缓存条目
typedef struct fd_cache_entry {
    char path[256];                // 文件路径
    int fd;                        // 文件描述符
    uint64_t last_access;          // 最后访问时间
    bool is_mmaped;                // 是否已映射
    void *mmap_addr;               // 映射地址
    size_t mmap_size;              // 映射大小
    struct fd_cache_entry *next;   // 下一个条目
} fd_cache_entry_t;

// 本地缓存条目
typedef struct {
    char key[256];                 // 缓存键(文件路径+偏移)
    void *data;                    // 缓存数据
    size_t size;                   // 数据大小
    uint64_t timestamp;            // 时间戳
    uint64_t access_count;         // 访问次数
} local_cache_entry_t;

// 本地文件后端初始化
static int local_init(storage_interface_t *iface, const char *config) {
    local_private_data_t *priv = calloc(1, sizeof(local_private_data_t));
    if (!priv) return -ENOMEM;
    
    // 解析配置
    if (parse_local_config(config, priv) != 0) {
        free(priv);
        return -EINVAL;
    }
    
    // 创建基础目录
    if (create_directory_recursive(priv->base_directory, 
                                  priv->dir_permissions) != 0) {
        free(priv);
        return -EACCES;
    }
    
    // 初始化异步I/O
    if (priv->aio_config.use_aio) {
        if (io_setup(priv->aio_config.max_events, &priv->aio_config.aio_ctx) != 0) {
            free(priv);
            return -EIO;
        }
        
        // 分配IOCB池
        priv->aio_config.iocb_pool = calloc(priv->aio_config.max_events, 
                                           sizeof(struct iocb));
        if (!priv->aio_config.iocb_pool) {
            io_destroy(priv->aio_config.aio_ctx);
            free(priv);
            return -ENOMEM;
        }
        
        // 启动AIO处理线程
        if (pthread_create(&priv->aio_config.aio_thread, NULL, 
                          aio_worker_thread, priv) != 0) {
            free(priv->aio_config.iocb_pool);
            io_destroy(priv->aio_config.aio_ctx);
            free(priv);
            return -EAGAIN;
        }
        priv->aio_config.aio_thread_running = true;
    }
    
    // 初始化缓存
    if (priv->cache_config.enable_read_cache || 
        priv->cache_config.enable_write_cache) {
        priv->cache_config.cache_instance = 
            local_cache_create(priv->cache_config.cache_size);
    }
    
    // 初始化文件描述符缓存
    pthread_mutex_init(&priv->file_mgmt.fd_cache_lock, NULL);
    
    pthread_mutex_init(&priv->stats_lock, NULL);
    
    iface->private_data = priv;
    return 0;
}

// 本地文件同步读取
static int local_read_sync(storage_interface_t *iface,
                          const char *path, uint64_t offset,
                          void *buffer, size_t size) {
    local_private_data_t *priv = iface->private_data;
    
    uint64_t start_time = get_monotonic_time_ns();
    
    // 检查读缓存
    if (priv->cache_config.enable_read_cache) {
        char cache_key[512];
        snprintf(cache_key, sizeof(cache_key), "%s:%lu", path, offset);
        
        if (local_cache_get(priv->cache_config.cache_instance, 
                           cache_key, buffer, size) == 0) {
            // 缓存命中
            pthread_mutex_lock(&priv->stats_lock);
            priv->stats.read_ops++;
            priv->stats.read_bytes += size;
            pthread_mutex_unlock(&priv->stats_lock);
            return 0;
        }
    }
    
    // 构造完整路径
    char full_path[512];
    build_full_path(priv, path, full_path, sizeof(full_path));
    
    // 获取文件描述符
    int fd = get_cached_fd(priv, full_path, O_RDONLY);
    if (fd < 0) return fd;
    
    ssize_t bytes_read;
    
    if (priv->io_config.use_mmap) {
        // 使用内存映射读取
        bytes_read = mmap_read(priv, fd, offset, buffer, size);
    } else {
        // 使用标准读取
        if (priv->io_config.use_direct_io) {
            // 直接I/O需要对齐
            bytes_read = aligned_pread(fd, buffer, size, offset, 
                                     priv->io_config.io_alignment);
        } else {
            bytes_read = pread(fd, buffer, size, offset);
        }
    }
    
    uint64_t end_time = get_monotonic_time_ns();
    
    if (bytes_read >= 0) {
        // 更新读缓存
        if (priv->cache_config.enable_read_cache && bytes_read > 0) {
            char cache_key[512];
            snprintf(cache_key, sizeof(cache_key), "%s:%lu", path, offset);
            local_cache_put(priv->cache_config.cache_instance, 
                           cache_key, buffer, bytes_read);
        }
        
        // 更新统计信息
        pthread_mutex_lock(&priv->stats_lock);
        priv->stats.read_ops++;
        priv->stats.read_bytes += bytes_read;
        update_latency_stats(&priv->stats, end_time - start_time, true);
        pthread_mutex_unlock(&priv->stats_lock);
        
        return (bytes_read == size) ? 0 : -EIO;
    }
    
    return -errno;
}

// 本地文件同步写入
static int local_write_sync(storage_interface_t *iface,
                           const char *path, uint64_t offset,
                           const void *data, size_t size) {
    local_private_data_t *priv = iface->private_data;
    
    uint64_t start_time = get_monotonic_time_ns();
    
    // 构造完整路径
    char full_path[512];
    build_full_path(priv, path, full_path, sizeof(full_path));
    
    // 确保目录存在
    if (priv->file_mgmt.auto_create_dirs) {
        char dir_path[512];
        get_directory_path(full_path, dir_path, sizeof(dir_path));
        create_directory_recursive(dir_path, priv->dir_permissions);
    }
    
    // 获取文件描述符
    int flags = O_WRONLY | O_CREAT;
    if (priv->io_config.use_direct_io) flags |= O_DIRECT;
    if (priv->io_config.use_sync_writes) flags |= O_SYNC;
    
    int fd = get_cached_fd(priv, full_path, flags);
    if (fd < 0) return fd;
    
    // 预分配文件空间
    if (priv->prealloc_config.use_fallocate) {
        off_t file_size = lseek(fd, 0, SEEK_END);
        if (offset + size > file_size) {
            size_t new_size = max(offset + size, 
                                priv->prealloc_config.default_file_size);
            fallocate(fd, 0, file_size, new_size - file_size);
        }
    }
    
    ssize_t bytes_written;
    
    // 数据压缩(如果启用)
    void *write_data = (void*)data;
    size_t write_size = size;
    void *compressed_data = NULL;
    
    if (priv->compression.enable_compression && 
        size > COMPRESSION_THRESHOLD) {
        compressed_data = compress_data(priv, data, size, &write_size);
        if (compressed_data) {
            write_data = compressed_data;
        }
    }
    
    if (priv->io_config.use_mmap) {
        // 使用内存映射写入
        bytes_written = mmap_write(priv, fd, offset, write_data, write_size);
    } else {
        // 使用标准写入
        if (priv->io_config.use_direct_io) {
            // 直接I/O需要对齐
            bytes_written = aligned_pwrite(fd, write_data, write_size, offset,
                                         priv->io_config.io_alignment);
        } else {
            bytes_written = pwrite(fd, write_data, write_size, offset);
        }
    }
    
    // 强制刷新(如果需要)
    if (priv->io_config.use_sync_writes && bytes_written > 0) {
        fsync(fd);
    }
    
    // 清理压缩数据
    if (compressed_data) {
        free(compressed_data);
    }
    
    uint64_t end_time = get_monotonic_time_ns();
    
    if (bytes_written >= 0) {
        // 更新写缓存
        if (priv->cache_config.enable_write_cache) {
            char cache_key[512];
            snprintf(cache_key, sizeof(cache_key), "%s:%lu", path, offset);
            local_cache_put(priv->cache_config.cache_instance, 
                           cache_key, write_data, bytes_written);
        }
        
        // 更新统计信息
        pthread_mutex_lock(&priv->stats_lock);
        priv->stats.write_ops++;
        priv->stats.write_bytes += bytes_written;
        update_latency_stats(&priv->stats, end_time - start_time, false);
        pthread_mutex_unlock(&priv->stats_lock);
        
        return (bytes_written == write_size) ? 0 : -EIO;
    }
    
    return -errno;
}

// 本地文件异步读取
static int local_read_async(storage_interface_t *iface,
                           const char *path, uint64_t offset,
                           void *buffer, size_t size,
                           storage_operation_ctx_t *ctx) {
    local_private_data_t *priv = iface->private_data;
    
    if (!priv->aio_config.use_aio) {
        // 如果不支持异步I/O，回退到同步操作
        int ret = local_read_sync(iface, path, offset, buffer, size);
        if (ctx->callback) {
            ctx->callback(ctx->user_context, 
                         (ret == 0) ? STORAGE_OP_SUCCESS : STORAGE_OP_ERROR,
                         buffer, (ret == 0) ? size : 0);
        }
        return ret;
    }
    
    // 构造完整路径
    char full_path[512];
    build_full_path(priv, path, full_path, sizeof(full_path));
    
    // 获取文件描述符
    int fd = get_cached_fd(priv, full_path, O_RDONLY);
    if (fd < 0) return fd;
    
    // 分配异步操作结构
    struct local_async_op *async_op = malloc(sizeof(struct local_async_op));
    if (!async_op) return -ENOMEM;
    
    async_op->ctx = *ctx;
    async_op->buffer = buffer;
    async_op->size = size;
    async_op->start_time = get_monotonic_time_ns();
    
    // 准备IOCB
    struct iocb *iocb = &async_op->iocb;
    memset(iocb, 0, sizeof(*iocb));
    
    iocb->aio_fildes = fd;
    iocb->aio_lio_opcode = IO_CMD_PREAD;
    iocb->aio_buf = (uint64_t)buffer;
    iocb->aio_nbytes = size;
    iocb->aio_offset = offset;
    iocb->aio_data = (uint64_t)async_op;
    
    // 提交异步操作
    struct iocb *iocbs[] = { iocb };
    int ret = io_submit(priv->aio_config.aio_ctx, 1, iocbs);
    
    if (ret != 1) {
        free(async_op);
        return -EIO;
    }
    
    return 0;
}

// 内存映射读取
static ssize_t mmap_read(local_private_data_t *priv, int fd, 
                        uint64_t offset, void *buffer, size_t size) {
    // 获取文件大小
    struct stat st;
    if (fstat(fd, &st) != 0) return -errno;
    
    if (offset >= st.st_size) return 0;
    
    size_t read_size = min(size, st.st_size - offset);
    size_t map_size = ((read_size + PAGE_SIZE - 1) / PAGE_SIZE) * PAGE_SIZE;
    
    // 映射文件区域
    void *mapped = mmap(NULL, map_size, PROT_READ, MAP_SHARED, fd, 
                       (offset / PAGE_SIZE) * PAGE_SIZE);
    if (mapped == MAP_FAILED) return -errno;
    
    // 拷贝数据
    size_t page_offset = offset % PAGE_SIZE;
    memcpy(buffer, (char*)mapped + page_offset, read_size);
    
    // 解除映射
    munmap(mapped, map_size);
    
    return read_size;
}

// 内存映射写入
static ssize_t mmap_write(local_private_data_t *priv, int fd,
                         uint64_t offset, const void *data, size_t size) {
    // 确保文件大小足够
    struct stat st;
    if (fstat(fd, &st) != 0) return -errno;
    
    if (offset + size > st.st_size) {
        if (ftruncate(fd, offset + size) != 0) return -errno;
    }
    
    size_t map_size = ((size + PAGE_SIZE - 1) / PAGE_SIZE) * PAGE_SIZE;
    
    // 映射文件区域
    void *mapped = mmap(NULL, map_size, PROT_WRITE, MAP_SHARED, fd,
                       (offset / PAGE_SIZE) * PAGE_SIZE);
    if (mapped == MAP_FAILED) return -errno;
    
    // 写入数据
    size_t page_offset = offset % PAGE_SIZE;
    memcpy((char*)mapped + page_offset, data, size);
    
    // 强制刷新到磁盘
    if (msync(mapped, map_size, MS_SYNC) != 0) {
        munmap(mapped, map_size);
        return -errno;
    }
    
    // 解除映射
    munmap(mapped, map_size);
    
    return size;
}

// 注册本地文件后端
storage_interface_t* create_local_backend(void) {
    storage_interface_t *iface = calloc(1, sizeof(storage_interface_t));
    if (!iface) return NULL;
    
    iface->type = STORAGE_BACKEND_LOCAL;
    strncpy(iface->name, "Local File", sizeof(iface->name) - 1);
    
    // 设置函数指针
    iface->init = local_init;
    iface->cleanup = local_cleanup;
    iface->health_check = local_health_check;
    iface->read_sync = local_read_sync;
    iface->write_sync = local_write_sync;
    iface->read_async = local_read_async;
    iface->write_async = local_write_async;
    iface->create_file = local_create_file;
    iface->delete_file = local_delete_file;
    iface->get_file_info = local_get_file_info;
    iface->batch_operations = local_batch_operations;
    iface->get_stats = local_get_stats;
    iface->reset_stats = local_reset_stats;
    
    return iface;
}
```

## 2. 本地文件配置和优化

### 2.1 配置文件格式

```ini
[local]
# 基本配置
base_directory = /var/lib/xdevice/data
storage_type = regular     # regular/mmap/direct/tmpfs/block/sparse/compressed/hybrid
file_permissions = 0644
dir_permissions = 0755

# I/O配置
use_direct_io = false      # 直接I/O(绕过页缓存)
use_sync_writes = false    # 同步写入
use_mmap = false          # 内存映射
mmap_size = 67108864      # 64MB映射大小
io_alignment = 4096       # 4KB对齐
read_ahead_kb = 128       # 预读大小

# 缓存配置
enable_page_cache = true   # 系统页缓存
enable_read_cache = true   # 应用读缓存
enable_write_cache = false # 应用写缓存
cache_size = 134217728    # 128MB缓存
cache_ttl = 300           # 5分钟TTL

# 异步I/O配置
use_aio = true            # 异步I/O
max_events = 1024         # 最大事件数

# 文件管理
max_open_files = 1000     # 最大打开文件数
auto_create_dirs = true   # 自动创建目录
preallocate_files = true  # 预分配文件

# 预分配配置
default_file_size = 1073741824  # 1GB默认大小
extent_size = 67108864    # 64MB扩展大小
use_fallocate = true      # 使用fallocate

# 稀疏文件配置 (当storage_type=sparse时)
enable_sparse = true      # 启用稀疏文件
hole_detection_size = 4096 # 空洞检测大小
zero_block_threshold = 0.95 # 零块阈值(95%为零视为空洞)

# 压缩配置 (当storage_type=compressed时)
enable_compression = true  # 启用压缩
compression_type = lz4    # lz4/zstd/snappy/gzip/brotli
compression_level = 1     # 压缩级别
compression_threshold = 4096 # 压缩阈值(字节)
adaptive_compression = true # 自适应压缩

# 混合存储配置 (当storage_type=hybrid时)
[hybrid]
# 热数据配置
hot_storage_type = tmpfs  # 热数据存储类型
hot_directory = /dev/shm/xdevice-hot
hot_cache_size = 268435456 # 256MB热数据缓存
hot_access_threshold = 10  # 访问10次以上认为是热数据

# 冷数据配置
cold_storage_type = compressed # 冷数据存储类型
cold_directory = /var/lib/xdevice/cold
cold_timeout_sec = 1800   # 30分钟未访问降为冷数据
cold_enable_compression = true # 冷数据启用压缩

# 迁移配置
auto_migration = true     # 自动迁移
migration_batch_size = 100 # 每次迁移100个文件
migration_interval_sec = 60 # 1分钟检查一次迁移

# TMPFS配置 (当storage_type=tmpfs时)
[tmpfs]
tmpfs_size = 1073741824   # 1GB TMPFS大小
tmpfs_mount_options = "size=1G,mode=0755,noatime"
auto_mount = true         # 自动挂载TMPFS
backup_to_disk = true     # 备份到磁盘
backup_interval_sec = 300 # 5分钟备份一次

# 块设备配置 (当storage_type=block时)
[block]
block_device = /dev/nvme0n1p1 # 块设备路径
sector_size = 512         # 扇区大小
use_raw_device = false    # 使用原始设备
enable_trim = true        # 启用TRIM
queue_depth = 32          # 队列深度

# 性能调优配置
[tuning]
enable_auto_tuning = true # 自动性能调优
tuning_interval_sec = 300 # 5分钟调优一次
target_latency_us = 1000  # 目标延迟1ms
target_iops = 10000       # 目标IOPS 10K
target_cache_hit_ratio = 0.85 # 目标缓存命中率85%

# 错误恢复配置
[recovery]
enable_checkpoints = true # 启用检查点
checkpoint_interval_sec = 3600 # 1小时检查点
checkpoint_directory = /var/lib/xdevice/checkpoints
max_retries = 3           # 最大重试3次
retry_delay_ms = 100      # 重试延迟100ms
exponential_backoff = true # 指数退避
enable_checksums = true   # 启用校验和
enable_redundancy = false # 启用冗余存储
redundancy_factor = 2     # 冗余因子

# 监控配置
[monitoring]
enable_metrics = true     # 启用指标收集
metrics_interval_sec = 60 # 1分钟采集一次
log_slow_operations = true # 记录慢操作
slow_operation_threshold_ms = 100 # 慢操作阈值100ms
enable_health_check = true # 启用健康检查
health_check_interval_sec = 30 # 30秒健康检查
```

### 2.2 多存储类型配置示例

#### 高性能内存存储配置
```ini
[local]
storage_type = hybrid
base_directory = /var/lib/xdevice

[hybrid]
hot_storage_type = tmpfs
hot_directory = /dev/shm/xdevice-hot
hot_cache_size = 536870912  # 512MB
hot_access_threshold = 5

cold_storage_type = mmap
cold_directory = /var/lib/xdevice/cold
cold_timeout_sec = 300      # 5分钟

[tmpfs]
tmpfs_size = 1073741824     # 1GB
auto_mount = true
backup_to_disk = true
backup_interval_sec = 60
```

#### 高容量压缩存储配置
```ini
[local]
storage_type = compressed
base_directory = /data/xdevice
enable_compression = true
compression_type = zstd
compression_level = 3
adaptive_compression = true
use_direct_io = true

[recovery]
enable_checksums = true
enable_redundancy = true
redundancy_factor = 2
```

#### 低延迟块设备配置
```ini
[local]
storage_type = block
use_direct_io = true
use_aio = true
max_events = 2048

[block]
block_device = /dev/nvme0n1
sector_size = 4096
queue_depth = 64
enable_trim = true

[tuning]
target_latency_us = 100     # 目标延迟100微秒
target_iops = 100000        # 目标IOPS 100K
```

### 2.2 特殊存储类型支持

```c
// TMPFS支持(内存文件系统)
static int setup_tmpfs_storage(local_private_data_t *priv) {
    // 检查是否已挂载tmpfs
    if (!is_tmpfs_mounted(priv->base_directory)) {
        // 创建临时挂载点
        mkdir(priv->base_directory, priv->dir_permissions);
        
        // 挂载tmpfs
        char mount_options[256];
        snprintf(mount_options, sizeof(mount_options),
                "size=%zu,mode=0755", priv->cache_config.cache_size);
        
        if (mount("tmpfs", priv->base_directory, "tmpfs", 
                 MS_NOATIME | MS_NODIRATIME, mount_options) != 0) {
            return -errno;
        }
    }
    
    return 0;
}

// 块设备支持
static int setup_block_device_storage(local_private_data_t *priv) {
    // 检查是否为块设备
    struct stat st;
    if (stat(priv->base_directory, &st) != 0) return -errno;
    
    if (!S_ISBLK(st.st_mode)) {
        return -ENOTBLK;
    }
    
    // 获取块设备信息
    int fd = open(priv->base_directory, O_RDONLY);
    if (fd < 0) return -errno;
    
    uint64_t size;
    if (ioctl(fd, BLKGETSIZE64, &size) != 0) {
        close(fd);
        return -errno;
    }
    
    close(fd);
    
    // 设置块设备特定配置
    priv->io_config.use_direct_io = true;  // 块设备通常使用直接I/O
    priv->io_config.io_alignment = 512;    // 512字节对齐
    priv->prealloc_config.use_fallocate = false; // 块设备不支持fallocate
    
    return 0;
}

// 对齐I/O操作
static ssize_t aligned_pread(int fd, void *buf, size_t count, 
                            off_t offset, size_t alignment) {
    // 检查是否已对齐
    if ((uintptr_t)buf % alignment == 0 && 
        offset % alignment == 0 && 
        count % alignment == 0) {
        return pread(fd, buf, count, offset);
    }
    
    // 需要对齐处理
    size_t aligned_size = ((count + alignment - 1) / alignment) * alignment;
    off_t aligned_offset = (offset / alignment) * alignment;
    
    void *aligned_buf = aligned_alloc(alignment, aligned_size);
    if (!aligned_buf) return -ENOMEM;
    
    ssize_t result = pread(fd, aligned_buf, aligned_size, aligned_offset);
    if (result > 0) {
        size_t copy_offset = offset - aligned_offset;
        size_t copy_size = min(count, result - copy_offset);
        memcpy(buf, (char*)aligned_buf + copy_offset, copy_size);
        result = copy_size;
    }
    
    free(aligned_buf);
    return result;
}

static ssize_t aligned_pwrite(int fd, const void *buf, size_t count,
                             off_t offset, size_t alignment) {
    // 检查是否已对齐
    if ((uintptr_t)buf % alignment == 0 && 
        offset % alignment == 0 && 
        count % alignment == 0) {
        return pwrite(fd, buf, count, offset);
    }
    
    // 需要对齐处理
    size_t aligned_size = ((count + alignment - 1) / alignment) * alignment;
    off_t aligned_offset = (offset / alignment) * alignment;
    
    void *aligned_buf = aligned_alloc(alignment, aligned_size);
    if (!aligned_buf) return -ENOMEM;
    
    // 读取原有数据(部分写入情况)
    ssize_t read_result = pread(fd, aligned_buf, aligned_size, aligned_offset);
    if (read_result < 0 && errno != 0) {
        free(aligned_buf);
        return read_result;
    }
    
    // 拷贝新数据
    size_t copy_offset = offset - aligned_offset;
    memcpy((char*)aligned_buf + copy_offset, buf, count);
    
    // 写入对齐数据
    ssize_t result = pwrite(fd, aligned_buf, aligned_size, aligned_offset);
    
    free(aligned_buf);
    
    return (result >= copy_offset + count) ? count : -EIO;
}
```

### 2.3 性能调优

```c
// 本地存储性能调优参数
typedef struct {
    // I/O优化
    bool use_direct_io;           // 直接I/O
    bool use_mmap;               // 内存映射
    size_t io_alignment;         // I/O对齐
    uint32_t read_ahead_kb;      // 预读大小
    
    // 缓存优化
    size_t page_cache_size;      // 页缓存大小
    bool disable_atime;          // 禁用访问时间更新
    bool use_tmpfs_cache;        // 使用tmpfs缓存
    
    // 文件系统优化
    bool use_ext4_delalloc;      // ext4延迟分配
    bool use_xfs_allocsize;      // XFS分配大小
    bool use_btrfs_compression;  // Btrfs压缩
    
    // 调度优化
    char io_scheduler[16];       // I/O调度器
    int nice_value;              // 进程优先级
    int ionice_class;            // I/O优先级类
    int ionice_level;            // I/O优先级级别
    
} local_tuning_params_t;

// 自动性能调优
static void local_auto_tuning(local_private_data_t *priv) {
    storage_stats_t *stats = &priv->stats;
    
    // 根据访问模式调整缓存策略
    double random_ratio = calculate_random_access_ratio(stats);
    if (random_ratio > 0.8) {
        // 随机访问为主，增加缓存大小
        if (priv->cache_config.cache_size < MAX_CACHE_SIZE) {
            priv->cache_config.cache_size *= 1.5;
            resize_local_cache(priv->cache_config.cache_instance,
                             priv->cache_config.cache_size);
        }
    } else {
        // 顺序访问为主，启用预读
        if (!priv->io_config.use_mmap) {
            enable_readahead(priv);
        }
    }
    
    // 根据延迟调整I/O策略
    if (stats->write_latency_avg_ns > HIGH_LATENCY_THRESHOLD) {
        // 写延迟高，考虑使用异步I/O
        if (!priv->aio_config.use_aio) {
            enable_async_io(priv);
        }
    }
    
    // 根据文件大小调整预分配策略
    uint64_t avg_file_size = stats->write_bytes / max(stats->write_ops, 1);
    if (avg_file_size > priv->prealloc_config.default_file_size) {
        priv->prealloc_config.default_file_size = avg_file_size * 1.2;
    }
}
```

## 3. 扩展本地文件类型实现

### 3.1 稀疏文件支持

```c
// 稀疏文件私有数据
typedef struct {
    bool enable_sparse;            // 启用稀疏文件
    size_t hole_detection_size;    // 空洞检测大小
    uint64_t total_allocated_bytes; // 实际分配字节数
    uint64_t total_logical_bytes;   // 逻辑文件大小
    struct sparse_map *hole_map;    // 空洞映射表
} sparse_config_t;

// 稀疏文件空洞映射
typedef struct sparse_extent {
    uint64_t start_offset;         // 起始偏移
    uint64_t length;              // 长度
    bool is_hole;                 // 是否为空洞
    struct sparse_extent *next;   // 下一个区段
} sparse_extent_t;

// 稀疏文件写入优化
static int sparse_write_sync(storage_interface_t *iface,
                            const char *path, uint64_t offset,
                            const void *data, size_t size) {
    local_private_data_t *priv = iface->private_data;
    
    // 检查是否为零数据块
    if (is_zero_block(data, size)) {
        // 创建空洞而不是写入零
        return create_file_hole(path, offset, size);
    }
    
    // 构造完整路径
    char full_path[512];
    build_full_path(priv, path, full_path, sizeof(full_path));
    
    // 打开文件（支持稀疏）
    int fd = open(full_path, O_WRONLY | O_CREAT, priv->file_permissions);
    if (fd < 0) return -errno;
    
    // 使用fallocate创建稀疏区域
    if (priv->prealloc_config.use_fallocate) {
        // FALLOC_FL_PUNCH_HOLE创建空洞
        if (is_mostly_zeros(data, size)) {
            fallocate(fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE,
                     offset, size);
        }
    }
    
    ssize_t result = pwrite(fd, data, size, offset);
    close(fd);
    
    return (result == size) ? 0 : -EIO;
}

// 检测空洞
static bool is_zero_block(const void *data, size_t size) {
    const uint64_t *p = (const uint64_t*)data;
    size_t count = size / sizeof(uint64_t);
    
    for (size_t i = 0; i < count; i++) {
        if (p[i] != 0) return false;
    }
    
    // 检查剩余字节
    const uint8_t *remainder = (const uint8_t*)(p + count);
    size_t remaining = size % sizeof(uint64_t);
    for (size_t i = 0; i < remaining; i++) {
        if (remainder[i] != 0) return false;
    }
    
    return true;
}
```

### 3.2 压缩文件支持

```c
// 压缩后端实现
typedef struct {
    int compression_type;          // 压缩类型
    int compression_level;         // 压缩级别
    size_t compression_threshold;  // 压缩阈值
    bool adaptive_compression;     // 自适应压缩
    
    // 压缩统计
    uint64_t compressed_bytes;     // 压缩后字节数
    uint64_t uncompressed_bytes;   // 压缩前字节数
    double avg_compression_ratio;  // 平均压缩比
    
    // 压缩缓存
    struct compression_cache *cache;
} compression_config_t;

// 压缩类型定义
typedef enum {
    COMPRESS_NONE = 0,
    COMPRESS_LZ4 = 1,              // 高速压缩
    COMPRESS_ZSTD = 2,             // 平衡压缩
    COMPRESS_SNAPPY = 3,           // Google Snappy
    COMPRESS_GZIP = 4,             // 标准gzip
    COMPRESS_BROTLI = 5            // Google Brotli
} compression_type_t;

// 自适应压缩决策
static bool should_compress_data(compression_config_t *comp_config,
                               const void *data, size_t size) {
    // 小于阈值不压缩
    if (size < comp_config->compression_threshold) {
        return false;
    }
    
    // 检查数据熵（随机数据压缩效果差）
    double entropy = calculate_entropy(data, min(size, 4096));
    if (entropy > 7.5) {  // 高熵数据不压缩
        return false;
    }
    
    // 自适应压缩：根据历史压缩比决策
    if (comp_config->adaptive_compression) {
        if (comp_config->avg_compression_ratio < 1.2) {
            return false;  // 压缩效果不好
        }
    }
    
    return true;
}

// LZ4压缩实现
static void* compress_lz4(const void *data, size_t size, 
                         size_t *compressed_size, int level) {
    int max_compressed = LZ4_compressBound(size);
    void *compressed = malloc(max_compressed + sizeof(compression_header_t));
    if (!compressed) return NULL;
    
    compression_header_t *header = (compression_header_t*)compressed;
    header->magic = COMPRESSION_MAGIC;
    header->type = COMPRESS_LZ4;
    header->level = level;
    header->original_size = size;
    header->crc32 = calculate_crc32(data, size);
    
    char *compressed_data = (char*)compressed + sizeof(compression_header_t);
    
    int result;
    if (level <= 3) {
        result = LZ4_compress_default(data, compressed_data, size, max_compressed);
    } else {
        result = LZ4_compress_HC(data, compressed_data, size, max_compressed, level);
    }
    
    if (result <= 0) {
        free(compressed);
        return NULL;
    }
    
    header->compressed_size = result;
    *compressed_size = result + sizeof(compression_header_t);
    
    return compressed;
}

// ZSTD压缩实现
static void* compress_zstd(const void *data, size_t size,
                          size_t *compressed_size, int level) {
    size_t max_compressed = ZSTD_compressBound(size);
    void *compressed = malloc(max_compressed + sizeof(compression_header_t));
    if (!compressed) return NULL;
    
    compression_header_t *header = (compression_header_t*)compressed;
    header->magic = COMPRESSION_MAGIC;
    header->type = COMPRESS_ZSTD;
    header->level = level;
    header->original_size = size;
    header->crc32 = calculate_crc32(data, size);
    
    char *compressed_data = (char*)compressed + sizeof(compression_header_t);
    
    size_t result = ZSTD_compress(compressed_data, max_compressed, 
                                 data, size, level);
    
    if (ZSTD_isError(result)) {
        free(compressed);
        return NULL;
    }
    
    header->compressed_size = result;
    *compressed_size = result + sizeof(compression_header_t);
    
    return compressed;
}
```

### 3.3 混合存储模式

```c
// 混合存储配置
typedef struct {
    // 热数据配置
    struct {
        local_storage_type_t hot_storage_type;  // 热数据存储类型
        char hot_directory[256];                // 热数据目录
        size_t hot_cache_size;                  // 热数据缓存大小
        uint32_t hot_access_threshold;          // 热数据访问阈值
    } hot_config;
    
    // 冷数据配置  
    struct {
        local_storage_type_t cold_storage_type; // 冷数据存储类型
        char cold_directory[256];               // 冷数据目录
        uint32_t cold_timeout_sec;              // 冷却时间
        bool enable_compression;                // 冷数据压缩
    } cold_config;
    
    // 迁移配置
    struct {
        bool auto_migration;                    // 自动迁移
        size_t migration_batch_size;            // 迁移批大小
        uint32_t migration_interval_sec;        // 迁移间隔
        pthread_t migration_thread;             // 迁移线程
        bool migration_thread_running;          // 迁移线程状态
    } migration_config;
    
    // 访问统计
    struct access_tracker *access_tracker;     // 访问跟踪器
    
} hybrid_config_t;

// 文件访问统计
typedef struct file_access_stats {
    char path[256];                // 文件路径
    uint64_t access_count;         // 访问次数
    uint64_t last_access_time;     // 最后访问时间
    uint64_t total_access_bytes;   // 总访问字节数
    double access_frequency;       // 访问频率
    bool is_hot;                   // 是否为热数据
} file_access_stats_t;

// 混合存储读取
static int hybrid_read_sync(storage_interface_t *iface,
                           const char *path, uint64_t offset,
                           void *buffer, size_t size) {
    local_private_data_t *priv = iface->private_data;
    hybrid_config_t *hybrid = &priv->hybrid_config;
    
    // 更新访问统计
    update_access_stats(hybrid->access_tracker, path, size, true);
    
    // 检查是否为热数据
    file_access_stats_t *stats = get_file_stats(hybrid->access_tracker, path);
    
    char full_path[512];
    if (stats && stats->is_hot) {
        // 从热存储读取
        snprintf(full_path, sizeof(full_path), "%s/%s", 
                hybrid->hot_config.hot_directory, path);
    } else {
        // 从冷存储读取
        snprintf(full_path, sizeof(full_path), "%s/%s",
                hybrid->cold_config.cold_directory, path);
    }
    
    // 执行实际读取
    int result = perform_storage_read(iface, full_path, offset, buffer, size);
    
    // 检查是否需要提升为热数据
    if (result == 0 && !stats->is_hot) {
        if (should_promote_to_hot(stats, hybrid)) {
            schedule_data_migration(hybrid, path, false); // 冷->热
        }
    }
    
    return result;
}

// 混合存储写入
static int hybrid_write_sync(storage_interface_t *iface,
                            const char *path, uint64_t offset,
                            const void *data, size_t size) {
    local_private_data_t *priv = iface->private_data;
    hybrid_config_t *hybrid = &priv->hybrid_config;
    
    // 更新访问统计
    update_access_stats(hybrid->access_tracker, path, size, false);
    
    // 新数据写入热存储
    char hot_path[512];
    snprintf(hot_path, sizeof(hot_path), "%s/%s",
            hybrid->hot_config.hot_directory, path);
    
    // 标记为热数据
    file_access_stats_t *stats = get_file_stats(hybrid->access_tracker, path);
    if (stats) {
        stats->is_hot = true;
    }
    
    return perform_storage_write(iface, hot_path, offset, data, size);
}

// 数据迁移决策
static bool should_promote_to_hot(file_access_stats_t *stats,
                                 hybrid_config_t *hybrid) {
    uint64_t current_time = get_monotonic_time_ns() / 1000000000; // 秒
    
    // 访问频率检查
    if (stats->access_count < hybrid->hot_config.hot_access_threshold) {
        return false;
    }
    
    // 最近访问检查
    if (current_time - stats->last_access_time > 300) { // 5分钟内访问
        return false;
    }
    
    // 访问量检查
    if (stats->total_access_bytes < 1024 * 1024) { // 至少1MB访问量
        return false;
    }
    
    return true;
}

static bool should_demote_to_cold(file_access_stats_t *stats,
                                 hybrid_config_t *hybrid) {
    uint64_t current_time = get_monotonic_time_ns() / 1000000000;
    
    // 长时间未访问
    if (current_time - stats->last_access_time > hybrid->cold_config.cold_timeout_sec) {
        return true;
    }
    
    // 访问频率降低
    if (stats->access_frequency < 0.1) { // 访问频率低于0.1次/秒
        return true;
    }
    
    return false;
}

// 数据迁移执行
static void* data_migration_worker(void *arg) {
    local_private_data_t *priv = (local_private_data_t*)arg;
    hybrid_config_t *hybrid = &priv->hybrid_config;
    
    while (hybrid->migration_config.migration_thread_running) {
        // 扫描需要迁移的文件
        scan_and_migrate_files(hybrid);
        
        // 等待下次迁移
        sleep(hybrid->migration_config.migration_interval_sec);
    }
    
    return NULL;
}

static void scan_and_migrate_files(hybrid_config_t *hybrid) {
    struct access_tracker *tracker = hybrid->access_tracker;
    
    // 扫描热数据，检查是否需要降级
    for (int i = 0; i < tracker->hot_file_count; i++) {
        file_access_stats_t *stats = &tracker->hot_files[i];
        
        if (should_demote_to_cold(stats, hybrid)) {
            migrate_file_to_cold(hybrid, stats->path);
            stats->is_hot = false;
        }
    }
    
    // 扫描冷数据，检查是否需要升级
    for (int i = 0; i < tracker->cold_file_count; i++) {
        file_access_stats_t *stats = &tracker->cold_files[i];
        
        if (should_promote_to_hot(stats, hybrid)) {
            migrate_file_to_hot(hybrid, stats->path);
            stats->is_hot = true;
        }
    }
}
```

### 3.4 性能监控和自动调优

```c
// 性能监控数据
typedef struct {
    // I/O性能指标
    struct {
        uint64_t read_iops;           // 读取IOPS
        uint64_t write_iops;          // 写入IOPS
        uint64_t read_bandwidth;      // 读取带宽
        uint64_t write_bandwidth;     // 写入带宽
        uint64_t avg_latency_us;      // 平均延迟(微秒)
        uint64_t p99_latency_us;      // P99延迟
    } io_metrics;
    
    // 缓存效率指标
    struct {
        double cache_hit_ratio;       // 缓存命中率
        uint64_t cache_size_bytes;    // 缓存大小
        uint64_t cache_evictions;     // 缓存驱逐次数
    } cache_metrics;
    
    // 存储效率指标
    struct {
        double compression_ratio;     // 压缩比
        double space_utilization;    // 空间利用率
        uint64_t sparse_holes_bytes;  // 稀疏文件空洞大小
    } storage_metrics;
    
    // 系统资源指标
    struct {
        double cpu_usage_percent;     // CPU使用率
        uint64_t memory_usage_bytes;  // 内存使用量
        uint64_t disk_usage_bytes;    // 磁盘使用量
        uint32_t open_files_count;    // 打开文件数
    } resource_metrics;
    
} performance_metrics_t;

// 自动调优引擎
typedef struct {
    performance_metrics_t current_metrics;
    performance_metrics_t target_metrics;
    
    // 调优历史
    struct tuning_history {
        uint64_t timestamp;
        char parameter[64];
        int old_value;
        int new_value;
        double performance_delta;
    } history[100];
    
    int history_count;
    
    // 调优策略
    bool enable_auto_tuning;
    uint32_t tuning_interval_sec;
    double min_improvement_threshold;
    
} auto_tuning_engine_t;

// 自动调优实现
static void auto_tune_performance(local_private_data_t *priv) {
    auto_tuning_engine_t *tuner = &priv->auto_tuner;
    performance_metrics_t *metrics = &tuner->current_metrics;
    
    // 收集当前性能指标
    collect_performance_metrics(priv, metrics);
    
    // I/O延迟优化
    if (metrics->io_metrics.avg_latency_us > TARGET_LATENCY_US) {
        // 延迟过高，尝试优化
        if (!priv->aio_config.use_aio) {
            // 启用异步I/O
            enable_async_io(priv);
            record_tuning_change(tuner, "enable_aio", 0, 1);
        } else if (!priv->io_config.use_direct_io) {
            // 启用直接I/O
            priv->io_config.use_direct_io = true;
            record_tuning_change(tuner, "enable_direct_io", 0, 1);
        }
    }
    
    // 缓存命中率优化
    if (metrics->cache_metrics.cache_hit_ratio < TARGET_CACHE_HIT_RATIO) {
        // 命中率低，增加缓存大小
        if (priv->cache_config.cache_size < MAX_CACHE_SIZE) {
            size_t old_size = priv->cache_config.cache_size;
            priv->cache_config.cache_size *= 1.5;
            resize_local_cache(priv->cache_config.cache_instance,
                             priv->cache_config.cache_size);
            record_tuning_change(tuner, "cache_size", old_size, 
                                priv->cache_config.cache_size);
        }
    }
    
    // 压缩效率优化
    if (priv->compression.enable_compression &&
        metrics->storage_metrics.compression_ratio < 1.5) {
        // 压缩效果差，降低压缩级别或禁用压缩
        if (priv->compression.compression_level > 1) {
            priv->compression.compression_level--;
            record_tuning_change(tuner, "compression_level",
                                priv->compression.compression_level + 1,
                                priv->compression.compression_level);
        } else {
            priv->compression.enable_compression = false;
            record_tuning_change(tuner, "enable_compression", 1, 0);
        }
    }
    
    // IOPS优化
    if (metrics->io_metrics.read_iops + metrics->io_metrics.write_iops < TARGET_IOPS) {
        // IOPS低，尝试批量操作
        if (!priv->batch_config.enable_batching) {
            priv->batch_config.enable_batching = true;
            priv->batch_config.batch_size = 32;
            record_tuning_change(tuner, "enable_batching", 0, 1);
        }
    }
}

// 性能指标收集
static void collect_performance_metrics(local_private_data_t *priv,
                                       performance_metrics_t *metrics) {
    storage_stats_t *stats = &priv->stats;
    
    // 计算IOPS
    uint64_t time_window = 1000000000; // 1秒窗口
    metrics->io_metrics.read_iops = 
        calculate_ops_per_second(stats->read_ops, time_window);
    metrics->io_metrics.write_iops = 
        calculate_ops_per_second(stats->write_ops, time_window);
    
    // 计算带宽
    metrics->io_metrics.read_bandwidth = 
        calculate_bytes_per_second(stats->read_bytes, time_window);
    metrics->io_metrics.write_bandwidth = 
        calculate_bytes_per_second(stats->write_bytes, time_window);
    
    // 计算延迟
    metrics->io_metrics.avg_latency_us = 
        stats->read_latency_avg_ns / 1000; // 转换为微秒
    metrics->io_metrics.p99_latency_us = 
        stats->read_latency_p99_ns / 1000;
    
    // 缓存指标
    if (priv->cache_config.cache_instance) {
        local_cache_stats_t cache_stats;
        get_cache_stats(priv->cache_config.cache_instance, &cache_stats);
        
        metrics->cache_metrics.cache_hit_ratio = 
            (double)cache_stats.hits / (cache_stats.hits + cache_stats.misses);
        metrics->cache_metrics.cache_size_bytes = cache_stats.size_bytes;
        metrics->cache_metrics.cache_evictions = cache_stats.evictions;
    }
    
    // 存储指标
    if (priv->compression.enable_compression) {
        metrics->storage_metrics.compression_ratio = 
            (double)stats->write_bytes / stats->compressed_bytes;
    }
    
    // 系统资源指标
    collect_system_metrics(metrics);
}
```

## 4. API使用示例

### 4.1 基本使用示例

```c
#include "xdevice.h"
#include "storage_interface.h"

int main() {
    // 创建本地文件存储实例
    storage_interface_t *local_storage = create_local_backend();
    if (!local_storage) {
        fprintf(stderr, "Failed to create local storage backend\n");
        return -1;
    }
    
    // 初始化存储后端
    const char *config = 
        "[local]\n"
        "storage_type = regular\n"
        "base_directory = /tmp/xdevice-test\n"
        "use_aio = true\n"
        "enable_read_cache = true\n"
        "cache_size = 67108864\n";
    
    if (local_storage->init(local_storage, config) != 0) {
        fprintf(stderr, "Failed to initialize local storage\n");
        free(local_storage);
        return -1;
    }
    
    // 写入数据
    const char *test_data = "Hello, xDevice!";
    int result = local_storage->write_sync(local_storage, 
                                          "test_file.dat", 0, 
                                          test_data, strlen(test_data));
    
    if (result != 0) {
        fprintf(stderr, "Failed to write data: %d\n", result);
        goto cleanup;
    }
    
    // 读取数据
    char read_buffer[256];
    result = local_storage->read_sync(local_storage,
                                     "test_file.dat", 0,
                                     read_buffer, sizeof(read_buffer));
    
    if (result == 0) {
        printf("Read data: %s\n", read_buffer);
    } else {
        fprintf(stderr, "Failed to read data: %d\n", result);
    }
    
    // 获取统计信息
    storage_stats_t stats;
    local_storage->get_stats(local_storage, &stats);
    printf("Read operations: %lu\n", stats.read_ops);
    printf("Write operations: %lu\n", stats.write_ops);
    printf("Read bytes: %lu\n", stats.read_bytes);
    printf("Write bytes: %lu\n", stats.write_bytes);
    
cleanup:
    // 清理资源
    local_storage->cleanup(local_storage);
    free(local_storage);
    
    return 0;
}
```

### 4.2 异步I/O使用示例

```c
#include <pthread.h>
#include <semaphore.h>

// 异步操作完成回调
void async_callback(void *context, storage_op_status_t status,
                   void *data, size_t size) {
    sem_t *completion_sem = (sem_t*)context;
    
    if (status == STORAGE_OP_SUCCESS) {
        printf("Async operation completed successfully, %zu bytes\n", size);
    } else {
        printf("Async operation failed: %d\n", status);
    }
    
    // 通知主线程操作完成
    sem_post(completion_sem);
}

int async_io_example() {
    storage_interface_t *local_storage = create_local_backend();
    
    // 配置异步I/O
    const char *config = 
        "[local]\n"
        "storage_type = regular\n"
        "base_directory = /tmp/xdevice-async\n"
        "use_aio = true\n"
        "max_events = 1024\n";
    
    local_storage->init(local_storage, config);
    
    // 创建信号量用于同步
    sem_t completion_sem;
    sem_init(&completion_sem, 0, 0);
    
    // 准备异步操作上下文
    storage_operation_ctx_t ctx = {
        .operation_id = 1,
        .user_context = &completion_sem,
        .callback = async_callback,
        .start_time_ns = get_monotonic_time_ns(),
        .timeout_ns = 5000000000ULL  // 5秒超时
    };
    
    // 异步写入
    const char *data = "Async write test";
    int result = local_storage->write_async(local_storage,
                                           "async_test.dat", 0,
                                           data, strlen(data), &ctx);
    
    if (result == 0) {
        // 等待操作完成
        sem_wait(&completion_sem);
        printf("Async write completed\n");
    }
    
    // 异步读取
    char read_buffer[256];
    ctx.operation_id = 2;
    result = local_storage->read_async(local_storage,
                                      "async_test.dat", 0,
                                      read_buffer, sizeof(read_buffer), &ctx);
    
    if (result == 0) {
        // 等待操作完成
        sem_wait(&completion_sem);
        printf("Async read completed: %s\n", read_buffer);
    }
    
    // 清理
    sem_destroy(&completion_sem);
    local_storage->cleanup(local_storage);
    free(local_storage);
    
    return 0;
}
```

### 4.3 混合存储使用示例

```c
int hybrid_storage_example() {
    storage_interface_t *hybrid_storage = create_local_backend();
    
    // 配置混合存储
    const char *config = 
        "[local]\n"
        "storage_type = hybrid\n"
        "base_directory = /var/lib/xdevice\n"
        "\n"
        "[hybrid]\n"
        "hot_storage_type = tmpfs\n"
        "hot_directory = /dev/shm/xdevice-hot\n"
        "hot_cache_size = 268435456\n"
        "hot_access_threshold = 5\n"
        "cold_storage_type = compressed\n"
        "cold_directory = /var/lib/xdevice/cold\n"
        "cold_timeout_sec = 600\n"
        "auto_migration = true\n"
        "\n"
        "[tmpfs]\n"
        "tmpfs_size = 536870912\n"
        "auto_mount = true\n";
    
    hybrid_storage->init(hybrid_storage, config);
    
    // 写入热数据（频繁访问）
    for (int i = 0; i < 10; i++) {
        char data[1024];
        snprintf(data, sizeof(data), "Hot data iteration %d", i);
        
        hybrid_storage->write_sync(hybrid_storage, "hot_file.dat", 
                                  i * 1024, data, strlen(data));
        
        // 立即读取（模拟频繁访问）
        char read_buf[1024];
        hybrid_storage->read_sync(hybrid_storage, "hot_file.dat",
                                 i * 1024, read_buf, sizeof(read_buf));
    }
    
    // 写入冷数据（不频繁访问）
    const char *cold_data = "This is cold data that will be migrated";
    hybrid_storage->write_sync(hybrid_storage, "cold_file.dat", 0,
                              cold_data, strlen(cold_data));
    
    // 等待一段时间让系统进行数据迁移
    sleep(10);
    
    // 检查数据是否被正确迁移
    char verify_buf[256];
    int result = hybrid_storage->read_sync(hybrid_storage, "cold_file.dat", 0,
                                         verify_buf, sizeof(verify_buf));
    
    if (result == 0) {
        printf("Cold data read successfully: %s\n", verify_buf);
    }
    
    // 获取混合存储统计信息
    storage_stats_t stats;
    hybrid_storage->get_stats(hybrid_storage, &stats);
    printf("Hot storage reads: %lu\n", stats.hot_reads);
    printf("Cold storage reads: %lu\n", stats.cold_reads);
    printf("Migration count: %lu\n", stats.migrations);
    
    hybrid_storage->cleanup(hybrid_storage);
    free(hybrid_storage);
    
    return 0;
}
```

### 4.4 批量操作示例

```c
int batch_operations_example() {
    storage_interface_t *local_storage = create_local_backend();
    
    const char *config = 
        "[local]\n"
        "storage_type = regular\n"
        "base_directory = /tmp/xdevice-batch\n"
        "use_aio = true\n"
        "enable_batching = true\n"
        "batch_size = 32\n";
    
    local_storage->init(local_storage, config);
    
    // 准备批量操作
    const int batch_size = 10;
    storage_batch_operation_t operations[batch_size];
    
    for (int i = 0; i < batch_size; i++) {
        operations[i].type = STORAGE_OP_WRITE;
        snprintf(operations[i].path, sizeof(operations[i].path), 
                "batch_file_%d.dat", i);
        operations[i].offset = 0;
        
        char *data = malloc(256);
        snprintf(data, 256, "Batch operation data %d", i);
        operations[i].data = data;
        operations[i].size = strlen(data);
    }
    
    // 执行批量操作
    storage_batch_result_t results[batch_size];
    int batch_result = local_storage->batch_operations(local_storage,
                                                      operations, batch_size,
                                                      results);
    
    if (batch_result == 0) {
        printf("Batch operations completed successfully\n");
        
        // 检查每个操作的结果
        for (int i = 0; i < batch_size; i++) {
            if (results[i].status == STORAGE_OP_SUCCESS) {
                printf("Operation %d succeeded\n", i);
            } else {
                printf("Operation %d failed: %d\n", i, results[i].status);
            }
        }
    } else {
        printf("Batch operations failed: %d\n", batch_result);
    }
    
    // 清理数据
    for (int i = 0; i < batch_size; i++) {
        free((void*)operations[i].data);
    }
    
    local_storage->cleanup(local_storage);
    free(local_storage);
    
    return 0;
}
```

### 4.5 错误处理和重试示例

```c
#include <errno.h>

int error_handling_example() {
    storage_interface_t *local_storage = create_local_backend();
    
    // 配置错误恢复
    const char *config = 
        "[local]\n"
        "storage_type = regular\n"
        "base_directory = /tmp/xdevice-error\n"
        "\n"
        "[recovery]\n"
        "max_retries = 3\n"
        "retry_delay_ms = 100\n"
        "exponential_backoff = true\n"
        "enable_checksums = true\n";
    
    local_storage->init(local_storage, config);
    
    const char *test_data = "Test data for error handling";
    
    // 带重试的写入操作
    int max_attempts = 3;
    int delay_ms = 100;
    
    for (int attempt = 0; attempt < max_attempts; attempt++) {
        int result = local_storage->write_sync(local_storage,
                                              "error_test.dat", 0,
                                              test_data, strlen(test_data));
        
        if (result == 0) {
            printf("Write succeeded on attempt %d\n", attempt + 1);
            break;
        }
        
        printf("Write failed on attempt %d: %s\n", 
               attempt + 1, strerror(-result));
        
        if (attempt < max_attempts - 1) {
            usleep(delay_ms * 1000);
            delay_ms *= 2;  // 指数退避
        }
    }
    
    // 数据完整性验证
    char read_buffer[256];
    int result = local_storage->read_sync(local_storage,
                                         "error_test.dat", 0,
                                         read_buffer, sizeof(read_buffer));
    
    if (result == 0) {
        // 验证数据完整性
        if (strcmp(read_buffer, test_data) == 0) {
            printf("Data integrity verified\n");
        } else {
            printf("Data corruption detected!\n");
        }
    }
    
    // 健康检查
    if (local_storage->health_check(local_storage) == 0) {
        printf("Storage backend is healthy\n");
    } else {
        printf("Storage backend health check failed\n");
    }
    
    local_storage->cleanup(local_storage);
    free(local_storage);
    
    return 0;
}
```

## 5. 性能测试和基准测试

### 5.1 性能测试工具

```c
// 性能测试结构
typedef struct {
    uint64_t total_operations;     // 总操作数
    uint64_t total_bytes;          // 总字节数
    uint64_t start_time_ns;        // 开始时间
    uint64_t end_time_ns;          // 结束时间
    uint64_t min_latency_ns;       // 最小延迟
    uint64_t max_latency_ns;       // 最大延迟
    uint64_t total_latency_ns;     // 总延迟
    uint64_t latency_histogram[20]; // 延迟直方图
} performance_test_result_t;

// 基准测试：顺序写入
int benchmark_sequential_write(storage_interface_t *storage,
                              size_t file_size, size_t block_size) {
    performance_test_result_t result = {0};
    result.start_time_ns = get_monotonic_time_ns();
    result.min_latency_ns = UINT64_MAX;
    
    char *buffer = malloc(block_size);
    memset(buffer, 0xAA, block_size);
    
    for (uint64_t offset = 0; offset < file_size; offset += block_size) {
        uint64_t op_start = get_monotonic_time_ns();
        
        int ret = storage->write_sync(storage, "benchmark.dat", 
                                     offset, buffer, block_size);
        
        uint64_t op_end = get_monotonic_time_ns();
        uint64_t latency = op_end - op_start;
        
        if (ret != 0) {
            printf("Write failed at offset %lu: %d\n", offset, ret);
            free(buffer);
            return -1;
        }
        
        // 更新统计
        result.total_operations++;
        result.total_bytes += block_size;
        result.total_latency_ns += latency;
        result.min_latency_ns = min(result.min_latency_ns, latency);
        result.max_latency_ns = max(result.max_latency_ns, latency);
        
        // 更新延迟直方图
        int bucket = min(latency / 100000, 19); // 100us bucket
        result.latency_histogram[bucket]++;
    }
    
    result.end_time_ns = get_monotonic_time_ns();
    
    // 输出结果
    print_benchmark_results("Sequential Write", &result);
    
    free(buffer);
    return 0;
}

// 基准测试：随机读取
int benchmark_random_read(storage_interface_t *storage,
                         size_t file_size, size_t block_size,
                         int num_operations) {
    performance_test_result_t result = {0};
    result.start_time_ns = get_monotonic_time_ns();
    result.min_latency_ns = UINT64_MAX;
    
    char *buffer = malloc(block_size);
    srand(time(NULL));
    
    for (int i = 0; i < num_operations; i++) {
        // 生成随机偏移
        uint64_t offset = (rand() % (file_size / block_size)) * block_size;
        
        uint64_t op_start = get_monotonic_time_ns();
        
        int ret = storage->read_sync(storage, "benchmark.dat",
                                    offset, buffer, block_size);
        
        uint64_t op_end = get_monotonic_time_ns();
        uint64_t latency = op_end - op_start;
        
        if (ret != 0) {
            printf("Read failed at offset %lu: %d\n", offset, ret);
            free(buffer);
            return -1;
        }
        
        // 更新统计
        result.total_operations++;
        result.total_bytes += block_size;
        result.total_latency_ns += latency;
        result.min_latency_ns = min(result.min_latency_ns, latency);
        result.max_latency_ns = max(result.max_latency_ns, latency);
        
        int bucket = min(latency / 100000, 19);
        result.latency_histogram[bucket]++;
    }
    
    result.end_time_ns = get_monotonic_time_ns();
    
    print_benchmark_results("Random Read", &result);
    
    free(buffer);
    return 0;
}

// 输出基准测试结果
void print_benchmark_results(const char *test_name,
                           performance_test_result_t *result) {
    uint64_t duration_ns = result->end_time_ns - result->start_time_ns;
    double duration_sec = duration_ns / 1000000000.0;
    
    double iops = result->total_operations / duration_sec;
    double bandwidth_mb = (result->total_bytes / (1024.0 * 1024.0)) / duration_sec;
    double avg_latency_us = (result->total_latency_ns / result->total_operations) / 1000.0;
    
    printf("\n=== %s Benchmark Results ===\n", test_name);
    printf("Duration: %.2f seconds\n", duration_sec);
    printf("Operations: %lu\n", result->total_operations);
    printf("Total bytes: %lu\n", result->total_bytes);
    printf("IOPS: %.2f\n", iops);
    printf("Bandwidth: %.2f MB/s\n", bandwidth_mb);
    printf("Average latency: %.2f us\n", avg_latency_us);
    printf("Min latency: %.2f us\n", result->min_latency_ns / 1000.0);
    printf("Max latency: %.2f us\n", result->max_latency_ns / 1000.0);
    
    printf("\nLatency histogram (100us buckets):\n");
    for (int i = 0; i < 20; i++) {
        if (result->latency_histogram[i] > 0) {
            printf("  %d-%d us: %lu operations\n", 
                   i * 100, (i + 1) * 100, result->latency_histogram[i]);
        }
    }
    printf("\n");
}
```

这个完善的本地文件后端实现提供了：

1. **多种存储类型支持**：普通文件、内存映射、直接I/O、TMPFS、块设备、稀疏文件、压缩文件、混合存储
2. **智能压缩**：支持多种压缩算法，自适应压缩决策
3. **混合存储**：热数据放内存，冷数据放磁盘，自动迁移
4. **性能监控**：全面的性能指标收集和自动调优
5. **错误恢复**：检查点、重试机制、数据完整性验证

这些功能可以满足不同WAL场景的性能和可靠性需求。
