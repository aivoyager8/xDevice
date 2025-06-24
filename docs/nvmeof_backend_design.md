# NVMe-oF 存储后端实现

## 1. NVMe-oF后端设计

### 1.1 NVMe-oF接口实现

```c
#include <libnvme.h>
#include <rdma/rdma_cma.h>

// NVMe-oF私有数据结构
typedef struct {
    // NVMe连接信息
    struct nvme_ctrl *ctrl;            // NVMe控制器
    struct nvme_ns *ns;                // NVMe命名空间
    char target_nqn[256];              // Target NQN
    char host_nqn[256];                // Host NQN
    char transport[16];                // 传输类型(rdma/tcp)
    char traddr[64];                   // 传输地址
    char trsvcid[16];                  // 传输服务ID
    
    // RDMA相关(如果使用RDMA传输)
    struct rdma_cm_id *cm_id;          // RDMA连接ID
    struct ibv_pd *pd;                 // Protection Domain
    struct ibv_cq *cq;                 // Completion Queue
    struct ibv_qp *qp;                 // Queue Pair
    
    // I/O队列
    struct nvme_io_queue *io_queues[MAX_IO_QUEUES];
    uint32_t queue_count;              // 队列数量
    uint32_t queue_depth;              // 队列深度
    
    // 内存管理
    struct nvme_mem_pool *mem_pool;    // 内存池
    void *dma_buffer;                  // DMA缓冲区
    size_t dma_buffer_size;            // DMA缓冲区大小
    
    // 性能优化
    bool polling_mode;                 // 轮询模式
    uint32_t poll_interval_us;         // 轮询间隔
    bool batch_operations;             // 批量操作
    uint32_t max_batch_size;           // 最大批量大小
    
    // 统计信息
    storage_stats_t stats;             // 性能统计
    pthread_mutex_t stats_lock;        // 统计锁
    
} nvmeof_private_data_t;

// NVMe-oF后端初始化
static int nvmeof_init(storage_interface_t *iface, const char *config) {
    nvmeof_private_data_t *priv = calloc(1, sizeof(nvmeof_private_data_t));
    if (!priv) return -ENOMEM;
    
    // 解析配置
    if (parse_nvmeof_config(config, priv) != 0) {
        free(priv);
        return -EINVAL;
    }
    
    // 发现NVMe-oF目标
    struct nvme_ctrl_opts opts = {
        .transport = priv->transport,
        .traddr = priv->traddr,
        .trsvcid = priv->trsvcid,
        .host_traddr = NULL,
        .host_iface = NULL,
        .hostnqn = priv->host_nqn,
        .hostid = NULL,
        .nr_io_queues = MAX_IO_QUEUES,
        .queue_size = priv->queue_depth,
        .keep_alive_tmo = 60,
        .reconnect_delay = 5,
        .ctrl_loss_tmo = 600
    };
    
    // 连接到NVMe-oF目标
    priv->ctrl = nvme_create_ctrl(&opts, priv->target_nqn);
    if (!priv->ctrl) {
        free(priv);
        return -ECONNREFUSED;
    }
    
    // 获取命名空间
    priv->ns = nvme_ctrl_first_ns(priv->ctrl);
    if (!priv->ns) {
        nvme_free_ctrl(priv->ctrl);
        free(priv);
        return -ENODEV;
    }
    
    // 初始化I/O队列
    if (init_nvmeof_io_queues(priv) != 0) {
        nvme_free_ctrl(priv->ctrl);
        free(priv);
        return -EIO;
    }
    
    // 分配DMA缓冲区
    priv->dma_buffer_size = DMA_BUFFER_SIZE;
    priv->dma_buffer = nvme_alloc_dma_mem(priv->dma_buffer_size);
    if (!priv->dma_buffer) {
        cleanup_nvmeof_io_queues(priv);
        nvme_free_ctrl(priv->ctrl);
        free(priv);
        return -ENOMEM;
    }
    
    // 初始化内存池
    priv->mem_pool = nvme_mem_pool_create(MEM_POOL_SIZE, MEM_BLOCK_SIZE);
    
    pthread_mutex_init(&priv->stats_lock, NULL);
    
    iface->private_data = priv;
    return 0;
}

// NVMe-oF同步读取
static int nvmeof_read_sync(storage_interface_t *iface,
                           const char *path, uint64_t offset,
                           void *buffer, size_t size) {
    nvmeof_private_data_t *priv = iface->private_data;
    
    // 将文件路径转换为LBA
    uint64_t lba = path_to_lba(path, offset);
    uint32_t block_count = (size + NVME_BLOCK_SIZE - 1) / NVME_BLOCK_SIZE;
    
    uint64_t start_time = get_monotonic_time_ns();
    
    // 执行NVMe读命令
    struct nvme_passthru_cmd cmd = {
        .opcode = nvme_cmd_read,
        .nsid = nvme_ns_get_nsid(priv->ns),
        .cdw10 = lba & 0xFFFFFFFF,
        .cdw11 = (lba >> 32) & 0xFFFFFFFF,
        .cdw12 = block_count - 1,
        .addr = (uint64_t)buffer,
        .data_len = size
    };
    
    int ret = nvme_submit_passthru(nvme_ns_get_fd(priv->ns), &cmd);
    
    uint64_t end_time = get_monotonic_time_ns();
    
    // 更新统计信息
    pthread_mutex_lock(&priv->stats_lock);
    priv->stats.read_ops++;
    priv->stats.read_bytes += size;
    update_latency_stats(&priv->stats, end_time - start_time, true);
    pthread_mutex_unlock(&priv->stats_lock);
    
    return ret;
}

// NVMe-oF异步读取
static int nvmeof_read_async(storage_interface_t *iface,
                            const char *path, uint64_t offset,
                            void *buffer, size_t size,
                            storage_operation_ctx_t *ctx) {
    nvmeof_private_data_t *priv = iface->private_data;
    
    // 分配异步操作结构
    struct nvmeof_async_op *async_op = nvme_mem_pool_alloc(priv->mem_pool);
    if (!async_op) return -ENOMEM;
    
    async_op->ctx = *ctx;
    async_op->buffer = buffer;
    async_op->size = size;
    async_op->start_time = get_monotonic_time_ns();
    
    // 准备NVMe命令
    uint64_t lba = path_to_lba(path, offset);
    uint32_t block_count = (size + NVME_BLOCK_SIZE - 1) / NVME_BLOCK_SIZE;
    
    struct nvme_uring_cmd *uring_cmd = &async_op->uring_cmd;
    uring_cmd->opcode = nvme_cmd_read;
    uring_cmd->nsid = nvme_ns_get_nsid(priv->ns);
    uring_cmd->cdw10 = lba & 0xFFFFFFFF;
    uring_cmd->cdw11 = (lba >> 32) & 0xFFFFFFFF;
    uring_cmd->cdw12 = block_count - 1;
    uring_cmd->addr = (uint64_t)buffer;
    uring_cmd->data_len = size;
    
    // 提交到io_uring
    struct io_uring_sqe *sqe = io_uring_get_sqe(&priv->uring);
    io_uring_prep_uring_cmd(sqe, nvme_ns_get_fd(priv->ns), 
                           uring_cmd, sizeof(*uring_cmd));
    sqe->user_data = (uint64_t)async_op;
    
    return io_uring_submit(&priv->uring);
}

// NVMe-oF批量操作
static int nvmeof_batch_operations(storage_interface_t *iface,
                                  storage_batch_op_t *ops,
                                  uint32_t count) {
    nvmeof_private_data_t *priv = iface->private_data;
    
    if (count > priv->max_batch_size) {
        return -E2BIG;
    }
    
    // 准备批量NVMe命令
    struct nvme_uring_cmd cmds[count];
    struct nvmeof_async_op *async_ops[count];
    
    for (uint32_t i = 0; i < count; i++) {
        // 分配异步操作结构
        async_ops[i] = nvme_mem_pool_alloc(priv->mem_pool);
        if (!async_ops[i]) {
            // 清理已分配的资源
            for (uint32_t j = 0; j < i; j++) {
                nvme_mem_pool_free(priv->mem_pool, async_ops[j]);
            }
            return -ENOMEM;
        }
        
        async_ops[i]->ctx = *ops[i].ctx;
        async_ops[i]->start_time = get_monotonic_time_ns();
        
        // 准备NVMe命令
        uint64_t lba = path_to_lba(ops[i].path, ops[i].offset);
        uint32_t block_count = (ops[i].size + NVME_BLOCK_SIZE - 1) / NVME_BLOCK_SIZE;
        
        cmds[i].opcode = (ops[i].op_type == STORAGE_BATCH_READ) ? 
                        nvme_cmd_read : nvme_cmd_write;
        cmds[i].nsid = nvme_ns_get_nsid(priv->ns);
        cmds[i].cdw10 = lba & 0xFFFFFFFF;
        cmds[i].cdw11 = (lba >> 32) & 0xFFFFFFFF;
        cmds[i].cdw12 = block_count - 1;
        cmds[i].addr = (uint64_t)ops[i].data;
        cmds[i].data_len = ops[i].size;
    }
    
    // 批量提交到io_uring
    for (uint32_t i = 0; i < count; i++) {
        struct io_uring_sqe *sqe = io_uring_get_sqe(&priv->uring);
        io_uring_prep_uring_cmd(sqe, nvme_ns_get_fd(priv->ns),
                               &cmds[i], sizeof(cmds[i]));
        sqe->user_data = (uint64_t)async_ops[i];
    }
    
    return io_uring_submit(&priv->uring);
}

// 注册NVMe-oF后端
storage_interface_t* create_nvmeof_backend(void) {
    storage_interface_t *iface = calloc(1, sizeof(storage_interface_t));
    if (!iface) return NULL;
    
    iface->type = STORAGE_BACKEND_NVMEOF;
    strncpy(iface->name, "NVMe-oF", sizeof(iface->name) - 1);
    
    // 设置函数指针
    iface->init = nvmeof_init;
    iface->cleanup = nvmeof_cleanup;
    iface->health_check = nvmeof_health_check;
    iface->read_sync = nvmeof_read_sync;
    iface->write_sync = nvmeof_write_sync;
    iface->read_async = nvmeof_read_async;
    iface->write_async = nvmeof_write_async;
    iface->batch_operations = nvmeof_batch_operations;
    iface->get_stats = nvmeof_get_stats;
    iface->reset_stats = nvmeof_reset_stats;
    
    return iface;
}
```

## 2. NVMe-oF配置和优化

### 2.1 配置文件格式

```ini
[nvmeof]
# 目标配置
target_nqn = nqn.2024-06.com.example:storage01
host_nqn = nqn.2024-06.com.client:host01
transport = rdma
target_addr = 192.168.1.100
target_port = 4420

# 性能配置
io_queue_count = 8
queue_depth = 128
max_batch_size = 64
polling_mode = true
poll_interval_us = 10

# 内存配置
dma_buffer_size = 16777216    # 16MB
mem_pool_size = 67108864      # 64MB
mem_block_size = 4096         # 4KB

# RDMA配置
max_inline_data = 512
max_send_wr = 256
max_recv_wr = 256
completion_vector = -1

# 超时和重试
connect_timeout_ms = 5000
io_timeout_ms = 30000
retry_count = 3
keep_alive_interval = 60
```

### 2.2 性能调优参数

```c
// NVMe-oF性能调优结构
typedef struct {
    // I/O优化
    uint32_t io_queue_count;           // I/O队列数量
    uint32_t queue_depth;              // 队列深度
    bool polling_mode;                 // 轮询模式
    uint32_t poll_interval_us;         // 轮询间隔
    
    // 批量处理
    uint32_t max_batch_size;           // 最大批量大小
    uint32_t batch_timeout_us;         // 批量超时
    bool adaptive_batching;            // 自适应批量
    
    // 内存优化
    size_t dma_buffer_size;           // DMA缓冲区大小
    bool use_huge_pages;              // 使用大页内存
    uint32_t mem_alignment;           // 内存对齐
    
    // RDMA优化
    uint32_t max_inline_data;         // 最大内联数据
    uint32_t completion_vector;       // 完成向量
    bool use_odp;                     // 使用ODP
    
    // CPU优化
    int cpu_core;                     // 绑定CPU核心
    bool use_realtime_priority;       // 使用实时优先级
    
} nvmeof_tuning_params_t;

// 动态性能调优
static void nvmeof_auto_tuning(nvmeof_private_data_t *priv) {
    storage_stats_t *stats = &priv->stats;
    
    // 根据延迟调整轮询间隔
    if (stats->read_latency_avg_ns > TARGET_LATENCY_NS) {
        // 延迟过高，减少轮询间隔
        priv->poll_interval_us = max(priv->poll_interval_us / 2, 1);
    } else if (stats->read_latency_avg_ns < TARGET_LATENCY_NS / 2) {
        // 延迟很低，可以适当增加轮询间隔以节省CPU
        priv->poll_interval_us = min(priv->poll_interval_us * 2, 100);
    }
    
    // 根据IOPS调整批量大小
    uint64_t current_iops = stats->read_ops + stats->write_ops;
    if (current_iops > HIGH_IOPS_THRESHOLD) {
        priv->max_batch_size = min(priv->max_batch_size * 2, MAX_BATCH_SIZE);
    } else if (current_iops < LOW_IOPS_THRESHOLD) {
        priv->max_batch_size = max(priv->max_batch_size / 2, MIN_BATCH_SIZE);
    }
}
```
