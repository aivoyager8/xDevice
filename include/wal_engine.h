#ifndef WAL_ENGINE_H
#define WAL_ENGINE_H

#include "../xdevice.h"

#ifdef __cplusplus
extern "C" {
#endif

/* WAL引擎统计信息 */
typedef struct {
    uint64_t total_writes;
    uint64_t total_syncs;
    uint64_t total_bytes_written;
    uint64_t current_lsn;
    uint64_t last_sync_lsn;
    uint64_t file_size;
    uint64_t current_offset;
    bool is_syncing;
} xdevice_wal_stats_t;

/**
 * 创建WAL引擎实例
 * @param wal_file WAL文件路径
 * @param initial_size 初始文件大小
 * @return WAL引擎实例，失败返回NULL
 */
xdevice_wal_t* xdevice_wal_create(const char* wal_file, size_t initial_size);

/**
 * 销毁WAL引擎实例
 * @param wal WAL引擎实例
 */
void xdevice_wal_destroy(xdevice_wal_t* wal);

/**
 * 向WAL追加记录
 * @param wal WAL引擎实例
 * @param device_name 设备名称
 * @param op_type 操作类型
 * @param data 数据指针
 * @param length 数据长度
 * @return 错误码
 */
xdevice_error_t xdevice_wal_append(xdevice_wal_t* wal, 
                                   const char* device_name,
                                   xdevice_wal_op_type_t op_type,
                                   const void* data, 
                                   size_t length);

/**
 * 同步WAL到磁盘
 * @param wal WAL引擎实例
 * @return 错误码
 */
xdevice_error_t xdevice_wal_sync(xdevice_wal_t* wal);

/**
 * 获取WAL统计信息
 * @param wal WAL引擎实例
 * @param stats 统计信息输出
 * @return 错误码
 */
xdevice_error_t xdevice_wal_get_stats(xdevice_wal_t* wal, 
                                      xdevice_wal_stats_t* stats);

/**
 * 读取WAL记录
 * @param wal WAL引擎实例
 * @param lsn 日志序列号
 * @param entry 输出的WAL条目
 * @return 错误码
 */
xdevice_error_t xdevice_wal_read(xdevice_wal_t* wal, 
                                 uint64_t lsn,
                                 xdevice_wal_entry_t* entry);

/**
 * 截断WAL文件（删除指定LSN之后的记录）
 * @param wal WAL引擎实例
 * @param lsn 截断点LSN
 * @return 错误码
 */
xdevice_error_t xdevice_wal_truncate(xdevice_wal_t* wal, uint64_t lsn);

/**
 * 检查点操作（创建一致性快照）
 * @param wal WAL引擎实例
 * @return 错误码
 */
xdevice_error_t xdevice_wal_checkpoint(xdevice_wal_t* wal);

#ifdef __cplusplus
}
#endif

#endif /* WAL_ENGINE_H */
