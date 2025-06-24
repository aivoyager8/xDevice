#include "../include/storage_interface.h"
#include "../include/xdevice.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <assert.h>

/**
 * 存储管理器初始化
 */
int storage_manager_init(storage_manager_t *manager) {
    if (!manager) {
        return XDEVICE_ERROR_INVALID_PARAM;
    }
    
    memset(manager, 0, sizeof(storage_manager_t));
    
    // 初始化路由策略
    manager->routing_policy.primary_backend = STORAGE_BACKEND_LOCAL;
    manager->routing_policy.fallback_backend = STORAGE_BACKEND_MOCK;
    manager->routing_policy.auto_failover = true;
    manager->routing_policy.failover_timeout_ms = 5000;
    
    // 初始化负载均衡
    manager->load_balancer.enabled = false;
    manager->load_balancer.algorithm = LOAD_BALANCE_ROUND_ROBIN;
    manager->load_balancer.current_backend = 0;
    
    // 初始化缓存策略
    manager->cache_policy.read_cache_enabled = false;
    manager->cache_policy.write_cache_enabled = false;
    manager->cache_policy.cache_size = 0;
    manager->cache_policy.cache_ttl_seconds = 300;
    
    // 初始化统计锁
    if (pthread_mutex_init(&manager->stats_lock, NULL) != 0) {
        return XDEVICE_ERROR;
    }
    
    memset(&manager->global_stats, 0, sizeof(storage_stats_t));
    
    printf("[StorageManager] 存储管理器初始化完成\n");
    return XDEVICE_OK;
}

/**
 * 存储管理器清理
 */
int storage_manager_cleanup(storage_manager_t *manager) {
    if (!manager) {
        return XDEVICE_ERROR_INVALID_PARAM;
    }
    
    // 清理所有注册的后端
    for (uint32_t i = 0; i < manager->backend_count; i++) {
        if (manager->backends[i] && manager->backends[i]->cleanup) {
            manager->backends[i]->cleanup(manager->backends[i]);
        }
        manager->backends[i] = NULL;
    }
    
    manager->backend_count = 0;
    
    // 销毁锁
    pthread_mutex_destroy(&manager->stats_lock);
    
    printf("[StorageManager] 存储管理器清理完成\n");
    return XDEVICE_OK;
}

/**
 * 注册存储后端
 */
int storage_manager_register_backend(storage_manager_t *manager,
                                    storage_interface_t *backend) {
    if (!manager || !backend) {
        return XDEVICE_ERROR_INVALID_PARAM;
    }
    
    if (manager->backend_count >= MAX_STORAGE_BACKENDS) {
        printf("[StorageManager] 错误：已达到最大后端数量限制\n");
        return XDEVICE_ERROR;
    }
    
    // 检查是否已经注册了相同类型的后端
    for (uint32_t i = 0; i < manager->backend_count; i++) {
        if (manager->backends[i]->type == backend->type) {
            printf("[StorageManager] 警告：后端类型 %d 已存在，将替换\n", backend->type);
            if (manager->backends[i]->cleanup) {
                manager->backends[i]->cleanup(manager->backends[i]);
            }
            manager->backends[i] = backend;
            return XDEVICE_OK;
        }
    }
    
    // 添加新后端
    manager->backends[manager->backend_count] = backend;
    manager->backend_count++;
    
    printf("[StorageManager] 注册存储后端: %s (类型: %d)\n", 
           backend->name, backend->type);
    
    return XDEVICE_OK;
}

/**
 * 注销存储后端
 */
int storage_manager_unregister_backend(storage_manager_t *manager,
                                      storage_backend_type_t type) {
    if (!manager) {
        return XDEVICE_ERROR_INVALID_PARAM;
    }
    
    for (uint32_t i = 0; i < manager->backend_count; i++) {
        if (manager->backends[i]->type == type) {
            // 清理后端
            if (manager->backends[i]->cleanup) {
                manager->backends[i]->cleanup(manager->backends[i]);
            }
            
            // 移动后续元素
            for (uint32_t j = i; j < manager->backend_count - 1; j++) {
                manager->backends[j] = manager->backends[j + 1];
            }
            
            manager->backend_count--;
            manager->backends[manager->backend_count] = NULL;
            
            printf("[StorageManager] 注销存储后端类型: %d\n", type);
            return XDEVICE_OK;
        }
    }
    
    return XDEVICE_ERROR_NOT_FOUND;
}

/**
 * 选择存储后端
 */
static storage_interface_t* select_backend(storage_manager_t *manager,
                                          const char *path) {
    if (!manager || manager->backend_count == 0) {
        return NULL;
    }
    
    // 简单策略：使用主要后端
    for (uint32_t i = 0; i < manager->backend_count; i++) {
        if (manager->backends[i]->type == manager->routing_policy.primary_backend) {
            // 健康检查
            if (manager->backends[i]->health_check && 
                manager->backends[i]->health_check(manager->backends[i]) == XDEVICE_OK) {
                return manager->backends[i];
            }
        }
    }
    
    // 故障转移到备用后端
    if (manager->routing_policy.auto_failover) {
        for (uint32_t i = 0; i < manager->backend_count; i++) {
            if (manager->backends[i]->type == manager->routing_policy.fallback_backend) {
                if (manager->backends[i]->health_check && 
                    manager->backends[i]->health_check(manager->backends[i]) == XDEVICE_OK) {
                    printf("[StorageManager] 故障转移到备用后端: %s\n", 
                           manager->backends[i]->name);
                    return manager->backends[i];
                }
            }
        }
    }
    
    // 返回第一个可用的后端
    for (uint32_t i = 0; i < manager->backend_count; i++) {
        if (manager->backends[i]->health_check && 
            manager->backends[i]->health_check(manager->backends[i]) == XDEVICE_OK) {
            return manager->backends[i];
        }
    }
    
    return NULL;
}

/**
 * 更新统计信息
 */
static void update_stats(storage_manager_t *manager, 
                        bool is_read, size_t bytes, uint64_t latency_ns) {
    pthread_mutex_lock(&manager->stats_lock);
    
    if (is_read) {
        manager->global_stats.read_ops++;
        manager->global_stats.read_bytes += bytes;
        // 简单的移动平均
        manager->global_stats.read_latency_avg_ns = 
            (manager->global_stats.read_latency_avg_ns + latency_ns) / 2;
    } else {
        manager->global_stats.write_ops++;
        manager->global_stats.write_bytes += bytes;
        manager->global_stats.write_latency_avg_ns = 
            (manager->global_stats.write_latency_avg_ns + latency_ns) / 2;
    }
    
    pthread_mutex_unlock(&manager->stats_lock);
}

/**
 * 存储管理器读取
 */
int storage_manager_read(storage_manager_t *manager,
                        const char *path, uint64_t offset,
                        void *buffer, size_t size) {
    if (!manager || !path || !buffer || size == 0) {
        return XDEVICE_ERROR_INVALID_PARAM;
    }
    
    storage_interface_t *backend = select_backend(manager, path);
    if (!backend) {
        printf("[StorageManager] 错误：无可用的存储后端\n");
        return XDEVICE_ERROR;
    }
    
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    
    int ret = backend->read_sync(backend, path, offset, buffer, size);
    
    clock_gettime(CLOCK_MONOTONIC, &end);
    uint64_t latency_ns = (end.tv_sec - start.tv_sec) * 1000000000UL + 
                          (end.tv_nsec - start.tv_nsec);
    
    if (ret == XDEVICE_OK) {
        update_stats(manager, true, size, latency_ns);
    } else {
        pthread_mutex_lock(&manager->stats_lock);
        manager->global_stats.error_count++;
        pthread_mutex_unlock(&manager->stats_lock);
    }
    
    return ret;
}

/**
 * 存储管理器写入
 */
int storage_manager_write(storage_manager_t *manager,
                         const char *path, uint64_t offset,
                         const void *data, size_t size) {
    if (!manager || !path || !data || size == 0) {
        return XDEVICE_ERROR_INVALID_PARAM;
    }
    
    storage_interface_t *backend = select_backend(manager, path);
    if (!backend) {
        printf("[StorageManager] 错误：无可用的存储后端\n");
        return XDEVICE_ERROR;
    }
    
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    
    int ret = backend->write_sync(backend, path, offset, data, size);
    
    clock_gettime(CLOCK_MONOTONIC, &end);
    uint64_t latency_ns = (end.tv_sec - start.tv_sec) * 1000000000UL + 
                          (end.tv_nsec - start.tv_nsec);
    
    if (ret == XDEVICE_OK) {
        update_stats(manager, false, size, latency_ns);
    } else {
        pthread_mutex_lock(&manager->stats_lock);
        manager->global_stats.error_count++;
        pthread_mutex_unlock(&manager->stats_lock);
    }
    
    return ret;
}

/**
 * 存储管理器同步
 */
int storage_manager_sync(storage_manager_t *manager, const char *path) {
    if (!manager || !path) {
        return XDEVICE_ERROR_INVALID_PARAM;
    }
    
    storage_interface_t *backend = select_backend(manager, path);
    if (!backend) {
        printf("[StorageManager] 错误：无可用的存储后端\n");
        return XDEVICE_ERROR;
    }
    
    if (!backend->fsync) {
        return XDEVICE_OK; // 某些后端可能不需要显式同步
    }
    
    return backend->fsync(backend, path);
}

/**
 * Raft存储适配器初始化
 */
int raft_storage_adapter_init(raft_storage_adapter_t *adapter,
                             storage_manager_t *manager,
                             const char *device_prefix) {
    if (!adapter || !manager || !device_prefix) {
        return XDEVICE_ERROR_INVALID_PARAM;
    }
    
    memset(adapter, 0, sizeof(raft_storage_adapter_t));
    
    adapter->storage_manager = manager;
    strncpy(adapter->device_prefix, device_prefix, sizeof(adapter->device_prefix) - 1);
    
    // 设置默认前缀
    strcpy(adapter->log_prefix, "log_");
    strcpy(adapter->state_prefix, "state_");
    strcpy(adapter->snapshot_prefix, "snap_");
    strcpy(adapter->metadata_prefix, "meta_");
    
    // 设置默认路由：全部使用主要后端
    adapter->routing.log_backend = manager->routing_policy.primary_backend;
    adapter->routing.state_backend = manager->routing_policy.primary_backend;
    adapter->routing.snapshot_backend = manager->routing_policy.primary_backend;
    adapter->routing.metadata_backend = manager->routing_policy.primary_backend;
    
    // 性能优化默认配置
    adapter->optimization.batch_writes_enabled = true;
    adapter->optimization.batch_size = 64;
    adapter->optimization.batch_timeout_ms = 10;
    adapter->optimization.async_writes_enabled = false;
    
    printf("[RaftAdapter] Raft存储适配器初始化完成，设备前缀: %s\n", device_prefix);
    return XDEVICE_OK;
}

/**
 * Raft存储适配器清理
 */
int raft_storage_adapter_cleanup(raft_storage_adapter_t *adapter) {
    if (!adapter) {
        return XDEVICE_ERROR_INVALID_PARAM;
    }
    
    adapter->storage_manager = NULL;
    
    printf("[RaftAdapter] Raft存储适配器清理完成\n");
    return XDEVICE_OK;
}

/**
 * 构建完整路径
 */
static void build_full_path(const raft_storage_adapter_t *adapter,
                           const char *prefix, const char *key,
                           char *full_path, size_t path_size) {
    snprintf(full_path, path_size, "%s%s%s", 
             adapter->device_prefix, prefix, key);
}

/**
 * Raft日志追加
 */
int raft_storage_append_log(raft_storage_adapter_t *adapter,
                           uint64_t log_index,
                           const void *entry_data, size_t entry_size) {
    if (!adapter || !entry_data || entry_size == 0) {
        return XDEVICE_ERROR_INVALID_PARAM;
    }
    
    char log_key[64];
    char full_path[MAX_PATH_LEN];
    
    snprintf(log_key, sizeof(log_key), "%lu", log_index);
    build_full_path(adapter, adapter->log_prefix, log_key, 
                   full_path, sizeof(full_path));
    
    return storage_manager_write(adapter->storage_manager, 
                                full_path, 0, entry_data, entry_size);
}

/**
 * Raft日志读取
 */
int raft_storage_read_log(raft_storage_adapter_t *adapter,
                         uint64_t log_index,
                         void *buffer, size_t *size) {
    if (!adapter || !buffer || !size) {
        return XDEVICE_ERROR_INVALID_PARAM;
    }
    
    char log_key[64];
    char full_path[MAX_PATH_LEN];
    
    snprintf(log_key, sizeof(log_key), "%lu", log_index);
    build_full_path(adapter, adapter->log_prefix, log_key, 
                   full_path, sizeof(full_path));
    
    return storage_manager_read(adapter->storage_manager, 
                               full_path, 0, buffer, *size);
}

/**
 * Raft状态保存
 */
int raft_storage_save_state(raft_storage_adapter_t *adapter,
                           const char *state_key,
                           const void *state_data, size_t size) {
    if (!adapter || !state_key || !state_data || size == 0) {
        return XDEVICE_ERROR_INVALID_PARAM;
    }
    
    char full_path[MAX_PATH_LEN];
    build_full_path(adapter, adapter->state_prefix, state_key, 
                   full_path, sizeof(full_path));
    
    return storage_manager_write(adapter->storage_manager, 
                                full_path, 0, state_data, size);
}

/**
 * Raft状态加载
 */
int raft_storage_load_state(raft_storage_adapter_t *adapter,
                           const char *state_key,
                           void *buffer, size_t *size) {
    if (!adapter || !state_key || !buffer || !size) {
        return XDEVICE_ERROR_INVALID_PARAM;
    }
    
    char full_path[MAX_PATH_LEN];
    build_full_path(adapter, adapter->state_prefix, state_key, 
                   full_path, sizeof(full_path));
    
    return storage_manager_read(adapter->storage_manager, 
                               full_path, 0, buffer, *size);
}

/**
 * Raft快照保存
 */
int raft_storage_save_snapshot(raft_storage_adapter_t *adapter,
                              uint64_t snapshot_index,
                              const void *snapshot_data, size_t size) {
    if (!adapter || !snapshot_data || size == 0) {
        return XDEVICE_ERROR_INVALID_PARAM;
    }
    
    char snapshot_key[64];
    char full_path[MAX_PATH_LEN];
    
    snprintf(snapshot_key, sizeof(snapshot_key), "%lu", snapshot_index);
    build_full_path(adapter, adapter->snapshot_prefix, snapshot_key, 
                   full_path, sizeof(full_path));
    
    return storage_manager_write(adapter->storage_manager, 
                                full_path, 0, snapshot_data, size);
}

/**
 * Raft快照加载
 */
int raft_storage_load_snapshot(raft_storage_adapter_t *adapter,
                              uint64_t snapshot_index,
                              void *buffer, size_t *size) {
    if (!adapter || !buffer || !size) {
        return XDEVICE_ERROR_INVALID_PARAM;
    }
    
    char snapshot_key[64];
    char full_path[MAX_PATH_LEN];
    
    snprintf(snapshot_key, sizeof(snapshot_key), "%lu", snapshot_index);
    build_full_path(adapter, adapter->snapshot_prefix, snapshot_key, 
                   full_path, sizeof(full_path));
    
    return storage_manager_read(adapter->storage_manager, 
                               full_path, 0, buffer, *size);
}
