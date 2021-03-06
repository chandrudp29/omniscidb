
/*
 * Copyright 2020 OmniSci, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "ForeignStorageMgr.h"

#include "Catalog/ForeignTable.h"
#include "CsvDataWrapper.h"
#include "ForeignTableSchema.h"
#include "ParquetDataWrapper.h"

namespace foreign_storage {
namespace {
constexpr int64_t MAX_REFRESH_TIME_IN_SECONDS = 60 * 60;

const ForeignTable* get_foreign_table(const ChunkKey& chunk_key) {
  CHECK(chunk_key.size() >= 2);
  auto db_id = chunk_key[CHUNK_KEY_DB_IDX];
  auto table_id = chunk_key[CHUNK_KEY_TABLE_IDX];
  auto catalog = Catalog_Namespace::Catalog::checkedGet(db_id);
  auto table = catalog->getMetadataForTableImpl(table_id, false);
  CHECK(table);
  auto foreign_table = dynamic_cast<const foreign_storage::ForeignTable*>(table);
  CHECK(foreign_table);
  return foreign_table;
}
}  // namespace

ForeignStorageMgr::ForeignStorageMgr(ForeignStorageCache* fsc)
    : AbstractBufferMgr(0), data_wrapper_map_({}), foreign_storage_cache_(fsc) {
  is_cache_enabled_ = (foreign_storage_cache_ != nullptr);
}

AbstractBuffer* ForeignStorageMgr::getBuffer(const ChunkKey& chunk_key,
                                             const size_t num_bytes) {
  UNREACHABLE();
  return nullptr;  // Added to avoid "no return statement" compiler warning
}

void ForeignStorageMgr::fetchBuffer(const ChunkKey& chunk_key,
                                    AbstractBuffer* destination_buffer,
                                    const size_t num_bytes) {
  CHECK(!destination_buffer->isDirty());
  bool cached = true;
  // This code is inlined from getBuffer() because we need to know if we had a cache hit
  // to know if we need to write back to the cache later.
  AbstractBuffer* buffer = is_cache_enabled_
                               ? foreign_storage_cache_->getCachedChunkIfExists(chunk_key)
                               : nullptr;
  bool is_buffer_from_map{false};
  if (!is_cache_enabled_) {
    std::shared_lock temp_chunk_buffer_map_lock(temp_chunk_buffer_map_mutex_);
    if (temp_chunk_buffer_map_.find(chunk_key) != temp_chunk_buffer_map_.end()) {
      buffer = temp_chunk_buffer_map_[chunk_key].get();
      CHECK(buffer);
      is_buffer_from_map = true;
    }
  }
  // TODO: Populate optional buffers as part of CSV performance improvement
  std::map<ChunkKey, AbstractBuffer*> optional_buffers;
  std::map<ChunkKey, AbstractBuffer*> required_buffers;
  std::vector<ChunkKey> chunk_keys;
  if (buffer == nullptr) {
    if (createDataWrapperIfNotExists(chunk_key)) {
      ChunkKey table_key{chunk_key[0], chunk_key[1]};
      // Try to recover datawrapper
      if (!recoverDataWrapperFromDisk(table_key)) {
        // If we cant recover the datawrapper from disk populate datawrapper via a scan
        ChunkMetadataVector chunk_metadata;
        getDataWrapper(table_key)->populateChunkMetadata(chunk_metadata);
      }
    }
    cached = false;
    required_buffers =
        getChunkBuffersToPopulate(chunk_key, destination_buffer, chunk_keys);
    CHECK(required_buffers.find(chunk_key) != required_buffers.end());
    getDataWrapper(chunk_key)->populateChunkBuffers(required_buffers, optional_buffers);
    buffer = required_buffers[chunk_key];
  }
  CHECK(buffer);

  // Read the contents of the source buffer into the destination buffer, if the
  // destination buffer was not directly populated by the data wrapper
  if (is_cache_enabled_ || is_buffer_from_map) {
    buffer->copyTo(destination_buffer, num_bytes);
  }

  if (is_buffer_from_map) {
    std::lock_guard temp_chunk_buffer_map_lock(temp_chunk_buffer_map_mutex_);
    temp_chunk_buffer_map_.erase(chunk_key);
  }

  // We only write back to the cache if we did not get the buffer from the cache.
  if (is_cache_enabled_ && !cached) {
    foreign_storage_cache_->cacheTableChunks(chunk_keys);
  }
}

std::map<ChunkKey, AbstractBuffer*> ForeignStorageMgr::getChunkBuffersToPopulate(
    const ChunkKey& destination_chunk_key,
    AbstractBuffer* destination_buffer,
    std::vector<ChunkKey>& chunk_keys) {
  auto db_id = destination_chunk_key[CHUNK_KEY_DB_IDX];
  auto table_id = destination_chunk_key[CHUNK_KEY_TABLE_IDX];
  auto destination_column_id = destination_chunk_key[CHUNK_KEY_COLUMN_IDX];
  auto fragment_id = destination_chunk_key[CHUNK_KEY_FRAGMENT_IDX];
  auto foreign_table = get_foreign_table(destination_chunk_key);

  ForeignTableSchema schema{db_id, foreign_table};
  auto logical_column = schema.getLogicalColumn(destination_column_id);
  auto logical_column_id = logical_column->columnId;

  for (auto column_id = logical_column_id;
       column_id <= (logical_column_id + logical_column->columnType.get_physical_cols());
       column_id++) {
    auto column = schema.getColumnDescriptor(column_id);
    if (column->columnType.is_varlen_indeed()) {
      ChunkKey data_chunk_key = {db_id, table_id, column->columnId, fragment_id, 1};
      chunk_keys.emplace_back(data_chunk_key);

      ChunkKey index_chunk_key{db_id, table_id, column->columnId, fragment_id, 2};
      chunk_keys.emplace_back(index_chunk_key);
    } else {
      ChunkKey data_chunk_key = {db_id, table_id, column->columnId, fragment_id};
      chunk_keys.emplace_back(data_chunk_key);
    }
  }

  std::map<ChunkKey, AbstractBuffer*> chunk_buffer_map;
  if (is_cache_enabled_) {
    chunk_buffer_map = foreign_storage_cache_->getChunkBuffersForCaching(chunk_keys);
  } else {
    chunk_buffer_map[destination_chunk_key] = destination_buffer;
    if (chunk_keys.size() > 1) {
      for (const auto& chunk_key : chunk_keys) {
        if (chunk_key != destination_chunk_key) {
          std::lock_guard temp_chunk_buffer_map_lock(temp_chunk_buffer_map_mutex_);
          temp_chunk_buffer_map_[chunk_key] = std::make_unique<ForeignStorageBuffer>();
          chunk_buffer_map[chunk_key] = temp_chunk_buffer_map_[chunk_key].get();
        }
      }
    } else {
      CHECK_EQ(chunk_keys.size(), static_cast<size_t>(1));
      CHECK(destination_chunk_key == chunk_keys[0]);
    }
  }
  return chunk_buffer_map;
}

void ForeignStorageMgr::getChunkMetadataVec(ChunkMetadataVector& chunk_metadata) {
  std::shared_lock data_wrapper_lock(data_wrapper_mutex_);
  for (auto& [table_chunk_key, data_wrapper] : data_wrapper_map_) {
    data_wrapper->populateChunkMetadata(chunk_metadata);
    if (is_cache_enabled_) {
      data_wrapper->serializeDataWrapperInternals(
          foreign_storage_cache_->getCacheDirectoryForTablePrefix(table_chunk_key) +
          "/wrapper_metadata.json");
    }
  }

  if (is_cache_enabled_) {
    foreign_storage_cache_->cacheMetadataVec(chunk_metadata);
  }
}

void ForeignStorageMgr::getChunkMetadataVecForKeyPrefix(
    ChunkMetadataVector& chunk_metadata,
    const ChunkKey& keyPrefix) {
  CHECK(isTableKey(keyPrefix));
  if (is_cache_enabled_ &&
      foreign_storage_cache_->hasCachedMetadataForKeyPrefix(keyPrefix)) {
    foreign_storage_cache_->getCachedMetadataVecForKeyPrefix(chunk_metadata, keyPrefix);
    return;
  }
  // If we haven't created a data wrapper yet then check to see if we can recover data.
  if (is_cache_enabled_) {
    if (data_wrapper_map_.find(keyPrefix) == data_wrapper_map_.end()) {
      if (foreign_storage_cache_->recoverCacheForTable(chunk_metadata, keyPrefix)) {
        return;
      }
    }
  }
  createDataWrapperIfNotExists(keyPrefix);
  getDataWrapper(keyPrefix)->populateChunkMetadata(chunk_metadata);
  if (is_cache_enabled_) {
    getDataWrapper(keyPrefix)->serializeDataWrapperInternals(
        foreign_storage_cache_->getCacheDirectoryForTablePrefix(keyPrefix) +
        "/wrapper_metadata.json");
  }
  if (is_cache_enabled_) {
    foreign_storage_cache_->cacheMetadataVec(chunk_metadata);
  }
}

void ForeignStorageMgr::removeTableRelatedDS(const int db_id, const int table_id) {
  const ChunkKey table_key{db_id, table_id};
  {
    std::lock_guard data_wrapper_lock(data_wrapper_mutex_);
    data_wrapper_map_.erase(table_key);
  }

  // Clear regardless of is_cache_enabled_
  if (is_cache_enabled_) {
    foreign_storage_cache_->clearForTablePrefix(table_key);
  }

  clearTempChunkBufferMapEntriesForTable(table_key);
}

MgrType ForeignStorageMgr::getMgrType() {
  return FOREIGN_STORAGE_MGR;
}

std::string ForeignStorageMgr::getStringMgrType() {
  return ToString(FOREIGN_STORAGE_MGR);
}

bool ForeignStorageMgr::hasDataWrapperForChunk(const ChunkKey& chunk_key) {
  std::shared_lock data_wrapper_lock(data_wrapper_mutex_);
  ChunkKey table_key{chunk_key[CHUNK_KEY_DB_IDX], chunk_key[CHUNK_KEY_TABLE_IDX]};
  return data_wrapper_map_.find(table_key) != data_wrapper_map_.end();
}

std::shared_ptr<ForeignDataWrapper> ForeignStorageMgr::getDataWrapper(
    const ChunkKey& chunk_key) {
  std::shared_lock data_wrapper_lock(data_wrapper_mutex_);
  ChunkKey table_key{chunk_key[CHUNK_KEY_DB_IDX], chunk_key[CHUNK_KEY_TABLE_IDX]};
  CHECK(data_wrapper_map_.find(table_key) != data_wrapper_map_.end());
  return data_wrapper_map_[table_key];
}

void ForeignStorageMgr::setDataWrapper(
    const ChunkKey& table_key,
    std::shared_ptr<MockForeignDataWrapper> data_wrapper) {
  CHECK(isTableKey(table_key));
  std::lock_guard data_wrapper_lock(data_wrapper_mutex_);
  CHECK(data_wrapper_map_.find(table_key) != data_wrapper_map_.end());
  data_wrapper->setParentWrapper(data_wrapper_map_[table_key]);
  data_wrapper_map_[table_key] = data_wrapper;
}

bool ForeignStorageMgr::createDataWrapperIfNotExists(const ChunkKey& chunk_key) {
  std::lock_guard data_wrapper_lock(data_wrapper_mutex_);
  ChunkKey table_key{chunk_key[CHUNK_KEY_DB_IDX], chunk_key[CHUNK_KEY_TABLE_IDX]};
  if (data_wrapper_map_.find(table_key) == data_wrapper_map_.end()) {
    auto db_id = chunk_key[CHUNK_KEY_DB_IDX];
    auto foreign_table = get_foreign_table(chunk_key);

    if (foreign_table->foreign_server->data_wrapper_type ==
        foreign_storage::DataWrapperType::CSV) {
      data_wrapper_map_[table_key] =
          std::make_shared<CsvDataWrapper>(db_id, foreign_table);
    } else if (foreign_table->foreign_server->data_wrapper_type ==
               foreign_storage::DataWrapperType::PARQUET) {
      data_wrapper_map_[table_key] =
          std::make_shared<ParquetDataWrapper>(db_id, foreign_table);
    } else {
      throw std::runtime_error("Unsupported data wrapper");
    }
    return true;
  }
  return false;
}

bool ForeignStorageMgr::recoverDataWrapperFromDisk(const ChunkKey& table_key) {
  bool has_cached_metadata = false;
  if (!is_cache_enabled_) {
    return false;
  }
  // Recover metadata to repopulate datawrapper
  ChunkMetadataVector chunk_metadata;
  if (foreign_storage_cache_->hasCachedMetadataForKeyPrefix(table_key)) {
    foreign_storage_cache_->getCachedMetadataVecForKeyPrefix(chunk_metadata, table_key);
    has_cached_metadata = true;
  }
  // If we dont have metadata for this table yet we need to restore it
  if (!has_cached_metadata) {
    has_cached_metadata =
        foreign_storage_cache_->recoverCacheForTable(chunk_metadata, table_key);
  }
  std::string filepath =
      foreign_storage_cache_->getCacheDirectoryForTablePrefix(table_key) +
      "/wrapper_metadata.json";
  if (boost::filesystem::exists(filepath) && has_cached_metadata) {
    // If the cache is enabled and we have a metadata file stored to disk restore it
    getDataWrapper(table_key)->restoreDataWrapperInternals(filepath, chunk_metadata);
    return true;
  } else {
    return false;
  }
}

ForeignStorageCache* ForeignStorageMgr::getForeignStorageCache() const {
  return foreign_storage_cache_;
}

void ForeignStorageMgr::refreshTable(const ChunkKey& table_key,
                                     const bool evict_cached_entries) {
  clearTempChunkBufferMapEntriesForTable(table_key);
  if (evict_cached_entries) {
    evictTableFromCache(table_key);
  } else {
    refreshTableInCache(table_key);
  }
}

void ForeignStorageMgr::refreshTableInCache(const ChunkKey& table_key) {
  if (!is_cache_enabled_) {
    return;
  }

  CHECK(isTableKey(table_key));
  bool append_mode = get_foreign_table(table_key)->isAppendMode();

  // create datawrapper if it does not exist prior to clearing metadata
  if (createDataWrapperIfNotExists(table_key) && append_mode) {
    // Restore last state if we are appending
    recoverDataWrapperFromDisk(table_key);
  }
  // Get a list of which chunks were cached for a table.
  std::vector<ChunkKey> old_chunk_keys =
      foreign_storage_cache_->getCachedChunksForKeyPrefix(table_key);

  // Refresh metadata.
  ChunkMetadataVector metadata_vec;
  getDataWrapper(table_key)->populateChunkMetadata(metadata_vec);
  getDataWrapper(table_key)->serializeDataWrapperInternals(
      foreign_storage_cache_->getCacheDirectoryForTablePrefix(table_key) +
      "/wrapper_metadata.json");

  int last_frag_id = 0;
  if (append_mode) {
    // Determine last fragment ID
    if (foreign_storage_cache_->hasCachedMetadataForKeyPrefix(table_key)) {
      ChunkMetadataVector cached_metadata_vec;
      foreign_storage_cache_->getCachedMetadataVecForKeyPrefix(cached_metadata_vec,
                                                               table_key);
      for (const auto& metadata : cached_metadata_vec) {
        last_frag_id = std::max(last_frag_id, metadata.first[CHUNK_KEY_FRAGMENT_IDX]);
      }
    }
  } else {
    // Clear entire table
    foreign_storage_cache_->clearForTablePrefix(table_key);
  }

  try {
    if (append_mode) {
      // Only re-cache last fragment and above
      ChunkMetadataVector new_metadata_vec;
      for (const auto& chunk_metadata : metadata_vec) {
        if (chunk_metadata.first[CHUNK_KEY_FRAGMENT_IDX] >= last_frag_id) {
          new_metadata_vec.push_back(chunk_metadata);
        }
      }
      foreign_storage_cache_->cacheMetadataVec(new_metadata_vec);
    } else {
      foreign_storage_cache_->cacheMetadataVec(metadata_vec);
    }
    // Iterate through previously cached chunks and re-cache them. Caching is
    // done one fragment at a time, for all applicable chunks in the fragment.
    std::map<ChunkKey, AbstractBuffer*> optional_buffers;
    std::vector<ChunkKey> chunk_keys_to_be_cached;
    int64_t total_time{0};
    auto fragment_refresh_start_time = std::chrono::high_resolution_clock::now();
    if (!old_chunk_keys.empty()) {
      auto fragment_id = old_chunk_keys[0][CHUNK_KEY_FRAGMENT_IDX];
      std::vector<ChunkKey> chunk_keys_in_fragment;
      for (const auto& chunk_key : old_chunk_keys) {
        if (append_mode && (chunk_key[CHUNK_KEY_FRAGMENT_IDX] < last_frag_id)) {
          continue;
        }
        if (foreign_storage_cache_->isMetadataCached(chunk_key)) {
          if ((chunk_key[CHUNK_KEY_FRAGMENT_IDX] != fragment_id)) {
            if (chunk_keys_in_fragment.size() > 0) {
              auto required_buffers = foreign_storage_cache_->getChunkBuffersForCaching(
                  chunk_keys_in_fragment);
              getDataWrapper(table_key)->populateChunkBuffers(required_buffers,
                                                              optional_buffers);
              chunk_keys_in_fragment.clear();
            }

            // At this point, cache buffers for refreshable chunks in the last fragment
            // have been populated. Exit if the max refresh time has been exceeded.
            // Otherwise, move to the next fragment.
            auto current_time = std::chrono::high_resolution_clock::now();
            total_time += std::chrono::duration_cast<std::chrono::seconds>(
                              current_time - fragment_refresh_start_time)
                              .count();
            if (total_time >= MAX_REFRESH_TIME_IN_SECONDS) {
              LOG(WARNING) << "Refresh time exceeded for table key: { " << table_key[0]
                           << ", " << table_key[1]
                           << " } after fragment id: " << fragment_id;
              break;
            } else {
              fragment_refresh_start_time = std::chrono::high_resolution_clock::now();
            }
            fragment_id = chunk_key[CHUNK_KEY_FRAGMENT_IDX];
          }
          if (isVarLenKey(chunk_key)) {
            CHECK(isVarLenDataKey(chunk_key));
            ChunkKey index_chunk_key{chunk_key[CHUNK_KEY_DB_IDX],
                                     chunk_key[CHUNK_KEY_TABLE_IDX],
                                     chunk_key[CHUNK_KEY_COLUMN_IDX],
                                     chunk_key[CHUNK_KEY_FRAGMENT_IDX],
                                     2};
            chunk_keys_in_fragment.emplace_back(index_chunk_key);
            chunk_keys_to_be_cached.emplace_back(index_chunk_key);
          }
          chunk_keys_in_fragment.emplace_back(chunk_key);
          chunk_keys_to_be_cached.emplace_back(chunk_key);
        }
      }
      if (chunk_keys_in_fragment.size() > 0) {
        auto required_buffers =
            foreign_storage_cache_->getChunkBuffersForCaching(chunk_keys_in_fragment);
        getDataWrapper(table_key)->populateChunkBuffers(required_buffers,
                                                        optional_buffers);
      }
      foreign_storage_cache_->cacheTableChunks(chunk_keys_to_be_cached);
    }
  } catch (std::runtime_error& e) {
    throw PostEvictionRefreshException(e);
  }
}

void ForeignStorageMgr::evictTableFromCache(const ChunkKey& table_key) {
  if (!is_cache_enabled_) {
    return;
  }

  CHECK(isTableKey(table_key));
  foreign_storage_cache_->clearForTablePrefix(table_key);
}

void ForeignStorageMgr::clearTempChunkBufferMapEntriesForTable(
    const ChunkKey& table_key) {
  CHECK(isTableKey(table_key));
  std::lock_guard temp_chunk_buffer_map_lock(temp_chunk_buffer_map_mutex_);
  auto start_it = temp_chunk_buffer_map_.lower_bound(table_key);
  ChunkKey upper_bound_prefix{table_key[CHUNK_KEY_DB_IDX],
                              table_key[CHUNK_KEY_TABLE_IDX],
                              std::numeric_limits<int>::max()};
  auto end_it = temp_chunk_buffer_map_.upper_bound(upper_bound_prefix);
  temp_chunk_buffer_map_.erase(start_it, end_it);
}

bool ForeignStorageMgr::isDatawrapperRestored(const ChunkKey& chunk_key) {
  if (!hasDataWrapperForChunk(chunk_key)) {
    return false;
  }
  return getDataWrapper(chunk_key)->isRestored();
}

void ForeignStorageMgr::deleteBuffer(const ChunkKey& chunk_key, const bool purge) {
  UNREACHABLE();
}

void ForeignStorageMgr::deleteBuffersWithPrefix(const ChunkKey& chunk_key_prefix,
                                                const bool purge) {
  UNREACHABLE();
}

bool ForeignStorageMgr::isBufferOnDevice(const ChunkKey& chunk_key) {
  UNREACHABLE();
  return false;  // Added to avoid "no return statement" compiler warning
}

size_t ForeignStorageMgr::getNumChunks() {
  UNREACHABLE();
  return 0;  // Added to avoid "no return statement" compiler warning
}

AbstractBuffer* ForeignStorageMgr::createBuffer(const ChunkKey& chunk_key,
                                                const size_t page_size,
                                                const size_t initial_size) {
  UNREACHABLE();
  return nullptr;  // Added to avoid "no return statement" compiler warning
}

AbstractBuffer* ForeignStorageMgr::putBuffer(const ChunkKey& chunk_key,
                                             AbstractBuffer* source_buffer,
                                             const size_t num_bytes) {
  UNREACHABLE();
  return nullptr;  // Added to avoid "no return statement" compiler warning
}

std::string ForeignStorageMgr::printSlabs() {
  UNREACHABLE();
  return {};  // Added to avoid "no return statement" compiler warning
}

void ForeignStorageMgr::clearSlabs() {
  UNREACHABLE();
}

size_t ForeignStorageMgr::getMaxSize() {
  UNREACHABLE();
  return 0;  // Added to avoid "no return statement" compiler warning
}

size_t ForeignStorageMgr::getInUseSize() {
  UNREACHABLE();
  return 0;  // Added to avoid "no return statement" compiler warning
}

size_t ForeignStorageMgr::getAllocated() {
  UNREACHABLE();
  return 0;  // Added to avoid "no return statement" compiler warning
}

bool ForeignStorageMgr::isAllocationCapped() {
  UNREACHABLE();
  return false;  // Added to avoid "no return statement" compiler warning
}

void ForeignStorageMgr::checkpoint() {
  UNREACHABLE();
}

void ForeignStorageMgr::checkpoint(const int db_id, const int tb_id) {
  UNREACHABLE();
}

AbstractBuffer* ForeignStorageMgr::alloc(const size_t num_bytes) {
  UNREACHABLE();
  return nullptr;  // Added to avoid "no return statement" compiler warning
}

void ForeignStorageMgr::free(AbstractBuffer* buffer) {
  UNREACHABLE();
}
}  // namespace foreign_storage
