#include "../include/xdevice.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>

// 显示帮助信息
static void show_help(const char* program_name) {
    printf("Usage: %s [COMMAND] [OPTIONS]\n\n", program_name);
    printf("Commands:\n");
    printf("  create-device      Create a new block device\n");
    printf("  delete-device      Delete a block device\n");
    printf("  list-devices       List all devices\n");
    printf("  device-info        Show device information\n");
    printf("  list-nodes         List all cluster nodes\n");
    printf("  add-node           Add a node to cluster\n");
    printf("  remove-node        Remove a node from cluster\n");
    printf("  status             Show cluster status\n");
    printf("  mount              Mount a device\n");
    printf("  unmount            Unmount a device\n");
    printf("\n");
    printf("Global Options:\n");
    printf("  -s, --server HOST  Server address (default: localhost:8080)\n");
    printf("  -h, --help         Show this help message\n");
    printf("  -v, --version      Show version information\n");
    printf("\n");
    printf("Examples:\n");
    printf("  %s create-device --name=wal-device --size=1GB\n", program_name);
    printf("  %s list-devices\n", program_name);
    printf("  %s add-node --id=node2 --address=192.168.1.2:8080\n", program_name);
}

// 显示版本信息
static void show_version(void) {
    printf("XDevice CLI %s\n", xdevice_get_version());
    printf("Built on %s %s\n", __DATE__, __TIME__);
}

// 解析大小字符串 (如 "1GB", "512MB", "2TB")
static uint64_t parse_size_string(const char* size_str) {
    if (!size_str) {
        return 0;
    }
    
    char* endptr;
    double size = strtod(size_str, &endptr);
    
    if (size <= 0) {
        return 0;
    }
    
    uint64_t multiplier = 1;
    if (*endptr) {
        if (strcasecmp(endptr, "KB") == 0 || strcasecmp(endptr, "K") == 0) {
            multiplier = 1024;
        } else if (strcasecmp(endptr, "MB") == 0 || strcasecmp(endptr, "M") == 0) {
            multiplier = 1024 * 1024;
        } else if (strcasecmp(endptr, "GB") == 0 || strcasecmp(endptr, "G") == 0) {
            multiplier = 1024 * 1024 * 1024;
        } else if (strcasecmp(endptr, "TB") == 0 || strcasecmp(endptr, "T") == 0) {
            multiplier = 1024ULL * 1024 * 1024 * 1024;
        } else {
            return 0; // 无效的单位
        }
    }
    
    return (uint64_t)(size * multiplier);
}

// 格式化大小显示
static void format_size(uint64_t bytes, char* buffer, size_t buffer_size) {
    if (bytes >= 1024ULL * 1024 * 1024 * 1024) {
        snprintf(buffer, buffer_size, "%.2f TB", (double)bytes / (1024ULL * 1024 * 1024 * 1024));
    } else if (bytes >= 1024 * 1024 * 1024) {
        snprintf(buffer, buffer_size, "%.2f GB", (double)bytes / (1024 * 1024 * 1024));
    } else if (bytes >= 1024 * 1024) {
        snprintf(buffer, buffer_size, "%.2f MB", (double)bytes / (1024 * 1024));
    } else if (bytes >= 1024) {
        snprintf(buffer, buffer_size, "%.2f KB", (double)bytes / 1024);
    } else {
        snprintf(buffer, buffer_size, "%lu bytes", bytes);
    }
}

// 创建设备命令
static int cmd_create_device(xdevice_context_t* ctx, int argc, char* argv[]) {
    const char* name = NULL;
    const char* size_str = NULL;
    
    static struct option long_options[] = {
        {"name", required_argument, 0, 'n'},
        {"size", required_argument, 0, 's'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };
    
    int c;
    optind = 0; // 重置getopt
    while ((c = getopt_long(argc, argv, "n:s:h", long_options, NULL)) != -1) {
        switch (c) {
            case 'n':
                name = optarg;
                break;
            case 's':
                size_str = optarg;
                break;
            case 'h':
                printf("Usage: create-device --name=NAME --size=SIZE\n");
                printf("  --name NAME    Device name\n");
                printf("  --size SIZE    Device size (e.g., 1GB, 512MB)\n");
                return 0;
            default:
                return 1;
        }
    }
    
    if (!name || !size_str) {
        fprintf(stderr, "Error: Both --name and --size are required\n");
        return 1;
    }
    
    uint64_t size = parse_size_string(size_str);
    if (size == 0) {
        fprintf(stderr, "Error: Invalid size format '%s'\n", size_str);
        return 1;
    }
    
    printf("Creating device '%s' with size %s...\n", name, size_str);
    
    xdevice_error_t error = xdevice_create_device(ctx, name, size);
    if (error != XDEVICE_OK) {
        fprintf(stderr, "Error: Failed to create device: %d\n", error);
        return 1;
    }
    
    printf("Device '%s' created successfully\n", name);
    return 0;
}

// 删除设备命令
static int cmd_delete_device(xdevice_context_t* ctx, int argc, char* argv[]) {
    const char* name = NULL;
    
    static struct option long_options[] = {
        {"name", required_argument, 0, 'n'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };
    
    int c;
    optind = 0;
    while ((c = getopt_long(argc, argv, "n:h", long_options, NULL)) != -1) {
        switch (c) {
            case 'n':
                name = optarg;
                break;
            case 'h':
                printf("Usage: delete-device --name=NAME\n");
                printf("  --name NAME    Device name to delete\n");
                return 0;
            default:
                return 1;
        }
    }
    
    if (!name) {
        fprintf(stderr, "Error: --name is required\n");
        return 1;
    }
    
    printf("Deleting device '%s'...\n", name);
    
    xdevice_error_t error = xdevice_delete_device(ctx, name);
    if (error != XDEVICE_OK) {
        fprintf(stderr, "Error: Failed to delete device: %d\n", error);
        return 1;
    }
    
    printf("Device '%s' deleted successfully\n", name);
    return 0;
}

// 列出设备命令
static int cmd_list_devices(xdevice_context_t* ctx, int argc, char* argv[]) {
    (void)argc;
    (void)argv;
    
    // TODO: 实现设备列表获取
    printf("Listing devices...\n");
    printf("%-20s %-10s %-10s %-15s\n", "NAME", "SIZE", "STATE", "PRIMARY NODE");
    printf("%-20s %-10s %-10s %-15s\n", "----", "----", "-----", "------------");
    
    // 示例数据
    printf("%-20s %-10s %-10s %-15s\n", "wal-device", "1.0 GB", "ACTIVE", "node1");
    printf("%-20s %-10s %-10s %-15s\n", "log-device", "512 MB", "ACTIVE", "node2");
    
    return 0;
}

// 列出节点命令
static int cmd_list_nodes(xdevice_context_t* ctx, int argc, char* argv[]) {
    (void)argc;
    (void)argv;
    
    xdevice_node_info_t nodes[XDEVICE_MAX_NODES];
    uint32_t count = XDEVICE_MAX_NODES;
    
    xdevice_error_t error = xdevice_get_nodes(ctx, nodes, &count);
    if (error != XDEVICE_OK) {
        fprintf(stderr, "Error: Failed to get node list: %d\n", error);
        return 1;
    }
    
    printf("Cluster nodes (%d total):\n", count);
    printf("%-15s %-20s %-10s %-15s\n", "NODE ID", "ADDRESS", "STATE", "LAST HEARTBEAT");
    printf("%-15s %-20s %-10s %-15s\n", "-------", "-------", "-----", "--------------");
    
    const char* state_names[] = {"UNKNOWN", "LEADER", "FOLLOWER", "CANDIDATE", "DOWN"};
    
    for (uint32_t i = 0; i < count; i++) {
        char address[256];
        snprintf(address, sizeof(address), "%s:%d", nodes[i].address, nodes[i].port);
        
        printf("%-15s %-20s %-10s %-15lu\n",
               nodes[i].node_id,
               address,
               state_names[nodes[i].state],
               nodes[i].last_heartbeat);
    }
    
    return 0;
}

// 添加节点命令
static int cmd_add_node(xdevice_context_t* ctx, int argc, char* argv[]) {
    const char* node_id = NULL;
    const char* address = NULL;
    uint16_t port = 8080;
    
    static struct option long_options[] = {
        {"id", required_argument, 0, 'i'},
        {"address", required_argument, 0, 'a'},
        {"port", required_argument, 0, 'p'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };
    
    int c;
    optind = 0;
    while ((c = getopt_long(argc, argv, "i:a:p:h", long_options, NULL)) != -1) {
        switch (c) {
            case 'i':
                node_id = optarg;
                break;
            case 'a':
                address = optarg;
                break;
            case 'p':
                port = (uint16_t)atoi(optarg);
                break;
            case 'h':
                printf("Usage: add-node --id=ID --address=ADDRESS [--port=PORT]\n");
                printf("  --id ID           Node ID\n");
                printf("  --address ADDRESS Node IP address\n");
                printf("  --port PORT       Node port (default: 8080)\n");
                return 0;
            default:
                return 1;
        }
    }
    
    if (!node_id || !address) {
        fprintf(stderr, "Error: Both --id and --address are required\n");
        return 1;
    }
    
    printf("Adding node '%s' at %s:%d to cluster...\n", node_id, address, port);
    
    xdevice_error_t error = xdevice_add_node(ctx, node_id, address, port);
    if (error != XDEVICE_OK) {
        fprintf(stderr, "Error: Failed to add node: %d\n", error);
        return 1;
    }
    
    printf("Node '%s' added successfully\n", node_id);
    return 0;
}

// 显示状态命令
static int cmd_status(xdevice_context_t* ctx, int argc, char* argv[]) {
    (void)argc;
    (void)argv;
    
    printf("XDevice Cluster Status\n");
    printf("======================\n\n");
    
    // 显示版本信息
    printf("Version: %s\n", xdevice_get_version());
    
    // 显示节点信息
    printf("\nNodes:\n");
    cmd_list_nodes(ctx, 0, NULL);
    
    // 显示设备信息
    printf("\nDevices:\n");
    cmd_list_devices(ctx, 0, NULL);
    
    return 0;
}

int main(int argc, char* argv[]) {
    const char* server_address = "localhost:8080";
    
    if (argc < 2) {
        show_help(argv[0]);
        return 1;
    }
    
    // 解析全局选项
    static struct option global_options[] = {
        {"server", required_argument, 0, 's'},
        {"help", no_argument, 0, 'h'},
        {"version", no_argument, 0, 'v'},
        {0, 0, 0, 0}
    };
    
    int c;
    while ((c = getopt_long(argc, argv, "s:hv", global_options, NULL)) != -1) {
        switch (c) {
            case 's':
                server_address = optarg;
                break;
            case 'h':
                show_help(argv[0]);
                return 0;
            case 'v':
                show_version();
                return 0;
            case '?':
                return 1;
            default:
                break;
        }
    }
    
    // 获取命令
    if (optind >= argc) {
        fprintf(stderr, "Error: No command specified\n");
        show_help(argv[0]);
        return 1;
    }
    
    const char* command = argv[optind];
    
    // 初始化XDevice上下文
    xdevice_context_t* ctx = xdevice_init(NULL);
    if (!ctx) {
        fprintf(stderr, "Error: Failed to initialize XDevice context\n");
        return 1;
    }
    
    printf("Connecting to server: %s\n", server_address);
    
    int result = 0;
    
    // 执行命令
    if (strcmp(command, "create-device") == 0) {
        result = cmd_create_device(ctx, argc - optind, argv + optind);
    } else if (strcmp(command, "delete-device") == 0) {
        result = cmd_delete_device(ctx, argc - optind, argv + optind);
    } else if (strcmp(command, "list-devices") == 0) {
        result = cmd_list_devices(ctx, argc - optind, argv + optind);
    } else if (strcmp(command, "list-nodes") == 0) {
        result = cmd_list_nodes(ctx, argc - optind, argv + optind);
    } else if (strcmp(command, "add-node") == 0) {
        result = cmd_add_node(ctx, argc - optind, argv + optind);
    } else if (strcmp(command, "status") == 0) {
        result = cmd_status(ctx, argc - optind, argv + optind);
    } else {
        fprintf(stderr, "Error: Unknown command '%s'\n", command);
        show_help(argv[0]);
        result = 1;
    }
    
    // 清理资源
    xdevice_cleanup(ctx);
    
    return result;
}
