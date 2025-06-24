# XDevice 架构设计文档

## 1. 项目概述

XDevice是一个专门为Write-Ahead Logging (WAL)场景优化的高性能分布式块设备系统。它提供了：

- **高性能WAL写入**: 针对顺序写入优化的存储引擎
- **分布式一致性**: 基于Raft协议的强一致性保证
- **高可用性**: 自动故障检测和快速恢复
- **可扩展性**: 支持动态集群管理

## 2. 系统架构

### 2.1 整体架构

```
┌─────────────────────────────────────────────────────────────┐
│                    Application Layer                        │
├─────────────────────────────────────────────────────────────┤
│                     XDevice API                             │
├─────────────────────────────────────────────────────────────┤
│  CLI Tool  │  Node Server  │  Block Device Interface       │
├─────────────────────────────────────────────────────────────┤
│              Distribution & Consensus Layer                 │
│  ┌─────────────────┐  ┌─────────────────┐                  │
│  │  Raft Protocol  │  │ Failure Detect  │                  │
│  └─────────────────┘  └─────────────────┘                  │
├─────────────────────────────────────────────────────────────┤
│                   Network Layer                             │
│  ┌─────────────────┐  ┌─────────────────┐                  │
│  │   TCP Server    │  │   Message Queue │                  │
│  └─────────────────┘  └─────────────────┘                  │
├─────────────────────────────────────────────────────────────┤
│                   Storage Layer                             │
│  ┌─────────────────┐  ┌─────────────────┐                  │
│  │   WAL Engine    │  │  Block Manager  │                  │
│  └─────────────────┘  └─────────────────┘                  │
├─────────────────────────────────────────────────────────────┤
│                  Operating System                           │
└─────────────────────────────────────────────────────────────┘
```

### 2.2 核心组件

#### 2.2.1 WAL引擎 (src/storage/wal_engine.c)
- **功能**: 提供高性能的Write-Ahead Logging
- **特性**:
  - 顺序写入优化
  - 批量同步机制
  - 内存映射I/O
  - CRC32校验和
  - 自动故障恢复

#### 2.2.2 Raft一致性协议 (src/distributed/raft.c)
- **功能**: 提供分布式一致性保证
- **特性**:
  - 领导者选举
  - 日志复制
  - 故障检测
  - 集群配置变更

#### 2.2.3 网络通信层 (src/network/server.c)
- **功能**: 高性能网络I/O
- **特性**:
  - epoll事件驱动
  - 非阻塞I/O
  - 连接池管理
  - 自定义协议

## 3. WAL优化设计

### 3.1 写入性能优化

1. **顺序写入**: 所有写入操作都是追加式的顺序写入
2. **批量同步**: 多个写入操作批量同步到磁盘
3. **内存映射**: 使用mmap减少系统调用开销
4. **预分配**: 预分配文件空间避免频繁扩展

### 3.2 数据安全性

1. **校验和**: 每个WAL记录都包含CRC32校验和
2. **原子写入**: 写入操作要么完全成功要么完全失败
3. **幂等恢复**: 系统重启后可以安全恢复未完成的操作

## 4. 分布式设计

### 4.1 Raft协议实现

```
Leader Election:
┌─────────────┐    vote request    ┌─────────────┐
│ Candidate   │ ──────────────────→│ Follower    │
│             │                    │             │
│             │←────────────────── │             │
└─────────────┘    vote response   └─────────────┘

Log Replication:
┌─────────────┐   append entries   ┌─────────────┐
│ Leader      │ ──────────────────→│ Follower    │
│             │                    │             │
│             │←────────────────── │             │
└─────────────┘    ack response    └─────────────┘
```

### 4.2 一致性保证

1. **强一致性**: 所有写入必须经过Raft协议同步
2. **线性一致性**: 读写操作都有全局顺序
3. **分区容错**: 只要多数节点存活，系统就能继续工作

## 5. 性能特征

### 5.1 写入性能
- **延迟**: 通常 < 1ms (单机), < 5ms (分布式)
- **吞吐量**: > 100,000 ops/sec (取决于硬件)
- **扩展性**: 线性扩展读性能，写性能受Raft同步限制

### 5.2 存储特征
- **空间效率**: 压缩和去重
- **可靠性**: 多副本+校验和
- **恢复时间**: 通常 < 10秒

## 6. 部署模式

### 6.1 单机模式
```bash
# 启动单节点
./bin/xdevice-node --config=config/standalone.conf
```

### 6.2 集群模式
```bash
# 节点1
./bin/xdevice-node --config=config/node1.conf

# 节点2  
./bin/xdevice-node --config=config/node2.conf

# 节点3
./bin/xdevice-node --config=config/node3.conf
```

### 6.3 动态扩展
```bash
# 添加新节点
./bin/xdevice-cli add-node --id=node4 --address=192.168.1.14:8080
```

## 7. 监控和运维

### 7.1 关键指标
- WAL写入延迟和吞吐量
- Raft日志同步延迟
- 集群健康状态
- 磁盘使用率

### 7.2 故障处理
- 自动故障检测
- 领导者重新选举
- 数据自动恢复
- 集群重新平衡

## 8. 使用场景

### 8.1 数据库WAL
```c
// 数据库事务日志
xdevice_device_t* wal_device = xdevice_open_device(ctx, "db-wal");
xdevice_write(wal_device, offset, transaction_log, log_size);
xdevice_sync(wal_device);
```

### 8.2 消息队列
```c
// 消息持久化
xdevice_device_t* mq_device = xdevice_open_device(ctx, "message-queue");
xdevice_write(mq_device, offset, message_data, message_size);
```

### 8.3 分布式日志
```c
// 应用日志
xdevice_device_t* log_device = xdevice_open_device(ctx, "app-log");
xdevice_write(log_device, offset, log_entry, entry_size);
```

## 9. 安全性

### 9.1 数据加密
- 传输层TLS加密
- 存储层AES加密
- 密钥管理

### 9.2 访问控制
- 节点认证
- 操作授权
- 审计日志

## 10. 未来规划

### 10.1 性能优化
- RDMA网络支持
- NVMe存储优化
- 无锁数据结构

### 10.2 功能扩展
- 多数据中心支持
- 在线压缩
- 增量备份

### 10.3 生态集成
- Kubernetes Operator
- Prometheus监控
- 标准块设备接口
