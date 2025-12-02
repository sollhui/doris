# Stream Load 显式事务实现说明

## 实现概述

本次实现为 Doris 的 Stream Load 功能添加了显式事务支持，允许在一个事务中执行多个 Stream Load 操作，实现跨表的原子性保证。

## 核心改动

### 1. 修改文件
- **文件**: `fe/fe-core/src/main/java/org/apache/doris/httpv2/rest/LoadAction.java`
- **改动内容**:
  - 添加事务上下文存储 (`streamLoadTxnMap`)
  - 新增 3 个 HTTP 接口（BEGIN、COMMIT、ROLLBACK）
  - 修改现有 `streamLoad` 方法支持事务模式

### 2. 新增功能

#### 2.1 事务 BEGIN 接口
```java
@RequestMapping(path = "/api/{db}/_stream_load_txn_begin", method = RequestMethod.POST)
public Object beginStreamLoadTransaction(...)
```

**功能**:
- 创建 `TransactionEntry` 对象
- 生成或接收事务标签 (label)
- 将事务存储到全局 Map 中
- 返回事务信息

#### 2.2 事务 COMMIT 接口
```java
@RequestMapping(path = "/api/{db}/_stream_load_txn_commit", method = RequestMethod.POST)
public Object commitStreamLoadTransaction(...)
```

**功能**:
- 从 Map 中获取事务上下文
- 调用 `txnEntry.commitTransaction()` 统一提交所有子事务
- 返回提交结果（包括 txn_id 和 txn_status）
- 清理事务上下文

#### 2.3 事务 ROLLBACK 接口
```java
@RequestMapping(path = "/api/{db}/_stream_load_txn_rollback", method = RequestMethod.POST)
public Object rollbackStreamLoadTransaction(...)
```

**功能**:
- 从 Map 中获取并移除事务上下文
- 调用 `txnEntry.abortTransaction()` 回滚事务
- 返回回滚结果

#### 2.4 事务模式 Stream Load
```java
private Object executeStreamLoadInTransaction(...)
```

**功能**:
- 检查事务是否存在
- 调用 `txnEntry.beginTransaction(table, SubTransactionType.INSERT)` 创建子事务
- 执行正常的 Stream Load 流程（但不提交）
- 将提交信息保存到子事务状态

### 3. 复用的核心机制

#### 3.1 TransactionEntry
- 管理事务的完整生命周期
- 维护子事务状态列表 (`subTransactionStates`)
- 提供 `beginTransaction()` 和 `commitTransaction()` 方法

#### 3.2 SubTransactionState
- 每个 Stream Load 对应一个子事务
- 包含：子事务ID、目标表、Tablet 提交信息、操作类型

#### 3.3 GlobalTransactionMgr
- 负责全局事务的协调和提交
- 保证多表的原子性和一致性
- 处理版本发布和 EditLog 写入

## 工作流程

### 完整流程图
```
用户请求                    FE 处理                             BE 处理
════════════════════════════════════════════════════════════════════════════

1. POST /_stream_load_txn_begin
   Headers: label=my_txn
   ↓
   创建 TransactionEntry
   label = "my_txn"
   transactionId = -1
   subTransactionStates = []
   存储到 streamLoadTxnMap
   ↓
   返回: {status: OK, label: my_txn}

2. PUT /table1/_stream_load
   Headers: txn_label=my_txn
   ↓
   从 streamLoadTxnMap 获取 TransactionEntry
   ↓
   beginTransaction(table1, INSERT)
   - 首次：向 GlobalTransactionMgr 申请 txnId=12345
   - 生成 subTxnId=12345
   ↓
   执行 Stream Load                              → 数据写入 MemTable
   coordinator 返回 TabletCommitInfo             ← {tabletId, backendId}
   ↓
   addTabletCommitInfos(subTxnId, table1, ...)
   subTransactionStates.add(SubTransactionState{
     subTxnId: 12345,
     table: table1,
     tabletCommitInfos: [...]
   })
   ↓
   返回: {Status: Success} (数据不可见)

3. PUT /table2/_stream_load
   Headers: txn_label=my_txn
   ↓
   获取 TransactionEntry
   ↓
   beginTransaction(table2, INSERT)
   - 生成 subTxnId=12346
   ↓
   执行 Stream Load                              → 数据写入 MemTable
   ↓
   addTabletCommitInfos(subTxnId, table2, ...)
   subTransactionStates.add(SubTransactionState{
     subTxnId: 12346,
     table: table2,
     tabletCommitInfos: [...]
   })
   ↓
   返回: {Status: Success} (数据不可见)

4. POST /_stream_load_txn_commit
   Headers: txn_label=my_txn
   ↓
   获取 TransactionEntry
   ↓
   commitTransaction()
   ↓
   commitAndPublishTransaction(
     db, txnId=12345,
     subTransactionStates=[
       {subTxnId:12345, table:table1, ...},
       {subTxnId:12346, table:table2, ...}
     ]
   )
   ↓
   [原子操作]
   - 按 ID 排序获取所有表的写锁
   - 检查所有表的副本一致性
   - 在 DatabaseTransactionMgr 写锁下：
     * 更新事务状态 → COMMITTED
     * 写入 EditLog（单条，包含所有表）
   - 发布版本到所有 BE                         → MemTable flush to Rowset
   - 等待版本可见                              → 更新版本号，数据可见
   - 释放所有表的锁
   ↓
   从 streamLoadTxnMap 移除事务
   ↓
   返回: {status: OK, txn_id: 12345, txn_status: VISIBLE}
```

### 关键保证

#### 原子性保证
1. **表级锁**: 提交前获取所有涉及表的写锁（按 ID 排序，避免死锁）
2. **单条 EditLog**: 所有表的事务信息作为单个 TransactionState 对象序列化为一条 EditLog
3. **统一版本发布**: 所有表的数据在同一个 PublishVersion 任务中发布，同时可见

#### 一致性保证
1. **副本检查**: 提交前检查所有表的副本一致性
2. **状态转换**: 在 DatabaseTransactionMgr 的写锁保护下，原子性地更新事务状态
3. **崩溃恢复**: EditLog 重放可以恢复完整的事务状态

## 技术特点

### 1. 复用 INSERT 事务逻辑
- 完全复用 `TransactionEntry` 和 `SubTransactionState` 机制
- 使用相同的 `commitAndPublishTransaction` 流程
- 原子性保证机制一致

### 2. HTTP 友好设计
- RESTful API 风格
- 通过 Header 传递事务标签
- 返回 JSON 格式结果

### 3. 兼容性
- 不影响现有 Stream Load 功能
- 只有指定 `txn_label` 时才启用事务模式
- 支持所有 Stream Load 的格式和选项

### 4. 错误处理
- 完善的错误检查和提示
- 支持 ROLLBACK 回滚
- 事务失败自动清理

## 使用示例

### Python 客户端示例
```python
import requests
import json

class StreamLoadTransaction:
    def __init__(self, host, db, user, password):
        self.host = host
        self.db = db
        self.auth = (user, password)
        self.txn_label = None
    
    def begin(self, label=None):
        """开启事务"""
        url = f"http://{self.host}/api/{self.db}/_stream_load_txn_begin"
        headers = {}
        if label:
            headers['label'] = label
        
        response = requests.post(url, headers=headers, auth=self.auth)
        result = response.json()
        
        if result['status'] == 'OK':
            self.txn_label = result['label']
            print(f"✓ Transaction began: {self.txn_label}")
            return True
        else:
            print(f"✗ Failed to begin transaction: {result['msg']}")
            return False
    
    def load(self, table, data_file, format='csv', **options):
        """在事务中执行 Stream Load"""
        if not self.txn_label:
            raise Exception("Transaction not started")
        
        url = f"http://{self.host}/api/{self.db}/{table}/_stream_load"
        headers = {
            'txn_label': self.txn_label,
            'format': format
        }
        headers.update(options)
        
        with open(data_file, 'rb') as f:
            response = requests.put(url, headers=headers, data=f, auth=self.auth)
        
        result = response.json()
        if result.get('Status') == 'Success':
            print(f"✓ Loaded data to {table}")
            return True
        else:
            print(f"✗ Failed to load data to {table}: {result}")
            return False
    
    def commit(self):
        """提交事务"""
        if not self.txn_label:
            raise Exception("Transaction not started")
        
        url = f"http://{self.host}/api/{self.db}/_stream_load_txn_commit"
        headers = {'txn_label': self.txn_label}
        
        response = requests.post(url, headers=headers, auth=self.auth)
        result = response.json()
        
        if result['status'] == 'OK':
            print(f"✓ Transaction committed: txn_id={result['txn_id']}, "
                  f"status={result['txn_status']}")
            return True
        else:
            print(f"✗ Failed to commit transaction: {result['msg']}")
            return False
    
    def rollback(self):
        """回滚事务"""
        if not self.txn_label:
            raise Exception("Transaction not started")
        
        url = f"http://{self.host}/api/{self.db}/_stream_load_txn_rollback"
        headers = {'txn_label': self.txn_label}
        
        response = requests.post(url, headers=headers, auth=self.auth)
        result = response.json()
        
        if result['status'] == 'OK':
            print(f"✓ Transaction rolled back")
            return True
        else:
            print(f"✗ Failed to rollback transaction: {result['msg']}")
            return False

# 使用示例
txn = StreamLoadTransaction('localhost:8030', 'test_db', 'root', '')

# 开启事务
txn.begin('my_batch_load_001')

# 加载多个表的数据
txn.load('orders', 'orders.csv', format='csv', column_separator=',')
txn.load('order_items', 'order_items.csv', format='csv', column_separator=',')
txn.load('inventory', 'inventory.json', format='json')

# 提交事务
txn.commit()
```

## 测试方法

### 1. 单元测试
```bash
# 测试脚本
bash docs/test_stream_load_transaction.sh
```

### 2. 功能测试
```bash
# 1. 创建测试表
mysql -h 127.0.0.1 -P 9030 -u root << EOF
CREATE DATABASE IF NOT EXISTS test_db;
USE test_db;

CREATE TABLE users (
    id INT,
    name VARCHAR(50),
    age INT
) DUPLICATE KEY(id)
DISTRIBUTED BY HASH(id) BUCKETS 1;

CREATE TABLE projects (
    id INT,
    name VARCHAR(100),
    owner_id INT
) DUPLICATE KEY(id)
DISTRIBUTED BY HASH(id) BUCKETS 1;
EOF

# 2. 测试事务加载
TXN_LABEL="test_$(date +%s)"

# BEGIN
curl -X POST -u root: \
  -H "label: ${TXN_LABEL}" \
  http://localhost:8030/api/test_db/_stream_load_txn_begin

# LOAD 1
echo -e "1,Alice,25\n2,Bob,30" | \
curl -X PUT -u root: \
  -H "txn_label: ${TXN_LABEL}" \
  -H "format: csv" \
  -T - \
  http://localhost:8030/api/test_db/users/_stream_load

# LOAD 2
echo -e "101,ProjectA,1\n102,ProjectB,2" | \
curl -X PUT -u root: \
  -H "txn_label: ${TXN_LABEL}" \
  -H "format: csv" \
  -T - \
  http://localhost:8030/api/test_db/projects/_stream_load

# COMMIT
curl -X POST -u root: \
  -H "txn_label: ${TXN_LABEL}" \
  http://localhost:8030/api/test_db/_stream_load_txn_commit

# 验证数据
mysql -h 127.0.0.1 -P 9030 -u root -e "SELECT * FROM test_db.users; SELECT * FROM test_db.projects;"
```

### 3. 回滚测试
```bash
# BEGIN
curl -X POST -u root: \
  -H "label: rollback_test" \
  http://localhost:8030/api/test_db/_stream_load_txn_begin

# LOAD (故意加载错误数据)
echo "invalid,data,format" | \
curl -X PUT -u root: \
  -H "txn_label: rollback_test" \
  -H "format: csv" \
  -T - \
  http://localhost:8030/api/test_db/users/_stream_load

# ROLLBACK
curl -X POST -u root: \
  -H "txn_label: rollback_test" \
  http://localhost:8030/api/test_db/_stream_load_txn_rollback
```

## 性能考虑

### 内存使用
- 事务期间数据在 BE 的 MemTable 中累积
- 建议单个事务数据量控制在 GB 级别
- 过大事务可能导致 BE 内存压力

### 并发能力
- `streamLoadTxnMap` 使用 ConcurrentHashMap，支持并发访问
- 不同事务标签的操作可以并发执行
- 相同标签的操作串行化（由 label 唯一性保证）

### 锁开销
- 提交时需要获取所有涉及表的写锁
- 锁获取按 ID 排序，避免死锁
- 锁持有时间较短，主要在元数据更新阶段

## 后续优化方向

1. **事务超时机制**
   - 添加事务超时配置
   - 自动清理长时间未提交的事务

2. **事务状态查询**
   - 添加查询事务状态的接口
   - 支持列出所有活跃事务

3. **监控指标**
   - 添加事务相关的监控指标
   - 统计事务成功率、耗时等

4. **性能优化**
   - 优化大事务的内存管理
   - 支持分批提交大量数据

## 总结

本次实现成功为 Stream Load 添加了显式事务支持，核心亮点：

✅ **完全复用** INSERT 事务机制，代码复用率高
✅ **HTTP 友好**，易于集成和使用
✅ **原子性保证**，多表数据要么全部成功，要么全部失败
✅ **向后兼容**，不影响现有功能
✅ **灵活可靠**，支持 COMMIT 和 ROLLBACK

这为需要批量加载多个相关表的场景提供了强大的事务支持！


