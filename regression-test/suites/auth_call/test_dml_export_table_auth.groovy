// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

import org.junit.Assert;
import org.codehaus.groovy.runtime.IOGroovyMethods

suite("test_dml_export_table_auth","p0,auth_call") {

    UUID uuid = UUID.randomUUID()
    String randomValue = uuid.toString()
    int hashCode = randomValue.hashCode()
    hashCode = hashCode > 0 ? hashCode : hashCode * (-1)

    String user = 'test_dml_export_table_auth_user'
    String pwd = 'C123_567p'
    String dbName = 'test_dml_export_table_auth_db'
    String tableName = 'test_dml_export_table_auth_tb'
    String exportLabel = 'test_dml_export_table_auth_label' + hashCode.toString()

    try_sql("DROP USER ${user}")
    try_sql """drop database if exists ${dbName}"""
    sql """CREATE USER '${user}' IDENTIFIED BY '${pwd}'"""
    //cloud-mode
    if (isCloudMode()) {
        def clusters = sql " SHOW CLUSTERS; "
        assertTrue(!clusters.isEmpty())
        def validCluster = clusters[0][0]
        sql """GRANT USAGE_PRIV ON CLUSTER `${validCluster}` TO ${user}""";
    }
    sql """grant select_priv on regression_test to ${user}"""
    sql """create database ${dbName}"""

    sql """create table ${dbName}.${tableName} (
                id BIGINT,
                username VARCHAR(20)
            )
            UNIQUE KEY (id)
            DISTRIBUTED BY HASH(id) BUCKETS 2
            PROPERTIES (
                "replication_num" = "1"
            );"""
    sql """
        insert into ${dbName}.`${tableName}` values 
        (1, "111"),
        (2, "222"),
        (3, "333");
        """
    String ak = getS3AK()
    String sk = getS3SK()
    String endpoint = getS3Endpoint()
    String region = getS3Region()
    String bucket = context.config.otherConfigs.get("s3BucketName");

    connect(user, "${pwd}", context.config.jdbcUrl) {
        test {
            sql """EXPORT TABLE ${dbName}.${tableName} TO "s3://${bucket}/test_outfile/exp_${exportLabel}"
                PROPERTIES(
                    "format" = "csv",
                    "max_file_size" = "2048MB"
                )
                WITH s3 (
                "s3.endpoint" = "${endpoint}",
                "s3.region" = "${region}",
                "s3.secret_key"="${sk}",
                "s3.access_key" = "${ak}"
                );"""
            exception "denied"
        }
        try {
            sql """CANCEL EXPORT
                FROM ${dbName}
                WHERE STATE = "EXPORTING";"""
        } catch (Exception e) {
            log.info(e.getMessage())
            assertTrue(e.getMessage().indexOf("denied") == -1)
        }
    }
    sql """grant select_priv on ${dbName}.${tableName} to ${user}"""
    connect(user, "${pwd}", context.config.jdbcUrl) {
        sql """EXPORT TABLE ${dbName}.${tableName} TO "s3://${bucket}/test_outfile/exp_${exportLabel}"
                PROPERTIES(
                    "format" = "csv",
                    "max_file_size" = "2048MB"
                )
                WITH s3 (
                "s3.endpoint" = "${endpoint}",
                "s3.region" = "${region}",
                "s3.secret_key"="${sk}",
                "s3.access_key" = "${ak}"
                );"""
        sql """use ${dbName}"""
        def res = sql """show export;"""
        logger.info("res: " + res)
        assertTrue(res.size() == 1)
    }
    sql """grant select_priv on ${dbName} to ${user}"""
    connect(user, "${pwd}", context.config.jdbcUrl) {
        sql """use ${dbName}"""
        def res = sql """show export;"""
        logger.info("res: " + res)
        assertTrue(res.size() == 1)
        res = sql """show grants;"""
        logger.info("res:" + res)
        try {
            sql """CANCEL EXPORT
            FROM ${dbName}
            WHERE STATE = "EXPORTING";"""
        } catch (Exception e) {
            log.info(e.getMessage())
            // should not cause by not have auth
            assertTrue(e.getMessage().indexOf("denied") == -1)
        }

    }

    sql """drop database if exists ${dbName}"""
    try_sql("DROP USER ${user}")
}
