# NFS 存储后端实现

## 1. NFS后端设计

### 1.1 NFS接口实现

```c
#include <rpc/rpc.h>
#include <nfs/libnfs.h>
#include <sys/mount.h>

// NFS私有数据结构
typedef struct {
    // NFS连接信息
    struct nfs_context *nfs_ctx;      // NFS上下文
    char server_addr[64];              // NFS服务器地址
    char export_path[256];             // 导出路径
    char mount_point[256];             // 挂载点
    uint32_t nfs_version;              // NFS版本(3/4)
    
    // 挂载选项
    struct {
        uint32_t rsize;                // 读缓冲区大小
        uint32_t wsize;                // 写缓冲区大小
        uint32_t timeo;                // 超时时间
        uint32_t retrans;              // 重传次数
        char proto[8];                 // 协议(tcp/udp)
        bool hard_mount;               // 硬挂载
        bool intr;                     // 可中断
        bool ac;                       // 属性缓存
        uint32_t acregmin;             // 文件属性缓存最小时间
        uint32_t acregmax;             // 文件属性缓存最大时间
    } mount_options;
    
    // 缓存管理
    struct {
        bool read_cache_enabled;       // 读缓存
        bool write_cache_enabled;      // 写缓存
        size_t cache_size;            // 缓存大小
        uint32_t cache_ttl;           // 缓存TTL
        struct nfs_cache *read_cache; // 读缓存实例
        struct nfs_cache *write_cache; // 写缓存实例
    } cache;
    
    // 异步I/O管理
    struct {
        pthread_t worker_threads[MAX_NFS_WORKERS]; // 工作线程
        uint32_t worker_count;         // 工作线程数
        struct nfs_async_queue *pending_ops; // 待处理操作队列
        bool use_async;               // 使用异步I/O
    } async_io;
    
    // 连接池
    struct {
        struct nfs_context *connections[MAX_NFS_CONNECTIONS];
        uint32_t connection_count;     // 连接数
        uint32_t current_connection;   // 当前连接索引
        pthread_mutex_t pool_lock;     // 连接池锁
    } connection_pool;
    
    // 性能优化
    struct {
        bool directio;                 // 直接I/O
        bool sync_writes;              // 同步写入
        uint32_t write_batch_size;     // 写入批量大小
        uint32_t prefetch_size;        // 预取大小
        bool compression;              // 数据压缩
    } optimization;
    
    // 统计信息
    storage_stats_t stats;             // 性能统计
    pthread_mutex_t stats_lock;        // 统计锁
    
} nfs_private_data_t;

// NFS后端初始化
static int nfs_init(storage_interface_t *iface, const char *config) {
    nfs_private_data_t *priv = calloc(1, sizeof(nfs_private_data_t));
    if (!priv) return -ENOMEM;
    
    // 解析配置
    if (parse_nfs_config(config, priv) != 0) {
        free(priv);
        return -EINVAL;
    }
    
    // 创建NFS上下文
    priv->nfs_ctx = nfs_init_context();
    if (!priv->nfs_ctx) {
        free(priv);
        return -ENOMEM;
    }
    
    // 设置NFS版本
    if (nfs_set_version(priv->nfs_ctx, priv->nfs_version) != 0) {
        nfs_destroy_context(priv->nfs_ctx);
        free(priv);
        return -EINVAL;
    }
    
    // 连接到NFS服务器
    if (nfs_mount(priv->nfs_ctx, priv->server_addr, priv->export_path) != 0) {
        nfs_destroy_context(priv->nfs_ctx);
        free(priv);
        return -ECONNREFUSED;
    }
    
    // 初始化连接池
    if (init_nfs_connection_pool(priv) != 0) {
        nfs_umount(priv->nfs_ctx);
        nfs_destroy_context(priv->nfs_ctx);
        free(priv);
        return -EIO;
    }
    
    // 初始化缓存
    if (priv->cache.read_cache_enabled) {
        priv->cache.read_cache = nfs_cache_create(priv->cache.cache_size);
    }
    if (priv->cache.write_cache_enabled) {
        priv->cache.write_cache = nfs_cache_create(priv->cache.cache_size);
    }
    
    // 启动异步工作线程
    if (priv->async_io.use_async) {
        init_nfs_async_workers(priv);
    }
    
    pthread_mutex_init(&priv->stats_lock, NULL);
    pthread_mutex_init(&priv->connection_pool.pool_lock, NULL);
    
    iface->private_data = priv;
    return 0;
}

// NFS同步读取
static int nfs_read_sync(storage_interface_t *iface,
                        const char *path, uint64_t offset,
                        void *buffer, size_t size) {
    nfs_private_data_t *priv = iface->private_data;
    
    uint64_t start_time = get_monotonic_time_ns();
    
    // 检查读缓存
    if (priv->cache.read_cache_enabled) {
        if (nfs_cache_get(priv->cache.read_cache, path, offset, 
                         buffer, size) == 0) {
            // 缓存命中
            pthread_mutex_lock(&priv->stats_lock);
            priv->stats.read_ops++;
            priv->stats.read_bytes += size;
            pthread_mutex_unlock(&priv->stats_lock);
            return 0;
        }
    }
    
    // 从连接池获取连接
    struct nfs_context *ctx = get_nfs_connection(priv);
    if (!ctx) return -ENODEV;
    
    // 打开文件
    struct nfsfh *fh;
    int ret = nfs_open(ctx, path, O_RDONLY, &fh);
    if (ret != 0) {
        put_nfs_connection(priv, ctx);
        return ret;
    }
    
    // 执行读取
    ssize_t bytes_read = nfs_pread(ctx, fh, offset, size, buffer);
    
    // 关闭文件
    nfs_close(ctx, fh);
    put_nfs_connection(priv, ctx);
    
    uint64_t end_time = get_monotonic_time_ns();
    
    if (bytes_read >= 0) {
        // 更新读缓存
        if (priv->cache.read_cache_enabled) {
            nfs_cache_put(priv->cache.read_cache, path, offset, 
                         buffer, bytes_read);
        }
        
        // 更新统计信息
        pthread_mutex_lock(&priv->stats_lock);
        priv->stats.read_ops++;
        priv->stats.read_bytes += bytes_read;
        update_latency_stats(&priv->stats, end_time - start_time, true);
        pthread_mutex_unlock(&priv->stats_lock);
        
        return (bytes_read == size) ? 0 : -EIO;
    }
    
    return -EIO;
}

// NFS同步写入
static int nfs_write_sync(storage_interface_t *iface,
                         const char *path, uint64_t offset,
                         const void *data, size_t size) {
    nfs_private_data_t *priv = iface->private_data;
    
    uint64_t start_time = get_monotonic_time_ns();
    
    // 检查写缓存
    if (priv->cache.write_cache_enabled && 
        size <= priv->optimization.write_batch_size) {
        // 写入缓存，延迟刷新
        if (nfs_cache_put(priv->cache.write_cache, path, offset, 
                         data, size) == 0) {
            // 异步刷新
            schedule_cache_flush(priv, path);
            return 0;
        }
    }
    
    // 从连接池获取连接
    struct nfs_context *ctx = get_nfs_connection(priv);
    if (!ctx) return -ENODEV;
    
    // 打开文件
    struct nfsfh *fh;
    int ret = nfs_open(ctx, path, O_WRONLY | O_CREAT, &fh);
    if (ret != 0) {
        put_nfs_connection(priv, ctx);
        return ret;
    }
    
    // 执行写入
    ssize_t bytes_written = nfs_pwrite(ctx, fh, offset, size, data);
    
    // 同步刷新(如果需要)
    if (priv->optimization.sync_writes) {
        nfs_fsync(ctx, fh);
    }
    
    // 关闭文件
    nfs_close(ctx, fh);
    put_nfs_connection(priv, ctx);
    
    uint64_t end_time = get_monotonic_time_ns();
    
    if (bytes_written >= 0) {
        // 更新统计信息
        pthread_mutex_lock(&priv->stats_lock);
        priv->stats.write_ops++;
        priv->stats.write_bytes += bytes_written;
        update_latency_stats(&priv->stats, end_time - start_time, false);
        pthread_mutex_unlock(&priv->stats_lock);
        
        return (bytes_written == size) ? 0 : -EIO;
    }
    
    return -EIO;
}

// NFS异步读取
static int nfs_read_async(storage_interface_t *iface,
                         const char *path, uint64_t offset,
                         void *buffer, size_t size,
                         storage_operation_ctx_t *ctx) {
    nfs_private_data_t *priv = iface->private_data;
    
    // 创建异步操作
    struct nfs_async_op *async_op = malloc(sizeof(struct nfs_async_op));
    if (!async_op) return -ENOMEM;
    
    async_op->type = NFS_ASYNC_READ;
    async_op->ctx = *ctx;
    strncpy(async_op->path, path, sizeof(async_op->path) - 1);
    async_op->offset = offset;
    async_op->buffer = buffer;
    async_op->size = size;
    async_op->start_time = get_monotonic_time_ns();
    
    // 提交到异步队列
    return nfs_async_queue_submit(priv->async_io.pending_ops, async_op);
}

// NFS批量操作
static int nfs_batch_operations(storage_interface_t *iface,
                               storage_batch_op_t *ops,
                               uint32_t count) {
    nfs_private_data_t *priv = iface->private_data;
    
    // 按操作类型分组
    storage_batch_op_t *read_ops[count];
    storage_batch_op_t *write_ops[count];
    uint32_t read_count = 0, write_count = 0;
    
    for (uint32_t i = 0; i < count; i++) {
        if (ops[i].op_type == STORAGE_BATCH_READ) {
            read_ops[read_count++] = &ops[i];
        } else {
            write_ops[write_count++] = &ops[i];
        }
    }
    
    // 并行处理读写操作
    int ret = 0;
    if (read_count > 0) {
        ret = nfs_batch_read_operations(priv, read_ops, read_count);
    }
    
    if (write_count > 0 && ret == 0) {
        ret = nfs_batch_write_operations(priv, write_ops, write_count);
    }
    
    return ret;
}

// 连接池管理
static struct nfs_context* get_nfs_connection(nfs_private_data_t *priv) {
    pthread_mutex_lock(&priv->connection_pool.pool_lock);
    
    if (priv->connection_pool.connection_count == 0) {
        pthread_mutex_unlock(&priv->connection_pool.pool_lock);
        return NULL;
    }
    
    // 轮询选择连接
    uint32_t index = priv->connection_pool.current_connection;
    priv->connection_pool.current_connection = 
        (index + 1) % priv->connection_pool.connection_count;
    
    struct nfs_context *ctx = priv->connection_pool.connections[index];
    pthread_mutex_unlock(&priv->connection_pool.pool_lock);
    
    return ctx;
}

static void put_nfs_connection(nfs_private_data_t *priv, 
                              struct nfs_context *ctx) {
    // NFS连接是无状态的，直接返回到池中
    // 实际实现中可能需要检查连接健康状态
}

// 注册NFS后端
storage_interface_t* create_nfs_backend(void) {
    storage_interface_t *iface = calloc(1, sizeof(storage_interface_t));
    if (!iface) return NULL;
    
    iface->type = STORAGE_BACKEND_NFS;
    strncpy(iface->name, "NFS", sizeof(iface->name) - 1);
    
    // 设置函数指针
    iface->init = nfs_init;
    iface->cleanup = nfs_cleanup;
    iface->health_check = nfs_health_check;
    iface->read_sync = nfs_read_sync;
    iface->write_sync = nfs_write_sync;
    iface->read_async = nfs_read_async;
    iface->write_async = nfs_write_async;
    iface->batch_operations = nfs_batch_operations;
    iface->get_stats = nfs_get_stats;
    iface->reset_stats = nfs_reset_stats;
    
    return iface;
}
```

## 2. NFS配置和优化

### 2.1 配置文件格式

```ini
[nfs]
# 服务器配置
server_addr = 192.168.1.200
export_path = /export/xdevice
mount_point = /mnt/xdevice
nfs_version = 4

# 挂载选项
rsize = 1048576          # 1MB读缓冲区
wsize = 1048576          # 1MB写缓冲区
timeo = 600              # 60秒超时
retrans = 2              # 重传2次
proto = tcp              # TCP协议
hard_mount = true        # 硬挂载
intr = true              # 可中断
ac = true                # 属性缓存

# 缓存配置
read_cache_enabled = true
write_cache_enabled = true
cache_size = 67108864    # 64MB缓存
cache_ttl = 300          # 5分钟TTL

# 异步I/O配置
use_async = true
worker_count = 4
max_pending_ops = 1000

# 连接池配置
connection_count = 8
max_connections = 16

# 性能优化
directio = false         # 不使用直接I/O
sync_writes = false      # 异步写入
write_batch_size = 65536 # 64KB批量写入
prefetch_size = 262144   # 256KB预取
compression = false      # 不压缩
```

### 2.2 性能调优

```c
// NFS性能调优结构
typedef struct {
    // 缓存优化
    size_t read_cache_size;        // 读缓存大小
    size_t write_cache_size;       // 写缓存大小
    uint32_t cache_ttl;           // 缓存TTL
    bool adaptive_cache;          // 自适应缓存
    
    // I/O优化
    uint32_t rsize;               // 读缓冲区大小
    uint32_t wsize;               // 写缓冲区大小
    uint32_t max_batch_size;      // 最大批量大小
    bool enable_readahead;        // 启用预读
    
    // 连接优化
    uint32_t connection_count;    // 连接数
    uint32_t max_connections;     // 最大连接数
    uint32_t connection_timeout;  // 连接超时
    
    // 协议优化
    bool use_rdma;               // 使用RDMA
    bool enable_delegation;      // 启用委托
    bool enable_pnfs;           // 启用pNFS
    
} nfs_tuning_params_t;

// 动态性能调优
static void nfs_auto_tuning(nfs_private_data_t *priv) {
    storage_stats_t *stats = &priv->stats;
    
    // 根据缓存命中率调整缓存大小
    double cache_hit_rate = calculate_cache_hit_rate(priv);
    if (cache_hit_rate < 0.8 && priv->cache.cache_size < MAX_CACHE_SIZE) {
        // 缓存命中率低，增加缓存大小
        priv->cache.cache_size *= 1.5;
        resize_nfs_cache(priv->cache.read_cache, priv->cache.cache_size);
    }
    
    // 根据延迟调整I/O参数
    if (stats->read_latency_avg_ns > HIGH_LATENCY_THRESHOLD) {
        // 延迟过高，增加批量大小
        priv->optimization.write_batch_size = 
            min(priv->optimization.write_batch_size * 2, MAX_BATCH_SIZE);
    }
    
    // 根据带宽利用率调整连接数
    double bandwidth_util = calculate_bandwidth_utilization(priv);
    if (bandwidth_util > 0.9 && 
        priv->connection_pool.connection_count < MAX_NFS_CONNECTIONS) {
        // 带宽利用率高，增加连接数
        add_nfs_connection(priv);
    }
}
```
