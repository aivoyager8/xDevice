#include "../include/storage_interface.h"
#include "../include/xdevice.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>

/* Mock存储文件结构 */
typedef struct mock_file {
    char path[MAX_PATH_LEN];
    void *data;
    size_t size;
    size_t capacity;
    uint64_t created_time;
    uint64_t modified_time;
    struct mock_file *next;
} mock_file_t;

/* Mock存储私有数据 */
typedef struct {
    mock_file_t *files;            // 文件链表
    pthread_mutex_t files_lock;    // 文件锁
    
    // 配置
    size_t max_file_size;          // 最大文件大小
    uint32_t max_files;            // 最大文件数
    uint32_t file_count;           // 当前文件数
    
    // 模拟延迟
    uint32_t read_delay_us;        // 读延迟(微秒)
    uint32_t write_delay_us;       // 写延迟(微秒)
    
    // 错误注入
    bool inject_read_errors;       // 注入读错误
    bool inject_write_errors;      // 注入写错误
    uint32_t error_rate_percent;   // 错误率(百分比)
    
    // 统计信息
    storage_stats_t stats;
    pthread_mutex_t stats_lock;
    
} mock_storage_private_t;

/* 前向声明 */
static int mock_storage_init(storage_interface_t *iface, const char *config);
static int mock_storage_cleanup(storage_interface_t *iface);
static int mock_storage_health_check(storage_interface_t *iface);
static int mock_storage_read_sync(storage_interface_t *iface,
                                 const char *path, uint64_t offset,
                                 void *buffer, size_t size);
static int mock_storage_write_sync(storage_interface_t *iface,
                                  const char *path, uint64_t offset,
                                  const void *data, size_t size);
static int mock_storage_fsync(storage_interface_t *iface, const char *path);
static int mock_storage_create_file(storage_interface_t *iface,
                                   const char *path, uint64_t initial_size);
static int mock_storage_delete_file(storage_interface_t *iface, const char *path);
static int mock_storage_get_file_info(storage_interface_t *iface,
                                     const char *path,
                                     storage_file_info_t *info);
static int mock_storage_get_stats(storage_interface_t *iface,
                                 storage_stats_t *stats);
static int mock_storage_reset_stats(storage_interface_t *iface);

/**
 * 创建Mock存储接口
 */
storage_interface_t* create_mock_storage_interface(void) {
    storage_interface_t *iface = malloc(sizeof(storage_interface_t));
    if (!iface) {
        return NULL;
    }
    
    memset(iface, 0, sizeof(storage_interface_t));
    
    // 设置基本信息
    iface->type = STORAGE_BACKEND_MOCK;
    strncpy(iface->name, "MockStorage", sizeof(iface->name) - 1);
    
    // 设置函数指针
    iface->init = mock_storage_init;
    iface->cleanup = mock_storage_cleanup;
    iface->health_check = mock_storage_health_check;
    iface->read_sync = mock_storage_read_sync;
    iface->write_sync = mock_storage_write_sync;
    iface->fsync = mock_storage_fsync;
    iface->create_file = mock_storage_create_file;
    iface->delete_file = mock_storage_delete_file;
    iface->get_file_info = mock_storage_get_file_info;
    iface->get_stats = mock_storage_get_stats;
    iface->reset_stats = mock_storage_reset_stats;
    
    // 异步I/O暂时不实现
    iface->read_async = NULL;
    iface->write_async = NULL;
    iface->batch_operations = NULL;
    
    return iface;
}

/**
 * 查找文件
 */
static mock_file_t* find_file(mock_storage_private_t *priv, const char *path) {
    mock_file_t *file = priv->files;
    while (file) {
        if (strcmp(file->path, path) == 0) {
            return file;
        }
        file = file->next;
    }
    return NULL;
}

/**
 * 创建新文件
 */
static mock_file_t* create_new_file(mock_storage_private_t *priv, 
                                   const char *path, size_t initial_size) {
    if (priv->file_count >= priv->max_files) {
        return NULL; // 文件数量限制
    }
    
    mock_file_t *file = malloc(sizeof(mock_file_t));
    if (!file) {
        return NULL;
    }
    
    memset(file, 0, sizeof(mock_file_t));
    strncpy(file->path, path, sizeof(file->path) - 1);
    
    if (initial_size > 0) {
        file->capacity = initial_size;
        file->data = malloc(initial_size);
        if (!file->data) {
            free(file);
            return NULL;
        }
        memset(file->data, 0, initial_size);
        file->size = initial_size;
    }
    
    uint64_t now = time(NULL);
    file->created_time = now;
    file->modified_time = now;
    
    // 添加到链表头部
    file->next = priv->files;
    priv->files = file;
    priv->file_count++;
    
    return file;
}

/**
 * 删除文件
 */
static int remove_file(mock_storage_private_t *priv, const char *path) {
    mock_file_t **current = &priv->files;
    
    while (*current) {
        if (strcmp((*current)->path, path) == 0) {
            mock_file_t *to_delete = *current;
            *current = (*current)->next;
            
            if (to_delete->data) {
                free(to_delete->data);
            }
            free(to_delete);
            priv->file_count--;
            return XDEVICE_OK;
        }
        current = &(*current)->next;
    }
    
    return XDEVICE_ERROR_NOT_FOUND;
}

/**
 * 扩展文件大小
 */
static int expand_file(mock_file_t *file, size_t new_size) {
    if (new_size <= file->capacity) {
        if (new_size > file->size) {
            // 清零新增部分
            memset((char*)file->data + file->size, 0, new_size - file->size);
            file->size = new_size;
        }
        return XDEVICE_OK;
    }
    
    // 需要重新分配内存
    size_t new_capacity = new_size * 2; // 预留一些空间
    void *new_data = realloc(file->data, new_capacity);
    if (!new_data) {
        return XDEVICE_ERROR_OUT_OF_MEMORY;
    }
    
    // 清零新增部分
    memset((char*)new_data + file->size, 0, new_capacity - file->size);
    
    file->data = new_data;
    file->capacity = new_capacity;
    file->size = new_size;
    
    return XDEVICE_OK;
}

/**
 * 模拟延迟
 */
static void simulate_delay(uint32_t delay_us) {
    if (delay_us > 0) {
        struct timespec ts;
        ts.tv_sec = delay_us / 1000000;
        ts.tv_nsec = (delay_us % 1000000) * 1000;
        nanosleep(&ts, NULL);
    }
}

/**
 * 检查是否应该注入错误
 */
static bool should_inject_error(mock_storage_private_t *priv) {
    if (priv->error_rate_percent == 0) {
        return false;
    }
    
    return (rand() % 100) < priv->error_rate_percent;
}

/**
 * Mock存储初始化
 */
static int mock_storage_init(storage_interface_t *iface, const char *config) {
    if (!iface) {
        return XDEVICE_ERROR_INVALID_PARAM;
    }
    
    mock_storage_private_t *priv = malloc(sizeof(mock_storage_private_t));
    if (!priv) {
        return XDEVICE_ERROR_OUT_OF_MEMORY;
    }
    
    memset(priv, 0, sizeof(mock_storage_private_t));
    
    // 默认配置
    priv->max_file_size = 1024 * 1024 * 1024; // 1GB
    priv->max_files = 1000;
    priv->read_delay_us = 0;
    priv->write_delay_us = 0;
    priv->inject_read_errors = false;
    priv->inject_write_errors = false;
    priv->error_rate_percent = 0;
    
    // 解析配置（简单实现）
    if (config) {
        // TODO: 解析配置字符串
    }
    
    // 初始化锁
    if (pthread_mutex_init(&priv->files_lock, NULL) != 0) {
        free(priv);
        return XDEVICE_ERROR;
    }
    
    if (pthread_mutex_init(&priv->stats_lock, NULL) != 0) {
        pthread_mutex_destroy(&priv->files_lock);
        free(priv);
        return XDEVICE_ERROR;
    }
    
    iface->private_data = priv;
    
    printf("[MockStorage] Mock存储初始化完成\n");
    return XDEVICE_OK;
}

/**
 * Mock存储清理
 */
static int mock_storage_cleanup(storage_interface_t *iface) {
    if (!iface || !iface->private_data) {
        return XDEVICE_ERROR_INVALID_PARAM;
    }
    
    mock_storage_private_t *priv = (mock_storage_private_t*)iface->private_data;
    
    // 删除所有文件
    mock_file_t *file = priv->files;
    while (file) {
        mock_file_t *next = file->next;
        if (file->data) {
            free(file->data);
        }
        free(file);
        file = next;
    }
    
    // 销毁锁
    pthread_mutex_destroy(&priv->stats_lock);
    pthread_mutex_destroy(&priv->files_lock);
    
    free(priv);
    iface->private_data = NULL;
    
    printf("[MockStorage] Mock存储清理完成\n");
    return XDEVICE_OK;
}

/**
 * 健康检查
 */
static int mock_storage_health_check(storage_interface_t *iface) {
    if (!iface || !iface->private_data) {
        return XDEVICE_ERROR_INVALID_PARAM;
    }
    
    return XDEVICE_OK; // Mock存储总是健康的
}

/**
 * 同步读取
 */
static int mock_storage_read_sync(storage_interface_t *iface,
                                 const char *path, uint64_t offset,
                                 void *buffer, size_t size) {
    if (!iface || !iface->private_data || !path || !buffer || size == 0) {
        return XDEVICE_ERROR_INVALID_PARAM;
    }
    
    mock_storage_private_t *priv = (mock_storage_private_t*)iface->private_data;
    
    // 检查错误注入
    if (priv->inject_read_errors && should_inject_error(priv)) {
        pthread_mutex_lock(&priv->stats_lock);
        priv->stats.error_count++;
        pthread_mutex_unlock(&priv->stats_lock);
        return XDEVICE_ERROR_IO;
    }
    
    // 模拟延迟
    simulate_delay(priv->read_delay_us);
    
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    
    pthread_mutex_lock(&priv->files_lock);
    
    mock_file_t *file = find_file(priv, path);
    if (!file) {
        pthread_mutex_unlock(&priv->files_lock);
        return XDEVICE_ERROR_NOT_FOUND;
    }
    
    if (offset >= file->size) {
        pthread_mutex_unlock(&priv->files_lock);
        return XDEVICE_ERROR_NOT_FOUND;
    }
    
    size_t read_size = size;
    if (offset + size > file->size) {
        read_size = file->size - offset;
    }
    
    memcpy(buffer, (char*)file->data + offset, read_size);
    
    if (read_size < size) {
        memset((char*)buffer + read_size, 0, size - read_size);
    }
    
    pthread_mutex_unlock(&priv->files_lock);
    
    clock_gettime(CLOCK_MONOTONIC, &end);
    uint64_t latency_ns = (end.tv_sec - start.tv_sec) * 1000000000UL + 
                          (end.tv_nsec - start.tv_nsec);
    
    // 更新统计
    pthread_mutex_lock(&priv->stats_lock);
    priv->stats.read_ops++;
    priv->stats.read_bytes += read_size;
    priv->stats.read_latency_avg_ns = 
        (priv->stats.read_latency_avg_ns + latency_ns) / 2;
    pthread_mutex_unlock(&priv->stats_lock);
    
    return XDEVICE_OK;
}

/**
 * 同步写入
 */
static int mock_storage_write_sync(storage_interface_t *iface,
                                  const char *path, uint64_t offset,
                                  const void *data, size_t size) {
    if (!iface || !iface->private_data || !path || !data || size == 0) {
        return XDEVICE_ERROR_INVALID_PARAM;
    }
    
    mock_storage_private_t *priv = (mock_storage_private_t*)iface->private_data;
    
    // 检查错误注入
    if (priv->inject_write_errors && should_inject_error(priv)) {
        pthread_mutex_lock(&priv->stats_lock);
        priv->stats.error_count++;
        pthread_mutex_unlock(&priv->stats_lock);
        return XDEVICE_ERROR_IO;
    }
    
    // 模拟延迟
    simulate_delay(priv->write_delay_us);
    
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    
    pthread_mutex_lock(&priv->files_lock);
    
    mock_file_t *file = find_file(priv, path);
    if (!file) {
        // 创建新文件
        file = create_new_file(priv, path, offset + size);
        if (!file) {
            pthread_mutex_unlock(&priv->files_lock);
            return XDEVICE_ERROR_OUT_OF_MEMORY;
        }
    }
    
    // 检查是否需要扩展文件
    if (offset + size > file->size) {
        if (expand_file(file, offset + size) != XDEVICE_OK) {
            pthread_mutex_unlock(&priv->files_lock);
            return XDEVICE_ERROR_OUT_OF_MEMORY;
        }
    }
    
    // 检查文件大小限制
    if (file->size > priv->max_file_size) {
        pthread_mutex_unlock(&priv->files_lock);
        return XDEVICE_ERROR_NO_SPACE;
    }
    
    memcpy((char*)file->data + offset, data, size);
    file->modified_time = time(NULL);
    
    pthread_mutex_unlock(&priv->files_lock);
    
    clock_gettime(CLOCK_MONOTONIC, &end);
    uint64_t latency_ns = (end.tv_sec - start.tv_sec) * 1000000000UL + 
                          (end.tv_nsec - start.tv_nsec);
    
    // 更新统计
    pthread_mutex_lock(&priv->stats_lock);
    priv->stats.write_ops++;
    priv->stats.write_bytes += size;
    priv->stats.write_latency_avg_ns = 
        (priv->stats.write_latency_avg_ns + latency_ns) / 2;
    pthread_mutex_unlock(&priv->stats_lock);
    
    return XDEVICE_OK;
}

/**
 * 文件同步
 */
static int mock_storage_fsync(storage_interface_t *iface, const char *path) {
    if (!iface || !iface->private_data || !path) {
        return XDEVICE_ERROR_INVALID_PARAM;
    }
    
    // Mock存储不需要实际同步，直接返回成功
    return XDEVICE_OK;
}

/**
 * 创建文件
 */
static int mock_storage_create_file(storage_interface_t *iface,
                                   const char *path, uint64_t initial_size) {
    if (!iface || !iface->private_data || !path) {
        return XDEVICE_ERROR_INVALID_PARAM;
    }
    
    mock_storage_private_t *priv = (mock_storage_private_t*)iface->private_data;
    
    pthread_mutex_lock(&priv->files_lock);
    
    // 检查文件是否已存在
    if (find_file(priv, path)) {
        pthread_mutex_unlock(&priv->files_lock);
        return XDEVICE_ERROR_EXISTS;
    }
    
    mock_file_t *file = create_new_file(priv, path, initial_size);
    
    pthread_mutex_unlock(&priv->files_lock);
    
    if (!file) {
        return XDEVICE_ERROR_OUT_OF_MEMORY;
    }
    
    return XDEVICE_OK;
}

/**
 * 删除文件
 */
static int mock_storage_delete_file(storage_interface_t *iface, const char *path) {
    if (!iface || !iface->private_data || !path) {
        return XDEVICE_ERROR_INVALID_PARAM;
    }
    
    mock_storage_private_t *priv = (mock_storage_private_t*)iface->private_data;
    
    pthread_mutex_lock(&priv->files_lock);
    int ret = remove_file(priv, path);
    pthread_mutex_unlock(&priv->files_lock);
    
    return ret;
}

/**
 * 获取文件信息
 */
static int mock_storage_get_file_info(storage_interface_t *iface,
                                     const char *path,
                                     storage_file_info_t *info) {
    if (!iface || !iface->private_data || !path || !info) {
        return XDEVICE_ERROR_INVALID_PARAM;
    }
    
    mock_storage_private_t *priv = (mock_storage_private_t*)iface->private_data;
    
    pthread_mutex_lock(&priv->files_lock);
    
    mock_file_t *file = find_file(priv, path);
    if (!file) {
        pthread_mutex_unlock(&priv->files_lock);
        return XDEVICE_ERROR_NOT_FOUND;
    }
    
    info->size = file->size;
    info->created_time = file->created_time;
    info->modified_time = file->modified_time;
    info->permissions = 0644;
    info->is_directory = false;
    
    pthread_mutex_unlock(&priv->files_lock);
    
    return XDEVICE_OK;
}

/**
 * 获取统计信息
 */
static int mock_storage_get_stats(storage_interface_t *iface,
                                 storage_stats_t *stats) {
    if (!iface || !iface->private_data || !stats) {
        return XDEVICE_ERROR_INVALID_PARAM;
    }
    
    mock_storage_private_t *priv = (mock_storage_private_t*)iface->private_data;
    
    pthread_mutex_lock(&priv->stats_lock);
    *stats = priv->stats;
    pthread_mutex_unlock(&priv->stats_lock);
    
    return XDEVICE_OK;
}

/**
 * 重置统计信息
 */
static int mock_storage_reset_stats(storage_interface_t *iface) {
    if (!iface || !iface->private_data) {
        return XDEVICE_ERROR_INVALID_PARAM;
    }
    
    mock_storage_private_t *priv = (mock_storage_private_t*)iface->private_data;
    
    pthread_mutex_lock(&priv->stats_lock);
    memset(&priv->stats, 0, sizeof(storage_stats_t));
    pthread_mutex_unlock(&priv->stats_lock);
    
    return XDEVICE_OK;
}

/**
 * Mock存储配置接口
 */
void mock_storage_set_delays(storage_interface_t *iface,
                            uint32_t read_delay_us, uint32_t write_delay_us) {
    if (!iface || !iface->private_data || iface->type != STORAGE_BACKEND_MOCK) {
        return;
    }
    
    mock_storage_private_t *priv = (mock_storage_private_t*)iface->private_data;
    priv->read_delay_us = read_delay_us;
    priv->write_delay_us = write_delay_us;
}

void mock_storage_set_error_injection(storage_interface_t *iface,
                                     bool inject_read_errors,
                                     bool inject_write_errors,
                                     uint32_t error_rate_percent) {
    if (!iface || !iface->private_data || iface->type != STORAGE_BACKEND_MOCK) {
        return;
    }
    
    mock_storage_private_t *priv = (mock_storage_private_t*)iface->private_data;
    priv->inject_read_errors = inject_read_errors;
    priv->inject_write_errors = inject_write_errors;
    priv->error_rate_percent = error_rate_percent;
}
