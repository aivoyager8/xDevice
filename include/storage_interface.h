#ifndef STORAGE_INTERFACE_H
#define STORAGE_INTERFACE_H

#include "xdevice.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 前向声明 */
struct storage_interface;

/* 异步I/O回调函数 */
typedef void (*storage_callback_t)(void *context, 
                                  storage_op_status_t status,
                                  void *data, size_t size);

/* 存储操作上下文 */
typedef struct {
    uint64_t operation_id;         // 操作ID
    void *user_context;            // 用户上下文
    storage_callback_t callback;   // 完成回调
    uint64_t start_time_ns;        // 开始时间
    uint64_t timeout_ns;           // 超时时间
} storage_operation_ctx_t;

/* 存储文件信息 */
typedef struct {
    uint64_t size;                 // 文件大小
    uint64_t created_time;         // 创建时间
    uint64_t modified_time;        // 修改时间
    uint32_t permissions;          // 权限
    bool is_directory;             // 是否为目录
} storage_file_info_t;

/* 批量操作定义 */
typedef struct {
    enum {
        STORAGE_BATCH_READ,
        STORAGE_BATCH_WRITE,
        STORAGE_BATCH_FSYNC
    } op_type;
    
    char path[MAX_PATH_LEN];       // 文件路径
    uint64_t offset;               // 偏移量
    void *data;                    // 数据指针
    size_t size;                   // 数据大小
    storage_operation_ctx_t *ctx;  // 操作上下文
} storage_batch_op_t;

/* 存储统计信息 */
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

/* 统一存储接口 */
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

/* 存储管理器 */
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

/* 存储管理器接口 */
int storage_manager_init(storage_manager_t *manager);
int storage_manager_cleanup(storage_manager_t *manager);

int storage_manager_register_backend(storage_manager_t *manager,
                                    storage_interface_t *backend);

int storage_manager_unregister_backend(storage_manager_t *manager,
                                      storage_backend_type_t type);

int storage_manager_read(storage_manager_t *manager,
                        const char *path, uint64_t offset,
                        void *buffer, size_t size);

int storage_manager_write(storage_manager_t *manager,
                         const char *path, uint64_t offset,
                         const void *data, size_t size);

int storage_manager_sync(storage_manager_t *manager, const char *path);

/* Raft存储适配器 */
typedef struct {
    storage_manager_t *storage_manager;
    char device_prefix[64];                            // 设备前缀
    
    // 文件前缀配置
    char log_prefix[32];                               // 日志前缀 "log_"
    char state_prefix[32];                             // 状态前缀 "state_"
    char snapshot_prefix[32];                          // 快照前缀 "snap_"
    char metadata_prefix[32];                          // 元数据前缀 "meta_"
    
    // 路由配置
    struct {
        storage_backend_type_t log_backend;            // 日志存储后端
        storage_backend_type_t state_backend;          // 状态存储后端
        storage_backend_type_t snapshot_backend;       // 快照存储后端
        storage_backend_type_t metadata_backend;       // 元数据存储后端
    } routing;
    
    // 性能优化
    struct {
        bool batch_writes_enabled;                     // 批量写入
        size_t batch_size;                             // 批量大小
        uint32_t batch_timeout_ms;                     // 批量超时
        bool async_writes_enabled;                     // 异步写入
    } optimization;
    
} raft_storage_adapter_t;

/* Raft存储适配器接口 */
int raft_storage_adapter_init(raft_storage_adapter_t *adapter,
                             storage_manager_t *manager,
                             const char *device_prefix);

int raft_storage_adapter_cleanup(raft_storage_adapter_t *adapter);

// Raft专用接口
int raft_storage_append_log(raft_storage_adapter_t *adapter,
                           uint64_t log_index,
                           const void *entry_data, size_t entry_size);

int raft_storage_read_log(raft_storage_adapter_t *adapter,
                         uint64_t log_index,
                         void *buffer, size_t *size);

int raft_storage_save_state(raft_storage_adapter_t *adapter,
                           const char *state_key,
                           const void *state_data, size_t size);

int raft_storage_load_state(raft_storage_adapter_t *adapter,
                           const char *state_key,
                           void *buffer, size_t *size);

int raft_storage_save_snapshot(raft_storage_adapter_t *adapter,
                              uint64_t snapshot_index,
                              const void *snapshot_data, size_t size);

int raft_storage_load_snapshot(raft_storage_adapter_t *adapter,
                              uint64_t snapshot_index,
                              void *buffer, size_t *size);

#ifdef __cplusplus
}
#endif

#endif // STORAGE_INTERFACE_H
