# 存储引擎设计

## 0. 存储引擎架构总览

### 0.1 设计理念
xDevice存储引擎采用**分层抽象 + 插件化后端**的架构，为Raft协议提供统一、高性能的存储服务。

### 0.2 架构层次
```
┌─────────────────────────────────────────────────────────────┐
│                   Raft Layer                               │
│  ┌─────────────┐ ┌─────────────┐ ┌─────────────┐          │
│  │    日志      │ │    状态      │ │    快照      │          │
│  └─────────────┘ └─────────────┘ └─────────────┘          │
└─────────────────┬───────────────────────────────────────────┘
                  │ Raft Storage Adapter API
┌─────────────────▼───────────────────────────────────────────┐
│              Raft Storage Adapter                          │
│  ┌─────────────────────────────────────────────────────────┐ │
│  │ • 语义化接口 (append_log, save_state, save_snapshot)   │ │
│  │ • 智能路由 (日志→高速存储, 快照→大容量存储)            │ │
│  │ • 批量优化 (batch writes, async operations)           │ │
│  │ • 数据分类 (文件前缀: log_, state_, snap_, meta_)     │ │
│  └─────────────────────────────────────────────────────────┘ │
└─────────────────┬───────────────────────────────────────────┘
                  │ Storage Manager API
┌─────────────────▼───────────────────────────────────────────┐
│                Storage Manager                              │
│  ┌─────────────────────────────────────────────────────────┐ │
│  │ • 后端管理 (注册、注销、健康检查)                       │ │
│  │ • 负载均衡 (轮询、延迟优先、带宽优先)                   │ │
│  │ • 故障转移 (主备切换、自动恢复)                        │ │
│  │ • 缓存管理 (读写缓存、LRU策略)                         │ │
│  │ • 性能监控 (延迟、吞吐量、错误率)                      │ │
│  └─────────────────────────────────────────────────────────┘ │
└─────────────────┬───────────────────────────────────────────┘
                  │ Storage Interface API
┌─────────────────▼───────────────────────────────────────────┐
│               Storage Backends                              │
│  ┌─────────────┐ ┌─────────────┐ ┌─────────────┐          │
│  │  NVMe-oF    │ │     NFS     │ │    Local    │          │
│  │  Backend    │ │   Backend   │ │   Backend   │          │
│  │             │ │             │ │             │          │
│  │ • RDMA优化  │ │ • 标准兼容  │ │ • 多模式   │          │
│  │ • 批量I/O   │ │ • 缓存策略  │ │ • 文件优化  │          │
│  │ • 低延迟    │ │ • 负载均衡  │ │ • 测试友好  │          │
│  └─────────────┘ └─────────────┘ └─────────────┘          │
└─────────────────────────────────────────────────────────────┘
```

### 0.3 关键设计原则

#### 0.3.1 统一抽象
**原则**：所有存储后端必须实现相同的接口，保证上层业务逻辑的一致性
```c
typedef struct storage_interface {
    // 统一的I/O接口
    int (*read_sync)(struct storage_interface *iface, 
                    const char *path, uint64_t offset,
                    void *buffer, size_t size);
    int (*write_sync)(struct storage_interface *iface,
                     const char *path, uint64_t offset, 
                     const void *data, size_t size);
    // ... 其他统一接口
} storage_interface_t;
```

#### 0.3.2 性能优先
**原则**：为WAL场景优化，重点优化顺序写入性能
- **批量写入**：合并小的写入操作
- **异步I/O**：减少阻塞等待时间
- **内存映射**：零拷贝数据传输
- **预分配**：避免动态扩展开销

#### 0.3.3 故障容错
**原则**：系统能够自动处理存储后端故障
- **健康检查**：定期检测后端状态
- **自动切换**：主备存储自动故障转移
- **优雅降级**：部分后端故障不影响整体服务

#### 0.3.4 可观测性
**原则**：提供丰富的监控和调试信息
- **性能指标**：延迟、吞吐量、IOPS
- **错误统计**：失败率、超时率、重试次数
- **资源使用**：内存、文件描述符、网络连接

### 0.4 数据流向图
```
Raft请求 → Raft Adapter → Storage Manager → Backend Selection → Storage Backend
    ↓           ↓              ↓                  ↓                    ↓
append_log → log_前缀路径 → 选择高速后端 → NVMe-oF Backend → 物理写入
save_state → state_前缀路径 → 选择可靠后端 → NFS Backend → 网络写入  
save_snap → snap_前缀路径 → 选择大容量后端 → Local Backend → 本地写入
```

# 存储引擎设计

## 0. 三大存储后端架构概述

xDevice分布式块设备支持三种主要的存储后端类型，每种都有其特定的适用场景和性能特点：

### 0.1 三大存储后端类型

```c
// 存储后端类型
typedef enum {
    STORAGE_BACKEND_NVMEOF = 1,    // NVMe-oF远程存储 - 高性能网络块存储
    STORAGE_BACKEND_NFS = 2,       // NFS网络文件系统 - 标准网络文件存储
    STORAGE_BACKEND_LOCAL = 3,     // 本地文件存储 - 多模式本地存储
    STORAGE_BACKEND_MOCK = 4       // Mock存储(单元测试)
} storage_backend_type_t;
```

### 0.2 三大后端对比

| 存储后端 | 性能特点 | 适用场景 | 网络要求 | 复杂度 |
|---------|---------|---------|---------|--------|
| **NVMe-oF** | 极高性能，低延迟(微秒级) | 高性能WAL，实时系统 | 高速网络(RDMA) | 高 |
| **NFS** | 中等性能，标准兼容 | 通用WAL，跨平台部署 | 标准以太网 | 中 |
| **Local** | 灵活性能，多种模式 | 开发测试，单机部署 | 无网络要求 | 低 |

### 0.3 统一存储接口抽象

为了统一对接三种不同存储后端，设计通用的存储抽象层：

// 存储操作状态
typedef enum {
    STORAGE_OP_SUCCESS = 0,
    STORAGE_OP_PENDING = 1,        // 异步操作进行中
    STORAGE_OP_ERROR = -1,         // 操作失败
    STORAGE_OP_TIMEOUT = -2,       // 操作超时
    STORAGE_OP_NOT_FOUND = -3,     // 文件不存在
    STORAGE_OP_NO_SPACE = -4       // 存储空间不足
} storage_op_status_t;

// 异步I/O回调函数
typedef void (*storage_callback_t)(void *context, 
                                  storage_op_status_t status,
                                  void *data, size_t size);

// 存储操作上下文
typedef struct {
    uint64_t operation_id;         // 操作ID
    void *user_context;            // 用户上下文
    storage_callback_t callback;   // 完成回调
    uint64_t start_time_ns;        // 开始时间
    uint64_t timeout_ns;           // 超时时间
} storage_operation_ctx_t;

// 统一存储接口
typedef struct storage_interface {
    // 基本信息
    storage_backend_type_t type;   // 后端类型
    char name[64];                 // 存储名称
    void *private_data;            // 私有数据
    
    // 生命周期管理
    int (*init)(struct storage_interface *iface, const char *config);
    int (*cleanup)(struct storage_interface *iface);
    int (*health_check)(struct storage_interface *iface);
    
    // 同步I/O接口
    int (*read_sync)(struct storage_interface *iface,
                    const char *path, uint64_t offset,
                    void *buffer, size_t size);
    
    int (*write_sync)(struct storage_interface *iface,
                     const char *path, uint64_t offset,
                     const void *data, size_t size);
    
    int (*fsync)(struct storage_interface *iface, const char *path);
    
    // 异步I/O接口
    int (*read_async)(struct storage_interface *iface,
                     const char *path, uint64_t offset,
                     void *buffer, size_t size,
                     storage_operation_ctx_t *ctx);
    
    int (*write_async)(struct storage_interface *iface,
                      const char *path, uint64_t offset,
                      const void *data, size_t size,
                      storage_operation_ctx_t *ctx);
    
    // 文件管理
    int (*create_file)(struct storage_interface *iface,
                      const char *path, uint64_t initial_size);
    
    int (*delete_file)(struct storage_interface *iface, const char *path);
    
    int (*get_file_info)(struct storage_interface *iface,
                        const char *path, 
                        struct storage_file_info *info);
    
    // 批量操作
    int (*batch_operations)(struct storage_interface *iface,
                           struct storage_batch_op *ops,
                           uint32_t count);
    
    // 性能和统计
    int (*get_stats)(struct storage_interface *iface,
                    struct storage_stats *stats);
    
    int (*reset_stats)(struct storage_interface *iface);
    
} storage_interface_t;

// 存储文件信息
typedef struct {
    uint64_t size;                 // 文件大小
    uint64_t created_time;         // 创建时间
    uint64_t modified_time;        // 修改时间
    uint32_t permissions;          // 权限
    bool is_directory;             // 是否为目录
} storage_file_info_t;

// 批量操作定义
typedef struct {
    enum {
        STORAGE_BATCH_READ,
        STORAGE_BATCH_WRITE,
        STORAGE_BATCH_FSYNC
    } op_type;
    
    char path[256];                // 文件路径
    uint64_t offset;               // 偏移量
    void *data;                    // 数据指针
    size_t size;                   // 数据大小
    storage_operation_ctx_t *ctx;  // 操作上下文
} storage_batch_op_t;

// 存储统计信息
typedef struct {
    uint64_t read_ops;             // 读操作数
    uint64_t write_ops;            // 写操作数
    uint64_t read_bytes;           // 读字节数
    uint64_t write_bytes;          // 写字节数
    uint64_t read_latency_avg_ns;  // 平均读延迟
    uint64_t write_latency_avg_ns; // 平均写延迟
    uint64_t read_latency_p99_ns;  // P99读延迟
    uint64_t write_latency_p99_ns; // P99写延迟
    uint64_t error_count;          // 错误次数
    uint64_t timeout_count;        // 超时次数
} storage_stats_t;
```

### 0.2 存储管理器设计

```c
// 存储管理器
typedef struct {
    storage_interface_t *backends[MAX_STORAGE_BACKENDS]; // 存储后端数组
    uint32_t backend_count;                             // 后端数量
    
    // 路由策略
    struct {
        storage_backend_type_t primary_backend;         // 主要后端
        storage_backend_type_t fallback_backend;        // 备用后端
        bool auto_failover;                             // 自动故障转移
        uint32_t failover_timeout_ms;                   // 故障转移超时
    } routing_policy;
    
    // 负载均衡
    struct {
        bool enabled;                                   // 是否启用
        enum {
            LOAD_BALANCE_ROUND_ROBIN,                   // 轮询
            LOAD_BALANCE_LATENCY,                       // 延迟优先
            LOAD_BALANCE_BANDWIDTH                      // 带宽优先
        } algorithm;
        uint32_t current_backend;                       // 当前后端索引
    } load_balancer;
    
    // 缓存策略
    struct {
        bool read_cache_enabled;                        // 读缓存
        bool write_cache_enabled;                       // 写缓存
        size_t cache_size;                             // 缓存大小
        uint32_t cache_ttl_seconds;                    // 缓存TTL
    } cache_policy;
    
    // 性能监控
    storage_stats_t global_stats;                      // 全局统计
    pthread_mutex_t stats_lock;                        // 统计锁
    
} storage_manager_t;

// 存储管理器接口
int storage_manager_init(storage_manager_t *manager);
int storage_manager_cleanup(storage_manager_t *manager);

int storage_manager_register_backend(storage_manager_t *manager,
                                    storage_interface_t *backend);

int storage_manager_unregister_backend(storage_manager_t *manager,
                                      storage_backend_type_t type);

## 1. 数据一致性和持久化保证

### 1.1 ACID属性在存储层的实现

#### 1.1.1 原子性 (Atomicity)
```c
// 原子写入操作 - 要么全部成功，要么全部失败
typedef struct {
    char path[MAX_PATH_LEN];
    uint64_t offset;
    void *data;
    size_t size;
    uint32_t checksum;              // 数据校验和
} atomic_write_request_t;

// 原子写入实现
int storage_atomic_write(storage_manager_t *manager,
                        atomic_write_request_t *request) {
    // 1. 预检查：验证参数和空间
    if (!validate_write_request(request)) {
        return STORAGE_ERROR_INVALID_PARAM;
    }
    
    // 2. 计算校验和
    uint32_t calculated_checksum = crc32(request->data, request->size);
    if (calculated_checksum != request->checksum) {
        return STORAGE_ERROR_CHECKSUM_MISMATCH;
    }
    
    // 3. 原子写入（使用临时文件+重命名）
    char temp_path[MAX_PATH_LEN];
    snprintf(temp_path, sizeof(temp_path), "%s.tmp.%lu", 
             request->path, get_current_time_ns());
    
    // 写入临时文件
    int ret = storage_write_sync(manager, temp_path, 0, 
                                request->data, request->size);
    if (ret != STORAGE_OP_SUCCESS) {
        return ret;
    }
    
    // 强制刷盘
    ret = storage_fsync(manager, temp_path);
    if (ret != STORAGE_OP_SUCCESS) {
        storage_delete_file(manager, temp_path);
        return ret;
    }
    
    // 原子重命名
    ret = storage_rename_file(manager, temp_path, request->path);
    if (ret != STORAGE_OP_SUCCESS) {
        storage_delete_file(manager, temp_path);
        return ret;
    }
    
    return STORAGE_OP_SUCCESS;
}
```

#### 1.1.2 一致性 (Consistency)
```c
// 多文件一致性写入（事务性写入多个相关文件）
typedef struct {
    uint32_t operation_count;
    atomic_write_request_t operations[MAX_BATCH_OPERATIONS];
    uint64_t transaction_id;        // 事务ID
} consistent_batch_write_t;

int storage_consistent_batch_write(storage_manager_t *manager,
                                  consistent_batch_write_t *batch) {
    char transaction_log_path[MAX_PATH_LEN];
    snprintf(transaction_log_path, sizeof(transaction_log_path),
             "transaction_%lu.log", batch->transaction_id);
    
    // 1. 写入事务日志（预写日志）
    transaction_log_entry_t log_entry = {
        .transaction_id = batch->transaction_id,
        .operation_count = batch->operation_count,
        .timestamp = get_current_time_ns(),
        .status = TRANSACTION_STATUS_STARTED
    };
    
    int ret = storage_write_sync(manager, transaction_log_path, 0,
                                &log_entry, sizeof(log_entry));
    if (ret != STORAGE_OP_SUCCESS) {
        return ret;
    }
    
    // 2. 执行所有写入操作
    for (uint32_t i = 0; i < batch->operation_count; i++) {
        ret = storage_atomic_write(manager, &batch->operations[i]);
        if (ret != STORAGE_OP_SUCCESS) {
            // 回滚已执行的操作
            rollback_operations(manager, batch, i);
            
            // 标记事务失败
            log_entry.status = TRANSACTION_STATUS_FAILED;
            storage_write_sync(manager, transaction_log_path, 0,
                              &log_entry, sizeof(log_entry));
            return ret;
        }
    }
    
    // 3. 标记事务成功
    log_entry.status = TRANSACTION_STATUS_COMMITTED;
    ret = storage_write_sync(manager, transaction_log_path, 0,
                            &log_entry, sizeof(log_entry));
    
    // 4. 清理事务日志
    storage_delete_file(manager, transaction_log_path);
    
    return STORAGE_OP_SUCCESS;
}
```

#### 1.1.3 隔离性 (Isolation)
```c
// 读写锁机制保证隔离性
typedef struct {
    char path[MAX_PATH_LEN];
    pthread_rwlock_t lock;          // 读写锁
    uint32_t reader_count;          // 读者数量
    uint32_t writer_count;          // 写者数量
    uint64_t last_access_time;      // 最后访问时间
} file_lock_entry_t;

typedef struct {
    file_lock_entry_t *entries;     // 锁条目数组
    uint32_t capacity;              // 容量
    uint32_t count;                 // 当前数量
    pthread_mutex_t table_lock;     // 表级锁
} file_lock_table_t;

// 获取文件读锁
int acquire_file_read_lock(file_lock_table_t *table, const char *path) {
    pthread_mutex_lock(&table->table_lock);
    
    file_lock_entry_t *entry = find_or_create_lock_entry(table, path);
    if (!entry) {
        pthread_mutex_unlock(&table->table_lock);
        return STORAGE_ERROR_OUT_OF_MEMORY;
    }
    
    pthread_mutex_unlock(&table->table_lock);
    
    // 获取读锁
    int ret = pthread_rwlock_rdlock(&entry->lock);
    if (ret == 0) {
        __atomic_fetch_add(&entry->reader_count, 1, __ATOMIC_SEQ_CST);
        entry->last_access_time = get_current_time_ns();
    }
    
    return (ret == 0) ? STORAGE_OP_SUCCESS : STORAGE_ERROR_LOCK_FAILED;
}

// 获取文件写锁
int acquire_file_write_lock(file_lock_table_t *table, const char *path) {
    pthread_mutex_lock(&table->table_lock);
    
    file_lock_entry_t *entry = find_or_create_lock_entry(table, path);
    if (!entry) {
        pthread_mutex_unlock(&table->table_lock);
        return STORAGE_ERROR_OUT_OF_MEMORY;
    }
    
    pthread_mutex_unlock(&table->table_lock);
    
    // 获取写锁
    int ret = pthread_rwlock_wrlock(&entry->lock);
    if (ret == 0) {
        entry->writer_count = 1;
        entry->last_access_time = get_current_time_ns();
    }
    
    return (ret == 0) ? STORAGE_OP_SUCCESS : STORAGE_ERROR_LOCK_FAILED;
}
```

#### 1.1.4 持久性 (Durability)
```c
// 多层持久性保证
typedef enum {
    DURABILITY_NONE = 0,            // 无持久性保证（最快)
    DURABILITY_BUFFER_SYNC,         // 缓冲区同步
    DURABILITY_FILE_SYNC,           // 文件同步
    DURABILITY_DEVICE_SYNC,         // 设备级同步（最安全)
} durability_level_t;

// 分级持久性写入
int storage_durable_write(storage_manager_t *manager,
                         const char *path, uint64_t offset,
                         const void *data, size_t size,
                         durability_level_t level) {
    int ret = storage_write_sync(manager, path, offset, data, size);
    if (ret != STORAGE_OP_SUCCESS) {
        return ret;
    }
    
    switch (level) {
        case DURABILITY_BUFFER_SYNC:
            // 只保证内核缓冲区同步
            ret = storage_buffer_sync(manager, path);
            break;
            
        case DURABILITY_FILE_SYNC:
            // 保证文件数据同步到存储设备
            ret = storage_fsync(manager, path);
            break;
            
        case DURABILITY_DEVICE_SYNC:
            // 保证存储设备缓存也刷新
            ret = storage_device_sync(manager, path);
            break;
            
        case DURABILITY_NONE:
        default:
            // 无额外同步操作
            break;
    }
    
    return ret;
}

// 写屏障 - 保证顺序持久性
int storage_write_barrier(storage_manager_t *manager) {
    // 强制所有pending的写入操作完成
    for (uint32_t i = 0; i < manager->backend_count; i++) {
        storage_interface_t *backend = manager->backends[i];
        if (backend && backend->barrier) {
            int ret = backend->barrier(backend);
            if (ret != STORAGE_OP_SUCCESS) {
                return ret;
            }
        }
    }
    
    return STORAGE_OP_SUCCESS;
}
```

### 1.2 Raft协议的存储一致性保证

#### 1.2.1 日志顺序一致性
```c
// Raft日志追加的存储保证
int raft_storage_append_log_safe(raft_storage_adapter_t *adapter,
                                 uint64_t log_index,
                                 const void *entry_data, size_t entry_size) {
    // 1. 检查日志连续性
    if (log_index > 1) {
        uint64_t prev_index = log_index - 1;
        if (!raft_storage_log_exists(adapter, prev_index)) {
            return RAFT_ERROR_LOG_GAP;
        }
    }
    
    // 2. 使用原子写入保证一致性
    atomic_write_request_t request = {
        .offset = 0,
        .data = (void*)entry_data,
        .size = entry_size,
        .checksum = crc32(entry_data, entry_size)
    };
    
    snprintf(request.path, sizeof(request.path), 
             "%s%s%lu", adapter->device_prefix, adapter->log_prefix, log_index);
    
    int ret = storage_atomic_write(adapter->storage_manager, &request);
    if (ret != STORAGE_OP_SUCCESS) {
        return ret;
    }
    
    // 3. 强制持久化（对于领导者日志）
    if (adapter->durability_level >= DURABILITY_FILE_SYNC) {
        ret = storage_durable_write(adapter->storage_manager, request.path, 0,
                                   entry_data, entry_size, adapter->durability_level);
    }
    
    return ret;
}
```

#### 1.2.2 状态机一致性
```c
// Raft状态保存的一致性保证
int raft_storage_save_state_consistent(raft_storage_adapter_t *adapter,
                                      raft_persistent_state_t *state) {
    // 准备批量一致性写入
    consistent_batch_write_t batch = {
        .operation_count = 3,
        .transaction_id = generate_transaction_id()
    };
    
    // 1. 当前任期
    char term_data[32];
    snprintf(term_data, sizeof(term_data), "%u", state->current_term);
    setup_write_operation(&batch.operations[0], 
                          adapter, "current_term", 
                          term_data, strlen(term_data));
    
    // 2. 投票记录
    char vote_data[32];
    snprintf(vote_data, sizeof(vote_data), "%u", state->voted_for);
    setup_write_operation(&batch.operations[1], 
                          adapter, "voted_for", 
                          vote_data, strlen(vote_data));
    
    // 3. 提交索引
    char commit_data[32];
    snprintf(commit_data, sizeof(commit_data), "%lu", state->commit_index);
    setup_write_operation(&batch.operations[2], 
                          adapter, "commit_index", 
                          commit_data, strlen(commit_data));
    
    // 执行一致性批量写入
    return storage_consistent_batch_write(adapter->storage_manager, &batch);
}
```

## 1. 存储架构概述

### 1.1 分层存储架构

```
┌─────────────────────────────────────┐
│            Block Device API         │ ← 块设备接口层
├─────────────────────────────────────┤
│         Cache & Buffer Layer        │ ← 缓存和缓冲层
│  ┌─────────────┐ ┌─────────────┐    │
│  │ Read Cache  │ │Write Buffer │    │
│  └─────────────┘ └─────────────┘    │
├─────────────────────────────────────┤
│        Storage Engine Core          │ ← 存储引擎核心
│  ┌─────────────┐ ┌─────────────┐    │
│  │ WAL Engine  │ │ Data Engine │    │
│  └─────────────┘ └─────────────┘    │
├─────────────────────────────────────┤
│         File System Layer           │ ← 文件系统层
│  ┌─────────────┐ ┌─────────────┐    │
│  │ Meta Store  │ │ Data Store  │    │
│  └─────────────┘ └─────────────┘    │
├─────────────────────────────────────┤
│        Operating System             │ ← 操作系统层
└─────────────────────────────────────┘
```

### 1.2 核心组件

- **WAL引擎**：高性能写前日志
- **数据引擎**：实际数据存储
- **缓存层**：内存缓存优化
- **元数据存储**：设备和索引信息
- **数据存储**：原始数据文件

## 2. 数据文件组织

### 2.1 文件布局

```
data/
├── metadata/
│   ├── devices.db          # 设备元数据数据库
│   ├── index.db           # 索引数据库
│   └── config.db          # 配置数据库
├── wal/
│   ├── wal-000001.log     # WAL段文件
│   ├── wal-000002.log
│   └── wal-current        # 当前活跃WAL
├── data/
│   ├── device-001/        # 设备数据目录
│   │   ├── data-000001.sst # 数据文件
│   │   ├── data-000002.sst
│   │   └── MANIFEST       # 清单文件
│   └── device-002/
└── snapshots/
    ├── snap-000001.snap   # 快照文件
    └── snap-000002.snap
```

### 2.2 数据文件格式 (SSTable-like)

```c
typedef struct {
    uint32_t magic;          // 0x53535444 ("SSTD")
    uint32_t version;        // 文件格式版本
    uint64_t file_size;      // 文件大小
    uint64_t data_size;      // 数据大小
    uint32_t block_count;    // 数据块数量
    uint32_t index_offset;   // 索引偏移
    uint32_t index_size;     // 索引大小
    uint32_t filter_offset;  // 布隆过滤器偏移
    uint32_t filter_size;    // 布隆过滤器大小
    uint64_t min_key;        // 最小键
    uint64_t max_key;        // 最大键
    uint32_t compression;    // 压缩算法
    uint32_t checksum_type;  // 校验和类型
    uint64_t created_time;   // 创建时间
    char     reserved[64];   // 保留字段
} __attribute__((packed)) sst_header_t;
```

### 2.3 数据块格式

```c
typedef struct {
    uint32_t block_size;     // 块大小
    uint32_t entry_count;    // 条目数量
    uint32_t checksum;       // 校验和
    uint32_t compression;    // 压缩类型
    char     data[];         // 实际数据
} __attribute__((packed)) data_block_t;

typedef struct {
    uint64_t key;            // 键(offset)
    uint32_t value_offset;   // 值偏移
    uint32_t value_size;     // 值大小
} __attribute__((packed)) data_entry_t;
```

## 3. 缓存管理

### 3.1 多级缓存架构

```c
typedef struct {
    // L1 缓存 - 最近访问的热数据
    struct {
        size_t   capacity;    // 容量
        size_t   size;        // 当前大小
        hash_table_t *table;  // 哈希表
        lru_list_t *lru;      // LRU链表
    } l1_cache;
    
    // L2 缓存 - 较大的暖数据缓存
    struct {
        size_t   capacity;
        size_t   size;
        hash_table_t *table;
        lru_list_t *lru;
    } l2_cache;
    
    // 写缓存 - 缓冲待写入数据
    struct {
        size_t   capacity;
        size_t   size;
        ring_buffer_t *buffer;
        pthread_mutex_t mutex;
    } write_cache;
    
    // 统计信息
    struct {
        uint64_t l1_hits;
        uint64_t l1_misses;
        uint64_t l2_hits;
        uint64_t l2_misses;
        uint64_t write_hits;
        uint64_t evictions;
    } stats;
} cache_manager_t;
```

### 3.2 缓存策略

```c
// LRU-K算法实现
typedef struct lru_k_entry {
    uint64_t key;
    void    *value;
    size_t   size;
    uint64_t access_times[LRU_K];  // 最近K次访问时间
    uint32_t access_count;         // 访问次数
    struct lru_k_entry *next;
    struct lru_k_entry *prev;
} lru_k_entry_t;

// 自适应替换算法 (ARC)
typedef struct {
    lru_list_t *t1;     // 最近访问的页面
    lru_list_t *t2;     // 频繁访问的页面
    lru_list_t *b1;     // T1的历史记录
    lru_list_t *b2;     // T2的历史记录
    size_t p;           // 自适应参数
    size_t c;           // 缓存容量
} arc_cache_t;
```

### 3.3 缓存预取策略

```c
typedef struct {
    bool     enabled;         // 是否启用预取
    uint32_t window_size;     // 预取窗口大小
    uint32_t max_distance;    // 最大预取距离
    double   hit_rate_threshold; // 命中率阈值
    uint32_t prefetch_degree; // 预取度数
} prefetch_config_t;

// 顺序预取
int sequential_prefetch(cache_manager_t *cache, 
                       uint64_t current_offset,
                       uint32_t size) {
    uint64_t next_offset = current_offset + size;
    uint32_t prefetch_size = cache->config.prefetch_size;
    
    // 检查是否已经在缓存中
    if (!cache_contains(cache, next_offset)) {
        // 异步预取
        return async_read_ahead(cache, next_offset, prefetch_size);
    }
    
    return 0;
}

// 基于历史模式的预取
typedef struct {
    uint64_t pattern[MAX_PATTERN_LENGTH]; // 访问模式
    uint32_t pattern_length;              // 模式长度
    uint32_t confidence;                  // 置信度
} access_pattern_t;
```

## 4. 压缩和合并

### 4.1 数据压缩

```c
typedef enum {
    COMPRESS_NONE = 0,        // 无压缩
    COMPRESS_LZ4 = 1,         // LZ4 (快速)
    COMPRESS_ZSTD = 2,        // ZSTD (高压缩比)
    COMPRESS_SNAPPY = 3,      // Snappy (平衡)
    COMPRESS_GZIP = 4         // GZIP (最高压缩比)
} compression_type_t;

typedef struct {
    compression_type_t type;  // 压缩类型
    uint32_t level;           // 压缩级别
    size_t   min_block_size;  // 最小压缩块大小
    double   ratio_threshold; // 压缩比阈值
    bool     adaptive;        // 自适应压缩
} compression_config_t;

// 压缩接口
int compress_block(compression_config_t *config,
                  const void *input, size_t input_size,
                  void *output, size_t *output_size) {
    switch (config->type) {
        case COMPRESS_LZ4:
            return lz4_compress(input, input_size, output, output_size, 
                               config->level);
        case COMPRESS_ZSTD:
            return zstd_compress(input, input_size, output, output_size,
                                config->level);
        default:
            return -1;
    }
}
```

### 4.2 LSM-Tree样式的合并

```c
typedef struct {
    uint32_t level;           // 层级
    uint32_t max_files;       // 最大文件数
    uint64_t max_size;        // 最大层级大小
    double   size_ratio;      // 大小比率
    uint32_t merge_threshold; // 合并阈值
} level_config_t;

typedef struct {
    level_config_t levels[MAX_LEVELS]; // 各层配置
    uint32_t max_levels;               // 最大层数
    bool     size_tiered;              // 大小分层
    bool     leveled;                  // 层级合并
} compaction_config_t;

// 合并触发条件
bool need_compaction(storage_engine_t *engine, uint32_t level) {
    level_stats_t *stats = &engine->level_stats[level];
    level_config_t *config = &engine->compaction_config.levels[level];
    
    // 检查文件数量
    if (stats->file_count >= config->max_files) {
        return true;
    }
    
    // 检查总大小
    if (stats->total_size >= config->max_size) {
        return true;
    }
    
    return false;
}
```

## 5. 索引管理

### 5.1 B+树索引

```c
typedef struct btree_node {
    bool     is_leaf;         // 是否叶子节点
    uint16_t key_count;       // 键数量
    uint16_t max_keys;        // 最大键数
    uint64_t keys[MAX_KEYS];  // 键数组
    
    union {
        struct btree_node *children[MAX_KEYS + 1]; // 内部节点
        data_pointer_t values[MAX_KEYS];           // 叶子节点
    };
    
    struct btree_node *next;  // 叶子节点链表
} btree_node_t;

typedef struct {
    uint64_t offset;          // 文件偏移
    uint32_t size;            // 数据大小
    uint16_t file_id;         // 文件ID
    uint16_t flags;           // 标志位
} data_pointer_t;
```

### 5.2 布隆过滤器

```c
typedef struct {
    uint8_t  *bits;           // 位数组
    size_t    size;           // 位数组大小
    uint32_t  hash_functions; // 哈希函数数量
    uint64_t  inserted_elements; // 已插入元素数
    double    false_positive_rate; // 假阳性率
} bloom_filter_t;

// 布隆过滤器操作
void bloom_filter_add(bloom_filter_t *filter, uint64_t key) {
    for (uint32_t i = 0; i < filter->hash_functions; i++) {
        uint64_t hash = hash_function(key, i);
        uint64_t bit_index = hash % (filter->size * 8);
        filter->bits[bit_index / 8] |= (1 << (bit_index % 8));
    }
}

bool bloom_filter_might_contain(bloom_filter_t *filter, uint64_t key) {
    for (uint32_t i = 0; i < filter->hash_functions; i++) {
        uint64_t hash = hash_function(key, i);
        uint64_t bit_index = hash % (filter->size * 8);
        if (!(filter->bits[bit_index / 8] & (1 << (bit_index % 8)))) {
            return false;
        }
    }
    return true;
}
```

## 6. 事务支持

### 6.1 事务结构

```c
typedef struct {
    uint64_t txn_id;          // 事务ID
    uint64_t start_time;      // 开始时间
    uint64_t commit_time;     // 提交时间
    txn_state_t state;        // 事务状态
    
    // 修改集合
    hash_table_t *write_set;  // 写集合
    hash_table_t *read_set;   // 读集合(可选)
    
    // WAL相关
    uint64_t first_lsn;       // 第一个LSN
    uint64_t last_lsn;        // 最后一个LSN
    
    // 锁信息
    lock_table_t *locks;      // 持有的锁
} transaction_t;

typedef enum {
    TXN_ACTIVE = 0,           // 活跃
    TXN_PREPARING = 1,        // 准备中
    TXN_COMMITTED = 2,        // 已提交
    TXN_ABORTED = 3           // 已中止
} txn_state_t;
```

### 6.2 MVCC (多版本并发控制)

```c
typedef struct {
    uint64_t key;             // 键
    uint64_t version;         // 版本号
    uint64_t txn_id;          // 创建事务ID
    uint64_t commit_time;     // 提交时间
    void    *value;           // 值
    size_t   value_size;      // 值大小
    struct version_chain *next; // 版本链
} version_chain_t;

// 版本可见性检查
bool is_version_visible(version_chain_t *version, 
                       transaction_t *txn,
                       uint64_t read_timestamp) {
    // 检查版本是否在事务开始时间之前提交
    if (version->commit_time <= read_timestamp) {
        return true;
    }
    
    // 检查是否是自己的写入
    if (version->txn_id == txn->txn_id) {
        return true;
    }
    
    return false;
}
```

## 7. 故障恢复

### 7.1 检查点机制

```c
typedef struct {
    uint64_t checkpoint_lsn;  // 检查点LSN
    uint64_t timestamp;       // 时间戳
    uint32_t active_txn_count; // 活跃事务数
    uint64_t *active_txns;    // 活跃事务列表
    uint64_t dirty_page_count; // 脏页数
    page_id_t *dirty_pages;   // 脏页列表
} checkpoint_record_t;

// 创建检查点
int create_checkpoint(storage_engine_t *engine) {
    checkpoint_record_t checkpoint = {0};
    
    // 获取当前LSN
    checkpoint.checkpoint_lsn = wal_get_current_lsn(engine->wal);
    checkpoint.timestamp = get_current_time_us();
    
    // 收集活跃事务
    collect_active_transactions(engine, &checkpoint);
    
    // 收集脏页
    collect_dirty_pages(engine, &checkpoint);
    
    // 写入检查点记录
    return wal_write_checkpoint(engine->wal, &checkpoint);
}
```

### 7.2 恢复算法 (ARIES)

```c
// 三阶段恢复算法
int perform_recovery(storage_engine_t *engine) {
    uint64_t checkpoint_lsn = find_last_checkpoint(engine);
    
    // 阶段1: 分析阶段
    recovery_analysis_phase(engine, checkpoint_lsn);
    
    // 阶段2: 重做阶段
    recovery_redo_phase(engine, checkpoint_lsn);
    
    // 阶段3: 撤销阶段
    recovery_undo_phase(engine);
    
    return 0;
}

// 分析阶段 - 确定重做起点和撤销列表
void recovery_analysis_phase(storage_engine_t *engine, 
                            uint64_t checkpoint_lsn) {
    // 从检查点开始扫描WAL
    uint64_t current_lsn = checkpoint_lsn;
    
    while (current_lsn < wal_get_current_lsn(engine->wal)) {
        wal_entry_t *entry = wal_read_entry(engine->wal, current_lsn);
        
        switch (entry->type) {
            case WAL_TXN_BEGIN:
                add_to_active_txn_table(engine, entry->txn_id);
                break;
                
            case WAL_TXN_COMMIT:
                remove_from_active_txn_table(engine, entry->txn_id);
                break;
                
            case WAL_DATA_UPDATE:
                add_to_dirty_page_table(engine, entry->page_id, current_lsn);
                break;
        }
        
        current_lsn = get_next_lsn(current_lsn);
    }
}
```

## 1. 三大存储后端详细架构

### 1.1 NVMe-oF后端 - 高性能远程块存储

```c
// NVMe-oF后端特性
typedef struct nvmeof_backend_config {
    // 连接配置
    char target_nqn[256];          // 目标NQN
    char transport_address[64];    // 传输地址
    char transport_service[16];    // 传输服务端口
    char transport_type[16];       // 传输类型(rdma/tcp/fc)
    
    // 性能配置
    uint32_t queue_depth;          // 队列深度(512-4096)
    uint32_t num_queues;           // 队列数量
    uint32_t buffer_size;          // 缓冲区大小
    bool enable_polling;           // 轮询模式
    
    // 可靠性配置
    uint32_t reconnect_delay_ms;   // 重连延迟
    uint32_t max_reconnects;       // 最大重连次数
    bool enable_multipath;         // 多路径支持
    
    // 优化配置
    bool enable_write_cache;       // 写缓存
    bool enable_fua;              // 强制单元访问
    uint32_t io_timeout_sec;      // I/O超时
} nvmeof_backend_config_t;

// 创建NVMe-oF后端
storage_interface_t* create_nvmeof_backend(void) {
    storage_interface_t *iface = calloc(1, sizeof(storage_interface_t));
    iface->type = STORAGE_BACKEND_NVMEOF;
    strncpy(iface->name, "NVMe-oF Remote Storage", sizeof(iface->name) - 1);
    
    // 设置NVMe-oF特有的函数指针
    iface->init = nvmeof_init;
    iface->cleanup = nvmeof_cleanup;
    iface->read_sync = nvmeof_read_sync;
    iface->write_sync = nvmeof_write_sync;
    iface->read_async = nvmeof_read_async;
    iface->write_async = nvmeof_write_async;
    
    return iface;
}
```

**性能特点**：
- 延迟：1-10微秒
- IOPS：100K-1M+
- 带宽：10-100GB/s
- 适合：高频WAL写入，实时系统

### 1.2 NFS后端 - 标准网络文件存储

```c
// NFS后端特性
typedef struct nfs_backend_config {
    // 挂载配置
    char server_address[64];       // NFS服务器地址
    char export_path[256];         // 导出路径
    char mount_point[256];         // 挂载点
    char nfs_version[8];          // NFS版本(v3/v4/v4.1)
    
    // 性能配置
    uint32_t rsize;               // 读取大小
    uint32_t wsize;               // 写入大小
    uint32_t timeo;               // 超时时间
    uint32_t retrans;             // 重传次数
    
    // 缓存配置
    bool enable_attribute_cache;   // 属性缓存
    bool enable_data_cache;       // 数据缓存
    char cache_mode[16];          // 缓存模式(none/strict/loose)
    
    // 安全配置
    char security_flavor[16];     // 安全类型(sys/krb5/krb5i/krb5p)
    char auth_type[16];          // 认证类型
    
    // 可靠性配置
    bool enable_soft_mount;       // 软挂载
    bool enable_backup;           // 备份支持
    uint32_t backup_interval_sec; // 备份间隔
} nfs_backend_config_t;

// 创建NFS后端
storage_interface_t* create_nfs_backend(void) {
    storage_interface_t *iface = calloc(1, sizeof(storage_interface_t));
    iface->type = STORAGE_BACKEND_NFS;
    strncpy(iface->name, "NFS Network Storage", sizeof(iface->name) - 1);
    
    // 设置NFS特有的函数指针
    iface->init = nfs_init;
    iface->cleanup = nfs_cleanup;
    iface->read_sync = nfs_read_sync;
    iface->write_sync = nfs_write_sync;
    iface->read_async = nfs_read_async;
    iface->write_async = nfs_write_async;
    
    return iface;
}
```

**性能特点**：
- 延迟：100微秒-10毫秒
- IOPS：1K-50K
- 带宽：1-10GB/s
- 适合：通用WAL存储，跨平台部署

### 1.3 Local后端 - 多模式本地存储

```c
// Local后端子类型
typedef enum {
    LOCAL_SUBTYPE_REGULAR = 1,     // 普通文件
    LOCAL_SUBTYPE_MMAP = 2,        // 内存映射
    LOCAL_SUBTYPE_DIRECT = 3,      // 直接I/O
    LOCAL_SUBTYPE_TMPFS = 4,       // 内存文件系统
    LOCAL_SUBTYPE_BLOCK = 5,       // 块设备
    LOCAL_SUBTYPE_SPARSE = 6,      // 稀疏文件
    LOCAL_SUBTYPE_COMPRESSED = 7,  // 压缩文件
    LOCAL_SUBTYPE_HYBRID = 8       // 混合存储
} local_storage_subtype_t;

// Local后端特性
typedef struct local_backend_config {
    local_storage_subtype_t subtype; // 子类型
    char base_directory[256];       // 基础目录
    
    // 通用配置
    bool use_aio;                  // 异步I/O
    bool enable_cache;             // 启用缓存
    size_t cache_size;            // 缓存大小
    
    // 子类型特定配置
    union {
        struct {
            bool use_direct_io;    // 直接I/O
            size_t io_alignment;   // I/O对齐
        } regular_config;
        
        struct {
            size_t mmap_size;      // 映射大小
            bool use_hugepages;    // 大页支持
        } mmap_config;
        
        struct {
            size_t tmpfs_size;     // TMPFS大小
            bool auto_mount;       // 自动挂载
        } tmpfs_config;
        
        struct {
            char device_path[256]; // 设备路径
            uint32_t queue_depth;  // 队列深度
        } block_config;
        
        struct {
            int compression_type;  // 压缩类型
            int compression_level; // 压缩级别
        } compressed_config;
        
        struct {
            char hot_directory[256];  // 热数据目录
            char cold_directory[256]; // 冷数据目录
            uint32_t migration_threshold; // 迁移阈值
        } hybrid_config;
    };
} local_backend_config_t;

// 创建Local后端
storage_interface_t* create_local_backend(void) {
    storage_interface_t *iface = calloc(1, sizeof(storage_interface_t));
    iface->type = STORAGE_BACKEND_LOCAL;
    strncpy(iface->name, "Local File Storage", sizeof(iface->name) - 1);
    
    // 设置Local特有的函数指针
    iface->init = local_init;
    iface->cleanup = local_cleanup;
    iface->read_sync = local_read_sync;
    iface->write_sync = local_write_sync;
    iface->read_async = local_read_async;
    iface->write_async = local_write_async;
    
    return iface;
}
```

**性能特点（依子类型而异）**：
- TMPFS: 延迟<1微秒，IOPS>1M
- Direct I/O: 延迟1-100微秒，IOPS 10K-500K
- Regular: 延迟100微秒-10毫秒，IOPS 1K-100K
- 适合：开发测试，单机部署，灵活配置

### 1.4 三大后端统一配置管理

```c
// 统一配置结构
typedef struct storage_backend_registry {
    // 后端工厂函数
    struct {
        storage_interface_t* (*create_nvmeof)(void);
        storage_interface_t* (*create_nfs)(void);
        storage_interface_t* (*create_local)(void);
    } factories;
    
    // 配置模板
    struct {
        nvmeof_backend_config_t nvmeof_default;
        nfs_backend_config_t nfs_default;
        local_backend_config_t local_default;
    } default_configs;
    
    // 性能基准
    struct {
        performance_profile_t nvmeof_profile;
        performance_profile_t nfs_profile;
        performance_profile_t local_profile;
    } performance_profiles;
    
} storage_backend_registry_t;

// 性能配置文件
typedef struct {
    uint64_t expected_latency_ns;    // 预期延迟
    uint64_t expected_iops;          // 预期IOPS
    uint64_t expected_bandwidth;     // 预期带宽
    double reliability_score;        // 可靠性评分(0-1)
    double cost_score;              // 成本评分(0-1)
} performance_profile_t;

// 智能后端选择算法
storage_backend_type_t select_optimal_backend_type(
    const workload_characteristics_t *workload,
    const deployment_constraints_t *constraints) {
    
    // 根据工作负载特征评分
    double nvmeof_score = calculate_backend_score(
        STORAGE_BACKEND_NVMEOF, workload, constraints);
    double nfs_score = calculate_backend_score(
        STORAGE_BACKEND_NFS, workload, constraints);
    double local_score = calculate_backend_score(
        STORAGE_BACKEND_LOCAL, workload, constraints);
    
    // 选择最高评分的后端
    if (nvmeof_score >= nfs_score && nvmeof_score >= local_score) {
        return STORAGE_BACKEND_NVMEOF;
    } else if (nfs_score >= local_score) {
        return STORAGE_BACKEND_NFS;
    } else {
        return STORAGE_BACKEND_LOCAL;
    }
}

// 工作负载特征
typedef struct {
    uint64_t avg_request_size;      // 平均请求大小
    double read_write_ratio;        // 读写比例
    uint64_t peak_iops;            // 峰值IOPS
    bool requires_low_latency;      // 是否需要低延迟
    bool requires_high_bandwidth;   // 是否需要高带宽
    bool requires_persistence;      // 是否需要持久化
} workload_characteristics_t;

// 部署约束
typedef struct {
    bool network_available;         // 网络可用性
    bool rdma_support;             // RDMA支持
    double budget_limit;           // 预算限制
    uint32_t max_latency_ms;       // 最大延迟要求
    bool data_locality_required;   // 是否需要数据本地性
} deployment_constraints_t;
```

### 1.5 三大后端配置文件示例

#### 生产环境高性能配置
```ini
[storage_manager]
primary_backend = nvmeof
fallback_backend = nfs
auto_failover = true
load_balance_algorithm = latency

[nvmeof]
target_nqn = nqn.2021-01.com.example:storage
transport_address = 192.168.1.100
transport_type = rdma
queue_depth = 1024
num_queues = 8
enable_polling = true

[nfs]
server_address = 192.168.1.200
export_path = /export/xdevice
nfs_version = v4.1
rsize = 1048576
wsize = 1048576
cache_mode = strict

[local]
subtype = tmpfs
base_directory = /dev/shm/xdevice-cache
tmpfs_size = 2147483648
auto_mount = true
use_aio = true
```

#### 开发测试环境配置
```ini
[storage_manager]
primary_backend = local
fallback_backend = local
auto_failover = false

[local]
subtype = hybrid
base_directory = /tmp/xdevice-dev
hot_directory = /dev/shm/xdevice-hot
cold_directory = /tmp/xdevice-cold
migration_threshold = 10
use_aio = true
enable_cache = true
cache_size = 67108864
```

#### 通用生产环境配置
```ini
[storage_manager]
primary_backend = nfs
fallback_backend = local
auto_failover = true

[nfs]
server_address = storage.company.com
export_path = /storage/xdevice
mount_point = /mnt/xdevice-storage
nfs_version = v4
rsize = 65536
wsize = 65536
enable_attribute_cache = true
enable_data_cache = true

[local]
subtype = regular
base_directory = /var/lib/xdevice/backup
use_aio = false
enable_cache = true
cache_size = 134217728
```

这三种存储后端的组合为xDevice提供了灵活的部署选择：

1. **NVMe-oF**: 最高性能，适合对延迟敏感的WAL场景
2. **NFS**: 平衡性能和兼容性，适合大多数生产环境  
3. **Local**: 最大灵活性，适合开发测试和特殊部署需求

通过统一的抽象层，应用程序可以透明地使用任何后端，并支持运行时切换和故障转移。

## 2. 三大后端集成架构图

```
                    ┌─────────────────────────────────────┐
                    │         xDevice应用层               │
                    │  (Raft实例、WAL引擎、业务逻辑)        │
                    └─────────────────┬───────────────────┘
                                      │
                    ┌─────────────────┴───────────────────┐
                    │      Raft存储适配器                 │
                    │  (raft_storage_adapter_t)           │
                    └─────────────────┬───────────────────┘
                                      │
                    ┌─────────────────┴───────────────────┐
                    │       存储管理器                    │
                    │   (storage_manager_t)               │
                    │  ┌─────────┬─────────┬─────────┐    │
                    │  │路由策略 │负载均衡 │缓存策略 │    │
                    │  └─────────┴─────────┴─────────┘    │
                    └─────────────────┬───────────────────┘
                                      │
                    ┌─────────────────┴───────────────────┐
                    │        统一存储接口                 │
                    │    (storage_interface_t)            │
                    └─────┬─────────┬─────────┬───────────┘
                          │         │         │
        ┌─────────────────┴─┐ ┌─────┴─────┐ ┌─┴─────────────────┐
        │   NVMe-oF后端     │ │  NFS后端  │ │   Local后端       │
        │                   │ │             │ │                   │
        │ ┌───────────────┐ │ │ ┌───────┐ │ │ ┌───────────────┐ │
        │ │RDMA/TCP传输层 │ │ │ │RPC层  │ │ │ │多种子类型支持 │ │
        │ │队列管理       │ │ │ │挂载管理│ │ │ │Regular/MMAP   │ │
        │ │连接池         │ │ │ │缓存策略│ │ │ │Direct/TMPFS   │ │
        │ │多路径支持     │ │ │ │版本兼容│ │ │ │Block/Sparse   │ │
        │ └───────────────┘ │ │ └───────┘ │ │ └───────────────┘ │
        └─────────┬─────────┘ └─────┬─────┘ │ └───────────────┘ │
                  │                 │       └─────────┬─────────┘
                  │                 │                 │
        ┌─────────┴─────────┐ ┌─────┴─────┐ ┌─────────┴─────────┐
        │  远程NVMe设备      │ │ NFS服务器 │ │     本地存储      │
        │ ┌───────────────┐ │ │ ┌───────┐ │ │ ┌───────────────┐ │
        │ │高速SSD/NVMe   │ │ │ │文件系统│ │ │ │文件系统/设备  │ │
        │ │RDMA网络       │ │ │ │网络存储│ │ │ │内存/磁盘      │ │
        │ │硬件队列       │ │ │ │备份系统│ │ │ │压缩/缓存      │ │
        │ └───────────────┘ │ │ └───────┘ │ │ └───────────────┘ │
        └───────────────────┘ └───────────┘ └───────────────────┘

                    性能特征对比：
        ┌─────────────┬─────────────┬─────────────┬─────────────┐
        │    后端类型  │    延迟     │    IOPS     │    带宽     │
        ├─────────────┼─────────────┼─────────────┼─────────────┤
        │   NVMe-oF   │   1-10μs    │  100K-1M+   │  10-100GB/s │
        │     NFS     │ 100μs-10ms  │   1K-50K    │   1-10GB/s  │
        │    Local    │   <1μs-ms   │  1K-1M+     │  100MB-GB/s │
        └─────────────┴─────────────┴─────────────┴─────────────┘

                    适用场景对比：
        ┌─────────────┬─────────────────┬─────────────────┬─────────────────┐
        │    后端类型  │    主要场景     │    优势         │    局限性       │
        ├─────────────┼─────────────────┼─────────────────┼─────────────────┤
        │   NVMe-oF   │ 高频WAL写入     │ 极低延迟        │ 网络和硬件要求高 │
        │             │ 实时系统        │ 高IOPS         │ 部署复杂        │
        ├─────────────┼─────────────────┼─────────────────┼─────────────────┤
        │     NFS     │ 通用WAL存储     │ 标准兼容        │ 延迟相对较高    │
        │             │ 跨平台部署      │ 易于管理        │ 网络依赖        │
        ├─────────────┼─────────────────┼─────────────────┼─────────────────┤
        │    Local    │ 开发测试        │ 灵活配置        │ 单机限制        │
        │             │ 单机部署        │ 无网络依赖      │ 扩展性有限      │
        └─────────────┴─────────────────┴─────────────────┴─────────────────┘
```

### 1.2 后端选择决策树

```
                    开始
                     │
                ┌────┴────┐
                │需要网络? │
                └────┬────┘
                     │
            ┌────────┴────────┐
            │                 │
           Yes               No
            │                 │
    ┌───────┴───────┐    ┌────┴────┐
    │需要极低延迟?   │    │选择Local │
    └───────┬───────┘    │后端      │
            │            └─────────┘
    ┌───────┴───────┐
    │              │
   Yes            No
    │              │
┌───┴───┐     ┌─────┴─────┐
│NVMe-oF│     │   NFS   │
│后端   │     │  后端   │
└───────┘     └─────────┘

决策因素：
1. 延迟要求：<10μs选NVMe-oF，<10ms选NFS，其他选Local
2. IOPS要求：>100K选NVMe-oF，>10K选NFS，其他选Local  
3. 网络环境：有RDMA选NVMe-oF，有以太网选NFS，无网络选Local
4. 预算约束：高预算选NVMe-oF，中预算选NFS，低预算选Local
5. 部署复杂度：简单部署选Local，中等选NFS，复杂部署选NVMe-oF
```

### 1.3 动态后端切换机制

```c
// 动态后端切换管理器
typedef struct {
    storage_manager_t *manager;
    
    // 切换策略
    struct {
        bool enable_auto_switch;      // 自动切换
        uint32_t monitor_interval_ms; // 监控间隔
        double latency_threshold;     // 延迟阈值
        double error_rate_threshold;  // 错误率阈值
    } switch_policy;
    
    // 切换状态
    struct {
        storage_backend_type_t current_backend;
        storage_backend_type_t target_backend;
        bool switching_in_progress;
        uint64_t switch_start_time;
    } switch_state;
    
    // 性能监控
    struct {
        performance_monitor_t monitors[STORAGE_BACKEND_COUNT];
        health_checker_t health_checkers[STORAGE_BACKEND_COUNT];
    } monitoring;
    
} dynamic_backend_switcher_t;

// 性能监控器
typedef struct {
    uint64_t total_ops;
    uint64_t failed_ops;
    uint64_t total_latency_ns;
    uint64_t max_latency_ns;
    double current_error_rate;
    double avg_latency_ms;
    bool is_healthy;
} performance_monitor_t;

// 自动后端切换逻辑
int check_and_switch_backend(dynamic_backend_switcher_t *switcher) {
    if (!switcher->switch_policy.enable_auto_switch) {
        return 0;
    }
    
    storage_backend_type_t current = switcher->switch_state.current_backend;
    performance_monitor_t *monitor = &switcher->monitoring.monitors[current];
    
    // 检查当前后端性能
    if (monitor->avg_latency_ms > switcher->switch_policy.latency_threshold ||
        monitor->current_error_rate > switcher->switch_policy.error_rate_threshold ||
        !monitor->is_healthy) {
        
        // 选择最优备用后端
        storage_backend_type_t best_backend = select_best_alternative_backend(switcher);
        
        if (best_backend != current) {
            log_info("Switching from backend %d to %d due to performance issues", 
                    current, best_backend);
            return initiate_backend_switch(switcher, best_backend);
        }
    }
    
    return 0;
}

// 无缝后端切换
int initiate_backend_switch(dynamic_backend_switcher_t *switcher,
                           storage_backend_type_t target_backend) {
    // 1. 标记切换开始
    switcher->switch_state.switching_in_progress = true;
    switcher->switch_state.target_backend = target_backend;
    switcher->switch_state.switch_start_time = get_monotonic_time_ns();
    
    // 2. 预热目标后端
    if (warmup_backend(switcher->manager, target_backend) != 0) {
        return -1;
    }
    
    // 3. 等待当前操作完成
    wait_for_pending_operations(switcher->manager);
    
    // 4. 原子切换
    storage_backend_type_t old_backend = switcher->switch_state.current_backend;
    switcher->switch_state.current_backend = target_backend;
    
    // 5. 更新路由策略
    update_routing_policy(switcher->manager, target_backend);
    
    // 6. 切换完成
    switcher->switch_state.switching_in_progress = false;
    
    log_info("Backend switch completed: %d -> %d", old_backend, target_backend);
    return 0;
}
```

这个完整的三大后端架构设计提供了：

1. **统一抽象**：所有后端都实现相同的接口，应用层无需关心底层实现
2. **智能路由**：根据工作负载特征自动选择最优后端
3. **动态切换**：支持运行时无缝切换后端，提高可用性
4. **性能监控**：实时监控各后端性能，支持自动调优
5. **故障转移**：自动检测故障并切换到备用后端

通过这三种存储后端的组合，xDevice可以适应从低延迟实时系统到大容量归档存储的各种应用场景。

## 0.9 Raft存储使用示例

```c
// 完整的Raft实例存储使用示例
#include "storage_engine.h"
#include "raft.h"

int main() {
    // 1. 创建存储管理器
    storage_manager_t storage_manager;
    storage_manager_init(&storage_manager);
    
    // 2. 注册三大存储后端
    storage_interface_t *nvmeof_backend = create_nvmeof_backend();
    storage_interface_t *nfs_backend = create_nfs_backend();
    storage_interface_t *local_backend = create_local_backend();
    
    // 3. 初始化各后端
    nvmeof_backend->init(nvmeof_backend, 
        "[nvmeof]\n"
        "target_nqn = nqn.2021-01.com.example:storage\n"
        "transport_address = 192.168.1.100\n"
        "transport_type = rdma\n"
        "queue_depth = 1024\n");
        
    nfs_backend->init(nfs_backend,
        "[nfs]\n"
        "server_address = 192.168.1.200\n"
        "export_path = /export/xdevice\n"
        "nfs_version = v4.1\n"
        "rsize = 1048576\n");
        
    local_backend->init(local_backend,
        "[local]\n"
        "storage_type = compressed\n"
        "base_directory = /tmp/xdevice\n"
        "compression_type = lz4\n");
    
    // 4. 注册后端到存储管理器
    storage_manager_register_backend(&storage_manager, nvmeof_backend);
    storage_manager_register_backend(&storage_manager, nfs_backend);
    storage_manager_register_backend(&storage_manager, local_backend);
    
    // 5. 创建Raft存储适配器
    raft_storage_adapter_t raft_storage;
    raft_storage_adapter_init(&raft_storage, &storage_manager, 1,
        "[raft_storage]\n"
        "instance_id = 1\n"
        "enable_smart_routing = true\n"
        "log_backend = nvmeof\n"
        "state_backend = nfs\n"
        "snapshot_backend = local\n"
        "[optimization]\n"
        "batch_log_writes = true\n"
        "log_batch_size = 100\n"
        "[consistency]\n"
        "fsync_on_log_write = true\n"
        "verify_checksums = true\n");
    
    // 6. 在Raft算法中使用存储
    
    // 加载持久化状态
    uint32_t current_term, voted_for;
    uint64_t commit_index;
    raft_storage_load_state(&raft_storage, &current_term, &voted_for, &commit_index);
    printf("Loaded state: term=%u, voted_for=%u, commit_index=%lu\n",
           current_term, voted_for, commit_index);
    
    // 追加日志条目
    const char *log_data = "SET key1 value1";
    int ret = raft_storage_append_log(&raft_storage, 1, current_term,
                                     log_data, strlen(log_data));
    if (ret == 0) {
        printf("Log entry appended successfully\n");
    }
    
    // 批量追加日志条目
    raft_log_batch_t batch = {0};
    batch.count = 3;
    
    const char *batch_data[] = {
        "SET key2 value2",
        "SET key3 value3", 
        "DELETE key1"
    };
    
    for (int i = 0; i < 3; i++) {
        batch.entries[i].index = 2 + i;
        batch.entries[i].term = current_term;
        batch.entries[i].data = (void*)batch_data[i];
        batch.entries[i].size = strlen(batch_data[i]);
    }
    
    ret = raft_storage_append_logs_batch(&raft_storage, &batch);
    if (ret == 0) {
        printf("Batch log entries appended successfully\n");
    }
    
    // 读取日志条目
    uint32_t term;
    void *data;
    size_t size;
    ret = raft_storage_read_log(&raft_storage, 1, &term, &data, &size);
    if (ret == 0) {
        printf("Read log entry: term=%u, data=%.*s\n", term, (int)size, (char*)data);
        free(data);
    }
    
    // 保存状态
    ret = raft_storage_save_state(&raft_storage, current_term + 1, 2, 5);
    if (ret == 0) {
        printf("State saved successfully\n");
    }
    
    // 创建快照
    const char *snapshot_data = "{'key2': 'value2', 'key3': 'value3'}";
    ret = raft_storage_save_snapshot(&raft_storage, 5, current_term + 1,
                                    snapshot_data, strlen(snapshot_data));
    if (ret == 0) {
        printf("Snapshot saved successfully\n");
    }
    
    // 截断日志(快照后清理旧日志)
    ret = raft_storage_truncate_log(&raft_storage, 3);
    if (ret == 0) {
        printf("Log truncated successfully\n");
    }
    
    // 7. 清理资源
    raft_storage_adapter_cleanup(&raft_storage);
    
    storage_manager_unregister_backend(&storage_manager, STORAGE_BACKEND_NVMEOF);
    storage_manager_unregister_backend(&storage_manager, STORAGE_BACKEND_NFS);
    storage_manager_unregister_backend(&storage_manager, STORAGE_BACKEND_LOCAL);
    
    nvmeof_backend->cleanup(nvmeof_backend);
    nfs_backend->cleanup(nfs_backend);
    local_backend->cleanup(local_backend);
    
    free(nvmeof_backend);
    free(nfs_backend);
    free(local_backend);
    
    storage_manager_cleanup(&storage_manager);
    
    return 0;
}

// 初始化地址映射和空间管理
int init_nvmeof_address_management(nvmeof_raft_storage_t *storage) {
    // 初始化日志管理
    storage->log_mgmt.log_head_lba = 
        storage->device_layout.partition_layout.log_start_lba;
    storage->log_mgmt.log_tail_lba = 
        storage->device_layout.partition_layout.log_start_lba + 1; // 跳过头部
    storage->log_mgmt.is_circular_buffer = true;
    storage->log_mgmt.first_log_index = 0;
    storage->log_mgmt.last_log_index = 0;
    pthread_mutex_init(&storage->log_mgmt.log_lock, NULL);
    
    // 初始化数据管理
    storage->data_mgmt.next_free_lba = 
        storage->device_layout.partition_layout.data_start_lba + 1; // 跳过头部
    storage->data_mgmt.layout_type = DATA_LAYOUT_BTREE;
    pthread_mutex_init(&storage->data_mgmt.data_lock, NULL);
    
    // 初始化快照管理
    storage->snapshot_mgmt.active_snapshot_lba = 
        storage->device_layout.partition_layout.snapshot_start_lba;
    storage->snapshot_mgmt.max_snapshots = 5;
    pthread_mutex_init(&storage->snapshot_mgmt.snapshot_lock, NULL);
    
    // 初始化空闲空间管理器
    storage->address_mgmt.log_free_mgr = init_free_space_manager(
        storage->device_layout.partition_layout.log_start_lba + 1,
        storage->device_layout.partition_layout.log_start_lba + 
        storage->device_layout.partition_layout.log_size_blocks);
        
    storage->address_mgmt.data_free_mgr = init_free_space_manager(
        storage->device_layout.partition_layout.data_start_lba + 1,
        storage->device_layout.partition_layout.data_start_lba + 
        storage->device_layout.partition_layout.data_size_blocks);
        
    storage->address_mgmt.snapshot_free_mgr = init_free_space_manager(
        storage->device_layout.partition_layout.snapshot_start_lba,
        storage->device_layout.partition_layout.snapshot_start_lba + 
        storage->device_layout.partition_layout.snapshot_size_blocks);
    
    pthread_mutex_init(&storage->address_mgmt.mapping_lock, NULL);
    
    return 0;
}

// 打印性能统计信息
void print_nvmeof_storage_stats(nvmeof_raft_storage_t *storage) {
    printf("  Log partition usage: %lu/%lu blocks (%.1f%%)\n",
           storage->device_layout.usage_stats.log_used_blocks,
           storage->device_layout.partition_layout.log_size_blocks,
           (double)storage->device_layout.usage_stats.log_used_blocks * 100.0 / 
           storage->device_layout.partition_layout.log_size_blocks);
           
    printf("  Data partition usage: %lu/%lu blocks (%.1f%%)\n",
           storage->device_layout.usage_stats.data_used_blocks,
           storage->device_layout.partition_layout.data_size_blocks,
           (double)storage->device_layout.usage_stats.data_used_blocks * 100.0 / 
           storage->device_layout.partition_layout.data_size_blocks);
           
    printf("  Total log entries: %lu\n", storage->log_mgmt.log_entries_count);
    printf("  Log index range: %lu - %lu\n", 
           storage->log_mgmt.first_log_index, storage->log_mgmt.last_log_index);
    printf("  Active snapshots: %lu\n", storage->snapshot_mgmt.snapshot_count);
    
    // 获取底层NVMe-oF性能统计
    storage_stats_t nvmeof_stats;
    storage->nvmeof_backend->get_stats(storage->nvmeof_backend, &nvmeof_stats);
    
    printf("  NVMe-oF read ops: %lu (avg latency: %.2f μs)\n",
           nvmeof_stats.read_ops,
           nvmeof_stats.read_latency_avg_ns / 1000.0);
    printf("  NVMe-oF write ops: %lu (avg latency: %.2f μs)\n",
           nvmeof_stats.write_ops,
           nvmeof_stats.write_latency_avg_ns / 1000.0);
}
```

### 3.7 性能优化策略

```c
// NVMe-oF设备性能优化配置
typedef struct {
    // I/O优化
    struct {
        uint32_t queue_depth;           // 队列深度(建议1024-4096)
        uint32_t io_size_blocks;        // I/O大小(块数)
        bool enable_write_coalescing;   // 写入合并
        bool enable_read_ahead;         // 预读
        uint32_t write_coalescing_timeout_us; // 写入合并超时
    } io_optimization;
    
    // 内存管理
    struct {
        size_t io_buffer_pool_size;     // I/O缓冲池大小
        bool use_huge_pages;            // 使用大页内存
        bool pin_memory;                // 锁定内存
        uint32_t buffer_alignment;      // 缓冲区对齐
    } memory_management;
    
    // 缓存策略
    struct {
        bool enable_log_cache;          // 日志缓存
        size_t log_cache_size;          // 日志缓存大小
        bool enable_data_cache;         // 数据缓存
        size_t data_cache_size;         // 数据缓存大小
        uint32_t cache_ttl_seconds;     // 缓存TTL
    } cache_strategy;
    
    // 并发控制
    struct {
        uint32_t max_concurrent_ios;    // 最大并发I/O
        bool enable_parallel_writes;    // 并行写入
        bool enable_pipeline;           // 流水线处理
        uint32_t worker_thread_count;   // 工作线程数
    } concurrency_control;
    
} nvmeof_performance_config_t;

// 写入合并优化
typedef struct {
    struct {
        uint64_t start_lba;
        uint32_t block_count;
        void *data;
        uint64_t timestamp;
    } pending_writes[MAX_COALESCE_WRITES];
    
    uint32_t pending_count;
    pthread_mutex_t coalesce_lock;
    pthread_cond_t coalesce_cond;
    pthread_t coalesce_thread;
    bool coalescing_enabled;
} write_coalescing_t;

// 初始化写入合并
void init_write_coalescing(write_coalescing_t *coalescing) {
    coalescing->pending_count = 0;
    coalescing->coalescing_enabled = true;
    pthread_mutex_init(&coalescing->coalesce_lock, NULL);
    pthread_cond_init(&coalescing->coalesce_cond, NULL);
}

// 合并写入请求
void coalesce_write_request(write_coalescing_t *coalescing,
                           uint64_t lba, uint32_t size, void *data) {
    pthread_mutex_lock(&coalescing->coalesce_lock);
    
    if (coalescing->pending_count < MAX_COALESCE_WRITES) {
        coalescing->pending_writes[coalescing->pending_count].start_lba = lba;
        coalescing->pending_writes[coalescing->pending_count].block_count = size;
        coalescing->pending_writes[coalescing->pending_count].data = data;
        coalescing->pending_writes[coalescing->pending_count].timestamp = get_current_time_ns();
        coalescing->pending_count++;
    }
    
    pthread_mutex_unlock(&coalescing->coalesce_lock);
}

// 刷新合并的写入请求
void flush_coalesced_writes(write_coalescing_t *coalescing,
                           storage_manager_t *manager) {
    pthread_mutex_lock(&coalescing->coalesce_lock);
    
    for (uint32_t i = 0; i < coalescing->pending_count; i++) {
        // 执行实际的写入操作
        storage_write_sync(manager, 
                          coalescing->pending_writes[i].start_lba,
                          coalescing->pending_writes[i].data,
                          coalescing->pending_writes[i].block_count);
    }
    
    coalescing->pending_count = 0;
    
    pthread_mutex_unlock(&coalescing->coalesce_lock);
}
```
