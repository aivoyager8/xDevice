# WAL引擎详细设计

## 1. 设计目标

### 1.1 性能目标
- **写入延迟**：< 1ms (99th percentile)
- **写入吞吐量**：> 100,000 ops/sec
- **批量写入**：支持多达1000个操作的批量提交
- **恢复速度**：< 10秒恢复1GB的WAL数据

### 1.2 可靠性目标
- **数据持久性**：99.999999% (8个9)
- **一致性**：强一致性，支持原子操作
- **故障恢复**：自动检测和恢复损坏的WAL文件

## 2. WAL文件格式设计

### 2.1 WAL段文件结构

```
┌─────────────────────────────────────┐
│          WAL Segment File           │
├─────────────────────────────────────┤
│  File Header (4KB)                  │ ← 段文件头部
├─────────────────────────────────────┤
│  Entry 1 | Header + Data            │
├─────────────────────────────────────┤
│  Entry 2 | Header + Data            │
├─────────────────────────────────────┤
│  ...                                │
├─────────────────────────────────────┤
│  Entry N | Header + Data            │
├─────────────────────────────────────┤
│  Padding (align to 4KB)             │ ← 对齐填充
├─────────────────────────────────────┤
│  Footer (1KB)                       │ ← 段文件尾部
└─────────────────────────────────────┘
```

### 2.2 WAL条目格式

```c
// WAL条目头部 (32字节，缓存行对齐)
typedef struct {
    uint32_t magic;          // 0x57414C45 ("WALE")
    uint32_t length;         // 条目总长度(包含头部)
    uint64_t sequence;       // 全局序列号
    uint32_t term;           // Raft任期
    uint32_t checksum;       // CRC32校验和(数据部分)
    uint64_t timestamp;      // 微秒时间戳
    uint8_t  type;           // 条目类型
    uint8_t  flags;          // 标志位
    uint16_t reserved;       // 保留字段
} __attribute__((packed)) wal_entry_header_t;

// 完整WAL条目
typedef struct {
    wal_entry_header_t header;
    char data[];             // 变长数据
} wal_entry_t;
```

### 2.3 段文件头部格式

```c
typedef struct {
    // 基本信息 (64字节)
    uint32_t magic;          // 0x57414C44 ("WALD")  
    uint32_t version;        // 格式版本号
    uint64_t segment_id;     // 段文件唯一ID
    uint64_t start_seq;      // 起始序列号
    uint64_t end_seq;        // 结束序列号(0表示活跃段)
    uint32_t entry_count;    // 条目数量
    uint32_t flags;          // 段标志位
    uint64_t created_time;   // 创建时间
    uint64_t closed_time;    // 关闭时间
    uint32_t header_crc;     // 头部校验和
    uint32_t data_crc;       // 数据校验和
    
    // 元数据 (192字节)
    uint64_t file_size;      // 文件大小
    uint32_t compress_algo;  // 压缩算法
    uint32_t compress_level; // 压缩级别
    uint64_t uncompressed_size; // 原始大小
    uint64_t min_timestamp;  // 最小时间戳
    uint64_t max_timestamp;  // 最大时间戳
    char     reserved[152];  // 保留空间
    
    // 索引信息 (256字节)
    uint32_t index_count;    // 索引条目数
    uint32_t index_offset;   // 索引开始偏移
    uint32_t index_size;     // 索引大小
    char     index_reserved[244]; // 索引保留空间
    
    // 统计信息 (256字节)
    uint64_t total_writes;   // 总写入次数
    uint64_t total_reads;    // 总读取次数  
    uint64_t avg_entry_size; // 平均条目大小
    uint64_t max_entry_size; // 最大条目大小
    char     stats_reserved[224]; // 统计保留空间
    
    // 扩展信息 (3328字节)
    char     extensions[3328]; // 扩展字段
} __attribute__((packed)) wal_segment_header_t;
```

## 3. 写入流程设计

### 3.1 单条目写入流程

```
Client Write Request
        ↓
┌─────────────────┐
│   Validate      │ ← 验证输入参数
│   Parameters    │
└─────────────────┘
        ↓
┌─────────────────┐
│   Allocate      │ ← 分配序列号
│   Sequence      │
└─────────────────┘
        ↓
┌─────────────────┐
│   Build WAL     │ ← 构建WAL条目
│   Entry         │
└─────────────────┘
        ↓
┌─────────────────┐
│   Write to      │ ← 写入缓冲区
│   Buffer        │
└─────────────────┘
        ↓
┌─────────────────┐
│   Trigger       │ ← 触发同步(可选)
│   Sync          │
└─────────────────┘
        ↓
┌─────────────────┐
│   Return        │ ← 返回结果
│   Result        │
└─────────────────┘
```

### 3.2 批量写入流程

```c
// 批量写入接口
typedef struct {
    const void *data;        // 数据指针
    size_t      length;      // 数据长度
    uint8_t     type;        // 条目类型
    uint64_t   *sequence;    // 输出序列号
} wal_batch_item_t;

int wal_write_batch(wal_t *wal, 
                   wal_batch_item_t *items, 
                   size_t count,
                   bool sync_after_write);
```

### 3.3 内存缓冲区设计

```c
typedef struct {
    char    *buffer;         // 缓冲区内存
    size_t   size;           // 缓冲区大小
    size_t   used;           // 已使用字节
    size_t   committed;      // 已提交字节
    uint64_t start_seq;      // 起始序列号
    uint64_t end_seq;        // 结束序列号
    
    pthread_mutex_t mutex;   // 写入互斥锁
    pthread_cond_t  space_available; // 空间可用条件
    pthread_cond_t  data_available;  // 数据可用条件
    
    // 双缓冲区支持
    struct wal_buffer *alternate; // 备用缓冲区
    atomic_bool switching;         // 切换标志
} wal_buffer_t;
```

## 4. 同步策略设计

### 4.1 同步策略选项

```c
typedef enum {
    WAL_SYNC_NONE = 0,       // 不同步(最快)
    WAL_SYNC_PERIODIC = 1,   // 定期同步
    WAL_SYNC_COUNT = 2,      // 按数量同步
    WAL_SYNC_SIZE = 3,       // 按大小同步
    WAL_SYNC_IMMEDIATE = 4   // 立即同步(最安全)
} wal_sync_policy_t;

typedef struct {
    wal_sync_policy_t policy;
    union {
        uint32_t interval_ms;    // 定期同步间隔
        uint32_t count_threshold; // 数量阈值
        size_t   size_threshold;  // 大小阈值
    };
    bool force_on_shutdown;      // 关闭时强制同步
} wal_sync_config_t;
```

### 4.2 同步线程设计

```c
// 同步线程主循环
void* sync_thread_main(void *arg) {
    wal_t *wal = (wal_t*)arg;
    
    while (wal->running) {
        // 等待同步条件
        pthread_mutex_lock(&wal->sync_mutex);
        while (!need_sync(wal) && wal->running) {
            pthread_cond_wait(&wal->sync_cond, &wal->sync_mutex);
        }
        pthread_mutex_unlock(&wal->sync_mutex);
        
        if (!wal->running) break;
        
        // 执行同步
        perform_sync(wal);
        
        // 通知等待的写入线程
        notify_sync_complete(wal);
    }
    
    return NULL;
}
```

## 5. 压缩和合并

### 5.1 在线压缩

```c
typedef struct {
    uint32_t algorithm;      // 压缩算法(LZ4/ZSTD)
    uint32_t level;          // 压缩级别
    size_t   min_size;       // 最小压缩大小
    double   ratio_threshold; // 压缩比阈值
    bool     background;     // 后台压缩
} wal_compression_config_t;

// 压缩接口
int wal_compress_segment(wal_t *wal, 
                        uint64_t segment_id,
                        wal_compression_config_t *config);
```

### 5.2 段文件合并

```c
// 合并策略
typedef struct {
    size_t   max_segments;   // 最大段数量
    size_t   merge_threshold; // 合并阈值
    size_t   target_size;    // 目标段大小
    bool     delete_source;  // 删除源文件
} wal_merge_config_t;

// 合并接口
int wal_merge_segments(wal_t *wal,
                      uint64_t *segment_ids,
                      size_t count,
                      wal_merge_config_t *config);
```

## 6. 故障恢复机制

### 6.1 WAL恢复流程

```
System Startup
        ↓
┌─────────────────┐
│   Scan WAL      │ ← 扫描WAL目录
│   Directory     │
└─────────────────┘
        ↓
┌─────────────────┐
│   Load Segment  │ ← 加载段文件列表
│   List          │
└─────────────────┘
        ↓
┌─────────────────┐
│   Validate      │ ← 验证文件完整性
│   Checksums     │
└─────────────────┘
        ↓
┌─────────────────┐
│   Find Last     │ ← 查找最后有效条目
│   Valid Entry   │
└─────────────────┘
        ↓
┌─────────────────┐
│   Truncate      │ ← 截断损坏部分
│   Corrupted     │
└─────────────────┘
        ↓
┌─────────────────┐
│   Rebuild       │ ← 重建索引
│   Index         │
└─────────────────┘
        ↓
┌─────────────────┐
│   Ready for     │ ← 准备服务
│   Service       │
└─────────────────┘
```

### 6.2 损坏检测和修复

```c
typedef struct {
    uint64_t sequence;       // 损坏条目序列号
    size_t   offset;         // 文件偏移
    uint32_t error_type;     // 错误类型
    char     description[256]; // 错误描述
} wal_corruption_info_t;

// 检测接口
int wal_check_integrity(wal_t *wal, 
                       wal_corruption_info_t **corruptions,
                       size_t *count);

// 修复接口  
int wal_repair_corruption(wal_t *wal,
                         wal_corruption_info_t *corruption,
                         bool auto_truncate);
```

## 7. 性能优化策略

### 7.1 内存映射I/O

```c
typedef struct {
    void   *mmap_addr;       // 映射地址
    size_t  mmap_size;       // 映射大小
    int     mmap_flags;      // 映射标志
    bool    read_ahead;      // 预读开启
    size_t  read_ahead_size; // 预读大小
} wal_mmap_config_t;
```

### 7.2 异步I/O

```c
#ifdef __linux__
#include <libaio.h>

typedef struct {
    io_context_t ctx;        // AIO上下文
    int          max_events; // 最大事件数
    struct iocb *iocbs;      // I/O控制块数组
    struct io_event *events; // 事件数组
} wal_aio_context_t;
#endif
```

### 7.3 CPU缓存优化

```c
// 缓存行对齐的数据结构
#define CACHE_LINE_SIZE 64

typedef struct {
    // 热路径数据(第一个缓存行)
    atomic_uint64_t next_sequence;
    atomic_size_t   buffer_used;
    atomic_bool     sync_pending;
    char            padding1[CACHE_LINE_SIZE - 24];
    
    // 次要数据(第二个缓存行)  
    pthread_mutex_t write_mutex;
    pthread_cond_t  sync_cond;
    char            padding2[CACHE_LINE_SIZE - sizeof(pthread_mutex_t) - sizeof(pthread_cond_t)];
} __attribute__((aligned(CACHE_LINE_SIZE))) wal_hot_data_t;
```

这个WAL引擎设计充分考虑了高性能写入、数据安全性和故障恢复能力，是分布式块设备的核心组件。
