#include "../include/xdevice.h"
#include "../include/raft.h"
#include "../include/network.h"
#include "../include/wal_engine.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <getopt.h>

static volatile bool g_running = true;
static xdevice_context_t* g_context = NULL;

// 信号处理函数
static void signal_handler(int sig) {
    printf("\nReceived signal %d, shutting down...\n", sig);
    g_running = false;
}

// 显示帮助信息
static void show_help(const char* program_name) {
    printf("Usage: %s [OPTIONS]\n\n", program_name);
    printf("Options:\n");
    printf("  -c, --config FILE    Configuration file path\n");
    printf("  -p, --port PORT      Listen port (default: 8080)\n");
    printf("  -i, --id ID          Node ID (default: node1)\n");
    printf("  -d, --data-dir DIR   Data directory (default: ./data)\n");
    printf("  -h, --help           Show this help message\n");
    printf("  -v, --version        Show version information\n");
    printf("\n");
    printf("Examples:\n");
    printf("  %s --config /etc/xdevice/node.conf\n", program_name);
    printf("  %s --port 9090 --id node2 --data-dir /var/lib/xdevice\n", program_name);
}

// 显示版本信息
static void show_version(void) {
    printf("XDevice Node Server %s\n", xdevice_get_version());
    printf("Built on %s %s\n", __DATE__, __TIME__);
    printf("Copyright (C) 2024 XDevice Project\n");
}

// 网络回调函数
static void on_connection(xdevice_network_connection_t* conn) {
    char remote_addr[64];
    uint16_t remote_port;
    
    if (xdevice_network_get_connection_info(conn, remote_addr, &remote_port) == XDEVICE_OK) {
        printf("New connection from %s:%d\n", remote_addr, remote_port);
    }
}

static void on_disconnection(xdevice_network_connection_t* conn) {
    char remote_addr[64];
    uint16_t remote_port;
    
    if (xdevice_network_get_connection_info(conn, remote_addr, &remote_port) == XDEVICE_OK) {
        printf("Connection closed from %s:%d\n", remote_addr, remote_port);
    }
}

static size_t on_data_received(xdevice_network_connection_t* conn, 
                              const void* data, size_t length) {
    // 处理接收到的数据
    // TODO: 实现协议解析
    printf("Received %zu bytes of data\n", length);
    return length; // 暂时处理所有数据
}

static void on_network_error(xdevice_network_connection_t* conn, xdevice_error_t error) {
    printf("Network error: %d\n", error);
}

// Raft回调函数
static xdevice_error_t send_vote_request(const char* node_id, 
                                         const xdevice_raft_vote_request_t* request,
                                         xdevice_raft_vote_response_t* response) {
    // TODO: 实现投票请求发送
    printf("Sending vote request to %s for term %lu\n", node_id, request->term);
    return XDEVICE_OK;
}

static xdevice_error_t send_append_entries(const char* node_id,
                                           const xdevice_raft_append_request_t* request,
                                           xdevice_raft_append_response_t* response) {
    // TODO: 实现日志追加请求发送
    printf("Sending append entries to %s for term %lu\n", node_id, request->term);
    return XDEVICE_OK;
}

static xdevice_error_t apply_log_entry(const xdevice_raft_log_entry_t* entry) {
    // TODO: 实现日志条目应用
    printf("Applying log entry %lu\n", entry->index);
    return XDEVICE_OK;
}

static void on_leader_changed(const char* leader_id) {
    printf("Leader changed to: %s\n", leader_id ? leader_id : "none");
}

static void on_state_changed(xdevice_node_state_t old_state, 
                           xdevice_node_state_t new_state) {
    const char* state_names[] = {"UNKNOWN", "LEADER", "FOLLOWER", "CANDIDATE", "DOWN"};
    printf("State changed from %s to %s\n", 
           state_names[old_state], state_names[new_state]);
}

int main(int argc, char* argv[]) {
    const char* config_file = NULL;
    uint16_t port = 8080;
    const char* node_id = "node1";
    const char* data_dir = "./data";
    
    // 解析命令行参数
    static struct option long_options[] = {
        {"config", required_argument, 0, 'c'},
        {"port", required_argument, 0, 'p'},
        {"id", required_argument, 0, 'i'},
        {"data-dir", required_argument, 0, 'd'},
        {"help", no_argument, 0, 'h'},
        {"version", no_argument, 0, 'v'},
        {0, 0, 0, 0}
    };
    
    int c;
    while ((c = getopt_long(argc, argv, "c:p:i:d:hv", long_options, NULL)) != -1) {
        switch (c) {
            case 'c':
                config_file = optarg;
                break;
            case 'p':
                port = (uint16_t)atoi(optarg);
                break;
            case 'i':
                node_id = optarg;
                break;
            case 'd':
                data_dir = optarg;
                break;
            case 'h':
                show_help(argv[0]);
                return 0;
            case 'v':
                show_version();
                return 0;
            case '?':
                fprintf(stderr, "Try '%s --help' for more information.\n", argv[0]);
                return 1;
            default:
                abort();
        }
    }
    
    // 设置信号处理
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    printf("Starting XDevice node server...\n");
    printf("Node ID: %s\n", node_id);
    printf("Port: %d\n", port);
    printf("Data directory: %s\n", data_dir);
    
    // 初始化XDevice上下文
    g_context = xdevice_init(config_file);
    if (!g_context) {
        fprintf(stderr, "Failed to initialize XDevice context\n");
        return 1;
    }
    
    // 创建网络服务器
    xdevice_network_server_t* server = xdevice_network_create_server(port);
    if (!server) {
        fprintf(stderr, "Failed to create network server\n");
        xdevice_cleanup(g_context);
        return 1;
    }
    
    // 设置网络回调
    xdevice_network_callbacks_t network_callbacks = {
        .on_connection = on_connection,
        .on_disconnection = on_disconnection,
        .on_data_received = on_data_received,
        .on_error = on_network_error
    };
    
    // 启动网络服务器
    xdevice_error_t error = xdevice_network_start_server(server, &network_callbacks);
    if (error != XDEVICE_OK) {
        fprintf(stderr, "Failed to start network server: %d\n", error);
        xdevice_network_destroy_server(server);
        xdevice_cleanup(g_context);
        return 1;
    }
    
    // 创建Raft节点
    xdevice_raft_node_t* raft_node = xdevice_raft_create_node(node_id);
    if (!raft_node) {
        fprintf(stderr, "Failed to create Raft node\n");
        xdevice_network_stop_server(server);
        xdevice_network_destroy_server(server);
        xdevice_cleanup(g_context);
        return 1;
    }
    
    // 设置Raft回调
    xdevice_raft_callbacks_t raft_callbacks = {
        .send_vote_request = send_vote_request,
        .send_append_entries = send_append_entries,
        .apply_log_entry = apply_log_entry,
        .on_leader_changed = on_leader_changed,
        .on_state_changed = on_state_changed
    };
    
    // 启动Raft节点
    error = xdevice_raft_start_node(raft_node, &raft_callbacks);
    if (error != XDEVICE_OK) {
        fprintf(stderr, "Failed to start Raft node: %d\n", error);
        xdevice_raft_destroy_node(raft_node);
        xdevice_network_stop_server(server);
        xdevice_network_destroy_server(server);
        xdevice_cleanup(g_context);
        return 1;
    }
    
    printf("XDevice node server started successfully\n");
    printf("Press Ctrl+C to stop the server\n");
    
    // 主循环
    while (g_running) {
        sleep(1);
        
        // 定期检查系统状态
        xdevice_node_state_t state = xdevice_raft_get_state(raft_node);
        if (state == XDEVICE_NODE_LEADER) {
            // 作为领导者，处理客户端请求
        }
    }
    
    printf("Shutting down server...\n");
    
    // 清理资源
    xdevice_raft_destroy_node(raft_node);
    xdevice_network_stop_server(server);
    xdevice_network_destroy_server(server);
    xdevice_cleanup(g_context);
    
    printf("Server stopped\n");
    
    return 0;
}
