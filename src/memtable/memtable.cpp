#include "../../include/memtable/memtable.h"
#include "../../include/config/config.h"
#include "../../include/consts.h"
#include "../../include/iterator/iterator.h"
#include "../../include/skiplist/skiplist.h"
#include "../../include/sst/sst.h"
#include "spdlog/spdlog.h"
#include <algorithm>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <sys/types.h>
#include <utility>
#include <vector>

namespace tiny_lsm {

class BlockCache;

// MemTable implementation using PIMPL idiom
MemTable::MemTable() : frozen_bytes(0) {
  current_table = std::make_shared<SkipList>();
}
MemTable::~MemTable() = default;

void MemTable::put_(const std::string &key, const std::string &value,
                    uint64_t tranc_id) {
  // TODO: Lab2.1 无锁版本的 put
  current_table->put(key, value, tranc_id);
  if (current_table->get_size() >
      TomlConfig::getInstance().getLsmPerMemSizeLimit()) {
    frozen_cur_table_(); // current_table超过了阈值,调用无锁冻结函数
  }
}

void MemTable::put(const std::string &key, const std::string &value,
                   uint64_t tranc_id) {
  // TODO: Lab2.1 有锁版本的 put
  std::unique_lock<std::shared_mutex> cur_lock(cur_mtx);
  current_table->put(key, value, tranc_id);
  if (current_table->get_size() >
      TomlConfig::getInstance().getLsmPerMemSizeLimit()) {
    std::unique_lock<std::shared_mutex> fro_lock(frozen_mtx);
    frozen_cur_table_();
  }
}

void MemTable::put_batch(
    const std::vector<std::pair<std::string, std::string>> &kvs,
    uint64_t tranc_id) {
  // TODO: Lab2.1 有锁版本的 put_batch
  // ? tranc_id 参数可暂时忽略其逻辑判断, 直接插入即可
  std::unique_lock<std::shared_mutex> cur_lock(cur_mtx);
  size_t limit = TomlConfig::getInstance().getLsmPerMemSizeLimit();
  for (auto [key, value] : kvs) {
    current_table->put(key, value, tranc_id);
    if (current_table->get_size() > limit) {
      std::unique_lock<std::shared_mutex> fro_lock(frozen_mtx);
      frozen_cur_table_();
    }
  }
}

SkipListIterator MemTable::cur_get_(const std::string &key, uint64_t tranc_id) {
  // 检查当前活跃的memtable
  // TODO: Lab2.1 从活跃跳表中查询
  return current_table->get(key, tranc_id);
}

SkipListIterator MemTable::frozen_get_(const std::string &key,
                                       uint64_t tranc_id) {
  // TODO: Lab2.1 从冻结跳表中查询
  // ? 你需要尤其注意跳表的遍历顺序
  // ? tranc_id 参数可暂时忽略, 直接传递参数即可
  // frozen_tables是一个list,插入采用push_front,所以越靠前的数据越新,因此从表头开始顺序读取即可
  for (auto &tabe : frozen_tables) {
    auto result = tabe->get(key, tranc_id);
    if (result.is_valid())
      return result;
  }
  return SkipListIterator{};
}

SkipListIterator MemTable::get(const std::string &key, uint64_t tranc_id) {
  // TODO: Lab2.1 查询, 建议复用 cur_get_ 和 frozen_get_
  // ? 注意并发控制
  {
    std::shared_lock<std::shared_mutex> cur_lock(
        cur_mtx); //先加读锁,再查current_table,加读锁保证读取期间数据结构不被写线程破坏
    auto cur_it = cur_get_(key, tranc_id);
    if (cur_it.is_valid()) {
      return cur_it;
    }
  }
  //读取完current_table，应该把读锁释放，所以加{}限制作用域
  //同时持有多把锁的话，锁粒度变大，会阻塞写线程
  {
    std::shared_lock<std::shared_mutex> frozen_lock(frozen_mtx);
    auto frozen_it = frozen_get_(key, tranc_id);
    if (frozen_it.is_valid()) {
      return frozen_it;
    }
  }
  return SkipListIterator{};
}

SkipListIterator MemTable::get_(const std::string &key, uint64_t tranc_id) {
  // TODO: Lab2.1 查询, 无锁版本
  auto cur_it = cur_get_(key, tranc_id);
  if (cur_it.is_valid()) {
    return cur_it;
  }
  auto frozen_it = frozen_get_(key, tranc_id);
  if (frozen_it.is_valid()) {
    return frozen_it;
  }
  return SkipListIterator{};
}

std::vector<
    std::pair<std::string, std::optional<std::pair<std::string, uint64_t>>>>
MemTable::get_batch(const std::vector<std::string> &keys, uint64_t tranc_id) {
  spdlog::trace("MemTable--get_batch with {} keys", keys.size());

  std::vector<
      std::pair<std::string, std::optional<std::pair<std::string, uint64_t>>>>
      results;
  results.reserve(keys.size());

  // 1. 先获取活跃表的锁
  std::shared_lock<std::shared_mutex> slock1(cur_mtx);
  for (size_t idx = 0; idx < keys.size(); idx++) {
    auto key = keys[idx];
    auto cur_res = cur_get_(key, tranc_id);
    if (cur_res.is_valid()) {
      // 值存在且不为空
      results.emplace_back(
          key, std::make_pair(cur_res.get_value(), cur_res.get_tranc_id()));
    } else {
      // 如果活跃表中未找到，先占位
      results.emplace_back(key, std::nullopt);
    }
  }

  // 2. 如果某些键在活跃表中未找到，还需要查找冻结表
  if (!std::any_of(results.begin(), results.end(), [](const auto &result) {
        return !result.second.has_value();
      })) {
    // ! 最后, 需要把 value 为空的键值对标记为 nullopt
    for (auto &[key, value] : results) {
      if (!value.has_value()) {
        value = std::nullopt;
      }
    }
    return results;
  }

  slock1.unlock(); // 释放活跃表的锁
  std::shared_lock<std::shared_mutex> slock2(frozen_mtx); // 获取冻结表的锁
  for (size_t idx = 0; idx < keys.size(); idx++) {
    if (results[idx].second.has_value()) {
      continue; // 如果在活跃表中已经找到，则跳过
    }
    auto key = keys[idx];
    auto frozen_result = frozen_get_(key, tranc_id);
    if (frozen_result.is_valid()) {
      // 值存在且不为空
      results[idx] =
          std::make_pair(key, std::make_pair(frozen_result.get_value(),
                                             frozen_result.get_tranc_id()));
    } else {
      results[idx] = std::make_pair(key, std::nullopt);
    }
  }

  return results;
}

void MemTable::remove_(const std::string &key, uint64_t tranc_id) {
  // TODO Lab2.1 无锁版本的remove
  //插入value为空的键值对表示对数据的删除标记
  current_table->put(key, "", tranc_id);
  size_t limit = TomlConfig::getInstance().getLsmPerMemSizeLimit();
  if (current_table->get_size() > limit) {
    frozen_cur_table_();
  }
}

void MemTable::remove(const std::string &key, uint64_t tranc_id) {
  // TODO Lab2.1 有锁版本的remove
  std::unique_lock<std::shared_mutex> cur_lock(cur_mtx);
  current_table->put(key, "", tranc_id);
  size_t limit = TomlConfig::getInstance().getLsmPerMemSizeLimit();
  if (current_table->get_size() > limit) {
    std::unique_lock<std::shared_mutex> frozen_lock(frozen_mtx);
    frozen_cur_table_();
  }
}

void MemTable::remove_batch(const std::vector<std::string> &keys,
                            uint64_t tranc_id) {
  // TODO Lab2.1 有锁版本的remove_batch
  std::unique_lock<std::shared_mutex> cur_lock(cur_mtx);
  size_t limit = TomlConfig::getInstance().getLsmPerMemSizeLimit();
  for (const auto &key : keys) {
    current_table->put(key, "", tranc_id);
    if (current_table->get_size() > limit) {
      std::unique_lock<std::shared_mutex> frozen_lock(frozen_mtx);
      frozen_cur_table_();
    }
  }
}

void MemTable::clear() {
  spdlog::info("MemTable--clear(): Clearing all tables");

  std::unique_lock<std::shared_mutex> lock1(cur_mtx);
  std::unique_lock<std::shared_mutex> lock2(frozen_mtx);
  frozen_tables.clear();
  current_table->clear();
}

// 将最老的 memtable 写入 SST, 并返回控制类
std::shared_ptr<SST>
MemTable::flush_last(SSTBuilder &builder, std::string &sst_path, size_t sst_id,
                     std::shared_ptr<BlockCache> block_cache) {
  spdlog::debug("MemTable--flush_last(): Starting to flush memtable to SST{}",
                sst_id);

  // 由于 flush 后需要移除最老的 memtable, 因此需要加写锁
  std::unique_lock<std::shared_mutex> lock(frozen_mtx);

  uint64_t max_tranc_id = 0;
  uint64_t min_tranc_id = UINT64_MAX;

  if (frozen_tables.empty()) {
    // 如果当前表为空，直接返回nullptr
    if (current_table->get_size() == 0) {
      spdlog::debug(
          "MemTable--flush_last(): Current table is empty, returning null");

      return nullptr;
    }
    // 将当前表加入到frozen_tables头部
    frozen_tables.push_front(current_table);
    frozen_bytes += current_table->get_size();
    // 创建新的空表作为当前表
    current_table = std::make_shared<SkipList>();
  }

  // 将最老的 memtable 写入 SST
  std::shared_ptr<SkipList> table = frozen_tables.back();
  frozen_tables.pop_back();
  frozen_bytes -= table->get_size();

  std::vector<std::tuple<std::string, std::string, uint64_t>> flush_data =
      table->flush();
  for (auto &[k, v, t] : flush_data) {
    max_tranc_id = std::max(t, max_tranc_id);
    min_tranc_id = std::min(t, min_tranc_id);
    builder.add(k, v, t);
  }
  auto sst = builder.build(sst_id, sst_path, block_cache);

  spdlog::info("MemTable--flush_last(): SST{} built successfully at '{}'",
               sst_id, sst_path);

  return sst;
}

void MemTable::frozen_cur_table_() {
  // TODO: 冻结活跃表
  frozen_tables.push_front(current_table);
  frozen_bytes += current_table->get_size();
  current_table = std::make_shared<SkipList>();
}

void MemTable::frozen_cur_table() {
  // TODO: 冻结活跃表, 有锁版本
  std::unique_lock<std::shared_mutex> cur_lock(cur_mtx);
  std::unique_lock<std::shared_mutex> frozen_lock(frozen_mtx);
  frozen_cur_table_();
}

size_t MemTable::get_cur_size() {
  std::shared_lock<std::shared_mutex> slock(cur_mtx);
  return current_table->get_size();
}

size_t MemTable::get_frozen_size() {
  std::shared_lock<std::shared_mutex> slock(frozen_mtx);
  return frozen_bytes;
}

size_t MemTable::get_total_size() {
  std::shared_lock<std::shared_mutex> slock1(cur_mtx);
  std::shared_lock<std::shared_mutex> slock2(frozen_mtx);
  return get_frozen_size() + get_cur_size();
}

HeapIterator MemTable::begin(uint64_t tranc_id) {
  // TODO Lab 2.2 MemTable 的迭代器
  std::vector<SearchItem> item_vec;
  int idx = 0;
  //遍历冻结表,KV键值对存入item_vec中
  for (auto it = frozen_tables.rbegin(); it != frozen_tables.rend(); ++it) {
    // it是std::shared_ptr<SkipList>
    auto &sp = *it;
    for (auto sit = sp->begin(); !sit.is_end(); ++sit) {
      auto tmp_key = sit.get_key();
      auto tmp_value = sit.get_value();
      auto tmp_trancid = sit.get_tranc_id();
      item_vec.push_back(SearchItem(tmp_key, tmp_value, idx, 0, tmp_trancid));
    }
    idx++;
  }
  //遍历活跃表
  if (current_table != nullptr) {
    for (auto it2 = current_table->begin(); !it2.is_end(); ++it2) {
      auto tmp_key = it2.get_key();
      auto tmp_value = it2.get_value();
      auto tmp_trancid = it2.get_tranc_id();
      item_vec.push_back(SearchItem(tmp_key, tmp_value, idx, 0, tmp_trancid));
    }
  }
  return HeapIterator(item_vec, tranc_id, /*skip_delete=*/true);
}

HeapIterator MemTable::end() {
  // TODO Lab 2.2 MemTable 的迭代器

  return HeapIterator(true);
}

HeapIterator MemTable::iters_preffix(const std::string &preffix,
                                     uint64_t tranc_id) {

  // TODO Lab 2.3 MemTable 的前缀迭代器
  //不需要像begin那样遍历单个current_table和所有frozen_table,对于每个跳表SkipList,只取preffix范围内的键值对
  std::vector<SearchItem> item_vec;
  int idx = 0;

  auto has_prefix = [](const std::string &s, const std::string &pre) -> bool {
    return s.rfind(pre, 0) == 0;
  };

  for (auto it = frozen_tables.rbegin(); it != frozen_tables.rend(); ++it) {
    auto &p = *it;
    auto it1 = p->begin_preffix(preffix);
    while (it1.is_valid() && has_prefix(it1.get_key(), preffix)) {
      item_vec.push_back(SearchItem(it1.get_key(), it1.get_value(), idx, 0,
                                    it1.get_tranc_id()));
      ++it1;
    }
    idx++;
  }
  if (current_table) {
    auto tmp_it = current_table->begin_preffix(preffix);
    while (tmp_it.is_valid() && has_prefix(tmp_it.get_key(), preffix)) {
      item_vec.push_back(SearchItem(tmp_it.get_key(), tmp_it.get_value(), idx,
                                    0, tmp_it.get_tranc_id()));
      ++tmp_it;
    }
  }

  return HeapIterator(item_vec, tranc_id, /*skip_delete=*/true);
  // return {};
}

std::optional<std::pair<HeapIterator, HeapIterator>>
MemTable::iters_monotony_predicate(
    uint64_t tranc_id, std::function<int(const std::string &)> predicate) {
  // TODO Lab 2.3 MemTable 的谓词查询迭代器起始范围
  std::vector<SearchItem> item_vec;
  int idx = 0;
  for (auto it = frozen_tables.rbegin(); it != frozen_tables.rend(); ++it) {
    auto &pr = *it;
    auto it1 = pr->iters_monotony_predicate(predicate);
    if (it1 != std::nullopt) {
      auto p = it1->first;
      while (p.is_valid() && predicate(p.get_key()) == 0) {
        item_vec.push_back(
            SearchItem(p.get_key(), p.get_value(), idx, 0, p.get_tranc_id()));
        ++p;
      }
    }
    idx++;
  }
  if (current_table) {
    auto tmp_it = current_table->iters_monotony_predicate(predicate);
    if (tmp_it != std::nullopt) {
      auto p = tmp_it->first;
      while (p.is_valid() && predicate(p.get_key()) == 0) {
        item_vec.push_back(
            SearchItem(p.get_key(), p.get_value(), idx, 0, p.get_tranc_id()));
        ++p;
      }
    }
  }
  HeapIterator begin(item_vec, tranc_id, true);
  HeapIterator end(true);
  return std::make_optional(std::make_pair(begin, end));
}
} // namespace tiny_lsm