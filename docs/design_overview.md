# xDevice - WAL分布式块设备设计文档

## 0. 关键架构决策澄清

### 0.1 部署模式选择
**决策**：采用单进程内多Raft实例架构，而非传统的多进程分布式架构

**理由**：
- **性能优势**：进程内共享内存通信延迟远低于网络通信（纳秒级 vs 微秒级）
- **简化部署**：无需复杂的网络配置和服务发现
- **资源效率**：共享内存池、线程池等系统资源
- **一致性保证**：通过共享内存和原子操作实现强一致性

**架构特点**：
```
Single Process
├── Raft Instance 1 (Leader)     ─┐
├── Raft Instance 2 (Follower)   ─┼─ 共享内存通信
├── Raft Instance 3 (Follower)   ─┘
├── WAL Engine (共享)
├── Storage Manager (共享)
└── Network Interface (可选)
```

### 0.2 存储抽象层设计
**决策**：三层存储架构 - 统一接口 → 存储管理器 → 具体后端

**层次结构**：
1. **Raft存储适配器**：面向Raft协议的语义化接口
2. **存储管理器**：统一的存储后端管理和路由
3. **存储后端**：NVMe-oF/NFS/本地文件的具体实现

### 0.3 数据组织方式
**决策**：无目录结构，采用文件前缀区分不同数据类型

**文件命名规范**：
- 日志：`{device_prefix}log_{index}`
- 状态：`{device_prefix}state_{key}`
- 快照：`{device_prefix}snap_{index}`
- 元数据：`{device_prefix}meta_{key}`

### 0.4 核心架构重新定义

**实际目标**：为应用提供分布式WAL设备，而非通用分布式存储

**关键洞察**：
- **应用视角**：应用看到一个高可用的WAL设备
- **Raft职责**：保证WAL数据的分布式一致性
- **存储管理**：每个Raft实例管理统一的虚拟地址空间

**架构调整**：
```
应用WAL请求 → Raft协调 → 统一存储后端 → 物理设备
     ↓           ↓           ↓            ↓
   追加写入    分布式复制   空间管理     顺序I/O
```

**核心原则**：
1. **WAL即存储**: Raft日志就是应用WAL的分布式副本
2. **统一地址空间**: 每个Raft实例管理连续的虚拟地址空间
3. **顺序分配**: 严格按顺序分配存储空间，优化WAL性能
4. **智能分层**: 根据数据热度选择最佳物理存储后端

**详细设计参考**: `docs/raft_storage_management.md`

## 1. 项目概述

### 1.1 项目目标
xDevice是一个专门为WAL（Write-Ahead Logging）场景优化的高性能分布式块设备系统。主要解决：
- 高频率顺序写入的性能优化
- 分布式环境下的数据一致性
- 故障恢复和高可用性
- 低延迟的读写操作

### 1.2 核心特性
- **WAL优化**：专门优化顺序写入性能，支持批量提交
- **分布式架构**：基于Raft协议的强一致性
- **高性能**：零拷贝I/O，内存映射，异步处理
- **故障恢复**：自动故障检测，快速恢复机制
- **弹性扩展**：支持动态添加/移除节点

### 1.3 应用场景
- 数据库WAL日志存储
- 消息队列持久化
- 文件系统日志
- 分布式事务日志

## 2. 系统架构

### 2.1 整体架构图
```
┌─────────────────────────────────────────────────────────┐
│                    客户端应用                           │
└─────────────────┬───────────────────────────────────────┘
                  │
┌─────────────────▼───────────────────────────────────────┐
│                块设备接口层                             │
│  ┌─────────────┐ ┌─────────────┐ ┌─────────────┐       │
│  │   读写API   │ │   管理API   │ │   监控API   │       │
│  └─────────────┘ └─────────────┘ └─────────────┘       │
└─────────────────┬───────────────────────────────────────┘
                  │
┌─────────────────▼───────────────────────────────────────┐
│                  核心引擎层                             │
│  ┌─────────────────┐ ┌─────────────────┐               │
│  │    WAL引擎      │ │   分布式协调    │               │
│  │  - 顺序写优化   │ │  - Raft协议     │               │
│  │  - 批量提交     │ │  - 领导者选举   │               │
│  │  - 压缩合并     │ │  - 日志复制     │               │
│  └─────────────────┘ └─────────────────┘               │
└─────────────────┬───────────────────────────────────────┘
                  │
┌─────────────────▼───────────────────────────────────────┐
│                 存储和网络层                            │
│  ┌─────────────────┐ ┌─────────────────┐               │
│  │    存储引擎     │ │    网络引擎     │               │
│  │  - 块管理       │ │  - 高性能I/O    │               │
│  │  - 缓存策略     │ │  - 协议栈       │               │
│  │  - 数据压缩     │ │  - 故障检测     │               │
│  └─────────────────┘ └─────────────────┘               │
└─────────────────────────────────────────────────────────┘
```

### 2.2 模块划分

#### 2.2.1 WAL引擎模块
- **职责**：负责WAL日志的高效写入和管理
- **核心功能**：
  - 顺序写入优化
  - 批量提交机制
  - 日志轮转和压缩
  - 故障恢复

#### 2.2.2 分布式协调模块
- **职责**：维护集群一致性和故障处理
- **核心功能**：
  - Raft一致性协议
  - 领导者选举
  - 日志复制
  - 成员变更

#### 2.2.3 存储引擎模块
- **职责**：底层数据存储和管理
- **核心功能**：
  - 块设备管理
  - 内存缓存
  - 数据压缩
  - 校验和验证

#### 2.2.4 网络引擎模块
- **职责**：节点间通信和客户端接口
- **核心功能**：
  - 高性能网络I/O
  - 自定义协议
  - 连接管理
  - 负载均衡

## 3. 技术栈选择

### 3.1 编程语言：C语言
- **优势**：极致性能，精确内存控制，系统级编程
- **版本**：C11标准
- **编译器**：GCC 9.0+ 或 Clang 10.0+

### 3.2 关键依赖库
- **libevent/libev**：高性能事件循环
- **zlib/lz4**：数据压缩
- **OpenSSL**：加密和安全
- **jemalloc**：内存分配器优化

### 3.3 构建系统
- **Make**：主要构建工具
- **CMake**：可选的跨平台构建
- **gcov/lcov**：代码覆盖率
- **valgrind**：内存检查

## 4. 性能目标

### 4.1 写入性能
- 顺序写入：> 1GB/s (SSD)
- 随机写入：> 100MB/s
- 批量写入延迟：< 1ms (99th percentile)

### 4.2 读取性能
- 顺序读取：> 2GB/s (SSD)
- 随机读取：> 500MB/s
- 读取延迟：< 0.1ms (99th percentile)

### 4.3 可用性目标
- 系统可用性：99.9%
- 故障检测时间：< 5s
- 故障恢复时间：< 30s

## 5. 设计文档索引

### 5.1 核心设计文档
- ✅ [数据结构设计](data_structures.md) - 核心数据结构和内存布局
- ✅ [WAL引擎设计](wal_engine_design.md) - 写前日志引擎详细设计
- ✅ [分布式协调设计](distributed_coordination.md) - Raft协议和一致性机制
- ✅ [存储引擎设计](storage_engine_design.md) - 底层存储引擎架构
- ✅ [Raft存储管理](raft_storage_management.md) - WAL生命周期导向的存储优化
- ✅ [WAL优化本地后端](wal_optimized_local_backend.md) - 专门针对WAL场景的本地存储优化
- ✅ [动态后端管理](dynamic_backend_management.md) - 多后端选择与自动故障转移

### 5.2 实现指南文档
- 📋 [API接口规范](api_specification.md) - 对外接口定义
- 📋 [性能优化策略](performance_optimization.md) - 性能调优指南
- 📋 [故障恢复机制](fault_recovery.md) - 容灾和恢复流程
- 📋 [监控和运维](monitoring_operations.md) - 系统监控和运维
- 📋 [测试策略](testing_strategy.md) - 测试方案和质量保证

### 5.3 部署运维文档
- 📋 [安装部署指南](deployment_guide.md) - 部署和配置说明
- 📋 [容量规划](capacity_planning.md) - 硬件配置和容量评估
- 📋 [安全配置](security_configuration.md) - 安全策略和配置
- 📋 [故障排除](troubleshooting.md) - 常见问题和解决方案

### 5.4 开发者文档
- 📋 [开发环境搭建](development_setup.md) - 开发环境配置
- 📋 [代码规范](coding_standards.md) - 编码规范和最佳实践
- 📋 [贡献指南](contributing.md) - 开源贡献流程
- 📋 [发布流程](release_process.md) - 版本发布和管理

> ✅ = 已完成  📋 = 待完成
