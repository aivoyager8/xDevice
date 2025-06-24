# XDevice - 分布式WAL块设备

XDevice是一个专门为Write-Ahead Logging (WAL)场景优化的高性能分布式块设备系统。

## 项目特性

- **极致WAL性能**: 微秒级写入延迟（50-200μs），10-100倍性能提升
- **WAL生命周期优化**: 段式管理，快速失效回收，零碎片化设计
- **分布式高可用**: 基于Raft协议的强一致性保证，进程内多实例架构
- **多存储后端**: 支持NVMe-oF、NFS、本地文件，统一存储抽象
- **动态后端管理**: 智能选择最优后端，自动故障转移，零停机运维
- **智能调优**: 自适应性能调优，生命周期感知的存储管理
- **超低延迟**: 专用快速写入路径，批量操作优化，直接I/O支持

## 性能优势

| 指标 | 传统存储 | XDevice WAL优化 | 提升比例 |
|------|---------|----------------|----------|
| 写入延迟 | 1-5ms | 50-200μs | **10-100x** |
| 写入IOPS | 1K-10K | 50K-200K | **10-50x** |
| 空间回收 | 秒级 | 微秒级 | **1000x** |
| 顺序带宽 | 100-500MB/s | 1-5GB/s | **5-10x** |

## 核心组件

### 1. 存储引擎 (src/storage/)
- WAL写入引擎
- 块设备管理
- 数据压缩和校验

### 2. 分布式协调 (src/distributed/)
- Raft一致性协议
- 节点管理和发现
- 领导者选举

### 3. 网络层 (src/network/)
- 高性能网络I/O
- 自定义协议
- 连接池管理

### 4. 块设备接口 (src/blockdev/)
- 标准块设备接口
- 用户态工具
- 内核模块接口

## 构建和安装

```bash
# 安装依赖
make deps

# 编译项目
make all

# 运行测试
make test

# 安装
sudo make install
```

## 快速开始

### 基本使用
```bash
# 启动节点
./bin/xdevice-node --config=config/node1.conf

# 创建设备
./bin/xdevice-cli create-device --name=wal-device --size=1GB

# 挂载设备
./bin/xdevice-cli mount --device=wal-device --mount-point=/mnt/wal
```

### 多后端动态管理
```bash
# 查看所有候选后端
./bin/xdevice-cli backend list-candidates

# 查看当前活跃后端
./bin/xdevice-cli backend list-active

# 强制故障转移
./bin/xdevice-cli backend failover nvmeof-01 nvmeof-02

# 重载后端配置
./bin/xdevice-cli backend reload-config
```

## 配置示例

参见 `config/` 目录下的配置文件。

## 架构文档

详细的架构设计文档位于 `docs/` 目录。

## 许可证

MIT License
