#include "../include/storage_interface.h"
#include "../include/xdevice.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <time.h>

// 外部函数声明
extern storage_interface_t* create_local_storage_interface(void);
extern storage_interface_t* create_mock_storage_interface(void);
extern void mock_storage_set_delays(storage_interface_t *iface,
                                   uint32_t read_delay_us, uint32_t write_delay_us);

/**
 * 测试存储接口基本功能
 */
static int test_storage_interface(storage_interface_t *storage, const char *name) {
    printf("\n=== 测试存储接口: %s ===\n", name);
    
    int ret;
    
    // 1. 初始化
    ret = storage->init(storage, "/tmp/xdevice_test");
    if (ret != XDEVICE_OK) {
        printf("错误: 存储初始化失败 (%d)\n", ret);
        return -1;
    }
    printf("✓ 存储初始化成功\n");
    
    // 2. 健康检查
    ret = storage->health_check(storage);
    if (ret != XDEVICE_OK) {
        printf("错误: 健康检查失败 (%d)\n", ret);
        return -1;
    }
    printf("✓ 健康检查成功\n");
    
    // 3. 创建文件
    const char *test_path = "test_file.dat";
    ret = storage->create_file(storage, test_path, 1024);
    if (ret != XDEVICE_OK) {
        printf("错误: 创建文件失败 (%d)\n", ret);
        return -1;
    }
    printf("✓ 创建文件成功\n");
    
    // 4. 写入数据
    const char *test_data = "Hello, XDevice Storage!";
    size_t test_data_len = strlen(test_data);
    ret = storage->write_sync(storage, test_path, 0, test_data, test_data_len);
    if (ret != XDEVICE_OK) {
        printf("错误: 写入数据失败 (%d)\n", ret);
        return -1;
    }
    printf("✓ 写入数据成功\n");
    
    // 5. 同步文件
    if (storage->fsync) {
        ret = storage->fsync(storage, test_path);
        if (ret != XDEVICE_OK) {
            printf("错误: 同步文件失败 (%d)\n", ret);
            return -1;
        }
        printf("✓ 同步文件成功\n");
    }
    
    // 6. 读取数据
    char read_buffer[128];
    memset(read_buffer, 0, sizeof(read_buffer));
    ret = storage->read_sync(storage, test_path, 0, read_buffer, test_data_len);
    if (ret != XDEVICE_OK) {
        printf("错误: 读取数据失败 (%d)\n", ret);
        return -1;
    }
    printf("✓ 读取数据成功\n");
    
    // 7. 验证数据
    if (memcmp(test_data, read_buffer, test_data_len) != 0) {
        printf("错误: 数据验证失败\n");
        printf("期望: %s\n", test_data);
        printf("实际: %s\n", read_buffer);
        return -1;
    }
    printf("✓ 数据验证成功\n");
    
    // 8. 获取文件信息
    storage_file_info_t file_info;
    ret = storage->get_file_info(storage, test_path, &file_info);
    if (ret != XDEVICE_OK) {
        printf("错误: 获取文件信息失败 (%d)\n", ret);
        return -1;
    }
    printf("✓ 文件信息: 大小=%lu, 权限=%o\n", 
           file_info.size, file_info.permissions);
    
    // 9. 获取统计信息
    storage_stats_t stats;
    ret = storage->get_stats(storage, &stats);
    if (ret != XDEVICE_OK) {
        printf("错误: 获取统计信息失败 (%d)\n", ret);
        return -1;
    }
    printf("✓ 统计信息: 读操作=%lu, 写操作=%lu, 读字节=%lu, 写字节=%lu\n",
           stats.read_ops, stats.write_ops, stats.read_bytes, stats.write_bytes);
    
    // 10. 删除文件
    ret = storage->delete_file(storage, test_path);
    if (ret != XDEVICE_OK) {
        printf("错误: 删除文件失败 (%d)\n", ret);
        return -1;
    }
    printf("✓ 删除文件成功\n");
    
    // 11. 清理
    ret = storage->cleanup(storage);
    if (ret != XDEVICE_OK) {
        printf("错误: 存储清理失败 (%d)\n", ret);
        return -1;
    }
    printf("✓ 存储清理成功\n");
    
    printf("=== %s 测试完成 ===\n", name);
    return 0;
}

/**
 * 测试存储管理器
 */
static int test_storage_manager(void) {
    printf("\n=== 测试存储管理器 ===\n");
    
    int ret;
    storage_manager_t manager;
    
    // 1. 初始化管理器
    ret = storage_manager_init(&manager);
    if (ret != XDEVICE_OK) {
        printf("错误: 存储管理器初始化失败 (%d)\n", ret);
        return -1;
    }
    printf("✓ 存储管理器初始化成功\n");
    
    // 2. 创建并注册Mock存储
    storage_interface_t *mock_storage = create_mock_storage_interface();
    if (!mock_storage) {
        printf("错误: 创建Mock存储失败\n");
        return -1;
    }
    
    ret = mock_storage->init(mock_storage, NULL);
    if (ret != XDEVICE_OK) {
        printf("错误: Mock存储初始化失败 (%d)\n", ret);
        return -1;
    }
    
    ret = storage_manager_register_backend(&manager, mock_storage);
    if (ret != XDEVICE_OK) {
        printf("错误: 注册Mock存储失败 (%d)\n", ret);
        return -1;
    }
    printf("✓ Mock存储注册成功\n");
    
    // 3. 设置路由策略
    manager.routing_policy.primary_backend = STORAGE_BACKEND_MOCK;
    
    // 4. 通过管理器进行I/O操作
    const char *test_path = "manager_test.dat";
    const char *test_data = "Storage Manager Test Data";
    size_t test_data_len = strlen(test_data);
    
    ret = storage_manager_write(&manager, test_path, 0, test_data, test_data_len);
    if (ret != XDEVICE_OK) {
        printf("错误: 管理器写入失败 (%d)\n", ret);
        return -1;
    }
    printf("✓ 管理器写入成功\n");
    
    char read_buffer[128];
    memset(read_buffer, 0, sizeof(read_buffer));
    ret = storage_manager_read(&manager, test_path, 0, read_buffer, test_data_len);
    if (ret != XDEVICE_OK) {
        printf("错误: 管理器读取失败 (%d)\n", ret);
        return -1;
    }
    printf("✓ 管理器读取成功\n");
    
    if (memcmp(test_data, read_buffer, test_data_len) != 0) {
        printf("错误: 管理器数据验证失败\n");
        return -1;
    }
    printf("✓ 管理器数据验证成功\n");
    
    // 5. 清理
    ret = storage_manager_cleanup(&manager);
    if (ret != XDEVICE_OK) {
        printf("错误: 存储管理器清理失败 (%d)\n", ret);
        return -1;
    }
    printf("✓ 存储管理器清理成功\n");
    
    printf("=== 存储管理器测试完成 ===\n");
    return 0;
}

/**
 * 测试Raft存储适配器
 */
static int test_raft_storage_adapter(void) {
    printf("\n=== 测试Raft存储适配器 ===\n");
    
    int ret;
    storage_manager_t manager;
    raft_storage_adapter_t adapter;
    
    // 1. 初始化存储管理器
    ret = storage_manager_init(&manager);
    if (ret != XDEVICE_OK) {
        printf("错误: 存储管理器初始化失败 (%d)\n", ret);
        return -1;
    }
    
    // 2. 注册Mock存储
    storage_interface_t *mock_storage = create_mock_storage_interface();
    ret = mock_storage->init(mock_storage, NULL);
    ret = storage_manager_register_backend(&manager, mock_storage);
    manager.routing_policy.primary_backend = STORAGE_BACKEND_MOCK;
    
    // 3. 初始化Raft适配器
    ret = raft_storage_adapter_init(&adapter, &manager, "raft1_");
    if (ret != XDEVICE_OK) {
        printf("错误: Raft适配器初始化失败 (%d)\n", ret);
        return -1;
    }
    printf("✓ Raft适配器初始化成功\n");
    
    // 4. 测试日志操作
    const char *log_entry = "Log Entry #1: Test Transaction";
    size_t log_entry_len = strlen(log_entry);
    
    ret = raft_storage_append_log(&adapter, 1, log_entry, log_entry_len);
    if (ret != XDEVICE_OK) {
        printf("错误: 日志追加失败 (%d)\n", ret);
        return -1;
    }
    printf("✓ 日志追加成功\n");
    
    char log_buffer[128];
    size_t log_buffer_size = sizeof(log_buffer);
    ret = raft_storage_read_log(&adapter, 1, log_buffer, &log_buffer_size);
    if (ret != XDEVICE_OK) {
        printf("错误: 日志读取失败 (%d)\n", ret);
        return -1;
    }
    printf("✓ 日志读取成功\n");
    
    // 5. 测试状态操作
    const char *state_data = "current_term=5,voted_for=node2";
    size_t state_data_len = strlen(state_data);
    
    ret = raft_storage_save_state(&adapter, "persistent_state", state_data, state_data_len);
    if (ret != XDEVICE_OK) {
        printf("错误: 状态保存失败 (%d)\n", ret);
        return -1;
    }
    printf("✓ 状态保存成功\n");
    
    char state_buffer[128];
    size_t state_buffer_size = sizeof(state_buffer);
    ret = raft_storage_load_state(&adapter, "persistent_state", state_buffer, &state_buffer_size);
    if (ret != XDEVICE_OK) {
        printf("错误: 状态加载失败 (%d)\n", ret);
        return -1;
    }
    printf("✓ 状态加载成功\n");
    
    // 6. 测试快照操作
    const char *snapshot_data = "Snapshot at index 100: Application State Checkpoint";
    size_t snapshot_data_len = strlen(snapshot_data);
    
    ret = raft_storage_save_snapshot(&adapter, 100, snapshot_data, snapshot_data_len);
    if (ret != XDEVICE_OK) {
        printf("错误: 快照保存失败 (%d)\n", ret);
        return -1;
    }
    printf("✓ 快照保存成功\n");
    
    char snapshot_buffer[128];
    size_t snapshot_buffer_size = sizeof(snapshot_buffer);
    ret = raft_storage_load_snapshot(&adapter, 100, snapshot_buffer, &snapshot_buffer_size);
    if (ret != XDEVICE_OK) {
        printf("错误: 快照加载失败 (%d)\n", ret);
        return -1;
    }
    printf("✓ 快照加载成功\n");
    
    // 7. 清理
    ret = raft_storage_adapter_cleanup(&adapter);
    if (ret != XDEVICE_OK) {
        printf("错误: Raft适配器清理失败 (%d)\n", ret);
        return -1;
    }
    
    ret = storage_manager_cleanup(&manager);
    if (ret != XDEVICE_OK) {
        printf("错误: 存储管理器清理失败 (%d)\n", ret);
        return -1;
    }
    
    printf("✓ Raft适配器清理成功\n");
    printf("=== Raft存储适配器测试完成 ===\n");
    return 0;
}

/**
 * 性能测试
 */
static int test_performance(void) {
    printf("\n=== 性能测试 ===\n");
    
    storage_interface_t *mock_storage = create_mock_storage_interface();
    if (!mock_storage) {
        printf("错误: 创建Mock存储失败\n");
        return -1;
    }
    
    int ret = mock_storage->init(mock_storage, NULL);
    if (ret != XDEVICE_OK) {
        printf("错误: Mock存储初始化失败\n");
        return -1;
    }
    
    // 设置延迟模拟
    mock_storage_set_delays(mock_storage, 100, 200); // 100us读延迟，200us写延迟
    
    const int num_operations = 1000;
    const size_t data_size = 4096; // 4KB
    char *test_data = malloc(data_size);
    char *read_buffer = malloc(data_size);
    
    if (!test_data || !read_buffer) {
        printf("错误: 内存分配失败\n");
        return -1;
    }
    
    // 初始化测试数据
    memset(test_data, 0xAB, data_size);
    
    struct timespec start, end;
    
    // 写入性能测试
    printf("开始写入性能测试 (%d 次操作，每次 %lu 字节)...\n", num_operations, data_size);
    clock_gettime(CLOCK_REALTIME, &start);
    
    for (int i = 0; i < num_operations; i++) {
        char path[64];
        snprintf(path, sizeof(path), "perf_test_%d.dat", i);
        
        ret = mock_storage->write_sync(mock_storage, path, 0, test_data, data_size);
        if (ret != XDEVICE_OK) {
            printf("错误: 写入操作 %d 失败\n", i);
            break;
        }
    }
    
    clock_gettime(CLOCK_REALTIME, &end);
    double write_time = (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1e9;
    double write_throughput = (num_operations * data_size) / (write_time * 1024 * 1024); // MB/s
    
    printf("✓ 写入测试完成：耗时 %.3f 秒，吞吐量 %.2f MB/s\n", write_time, write_throughput);
    
    // 读取性能测试
    printf("开始读取性能测试 (%d 次操作，每次 %lu 字节)...\n", num_operations, data_size);
    clock_gettime(CLOCK_REALTIME, &start);
    
    for (int i = 0; i < num_operations; i++) {
        char path[64];
        snprintf(path, sizeof(path), "perf_test_%d.dat", i);
        
        ret = mock_storage->read_sync(mock_storage, path, 0, read_buffer, data_size);
        if (ret != XDEVICE_OK) {
            printf("错误: 读取操作 %d 失败\n", i);
            break;
        }
    }
    
    clock_gettime(CLOCK_REALTIME, &end);
    double read_time = (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1e9;
    double read_throughput = (num_operations * data_size) / (read_time * 1024 * 1024); // MB/s
    
    printf("✓ 读取测试完成：耗时 %.3f 秒，吞吐量 %.2f MB/s\n", read_time, read_throughput);
    
    // 获取统计信息
    storage_stats_t stats;
    mock_storage->get_stats(mock_storage, &stats);
    printf("✓ 统计信息：\n");
    printf("  读操作：%lu 次，%lu 字节，平均延迟 %lu ns\n",
           stats.read_ops, stats.read_bytes, stats.read_latency_avg_ns);
    printf("  写操作：%lu 次，%lu 字节，平均延迟 %lu ns\n",
           stats.write_ops, stats.write_bytes, stats.write_latency_avg_ns);
    
    // 清理
    free(test_data);
    free(read_buffer);
    mock_storage->cleanup(mock_storage);
    free(mock_storage);
    
    printf("=== 性能测试完成 ===\n");
    return 0;
}

/**
 * 主函数
 */
int main(int argc, char *argv[]) {
    printf("XDevice 存储引擎测试程序\n");
    printf("==============================\n");
    
    int ret = 0;
    
    // 1. 测试Mock存储接口
    storage_interface_t *mock_storage = create_mock_storage_interface();
    if (mock_storage) {
        if (test_storage_interface(mock_storage, "Mock存储") != 0) {
            ret = -1;
        }
        free(mock_storage);
    }
    
    // 2. 测试本地文件存储接口
    storage_interface_t *local_storage = create_local_storage_interface();
    if (local_storage) {
        if (test_storage_interface(local_storage, "本地文件存储") != 0) {
            ret = -1;
        }
        free(local_storage);
    }
    
    // 3. 测试存储管理器
    if (test_storage_manager() != 0) {
        ret = -1;
    }
    
    // 4. 测试Raft存储适配器
    if (test_raft_storage_adapter() != 0) {
        ret = -1;
    }
    
    // 5. 性能测试
    if (test_performance() != 0) {
        ret = -1;
    }
    
    printf("\n==============================\n");
    if (ret == 0) {
        printf("✓ 所有测试通过！\n");
    } else {
        printf("✗ 部分测试失败！\n");
    }
    
    return ret;
}
