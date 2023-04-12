# 文件系统元数据管理
- C/S架构, eRPC
- 服务端元数据基于RocksDB管理
  - <pinode+fname, inode>, <inode, stat>
- 客户端hook文件系统调用
- 一致性哈希负载均衡
- 基于region监测, 数据迁移
