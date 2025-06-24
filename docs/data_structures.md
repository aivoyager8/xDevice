# 数据结构设计

## 1. 核心数据结构

### 1.1 WAL日志条目 (WAL Entry)

```c
typedef struct wal_entry {
    uint64_t sequence_num;    // 全局序列号
    uint32_t term;           // Raft任期号
    uint32_t checksum;       // CRC32校验和
    uint64_t timestamp;      // 时间戳(微秒)
    uint32_t data_length;    // 数据长度
    uint8_t  entry_type;     // 条目类型
    uint8_t  flags;          // 标志位
    uint16_t reserved;       // 保留字段
    char     data[];         // 变长数据
} __attribute__((packed)) wal_entry_t;

// 条目类型定义
typedef enum {
    WAL_ENTRY_DATA = 1,      // 普通数据
    WAL_ENTRY_CHECKPOINT = 2, // 检查点
    WAL_ENTRY_CONFIG = 3,    // 配置变更
    WAL_ENTRY_NOOP = 4       // 空操作
} wal_entry_type_t;
```

### 1.2 WAL段文件头 (WAL Segment Header)

```c
typedef struct wal_segment_header {
    uint32_t magic;          // 魔数: 0x57414C44 ("WALD")
    uint32_t version;        // 格式版本
    uint64_t segment_id;     // 段文件ID
    uint64_t start_sequence; // 起始序列号
    uint64_t end_sequence;   // 结束序列号
    uint32_t entry_count;    // 条目数量
    uint32_t file_size;      // 文件大小
    uint64_t created_time;   // 创建时间
    uint64_t last_modified;  // 最后修改时间
    uint32_t header_checksum; // 头部校验和
    uint32_t data_checksum;   // 数据校验和
    char     reserved[64];    // 保留空间
} __attribute__((packed)) wal_segment_header_t;
```

### 1.3 节点信息 (Node Info)

```c
typedef struct node_info {
    uint32_t node_id;        // 节点ID
    char     hostname[256];  // 主机名
    uint16_t port;           // 端口
    uint8_t  status;         // 节点状态
    uint8_t  role;           // 节点角色
    uint64_t last_heartbeat; // 最后心跳时间
    uint32_t term;           // 当前任期
    uint64_t commit_index;   // 已提交索引
    uint64_t last_applied;   // 最后应用索引
    struct sockaddr_in addr; // 网络地址
} node_info_t;

// 节点状态
typedef enum {
    NODE_STATUS_UNKNOWN = 0,
    NODE_STATUS_ACTIVE = 1,
    NODE_STATUS_INACTIVE = 2,
    NODE_STATUS_FAILED = 3
} node_status_t;

// 节点角色
typedef enum {
    NODE_ROLE_FOLLOWER = 0,
    NODE_ROLE_CANDIDATE = 1,
    NODE_ROLE_LEADER = 2
} node_role_t;
```

### 1.4 Raft消息结构

```c
typedef struct raft_message {
    uint32_t msg_type;       // 消息类型
    uint32_t term;           // 任期号
    uint32_t from_node;      // 发送节点ID
    uint32_t to_node;        // 接收节点ID
    uint64_t timestamp;      // 时间戳
    uint32_t data_length;    // 数据长度
    uint32_t checksum;       // 校验和
    char     data[];         // 消息数据
} __attribute__((packed)) raft_message_t;

// Raft消息类型
typedef enum {
    RAFT_MSG_VOTE_REQUEST = 1,    // 投票请求
    RAFT_MSG_VOTE_RESPONSE = 2,   // 投票响应
    RAFT_MSG_APPEND_ENTRIES = 3,  // 追加条目
    RAFT_MSG_APPEND_RESPONSE = 4, // 追加响应
    RAFT_MSG_HEARTBEAT = 5,       // 心跳
    RAFT_MSG_SNAPSHOT = 6         // 快照
} raft_msg_type_t;
```

### 1.5 缓存项结构 (Cache Entry)

```c
typedef struct cache_entry {
    uint64_t key;            // 缓存键值
    uint32_t size;           // 数据大小
    uint64_t access_time;    // 访问时间
    uint32_t access_count;   // 访问次数
    uint8_t  flags;          // 标志位
    uint8_t  reserved[3];    // 保留字段
    struct cache_entry *next; // 链表指针
    struct cache_entry *prev; // 链表指针
    char     data[];         // 数据内容
} cache_entry_t;
```

## 2. 内存管理结构

### 2.1 内存池 (Memory Pool)

```c
typedef struct memory_pool {
    size_t   block_size;     // 块大小
    size_t   pool_size;      // 池大小
    size_t   used_blocks;    // 已使用块数
    size_t   free_blocks;    // 空闲块数
    void    *memory_base;    // 内存基址
    void    *free_list;      // 空闲列表
    pthread_mutex_t mutex;   // 互斥锁
} memory_pool_t;
```

### 2.2 环形缓冲区 (Ring Buffer)

```c
typedef struct ring_buffer {
    char    *buffer;         // 缓冲区
    size_t   size;           // 缓冲区大小
    size_t   head;           // 头指针
    size_t   tail;           // 尾指针
    size_t   count;          // 元素数量
    pthread_mutex_t mutex;   // 互斥锁
    pthread_cond_t  not_empty; // 非空条件变量
    pthread_cond_t  not_full;  // 非满条件变量
} ring_buffer_t;
```

## 3. 网络通信结构

### 3.1 连接上下文 (Connection Context)

```c
typedef struct connection_ctx {
    int      fd;             // 文件描述符
    uint32_t node_id;        // 对端节点ID
    struct sockaddr_in addr; // 对端地址
    uint64_t last_activity;  // 最后活动时间
    uint8_t  status;         // 连接状态
    uint8_t  type;           // 连接类型
    uint16_t flags;          // 标志位
    
    // 发送缓冲区
    ring_buffer_t send_buffer;
    
    // 接收缓冲区
    ring_buffer_t recv_buffer;
    
    // 统计信息
    uint64_t bytes_sent;
    uint64_t bytes_received;
    uint64_t messages_sent;
    uint64_t messages_received;
} connection_ctx_t;
```

### 3.2 网络事件 (Network Event)

```c
typedef struct network_event {
    int      fd;             // 文件描述符
    uint32_t events;         // 事件类型
    void    *data;           // 关联数据
    uint64_t timestamp;      // 时间戳
} network_event_t;

// 事件类型
#define NET_EVENT_READ    0x01
#define NET_EVENT_WRITE   0x02
#define NET_EVENT_ERROR   0x04
#define NET_EVENT_CLOSE   0x08
```

## 4. 配置和状态结构

### 4.1 系统配置 (System Config)

```c
typedef struct system_config {
    // 节点配置
    uint32_t node_id;
    char     data_dir[256];
    char     log_dir[256];
    
    // WAL配置
    size_t   wal_segment_size;
    size_t   wal_buffer_size;
    uint32_t wal_sync_interval;
    uint32_t wal_compression_level;
    
    // 网络配置
    char     bind_address[64];
    uint16_t bind_port;
    uint32_t max_connections;
    uint32_t connection_timeout;
    
    // Raft配置
    uint32_t election_timeout_min;
    uint32_t election_timeout_max;
    uint32_t heartbeat_interval;
    uint32_t max_log_entries_per_request;
    
    // 缓存配置
    size_t   cache_size;
    uint32_t cache_ttl;
    
    // 性能配置
    uint32_t io_thread_count;
    uint32_t worker_thread_count;
    size_t   read_buffer_size;
    size_t   write_buffer_size;
} system_config_t;
```

### 4.2 系统状态 (System State)

```c
typedef struct system_state {
    // 运行状态
    uint8_t  running;
    uint64_t start_time;
    uint64_t uptime;
    
    // Raft状态
    uint32_t current_term;
    uint32_t voted_for;
    uint32_t commit_index;
    uint32_t last_applied;
    node_role_t role;
    
    // WAL状态
    uint64_t last_sequence;
    uint64_t last_checkpoint;
    uint32_t active_segments;
    
    // 性能统计
    uint64_t total_writes;
    uint64_t total_reads;
    uint64_t total_bytes_written;
    uint64_t total_bytes_read;
    
    // 错误统计
    uint64_t write_errors;
    uint64_t read_errors;
    uint64_t network_errors;
    uint64_t consistency_errors;
} system_state_t;
```

## 5. 索引和查找结构

### 5.1 B+树节点 (B+ Tree Node)

```c
typedef struct btree_node {
    uint8_t  is_leaf;        // 是否叶子节点
    uint16_t key_count;      // 键值数量
    uint16_t max_keys;       // 最大键值数
    uint64_t keys[];         // 键值数组
    
    union {
        struct btree_node **children; // 内部节点的子指针
        void **values;                 // 叶子节点的值指针
    };
    
    struct btree_node *next; // 叶子节点链表指针
} btree_node_t;
```

### 5.2 哈希表项 (Hash Table Entry)

```c
typedef struct hash_entry {
    uint64_t key;            // 键值
    void    *value;          // 值指针
    uint32_t hash;           // 哈希值
    struct hash_entry *next; // 冲突链表
} hash_entry_t;

typedef struct hash_table {
    size_t   bucket_count;   // 桶数量
    size_t   entry_count;    // 条目数量
    double   load_factor;    // 负载因子
    hash_entry_t **buckets;  // 桶数组
    pthread_rwlock_t lock;   // 读写锁
} hash_table_t;
```

这些数据结构设计考虑了：
1. **内存对齐**：使用 `__attribute__((packed))` 确保结构紧密排列
2. **性能优化**：针对缓存友好的内存布局
3. **并发安全**：包含必要的同步原语
4. **扩展性**：预留字段用于未来扩展
5. **错误检测**：包含校验和和状态标志
