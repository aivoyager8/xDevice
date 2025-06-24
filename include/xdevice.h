#ifndef XDEVICE_H
#define XDEVICE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <pthread.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 版本信息 */
#define XDEVICE_VERSION_MAJOR 1
#define XDEVICE_VERSION_MINOR 0
#define XDEVICE_VERSION_PATCH 0

/* 配置常量 */
#define XDEVICE_MAX_NODES 64
#define XDEVICE_MAX_DEVICES 256
#define XDEVICE_BLOCK_SIZE 4096
#define XDEVICE_WAL_BUFFER_SIZE (64 * 1024 * 1024)  // 64MB
#define XDEVICE_MAX_NODE_ID_LEN 64
#define XDEVICE_MAX_DEVICE_NAME_LEN 128
#define MAX_STORAGE_BACKENDS 8
#define MAX_PATH_LEN 512

/* 错误码 */
typedef enum {
    XDEVICE_OK = 0,
    XDEVICE_ERROR = -1,
    XDEVICE_ERROR_INVALID_PARAM = -2,
    XDEVICE_ERROR_OUT_OF_MEMORY = -3,
    XDEVICE_ERROR_IO = -4,
    XDEVICE_ERROR_NETWORK = -5,
    XDEVICE_ERROR_TIMEOUT = -6,
    XDEVICE_ERROR_NOT_FOUND = -7,
    XDEVICE_ERROR_EXISTS = -8,
    XDEVICE_ERROR_PERMISSION = -9,
    XDEVICE_ERROR_CONSISTENCY = -10,
    XDEVICE_ERROR_NODE_DOWN = -11
} xdevice_error_t;

/* 节点状态 */
typedef enum {
    XDEVICE_NODE_UNKNOWN = 0,
    XDEVICE_NODE_LEADER = 1,
    XDEVICE_NODE_FOLLOWER = 2,
    XDEVICE_NODE_CANDIDATE = 3,
    XDEVICE_NODE_DOWN = 4
} xdevice_node_state_t;

/* 存储后端类型 */
typedef enum {
    STORAGE_BACKEND_NVMEOF = 1,    // NVMe-oF远程存储
    STORAGE_BACKEND_NFS = 2,       // NFS网络文件系统
    STORAGE_BACKEND_LOCAL = 3,     // 本地文件存储
    STORAGE_BACKEND_MOCK = 4       // Mock存储(单元测试)
} storage_backend_type_t;

/* 存储操作状态 */
typedef enum {
    STORAGE_OP_SUCCESS = 0,
    STORAGE_OP_PENDING = 1,        // 异步操作进行中
    STORAGE_OP_ERROR = -1,         // 操作失败
    STORAGE_OP_TIMEOUT = -2,       // 操作超时
    STORAGE_OP_NOT_FOUND = -3,     // 文件不存在
    STORAGE_OP_NO_SPACE = -4       // 存储空间不足
} storage_op_status_t;

/* 设备状态 */
typedef enum {
    XDEVICE_DEVICE_CREATING = 0,
    XDEVICE_DEVICE_ACTIVE = 1,
    XDEVICE_DEVICE_READONLY = 2,
    XDEVICE_DEVICE_ERROR = 3,
    XDEVICE_DEVICE_DELETED = 4
} xdevice_device_state_t;

/* WAL操作类型 */
typedef enum {
    XDEVICE_WAL_WRITE = 1,
    XDEVICE_WAL_SYNC = 2,
    XDEVICE_WAL_CHECKPOINT = 3
} xdevice_wal_op_type_t;

/* 基础数据结构 */
typedef struct {
    char node_id[XDEVICE_MAX_NODE_ID_LEN];
    char address[256];
    uint16_t port;
    xdevice_node_state_t state;
    uint64_t last_heartbeat;
    uint64_t term;
} xdevice_node_info_t;

typedef struct {
    char name[XDEVICE_MAX_DEVICE_NAME_LEN];
    uint64_t size;
    uint32_t block_size;
    xdevice_device_state_t state;
    uint64_t created_time;
    uint64_t modified_time;
    char primary_node[XDEVICE_MAX_NODE_ID_LEN];
    char replica_nodes[XDEVICE_MAX_NODES][XDEVICE_MAX_NODE_ID_LEN];
    uint32_t replica_count;
} xdevice_device_info_t;

typedef struct {
    uint64_t lsn;  // Log Sequence Number
    uint64_t offset;
    uint32_t length;
    xdevice_wal_op_type_t op_type;
    uint64_t timestamp;
    uint32_t checksum;
    char device_name[XDEVICE_MAX_DEVICE_NAME_LEN];
} xdevice_wal_entry_t;

/* 前向声明 */
typedef struct xdevice_context xdevice_context_t;
typedef struct xdevice_device xdevice_device_t;
typedef struct xdevice_wal xdevice_wal_t;

/* 回调函数类型 */
typedef void (*xdevice_log_callback_t)(int level, const char* message);
typedef void (*xdevice_error_callback_t)(xdevice_error_t error, const char* message);

/* 核心API */

/**
 * 初始化XDevice上下文
 */
xdevice_context_t* xdevice_init(const char* config_file);

/**
 * 清理XDevice上下文
 */
void xdevice_cleanup(xdevice_context_t* ctx);

/**
 * 启动节点服务
 */
xdevice_error_t xdevice_start_node(xdevice_context_t* ctx);

/**
 * 停止节点服务
 */
xdevice_error_t xdevice_stop_node(xdevice_context_t* ctx);

/**
 * 创建块设备
 */
xdevice_error_t xdevice_create_device(xdevice_context_t* ctx, 
                                      const char* name, 
                                      uint64_t size);

/**
 * 删除块设备
 */
xdevice_error_t xdevice_delete_device(xdevice_context_t* ctx, 
                                      const char* name);

/**
 * 打开块设备
 */
xdevice_device_t* xdevice_open_device(xdevice_context_t* ctx, 
                                      const char* name);

/**
 * 关闭块设备
 */
void xdevice_close_device(xdevice_device_t* device);

/**
 * 读取数据
 */
xdevice_error_t xdevice_read(xdevice_device_t* device, 
                             uint64_t offset, 
                             void* buffer, 
                             size_t length);

/**
 * 写入数据 (WAL优化)
 */
xdevice_error_t xdevice_write(xdevice_device_t* device, 
                              uint64_t offset, 
                              const void* buffer, 
                              size_t length);

/**
 * 同步数据到磁盘
 */
xdevice_error_t xdevice_sync(xdevice_device_t* device);

/**
 * 获取设备信息
 */
xdevice_error_t xdevice_get_device_info(xdevice_device_t* device, 
                                        xdevice_device_info_t* info);

/**
 * 获取节点列表
 */
xdevice_error_t xdevice_get_nodes(xdevice_context_t* ctx, 
                                  xdevice_node_info_t* nodes, 
                                  uint32_t* count);

/**
 * 添加节点到集群
 */
xdevice_error_t xdevice_add_node(xdevice_context_t* ctx, 
                                 const char* node_id, 
                                 const char* address, 
                                 uint16_t port);

/**
 * 从集群移除节点
 */
xdevice_error_t xdevice_remove_node(xdevice_context_t* ctx, 
                                    const char* node_id);

/**
 * 设置日志回调
 */
void xdevice_set_log_callback(xdevice_log_callback_t callback);

/**
 * 设置错误回调
 */
void xdevice_set_error_callback(xdevice_error_callback_t callback);

/**
 * 获取版本信息
 */
const char* xdevice_get_version(void);

#ifdef __cplusplus
}
#endif

#endif /* XDEVICE_H */
