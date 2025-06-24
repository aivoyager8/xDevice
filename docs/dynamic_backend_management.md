# 动态存储后端管理与故障转移

## 1. 需求分析

### 1.1 场景描述
- **10个节点集群**，每个节点配置多种存储后端（NVMe-oF、NFS、本地存储）
- **动态选择3个活跃后端**，用于当前数据存储
- **故障自动转移**，当活跃后端故障时自动替换为健康后端
- **零停机变更**，后端切换过程不影响业务连续性

### 1.2 技术挑战
- **后端状态管理**：实时监控所有候选后端的健康状态
- **动态选择算法**：基于性能、可用性、负载等指标智能选择
- **故障检测速度**：快速检测后端故障（秒级）
- **数据一致性**：切换过程中保证数据完整性
- **配置热更新**：运行时动态添加/移除候选后端

## 2. 架构设计

### 2.1 整体架构

```c
┌─────────────────────────────────────────────────────────────┐
│                  Raft Storage Layer                        │
└─────────────────┬───────────────────────────────────────────┘
                  │
┌─────────────────▼───────────────────────────────────────────┐
│            Dynamic Backend Manager                         │
│  ┌─────────────────────────────────────────────────────────┐ │
│  │               Active Backend Pool                      │ │
│  │  ┌─────────────┐ ┌─────────────┐ ┌─────────────┐      │ │
│  │  │   Backend   │ │   Backend   │ │   Backend   │      │ │
│  │  │      A      │ │      B      │ │      C      │      │ │
│  │  │   (Primary) │ │ (Secondary) │ │  (Tertiary) │      │ │
│  │  └─────────────┘ └─────────────┘ └─────────────┘      │ │
│  └─────────────────────────────────────────────────────────┘ │
│  ┌─────────────────────────────────────────────────────────┐ │
│  │              Candidate Backend Pool                    │ │
│  │  ┌───────┐ ┌───────┐ ┌───────┐ ┌───────┐ ┌───────┐    │ │
│  │  │Node-1 │ │Node-2 │ │Node-3 │ │ ... │ │Node-10│    │ │
│  │  │NVMe-oF│ │  NFS  │ │ Local │ │     │ │ Mixed │    │ │
│  │  └───────┘ └───────┘ └───────┘ └───────┘ └───────┘    │ │
│  └─────────────────────────────────────────────────────────┘ │
│  ┌─────────────────────────────────────────────────────────┐ │
│  │                Management Components                   │ │
│  │  ┌─────────────┐ ┌─────────────┐ ┌─────────────┐      │ │
│  │  │   Health    │ │ Selection   │ │  Failover   │      │ │
│  │  │  Monitor    │ │  Algorithm  │ │  Manager    │      │ │
│  │  └─────────────┘ └─────────────┘ └─────────────┘      │ │
│  └─────────────────────────────────────────────────────────┘ │
└─────────────────────────────────────────────────────────────┘
```

### 2.2 核心数据结构

```c
// 后端节点信息
typedef struct backend_node {
    char node_id[64];              // 节点ID
    char node_address[256];        // 节点地址
    
    // 后端列表
    struct backend_endpoint {
        char endpoint_id[64];      // 后端ID
        storage_backend_type_t type; // 后端类型
        char connection_string[512]; // 连接字符串
        
        // 性能指标
        struct {
            uint64_t avg_latency_us;    // 平均延迟
            uint64_t bandwidth_mbps;    // 带宽
            uint64_t iops;              // IOPS
            double error_rate;          // 错误率
            uint64_t last_update_time;  // 最后更新时间
        } metrics;
        
        // 状态信息
        backend_health_state_t health_state; // 健康状态
        uint64_t last_health_check;   // 最后健康检查时间
        uint32_t consecutive_failures; // 连续失败次数
        
        storage_interface_t *interface; // 存储接口实例
        
        struct backend_endpoint *next;
    } *endpoints;
    
    uint32_t endpoint_count;
    struct backend_node *next;
} backend_node_t;

// 后端健康状态
typedef enum {
    BACKEND_HEALTHY = 1,           // 健康
    BACKEND_DEGRADED = 2,          // 性能下降
    BACKEND_UNHEALTHY = 3,         // 不健康
    BACKEND_FAILED = 4,            // 完全失败
    BACKEND_UNKNOWN = 5            // 状态未知
} backend_health_state_t;

// 活跃后端配置
typedef struct {
    backend_endpoint_t *endpoints[MAX_ACTIVE_BACKENDS]; // 活跃后端列表
    uint32_t active_count;         // 当前活跃数量
    uint32_t target_count;         // 目标活跃数量
    
    // 选择策略配置
    struct {
        backend_selection_policy_t policy; // 选择策略
        double weight_latency;      // 延迟权重
        double weight_bandwidth;    // 带宽权重
        double weight_reliability;  // 可靠性权重
        double weight_load;         // 负载权重
    } selection_config;
    
    // 故障转移配置
    struct {
        uint32_t health_check_interval_ms; // 健康检查间隔
        uint32_t failure_threshold;        // 故障阈值
        uint32_t recovery_threshold;       // 恢复阈值
        uint32_t failover_timeout_ms;      // 故障转移超时
        bool enable_auto_failover;         // 启用自动故障转移
    } failover_config;
    
    pthread_mutex_t config_lock;   // 配置锁
} active_backend_config_t;

// 动态后端管理器
typedef struct {
    // 候选后端池
    backend_node_t *candidate_nodes;   // 候选节点列表
    uint32_t total_candidates;         // 总候选数量
    
    // 活跃后端池
    active_backend_config_t active_config;
    
    // 健康监控
    struct {
        pthread_t monitor_thread;     // 监控线程
        bool monitor_running;         // 监控运行状态
        uint32_t check_interval_ms;   // 检查间隔
        
        // 监控统计
        uint64_t total_checks;        // 总检查次数
        uint64_t failed_checks;       // 失败检查次数
        uint64_t last_full_scan_time; // 最后全量扫描时间
    } health_monitor;
    
    // 故障转移管理
    struct {
        pthread_t failover_thread;    // 故障转移线程
        bool failover_running;        // 故障转移运行状态
        
        // 故障转移队列
        struct failover_task {
            backend_endpoint_t *failed_backend;  // 失败的后端
            backend_endpoint_t *replacement;     // 替换后端
            uint64_t timestamp;                  // 时间戳
            struct failover_task *next;
        } *failover_queue;
        
        pthread_mutex_t queue_lock;   // 队列锁
        pthread_cond_t queue_cond;    // 队列条件变量
    } failover_mgr;
    
    // 性能统计
    struct {
        uint64_t successful_operations; // 成功操作数
        uint64_t failed_operations;     // 失败操作数
        uint64_t failover_count;        // 故障转移次数
        uint64_t backend_switches;      // 后端切换次数
        
        // 延迟统计
        uint64_t avg_operation_latency_us;
        uint64_t p99_operation_latency_us;
    } stats;
    
    pthread_mutex_t manager_lock;     // 管理器锁
} dynamic_backend_manager_t;

// 后端选择策略
typedef enum {
    SELECTION_ROUND_ROBIN = 1,     // 轮询
    SELECTION_LATENCY_FIRST = 2,   // 延迟优先
    SELECTION_BANDWIDTH_FIRST = 3, // 带宽优先
    SELECTION_LOAD_BALANCED = 4,   // 负载均衡
    SELECTION_WEIGHTED_SCORE = 5   // 加权评分
} backend_selection_policy_t;
```

## 3. 健康监控系统

### 3.1 多层次健康检查

```c
// 健康检查类型
typedef enum {
    HEALTH_CHECK_BASIC = 1,        // 基础连通性检查
    HEALTH_CHECK_PERFORMANCE = 2,  // 性能检查
    HEALTH_CHECK_STRESS = 3,       // 压力测试
    HEALTH_CHECK_FULL = 4          // 全面检查
} health_check_type_t;

// 健康检查实现
static int perform_health_check(backend_endpoint_t *endpoint, 
                               health_check_type_t check_type) {
    uint64_t start_time = get_monotonic_time_us();
    int result = 0;
    
    switch (check_type) {
        case HEALTH_CHECK_BASIC:
            result = basic_connectivity_check(endpoint);
            break;
            
        case HEALTH_CHECK_PERFORMANCE:
            result = performance_benchmark_check(endpoint);
            break;
            
        case HEALTH_CHECK_STRESS:
            result = stress_test_check(endpoint);
            break;
            
        case HEALTH_CHECK_FULL:
            result = comprehensive_health_check(endpoint);
            break;
    }
    
    uint64_t end_time = get_monotonic_time_us();
    uint64_t latency = end_time - start_time;
    
    // 更新性能指标
    update_endpoint_metrics(endpoint, latency, result == 0);
    
    return result;
}

// 基础连通性检查
static int basic_connectivity_check(backend_endpoint_t *endpoint) {
    if (!endpoint->interface) {
        return -1;
    }
    
    // 执行轻量级读写测试
    char test_data[] = "health_check_probe";
    char read_buffer[32];
    
    // 写入测试数据
    int write_result = endpoint->interface->write_sync(
        endpoint->interface,
        "_health_check_", 0,
        test_data, sizeof(test_data));
    
    if (write_result != 0) {
        return -1;
    }
    
    // 读取测试数据
    int read_result = endpoint->interface->read_sync(
        endpoint->interface,
        "_health_check_", 0,
        read_buffer, sizeof(test_data));
    
    if (read_result != 0) {
        return -1;
    }
    
    // 验证数据一致性
    if (memcmp(test_data, read_buffer, sizeof(test_data)) != 0) {
        return -1;
    }
    
    // 清理测试数据
    endpoint->interface->delete_file(endpoint->interface, "_health_check_");
    
    return 0;
}

// 性能基准检查
static int performance_benchmark_check(backend_endpoint_t *endpoint) {
    const size_t test_size = 4096;      // 4KB测试块
    const uint32_t test_count = 100;    // 100次测试
    
    char *test_data = malloc(test_size);
    if (!test_data) return -1;
    
    memset(test_data, 0xAA, test_size);
    
    uint64_t total_latency = 0;
    uint32_t successful_ops = 0;
    
    for (uint32_t i = 0; i < test_count; i++) {
        uint64_t start = get_monotonic_time_us();
        
        char test_path[64];
        snprintf(test_path, sizeof(test_path), "_perf_test_%u", i);
        
        int result = endpoint->interface->write_sync(
            endpoint->interface, test_path, 0, test_data, test_size);
        
        uint64_t end = get_monotonic_time_us();
        
        if (result == 0) {
            total_latency += (end - start);
            successful_ops++;
        }
        
        // 清理测试文件
        endpoint->interface->delete_file(endpoint->interface, test_path);
    }
    
    free(test_data);
    
    if (successful_ops == 0) {
        return -1;
    }
    
    // 更新性能指标
    uint64_t avg_latency = total_latency / successful_ops;
    endpoint->metrics.avg_latency_us = avg_latency;
    endpoint->metrics.error_rate = 
        (double)(test_count - successful_ops) / test_count;
    
    // 性能阈值检查
    if (avg_latency > MAX_ACCEPTABLE_LATENCY_US) {
        return -1; // 延迟过高
    }
    
    if (endpoint->metrics.error_rate > MAX_ACCEPTABLE_ERROR_RATE) {
        return -1; // 错误率过高
    }
    
    return 0;
}

// 健康监控主线程
static void* health_monitor_thread(void *arg) {
    dynamic_backend_manager_t *manager = (dynamic_backend_manager_t*)arg;
    
    while (manager->health_monitor.monitor_running) {
        uint64_t scan_start = get_monotonic_time_us();
        
        // 扫描所有候选后端
        for (backend_node_t *node = manager->candidate_nodes; 
             node; node = node->next) {
            
            for (backend_endpoint_t *endpoint = node->endpoints;
                 endpoint; endpoint = endpoint->next) {
                
                // 执行健康检查
                health_check_type_t check_type = determine_check_type(endpoint);
                int check_result = perform_health_check(endpoint, check_type);
                
                // 更新健康状态
                update_endpoint_health_state(endpoint, check_result);
                
                manager->health_monitor.total_checks++;
                if (check_result != 0) {
                    manager->health_monitor.failed_checks++;
                }
            }
        }
        
        uint64_t scan_end = get_monotonic_time_us();
        manager->health_monitor.last_full_scan_time = scan_end - scan_start;
        
        // 等待下次检查
        usleep(manager->health_monitor.check_interval_ms * 1000);
    }
    
    return NULL;
}

// 动态健康状态更新
static void update_endpoint_health_state(backend_endpoint_t *endpoint, 
                                        int check_result) {
    uint64_t current_time = get_monotonic_time_us();
    endpoint->last_health_check = current_time;
    
    if (check_result == 0) {
        // 检查成功
        endpoint->consecutive_failures = 0;
        
        if (endpoint->health_state == BACKEND_FAILED ||
            endpoint->health_state == BACKEND_UNHEALTHY) {
            endpoint->health_state = BACKEND_DEGRADED;
            log_info("Backend %s recovering from failure", endpoint->endpoint_id);
        } else if (endpoint->health_state == BACKEND_DEGRADED) {
            endpoint->health_state = BACKEND_HEALTHY;
            log_info("Backend %s fully recovered", endpoint->endpoint_id);
        }
    } else {
        // 检查失败
        endpoint->consecutive_failures++;
        
        if (endpoint->consecutive_failures >= FAILURE_THRESHOLD) {
            backend_health_state_t old_state = endpoint->health_state;
            endpoint->health_state = BACKEND_FAILED;
            
            if (old_state != BACKEND_FAILED) {
                log_warning("Backend %s marked as FAILED after %u consecutive failures",
                           endpoint->endpoint_id, endpoint->consecutive_failures);
                
                // 触发故障转移
                trigger_failover_if_active(endpoint);
            }
        } else if (endpoint->consecutive_failures >= DEGRADED_THRESHOLD) {
            if (endpoint->health_state == BACKEND_HEALTHY) {
                endpoint->health_state = BACKEND_DEGRADED;
                log_warning("Backend %s degraded", endpoint->endpoint_id);
            }
        }
    }
}
```

## 4. 智能后端选择算法

### 4.1 多因子评分算法

```c
// 后端评分结构
typedef struct {
    backend_endpoint_t *endpoint;
    double total_score;          // 总评分
    double latency_score;        // 延迟评分
    double bandwidth_score;      // 带宽评分
    double reliability_score;    // 可靠性评分
    double load_score;          // 负载评分
} backend_score_t;

// 智能后端选择
static int select_optimal_backends(dynamic_backend_manager_t *manager,
                                  backend_endpoint_t **selected,
                                  uint32_t target_count) {
    // 收集所有健康的候选后端
    backend_endpoint_t **candidates = NULL;
    uint32_t candidate_count = 0;
    
    collect_healthy_candidates(manager, &candidates, &candidate_count);
    
    if (candidate_count < target_count) {
        log_warning("Insufficient healthy backends: %u available, %u required",
                   candidate_count, target_count);
        return -ENOENT;
    }
    
    // 计算评分
    backend_score_t *scores = malloc(candidate_count * sizeof(backend_score_t));
    if (!scores) {
        free(candidates);
        return -ENOMEM;
    }
    
    for (uint32_t i = 0; i < candidate_count; i++) {
        calculate_backend_score(&scores[i], candidates[i], 
                               &manager->active_config.selection_config);
    }
    
    // 排序选择最优的后端
    qsort(scores, candidate_count, sizeof(backend_score_t), compare_scores);
    
    // 选择前N个最优后端
    for (uint32_t i = 0; i < target_count; i++) {
        selected[i] = scores[i].endpoint;
        log_info("Selected backend %s with score %.3f",
                selected[i]->endpoint_id, scores[i].total_score);
    }
    
    free(scores);
    free(candidates);
    return 0;
}

// 计算后端综合评分
static void calculate_backend_score(backend_score_t *score,
                                   backend_endpoint_t *endpoint,
                                   const selection_config_t *config) {
    score->endpoint = endpoint;
    
    // 延迟评分 (越低越好)
    score->latency_score = calculate_latency_score(endpoint->metrics.avg_latency_us);
    
    // 带宽评分 (越高越好)
    score->bandwidth_score = calculate_bandwidth_score(endpoint->metrics.bandwidth_mbps);
    
    // 可靠性评分 (基于错误率和历史故障)
    score->reliability_score = calculate_reliability_score(endpoint);
    
    // 负载评分 (当前负载越低越好)
    score->load_score = calculate_load_score(endpoint);
    
    // 加权总评分
    score->total_score = 
        config->weight_latency * score->latency_score +
        config->weight_bandwidth * score->bandwidth_score +
        config->weight_reliability * score->reliability_score +
        config->weight_load * score->load_score;
    
    log_debug("Backend %s scores: L=%.3f B=%.3f R=%.3f L=%.3f Total=%.3f",
             endpoint->endpoint_id,
             score->latency_score, score->bandwidth_score,
             score->reliability_score, score->load_score,
             score->total_score);
}

// 延迟评分算法
static double calculate_latency_score(uint64_t latency_us) {
    // 使用反向对数评分，延迟越低分数越高
    const uint64_t baseline_latency = 100;  // 100us基准
    const uint64_t max_latency = 10000;     // 10ms最大可接受延迟
    
    if (latency_us <= baseline_latency) {
        return 1.0;  // 满分
    }
    
    if (latency_us >= max_latency) {
        return 0.1;  // 最低分
    }
    
    // 对数衰减
    double ratio = (double)latency_us / baseline_latency;
    return 1.0 / (1.0 + log(ratio));
}

// 带宽评分算法
static double calculate_bandwidth_score(uint64_t bandwidth_mbps) {
    // 使用对数增长评分，带宽越高分数越高
    const uint64_t baseline_bandwidth = 100;  // 100Mbps基准
    const uint64_t max_bandwidth = 10000;     // 10Gbps上限
    
    if (bandwidth_mbps >= max_bandwidth) {
        return 1.0;  // 满分
    }
    
    if (bandwidth_mbps <= baseline_bandwidth) {
        return 0.3;  // 基础分
    }
    
    // 对数增长
    double ratio = (double)bandwidth_mbps / baseline_bandwidth;
    return 0.3 + 0.7 * log(ratio) / log((double)max_bandwidth / baseline_bandwidth);
}

// 可靠性评分算法
static double calculate_reliability_score(backend_endpoint_t *endpoint) {
    double score = 1.0;
    
    // 错误率惩罚
    if (endpoint->metrics.error_rate > 0) {
        score *= (1.0 - endpoint->metrics.error_rate);
    }
    
    // 连续失败惩罚
    if (endpoint->consecutive_failures > 0) {
        score *= exp(-0.1 * endpoint->consecutive_failures);
    }
    
    // 健康状态调整
    switch (endpoint->health_state) {
        case BACKEND_HEALTHY:
            break;  // 无调整
        case BACKEND_DEGRADED:
            score *= 0.8;
            break;
        case BACKEND_UNHEALTHY:
            score *= 0.5;
            break;
        case BACKEND_FAILED:
            score = 0.0;
            break;
        default:
            score *= 0.6;
            break;
    }
    
    return max(score, 0.0);
}

// 动态重新选择触发器
static void trigger_backend_reselection(dynamic_backend_manager_t *manager,
                                       const char *reason) {
    log_info("Triggering backend reselection: %s", reason);
    
    pthread_mutex_lock(&manager->manager_lock);
    
    backend_endpoint_t *new_selection[MAX_ACTIVE_BACKENDS];
    uint32_t target_count = manager->active_config.target_count;
    
    int result = select_optimal_backends(manager, new_selection, target_count);
    
    if (result == 0) {
        // 执行平滑切换
        smooth_backend_transition(manager, new_selection, target_count);
        manager->stats.backend_switches++;
    } else {
        log_error("Failed to reselect backends: %d", result);
    }
    
    pthread_mutex_unlock(&manager->manager_lock);
}
```

## 5. 零停机故障转移

### 5.1 平滑后端切换

```c
// 平滑后端过渡
static int smooth_backend_transition(dynamic_backend_manager_t *manager,
                                   backend_endpoint_t **new_backends,
                                   uint32_t new_count) {
    active_backend_config_t *config = &manager->active_config;
    
    // 分析变更
    struct transition_plan {
        backend_endpoint_t **to_remove;   // 需要移除的后端
        backend_endpoint_t **to_add;      // 需要添加的后端
        backend_endpoint_t **to_keep;     // 保持不变的后端
        uint32_t remove_count;
        uint32_t add_count;
        uint32_t keep_count;
    } plan;
    
    analyze_transition_changes(config->endpoints, config->active_count,
                              new_backends, new_count, &plan);
    
    log_info("Transition plan: keep=%u, add=%u, remove=%u",
             plan.keep_count, plan.add_count, plan.remove_count);
    
    // 第一阶段：添加新后端
    for (uint32_t i = 0; i < plan.add_count; i++) {
        backend_endpoint_t *new_backend = plan.to_add[i];
        
        // 预热新后端
        if (warmup_backend(new_backend) != 0) {
            log_error("Failed to warmup new backend %s", 
                     new_backend->endpoint_id);
            continue;
        }
        
        // 添加到活跃列表
        config->endpoints[config->active_count] = new_backend;
        config->active_count++;
        
        log_info("Added new backend %s to active pool", 
                new_backend->endpoint_id);
    }
    
    // 第二阶段：数据迁移（如需要）
    if (plan.remove_count > 0) {
        migrate_data_from_removing_backends(&plan);
    }
    
    // 第三阶段：移除旧后端
    for (uint32_t i = 0; i < plan.remove_count; i++) {
        backend_endpoint_t *old_backend = plan.to_remove[i];
        
        // 从活跃列表移除
        remove_from_active_list(config, old_backend);
        
        // 优雅关闭
        graceful_backend_shutdown(old_backend);
        
        log_info("Removed backend %s from active pool", 
                old_backend->endpoint_id);
    }
    
    // 更新配置
    config->active_count = new_count;
    memcpy(config->endpoints, new_backends, 
           new_count * sizeof(backend_endpoint_t*));
    
    log_info("Backend transition completed successfully");
    
    // 清理计划结构
    cleanup_transition_plan(&plan);
    
    return 0;
}

// 后端预热
static int warmup_backend(backend_endpoint_t *backend) {
    log_info("Warming up backend %s", backend->endpoint_id);
    
    // 建立连接
    if (backend->interface->init) {
        int result = backend->interface->init(backend->interface, NULL);
        if (result != 0) {
            log_error("Failed to initialize backend %s: %d", 
                     backend->endpoint_id, result);
            return result;
        }
    }
    
    // 执行预热写入
    const char warmup_data[] = "backend_warmup_test";
    int write_result = backend->interface->write_sync(
        backend->interface, "_warmup_test", 0,
        warmup_data, sizeof(warmup_data));
    
    if (write_result != 0) {
        log_error("Backend %s warmup write failed: %d",
                 backend->endpoint_id, write_result);
        return write_result;
    }
    
    // 执行预热读取
    char read_buffer[32];
    int read_result = backend->interface->read_sync(
        backend->interface, "_warmup_test", 0,
        read_buffer, sizeof(warmup_data));
    
    if (read_result != 0) {
        log_error("Backend %s warmup read failed: %d",
                 backend->endpoint_id, read_result);
        return read_result;
    }
    
    // 清理测试数据
    backend->interface->delete_file(backend->interface, "_warmup_test");
    
    log_info("Backend %s warmed up successfully", backend->endpoint_id);
    return 0;
}

// 故障转移任务处理
static void* failover_worker_thread(void *arg) {
    dynamic_backend_manager_t *manager = (dynamic_backend_manager_t*)arg;
    
    while (manager->failover_mgr.failover_running) {
        pthread_mutex_lock(&manager->failover_mgr.queue_lock);
        
        // 等待故障转移任务
        while (manager->failover_mgr.failover_queue == NULL &&
               manager->failover_mgr.failover_running) {
            pthread_cond_wait(&manager->failover_mgr.queue_cond,
                             &manager->failover_mgr.queue_lock);
        }
        
        if (!manager->failover_mgr.failover_running) {
            pthread_mutex_unlock(&manager->failover_mgr.queue_lock);
            break;
        }
        
        // 取出故障转移任务
        struct failover_task *task = manager->failover_mgr.failover_queue;
        manager->failover_mgr.failover_queue = task->next;
        
        pthread_mutex_unlock(&manager->failover_mgr.queue_lock);
        
        // 执行故障转移
        execute_failover_task(manager, task);
        
        // 清理任务
        free(task);
        
        manager->stats.failover_count++;
    }
    
    return NULL;
}

// 执行故障转移任务
static int execute_failover_task(dynamic_backend_manager_t *manager,
                                struct failover_task *task) {
    uint64_t start_time = get_monotonic_time_us();
    
    log_info("Executing failover: replacing %s with %s",
             task->failed_backend->endpoint_id,
             task->replacement ? task->replacement->endpoint_id : "auto-select");
    
    pthread_mutex_lock(&manager->manager_lock);
    
    active_backend_config_t *config = &manager->active_config;
    
    // 找到失败后端在活跃列表中的位置
    int failed_index = -1;
    for (uint32_t i = 0; i < config->active_count; i++) {
        if (config->endpoints[i] == task->failed_backend) {
            failed_index = i;
            break;
        }
    }
    
    if (failed_index == -1) {
        log_warning("Failed backend %s not found in active list",
                   task->failed_backend->endpoint_id);
        pthread_mutex_unlock(&manager->manager_lock);
        return -1;
    }
    
    // 选择替换后端
    backend_endpoint_t *replacement = task->replacement;
    if (!replacement) {
        replacement = select_best_replacement(manager, task->failed_backend);
    }
    
    if (!replacement) {
        log_error("No suitable replacement found for failed backend %s",
                 task->failed_backend->endpoint_id);
        pthread_mutex_unlock(&manager->manager_lock);
        return -1;
    }
    
    // 预热替换后端
    if (warmup_backend(replacement) != 0) {
        log_error("Failed to warmup replacement backend %s",
                 replacement->endpoint_id);
        pthread_mutex_unlock(&manager->manager_lock);
        return -1;
    }
    
    // 原子性替换
    config->endpoints[failed_index] = replacement;
    
    // 优雅关闭失败后端
    graceful_backend_shutdown(task->failed_backend);
    
    uint64_t end_time = get_monotonic_time_us();
    uint64_t failover_latency = end_time - start_time;
    
    log_info("Failover completed in %lu us: %s -> %s",
             failover_latency,
             task->failed_backend->endpoint_id,
             replacement->endpoint_id);
    
    pthread_mutex_unlock(&manager->manager_lock);
    
    return 0;
}
```

## 6. 配置和管理接口

### 6.1 配置文件格式

```yaml
# dynamic_backend_config.yaml
dynamic_backend_manager:
  # 基本配置
  target_active_backends: 3
  health_check_interval_ms: 5000
  
  # 候选后端节点
  candidate_nodes:
    - node_id: "node-01"
      address: "192.168.1.101"
      endpoints:
        - endpoint_id: "nvmeof-01"
          type: "nvmeof"
          connection_string: "nvme://192.168.1.101:4420/namespace1"
          weight: 1.0
        - endpoint_id: "nfs-01"
          type: "nfs"
          connection_string: "nfs://192.168.1.101:/export/xdevice"
          weight: 0.8
    
    - node_id: "node-02"
      address: "192.168.1.102"
      endpoints:
        - endpoint_id: "nvmeof-02"
          type: "nvmeof"
          connection_string: "nvme://192.168.1.102:4420/namespace1"
          weight: 1.0
        - endpoint_id: "local-02"
          type: "local"
          connection_string: "file:///data/xdevice"
          weight: 0.6
    
    # ... 更多节点配置
    - node_id: "node-10"
      address: "192.168.1.110"
      endpoints:
        - endpoint_id: "mixed-10"
          type: "nvmeof"
          connection_string: "nvme://192.168.1.110:4420/namespace1"
          weight: 0.9

  # 选择策略配置
  selection_policy:
    algorithm: "weighted_score"
    weights:
      latency: 0.4      # 延迟权重40%
      bandwidth: 0.3    # 带宽权重30%
      reliability: 0.2  # 可靠性权重20%
      load: 0.1        # 负载权重10%
  
  # 故障转移配置
  failover:
    enable_auto_failover: true
    failure_threshold: 3        # 连续3次失败触发故障转移
    recovery_threshold: 5       # 连续5次成功才认为恢复
    failover_timeout_ms: 30000  # 故障转移超时30秒
    enable_data_migration: false # 禁用数据迁移（WAL场景）
  
  # 监控配置
  monitoring:
    enable_detailed_metrics: true
    metrics_export_interval_ms: 10000
    health_check_types:
      - "basic"         # 基础检查
      - "performance"   # 性能检查
```

### 6.2 运行时管理API

```c
// 管理API接口
typedef struct {
    // 配置管理
    int (*load_config)(dynamic_backend_manager_t *manager, const char *config_file);
    int (*reload_config)(dynamic_backend_manager_t *manager);
    int (*save_config)(dynamic_backend_manager_t *manager, const char *config_file);
    
    // 后端管理
    int (*add_candidate_node)(dynamic_backend_manager_t *manager, 
                             const backend_node_t *node);
    int (*remove_candidate_node)(dynamic_backend_manager_t *manager, 
                                const char *node_id);
    int (*update_node_config)(dynamic_backend_manager_t *manager,
                             const char *node_id, const backend_node_t *new_config);
    
    // 活跃后端管理
    int (*force_backend_switch)(dynamic_backend_manager_t *manager,
                               const char *old_backend_id, const char *new_backend_id);
    int (*set_backend_weight)(dynamic_backend_manager_t *manager,
                             const char *backend_id, double weight);
    int (*trigger_reselection)(dynamic_backend_manager_t *manager, const char *reason);
    
    // 监控和状态
    int (*get_active_backends)(dynamic_backend_manager_t *manager,
                              backend_endpoint_t ***backends, uint32_t *count);
    int (*get_backend_status)(dynamic_backend_manager_t *manager,
                             const char *backend_id, backend_status_t *status);
    int (*get_manager_stats)(dynamic_backend_manager_t *manager, 
                            manager_stats_t *stats);
    
} dynamic_backend_api_t;

// CLI命令实现
int cli_list_candidates(int argc, char **argv) {
    dynamic_backend_manager_t *manager = get_global_backend_manager();
    
    printf("Candidate Backend Nodes:\n");
    printf("%-12s %-15s %-12s %-10s %-12s\n", 
           "NODE_ID", "ADDRESS", "ENDPOINT_ID", "TYPE", "STATUS");
    printf("================================================================\n");
    
    for (backend_node_t *node = manager->candidate_nodes; node; node = node->next) {
        for (backend_endpoint_t *endpoint = node->endpoints; 
             endpoint; endpoint = endpoint->next) {
            
            const char *status_str = health_state_to_string(endpoint->health_state);
            const char *type_str = backend_type_to_string(endpoint->type);
            
            printf("%-12s %-15s %-12s %-10s %-12s\n",
                   node->node_id, node->node_address,
                   endpoint->endpoint_id, type_str, status_str);
        }
    }
    
    return 0;
}

int cli_list_active_backends(int argc, char **argv) {
    dynamic_backend_manager_t *manager = get_global_backend_manager();
    active_backend_config_t *config = &manager->active_config;
    
    printf("Active Backend Pool:\n");
    printf("%-12s %-10s %-12s %-12s %-12s\n",
           "ENDPOINT_ID", "TYPE", "LATENCY_US", "BANDWIDTH", "LOAD");
    printf("================================================================\n");
    
    pthread_mutex_lock(&config->config_lock);
    
    for (uint32_t i = 0; i < config->active_count; i++) {
        backend_endpoint_t *endpoint = config->endpoints[i];
        
        printf("%-12s %-10s %-12lu %-12lu %.2f%%\n",
               endpoint->endpoint_id,
               backend_type_to_string(endpoint->type),
               endpoint->metrics.avg_latency_us,
               endpoint->metrics.bandwidth_mbps,
               get_backend_load_percentage(endpoint));
    }
    
    pthread_mutex_unlock(&config->config_lock);
    
    return 0;
}

int cli_force_failover(int argc, char **argv) {
    if (argc < 3) {
        printf("Usage: %s <failed_backend_id> [replacement_backend_id]\n", argv[0]);
        return -1;
    }
    
    const char *failed_backend_id = argv[1];
    const char *replacement_backend_id = (argc > 2) ? argv[2] : NULL;
    
    dynamic_backend_manager_t *manager = get_global_backend_manager();
    
    // 找到失败的后端
    backend_endpoint_t *failed_backend = find_backend_by_id(manager, failed_backend_id);
    if (!failed_backend) {
        printf("Error: Backend '%s' not found\n", failed_backend_id);
        return -1;
    }
    
    // 找到替换后端（如果指定）
    backend_endpoint_t *replacement = NULL;
    if (replacement_backend_id) {
        replacement = find_backend_by_id(manager, replacement_backend_id);
        if (!replacement) {
            printf("Error: Replacement backend '%s' not found\n", replacement_backend_id);
            return -1;
        }
    }
    
    // 触发故障转移
    struct failover_task *task = malloc(sizeof(struct failover_task));
    task->failed_backend = failed_backend;
    task->replacement = replacement;
    task->timestamp = get_monotonic_time_us();
    task->next = NULL;
    
    pthread_mutex_lock(&manager->failover_mgr.queue_lock);
    
    // 添加到故障转移队列
    if (manager->failover_mgr.failover_queue == NULL) {
        manager->failover_mgr.failover_queue = task;
    } else {
        struct failover_task *tail = manager->failover_mgr.failover_queue;
        while (tail->next) tail = tail->next;
        tail->next = task;
    }
    
    pthread_cond_signal(&manager->failover_mgr.queue_cond);
    pthread_mutex_unlock(&manager->failover_mgr.queue_lock);
    
    printf("Failover triggered: %s -> %s\n", 
           failed_backend_id, 
           replacement_backend_id ? replacement_backend_id : "auto-select");
    
    return 0;
}

// 配置热重载
int cli_reload_config(int argc, char **argv) {
    const char *config_file = (argc > 1) ? argv[1] : "config/dynamic_backend.yaml";
    
    dynamic_backend_manager_t *manager = get_global_backend_manager();
    
    printf("Reloading configuration from %s...\n", config_file);
    
    int result = manager->api.reload_config(manager);
    if (result == 0) {
        printf("Configuration reloaded successfully\n");
    } else {
        printf("Failed to reload configuration: %d\n", result);
    }
    
    return result;
}
```

## 7. 使用示例

### 7.1 初始化和配置

```c
#include "dynamic_backend_manager.h"

int main() {
    // 创建动态后端管理器
    dynamic_backend_manager_t *manager = create_dynamic_backend_manager();
    if (!manager) {
        fprintf(stderr, "Failed to create backend manager\n");
        return -1;
    }
    
    // 加载配置
    if (load_backend_config(manager, "config/dynamic_backend.yaml") != 0) {
        fprintf(stderr, "Failed to load backend configuration\n");
        return -1;
    }
    
    // 启动管理器
    if (start_dynamic_backend_manager(manager) != 0) {
        fprintf(stderr, "Failed to start backend manager\n");
        return -1;
    }
    
    printf("Dynamic backend manager started with %u candidate nodes\n",
           count_candidate_nodes(manager));
    
    // 等待初始后端选择完成
    wait_for_initial_selection(manager, 10000); // 10秒超时
    
    // 获取当前活跃后端
    backend_endpoint_t **active_backends = NULL;
    uint32_t active_count = 0;
    
    get_active_backends(manager, &active_backends, &active_count);
    
    printf("Selected %u active backends:\n", active_count);
    for (uint32_t i = 0; i < active_count; i++) {
        printf("  - %s (%s)\n", 
               active_backends[i]->endpoint_id,
               backend_type_to_string(active_backends[i]->type));
    }
    
    // 使用后端进行I/O操作
    demonstrate_io_operations(active_backends, active_count);
    
    // 清理
    stop_dynamic_backend_manager(manager);
    destroy_dynamic_backend_manager(manager);
    
    return 0;
}

// I/O操作示例
static void demonstrate_io_operations(backend_endpoint_t **backends, uint32_t count) {
    const char *test_data = "Hello, dynamic backend!";
    char read_buffer[256];
    
    // 轮询写入到所有活跃后端
    for (uint32_t i = 0; i < count; i++) {
        backend_endpoint_t *backend = backends[i];
        
        printf("Writing to backend %s...\n", backend->endpoint_id);
        
        int write_result = backend->interface->write_sync(
            backend->interface, "test_file", 0, test_data, strlen(test_data));
        
        if (write_result == 0) {
            printf("  Write successful\n");
            
            // 读取验证
            int read_result = backend->interface->read_sync(
                backend->interface, "test_file", 0, read_buffer, strlen(test_data));
            
            if (read_result == 0) {
                read_buffer[strlen(test_data)] = '\0';
                printf("  Read successful: %s\n", read_buffer);
            } else {
                printf("  Read failed: %d\n", read_result);
            }
        } else {
            printf("  Write failed: %d\n", write_result);
        }
    }
}

// 故障模拟和恢复
static void simulate_backend_failure(dynamic_backend_manager_t *manager) {
    printf("\n=== Simulating Backend Failure ===\n");
    
    // 人为标记一个后端为失败状态
    backend_endpoint_t **active_backends = NULL;
    uint32_t active_count = 0;
    
    get_active_backends(manager, &active_backends, &active_count);
    
    if (active_count > 0) {
        backend_endpoint_t *victim = active_backends[0];
        printf("Marking backend %s as failed\n", victim->endpoint_id);
        
        // 模拟失败
        victim->health_state = BACKEND_FAILED;
        victim->consecutive_failures = 10;
        
        // 触发故障转移
        trigger_failover_if_active(victim);
        
        // 等待故障转移完成
        sleep(5);
        
        // 检查新的活跃后端
        get_active_backends(manager, &active_backends, &active_count);
        printf("After failover, active backends:\n");
        for (uint32_t i = 0; i < active_count; i++) {
            printf("  - %s\n", active_backends[i]->endpoint_id);
        }
    }
}
```

## 8. 监控和告警

### 8.1 关键指标监控

```c
// 监控指标定义
typedef struct {
    // 可用性指标
    double overall_availability;        // 整体可用性
    uint32_t healthy_backend_count;     // 健康后端数量
    uint32_t total_backend_count;       // 总后端数量
    
    // 性能指标
    uint64_t avg_operation_latency_us;  // 平均操作延迟
    uint64_t p99_operation_latency_us;  // P99操作延迟
    uint64_t operations_per_second;     // 每秒操作数
    
    // 故障转移指标
    uint32_t failover_events_last_hour; // 最近1小时故障转移次数
    uint64_t avg_failover_time_ms;      // 平均故障转移时间
    uint32_t failed_failovers;          // 失败的故障转移次数
    
    // 后端健康指标
    struct backend_health_summary {
        uint32_t healthy_count;
        uint32_t degraded_count;
        uint32_t unhealthy_count;
        uint32_t failed_count;
    } health_summary;
    
} monitoring_metrics_t;

// 预定义告警规则
static alert_rule_t default_alert_rules[] = {
    {
        .rule_name = "HighFailoverRate",
        .condition = "failover_events_last_hour > 10",
        .severity = ALERT_WARNING,
        .evaluation_interval_sec = 60,
        .consecutive_threshold = 2,
        .enabled = true
    },
    {
        .rule_name = "LowBackendAvailability", 
        .condition = "overall_availability < 0.7",
        .severity = ALERT_CRITICAL,
        .evaluation_interval_sec = 30,
        .consecutive_threshold = 1,
        .enabled = true
    },
    {
        .rule_name = "HighOperationLatency",
        .condition = "p99_operation_latency_us > 10000", // > 10ms
        .severity = ALERT_WARNING,
        .evaluation_interval_sec = 120,
        .consecutive_threshold = 3,
        .enabled = true
    },
    {
        .rule_name = "NoHealthyBackends",
        .condition = "healthy_backend_count == 0",
        .severity = ALERT_CRITICAL,
        .evaluation_interval_sec = 10,
        .consecutive_threshold = 1,
        .enabled = true
    }
};
```

## 9. 总结

这个动态存储后端管理系统提供了以下核心能力：

### 🎯 **核心功能**
1. **智能后端选择**：基于延迟、带宽、可靠性、负载的多因子评分算法
2. **零停机故障转移**：秒级故障检测，平滑后端切换
3. **实时健康监控**：多层次健康检查，智能状态管理
4. **配置热更新**：运行时动态添加/移除候选后端
5. **全面监控告警**：丰富的性能指标和自动告警

### 🚀 **技术特点**
- **高可用性**：自动故障转移，确保服务连续性
- **高性能**：智能负载均衡，优化I/O路径
- **易管理**：CLI工具，配置文件驱动
- **可观测性**：详细监控指标，实时状态展示

### 📋 **使用场景**
- ✅ 10节点集群，每节点多种存储后端（NVMe-oF、NFS、本地）
- ✅ 动态选择3个最优后端作为活跃存储
- ✅ 自动故障检测和替换
- ✅ 零停机运维操作

这个设计完美解决了您提出的需求，实现了灵活的多后端管理和自动故障转移能力！
