#include "../include/xdevice.h"
#include "../include/wal_engine.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <errno.h>
#include <pthread.h>

struct xdevice_wal {
    int fd;
    char* mmap_region;
    size_t file_size;
    size_t current_offset;
    uint64_t next_lsn;
    
    pthread_mutex_t write_mutex;
    pthread_cond_t sync_cond;
    
    char wal_file_path[256];
    bool is_syncing;
    uint64_t last_sync_lsn;
    
    // 缓冲区用于批量写入
    char* write_buffer;
    size_t buffer_size;
    size_t buffer_used;
    
    // 统计信息
    uint64_t total_writes;
    uint64_t total_syncs;
    uint64_t total_bytes_written;
};

static const char* WAL_MAGIC = "XDEV_WAL";
static const uint32_t WAL_VERSION = 1;

typedef struct {
    char magic[8];
    uint32_t version;
    uint64_t created_time;
    uint64_t file_size;
    uint32_t checksum;
} wal_header_t;

typedef struct {
    uint64_t lsn;
    uint32_t length;
    uint32_t checksum;
    uint64_t timestamp;
    xdevice_wal_op_type_t op_type;
    char device_name[XDEVICE_MAX_DEVICE_NAME_LEN];
    // 数据跟在header后面
} wal_record_header_t;

// CRC32校验和计算
static uint32_t crc32(const void* data, size_t length) {
    static uint32_t crc_table[256];
    static bool table_initialized = false;
    
    if (!table_initialized) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t crc = i;
            for (int j = 0; j < 8; j++) {
                if (crc & 1) {
                    crc = (crc >> 1) ^ 0xEDB88320;
                } else {
                    crc >>= 1;
                }
            }
            crc_table[i] = crc;
        }
        table_initialized = true;
    }
    
    uint32_t crc = 0xFFFFFFFF;
    const uint8_t* bytes = (const uint8_t*)data;
    
    for (size_t i = 0; i < length; i++) {
        crc = crc_table[(crc ^ bytes[i]) & 0xFF] ^ (crc >> 8);
    }
    
    return crc ^ 0xFFFFFFFF;
}

// 创建WAL文件头
static void create_wal_header(wal_header_t* header, uint64_t file_size) {
    memcpy(header->magic, WAL_MAGIC, 8);
    header->version = WAL_VERSION;
    header->created_time = time(NULL);
    header->file_size = file_size;
    header->checksum = crc32(header, sizeof(wal_header_t) - sizeof(uint32_t));
}

// 验证WAL文件头
static bool validate_wal_header(const wal_header_t* header) {
    if (memcmp(header->magic, WAL_MAGIC, 8) != 0) {
        return false;
    }
    
    if (header->version != WAL_VERSION) {
        return false;
    }
    
    uint32_t calculated_checksum = crc32(header, sizeof(wal_header_t) - sizeof(uint32_t));
    return calculated_checksum == header->checksum;
}

xdevice_wal_t* xdevice_wal_create(const char* wal_file, size_t initial_size) {
    if (!wal_file || initial_size == 0) {
        return NULL;
    }
    
    xdevice_wal_t* wal = malloc(sizeof(xdevice_wal_t));
    if (!wal) {
        return NULL;
    }
    
    memset(wal, 0, sizeof(xdevice_wal_t));
    
    // 初始化互斥锁和条件变量
    if (pthread_mutex_init(&wal->write_mutex, NULL) != 0) {
        free(wal);
        return NULL;
    }
    
    if (pthread_cond_init(&wal->sync_cond, NULL) != 0) {
        pthread_mutex_destroy(&wal->write_mutex);
        free(wal);
        return NULL;
    }
    
    strncpy(wal->wal_file_path, wal_file, sizeof(wal->wal_file_path) - 1);
    wal->file_size = initial_size;
    wal->next_lsn = 1;
    wal->buffer_size = XDEVICE_WAL_BUFFER_SIZE;
    
    // 分配写缓冲区
    wal->write_buffer = malloc(wal->buffer_size);
    if (!wal->write_buffer) {
        pthread_cond_destroy(&wal->sync_cond);
        pthread_mutex_destroy(&wal->write_mutex);
        free(wal);
        return NULL;
    }
    
    // 创建或打开WAL文件
    wal->fd = open(wal_file, O_CREAT | O_RDWR, 0644);
    if (wal->fd == -1) {
        free(wal->write_buffer);
        pthread_cond_destroy(&wal->sync_cond);
        pthread_mutex_destroy(&wal->write_mutex);
        free(wal);
        return NULL;
    }
    
    // 检查文件是否为新文件
    struct stat st;
    if (fstat(wal->fd, &st) == 0 && st.st_size == 0) {
        // 新文件，写入头部并扩展到指定大小
        if (ftruncate(wal->fd, initial_size) == -1) {
            close(wal->fd);
            free(wal->write_buffer);
            pthread_cond_destroy(&wal->sync_cond);
            pthread_mutex_destroy(&wal->write_mutex);
            free(wal);
            return NULL;
        }
        
        wal_header_t header;
        create_wal_header(&header, initial_size);
        
        if (write(wal->fd, &header, sizeof(header)) != sizeof(header)) {
            close(wal->fd);
            free(wal->write_buffer);
            pthread_cond_destroy(&wal->sync_cond);
            pthread_mutex_destroy(&wal->write_mutex);
            free(wal);
            return NULL;
        }
        
        wal->current_offset = sizeof(wal_header_t);
    } else {
        // 现有文件，验证头部并恢复状态
        wal_header_t header;
        if (read(wal->fd, &header, sizeof(header)) != sizeof(header) ||
            !validate_wal_header(&header)) {
            close(wal->fd);
            free(wal->write_buffer);
            pthread_cond_destroy(&wal->sync_cond);
            pthread_mutex_destroy(&wal->write_mutex);
            free(wal);
            return NULL;
        }
        
        wal->file_size = header.file_size;
        wal->current_offset = sizeof(wal_header_t);
        
        // 扫描现有记录以恢复next_lsn
        // TODO: 实现WAL恢复逻辑
    }
    
    // 内存映射文件
    wal->mmap_region = mmap(NULL, wal->file_size, PROT_READ | PROT_WRITE, 
                           MAP_SHARED, wal->fd, 0);
    if (wal->mmap_region == MAP_FAILED) {
        close(wal->fd);
        free(wal->write_buffer);
        pthread_cond_destroy(&wal->sync_cond);
        pthread_mutex_destroy(&wal->write_mutex);
        free(wal);
        return NULL;
    }
    
    return wal;
}

void xdevice_wal_destroy(xdevice_wal_t* wal) {
    if (!wal) {
        return;
    }
    
    // 同步所有未写入的数据
    xdevice_wal_sync(wal);
    
    // 清理资源
    if (wal->mmap_region && wal->mmap_region != MAP_FAILED) {
        munmap(wal->mmap_region, wal->file_size);
    }
    
    if (wal->fd != -1) {
        close(wal->fd);
    }
    
    if (wal->write_buffer) {
        free(wal->write_buffer);
    }
    
    pthread_cond_destroy(&wal->sync_cond);
    pthread_mutex_destroy(&wal->write_mutex);
    
    free(wal);
}

xdevice_error_t xdevice_wal_append(xdevice_wal_t* wal, 
                                   const char* device_name,
                                   xdevice_wal_op_type_t op_type,
                                   const void* data, 
                                   size_t length) {
    if (!wal || !device_name || (!data && length > 0)) {
        return XDEVICE_ERROR_INVALID_PARAM;
    }
    
    pthread_mutex_lock(&wal->write_mutex);
    
    // 构建WAL记录
    wal_record_header_t record_header;
    record_header.lsn = wal->next_lsn++;
    record_header.length = length;
    record_header.timestamp = time(NULL);
    record_header.op_type = op_type;
    strncpy(record_header.device_name, device_name, 
            sizeof(record_header.device_name) - 1);
    
    // 计算校验和（包括头部和数据）
    uint32_t checksum = crc32(&record_header, sizeof(record_header) - sizeof(uint32_t));
    if (data && length > 0) {
        checksum ^= crc32(data, length);
    }
    record_header.checksum = checksum;
    
    size_t total_record_size = sizeof(wal_record_header_t) + length;
    
    // 检查空间是否足够
    if (wal->current_offset + total_record_size > wal->file_size) {
        pthread_mutex_unlock(&wal->write_mutex);
        return XDEVICE_ERROR_IO;
    }
    
    // 写入记录头
    memcpy(wal->mmap_region + wal->current_offset, &record_header, 
           sizeof(wal_record_header_t));
    wal->current_offset += sizeof(wal_record_header_t);
    
    // 写入数据
    if (data && length > 0) {
        memcpy(wal->mmap_region + wal->current_offset, data, length);
        wal->current_offset += length;
    }
    
    // 更新统计信息
    wal->total_writes++;
    wal->total_bytes_written += total_record_size;
    
    pthread_mutex_unlock(&wal->write_mutex);
    
    return XDEVICE_OK;
}

xdevice_error_t xdevice_wal_sync(xdevice_wal_t* wal) {
    if (!wal) {
        return XDEVICE_ERROR_INVALID_PARAM;
    }
    
    pthread_mutex_lock(&wal->write_mutex);
    
    wal->is_syncing = true;
    
    // 同步内存映射区域到磁盘
    if (msync(wal->mmap_region, wal->current_offset, MS_SYNC) == -1) {
        wal->is_syncing = false;
        pthread_mutex_unlock(&wal->write_mutex);
        return XDEVICE_ERROR_IO;
    }
    
    // 同步文件描述符
    if (fsync(wal->fd) == -1) {
        wal->is_syncing = false;
        pthread_mutex_unlock(&wal->write_mutex);
        return XDEVICE_ERROR_IO;
    }
    
    wal->last_sync_lsn = wal->next_lsn - 1;
    wal->total_syncs++;
    wal->is_syncing = false;
    
    // 通知等待同步的线程
    pthread_cond_broadcast(&wal->sync_cond);
    
    pthread_mutex_unlock(&wal->write_mutex);
    
    return XDEVICE_OK;
}

xdevice_error_t xdevice_wal_get_stats(xdevice_wal_t* wal, 
                                      xdevice_wal_stats_t* stats) {
    if (!wal || !stats) {
        return XDEVICE_ERROR_INVALID_PARAM;
    }
    
    pthread_mutex_lock(&wal->write_mutex);
    
    stats->total_writes = wal->total_writes;
    stats->total_syncs = wal->total_syncs;
    stats->total_bytes_written = wal->total_bytes_written;
    stats->current_lsn = wal->next_lsn - 1;
    stats->last_sync_lsn = wal->last_sync_lsn;
    stats->file_size = wal->file_size;
    stats->current_offset = wal->current_offset;
    stats->is_syncing = wal->is_syncing;
    
    pthread_mutex_unlock(&wal->write_mutex);
    
    return XDEVICE_OK;
}
