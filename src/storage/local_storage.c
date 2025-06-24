#include "../include/storage_interface.h"
#include "../include/xdevice.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <errno.h>
#include <pthread.h>
#include <time.h>

/* 本地文件存储类型 */
typedef enum {
    LOCAL_STORAGE_REGULAR = 0,     // 普通文件
    LOCAL_STORAGE_MMAP,            // 内存映射
    LOCAL_STORAGE_DIRECT,          // 直接I/O
    LOCAL_STORAGE_TMPFS,           // 内存文件系统
    LOCAL_STORAGE_BLOCK,           // 块设备
    LOCAL_STORAGE_SPARSE,          // 稀疏文件
    LOCAL_STORAGE_COMPRESSED,      // 压缩文件
    LOCAL_STORAGE_HYBRID           // 混合模式
} local_storage_type_t;

/* 本地存储私有数据 */
typedef struct {
    char base_path[MAX_PATH_LEN];  // 基础路径
    local_storage_type_t storage_type; // 存储类型
    
    // 性能配置
    bool use_odirect;              // 使用O_DIRECT
    bool use_osync;                // 使用O_SYNC
    size_t io_buffer_size;         // I/O缓冲区大小
    uint32_t max_open_files;       // 最大打开文件数
    
    // 文件描述符缓存
    struct {
        char path[MAX_PATH_LEN];
        int fd;
        uint64_t last_access;
    } *fd_cache;
    uint32_t fd_cache_size;
    pthread_mutex_t fd_cache_lock;
    
    // 内存映射管理
    struct {
        void *addr;
        size_t size;
        char path[MAX_PATH_LEN];
        uint64_t last_access;
    } *mmap_cache;
    uint32_t mmap_cache_size;
    pthread_mutex_t mmap_cache_lock;
    
    // 统计信息
    storage_stats_t stats;
    pthread_mutex_t stats_lock;
    
} local_storage_private_t;

/* 前向声明 */
static int local_storage_init(storage_interface_t *iface, const char *config);
static int local_storage_cleanup(storage_interface_t *iface);
static int local_storage_health_check(storage_interface_t *iface);
static int local_storage_read_sync(storage_interface_t *iface,
                                  const char *path, uint64_t offset,
                                  void *buffer, size_t size);
static int local_storage_write_sync(storage_interface_t *iface,
                                   const char *path, uint64_t offset,
                                   const void *data, size_t size);
static int local_storage_fsync(storage_interface_t *iface, const char *path);
static int local_storage_create_file(storage_interface_t *iface,
                                    const char *path, uint64_t initial_size);
static int local_storage_delete_file(storage_interface_t *iface, const char *path);
static int local_storage_get_file_info(storage_interface_t *iface,
                                      const char *path,
                                      storage_file_info_t *info);
static int local_storage_get_stats(storage_interface_t *iface,
                                  storage_stats_t *stats);
static int local_storage_reset_stats(storage_interface_t *iface);

/**
 * 创建本地文件存储接口
 */
storage_interface_t* create_local_storage_interface(void) {
    storage_interface_t *iface = malloc(sizeof(storage_interface_t));
    if (!iface) {
        return NULL;
    }
    
    memset(iface, 0, sizeof(storage_interface_t));
    
    // 设置基本信息
    iface->type = STORAGE_BACKEND_LOCAL;
    strncpy(iface->name, "LocalFileStorage", sizeof(iface->name) - 1);
    
    // 设置函数指针
    iface->init = local_storage_init;
    iface->cleanup = local_storage_cleanup;
    iface->health_check = local_storage_health_check;
    iface->read_sync = local_storage_read_sync;
    iface->write_sync = local_storage_write_sync;
    iface->fsync = local_storage_fsync;
    iface->create_file = local_storage_create_file;
    iface->delete_file = local_storage_delete_file;
    iface->get_file_info = local_storage_get_file_info;
    iface->get_stats = local_storage_get_stats;
    iface->reset_stats = local_storage_reset_stats;
    
    // 异步I/O暂时不实现
    iface->read_async = NULL;
    iface->write_async = NULL;
    iface->batch_operations = NULL;
    
    return iface;
}

/**
 * 构建完整文件路径
 */
static void build_file_path(const char *base_path, const char *path,
                           char *full_path, size_t path_size) {
    if (path[0] == '/') {
        // 绝对路径
        strncpy(full_path, path, path_size - 1);
    } else {
        // 相对路径
        snprintf(full_path, path_size, "%s/%s", base_path, path);
    }
    full_path[path_size - 1] = '\0';
}

/**
 * 创建目录结构
 */
static int create_directories(const char *path) {
    char temp_path[MAX_PATH_LEN];
    char *p = NULL;
    size_t len;
    
    strncpy(temp_path, path, sizeof(temp_path) - 1);
    temp_path[sizeof(temp_path) - 1] = '\0';
    
    len = strlen(temp_path);
    if (temp_path[len - 1] == '/') {
        temp_path[len - 1] = '\0';
    }
    
    for (p = temp_path + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(temp_path, S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH) != 0) {
                if (errno != EEXIST) {
                    return XDEVICE_ERROR_IO;
                }
            }
            *p = '/';
        }
    }
    
    if (mkdir(temp_path, S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH) != 0) {
        if (errno != EEXIST) {
            return XDEVICE_ERROR_IO;
        }
    }
    
    return XDEVICE_OK;
}

/**
 * 获取或创建文件描述符
 */
static int get_file_descriptor(local_storage_private_t *priv,
                              const char *path, int flags) {
    pthread_mutex_lock(&priv->fd_cache_lock);
    
    // 查找缓存
    for (uint32_t i = 0; i < priv->fd_cache_size; i++) {
        if (strcmp(priv->fd_cache[i].path, path) == 0) {
            priv->fd_cache[i].last_access = time(NULL);
            int fd = priv->fd_cache[i].fd;
            pthread_mutex_unlock(&priv->fd_cache_lock);
            return fd;
        }
    }
    
    // 打开文件
    int open_flags = flags;
    if (priv->use_odirect) {
        open_flags |= O_DIRECT;
    }
    if (priv->use_osync) {
        open_flags |= O_SYNC;
    }
    
    int fd = open(path, open_flags, 0644);
    if (fd < 0) {
        pthread_mutex_unlock(&priv->fd_cache_lock);
        return -1;
    }
    
    // 添加到缓存（简单实现，实际应该有LRU策略）
    if (priv->fd_cache_size < priv->max_open_files) {
        strncpy(priv->fd_cache[priv->fd_cache_size].path, path, MAX_PATH_LEN - 1);
        priv->fd_cache[priv->fd_cache_size].fd = fd;
        priv->fd_cache[priv->fd_cache_size].last_access = time(NULL);
        priv->fd_cache_size++;
    }
    
    pthread_mutex_unlock(&priv->fd_cache_lock);
    return fd;
}

/**
 * 本地存储初始化
 */
static int local_storage_init(storage_interface_t *iface, const char *config) {
    if (!iface) {
        return XDEVICE_ERROR_INVALID_PARAM;
    }
    
    local_storage_private_t *priv = malloc(sizeof(local_storage_private_t));
    if (!priv) {
        return XDEVICE_ERROR_OUT_OF_MEMORY;
    }
    
    memset(priv, 0, sizeof(local_storage_private_t));
    
    // 解析配置（简单实现）
    if (config) {
        strncpy(priv->base_path, config, sizeof(priv->base_path) - 1);
    } else {
        strcpy(priv->base_path, "/tmp/xdevice");
    }
    
    // 默认配置
    priv->storage_type = LOCAL_STORAGE_REGULAR;
    priv->use_odirect = false;
    priv->use_osync = false;
    priv->io_buffer_size = 64 * 1024; // 64KB
    priv->max_open_files = 100;
    
    // 创建基础目录
    if (create_directories(priv->base_path) != XDEVICE_OK) {
        free(priv);
        return XDEVICE_ERROR_IO;
    }
    
    // 初始化文件描述符缓存
    priv->fd_cache = malloc(priv->max_open_files * sizeof(*priv->fd_cache));
    if (!priv->fd_cache) {
        free(priv);
        return XDEVICE_ERROR_OUT_OF_MEMORY;
    }
    
    if (pthread_mutex_init(&priv->fd_cache_lock, NULL) != 0) {
        free(priv->fd_cache);
        free(priv);
        return XDEVICE_ERROR;
    }
    
    // 初始化内存映射缓存
    priv->mmap_cache = malloc(priv->max_open_files * sizeof(*priv->mmap_cache));
    if (!priv->mmap_cache) {
        pthread_mutex_destroy(&priv->fd_cache_lock);
        free(priv->fd_cache);
        free(priv);
        return XDEVICE_ERROR_OUT_OF_MEMORY;
    }
    
    if (pthread_mutex_init(&priv->mmap_cache_lock, NULL) != 0) {
        free(priv->mmap_cache);
        pthread_mutex_destroy(&priv->fd_cache_lock);
        free(priv->fd_cache);
        free(priv);
        return XDEVICE_ERROR;
    }
    
    // 初始化统计锁
    if (pthread_mutex_init(&priv->stats_lock, NULL) != 0) {
        pthread_mutex_destroy(&priv->mmap_cache_lock);
        free(priv->mmap_cache);
        pthread_mutex_destroy(&priv->fd_cache_lock);
        free(priv->fd_cache);
        free(priv);
        return XDEVICE_ERROR;
    }
    
    iface->private_data = priv;
    
    printf("[LocalStorage] 本地存储初始化完成，基础路径: %s\n", priv->base_path);
    return XDEVICE_OK;
}

/**
 * 本地存储清理
 */
static int local_storage_cleanup(storage_interface_t *iface) {
    if (!iface || !iface->private_data) {
        return XDEVICE_ERROR_INVALID_PARAM;
    }
    
    local_storage_private_t *priv = (local_storage_private_t*)iface->private_data;
    
    // 关闭所有文件描述符
    for (uint32_t i = 0; i < priv->fd_cache_size; i++) {
        if (priv->fd_cache[i].fd >= 0) {
            close(priv->fd_cache[i].fd);
        }
    }
    
    // 释放内存映射
    for (uint32_t i = 0; i < priv->mmap_cache_size; i++) {
        if (priv->mmap_cache[i].addr) {
            munmap(priv->mmap_cache[i].addr, priv->mmap_cache[i].size);
        }
    }
    
    // 销毁锁
    pthread_mutex_destroy(&priv->stats_lock);
    pthread_mutex_destroy(&priv->mmap_cache_lock);
    pthread_mutex_destroy(&priv->fd_cache_lock);
    
    // 释放内存
    free(priv->mmap_cache);
    free(priv->fd_cache);
    free(priv);
    
    iface->private_data = NULL;
    
    printf("[LocalStorage] 本地存储清理完成\n");
    return XDEVICE_OK;
}

/**
 * 健康检查
 */
static int local_storage_health_check(storage_interface_t *iface) {
    if (!iface || !iface->private_data) {
        return XDEVICE_ERROR_INVALID_PARAM;
    }
    
    local_storage_private_t *priv = (local_storage_private_t*)iface->private_data;
    
    // 检查基础目录是否可访问
    if (access(priv->base_path, R_OK | W_OK) != 0) {
        return XDEVICE_ERROR_IO;
    }
    
    return XDEVICE_OK;
}

/**
 * 同步读取
 */
static int local_storage_read_sync(storage_interface_t *iface,
                                  const char *path, uint64_t offset,
                                  void *buffer, size_t size) {
    if (!iface || !iface->private_data || !path || !buffer || size == 0) {
        return XDEVICE_ERROR_INVALID_PARAM;
    }
    
    local_storage_private_t *priv = (local_storage_private_t*)iface->private_data;
    char full_path[MAX_PATH_LEN];
    
    build_file_path(priv->base_path, path, full_path, sizeof(full_path));
    
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    
    int fd = get_file_descriptor(priv, full_path, O_RDONLY);
    if (fd < 0) {
        return XDEVICE_ERROR_IO;
    }
    
    if (lseek(fd, offset, SEEK_SET) != (off_t)offset) {
        return XDEVICE_ERROR_IO;
    }
    
    ssize_t bytes_read = read(fd, buffer, size);
    if (bytes_read != (ssize_t)size) {
        if (bytes_read < 0) {
            return XDEVICE_ERROR_IO;
        } else {
            return XDEVICE_ERROR_NOT_FOUND; // 文件不够大
        }
    }
    
    clock_gettime(CLOCK_MONOTONIC, &end);
    uint64_t latency_ns = (end.tv_sec - start.tv_sec) * 1000000000UL + 
                          (end.tv_nsec - start.tv_nsec);
    
    // 更新统计
    pthread_mutex_lock(&priv->stats_lock);
    priv->stats.read_ops++;
    priv->stats.read_bytes += size;
    priv->stats.read_latency_avg_ns = 
        (priv->stats.read_latency_avg_ns + latency_ns) / 2;
    pthread_mutex_unlock(&priv->stats_lock);
    
    return XDEVICE_OK;
}

/**
 * 同步写入
 */
static int local_storage_write_sync(storage_interface_t *iface,
                                   const char *path, uint64_t offset,
                                   const void *data, size_t size) {
    if (!iface || !iface->private_data || !path || !data || size == 0) {
        return XDEVICE_ERROR_INVALID_PARAM;
    }
    
    local_storage_private_t *priv = (local_storage_private_t*)iface->private_data;
    char full_path[MAX_PATH_LEN];
    
    build_file_path(priv->base_path, path, full_path, sizeof(full_path));
    
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    
    // 创建目录结构
    char dir_path[MAX_PATH_LEN];
    strncpy(dir_path, full_path, sizeof(dir_path) - 1);
    char *last_slash = strrchr(dir_path, '/');
    if (last_slash) {
        *last_slash = '\0';
        create_directories(dir_path);
    }
    
    int fd = get_file_descriptor(priv, full_path, O_WRONLY | O_CREAT);
    if (fd < 0) {
        return XDEVICE_ERROR_IO;
    }
    
    if (lseek(fd, offset, SEEK_SET) != (off_t)offset) {
        return XDEVICE_ERROR_IO;
    }
    
    ssize_t bytes_written = write(fd, data, size);
    if (bytes_written != (ssize_t)size) {
        return XDEVICE_ERROR_IO;
    }
    
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
static int local_storage_fsync(storage_interface_t *iface, const char *path) {
    if (!iface || !iface->private_data || !path) {
        return XDEVICE_ERROR_INVALID_PARAM;
    }
    
    local_storage_private_t *priv = (local_storage_private_t*)iface->private_data;
    char full_path[MAX_PATH_LEN];
    
    build_file_path(priv->base_path, path, full_path, sizeof(full_path));
    
    int fd = get_file_descriptor(priv, full_path, O_WRONLY);
    if (fd < 0) {
        return XDEVICE_ERROR_IO;
    }
    
    if (fsync(fd) != 0) {
        return XDEVICE_ERROR_IO;
    }
    
    return XDEVICE_OK;
}

/**
 * 创建文件
 */
static int local_storage_create_file(storage_interface_t *iface,
                                    const char *path, uint64_t initial_size) {
    if (!iface || !iface->private_data || !path) {
        return XDEVICE_ERROR_INVALID_PARAM;
    }
    
    local_storage_private_t *priv = (local_storage_private_t*)iface->private_data;
    char full_path[MAX_PATH_LEN];
    
    build_file_path(priv->base_path, path, full_path, sizeof(full_path));
    
    // 创建目录结构
    char dir_path[MAX_PATH_LEN];
    strncpy(dir_path, full_path, sizeof(dir_path) - 1);
    char *last_slash = strrchr(dir_path, '/');
    if (last_slash) {
        *last_slash = '\0';
        create_directories(dir_path);
    }
    
    int fd = open(full_path, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0) {
        if (errno == EEXIST) {
            return XDEVICE_ERROR_EXISTS;
        }
        return XDEVICE_ERROR_IO;
    }
    
    if (initial_size > 0) {
        if (ftruncate(fd, initial_size) != 0) {
            close(fd);
            unlink(full_path);
            return XDEVICE_ERROR_IO;
        }
    }
    
    close(fd);
    return XDEVICE_OK;
}

/**
 * 删除文件
 */
static int local_storage_delete_file(storage_interface_t *iface, const char *path) {
    if (!iface || !iface->private_data || !path) {
        return XDEVICE_ERROR_INVALID_PARAM;
    }
    
    local_storage_private_t *priv = (local_storage_private_t*)iface->private_data;
    char full_path[MAX_PATH_LEN];
    
    build_file_path(priv->base_path, path, full_path, sizeof(full_path));
    
    if (unlink(full_path) != 0) {
        if (errno == ENOENT) {
            return XDEVICE_ERROR_NOT_FOUND;
        }
        return XDEVICE_ERROR_IO;
    }
    
    return XDEVICE_OK;
}

/**
 * 获取文件信息
 */
static int local_storage_get_file_info(storage_interface_t *iface,
                                      const char *path,
                                      storage_file_info_t *info) {
    if (!iface || !iface->private_data || !path || !info) {
        return XDEVICE_ERROR_INVALID_PARAM;
    }
    
    local_storage_private_t *priv = (local_storage_private_t*)iface->private_data;
    char full_path[MAX_PATH_LEN];
    
    build_file_path(priv->base_path, path, full_path, sizeof(full_path));
    
    struct stat st;
    if (stat(full_path, &st) != 0) {
        if (errno == ENOENT) {
            return XDEVICE_ERROR_NOT_FOUND;
        }
        return XDEVICE_ERROR_IO;
    }
    
    info->size = st.st_size;
    info->created_time = st.st_ctime;
    info->modified_time = st.st_mtime;
    info->permissions = st.st_mode & 0777;
    info->is_directory = S_ISDIR(st.st_mode);
    
    return XDEVICE_OK;
}

/**
 * 获取统计信息
 */
static int local_storage_get_stats(storage_interface_t *iface,
                                  storage_stats_t *stats) {
    if (!iface || !iface->private_data || !stats) {
        return XDEVICE_ERROR_INVALID_PARAM;
    }
    
    local_storage_private_t *priv = (local_storage_private_t*)iface->private_data;
    
    pthread_mutex_lock(&priv->stats_lock);
    *stats = priv->stats;
    pthread_mutex_unlock(&priv->stats_lock);
    
    return XDEVICE_OK;
}

/**
 * 重置统计信息
 */
static int local_storage_reset_stats(storage_interface_t *iface) {
    if (!iface || !iface->private_data) {
        return XDEVICE_ERROR_INVALID_PARAM;
    }
    
    local_storage_private_t *priv = (local_storage_private_t*)iface->private_data;
    
    pthread_mutex_lock(&priv->stats_lock);
    memset(&priv->stats, 0, sizeof(storage_stats_t));
    pthread_mutex_unlock(&priv->stats_lock);
    
    return XDEVICE_OK;
}
