# Raft实例与存储后端管理策略

## 1. 问题分析

### 1.1 核心目标重新审视
我们的目标是**为应用提供分布式WAL设备**，而不是简单的分布式文件存储。这意味着：

- **应用视角**: 应用看到的是一个高可用的WAL设备
- **Raft职责**: Raft负责保证WAL数据的分布式一致性
- **存储管理**: 每个Raft实例需要智能地管理自己的存储空间

### 1.2 当前架构的问题
之前的设计过于通用化，没有充分考虑WAL场景的特殊性：

1. **文件粒度过细**: 每个日志条目一个文件，开销太大
2. **存储分散**: 没有统一的存储空间管理
3. **缺乏WAL语义**: 没有体现WAL的顺序写入特性
4. **复制冗余**: Raft日志和应用WAL重复存储

## 2. 重新设计：WAL导向的存储管理

### 2.1 核心设计理念

```
应用WAL请求 → Raft协调 → 统一存储后端 → 物理设备
     ↓           ↓           ↓            ↓
   追加写入    分布式复制   空间管理     顺序I/O
```

**关键原则**:
1. **WAL即存储**: Raft日志就是应用WAL的分布式副本
2. **统一地址空间**: 每个Raft实例管理一个连续的虚拟地址空间
3. **顺序分配**: 严格按顺序分配存储空间，优化WAL性能
4. **智能路由**: 根据数据类型和访问模式选择最佳物理存储

### 2.2 存储空间抽象

```c
// WAL存储空间抽象
typedef struct {
    uint64_t logical_start;         // 逻辑起始地址
    uint64_t logical_end;           // 逻辑结束地址
    uint64_t current_head;          // 当前写入头部
    uint64_t committed_tail;        // 已提交尾部
    uint64_t total_capacity;        // 总容量
    
    // 分段管理
    struct {
        uint64_t segment_size;      // 段大小
        uint32_t active_segments;   // 活跃段数
        uint32_t total_segments;    // 总段数
        wal_segment_t *segments;    // 段数组
    } segmentation;
    
    // 存储后端映射
    struct {
        storage_backend_type_t hot_backend;   // 热数据后端(NVMe-oF)
        storage_backend_type_t warm_backend;  // 温数据后端(NFS)
        storage_backend_type_t cold_backend;  // 冷数据后端(Local)
        uint64_t hot_threshold;              // 热数据阈值
        uint64_t cold_threshold;             // 冷数据阈值
    } tiering;
    
} wal_storage_space_t;

// WAL段管理
typedef struct {
    uint64_t segment_id;            // 段ID
    uint64_t logical_start;         // 逻辑起始地址
    uint64_t logical_end;           // 逻辑结束地址
    uint64_t physical_offset;       // 物理偏移
    storage_backend_type_t backend; // 存储后端
    
    enum {
        SEGMENT_ACTIVE,             // 活跃段（正在写入）
        SEGMENT_SEALED,             // 封闭段（只读）
        SEGMENT_ARCHIVED,           // 归档段（可删除）
        SEGMENT_COMPACTING          // 压缩中
    } state;
    
    uint32_t entry_count;           // 条目数量
    uint64_t last_access_time;      // 最后访问时间
    uint32_t reference_count;       // 引用计数
    
} wal_segment_t;
```

### 2.3 Raft实例存储管理策略

#### 2.3.1 领导者实例的存储管理

```c
// 领导者WAL写入管理
typedef struct {
    wal_storage_space_t *storage_space;    // 存储空间
    
    // 写入缓冲
    struct {
        void *buffer;                       // 写入缓冲区
        size_t buffer_size;                 // 缓冲区大小
        size_t used_size;                   // 已使用大小
        uint64_t flush_threshold;           // 刷新阈值
        uint64_t last_flush_time;           // 上次刷新时间
    } write_buffer;
    
    // 批量写入
    struct {
        wal_entry_t *pending_entries;       // 待写入条目
        uint32_t pending_count;             // 待写入数量
        uint32_t max_batch_size;            // 最大批量大小
        uint64_t batch_timeout_ns;          // 批量超时
    } batching;
    
    // 性能优化
    struct {
        bool enable_write_ahead;            // 启用预写
        bool enable_compression;            // 启用压缩
        uint32_t compression_threshold;     // 压缩阈值
        bool enable_deduplication;          // 启用去重
    } optimization;
    
} leader_wal_manager_t;

// 领导者WAL追加接口
int leader_wal_append(leader_wal_manager_t *manager,
                     const void *data, size_t size,
                     uint64_t *assigned_lsn) {
    // 1. 分配逻辑序列号
    uint64_t lsn = allocate_next_lsn(manager->storage_space);
    *assigned_lsn = lsn;
    
    // 2. 创建WAL条目
    wal_entry_t entry = {
        .lsn = lsn,
        .data_size = size,
        .timestamp = get_current_time_ns(),
        .checksum = calculate_checksum(data, size)
    };
    
    // 3. 添加到批量缓冲
    int ret = add_to_batch(manager, &entry, data);
    if (ret != WAL_SUCCESS) {
        return ret;
    }
    
    // 4. 检查是否需要立即刷新
    if (should_flush_batch(manager)) {
        ret = flush_batch_to_storage(manager);
    }
    
    return ret;
}
```

#### 2.3.2 跟随者实例的存储管理

```c
// 跟随者WAL复制管理
typedef struct {
    wal_storage_space_t *storage_space;    // 存储空间
    uint64_t last_applied_lsn;             // 最后应用的LSN
    uint64_t replication_lag;              // 复制延迟
    
    // 复制缓冲
    struct {
        wal_entry_t *replication_buffer;    // 复制缓冲区
        uint32_t buffer_capacity;           // 缓冲容量
        uint32_t pending_count;             // 待处理数量
        pthread_mutex_t buffer_lock;        // 缓冲锁
    } replication;
    
    // 一致性检查
    struct {
        uint64_t last_check_lsn;            // 最后检查的LSN
        uint32_t check_interval;            // 检查间隔
        bool integrity_verified;            // 完整性验证
    } consistency;
    
} follower_wal_manager_t;

// 跟随者WAL应用接口
int follower_wal_apply(follower_wal_manager_t *manager,
                      const wal_entry_t *entry,
                      const void *data) {
    // 1. 验证LSN连续性
    if (entry->lsn != manager->last_applied_lsn + 1) {
        return WAL_ERROR_LSN_GAP;
    }
    
    // 2. 校验数据完整性
    uint32_t calculated_checksum = calculate_checksum(data, entry->data_size);
    if (calculated_checksum != entry->checksum) {
        return WAL_ERROR_CHECKSUM_MISMATCH;
    }
    
    // 3. 写入本地存储
    uint64_t physical_offset = map_lsn_to_physical_offset(
        manager->storage_space, entry->lsn);
    
    int ret = write_to_storage_backend(manager->storage_space,
                                      physical_offset, data, entry->data_size);
    if (ret != WAL_SUCCESS) {
        return ret;
    }
    
    // 4. 更新状态
    manager->last_applied_lsn = entry->lsn;
    
    return WAL_SUCCESS;
}
```

## 3. 存储后端的智能管理

### 3.1 多层存储策略

```c
// 分层存储管理器
typedef struct {
    // 热层：最新的WAL数据，需要高性能访问
    struct {
        storage_interface_t *backend;       // NVMe-oF后端
        uint64_t capacity;                  // 容量
        uint64_t used;                      // 已使用
        uint32_t retention_hours;           // 保留时间（小时）
    } hot_tier;
    
    // 温层：较旧的WAL数据，需要可靠存储
    struct {
        storage_interface_t *backend;       // NFS后端
        uint64_t capacity;                  // 容量
        uint64_t used;                      // 已使用
        uint32_t retention_days;            // 保留时间（天）
    } warm_tier;
    
    // 冷层：归档数据，需要大容量低成本
    struct {
        storage_interface_t *backend;       // Local后端
        uint64_t capacity;                  // 容量
        uint64_t used;                      // 已使用
        uint32_t retention_months;          // 保留时间（月）
    } cold_tier;
    
    // 自动分层策略
    struct {
        bool enable_auto_tiering;           // 启用自动分层
        uint64_t tier_check_interval;       // 检查间隔
        double hot_to_warm_threshold;       // 热到温阈值
        double warm_to_cold_threshold;      // 温到冷阈值
    } tiering_policy;
    
} tiered_storage_manager_t;

// 智能数据分层
int auto_tier_data(tiered_storage_manager_t *manager,
                  wal_storage_space_t *space) {
    uint64_t now = get_current_time_ns();
    
    // 遍历所有段
    for (uint32_t i = 0; i < space->segmentation.total_segments; i++) {
        wal_segment_t *segment = &space->segmentation.segments[i];
        uint64_t age = now - segment->last_access_time;
        
        // 决定分层策略
        storage_backend_type_t target_backend = determine_target_backend(
            manager, segment, age);
        
        if (target_backend != segment->backend) {
            // 执行数据迁移
            int ret = migrate_segment_to_backend(segment, target_backend);
            if (ret == WAL_SUCCESS) {
                update_segment_backend(segment, target_backend);
            }
        }
    }
    
    return WAL_SUCCESS;
}
```

### 3.2 物理存储优化

#### 3.2.1 NVMe-oF后端优化（热数据）

```c
// NVMe-oF WAL优化配置
typedef struct {
    // 设备配置
    char nvmeof_target[256];            // NVMe-oF目标
    uint32_t queue_depth;               // 队列深度
    uint32_t io_size;                   // I/O大小
    bool enable_write_cache;            // 启用写缓存
    
    // WAL专用优化
    struct {
        uint64_t wal_partition_offset;   // WAL分区偏移
        uint64_t wal_partition_size;     // WAL分区大小
        uint32_t log_block_size;         // 日志块大小
        bool enable_fua;                 // 强制单元访问
    } wal_config;
    
    // 性能调优
    struct {
        uint32_t write_batch_size;       // 写入批量大小
        uint32_t flush_frequency;        // 刷新频率
        bool enable_polling;             // 启用轮询模式
        uint32_t interrupt_coalescing;   // 中断合并
    } performance;
    
} nvmeof_wal_config_t;

// NVMe-oF WAL写入优化
int nvmeof_wal_write_optimized(nvmeof_wal_config_t *config,
                              uint64_t offset,
                              const void *data, size_t size) {
    // 1. 对齐到块边界
    uint64_t aligned_offset = align_to_block(offset, 
                                            config->wal_config.log_block_size);
    size_t aligned_size = align_size(size, 
                                    config->wal_config.log_block_size);
    
    // 2. 使用批量I/O
    io_batch_t batch = {
        .entries = {{
            .offset = aligned_offset,
            .data = data,
            .size = aligned_size,
            .flags = config->wal_config.enable_fua ? IO_FLAG_FUA : 0
        }},
        .count = 1
    };
    
    // 3. 提交批量I/O
    return submit_batch_io(config, &batch);
}
```

#### 3.2.2 存储空间的统一寻址

```c
// 统一虚拟地址空间
typedef struct {
    uint64_t virtual_base;              // 虚拟基址
    uint64_t virtual_size;              // 虚拟大小
    
    // 物理映射表
    struct {
        uint64_t virtual_start;         // 虚拟起始
        uint64_t virtual_end;           // 虚拟结束
        storage_backend_type_t backend; // 物理后端
        uint64_t physical_offset;       // 物理偏移
        void *backend_handle;           // 后端句柄
    } *mappings;
    
    uint32_t mapping_count;             // 映射数量
    pthread_rwlock_t mapping_lock;      // 映射锁
    
} virtual_address_space_t;

// 虚拟地址到物理地址转换
int translate_virtual_to_physical(virtual_address_space_t *vas,
                                 uint64_t virtual_addr,
                                 storage_backend_mapping_t *mapping) {
    pthread_rwlock_rdlock(&vas->mapping_lock);
    
    // 二分查找映射
    int left = 0, right = vas->mapping_count - 1;
    while (left <= right) {
        int mid = (left + right) / 2;
        auto *map = &vas->mappings[mid];
        
        if (virtual_addr >= map->virtual_start && 
            virtual_addr < map->virtual_end) {
            // 找到映射
            mapping->backend = map->backend;
            mapping->physical_offset = map->physical_offset + 
                                     (virtual_addr - map->virtual_start);
            mapping->backend_handle = map->backend_handle;
            
            pthread_rwlock_unlock(&vas->mapping_lock);
            return WAL_SUCCESS;
        } else if (virtual_addr < map->virtual_start) {
            right = mid - 1;
        } else {
            left = mid + 1;
        }
    }
    
    pthread_rwlock_unlock(&vas->mapping_lock);
    return WAL_ERROR_ADDRESS_NOT_MAPPED;
}
```

## 4. 应用接口设计

### 4.1 应用视角的WAL设备

```c
// 应用看到的WAL设备接口
typedef struct {
    char device_name[64];               // 设备名称
    uint64_t capacity;                  // 容量
    uint32_t block_size;                // 块大小
    
    // WAL语义接口
    int (*append)(void *handle, const void *data, size_t size, uint64_t *lsn);
    int (*read)(void *handle, uint64_t lsn, void *buffer, size_t *size);
    int (*sync)(void *handle, uint64_t lsn);
    int (*truncate)(void *handle, uint64_t lsn);
    
    // 管理接口
    int (*get_stats)(void *handle, wal_device_stats_t *stats);
    int (*set_config)(void *handle, const wal_device_config_t *config);
    
    void *private_data;                 // 私有数据
    
} wal_device_t;

// 应用WAL追加接口
int wal_device_append(wal_device_t *device,
                     const void *data, size_t size,
                     uint64_t *assigned_lsn) {
    // 获取设备的Raft集群句柄
    raft_cluster_t *cluster = (raft_cluster_t*)device->private_data;
    
    // 检查是否为领导者
    if (!is_cluster_leader(cluster)) {
        return WAL_ERROR_NOT_LEADER;
    }
    
    // 通过Raft协议复制WAL条目
    raft_log_entry_t entry = {
        .type = RAFT_ENTRY_WAL_DATA,
        .data = data,
        .data_size = size,
        .timestamp = get_current_time_ns()
    };
    
    return raft_cluster_append_log(cluster, &entry, assigned_lsn);
}
```

### 4.2 总结：最佳实践

1. **统一地址空间**: 每个Raft实例管理连续的虚拟地址空间
2. **智能分层**: 根据数据热度自动选择最佳存储后端
3. **WAL语义**: 直接支持WAL的追加、读取、同步、截断操作
4. **批量优化**: 聚合小的写入请求，提升整体性能
5. **透明复制**: 应用无需关心分布式细节，Raft自动处理复制

这样的设计既保证了WAL的高性能，又提供了分布式的高可用性。

## 3. WAL生命周期导向的存储优化

### 3.1 WAL数据生命周期分析

```c
// WAL数据生命周期状态
typedef enum {
    WAL_DATA_ACTIVE,        // 活跃数据：应用正在写入的最新数据
    WAL_DATA_COMMITTED,     // 已提交数据：事务已提交，但可能需要恢复
    WAL_DATA_CHECKPOINTED,  // 已检查点：数据已持久化到主存储
    WAL_DATA_OBSOLETE,      // 过期数据：可以安全删除
    WAL_DATA_PURGING        // 清理中：正在删除过程中
} wal_data_lifecycle_t;

// WAL段生命周期管理
typedef struct {
    uint64_t segment_id;                // 段ID
    uint64_t start_lsn;                 // 起始LSN
    uint64_t end_lsn;                   // 结束LSN
    wal_data_lifecycle_t lifecycle;     // 生命周期状态
    
    // 生命周期时间戳
    uint64_t created_time;              // 创建时间
    uint64_t committed_time;            // 提交时间
    uint64_t checkpointed_time;         // 检查点时间
    uint64_t obsolete_time;             // 过期时间
    
    // 存储信息
    storage_backend_type_t backend;     // 当前存储后端
    uint64_t physical_offset;           // 物理偏移
    size_t data_size;                   // 数据大小
    uint32_t reference_count;           // 引用计数
    
    // 优化标志
    bool can_be_compressed;             // 可压缩
    bool can_be_deduplicated;           // 可去重
    bool ready_for_purge;               // 可清理
    
} wal_lifecycle_segment_t;
```

### 3.2 基于生命周期的存储策略

#### 3.2.1 双层存储架构（取消冷存储）

```c
// 简化的双层存储管理器
typedef struct {
    // 热层：活跃和已提交的WAL数据
    struct {
        storage_interface_t *backend;       // 高性能后端(NVMe-oF/NFS)
        uint64_t capacity;                  // 容量
        uint64_t used;                      // 已使用
        uint64_t active_data_size;          // 活跃数据大小
        uint64_t committed_data_size;       // 已提交数据大小
        
        // 热层优化配置
        uint32_t write_batch_size;          // 写入批量大小
        uint32_t flush_frequency_ms;        // 刷新频率
        bool enable_write_combining;        // 启用写合并
    } hot_tier;
    
    // 温层：已检查点的WAL数据（等待清理）
    struct {
        storage_interface_t *backend;       // 标准后端(NFS/Local)
        uint64_t capacity;                  // 容量
        uint64_t used;                      // 已使用
        uint64_t checkpointed_data_size;    // 已检查点数据大小
        
        // 温层主要用于故障恢复和审计
        uint32_t retention_hours;           // 保留时间（小时）
        bool enable_compression;            // 启用压缩
        uint32_t compression_ratio;         // 压缩比例
    } warm_tier;
    
    // 生命周期管理
    struct {
        uint64_t checkpoint_lsn;            // 当前检查点LSN
        uint64_t purge_below_lsn;           // 可清理的LSN阈值
        uint32_t auto_purge_interval_ms;    // 自动清理间隔
        bool enable_aggressive_purge;       // 启用激进清理
    } lifecycle_mgmt;
    
} wal_optimized_storage_t;
```

#### 3.2.2 智能数据清理策略

```c
// WAL数据清理管理器
typedef struct {
    wal_optimized_storage_t *storage;
    
    // 清理策略配置
    struct {
        uint64_t min_checkpoint_interval;   // 最小检查点间隔
        uint64_t max_obsolete_data_size;    // 最大过期数据大小
        double purge_trigger_ratio;         // 清理触发比例
        uint32_t batch_purge_size;          // 批量清理大小
    } purge_policy;
    
    // 清理统计
    struct {
        uint64_t total_purged_bytes;        // 总清理字节数
        uint64_t total_purged_segments;     // 总清理段数
        uint64_t purge_operations;          // 清理操作数
        uint64_t space_reclaimed;           // 回收空间
        double avg_purge_latency_ms;        // 平均清理延迟
    } purge_stats;
    
    // 清理线程控制
    pthread_t purge_thread;
    volatile bool purge_thread_running;
    pthread_mutex_t purge_lock;
    pthread_cond_t purge_condition;
    
} wal_purge_manager_t;

// 智能清理算法
int wal_intelligent_purge(wal_purge_manager_t *manager,
                         uint64_t checkpoint_lsn) {
    wal_optimized_storage_t *storage = manager->storage;
    
    // 1. 更新检查点信息
    storage->lifecycle_mgmt.checkpoint_lsn = checkpoint_lsn;
    
    // 2. 标记可清理的段
    wal_lifecycle_segment_t *segments_to_purge[MAX_PURGE_BATCH];
    uint32_t purge_count = 0;
    
    // 遍历所有段，找出可清理的段
    for (uint32_t i = 0; i < storage->total_segments && 
         purge_count < MAX_PURGE_BATCH; i++) {
        wal_lifecycle_segment_t *segment = &storage->segments[i];
        
        // 检查是否可以清理
        if (can_purge_segment(segment, checkpoint_lsn, 
                             manager->purge_policy.min_checkpoint_interval)) {
            segments_to_purge[purge_count++] = segment;
        }
    }
    
    // 3. 批量清理段
    if (purge_count > 0) {
        int ret = batch_purge_segments(manager, segments_to_purge, purge_count);
        if (ret == WAL_SUCCESS) {
            update_purge_stats(manager, segments_to_purge, purge_count);
        }
        return ret;
    }
    
    return WAL_SUCCESS;
}

// 判断段是否可以清理
static bool can_purge_segment(wal_lifecycle_segment_t *segment,
                             uint64_t checkpoint_lsn,
                             uint64_t min_interval) {
    // 1. 段的结束LSN必须小于检查点LSN
    if (segment->end_lsn >= checkpoint_lsn) {
        return false;
    }
    
    // 2. 段必须处于已检查点状态
    if (segment->lifecycle != WAL_DATA_CHECKPOINTED) {
        return false;
    }
    
    // 3. 检查点后必须经过最小间隔时间
    uint64_t now = get_current_time_ns();
    if (now - segment->checkpointed_time < min_interval) {
        return false;
    }
    
    // 4. 引用计数为0
    if (segment->reference_count > 0) {
        return false;
    }
    
    return true;
}
```

### 3.3 写入优化：预分配和快速回收

#### 3.3.1 段预分配策略

```c
// WAL段预分配管理器
typedef struct {
    // 预分配池
    struct {
        wal_lifecycle_segment_t *preallocated_segments;
        uint32_t pool_size;                 // 池大小
        uint32_t available_count;           // 可用数量
        uint32_t allocation_watermark;      // 分配水位线
        pthread_mutex_t pool_lock;          // 池锁
    } segment_pool;
    
    // 预分配策略
    struct {
        size_t default_segment_size;        // 默认段大小
        uint32_t min_pool_size;            // 最小池大小
        uint32_t max_pool_size;            // 最大池大小
        uint32_t prealloc_batch_size;      // 预分配批量大小
        bool enable_size_prediction;       // 启用大小预测
    } prealloc_policy;
    
    // 回收策略
    struct {
        uint32_t recycle_batch_size;       // 回收批量大小
        bool enable_immediate_recycle;     // 启用立即回收
        uint32_t max_recycled_segments;    // 最大回收段数
    } recycle_policy;
    
} wal_segment_allocator_t;

// 快速段分配
wal_lifecycle_segment_t* allocate_wal_segment_fast(
    wal_segment_allocator_t *allocator,
    size_t required_size) {
    
    pthread_mutex_lock(&allocator->segment_pool.pool_lock);
    
    // 1. 从预分配池获取段
    if (allocator->segment_pool.available_count > 0) {
        wal_lifecycle_segment_t *segment = 
            &allocator->segment_pool.preallocated_segments[
                --allocator->segment_pool.available_count];
        
        // 初始化段
        initialize_segment_for_use(segment, required_size);
        
        pthread_mutex_unlock(&allocator->segment_pool.pool_lock);
        
        // 检查是否需要补充预分配池
        if (allocator->segment_pool.available_count < 
            allocator->prealloc_policy.min_pool_size) {
            trigger_async_preallocation(allocator);
        }
        
        return segment;
    }
    
    pthread_mutex_unlock(&allocator->segment_pool.pool_lock);
    
    // 2. 预分配池为空，紧急分配
    return emergency_allocate_segment(allocator, required_size);
}

// 快速段回收
int recycle_wal_segment_fast(wal_segment_allocator_t *allocator,
                            wal_lifecycle_segment_t *segment) {
    // 1. 清理段数据
    clear_segment_data(segment);
    
    // 2. 重置段状态
    reset_segment_state(segment);
    
    // 3. 返回到预分配池
    pthread_mutex_lock(&allocator->segment_pool.pool_lock);
    
    if (allocator->segment_pool.available_count < 
        allocator->prealloc_policy.max_pool_size) {
        // 放回池中
        allocator->segment_pool.preallocated_segments[
            allocator->segment_pool.available_count++] = *segment;
        
        pthread_mutex_unlock(&allocator->segment_pool.pool_lock);
        return WAL_SUCCESS;
    }
    
    pthread_mutex_unlock(&allocator->segment_pool.pool_lock);
    
    // 池已满，直接释放
    return deallocate_segment(segment);
}
```

#### 3.3.2 写入路径优化

```c
// 优化的WAL写入路径
typedef struct {
    // 写入缓冲区
    struct {
        void *buffer;                       // 缓冲区
        size_t buffer_size;                 // 缓冲区大小
        size_t used_size;                   // 已使用大小
        uint64_t first_lsn;                 // 第一个LSN
        uint64_t last_lsn;                  // 最后一个LSN
        
        // 快速刷新控制
        uint64_t flush_threshold_bytes;     // 刷新字节阈值
        uint64_t flush_timeout_ns;          // 刷新超时
        uint64_t last_flush_time;           // 上次刷新时间
    } write_buffer;
    
    // 段管理
    wal_lifecycle_segment_t *active_segment;    // 当前活跃段
    wal_segment_allocator_t *allocator;         // 段分配器
    
    // 性能优化开关
    struct {
        bool enable_write_combining;       // 启用写合并
        bool enable_zero_copy;             // 启用零拷贝
        bool enable_async_flush;           // 启用异步刷新
        uint32_t max_pending_writes;       // 最大挂起写入数
    } optimization;
    
} optimized_wal_writer_t;

// 超快WAL写入
int wal_append_ultra_fast(optimized_wal_writer_t *writer,
                         const void *data, size_t size,
                         uint64_t *assigned_lsn) {
    // 1. 快速路径：直接写入缓冲区
    if (writer->write_buffer.used_size + size <= 
        writer->write_buffer.buffer_size) {
        
        // 分配LSN
        uint64_t lsn = atomic_fetch_add(&global_lsn_counter, 1);
        *assigned_lsn = lsn;
        
        // 写入缓冲区
        memcpy((char*)writer->write_buffer.buffer + 
               writer->write_buffer.used_size, data, size);
        writer->write_buffer.used_size += size;
        writer->write_buffer.last_lsn = lsn;
        
        // 检查是否需要刷新
        if (should_flush_buffer(writer)) {
            return async_flush_buffer(writer);
        }
        
        return WAL_SUCCESS;
    }
    
    // 2. 慢速路径：缓冲区满，需要刷新
    int ret = flush_buffer_and_retry(writer, data, size, assigned_lsn);
    return ret;
}
```

### 3.4 故障恢复优化

```c
// 基于生命周期的快速恢复
typedef struct {
    uint64_t recovery_checkpoint_lsn;      // 恢复检查点LSN
    uint64_t last_valid_lsn;               // 最后有效LSN
    
    // 只需要恢复活跃和已提交的数据
    struct {
        wal_lifecycle_segment_t *active_segments;
        uint32_t active_count;
        wal_lifecycle_segment_t *committed_segments;  
        uint32_t committed_count;
    } recovery_data;
    
    // 恢复统计
    struct {
        uint64_t recovered_bytes;           // 恢复字节数
        uint64_t skipped_obsolete_bytes;    // 跳过的过期字节数
        uint32_t recovery_time_ms;          // 恢复时间
    } recovery_stats;
    
} wal_fast_recovery_t;

// 快速恢复算法
int wal_fast_recovery(wal_fast_recovery_t *recovery,
                     wal_optimized_storage_t *storage) {
    uint64_t start_time = get_current_time_ms();
    
    // 1. 只恢复必要的数据（活跃+已提交）
    for (uint32_t i = 0; i < storage->total_segments; i++) {
        wal_lifecycle_segment_t *segment = &storage->segments[i];
        
        // 跳过过期数据，大幅减少恢复时间
        if (segment->lifecycle == WAL_DATA_OBSOLETE ||
            segment->end_lsn < recovery->recovery_checkpoint_lsn) {
            recovery->recovery_stats.skipped_obsolete_bytes += segment->data_size;
            continue;
        }
        
        // 恢复活跃和已提交的数据
        if (segment->lifecycle == WAL_DATA_ACTIVE ||
            segment->lifecycle == WAL_DATA_COMMITTED) {
            int ret = recover_segment_data(segment);
            if (ret != WAL_SUCCESS) {
                return ret;
            }
            recovery->recovery_stats.recovered_bytes += segment->data_size;
        }
    }
    
    // 2. 重建内存状态
    rebuild_in_memory_state(recovery, storage);
    
    recovery->recovery_stats.recovery_time_ms = 
        get_current_time_ms() - start_time;
    
    return WAL_SUCCESS;
}
```
