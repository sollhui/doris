# Stream Load 显式事务 API 文档

## 概述

Stream Load 现在支持显式事务功能，允许在一个事务中执行多个 Stream Load 操作，实现跨表的原子性保证。

## 核心机制

- **子事务模式**：每个 Stream Load 作为一个子事务
- **延迟提交**：数据写入 BE 但不提交，等待统一 COMMIT
- **原子性保证**：所有表的数据要么全部成功，要么全部失败
- **复用 INSERT 逻辑**：基于 `TransactionEntry` 和 `SubTransactionState` 机制

## API 接口

### 1. 开启事务 (BEGIN)

**接口**：`POST /api/{db}/_stream_load_txn_begin`

**Headers**：
- `Authorization`: Basic auth 或 token
- `label` (可选): 事务标签，不指定则自动生成

**请求示例**：
```bash
curl -X POST \
  -H "Authorization: Basic $(echo -n 'user:password' | base64)" \
  -H "label: my_txn_001" \
  http://localhost:8030/api/test_db/_stream_load_txn_begin
```

**响应示例**：
```json
{
  "status": "OK",
  "msg": "Success",
  "label": "my_txn_001",
  "txn_id": -1
}
```

### 2. 在事务中执行 Stream Load

**接口**：`PUT /api/{db}/{table}/_stream_load`

**Headers**：
- `Authorization`: Basic auth 或 token
- `txn_label`: 事务标签（必须）
- 其他 Stream Load 标准 headers（format、column_separator 等）

**请求示例**：
```bash
# 向 table1 加载数据
curl -X PUT \
  -H "Authorization: Basic $(echo -n 'user:password' | base64)" \
  -H "txn_label: my_txn_001" \
  -H "format: csv" \
  -H "column_separator: ," \
  -T data1.csv \
  http://localhost:8030/api/test_db/table1/_stream_load

# 向 table2 加载数据
curl -X PUT \
  -H "Authorization: Basic $(echo -n 'user:password' | base64)" \
  -H "txn_label: my_txn_001" \
  -H "format: csv" \
  -H "column_separator: ," \
  -T data2.csv \
  http://localhost:8030/api/test_db/table2/_stream_load

# 向 table3 加载数据
curl -X PUT \
  -H "Authorization: Basic $(echo -n 'user:password' | base64)" \
  -H "txn_label: my_txn_001" \
  -H "format: json" \
  -H "format: json" \
  -T data3.json \
  http://localhost:8030/api/test_db/table3/_stream_load
```

**响应**：与普通 Stream Load 相同，但数据暂不可见

### 3. 提交事务 (COMMIT)

**接口**：`POST /api/{db}/_stream_load_txn_commit`

**Headers**：
- `Authorization`: Basic auth 或 token
- `txn_label`: 事务标签（必须）

**请求示例**：
```bash
curl -X POST \
  -H "Authorization: Basic $(echo -n 'user:password' | base64)" \
  -H "txn_label: my_txn_001" \
  http://localhost:8030/api/test_db/_stream_load_txn_commit
```

**响应示例**：
```json
{
  "status": "OK",
  "msg": "Success",
  "label": "my_txn_001",
  "txn_id": 12345,
  "txn_status": "VISIBLE"
}
```

**txn_status 取值**：
- `VISIBLE`: 数据已提交并可见
- `COMMITTED`: 数据已提交但版本尚未发布（稍后可见）

### 4. 回滚事务 (ROLLBACK)

**接口**：`POST /api/{db}/_stream_load_txn_rollback`

**Headers**：
- `Authorization`: Basic auth 或 token
- `txn_label`: 事务标签（必须）

**请求示例**：
```bash
curl -X POST \
  -H "Authorization: Basic $(echo -n 'user:password' | base64)" \
  -H "txn_label: my_txn_001" \
  http://localhost:8030/api/test_db/_stream_load_txn_rollback
```

**响应示例**：
```json
{
  "status": "OK",
  "msg": "Success",
  "label": "my_txn_001"
}
```

## 完整使用示例

### 示例 1：多表数据加载（成功场景）

```bash
#!/bin/bash

DB="test_db"
USER="root"
PASSWORD=""
HOST="localhost:8030"
TXN_LABEL="order_txn_$(date +%s)"

# 1. 开启事务
echo "1. Begin transaction..."
curl -X POST \
  -u "${USER}:${PASSWORD}" \
  -H "label: ${TXN_LABEL}" \
  "http://${HOST}/api/${DB}/_stream_load_txn_begin"

echo -e "\n"

# 2. 加载订单数据到 orders 表
echo "2. Load orders data..."
curl -X PUT \
  -u "${USER}:${PASSWORD}" \
  -H "txn_label: ${TXN_LABEL}" \
  -H "format: csv" \
  -H "column_separator: ," \
  -T orders.csv \
  "http://${HOST}/api/${DB}/orders/_stream_load"

echo -e "\n"

# 3. 加载订单详情数据到 order_items 表
echo "3. Load order items data..."
curl -X PUT \
  -u "${USER}:${PASSWORD}" \
  -H "txn_label: ${TXN_LABEL}" \
  -H "format: csv" \
  -H "column_separator: ," \
  -T order_items.csv \
  "http://${HOST}/api/${DB}/order_items/_stream_load"

echo -e "\n"

# 4. 更新库存数据到 inventory 表
echo "4. Update inventory data..."
curl -X PUT \
  -u "${USER}:${PASSWORD}" \
  -H "txn_label: ${TXN_LABEL}" \
  -H "format: json" \
  -T inventory_updates.json \
  "http://${HOST}/api/${DB}/inventory/_stream_load"

echo -e "\n"

# 5. 提交事务
echo "5. Commit transaction..."
curl -X POST \
  -u "${USER}:${PASSWORD}" \
  -H "txn_label: ${TXN_LABEL}" \
  "http://${HOST}/api/${DB}/_stream_load_txn_commit"

echo -e "\n\nTransaction committed successfully!"
```

### 示例 2：错误处理（失败场景）

```bash
#!/bin/bash

DB="test_db"
USER="root"
PASSWORD=""
HOST="localhost:8030"
TXN_LABEL="failed_txn_$(date +%s)"

# 1. 开启事务
echo "1. Begin transaction..."
curl -X POST \
  -u "${USER}:${PASSWORD}" \
  -H "label: ${TXN_LABEL}" \
  "http://${HOST}/api/${DB}/_stream_load_txn_begin"

# 2. 加载数据
echo "2. Load data..."
response=$(curl -X PUT \
  -u "${USER}:${PASSWORD}" \
  -H "txn_label: ${TXN_LABEL}" \
  -H "format: csv" \
  -T data.csv \
  "http://${HOST}/api/${DB}/table1/_stream_load")

# 3. 检查是否有错误
if echo "$response" | grep -q "Fail"; then
    echo "Load failed! Rolling back..."
    curl -X POST \
      -u "${USER}:${PASSWORD}" \
      -H "txn_label: ${TXN_LABEL}" \
      "http://${HOST}/api/${DB}/_stream_load_txn_rollback"
    echo "Transaction rolled back"
    exit 1
else
    echo "3. Commit transaction..."
    curl -X POST \
      -u "${USER}:${PASSWORD}" \
      -H "txn_label: ${TXN_LABEL}" \
      "http://${HOST}/api/${DB}/_stream_load_txn_commit"
fi
```

## 工作原理

### 数据流程

```
┌──────────────────────────────────────────────────────────┐
│  Client                                                  │
├──────────────────────────────────────────────────────────┤
│                                                          │
│  1. POST /_stream_load_txn_begin                        │
│     ↓                                                    │
│     创建 TransactionEntry                                │
│     label = "my_txn_001"                                 │
│     transactionId = -1                                   │
│     subTransactionStates = []                            │
│                                                          │
│  2. PUT /table1/_stream_load (with txn_label)           │
│     ↓                                                    │
│     beginTransaction(table1) → subTxnId = 12345         │
│     数据写入 BE MemTable                                 │
│     addTabletCommitInfos(subTxnId, table1, ...)         │
│                                                          │
│  3. PUT /table2/_stream_load (with txn_label)           │
│     ↓                                                    │
│     beginTransaction(table2) → subTxnId = 12346         │
│     数据写入 BE MemTable                                 │
│     addTabletCommitInfos(subTxnId, table2, ...)         │
│                                                          │
│  4. POST /_stream_load_txn_commit                       │
│     ↓                                                    │
│     commitAndPublishTransaction(                         │
│       transactionId = 12345,                             │
│       subTransactionStates = [                           │
│         {subTxnId: 12345, table: table1, ...},          │
│         {subTxnId: 12346, table: table2, ...}           │
│       ]                                                  │
│     )                                                    │
│     ↓                                                    │
│     - 获取所有表的写锁                                   │
│     - 检查副本一致性                                     │
│     - 原子性更新事务状态 → COMMITTED                     │
│     - 写入单条 EditLog（包含所有表信息）                 │
│     - 发布版本到所有 BE                                  │
│     - 数据同时可见                                       │
│                                                          │
└──────────────────────────────────────────────────────────┘
```

### 与 INSERT 事务的对比

| 特性 | INSERT 事务 | Stream Load 事务 |
|------|------------|-----------------|
| **触发方式** | `BEGIN; INSERT...; COMMIT;` | HTTP API 调用 |
| **会话绑定** | 绑定到 SQL 会话 | 通过 txn_label 关联 |
| **数据来源** | SQL 语句 | HTTP 上传文件 |
| **底层机制** | TransactionEntry + SubTransactionState | **相同** |
| **原子性保证** | ✅ 相同机制 | ✅ 相同机制 |

## 注意事项

### 1. 事务生命周期
- 事务标签 (txn_label) 在 BEGIN 到 COMMIT/ROLLBACK 期间有效
- COMMIT 或 ROLLBACK 后，事务自动清理
- 建议使用唯一的 label 避免冲突

### 2. 并发限制
- 同一个 txn_label 不能被多个客户端同时使用
- 事务标签全局唯一，建议包含时间戳

### 3. 数据可见性
- Stream Load 执行后，数据写入 BE 但**不可见**
- 只有 COMMIT 成功后，数据才对所有用户可见
- ROLLBACK 后，所有数据被丢弃

### 4. 错误处理
- 任何 Stream Load 失败，建议 ROLLBACK 整个事务
- COMMIT 失败会自动触发 ROLLBACK
- 网络中断可能导致事务悬挂，需要手动 ROLLBACK

### 5. 性能考虑
- 事务中的数据在 BE 内存中累积
- 避免单个事务加载过多数据
- 建议事务时长控制在几分钟内

### 6. 兼容性
- 与现有 Stream Load 完全兼容
- 不指定 txn_label 时，行为与普通 Stream Load 一致
- 支持所有 Stream Load 的格式和选项

## 错误码

| 错误消息 | 原因 | 解决方案 |
|---------|------|---------|
| Transaction with label 'xxx' already exists | Label 已被使用 | 使用新的 label 或先 ROLLBACK |
| Transaction with label 'xxx' not found | Label 不存在 | 检查 label 或先调用 BEGIN |
| No data loaded in transaction 'xxx' | 事务中没有执行任何 Stream Load | 至少执行一次 Stream Load |
| Commit failed: ... | 提交失败 | 查看错误详情，可能需要 ROLLBACK |

## 最佳实践

1. **使用有意义的标签**
   ```bash
   TXN_LABEL="order_batch_$(date +%Y%m%d_%H%M%S)"
   ```

2. **错误处理**
   ```bash
   if ! curl ... /_stream_load; then
       curl ... /_stream_load_txn_rollback
       exit 1
   fi
   ```

3. **超时控制**
   - 设置合理的 HTTP 超时时间
   - 事务不要保持太长时间

4. **日志记录**
   - 记录每个步骤的响应
   - 保存 txn_label 用于追踪

5. **测试回滚**
   - 先在测试环境验证回滚逻辑
   - 确保数据一致性

## 监控和调试

### 查看事务状态
```sql
-- 查看运行中的事务
SHOW PROC '/transactions/{db_id}/running';

-- 查看已完成的事务
SHOW PROC '/transactions/{db_id}/finished';
```

### FE 日志
```bash
# 查看事务相关日志
grep "stream load transaction" fe.log
grep "label=my_txn_001" fe.log
```

### 调试建议
1. 检查 FE 日志中的事务操作记录
2. 确认所有 Stream Load 都使用相同的 txn_label
3. 验证数据库和表的权限
4. 检查 BE 的内存使用情况

## 总结

Stream Load 显式事务功能通过复用 INSERT 的事务机制，实现了：
- ✅ 跨表原子性保证
- ✅ HTTP API 友好
- ✅ 灵活的错误处理
- ✅ 与现有功能完全兼容

这为批量数据加载场景提供了强大的事务支持！


