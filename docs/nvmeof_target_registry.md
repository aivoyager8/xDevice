# NVMe-oF Target注册与发现系统

## 1. 系统概述

### 1.1 需求分析
在分布式xDevice环境中，需要一个中央化的NVMe-oF Target注册与发现系统来：

- **集中管理**：统一记录所有节点的NVMe-oF Target信息
- **自动发现**：动态发现新的Target，自动注册到系统
- **健康监控**：实时监控Target状态，及时发现故障
- **负载均衡**：基于Target负载和性能进行智能调度
- **配置管理**：统一管理Target配置，支持热更新

### 1.2 架构设计

```
┌─────────────────────────────────────────────────────────────┐
│                NVMe-oF Target Registry                     │
│  ┌─────────────────────────────────────────────────────────┐ │
│  │                 Registry Core                          │ │
│  │  ┌─────────────┐ ┌─────────────┐ ┌─────────────┐      │ │
│  │  │   Target    │ │ Discovery   │ │   Health    │      │ │
│  │  │   Store     │ │   Service   │ │  Monitor    │      │ │
│  │  └─────────────┘ └─────────────┘ └─────────────┘      │ │
│  └─────────────────────────────────────────────────────────┘ │
│  ┌─────────────────────────────────────────────────────────┐ │
│  │                Management APIs                         │ │
│  │  ┌─────────────┐ ┌─────────────┐ ┌─────────────┐      │ │
│  │  │ Registration│ │   Query     │ │   Update    │      │ │
│  │  │     API     │ │    API      │ │    API      │      │ │
│  │  └─────────────┘ └─────────────┘ └─────────────┘      │ │
│  └─────────────────────────────────────────────────────────┘ │
└─────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────┐
│              NVMe-oF Target Nodes                          │
│  ┌─────────────┐ ┌─────────────┐ ┌─────────────┐          │
│  │   Node-1    │ │   Node-2    │ │   Node-N    │          │
│  │   Target    │ │   Target    │ │   Target    │          │
│  │   Agent     │ │   Agent     │ │   Agent     │          │
│  └─────────────┘ └─────────────┘ └─────────────┘          │
└─────────────────────────────────────────────────────────────┘
```

## 2. 核心数据结构

### 2.1 Target信息模型

```c
// NVMe-oF Target基本信息
typedef struct nvmeof_target_info {
    // 基本标识
    char target_id[64];            // Target唯一ID
    char node_id[64];              // 所属节点ID
    char nqn[256];                 // NVMe Qualified Name
    
    // 网络信息
    struct {
        char transport[16];        // 传输协议 (rdma/tcp)
        char address[64];          // IP地址
        uint16_t port;             // 端口号
        char interface[32];        // 网络接口
    } network;
    
    // 存储信息
    struct {
        char device_path[256];     // 块设备路径
        uint64_t capacity_bytes;   // 容量
        uint32_t block_size;       // 块大小
        uint16_t namespace_id;     // 命名空间ID
        bool readonly;             // 只读标志
    } storage;
    
    // 性能指标
    struct {
        uint64_t max_iops;         // 最大IOPS
        uint64_t max_bandwidth_mbps; // 最大带宽
        uint32_t queue_depth;      // 队列深度
        uint32_t max_connections;  // 最大连接数
        uint32_t current_connections; // 当前连接数
    } performance;
    
    // 状态信息
    struct {
        nvmeof_target_state_t state; // Target状态
        uint64_t last_seen;        // 最后发现时间
        uint64_t uptime_seconds;   // 运行时间
        double cpu_usage;          // CPU使用率
        double memory_usage;       // 内存使用率
        uint64_t io_count;         // I/O计数
        uint64_t error_count;      // 错误计数
    } status;
    
    // 配置信息
    struct {
        bool auto_discovery;       // 自动发现
        uint32_t discovery_interval; // 发现间隔
        char tags[512];            // 标签信息
        double weight;             // 权重
    } config;
    
    struct nvmeof_target_info *next;
} nvmeof_target_info_t;

// Target状态枚举
typedef enum {
    NVMEOF_TARGET_UNKNOWN = 0,     // 未知状态
    NVMEOF_TARGET_ACTIVE = 1,      // 活跃状态
    NVMEOF_TARGET_DEGRADED = 2,    // 性能下降
    NVMEOF_TARGET_MAINTENANCE = 3, // 维护状态
    NVMEOF_TARGET_FAILED = 4,      // 故障状态
    NVMEOF_TARGET_OFFLINE = 5      // 离线状态
} nvmeof_target_state_t;
```

### 2.2 注册中心核心结构

```c
// NVMe-oF Target注册中心
typedef struct nvmeof_target_registry {
    // Target存储
    struct {
        nvmeof_target_info_t **targets;  // Target数组
        uint32_t capacity;               // 容量
        uint32_t count;                  // 当前数量
        pthread_rwlock_t lock;           // 读写锁
        
        // 索引结构
        struct target_index {
            char key[64];                // 索引键
            nvmeof_target_info_t *target; // Target指针
            struct target_index *next;
        } *by_id, *by_nqn, *by_node;
    } store;
    
    // 发现服务
    struct {
        pthread_t discovery_thread;     // 发现线程
        bool discovery_running;         // 运行状态
        uint32_t discovery_interval_ms; // 发现间隔
        
        // 发现方式
        struct {
            bool enable_mdns;           // mDNS发现
            bool enable_dhcp;           // DHCP发现
            bool enable_static;         // 静态配置
            bool enable_multicast;      // 组播发现
        } methods;
        
        // 发现统计
        uint64_t discovery_attempts;   // 发现尝试次数
        uint64_t successful_discoveries; // 成功发现次数
        uint64_t failed_discoveries;   // 失败发现次数
    } discovery;
    
    // 健康监控
    struct {
        pthread_t monitor_thread;      // 监控线程
        bool monitor_running;          // 运行状态
        uint32_t monitor_interval_ms;  // 监控间隔
        
        // 监控统计
        uint64_t health_checks;        // 健康检查次数
        uint64_t failed_checks;        // 失败检查次数
        uint32_t offline_targets;      // 离线Target数
    } monitor;
    
    // 负载均衡
    struct {
        load_balance_policy_t policy;  // 负载均衡策略
        double load_threshold;         // 负载阈值
        uint32_t rebalance_interval_ms; // 重新平衡间隔
    } load_balancer;
    
    // 事件通知
    struct {
        event_callback_t callbacks[MAX_EVENT_CALLBACKS];
        uint32_t callback_count;
        pthread_mutex_t callback_lock;
    } events;
    
    // 配置
    struct {
        char config_file[256];         // 配置文件路径
        bool auto_save;                // 自动保存
        uint32_t save_interval_ms;     // 保存间隔
    } config;
    
    pthread_mutex_t registry_lock;     // 注册中心锁
} nvmeof_target_registry_t;

// 负载均衡策略
typedef enum {
    LOAD_BALANCE_ROUND_ROBIN = 1,   // 轮询
    LOAD_BALANCE_LEAST_CONNECTIONS, // 最少连接
    LOAD_BALANCE_LEAST_LOAD,        // 最少负载
    LOAD_BALANCE_WEIGHTED,          // 加权
    LOAD_BALANCE_HASH              // 哈希
} load_balance_policy_t;

// 事件类型
typedef enum {
    TARGET_EVENT_REGISTERED = 1,    // Target注册
    TARGET_EVENT_UNREGISTERED,      // Target注销
    TARGET_EVENT_STATE_CHANGED,     // 状态变更
    TARGET_EVENT_PERFORMANCE_ALERT, // 性能告警
    TARGET_EVENT_CONNECTION_CHANGED // 连接变更
} target_event_type_t;

// 事件回调函数
typedef void (*event_callback_t)(target_event_type_t event_type,
                                nvmeof_target_info_t *target,
                                void *user_data);
```

## 3. Target发现机制

### 3.1 多种发现方式

```c
// Target发现方法接口
typedef struct target_discovery_method {
    char method_name[32];
    int (*discover)(nvmeof_target_registry_t *registry,
                   nvmeof_target_info_t **discovered_targets,
                   uint32_t *count);
    int (*init)(struct target_discovery_method *method, const char *config);
    void (*cleanup)(struct target_discovery_method *method);
    void *private_data;
} target_discovery_method_t;

// mDNS发现实现
static int mdns_discover_targets(nvmeof_target_registry_t *registry,
                               nvmeof_target_info_t **targets,
                               uint32_t *count) {
    // 使用Avahi或类似库进行mDNS发现
    // 查找_nvme-disc._tcp服务
    
    mdns_query_t query = {
        .service_type = "_nvme-disc._tcp",
        .domain = "local",
        .timeout_ms = 5000
    };
    
    mdns_result_t *results = NULL;
    int result_count = 0;
    
    int ret = mdns_query(&query, &results, &result_count);
    if (ret != 0) {
        return ret;
    }
    
    *targets = malloc(result_count * sizeof(nvmeof_target_info_t));
    *count = 0;
    
    for (int i = 0; i < result_count; i++) {
        nvmeof_target_info_t *target = &(*targets)[*count];
        
        // 解析mDNS记录
        parse_mdns_record(&results[i], target);
        
        // 验证Target有效性
        if (validate_target_info(target)) {
            (*count)++;
        }
    }
    
    mdns_free_results(results, result_count);
    return 0;
}

// 静态配置发现
static int static_discover_targets(nvmeof_target_registry_t *registry,
                                 nvmeof_target_info_t **targets,
                                 uint32_t *count) {
    // 从配置文件读取静态Target列表
    config_parser_t *parser = config_parser_create();
    
    if (config_parser_load_file(parser, registry->config.config_file) != 0) {
        config_parser_destroy(parser);
        return -1;
    }
    
    config_section_t *targets_section = 
        config_parser_get_section(parser, "static_targets");
    
    if (!targets_section) {
        config_parser_destroy(parser);
        return 0; // 无静态配置
    }
    
    uint32_t static_count = config_section_get_entry_count(targets_section);
    *targets = malloc(static_count * sizeof(nvmeof_target_info_t));
    *count = 0;
    
    for (uint32_t i = 0; i < static_count; i++) {
        config_entry_t *entry = config_section_get_entry(targets_section, i);
        nvmeof_target_info_t *target = &(*targets)[*count];
        
        // 解析静态配置
        if (parse_static_target_config(entry, target) == 0 &&
            validate_target_info(target)) {
            (*count)++;
        }
    }
    
    config_parser_destroy(parser);
    return 0;
}

// 组播发现实现
static int multicast_discover_targets(nvmeof_target_registry_t *registry,
                                    nvmeof_target_info_t **targets,
                                    uint32_t *count) {
    // 发送NVMe Discovery组播包
    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        return -errno;
    }
    
    // 设置组播选项
    int ttl = 1;
    setsockopt(sockfd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
    
    struct sockaddr_in mcast_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(NVME_DISCOVERY_PORT),
        .sin_addr.s_addr = inet_addr(NVME_DISCOVERY_MCAST_ADDR)
    };
    
    // 构造发现请求
    nvme_discovery_request_t request = {
        .opcode = NVME_DISCOVERY_REQUEST,
        .flags = 0,
        .request_id = generate_request_id(),
        .timestamp = get_current_timestamp()
    };
    
    // 发送请求
    if (sendto(sockfd, &request, sizeof(request), 0,
              (struct sockaddr*)&mcast_addr, sizeof(mcast_addr)) < 0) {
        close(sockfd);
        return -errno;
    }
    
    // 接收响应
    *targets = malloc(MAX_DISCOVERY_TARGETS * sizeof(nvmeof_target_info_t));
    *count = 0;
    
    fd_set readfds;
    struct timeval timeout = { .tv_sec = 5, .tv_usec = 0 };
    
    FD_ZERO(&readfds);
    FD_SET(sockfd, &readfds);
    
    while (select(sockfd + 1, &readfds, NULL, NULL, &timeout) > 0 &&
           *count < MAX_DISCOVERY_TARGETS) {
        
        nvme_discovery_response_t response;
        struct sockaddr_in from_addr;
        socklen_t from_len = sizeof(from_addr);
        
        ssize_t recv_len = recvfrom(sockfd, &response, sizeof(response), 0,
                                   (struct sockaddr*)&from_addr, &from_len);
        
        if (recv_len > 0 && response.request_id == request.request_id) {
            nvmeof_target_info_t *target = &(*targets)[*count];
            
            // 解析发现响应
            if (parse_discovery_response(&response, &from_addr, target) == 0 &&
                validate_target_info(target)) {
                (*count)++;
            }
        }
        
        FD_ZERO(&readfds);
        FD_SET(sockfd, &readfds);
        timeout.tv_sec = 1; // 后续超时时间缩短
    }
    
    close(sockfd);
    return 0;
}
```

(继续第二部分...)
````markdown
## 4. 注册中心API设计

### 4.1 RESTful API接口

#### 4.1.1 Target注册API

```bash
# 注册新的NVMe-oF Target
POST /api/v1/targets
Content-Type: application/json

{
  "node_id": "xdevice-node-001",
  "target_info": {
    "nqn": "nqn.2024-01.com.xdevice:nvme:001",
    "transport": "tcp",
    "address": "192.168.1.100",
    "port": 4420,
    "namespace_capacity": 1073741824000,
    "security_type": "none"
  },
  "performance_caps": {
    "max_iops": 500000,
    "max_bandwidth_mbps": 6000,
    "avg_latency_us": 50
  },
  "metadata": {
    "location": "rack-1-slot-3",
    "hw_type": "nvme-ssd",
    "firmware_version": "1.2.3"
  }
}

# 响应
HTTP/1.1 201 Created
{
  "target_id": "tgt-550e8400-e29b-41d4-a716-446655440000",
  "status": "registered",
  "registered_at": "2024-01-15T10:30:00Z"
}
```

#### 4.1.2 Target查询API

```bash
# 查询所有可用Target
GET /api/v1/targets?status=available&location=rack-1

# 响应
HTTP/1.1 200 OK
{
  "targets": [
    {
      "target_id": "tgt-550e8400-e29b-41d4-a716-446655440000",
      "node_id": "xdevice-node-001",
      "nqn": "nqn.2024-01.com.xdevice:nvme:001",
      "transport": "tcp",
      "address": "192.168.1.100",
      "port": 4420,
      "status": "available",
      "health_score": 95,
      "load_percentage": 25,
      "last_seen": "2024-01-15T10:35:00Z"
    }
  ],
  "total_count": 1,
  "healthy_count": 1
}

# 查询特定Target详情
GET /api/v1/targets/{target_id}

# 基于性能要求筛选Target
GET /api/v1/targets/select?min_iops=100000&max_latency_us=100&min_bandwidth_mbps=2000
```

#### 4.1.3 Target更新API

```bash
# 更新Target状态
PUT /api/v1/targets/{target_id}/status
{
  "status": "maintenance",
  "reason": "scheduled maintenance",
  "estimated_duration_minutes": 30
}

# 更新Target性能指标
PUT /api/v1/targets/{target_id}/metrics
{
  "current_iops": 45000,
  "current_bandwidth_mbps": 2800,
  "avg_latency_us": 45,
  "load_percentage": 35,
  "error_rate": 0.001
}
```

#### 4.1.4 Target删除API

```bash
# 取消注册Target
DELETE /api/v1/targets/{target_id}
{
  "force": false,
  "drain_timeout_seconds": 300
}

# 响应
HTTP/1.1 200 OK
{
  "status": "unregistered",
  "drained_connections": 5,
  "unregistered_at": "2024-01-15T10:45:00Z"
}
```

### 4.2 gRPC API设计

```protobuf
syntax = "proto3";

package xdevice.nvmeof.registry;

service NVMeOFTargetRegistry {
  // Target管理
  rpc RegisterTarget(RegisterTargetRequest) returns (RegisterTargetResponse);
  rpc UpdateTarget(UpdateTargetRequest) returns (UpdateTargetResponse);
  rpc UnregisterTarget(UnregisterTargetRequest) returns (UnregisterTargetResponse);
  
  // Target查询
  rpc GetTarget(GetTargetRequest) returns (GetTargetResponse);
  rpc ListTargets(ListTargetsRequest) returns (ListTargetsResponse);
  rpc SelectOptimalTargets(SelectTargetsRequest) returns (SelectTargetsResponse);
  
  // 健康监控
  rpc ReportHealth(HealthReportRequest) returns (HealthReportResponse);
  rpc GetHealthStatus(HealthStatusRequest) returns (HealthStatusResponse);
  
  // 事件订阅
  rpc SubscribeEvents(EventSubscriptionRequest) returns (stream TargetEvent);
}

message TargetInfo {
  string target_id = 1;
  string node_id = 2;
  string nqn = 3;
  TransportType transport = 4;
  string address = 5;
  uint32 port = 6;
  uint64 namespace_capacity = 7;
  SecurityType security_type = 8;
  PerformanceCaps performance_caps = 9;
  map<string, string> metadata = 10;
}

message PerformanceCaps {
  uint64 max_iops = 1;
  uint64 max_bandwidth_mbps = 2;
  uint32 avg_latency_us = 3;
  uint32 max_queue_depth = 4;
}

message HealthMetrics {
  uint32 health_score = 1;          // 0-100
  uint32 load_percentage = 2;       // 0-100
  uint64 current_iops = 3;
  uint64 current_bandwidth_mbps = 4;
  uint32 current_latency_us = 5;
  double error_rate = 6;
  uint32 active_connections = 7;
  int64 last_updated = 8;           // Unix timestamp
}

enum TargetStatus {
  UNKNOWN = 0;
  AVAILABLE = 1;
  BUSY = 2;
  MAINTENANCE = 3;
  FAILED = 4;
  OFFLINE = 5;
}

enum TransportType {
  TCP = 0;
  RDMA = 1;
  FC = 2;
}

enum SecurityType {
  NONE = 0;
  TLS = 1;
  IPSEC = 2;
}
```

## 5. 健康监控系统

### 5.1 多维度健康评估

```c
// 健康评估算法
typedef struct {
    uint32_t health_score;      // 综合健康分数 (0-100)
    uint32_t availability;      // 可用性分数 (0-100)
    uint32_t performance;       // 性能分数 (0-100)
    uint32_t reliability;       // 可靠性分数 (0-100)
    uint32_t resource_usage;    // 资源使用分数 (0-100)
} target_health_assessment_t;

// 健康评估配置
typedef struct {
    // 权重配置
    double availability_weight;    // 可用性权重
    double performance_weight;     // 性能权重
    double reliability_weight;     // 可靠性权重
    double resource_weight;        // 资源权重
    
    // 阈值配置
    uint32_t max_acceptable_latency_us;
    double max_acceptable_error_rate;
    uint32_t max_acceptable_load_pct;
    uint32_t min_acceptable_iops;
    
    // 监控间隔
    uint32_t monitoring_interval_sec;
    uint32_t health_check_timeout_sec;
} health_monitor_config_t;

// 计算综合健康分数
uint32_t calculate_health_score(const nvmeof_target_info_t *target,
                               const target_health_metrics_t *metrics,
                               const health_monitor_config_t *config) {
    target_health_assessment_t assessment = {0};
    
    // 1. 可用性评估
    if (target->status == TARGET_STATUS_AVAILABLE) {
        assessment.availability = 100;
    } else if (target->status == TARGET_STATUS_BUSY) {
        assessment.availability = 80;
    } else if (target->status == TARGET_STATUS_MAINTENANCE) {
        assessment.availability = 20;
    } else {
        assessment.availability = 0;
    }
    
    // 2. 性能评估
    assessment.performance = calculate_performance_score(metrics, config);
    
    // 3. 可靠性评估
    assessment.reliability = calculate_reliability_score(metrics, config);
    
    // 4. 资源使用评估
    assessment.resource_usage = 100 - metrics->load_percentage;
    
    // 5. 加权计算综合分数
    double weighted_score = 
        assessment.availability * config->availability_weight +
        assessment.performance * config->performance_weight +
        assessment.reliability * config->reliability_weight +
        assessment.resource_usage * config->resource_weight;
    
    return (uint32_t)fmin(100.0, fmax(0.0, weighted_score));
}

// 性能评估子函数
uint32_t calculate_performance_score(const target_health_metrics_t *metrics,
                                   const health_monitor_config_t *config) {
    uint32_t latency_score = 100;
    uint32_t iops_score = 100;
    uint32_t bandwidth_score = 100;
    
    // 延迟评分
    if (metrics->current_latency_us > config->max_acceptable_latency_us) {
        double latency_penalty = (double)(metrics->current_latency_us - 
                                        config->max_acceptable_latency_us) /
                               config->max_acceptable_latency_us;
        latency_score = (uint32_t)fmax(0, 100 - latency_penalty * 50);
    }
    
    // IOPS评分
    if (metrics->current_iops < config->min_acceptable_iops) {
        double iops_ratio = (double)metrics->current_iops / 
                           config->min_acceptable_iops;
        iops_score = (uint32_t)(iops_ratio * 100);
    }
    
    // 带宽评分基于目标最大能力的利用率
    if (metrics->target_info && metrics->target_info->performance_caps.max_bandwidth_mbps > 0) {
        double bandwidth_utilization = (double)metrics->current_bandwidth_mbps /
                                     metrics->target_info->performance_caps.max_bandwidth_mbps;
        
        if (bandwidth_utilization > 0.9) {
            bandwidth_score = 70; // 过载扣分
        } else if (bandwidth_utilization > 0.7) {
            bandwidth_score = 85; // 高负载扣分
        }
    }
    
    return (latency_score + iops_score + bandwidth_score) / 3;
}
```

### 5.2 故障检测与自动修复

```c
// 故障检测器
typedef struct {
    char target_id[MAX_TARGET_ID_LEN];
    failure_type_t failure_type;
    severity_level_t severity;
    char description[256];
    time_t detected_at;
    time_t last_seen;
    uint32_t occurrence_count;
    bool auto_recovery_attempted;
} target_failure_t;

typedef enum {
    FAILURE_NETWORK_TIMEOUT,
    FAILURE_HIGH_LATENCY,
    FAILURE_HIGH_ERROR_RATE,
    FAILURE_RESOURCE_EXHAUSTION,
    FAILURE_AUTHENTICATION,
    FAILURE_PROTOCOL_ERROR,
    FAILURE_HARDWARE_ERROR,
    FAILURE_UNKNOWN
} failure_type_t;

typedef enum {
    SEVERITY_LOW,      // 性能轻微下降
    SEVERITY_MEDIUM,   // 影响部分功能
    SEVERITY_HIGH,     // 严重影响使用
    SEVERITY_CRITICAL  // 完全不可用
} severity_level_t;

// 故障检测引擎
int detect_target_failures(nvmeof_registry_t *registry) {
    for (int i = 0; i < registry->target_count; i++) {
        nvmeof_target_info_t *target = &registry->targets[i];
        target_health_metrics_t *metrics = &target->health_metrics;
        
        // 检测网络超时
        time_t now = time(NULL);
        if (now - metrics->last_updated > HEALTH_CHECK_TIMEOUT) {
            register_failure(registry, target->target_id, 
                           FAILURE_NETWORK_TIMEOUT, SEVERITY_HIGH,
                           "Target health check timeout");
        }
        
        // 检测高延迟
        if (metrics->current_latency_us > TARGET_MAX_LATENCY_THRESHOLD) {
            register_failure(registry, target->target_id,
                           FAILURE_HIGH_LATENCY, SEVERITY_MEDIUM,
                           "Target latency exceeds threshold");
        }
        
        // 检测高错误率
        if (metrics->error_rate > TARGET_MAX_ERROR_RATE_THRESHOLD) {
            register_failure(registry, target->target_id,
                           FAILURE_HIGH_ERROR_RATE, SEVERITY_HIGH,
                           "Target error rate too high");
        }
        
        // 检测资源耗尽
        if (metrics->load_percentage > TARGET_MAX_LOAD_THRESHOLD) {
            register_failure(registry, target->target_id,
                           FAILURE_RESOURCE_EXHAUSTION, SEVERITY_MEDIUM,
                           "Target resource usage too high");
        }
    }
    
    return 0;
}

// 自动修复机制
int attempt_auto_recovery(nvmeof_registry_t *registry, 
                         const target_failure_t *failure) {
    nvmeof_target_info_t *target = find_target_by_id(registry, failure->target_id);
    if (!target) return -1;
    
    switch (failure->failure_type) {
        case FAILURE_NETWORK_TIMEOUT:
            return recover_network_timeout(target);
            
        case FAILURE_HIGH_LATENCY:
            return recover_high_latency(target);
            
        case FAILURE_RESOURCE_EXHAUSTION:
            return recover_resource_exhaustion(target);
            
        case FAILURE_HIGH_ERROR_RATE:
            return recover_high_error_rate(target);
            
        default:
            // 对于未知故障，尝试重连
            return attempt_target_reconnection(target);
    }
}

// 网络超时恢复
int recover_network_timeout(nvmeof_target_info_t *target) {
    // 1. 尝试ping测试
    if (ping_target(target->address) != 0) {
        log_error("Target %s network unreachable", target->target_id);
        return -1;
    }
    
    // 2. 尝试重新建立连接
    if (test_nvmeof_connection(target) == 0) {
        log_info("Target %s network connection recovered", target->target_id);
        target->status = TARGET_STATUS_AVAILABLE;
        return 0;
    }
    
    // 3. 标记为离线
    target->status = TARGET_STATUS_OFFLINE;
    return -1;
}

// 高延迟恢复
int recover_high_latency(nvmeof_target_info_t *target) {
    // 1. 减少队列深度
    if (target->connection.queue_depth > MIN_QUEUE_DEPTH) {
        target->connection.queue_depth /= 2;
        log_info("Reduced queue depth for target %s to %d", 
                target->target_id, target->connection.queue_depth);
    }
    
    // 2. 调整I/O大小
    if (target->connection.max_io_size > MIN_IO_SIZE) {
        target->connection.max_io_size /= 2;
        log_info("Reduced max I/O size for target %s to %d", 
                target->target_id, target->connection.max_io_size);
    }
    
    // 3. 启用压缩（如果支持）
    if (target->capabilities.compression_supported && 
        !target->connection.compression_enabled) {
        target->connection.compression_enabled = true;
        log_info("Enabled compression for target %s", target->target_id);
    }
    
    return 0;
}
```

## 6. 与动态后端管理集成

### 6.1 集成架构

```c
// 集成接口定义
typedef struct {
    // 向动态后端管理系统注册NVMe-oF后端
    int (*register_nvmeof_backend)(const char *backend_id,
                                  const nvmeof_target_info_t *target_info);
    
    // 通知后端状态变化
    int (*notify_backend_status_change)(const char *backend_id,
                                       backend_status_t new_status,
                                       const char *reason);
    
    // 获取后端选择建议
    backend_selection_result_t* (*get_backend_selection)(
        const backend_selection_criteria_t *criteria);
    
    // 报告后端性能指标
    int (*report_backend_metrics)(const char *backend_id,
                                 const backend_performance_metrics_t *metrics);
} dynamic_backend_integration_t;

// 注册中心与动态后端管理的桥接器
typedef struct {
    nvmeof_registry_t *registry;
    dynamic_backend_manager_t *backend_manager;
    dynamic_backend_integration_t integration;
    
    // 事件监听器
    event_listener_t status_listener;
    event_listener_t health_listener;
    
    // 同步配置
    bool auto_sync_enabled;
    uint32_t sync_interval_sec;
    uint32_t max_retry_count;
} registry_backend_bridge_t;

// 初始化集成桥接器
int init_registry_backend_bridge(registry_backend_bridge_t *bridge,
                                nvmeof_registry_t *registry,
                                dynamic_backend_manager_t *backend_manager) {
    bridge->registry = registry;
    bridge->backend_manager = backend_manager;
    bridge->auto_sync_enabled = true;
    bridge->sync_interval_sec = 30;
    bridge->max_retry_count = 3;
    
    // 设置集成接口
    bridge->integration.register_nvmeof_backend = register_nvmeof_backend_impl;
    bridge->integration.notify_backend_status_change = notify_backend_status_impl;
    bridge->integration.get_backend_selection = get_backend_selection_impl;
    bridge->integration.report_backend_metrics = report_backend_metrics_impl;
    
    // 注册事件监听器
    register_target_status_listener(&bridge->status_listener, 
                                   on_target_status_changed, bridge);
    register_target_health_listener(&bridge->health_listener,
                                   on_target_health_changed, bridge);
    
    return 0;
}

// Target状态变化事件处理
void on_target_status_changed(const char *target_id, 
                             target_status_t old_status,
                             target_status_t new_status,
                             void *user_data) {
    registry_backend_bridge_t *bridge = (registry_backend_bridge_t*)user_data;
    
    // 将Target状态映射为后端状态
    backend_status_t backend_status = map_target_to_backend_status(new_status);
    
    // 通知动态后端管理系统
    char backend_id[256];
    snprintf(backend_id, sizeof(backend_id), "nvmeof-%s", target_id);
    
    int result = bridge->integration.notify_backend_status_change(
        backend_id, backend_status, "Target status changed");
    
    if (result != 0) {
        log_warning("Failed to notify backend status change for %s", backend_id);
    }
}

// Target健康状态变化事件处理
void on_target_health_changed(const char *target_id,
                             uint32_t old_health_score,
                             uint32_t new_health_score,
                             void *user_data) {
    registry_backend_bridge_t *bridge = (registry_backend_bridge_t*)user_data;
    
    // 获取Target信息
    nvmeof_target_info_t *target = find_target_by_id(bridge->registry, target_id);
    if (!target) return;
    
    // 构建性能指标
    backend_performance_metrics_t metrics;
    metrics.health_score = new_health_score;
    metrics.iops = target->health_metrics.current_iops;
    metrics.bandwidth_mbps = target->health_metrics.current_bandwidth_mbps;
    metrics.latency_us = target->health_metrics.current_latency_us;
    metrics.error_rate = target->health_metrics.error_rate;
    metrics.load_percentage = target->health_metrics.load_percentage;
    
    // 报告给动态后端管理系统
    char backend_id[256];
    snprintf(backend_id, sizeof(backend_id), "nvmeof-%s", target_id);
    
    bridge->integration.report_backend_metrics(backend_id, &metrics);
}
```

### 6.2 智能后端选择

```c
// 基于注册中心信息的智能后端选择
backend_selection_result_t* select_optimal_nvmeof_targets(
    const backend_selection_criteria_t *criteria,
    nvmeof_registry_t *registry) {
    
    backend_selection_result_t *result = calloc(1, sizeof(backend_selection_result_t));
    if (!result) return NULL;
    
    // 1. 筛选符合基本条件的Target
    nvmeof_target_info_t **candidates = NULL;
    int candidate_count = 0;
    
    filter_targets_by_criteria(registry, criteria, &candidates, &candidate_count);
    
    if (candidate_count == 0) {
        result->status = SELECTION_NO_SUITABLE_BACKEND;
        return result;
    }
    
    // 2. 按多维度评分排序
    target_score_t *scores = calloc(candidate_count, sizeof(target_score_t));
    
    for (int i = 0; i < candidate_count; i++) {
        scores[i].target = candidates[i];
        scores[i].total_score = calculate_selection_score(candidates[i], criteria);
    }
    
    // 按分数降序排序
    qsort(scores, candidate_count, sizeof(target_score_t), compare_target_scores);
    
    // 3. 选择最优Target
    result->selected_backends = malloc(sizeof(backend_info_t) * criteria->target_count);
    result->backend_count = 0;
    
    for (int i = 0; i < candidate_count && result->backend_count < criteria->target_count; i++) {
        backend_info_t *backend = &result->selected_backends[result->backend_count];
        
        snprintf(backend->backend_id, sizeof(backend->backend_id), 
                "nvmeof-%s", scores[i].target->target_id);
        backend->backend_type = BACKEND_TYPE_NVMEOF;
        backend->priority = result->backend_count + 1;
        backend->health_score = scores[i].target->health_metrics.health_score;
        backend->performance_score = scores[i].performance_score;
        backend->reliability_score = scores[i].reliability_score;
        
        result->backend_count++;
    }
    
    result->status = SELECTION_SUCCESS;
    
    free(scores);
    free(candidates);
    return result;
}

// 多维度评分算法
double calculate_selection_score(const nvmeof_target_info_t *target,
                               const backend_selection_criteria_t *criteria) {
    double score = 0.0;
    double total_weight = 0.0;
    
    // 1. 健康分数 (权重: 0.3)
    if (criteria->health_score_weight > 0) {
        score += target->health_metrics.health_score * criteria->health_score_weight;
        total_weight += criteria->health_score_weight;
    }
    
    // 2. 性能分数 (权重: 0.4)
    if (criteria->performance_weight > 0) {
        double perf_score = calculate_performance_score_for_selection(target, criteria);
        score += perf_score * criteria->performance_weight;
        total_weight += criteria->performance_weight;
    }
    
    // 3. 可靠性分数 (权重: 0.2)
    if (criteria->reliability_weight > 0) {
        double rel_score = calculate_reliability_score_for_selection(target);
        score += rel_score * criteria->reliability_weight;
        total_weight += criteria->reliability_weight;
    }
    
    // 4. 位置亲和性 (权重: 0.1)
    if (criteria->location_affinity_weight > 0 && criteria->preferred_location) {
        double loc_score = calculate_location_affinity_score(target, criteria->preferred_location);
        score += loc_score * criteria->location_affinity_weight;
        total_weight += criteria->location_affinity_weight;
    }
    
    return total_weight > 0 ? score / total_weight : 0.0;
}

// 性能评分（针对选择场景）
double calculate_performance_score_for_selection(const nvmeof_target_info_t *target,
                                                const backend_selection_criteria_t *criteria) {
    double score = 100.0;
    
    // IOPS评估
    if (criteria->min_iops > 0) {
        if (target->performance_caps.max_iops < criteria->min_iops) {
            return 0.0; // 不满足最低要求
        }
        
        double iops_ratio = (double)target->performance_caps.max_iops / criteria->min_iops;
        score *= fmin(1.0, iops_ratio / 2.0); // 超出需求越多分数越高
    }
    
    // 带宽评估
    if (criteria->min_bandwidth_mbps > 0) {
        if (target->performance_caps.max_bandwidth_mbps < criteria->min_bandwidth_mbps) {
            return 0.0;
        }
        
        double bw_ratio = (double)target->performance_caps.max_bandwidth_mbps / 
                         criteria->min_bandwidth_mbps;
        score *= fmin(1.0, bw_ratio / 2.0);
    }
    
    // 延迟评估
    if (criteria->max_latency_us > 0) {
        if (target->health_metrics.current_latency_us > criteria->max_latency_us) {
            return 0.0;
        }
        
        double latency_score = 1.0 - (double)target->health_metrics.current_latency_us / 
                              criteria->max_latency_us;
        score *= latency_score;
    }
    
    // 当前负载评估（负载越低分数越高）
    double load_score = 1.0 - (double)target->health_metrics.load_percentage / 100.0;
    score *= load_score;
    
    return score;
}
```

## 7. 运维管理功能

### 7.1 配置管理

```c
// 注册中心配置结构
typedef struct {
    // 服务配置
    char listen_address[64];
    uint16_t listen_port;
    char data_directory[256];
    
    // 发现配置
    bool auto_discovery_enabled;
    uint32_t discovery_interval_sec;
    uint32_t discovery_timeout_sec;
    char discovery_multicast_group[64];
    uint16_t discovery_multicast_port;
    
    // 健康监控配置
    health_monitor_config_t health_config;
    
    // 故障转移配置
    bool auto_failover_enabled;
    uint32_t failover_threshold_failures;
    uint32_t failover_timeout_sec;
    
    // 集成配置
    bool backend_integration_enabled;
    char backend_manager_endpoint[256];
    
    // 安全配置
    bool tls_enabled;
    char cert_file[256];
    char key_file[256];
    char ca_file[256];
    
    // 日志配置
    log_level_t log_level;
    char log_file[256];
    bool log_rotation_enabled;
    uint32_t max_log_file_size_mb;
    
    // 持久化配置
    bool persistence_enabled;
    char persistence_backend[32]; // "sqlite", "etcd", "consul"
    char persistence_connection_string[512];
    uint32_t persistence_sync_interval_sec;
} registry_config_t;

// 从配置文件加载配置
int load_registry_config(const char *config_file, registry_config_t *config) {
    FILE *file = fopen(config_file, "r");
    if (!file) {
        log_error("Failed to open config file: %s", config_file);
        return -1;
    }
    
    // 使用JSON解析库(如cJSON)解析配置
    char buffer[8192];
    size_t len = fread(buffer, 1, sizeof(buffer) - 1, file);
    buffer[len] = '\0';
    fclose(file);
    
    cJSON *json = cJSON_Parse(buffer);
    if (!json) {
        log_error("Invalid JSON in config file");
        return -1;
    }
    
    // 解析各个配置项
    parse_service_config(json, config);
    parse_discovery_config(json, config);
    parse_health_config(json, config);
    parse_failover_config(json, config);
    parse_integration_config(json, config);
    parse_security_config(json, config);
    parse_logging_config(json, config);
    parse_persistence_config(json, config);
    
    cJSON_Delete(json);
    return 0;
}

// 示例配置文件 (registry.json)
/*
{
  "service": {
    "listen_address": "0.0.0.0",
    "listen_port": 8080,
    "data_directory": "/var/lib/xdevice/registry"
  },
  "discovery": {
    "auto_discovery_enabled": true,
    "discovery_interval_sec": 60,
    "discovery_timeout_sec": 5,
    "multicast_group": "239.255.43.21",
    "multicast_port": 4791
  },
  "health_monitoring": {
    "monitoring_interval_sec": 30,
    "health_check_timeout_sec": 10,
    "availability_weight": 0.3,
    "performance_weight": 0.4,
    "reliability_weight": 0.2,
    "resource_weight": 0.1,
    "max_acceptable_latency_us": 1000,
    "max_acceptable_error_rate": 0.01,
    "max_acceptable_load_pct": 80,
    "min_acceptable_iops": 10000
  },
  "failover": {
    "auto_failover_enabled": true,
    "failover_threshold_failures": 3,
    "failover_timeout_sec": 300
  },
  "integration": {
    "backend_integration_enabled": true,
    "backend_manager_endpoint": "http://localhost:8090"
  },
  "security": {
    "tls_enabled": true,
    "cert_file": "/etc/xdevice/tls/server.crt",
    "key_file": "/etc/xdevice/tls/server.key",
    "ca_file": "/etc/xdevice/tls/ca.crt"
  },
  "logging": {
    "log_level": "INFO",
    "log_file": "/var/log/xdevice/registry.log",
    "log_rotation_enabled": true,
    "max_log_file_size_mb": 100
  },
  "persistence": {
    "persistence_enabled": true,
    "persistence_backend": "sqlite",
    "connection_string": "/var/lib/xdevice/registry/targets.db",
    "sync_interval_sec": 60
  }
}
*/
```

### 7.2 监控与报警

```c
// 监控指标定义
typedef struct {
    // 系统指标
    uint32_t total_targets;
    uint32_t healthy_targets;
    uint32_t failed_targets;
    uint32_t maintenance_targets;
    
    // 性能指标
    uint64_t total_iops;
    uint64_t total_bandwidth_mbps;
    uint32_t avg_latency_us;
    double overall_health_score;
    
    // 事件指标
    uint32_t registrations_per_hour;
    uint32_t failures_per_hour;
    uint32_t recoveries_per_hour;
    uint32_t api_requests_per_minute;
    
    // 资源指标
    uint32_t memory_usage_mb;
    uint32_t cpu_usage_percentage;
    uint32_t disk_usage_percentage;
    uint32_t network_connections;
    
    time_t last_updated;
} registry_monitoring_metrics_t;

// 报警规则定义
typedef struct {
    char rule_name[64];
    metric_type_t metric_type;
    comparison_operator_t operator;
    double threshold_value;
    uint32_t duration_sec;          // 持续时间
    severity_level_t severity;
    bool enabled;
    char notification_targets[512]; // 通知目标列表
} alert_rule_t;

typedef enum {
    METRIC_HEALTHY_TARGET_RATIO,
    METRIC_AVERAGE_HEALTH_SCORE,
    METRIC_FAILURE_RATE,
    METRIC_API_RESPONSE_TIME,
    METRIC_MEMORY_USAGE,
    METRIC_CPU_USAGE,
    METRIC_DISK_USAGE
} metric_type_t;

typedef enum {
    OP_GREATER_THAN,
    OP_LESS_THAN,
    OP_EQUALS,
    OP_NOT_EQUALS
} comparison_operator_t;

// 监控系统初始化
int init_monitoring_system(nvmeof_registry_t *registry,
                          const registry_config_t *config) {
    registry->monitoring = malloc(sizeof(registry_monitoring_t));
    if (!registry->monitoring) return -1;
    
    // 初始化指标收集
    memset(&registry->monitoring->metrics, 0, sizeof(registry_monitoring_metrics_t));
    
    // 加载报警规则
    load_alert_rules(config->alert_rules_file, registry->monitoring);
    
    // 启动监控线程
    pthread_create(&registry->monitoring->collector_thread, NULL,
                  metrics_collector_thread, registry);
    
    pthread_create(&registry->monitoring->alerting_thread, NULL,
                  alerting_thread, registry);
    
    return 0;
}

// 指标收集线程
void* metrics_collector_thread(void *arg) {
    nvmeof_registry_t *registry = (nvmeof_registry_t*)arg;
    
    while (registry->running) {
        collect_registry_metrics(registry);
        
        // 每30秒收集一次指标
        sleep(30);
    }
    
    return NULL;
}

// 收集注册中心指标
void collect_registry_metrics(nvmeof_registry_t *registry) {
    registry_monitoring_metrics_t *metrics = &registry->monitoring->metrics;
    
    // 重置计数器
    memset(metrics, 0, sizeof(registry_monitoring_metrics_t));
    
    // 统计Target状态
    for (int i = 0; i < registry->target_count; i++) {
        nvmeof_target_info_t *target = &registry->targets[i];
        
        metrics->total_targets++;
        
        switch (target->status) {
            case TARGET_STATUS_AVAILABLE:
                metrics->healthy_targets++;
                break;
            case TARGET_STATUS_FAILED:
            case TARGET_STATUS_OFFLINE:
                metrics->failed_targets++;
                break;
            case TARGET_STATUS_MAINTENANCE:
                metrics->maintenance_targets++;
                break;
        }
        
        // 累加性能指标
        if (target->status == TARGET_STATUS_AVAILABLE) {
            metrics->total_iops += target->health_metrics.current_iops;
            metrics->total_bandwidth_mbps += target->health_metrics.current_bandwidth_mbps;
            metrics->avg_latency_us += target->health_metrics.current_latency_us;
            metrics->overall_health_score += target->health_metrics.health_score;
        }
    }
    
    // 计算平均值
    if (metrics->healthy_targets > 0) {
        metrics->avg_latency_us /= metrics->healthy_targets;
        metrics->overall_health_score /= metrics->healthy_targets;
    }
    
    // 收集系统资源指标
    collect_system_metrics(metrics);
    
    // 更新时间戳
    metrics->last_updated = time(NULL);
    
    // 更新Prometheus指标（如果启用）
    update_prometheus_metrics(metrics);
}

// 报警检查线程
void* alerting_thread(void *arg) {
    nvmeof_registry_t *registry = (nvmeof_registry_t*)arg;
    
    while (registry->running) {
        check_alert_rules(registry);
        
        // 每分钟检查一次报警
        sleep(60);
    }
    
    return NULL;
}

// 检查报警规则
void check_alert_rules(nvmeof_registry_t *registry) {
    registry_monitoring_t *monitoring = registry->monitoring;
    registry_monitoring_metrics_t *metrics = &monitoring->metrics;
    
    for (int i = 0; i < monitoring->alert_rule_count; i++) {
        alert_rule_t *rule = &monitoring->alert_rules[i];
        
        if (!rule->enabled) continue;
        
        double current_value = get_metric_value(metrics, rule->metric_type);
        bool condition_met = evaluate_condition(current_value, rule->operator, 
                                               rule->threshold_value);
        
        if (condition_met) {
            // 检查持续时间
            if (rule->last_triggered_time == 0) {
                rule->last_triggered_time = time(NULL);
            } else if (time(NULL) - rule->last_triggered_time >= rule->duration_sec) {
                // 触发报警
                trigger_alert(rule, current_value);
                rule->last_triggered_time = time(NULL); // 重置计时，避免重复报警
            }
        } else {
            rule->last_triggered_time = 0; // 重置计时
        }
    }
}

// 触发报警
void trigger_alert(const alert_rule_t *rule, double current_value) {
    // 构建报警消息
    char message[1024];
    snprintf(message, sizeof(message),
            "ALERT: %s\n"
            "Metric: %s\n"
            "Current Value: %.2f\n"
            "Threshold: %.2f\n"
            "Severity: %s\n"
            "Time: %s",
            rule->rule_name,
            get_metric_name(rule->metric_type),
            current_value,
            rule->threshold_value,
            get_severity_name(rule->severity),
            get_current_time_string());
    
    log_alert(rule->severity, "%s", message);
    
    // 发送通知
    send_alert_notifications(rule, message);
}

// 发送报警通知
void send_alert_notifications(const alert_rule_t *rule, const char *message) {
    // 解析通知目标
    char *targets = strdup(rule->notification_targets);
    char *target = strtok(targets, ",");
    
    while (target) {
        target = trim_whitespace(target);
        
        if (strncmp(target, "email:", 6) == 0) {
            send_email_alert(target + 6, rule->rule_name, message);
        } else if (strncmp(target, "slack:", 6) == 0) {
            send_slack_alert(target + 6, rule->rule_name, message);
        } else if (strncmp(target, "webhook:", 8) == 0) {
            send_webhook_alert(target + 8, rule->rule_name, message);
        }
        
        target = strtok(NULL, ",");
    }
    
    free(targets);
}
````
