#!/bin/bash

##############################################################################
# Stream Load 显式事务测试脚本
# 
# 功能：演示如何使用 Stream Load 的显式事务 API
# 
# 使用方法：
#   ./test_stream_load_transaction.sh
##############################################################################

set -e

# 配置参数
HOST="${DORIS_HOST:-localhost:8030}"
USER="${DORIS_USER:-root}"
PASSWORD="${DORIS_PASSWORD:-}"
DB="${DORIS_DB:-test_db}"

# 颜色输出
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# 打印函数
log_info() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

log_warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

# 生成唯一的事务标签
TXN_LABEL="stream_load_txn_test_$(date +%s)"

log_info "==================================================================="
log_info "Stream Load Transaction Test"
log_info "Host: ${HOST}"
log_info "Database: ${DB}"
log_info "Transaction Label: ${TXN_LABEL}"
log_info "==================================================================="
echo ""

# 准备测试数据
log_info "Step 0: Preparing test data..."

# 创建临时测试文件
mkdir -p /tmp/stream_load_test

# 创建 table1 测试数据
cat > /tmp/stream_load_test/data1.csv <<EOF
1,Alice,25,Engineer
2,Bob,30,Manager
3,Charlie,28,Developer
EOF

# 创建 table2 测试数据
cat > /tmp/stream_load_test/data2.csv <<EOF
101,Project A,1,2024-01-01
102,Project B,2,2024-01-02
103,Project C,3,2024-01-03
EOF

# 创建 table3 测试数据
cat > /tmp/stream_load_test/data3.json <<EOF
{"id": 1001, "product": "Laptop", "price": 1200.00}
{"id": 1002, "product": "Mouse", "price": 25.50}
{"id": 1003, "product": "Keyboard", "price": 75.00}
EOF

log_info "Test data created in /tmp/stream_load_test/"
echo ""

# Step 1: 开启事务
log_info "Step 1: Begin transaction..."
BEGIN_RESPONSE=$(curl -s -X POST \
  -u "${USER}:${PASSWORD}" \
  -H "label: ${TXN_LABEL}" \
  "http://${HOST}/api/${DB}/_stream_load_txn_begin")

echo "Response: ${BEGIN_RESPONSE}"

if echo "${BEGIN_RESPONSE}" | grep -q '"status":"OK"'; then
    log_info "✓ Transaction began successfully"
else
    log_error "✗ Failed to begin transaction"
    exit 1
fi
echo ""

# Step 2: 执行第一个 Stream Load (table1)
log_info "Step 2: Execute first stream load (users table)..."
LOAD1_RESPONSE=$(curl -s -X PUT \
  -u "${USER}:${PASSWORD}" \
  -H "txn_label: ${TXN_LABEL}" \
  -H "format: csv" \
  -H "column_separator: ," \
  -H "columns: id,name,age,role" \
  -T /tmp/stream_load_test/data1.csv \
  "http://${HOST}/api/${DB}/users/_stream_load")

echo "Response: ${LOAD1_RESPONSE}"

if echo "${LOAD1_RESPONSE}" | grep -q '"Status":"Success"'; then
    log_info "✓ First stream load completed"
else
    log_warn "⚠ First stream load may have issues (table might not exist)"
fi
echo ""

# Step 3: 执行第二个 Stream Load (table2)
log_info "Step 3: Execute second stream load (projects table)..."
LOAD2_RESPONSE=$(curl -s -X PUT \
  -u "${USER}:${PASSWORD}" \
  -H "txn_label: ${TXN_LABEL}" \
  -H "format: csv" \
  -H "column_separator: ," \
  -H "columns: project_id,project_name,owner_id,start_date" \
  -T /tmp/stream_load_test/data2.csv \
  "http://${HOST}/api/${DB}/projects/_stream_load")

echo "Response: ${LOAD2_RESPONSE}"

if echo "${LOAD2_RESPONSE}" | grep -q '"Status":"Success"'; then
    log_info "✓ Second stream load completed"
else
    log_warn "⚠ Second stream load may have issues (table might not exist)"
fi
echo ""

# Step 4: 执行第三个 Stream Load (table3)
log_info "Step 4: Execute third stream load (products table)..."
LOAD3_RESPONSE=$(curl -s -X PUT \
  -u "${USER}:${PASSWORD}" \
  -H "txn_label: ${TXN_LABEL}" \
  -H "format: json" \
  -T /tmp/stream_load_test/data3.json \
  "http://${HOST}/api/${DB}/products/_stream_load")

echo "Response: ${LOAD3_RESPONSE}"

if echo "${LOAD3_RESPONSE}" | grep -q '"Status":"Success"'; then
    log_info "✓ Third stream load completed"
else
    log_warn "⚠ Third stream load may have issues (table might not exist)"
fi
echo ""

# Step 5: 提交事务
log_info "Step 5: Commit transaction..."
COMMIT_RESPONSE=$(curl -s -X POST \
  -u "${USER}:${PASSWORD}" \
  -H "txn_label: ${TXN_LABEL}" \
  "http://${HOST}/api/${DB}/_stream_load_txn_commit")

echo "Response: ${COMMIT_RESPONSE}"

if echo "${COMMIT_RESPONSE}" | grep -q '"status":"OK"'; then
    TXN_ID=$(echo "${COMMIT_RESPONSE}" | grep -o '"txn_id":[0-9]*' | grep -o '[0-9]*')
    TXN_STATUS=$(echo "${COMMIT_RESPONSE}" | grep -o '"txn_status":"[^"]*"' | cut -d'"' -f4)
    
    log_info "✓ Transaction committed successfully"
    log_info "  Transaction ID: ${TXN_ID}"
    log_info "  Transaction Status: ${TXN_STATUS}"
else
    log_error "✗ Failed to commit transaction"
    
    log_warn "Attempting rollback..."
    ROLLBACK_RESPONSE=$(curl -s -X POST \
      -u "${USER}:${PASSWORD}" \
      -H "txn_label: ${TXN_LABEL}" \
      "http://${HOST}/api/${DB}/_stream_load_txn_rollback")
    
    echo "Rollback Response: ${ROLLBACK_RESPONSE}"
    exit 1
fi
echo ""

# 清理测试数据
log_info "Cleaning up test data..."
rm -rf /tmp/stream_load_test
log_info "✓ Test data cleaned up"
echo ""

log_info "==================================================================="
log_info "Test completed successfully!"
log_info "==================================================================="

# 打印使用说明
cat <<EOF

📝 Note: This test demonstrates the API usage. To actually load data:

1. Create the test tables first:
   
   CREATE TABLE users (
       id INT,
       name VARCHAR(50),
       age INT,
       role VARCHAR(50)
   ) DUPLICATE KEY(id)
   DISTRIBUTED BY HASH(id) BUCKETS 1;
   
   CREATE TABLE projects (
       project_id INT,
       project_name VARCHAR(100),
       owner_id INT,
       start_date DATE
   ) DUPLICATE KEY(project_id)
   DISTRIBUTED BY HASH(project_id) BUCKETS 1;
   
   CREATE TABLE products (
       id INT,
       product VARCHAR(100),
       price DECIMAL(10,2)
   ) DUPLICATE KEY(id)
   DISTRIBUTED BY HASH(id) BUCKETS 1;

2. Re-run this test script

3. Check the data:
   SELECT * FROM users;
   SELECT * FROM projects;
   SELECT * FROM products;

EOF


