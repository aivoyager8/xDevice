#ifndef RAFT_H
#define RAFT_H

#include "../xdevice.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Raft日志条目 */
typedef struct {
    uint64_t term;
    uint64_t index;
    void* data;
    size_t length;
} xdevice_raft_log_entry_t;

/* Raft投票请求 */
typedef struct {
    uint64_t term;
    char candidate_id[XDEVICE_MAX_NODE_ID_LEN];
    uint64_t last_log_index;
    uint64_t last_log_term;
} xdevice_raft_vote_request_t;

/* Raft投票响应 */
typedef struct {
    uint64_t term;
    bool vote_granted;
} xdevice_raft_vote_response_t;

/* Raft日志追加请求 */
typedef struct {
    uint64_t term;
    char leader_id[XDEVICE_MAX_NODE_ID_LEN];
    uint64_t prev_log_index;
    uint64_t prev_log_term;
    uint64_t leader_commit;
    xdevice_raft_log_entry_t* entries;
    uint32_t entries_count;
} xdevice_raft_append_request_t;

/* Raft日志追加响应 */
typedef struct {
    uint64_t term;
    bool success;
} xdevice_raft_append_response_t;

/* 前向声明 */
typedef struct xdevice_raft_node xdevice_raft_node_t;

/* Raft回调函数 */
typedef struct {
    xdevice_error_t (*send_vote_request)(const char* node_id, 
                                         const xdevice_raft_vote_request_t* request,
                                         xdevice_raft_vote_response_t* response);
    
    xdevice_error_t (*send_append_entries)(const char* node_id,
                                           const xdevice_raft_append_request_t* request,
                                           xdevice_raft_append_response_t* response);
    
    xdevice_error_t (*apply_log_entry)(const xdevice_raft_log_entry_t* entry);
    
    void (*on_leader_changed)(const char* leader_id);
    
    void (*on_state_changed)(xdevice_node_state_t old_state, 
                           xdevice_node_state_t new_state);
} xdevice_raft_callbacks_t;

/**
 * 创建Raft节点
 * @param node_id 节点ID
 * @return Raft节点实例
 */
xdevice_raft_node_t* xdevice_raft_create_node(const char* node_id);

/**
 * 销毁Raft节点
 * @param node Raft节点实例
 */
void xdevice_raft_destroy_node(xdevice_raft_node_t* node);

/**
 * 启动Raft节点
 * @param node Raft节点实例
 * @param callbacks 回调函数
 * @return 错误码
 */
xdevice_error_t xdevice_raft_start_node(xdevice_raft_node_t* node, 
                                        const xdevice_raft_callbacks_t* callbacks);

/**
 * 添加节点到集群
 * @param node Raft节点实例
 * @param new_node 新节点信息
 * @return 错误码
 */
xdevice_error_t xdevice_raft_add_node_to_cluster(xdevice_raft_node_t* node, 
                                                  const xdevice_node_info_t* new_node);

/**
 * 向Raft日志追加条目
 * @param node Raft节点实例
 * @param data 数据
 * @param length 数据长度
 * @return 错误码
 */
xdevice_error_t xdevice_raft_append_log(xdevice_raft_node_t* node, 
                                        const void* data, 
                                        size_t length);

/**
 * 处理投票请求
 * @param node Raft节点实例
 * @param request 投票请求
 * @param response 投票响应
 * @return 错误码
 */
xdevice_error_t xdevice_raft_handle_vote_request(xdevice_raft_node_t* node,
                                                 const xdevice_raft_vote_request_t* request,
                                                 xdevice_raft_vote_response_t* response);

/**
 * 处理日志追加请求
 * @param node Raft节点实例
 * @param request 日志追加请求
 * @param response 日志追加响应
 * @return 错误码
 */
xdevice_error_t xdevice_raft_handle_append_entries(xdevice_raft_node_t* node,
                                                   const xdevice_raft_append_request_t* request,
                                                   xdevice_raft_append_response_t* response);

/**
 * 获取节点当前状态
 * @param node Raft节点实例
 * @return 节点状态
 */
xdevice_node_state_t xdevice_raft_get_state(xdevice_raft_node_t* node);

/**
 * 获取当前任期
 * @param node Raft节点实例
 * @return 当前任期
 */
uint64_t xdevice_raft_get_current_term(xdevice_raft_node_t* node);

/**
 * 获取当前领导者
 * @param node Raft节点实例
 * @return 领导者ID，如果没有领导者返回NULL
 */
const char* xdevice_raft_get_leader(xdevice_raft_node_t* node);

#ifdef __cplusplus
}
#endif

#endif /* RAFT_H */
