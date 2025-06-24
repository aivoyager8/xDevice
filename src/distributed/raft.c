#include "../include/xdevice.h"
#include "../include/raft.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <sys/time.h>

#define RAFT_HEARTBEAT_INTERVAL_MS 50
#define RAFT_ELECTION_TIMEOUT_MIN_MS 150
#define RAFT_ELECTION_TIMEOUT_MAX_MS 300

struct xdevice_raft_node {
    char node_id[XDEVICE_MAX_NODE_ID_LEN];
    xdevice_node_state_t state;
    
    // Raft状态
    uint64_t current_term;
    char voted_for[XDEVICE_MAX_NODE_ID_LEN];
    uint64_t commit_index;
    uint64_t last_applied;
    
    // 日志
    xdevice_raft_log_entry_t* log_entries;
    size_t log_capacity;
    size_t log_size;
    
    // 领导者状态
    uint64_t next_index[XDEVICE_MAX_NODES];
    uint64_t match_index[XDEVICE_MAX_NODES];
    
    // 集群配置
    xdevice_node_info_t cluster_nodes[XDEVICE_MAX_NODES];
    uint32_t cluster_size;
    
    // 定时器
    uint64_t election_timeout;
    uint64_t last_heartbeat;
    
    // 线程同步
    pthread_mutex_t mutex;
    pthread_t election_thread;
    pthread_t heartbeat_thread;
    bool running;
    
    // 回调函数
    xdevice_raft_callbacks_t callbacks;
};

// 获取当前时间（毫秒）
static uint64_t get_current_time_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

// 生成随机选举超时时间
static uint64_t generate_election_timeout(void) {
    return RAFT_ELECTION_TIMEOUT_MIN_MS + 
           (rand() % (RAFT_ELECTION_TIMEOUT_MAX_MS - RAFT_ELECTION_TIMEOUT_MIN_MS));
}

// 重置选举定时器
static void reset_election_timer(xdevice_raft_node_t* node) {
    node->election_timeout = get_current_time_ms() + generate_election_timeout();
}

// 选举线程
static void* election_thread_func(void* arg) {
    xdevice_raft_node_t* node = (xdevice_raft_node_t*)arg;
    
    while (node->running) {
        pthread_mutex_lock(&node->mutex);
        
        uint64_t current_time = get_current_time_ms();
        
        // 检查是否需要开始选举
        if (node->state != XDEVICE_NODE_LEADER && 
            current_time >= node->election_timeout) {
            
            // 开始选举
            node->state = XDEVICE_NODE_CANDIDATE;
            node->current_term++;
            strncpy(node->voted_for, node->node_id, sizeof(node->voted_for) - 1);
            reset_election_timer(node);
            
            printf("Node %s starting election for term %lu\n", 
                   node->node_id, node->current_term);
            
            // 发送投票请求给所有节点
            uint32_t votes_received = 1; // 自己的票
            
            for (uint32_t i = 0; i < node->cluster_size; i++) {
                if (strcmp(node->cluster_nodes[i].node_id, node->node_id) != 0) {
                    // 发送RequestVote RPC
                    xdevice_raft_vote_request_t vote_req;
                    vote_req.term = node->current_term;
                    strncpy(vote_req.candidate_id, node->node_id, 
                           sizeof(vote_req.candidate_id) - 1);
                    vote_req.last_log_index = node->log_size;
                    vote_req.last_log_term = node->log_size > 0 ? 
                        node->log_entries[node->log_size - 1].term : 0;
                    
                    xdevice_raft_vote_response_t vote_resp;
                    if (node->callbacks.send_vote_request && 
                        node->callbacks.send_vote_request(
                            node->cluster_nodes[i].node_id, &vote_req, &vote_resp) == XDEVICE_OK) {
                        
                        if (vote_resp.vote_granted) {
                            votes_received++;
                        }
                        
                        // 如果收到更高任期，退回到跟随者
                        if (vote_resp.term > node->current_term) {
                            node->current_term = vote_resp.term;
                            node->state = XDEVICE_NODE_FOLLOWER;
                            memset(node->voted_for, 0, sizeof(node->voted_for));
                            reset_election_timer(node);
                            break;
                        }
                    }
                }
            }
            
            // 检查是否获得多数票
            if (node->state == XDEVICE_NODE_CANDIDATE && 
                votes_received > node->cluster_size / 2) {
                
                node->state = XDEVICE_NODE_LEADER;
                printf("Node %s became leader for term %lu\n", 
                       node->node_id, node->current_term);
                
                // 初始化领导者状态
                for (uint32_t i = 0; i < node->cluster_size; i++) {
                    node->next_index[i] = node->log_size + 1;
                    node->match_index[i] = 0;
                }
                
                // 立即发送心跳
                node->last_heartbeat = 0;
            }
        }
        
        pthread_mutex_unlock(&node->mutex);
        
        usleep(10000); // 10ms
    }
    
    return NULL;
}

// 心跳线程
static void* heartbeat_thread_func(void* arg) {
    xdevice_raft_node_t* node = (xdevice_raft_node_t*)arg;
    
    while (node->running) {
        pthread_mutex_lock(&node->mutex);
        
        if (node->state == XDEVICE_NODE_LEADER) {
            uint64_t current_time = get_current_time_ms();
            
            // 检查是否需要发送心跳
            if (current_time - node->last_heartbeat >= RAFT_HEARTBEAT_INTERVAL_MS) {
                
                // 发送心跳给所有跟随者
                for (uint32_t i = 0; i < node->cluster_size; i++) {
                    if (strcmp(node->cluster_nodes[i].node_id, node->node_id) != 0) {
                        
                        xdevice_raft_append_request_t append_req;
                        append_req.term = node->current_term;
                        strncpy(append_req.leader_id, node->node_id, 
                               sizeof(append_req.leader_id) - 1);
                        append_req.prev_log_index = node->next_index[i] - 1;
                        append_req.prev_log_term = append_req.prev_log_index > 0 ? 
                            node->log_entries[append_req.prev_log_index - 1].term : 0;
                        append_req.leader_commit = node->commit_index;
                        append_req.entries_count = 0; // 心跳消息无日志条目
                        append_req.entries = NULL;
                        
                        xdevice_raft_append_response_t append_resp;
                        if (node->callbacks.send_append_entries && 
                            node->callbacks.send_append_entries(
                                node->cluster_nodes[i].node_id, &append_req, &append_resp) == XDEVICE_OK) {
                            
                            // 处理响应
                            if (append_resp.term > node->current_term) {
                                node->current_term = append_resp.term;
                                node->state = XDEVICE_NODE_FOLLOWER;
                                memset(node->voted_for, 0, sizeof(node->voted_for));
                                reset_election_timer(node);
                                break;
                            }
                            
                            if (append_resp.success) {
                                node->match_index[i] = append_req.prev_log_index;
                                node->next_index[i] = node->match_index[i] + 1;
                            } else {
                                // 日志不匹配，减少next_index重试
                                if (node->next_index[i] > 1) {
                                    node->next_index[i]--;
                                }
                            }
                        }
                    }
                }
                
                node->last_heartbeat = current_time;
            }
        }
        
        pthread_mutex_unlock(&node->mutex);
        
        usleep(RAFT_HEARTBEAT_INTERVAL_MS * 1000);
    }
    
    return NULL;
}

xdevice_raft_node_t* xdevice_raft_create_node(const char* node_id) {
    if (!node_id) {
        return NULL;
    }
    
    xdevice_raft_node_t* node = malloc(sizeof(xdevice_raft_node_t));
    if (!node) {
        return NULL;
    }
    
    memset(node, 0, sizeof(xdevice_raft_node_t));
    
    strncpy(node->node_id, node_id, sizeof(node->node_id) - 1);
    node->state = XDEVICE_NODE_FOLLOWER;
    node->current_term = 0;
    node->log_capacity = 1024;
    
    // 分配日志数组
    node->log_entries = malloc(sizeof(xdevice_raft_log_entry_t) * node->log_capacity);
    if (!node->log_entries) {
        free(node);
        return NULL;
    }
    
    // 初始化互斥锁
    if (pthread_mutex_init(&node->mutex, NULL) != 0) {
        free(node->log_entries);
        free(node);
        return NULL;
    }
    
    reset_election_timer(node);
    
    return node;
}

void xdevice_raft_destroy_node(xdevice_raft_node_t* node) {
    if (!node) {
        return;
    }
    
    // 停止线程
    node->running = false;
    if (node->election_thread) {
        pthread_join(node->election_thread, NULL);
    }
    if (node->heartbeat_thread) {
        pthread_join(node->heartbeat_thread, NULL);
    }
    
    pthread_mutex_destroy(&node->mutex);
    
    if (node->log_entries) {
        free(node->log_entries);
    }
    
    free(node);
}

xdevice_error_t xdevice_raft_start_node(xdevice_raft_node_t* node, 
                                        const xdevice_raft_callbacks_t* callbacks) {
    if (!node || !callbacks) {
        return XDEVICE_ERROR_INVALID_PARAM;
    }
    
    node->callbacks = *callbacks;
    node->running = true;
    
    // 启动选举线程
    if (pthread_create(&node->election_thread, NULL, election_thread_func, node) != 0) {
        return XDEVICE_ERROR;
    }
    
    // 启动心跳线程
    if (pthread_create(&node->heartbeat_thread, NULL, heartbeat_thread_func, node) != 0) {
        node->running = false;
        pthread_join(node->election_thread, NULL);
        return XDEVICE_ERROR;
    }
    
    return XDEVICE_OK;
}

xdevice_error_t xdevice_raft_add_node_to_cluster(xdevice_raft_node_t* node, 
                                                  const xdevice_node_info_t* new_node) {
    if (!node || !new_node || node->cluster_size >= XDEVICE_MAX_NODES) {
        return XDEVICE_ERROR_INVALID_PARAM;
    }
    
    pthread_mutex_lock(&node->mutex);
    
    node->cluster_nodes[node->cluster_size] = *new_node;
    node->cluster_size++;
    
    pthread_mutex_unlock(&node->mutex);
    
    return XDEVICE_OK;
}

xdevice_error_t xdevice_raft_append_log(xdevice_raft_node_t* node, 
                                        const void* data, 
                                        size_t length) {
    if (!node || !data || length == 0) {
        return XDEVICE_ERROR_INVALID_PARAM;
    }
    
    pthread_mutex_lock(&node->mutex);
    
    // 只有领导者可以添加日志条目
    if (node->state != XDEVICE_NODE_LEADER) {
        pthread_mutex_unlock(&node->mutex);
        return XDEVICE_ERROR_PERMISSION;
    }
    
    // 检查日志容量
    if (node->log_size >= node->log_capacity) {
        size_t new_capacity = node->log_capacity * 2;
        xdevice_raft_log_entry_t* new_entries = realloc(node->log_entries, 
            sizeof(xdevice_raft_log_entry_t) * new_capacity);
        if (!new_entries) {
            pthread_mutex_unlock(&node->mutex);
            return XDEVICE_ERROR_OUT_OF_MEMORY;
        }
        node->log_entries = new_entries;
        node->log_capacity = new_capacity;
    }
    
    // 添加日志条目
    xdevice_raft_log_entry_t* entry = &node->log_entries[node->log_size];
    entry->term = node->current_term;
    entry->index = node->log_size + 1;
    entry->length = length;
    entry->data = malloc(length);
    if (!entry->data) {
        pthread_mutex_unlock(&node->mutex);
        return XDEVICE_ERROR_OUT_OF_MEMORY;
    }
    
    memcpy(entry->data, data, length);
    node->log_size++;
    
    pthread_mutex_unlock(&node->mutex);
    
    return XDEVICE_OK;
}

xdevice_node_state_t xdevice_raft_get_state(xdevice_raft_node_t* node) {
    if (!node) {
        return XDEVICE_NODE_UNKNOWN;
    }
    
    pthread_mutex_lock(&node->mutex);
    xdevice_node_state_t state = node->state;
    pthread_mutex_unlock(&node->mutex);
    
    return state;
}
