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

#pragma once

#include <common/multi_version.h>

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "common/status.h"
#include "io/cache/fs_file_cache_storage.h"
#include "olap/memtable_memory_limiter.h"
#include "olap/options.h"
#include "olap/rowset/segment_v2/inverted_index_writer.h"
#include "olap/tablet_fwd.h"
#include "pipeline/pipeline_tracing.h"
#include "runtime/cluster_info.h"
#include "runtime/frontend_info.h" // TODO(zhiqiang): find a way to remove this include header
#include "util/threadpool.h"

namespace orc {
class MemoryPool;
}
namespace arrow {
class MemoryPool;
}

namespace doris {
namespace vectorized {
class VDataStreamMgr;
class ScannerScheduler;
class SpillStreamManager;
class DeltaWriterV2Pool;
class DictionaryFactory;
} // namespace vectorized
namespace pipeline {
class TaskScheduler;
struct RuntimeFilterTimerQueue;
} // namespace pipeline
class WorkloadGroupMgr;
struct WriteCooldownMetaExecutors;
namespace io {
class FileCacheFactory;
class HdfsMgr;
} // namespace io
namespace segment_v2 {
class InvertedIndexSearcherCache;
class InvertedIndexQueryCache;
class TmpFileDirs;

namespace inverted_index {
class AnalysisFactoryMgr;
}

} // namespace segment_v2

namespace kerberos {
class KerberosTicketMgr;
}

class QueryCache;
class WorkloadSchedPolicyMgr;
class BfdParser;
class BrokerMgr;
template <class T>
class BrpcClientCache;
class ExternalScanContextMgr;
class FragmentMgr;
class ResultCache;
class LoadPathMgr;
class NewLoadStreamMgr;
class MemTrackerLimiter;
class MemTracker;
struct TrackerLimiterGroup;
class BaseStorageEngine;
class ResultBufferMgr;
class ResultQueueMgr;
class RuntimeQueryStatisticsMgr;
class LoadChannelMgr;
class LoadStreamMgr;
class LoadStreamMapPool;
class StreamLoadExecutor;
class RoutineLoadTaskExecutor;
class SmallFileMgr;
class BackendServiceClient;
class TPaloBrokerServiceClient;
class PBackendService_Stub;
class PFunctionService_Stub;
template <class T>
class ClientCache;
class HeartbeatFlags;
class FrontendServiceClient;
class FileMetaCache;
class GroupCommitMgr;
class TabletSchemaCache;
class TabletColumnObjectPool;
class UserFunctionCache;
class SchemaCache;
class StoragePageCache;
class SegmentLoader;
class LookupConnectionCache;
class RowCache;
class DummyLRUCache;
class CacheManager;
class IdManager;
class ProcessProfile;
class HeapProfiler;
class WalManager;
class DNSCache;
class IndexPolicyMgr;
struct SyncRowsetStats;
class DeleteBitmapAggCache;

inline bool k_doris_exit = false;

// Execution environment for queries/plan fragments.
// Contains all required global structures, and handles to
// singleton services. Clients must call StartServices exactly
// once to properly initialise service state.
class ExecEnv {
public:
    // Empty destructor because the compiler-generated one requires full
    // declarations for classes in scoped_ptrs.
    ~ExecEnv();

    BaseStorageEngine& storage_engine() { return *_storage_engine; }

    // Initial exec environment. must call this to init all
    [[nodiscard]] static Status init(ExecEnv* env, const std::vector<StorePath>& store_paths,
                                     const std::vector<StorePath>& spill_store_paths,
                                     const std::set<std::string>& broken_paths);

    // Stop all threads and delete resources.
    void destroy();

    /// Returns the first created exec env instance. In a normal doris, this is
    /// the only instance. In test setups with multiple ExecEnv's per process,
    /// we return the most recently created instance.
    static ExecEnv* GetInstance() {
        static ExecEnv s_exec_env;
        return &s_exec_env;
    }

    // Requires ExenEnv ready
    static Result<BaseTabletSPtr> get_tablet(int64_t tablet_id,
                                             SyncRowsetStats* sync_stats = nullptr,
                                             bool force_use_cache = false);

    static bool ready() { return _s_ready.load(std::memory_order_acquire); }
    static bool tracking_memory() { return _s_tracking_memory.load(std::memory_order_acquire); }
    static bool get_is_upgrading() { return _s_upgrading.load(std::memory_order_acquire); }
    static void set_is_upgrading() { _s_upgrading = true; }
    const std::string& token() const;
    ExternalScanContextMgr* external_scan_context_mgr() { return _external_scan_context_mgr; }
    vectorized::VDataStreamMgr* vstream_mgr() { return _vstream_mgr; }
    ResultBufferMgr* result_mgr() { return _result_mgr; }
    ResultQueueMgr* result_queue_mgr() { return _result_queue_mgr; }
    ClientCache<BackendServiceClient>* client_cache() { return _backend_client_cache; }
    ClientCache<FrontendServiceClient>* frontend_client_cache() { return _frontend_client_cache; }
    ClientCache<TPaloBrokerServiceClient>* broker_client_cache() { return _broker_client_cache; }

    WorkloadGroupMgr* workload_group_mgr() { return _workload_group_manager; }
    WorkloadSchedPolicyMgr* workload_sched_policy_mgr() { return _workload_sched_mgr; }
    RuntimeQueryStatisticsMgr* runtime_query_statistics_mgr() {
        return _runtime_query_statistics_mgr;
    }

    // using template to simplify client cache management
    template <typename T>
    inline ClientCache<T>* get_client_cache() {
        return nullptr;
    }

    // Save all MemTrackerLimiters in use.
    // Each group corresponds to several MemTrackerLimiters and has a lock.
    // Multiple groups are used to reduce the impact of locks.
    std::vector<TrackerLimiterGroup> mem_tracker_limiter_pool;
    void init_mem_tracker();
    std::shared_ptr<MemTrackerLimiter> orphan_mem_tracker() { return _orphan_mem_tracker; }
    std::shared_ptr<MemTrackerLimiter> brpc_iobuf_block_memory_tracker() {
        return _brpc_iobuf_block_memory_tracker;
    }
    std::shared_ptr<MemTrackerLimiter> segcompaction_mem_tracker() {
        return _segcompaction_mem_tracker;
    }
    std::shared_ptr<MemTrackerLimiter> stream_load_pipe_tracker() {
        return _stream_load_pipe_tracker;
    }
    std::shared_ptr<MemTrackerLimiter> tablets_no_cache_mem_tracker() {
        return _tablets_no_cache_mem_tracker;
    }
    std::shared_ptr<MemTrackerLimiter> rowsets_no_cache_mem_tracker() {
        return _rowsets_no_cache_mem_tracker;
    }
    std::shared_ptr<MemTrackerLimiter> segments_no_cache_mem_tracker() {
        return _segments_no_cache_mem_tracker;
    }
    std::shared_ptr<MemTrackerLimiter> point_query_executor_mem_tracker() {
        return _point_query_executor_mem_tracker;
    }
    std::shared_ptr<MemTrackerLimiter> query_cache_mem_tracker() {
        return _query_cache_mem_tracker;
    }
    std::shared_ptr<MemTrackerLimiter> block_compression_mem_tracker() {
        return _block_compression_mem_tracker;
    }
    std::shared_ptr<MemTrackerLimiter> rowid_storage_reader_tracker() {
        return _rowid_storage_reader_tracker;
    }
    std::shared_ptr<MemTrackerLimiter> subcolumns_tree_tracker() {
        return _subcolumns_tree_tracker;
    }
    std::shared_ptr<MemTrackerLimiter> s3_file_buffer_tracker() { return _s3_file_buffer_tracker; }
    std::shared_ptr<MemTrackerLimiter> parquet_meta_tracker() { return _parquet_meta_tracker; }

    ThreadPool* send_batch_thread_pool() { return _send_batch_thread_pool.get(); }
    ThreadPool* buffered_reader_prefetch_thread_pool() {
        return _buffered_reader_prefetch_thread_pool.get();
    }
    ThreadPool* send_table_stats_thread_pool() { return _send_table_stats_thread_pool.get(); }
    ThreadPool* s3_file_upload_thread_pool() { return _s3_file_upload_thread_pool.get(); }
    ThreadPool* lazy_release_obj_pool() { return _lazy_release_obj_pool.get(); }
    ThreadPool* non_block_close_thread_pool();
    ThreadPool* s3_file_system_thread_pool() { return _s3_file_system_thread_pool.get(); }

    void init_file_cache_factory(std::vector<doris::CachePath>& cache_paths);
    io::FileCacheFactory* file_cache_factory() { return _file_cache_factory; }
    UserFunctionCache* user_function_cache() { return _user_function_cache; }
    FragmentMgr* fragment_mgr() { return _fragment_mgr; }
    ResultCache* result_cache() { return _result_cache; }
    ClusterInfo* cluster_info() { return _cluster_info; }
    LoadPathMgr* load_path_mgr() { return _load_path_mgr; }
    BfdParser* bfd_parser() const { return _bfd_parser; }
    BrokerMgr* broker_mgr() const { return _broker_mgr; }
    BrpcClientCache<PBackendService_Stub>* brpc_internal_client_cache() const {
        return _internal_client_cache;
    }
    BrpcClientCache<PBackendService_Stub>* brpc_streaming_client_cache() const {
        return _streaming_client_cache;
    }
    BrpcClientCache<PFunctionService_Stub>* brpc_function_client_cache() const {
        return _function_client_cache;
    }
    LoadChannelMgr* load_channel_mgr() { return _load_channel_mgr; }
    LoadStreamMgr* load_stream_mgr() { return _load_stream_mgr.get(); }
    NewLoadStreamMgr* new_load_stream_mgr() { return _new_load_stream_mgr.get(); }
    SmallFileMgr* small_file_mgr() { return _small_file_mgr; }
    doris::vectorized::SpillStreamManager* spill_stream_mgr() { return _spill_stream_mgr; }
    GroupCommitMgr* group_commit_mgr() { return _group_commit_mgr; }

    const std::vector<StorePath>& store_paths() const { return _store_paths; }

    StreamLoadExecutor* stream_load_executor() { return _stream_load_executor.get(); }
    RoutineLoadTaskExecutor* routine_load_task_executor() { return _routine_load_task_executor; }
    HeartbeatFlags* heartbeat_flags() { return _heartbeat_flags; }
    vectorized::ScannerScheduler* scanner_scheduler() { return _scanner_scheduler; }
    FileMetaCache* file_meta_cache() { return _file_meta_cache; }
    MemTableMemoryLimiter* memtable_memory_limiter() { return _memtable_memory_limiter.get(); }
    WalManager* wal_mgr() { return _wal_manager.get(); }
    DNSCache* dns_cache() { return _dns_cache; }
    WriteCooldownMetaExecutors* write_cooldown_meta_executors() {
        return _write_cooldown_meta_executors.get();
    }

    kerberos::KerberosTicketMgr* kerberos_ticket_mgr() { return _kerberos_ticket_mgr; }
    io::HdfsMgr* hdfs_mgr() { return _hdfs_mgr; }
    IndexPolicyMgr* index_policy_mgr() { return _index_policy_mgr; }

#ifdef BE_TEST
    void set_tmp_file_dir(std::unique_ptr<segment_v2::TmpFileDirs> tmp_file_dirs) {
        this->_tmp_file_dirs = std::move(tmp_file_dirs);
    }
    void set_ready() { this->_s_ready = true; }
    void set_not_ready() { this->_s_ready = false; }
    void set_memtable_memory_limiter(MemTableMemoryLimiter* limiter) {
        _memtable_memory_limiter.reset(limiter);
    }
    void set_cluster_info(ClusterInfo* cluster_info) { this->_cluster_info = cluster_info; }
    void set_new_load_stream_mgr(std::unique_ptr<NewLoadStreamMgr>&& new_load_stream_mgr);
    void clear_new_load_stream_mgr();
    void set_stream_load_executor(std::unique_ptr<StreamLoadExecutor>&& stream_load_executor);
    void clear_stream_load_executor();

    void set_storage_engine(std::unique_ptr<BaseStorageEngine>&& engine);
    void set_inverted_index_searcher_cache(
            segment_v2::InvertedIndexSearcherCache* inverted_index_searcher_cache);
    void set_cache_manager(CacheManager* cm) { this->_cache_manager = cm; }
    void set_process_profile(ProcessProfile* pp) { this->_process_profile = pp; }
    void set_tablet_schema_cache(TabletSchemaCache* c) { this->_tablet_schema_cache = c; }
    void set_delete_bitmap_agg_cache(DeleteBitmapAggCache* c) { _delete_bitmap_agg_cache = c; }
    void set_tablet_column_object_pool(TabletColumnObjectPool* c) {
        this->_tablet_column_object_pool = c;
    }
    void set_storage_page_cache(StoragePageCache* c) { this->_storage_page_cache = c; }
    void set_segment_loader(SegmentLoader* sl) { this->_segment_loader = sl; }
    void set_routine_load_task_executor(RoutineLoadTaskExecutor* r) {
        this->_routine_load_task_executor = r;
    }
    void set_wal_mgr(std::unique_ptr<WalManager>&& wm);
    void clear_wal_mgr();

    void set_write_cooldown_meta_executors();
    static void set_tracking_memory(bool tracking_memory) {
        _s_tracking_memory.store(tracking_memory, std::memory_order_release);
    }
    void set_orc_memory_pool(orc::MemoryPool* pool) { _orc_memory_pool = pool; }
    void set_non_block_close_thread_pool(std::unique_ptr<ThreadPool>&& pool) {
        _non_block_close_thread_pool = std::move(pool);
    }
#endif
    LoadStreamMapPool* load_stream_map_pool() { return _load_stream_map_pool.get(); }

    vectorized::DeltaWriterV2Pool* delta_writer_v2_pool() { return _delta_writer_v2_pool.get(); }

    void wait_for_all_tasks_done();

    void update_frontends(const std::vector<TFrontendInfo>& new_infos);
    std::vector<TFrontendInfo> get_frontends();
    std::map<TNetworkAddress, FrontendInfo> get_running_frontends();

    TabletSchemaCache* get_tablet_schema_cache() { return _tablet_schema_cache; }
    TabletColumnObjectPool* get_tablet_column_object_pool() { return _tablet_column_object_pool; }
    SchemaCache* schema_cache() { return _schema_cache; }
    StoragePageCache* get_storage_page_cache() { return _storage_page_cache; }
    SegmentLoader* segment_loader() { return _segment_loader; }
    LookupConnectionCache* get_lookup_connection_cache() { return _lookup_connection_cache; }
    RowCache* get_row_cache() { return _row_cache; }
    CacheManager* get_cache_manager() { return _cache_manager; }
    IdManager* get_id_manager() { return _id_manager; }
    ProcessProfile* get_process_profile() { return _process_profile; }
    HeapProfiler* get_heap_profiler() { return _heap_profiler; }
    segment_v2::InvertedIndexSearcherCache* get_inverted_index_searcher_cache() {
        return _inverted_index_searcher_cache;
    }
    segment_v2::InvertedIndexQueryCache* get_inverted_index_query_cache() {
        return _inverted_index_query_cache;
    }
    QueryCache* get_query_cache() { return _query_cache; }

    pipeline::RuntimeFilterTimerQueue* runtime_filter_timer_queue() {
        return _runtime_filter_timer_queue;
    }

    vectorized::DictionaryFactory* dict_factory() { return _dict_factory; }

    pipeline::PipelineTracerContext* pipeline_tracer_context() {
        return _pipeline_tracer_ctx.get();
    }

    segment_v2::TmpFileDirs* get_tmp_file_dirs() { return _tmp_file_dirs.get(); }
    io::FDCache* file_cache_open_fd_cache() const { return _file_cache_open_fd_cache.get(); }

    orc::MemoryPool* orc_memory_pool() { return _orc_memory_pool; }
    arrow::MemoryPool* arrow_memory_pool() { return _arrow_memory_pool; }

    bool check_auth_token(const std::string& auth_token);
    void set_stream_mgr(vectorized::VDataStreamMgr* vstream_mgr) { _vstream_mgr = vstream_mgr; }
    void clear_stream_mgr();

    DeleteBitmapAggCache* delete_bitmap_agg_cache() { return _delete_bitmap_agg_cache; }

private:
    ExecEnv();

    [[nodiscard]] Status _init(const std::vector<StorePath>& store_paths,
                               const std::vector<StorePath>& spill_store_paths,
                               const std::set<std::string>& broken_paths);
    void _destroy();

    Status _init_mem_env();
    Status _check_deploy_mode();

    Status _create_internal_workload_group();
    void _init_runtime_filter_timer_queue();

    inline static std::atomic_bool _s_ready {false};
    inline static std::atomic_bool _s_tracking_memory {false};
    std::vector<StorePath> _store_paths;
    std::vector<StorePath> _spill_store_paths;
    inline static std::atomic_bool _s_upgrading {false};

    io::FileCacheFactory* _file_cache_factory = nullptr;
    UserFunctionCache* _user_function_cache = nullptr;
    // Leave protected so that subclasses can override
    ExternalScanContextMgr* _external_scan_context_mgr = nullptr;
    vectorized::VDataStreamMgr* _vstream_mgr = nullptr;
    ResultBufferMgr* _result_mgr = nullptr;
    ResultQueueMgr* _result_queue_mgr = nullptr;
    ClientCache<BackendServiceClient>* _backend_client_cache = nullptr;
    ClientCache<FrontendServiceClient>* _frontend_client_cache = nullptr;
    ClientCache<TPaloBrokerServiceClient>* _broker_client_cache = nullptr;

    // The default tracker consumed by mem hook. If the thread does not attach other trackers,
    // by default all consumption will be passed to the process tracker through the orphan tracker.
    // In real time, `consumption of all limiter trackers` + `orphan tracker consumption` = `process tracker consumption`.
    // Ideally, all threads are expected to attach to the specified tracker, so that "all memory has its own ownership",
    // and the consumption of the orphan mem tracker is close to 0, but greater than 0.
    std::shared_ptr<MemTrackerLimiter> _orphan_mem_tracker;
    std::shared_ptr<MemTrackerLimiter> _brpc_iobuf_block_memory_tracker;
    // Count the memory consumption of segment compaction tasks.
    std::shared_ptr<MemTrackerLimiter> _segcompaction_mem_tracker;
    std::shared_ptr<MemTrackerLimiter> _stream_load_pipe_tracker;

    std::shared_ptr<MemTrackerLimiter> _tablets_no_cache_mem_tracker;
    std::shared_ptr<MemTrackerLimiter> _rowsets_no_cache_mem_tracker;
    std::shared_ptr<MemTrackerLimiter> _segments_no_cache_mem_tracker;

    // Tracking memory may be shared between multiple queries.
    std::shared_ptr<MemTrackerLimiter> _point_query_executor_mem_tracker;
    std::shared_ptr<MemTrackerLimiter> _block_compression_mem_tracker;
    std::shared_ptr<MemTrackerLimiter> _query_cache_mem_tracker;

    // TODO, looking forward to more accurate tracking.
    std::shared_ptr<MemTrackerLimiter> _rowid_storage_reader_tracker;
    std::shared_ptr<MemTrackerLimiter> _subcolumns_tree_tracker;
    std::shared_ptr<MemTrackerLimiter> _s3_file_buffer_tracker;

    // Tracking memory consumption of parquet meta
    std::shared_ptr<MemTrackerLimiter> _parquet_meta_tracker;

    std::unique_ptr<ThreadPool> _send_batch_thread_pool;
    // Threadpool used to prefetch remote file for buffered reader
    std::unique_ptr<ThreadPool> _buffered_reader_prefetch_thread_pool;
    // Threadpool used to send TableStats to FE
    std::unique_ptr<ThreadPool> _send_table_stats_thread_pool;
    // Threadpool used to upload local file to s3
    std::unique_ptr<ThreadPool> _s3_file_upload_thread_pool;
    // Pool used by join node to build hash table
    // Pool to use a new thread to release object
    std::unique_ptr<ThreadPool> _lazy_release_obj_pool;
    std::unique_ptr<ThreadPool> _non_block_close_thread_pool;
    std::unique_ptr<ThreadPool> _s3_file_system_thread_pool;

    FragmentMgr* _fragment_mgr = nullptr;
    WorkloadGroupMgr* _workload_group_manager = nullptr;

    ResultCache* _result_cache = nullptr;
    ClusterInfo* _cluster_info = nullptr;
    LoadPathMgr* _load_path_mgr = nullptr;

    BfdParser* _bfd_parser = nullptr;
    BrokerMgr* _broker_mgr = nullptr;
    LoadChannelMgr* _load_channel_mgr = nullptr;
    std::unique_ptr<LoadStreamMgr> _load_stream_mgr;
    std::unique_ptr<NewLoadStreamMgr> _new_load_stream_mgr;
    BrpcClientCache<PBackendService_Stub>* _internal_client_cache = nullptr;
    BrpcClientCache<PBackendService_Stub>* _streaming_client_cache = nullptr;
    BrpcClientCache<PFunctionService_Stub>* _function_client_cache = nullptr;

    std::unique_ptr<StreamLoadExecutor> _stream_load_executor;
    RoutineLoadTaskExecutor* _routine_load_task_executor = nullptr;
    SmallFileMgr* _small_file_mgr = nullptr;
    HeartbeatFlags* _heartbeat_flags = nullptr;
    vectorized::ScannerScheduler* _scanner_scheduler = nullptr;

    // To save meta info of external file, such as parquet footer.
    FileMetaCache* _file_meta_cache = nullptr;
    std::unique_ptr<MemTableMemoryLimiter> _memtable_memory_limiter;
    std::unique_ptr<LoadStreamMapPool> _load_stream_map_pool;
    std::unique_ptr<vectorized::DeltaWriterV2Pool> _delta_writer_v2_pool;
    std::unique_ptr<WalManager> _wal_manager;
    DNSCache* _dns_cache = nullptr;
    std::unique_ptr<WriteCooldownMetaExecutors> _write_cooldown_meta_executors;

    std::mutex _frontends_lock;
    // ip:brpc_port -> frontend_indo
    std::map<TNetworkAddress, FrontendInfo> _frontends;
    GroupCommitMgr* _group_commit_mgr = nullptr;

    // Maybe we should use unique_ptr, but it need complete type, which means we need
    // to include many headers, and for some cpp file that do not need class like TabletSchemaCache,
    // these redundancy header could introduce potential bug, at least, more header means slow compile.
    // So we choose to use raw pointer, please remember to delete these pointer in deconstructor.
    TabletSchemaCache* _tablet_schema_cache = nullptr;
    TabletColumnObjectPool* _tablet_column_object_pool = nullptr;
    std::unique_ptr<BaseStorageEngine> _storage_engine;
    SchemaCache* _schema_cache = nullptr;
    StoragePageCache* _storage_page_cache = nullptr;
    SegmentLoader* _segment_loader = nullptr;
    LookupConnectionCache* _lookup_connection_cache = nullptr;
    RowCache* _row_cache = nullptr;
    CacheManager* _cache_manager = nullptr;
    IdManager* _id_manager = nullptr;
    ProcessProfile* _process_profile = nullptr;
    HeapProfiler* _heap_profiler = nullptr;
    segment_v2::InvertedIndexSearcherCache* _inverted_index_searcher_cache = nullptr;
    segment_v2::InvertedIndexQueryCache* _inverted_index_query_cache = nullptr;
    QueryCache* _query_cache = nullptr;
    std::unique_ptr<io::FDCache> _file_cache_open_fd_cache;
    DeleteBitmapAggCache* _delete_bitmap_agg_cache {nullptr};

    pipeline::RuntimeFilterTimerQueue* _runtime_filter_timer_queue = nullptr;
    vectorized::DictionaryFactory* _dict_factory = nullptr;

    WorkloadSchedPolicyMgr* _workload_sched_mgr = nullptr;
    IndexPolicyMgr* _index_policy_mgr = nullptr;

    RuntimeQueryStatisticsMgr* _runtime_query_statistics_mgr = nullptr;

    std::unique_ptr<pipeline::PipelineTracerContext> _pipeline_tracer_ctx;
    std::unique_ptr<segment_v2::TmpFileDirs> _tmp_file_dirs;
    doris::vectorized::SpillStreamManager* _spill_stream_mgr = nullptr;

    orc::MemoryPool* _orc_memory_pool = nullptr;
    arrow::MemoryPool* _arrow_memory_pool = nullptr;

    kerberos::KerberosTicketMgr* _kerberos_ticket_mgr = nullptr;
    io::HdfsMgr* _hdfs_mgr = nullptr;
};

template <>
inline ClientCache<BackendServiceClient>* ExecEnv::get_client_cache<BackendServiceClient>() {
    return _backend_client_cache;
}
template <>
inline ClientCache<FrontendServiceClient>* ExecEnv::get_client_cache<FrontendServiceClient>() {
    return _frontend_client_cache;
}
template <>
inline ClientCache<TPaloBrokerServiceClient>*
ExecEnv::get_client_cache<TPaloBrokerServiceClient>() {
    return _broker_client_cache;
}

inline segment_v2::InvertedIndexQueryCache* GetInvertedIndexQueryCache() {
    return ExecEnv::GetInstance()->get_inverted_index_query_cache();
}

} // namespace doris
