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
//
// The following only applies to changes made to this file as part of YugaByte development.
//
// Portions Copyright (c) YugaByte, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
// in compliance with the License.  You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied.  See the License for the specific language governing permissions and limitations
// under the License.
//
#ifndef YB_MASTER_SYS_CATALOG_H_
#define YB_MASTER_SYS_CATALOG_H_

#include <string>
#include <vector>
#include <unordered_map>

#include "yb/common/ql_protocol.pb.h"

#include "yb/consensus/consensus_fwd.h"
#include "yb/consensus/metadata.pb.h"

#include "yb/gutil/callback.h"

#include "yb/master/master_fwd.h"
#include "yb/master/sys_catalog_constants.h"

#include "yb/tablet/snapshot_coordinator.h"

#include "yb/tserver/tablet_memory_manager.h"

#include "yb/util/mem_tracker.h"
#include "yb/util/metrics_fwd.h"
#include "yb/util/pb_util.h"
#include "yb/util/status_fwd.h"

namespace yb {

class Schema;
class FsManager;
class ThreadPool;

namespace tserver {
class WriteRequestPB;
class WriteResponsePB;
}

namespace master {
class Master;
class MasterOptions;

// Forward declaration from internal header file.
class VisitorBase;
class SysCatalogWriter;

// SysCatalogTable is a YB table that keeps track of table and
// tablet metadata.
// - SysCatalogTable has only one tablet.
// - SysCatalogTable is managed by the master and not exposed to the user
//   as a "normal table", instead we have Master APIs to query the table.
class SysCatalogTable {
 public:
  typedef Callback<Status()> ElectedLeaderCallback;

  // 'leader_cb_' is invoked whenever this node is elected as a leader
  // of the consensus configuration for this tablet, including for local standalone
  // master consensus configurations. It used to initialize leader state, submit any
  // leader-specific tasks and so forth.
  //
  /// NOTE: Since 'leader_cb_' is invoked synchronously and can block
  // the consensus configuration's progress, any long running tasks (e.g., scanning
  // tablets) should be performed asynchronously (by, e.g., submitting
  // them to a to a separate threadpool).
  SysCatalogTable(Master* master, MetricRegistry* metrics, ElectedLeaderCallback leader_cb);

  ~SysCatalogTable();

  // Allow for orderly shutdown of tablet peer, etc.
  void StartShutdown();
  void CompleteShutdown();

  // Load the Metadata from disk, and initialize the TabletPeer for the sys-table
  CHECKED_STATUS Load(FsManager *fs_manager);

  // Create the new Metadata and initialize the TabletPeer for the sys-table.
  CHECKED_STATUS CreateNew(FsManager *fs_manager);

  // ==================================================================
  // Templated CRUD methods for items in sys.catalog.
  // ==================================================================
  template <class... Items>
  CHECKED_STATUS Upsert(int64_t leader_term, Items&&... items);

  template <class... Items>
  CHECKED_STATUS Delete(int64_t leader_term, Items&&... items);

  template <class... Items>
  CHECKED_STATUS Mutate(
      QLWriteRequestPB::QLStmtType op_type, int64_t leader_term, Items&&... items);

  // ==================================================================
  // Static schema related methods.
  // ==================================================================
  static std::string schema_column_type();
  static std::string schema_column_id();
  static std::string schema_column_metadata();

  ThreadPool* raft_pool() const { return raft_pool_.get(); }
  ThreadPool* tablet_prepare_pool() const { return tablet_prepare_pool_.get(); }
  ThreadPool* append_pool() const { return append_pool_.get(); }

  std::shared_ptr<tablet::TabletPeer> tablet_peer() const {
    return std::atomic_load(&tablet_peer_);
  }

  // Create a new tablet peer with information from the metadata
  void SetupTabletPeer(const scoped_refptr<tablet::RaftGroupMetadata>& metadata);

  // Update the in-memory master addresses. Report missing uuid's in the
  // config when check_missing_uuids is set to true.
  CHECKED_STATUS ConvertConfigToMasterAddresses(
      const yb::consensus::RaftConfigPB& config,
      bool check_missing_uuids = false);

  // Create consensus metadata object and flush it to disk.
  CHECKED_STATUS CreateAndFlushConsensusMeta(
      FsManager* fs_manager,
      const yb::consensus::RaftConfigPB& config,
      int64_t current_term);

  CHECKED_STATUS Visit(VisitorBase* visitor);

  // Read the ysql catalog version info from the pg_yb_catalog_version catalog table.
  CHECKED_STATUS ReadYsqlCatalogVersion(TableId ysql_catalog_table_id,
                                        uint64_t* catalog_version,
                                        uint64_t* last_breaking_version);

  // Read the pg_class catalog table. There is a separate pg_class table in each
  // YSQL database, read the information in the pg_class table for the database
  // 'database_oid' and load this information into 'table_to_tablespace_map'.
  CHECKED_STATUS ReadPgClassInfo(const uint32_t database_oid,
                                 TableToTablespaceIdMap* table_to_tablespace_map);

  CHECKED_STATUS ReadTablespaceInfoFromPgYbTablegroup(
    const uint32_t database_oid,
    TableToTablespaceIdMap *table_tablespace_map);

  // Read relnamespace OID from the pg_class catalog table.
  Result<uint32_t> ReadPgClassRelnamespace(const uint32_t database_oid,
                                           const uint32_t table_oid);

  // Read nspname string from the pg_namespace catalog table.
  Result<std::string> ReadPgNamespaceNspname(const uint32_t database_oid,
                                             const uint32_t relnamespace_oid);

  // Read the pg_tablespace catalog table and return a map with all the tablespaces and their
  // respective placement information.
  Result<std::shared_ptr<TablespaceIdToReplicationInfoMap>> ReadPgTablespaceInfo();

  // Copy the content of co-located tables in sys catalog as a batch.
  CHECKED_STATUS CopyPgsqlTables(const std::vector<TableId>& source_table_ids,
                                 const std::vector<TableId>& target_table_ids,
                                 int64_t leader_term);

  // Drop YSQL table by removing the table metadata in sys-catalog.
  CHECKED_STATUS DeleteYsqlSystemTable(const string& table_id);

  const Schema& schema();

  const scoped_refptr<MetricEntity>& GetMetricEntity() const { return metric_entity_; }

  CHECKED_STATUS FetchDdlLog(google::protobuf::RepeatedPtrField<DdlLogEntryPB>* entries);

 private:
  friend class CatalogManager;
  friend class enterprise::CatalogManager;

  inline std::unique_ptr<SysCatalogWriter> NewWriter(int64_t leader_term);

  const char *table_name() const { return kSysCatalogTableName; }

  // Return the schema of the table.
  // NOTE: This is the "server-side" schema, so it must have the column IDs.
  Schema BuildTableSchema();

  // Returns 'Status::OK()' if the WriteTranasction completed
  CHECKED_STATUS SyncWrite(SysCatalogWriter* writer);

  void SysCatalogStateChanged(const std::string& tablet_id,
                              std::shared_ptr<consensus::StateChangeContext> context);

  CHECKED_STATUS SetupTablet(const scoped_refptr<tablet::RaftGroupMetadata>& metadata);

  CHECKED_STATUS OpenTablet(const scoped_refptr<tablet::RaftGroupMetadata>& metadata);

  // Use the master options to generate a new consensus configuration.
  // In addition, resolve all UUIDs of this consensus configuration.
  CHECKED_STATUS SetupConfig(const MasterOptions& options,
                             consensus::RaftConfigPB* committed_config);

  std::string tablet_id() const;

  // Conventional "T xxx P xxxx..." prefix for logging.
  std::string LogPrefix() const;

  // Waits for the tablet to reach 'RUNNING' state.
  //
  // Contrary to tablet servers, in master we actually wait for the master tablet
  // to become online synchronously, this allows us to fail fast if something fails
  // and shouldn't induce the all-workers-blocked-waiting-for-tablets problem
  // that we've seen in tablet servers since the master only has to boot a few
  // tablets.
  CHECKED_STATUS WaitUntilRunning();

  // Shutdown the tablet peer and apply pool which are not needed in shell mode for this master.
  CHECKED_STATUS GoIntoShellMode();

  // Initializes the RaftPeerPB for the local peer.
  // Crashes due to an invariant check if the rpc server is not running.
  void InitLocalRaftPeerPB();

  // Table schema, with IDs, used for the YQL write path.
  std::unique_ptr<Schema> schema_;

  MetricRegistry* metric_registry_;

  scoped_refptr<MetricEntity> metric_entity_;

  std::unique_ptr<ThreadPool> inform_removed_master_pool_;

  // Thread pool for Raft-related operations
  std::unique_ptr<ThreadPool> raft_pool_;

  // Thread pool for preparing transactions, shared between all tablets.
  std::unique_ptr<ThreadPool> tablet_prepare_pool_;

  // Thread pool for appender tasks
  std::unique_ptr<ThreadPool> append_pool_;

  std::unique_ptr<ThreadPool> allocation_pool_;

  std::shared_ptr<tablet::TabletPeer> tablet_peer_;

  Master* master_;

  ElectedLeaderCallback leader_cb_;

  consensus::RaftPeerPB local_peer_pb_;

  scoped_refptr<Histogram> setup_config_dns_histogram_;

  scoped_refptr<Counter> peer_write_count;

  std::unordered_map<std::string, scoped_refptr<AtomicGauge<uint64>>> visitor_duration_metrics_;

  std::shared_ptr<tserver::TabletMemoryManager> mem_manager_;

  std::unique_ptr<consensus::MultiRaftManager> multi_raft_manager_;

  DISALLOW_COPY_AND_ASSIGN(SysCatalogTable);
};

} // namespace master
} // namespace yb

#include "yb/master/sys_catalog-internal.h"

#endif // YB_MASTER_SYS_CATALOG_H_
