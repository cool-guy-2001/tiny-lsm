#include "sst/sst_iterator.h"
#include "block/block_iterator.h"
#include "sst/sst.h"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>

namespace tiny_lsm {

// predicate返回值:
//   0: 谓词
//   >0: 不满足谓词, 需要向右移动
//   <0: 不满足谓词, 需要向左移动
std::optional<std::pair<SstIterator, SstIterator>>
sst_iters_monotony_predicate(std::shared_ptr<SST> sst, uint64_t tranc_id,
                             std::function<int(const std::string&)> predicate) {
    if (!sst || sst->meta_entries.empty()) {
        return std::nullopt;
    }
    const auto& metas = sst->meta_entries;
    const size_t block_count = metas.size();
    /*
     * 构造一个指向指定 BlockIterator 的 SstIterator。
     *
     * block_idx 表示当前位于 SST 中的第几个 Block；
     * block_it 表示在这个 Block 内部的位置。
     */
    auto make_sst_iterator = [&](size_t block_idx, const std::shared_ptr<BlockIterator>& block_it) {
        SstIterator it(sst, tranc_id);
        it.set_block_idx(static_cast<int64_t>(block_idx));
        it.set_block_it(block_it);
        /*
         * 如果 BlockIterator 已经越过 SST 的最后一个 Block，
         * 把内部 BlockIterator 规范化为空指针，表示 SST end。
         */
        if (it.is_end() && it.m_block_idx == static_cast<int64_t>(sst->num_blocks())) {
            it.set_block_it(nullptr);
        }
        return it;
    };
    // ------------------------------------------------------------
    // 1. 二分查找第一个“不完全位于目标范围左侧”的 Block
    //
    // 如果 predicate(last_key) > 0，
    // 说明该 Block 中最大的 key 仍然太小，需要向右查找。
    // ------------------------------------------------------------
    size_t left = 0;
    size_t right = block_count;
    while (left < right) {
        size_t mid = left + (right - left) / 2;
        if (predicate(metas[mid].last_key) > 0) {
            left = mid + 1;
        } else {
            right = mid;
        }
    }
    const size_t first_candidate = left;
    if (first_candidate == block_count) {
        return std::nullopt;
    }
    // ------------------------------------------------------------
    // 2. 二分查找第一个“完全位于目标范围右侧”的 Block
    //
    // 如果 predicate(first_key) < 0，
    // 说明该 Block 中最小的 key 已经太大。
    //
    // 最终候选区间是：
    // [first_candidate, candidate_end)
    // ------------------------------------------------------------
    left = first_candidate;
    right = block_count;
    while (left < right) {
        size_t mid = left + (right - left) / 2;
        if (predicate(metas[mid].first_key) < 0) {
            right = mid;
        } else {
            left = mid + 1;
        }
    }
    const size_t candidate_end = left;
    if (first_candidate >= candidate_end) {
        return std::nullopt;
    }
    std::optional<SstIterator> final_begin;
    size_t first_match_idx = block_count;
    // 保存第一个匹配 Block 中的区间终点。
    // 如果它也是最后一个匹配 Block，就不必再次查询。
    std::shared_ptr<BlockIterator> first_match_end;

    for (size_t block_idx = first_candidate; block_idx < candidate_end; ++block_idx) {
        auto block = sst->read_block(static_cast<int64_t>(block_idx));
        auto result = block->get_monotony_predicate_iters(tranc_id, predicate);
        if (!result.has_value()) {
            continue;
        }
        const auto& [block_begin, block_end] = *result;
        final_begin = make_sst_iterator(block_idx, block_begin);
        first_match_idx = block_idx;
        first_match_end = block_end;
        break;
    }
    if (!final_begin.has_value()) {
        return std::nullopt;
    }
    std::optional<SstIterator> final_end;
    for (size_t block_idx = candidate_end; block_idx > first_match_idx;) {
        --block_idx;
        if (block_idx == first_match_idx) {
            final_end = make_sst_iterator(block_idx, first_match_end);
            break;
        }
        auto block = sst->read_block(static_cast<int64_t>(block_idx));
        auto result = block->get_monotony_predicate_iters(tranc_id, predicate);
        if (!result.has_value()) {
            continue;
        }
        const auto& [block_begin, block_end] = *result;
        final_end = make_sst_iterator(block_idx, block_end);
        break;
    }
    if (!final_end.has_value()) {
        return std::nullopt;
    }
    return std::pair<SstIterator, SstIterator>{std::move(*final_begin), std::move(*final_end)};
}

SstIterator::SstIterator(std::shared_ptr<SST> sst, uint64_t tranc_id, bool keep_all_versions)
    : m_sst(sst), m_block_idx(0), m_block_it(nullptr), max_tranc_id_(tranc_id),
      keep_all_versions_(keep_all_versions) {
    if (m_sst) {
        seek_first();
    }
}

SstIterator::SstIterator(std::shared_ptr<SST> sst, const std::string& key, uint64_t tranc_id,
                         bool keep_all_versions)
    : m_sst(sst), m_block_idx(0), m_block_it(nullptr), max_tranc_id_(tranc_id),
      keep_all_versions_(keep_all_versions) {
    if (m_sst) {
        seek(key);
    }
}

void SstIterator::set_block_idx(size_t idx) {
    m_block_idx = idx;
}
void SstIterator::set_block_it(std::shared_ptr<BlockIterator> it) {
    m_block_it = it;
}

void SstIterator::seek_first() {
    if (!m_sst || m_sst->num_blocks() == 0) {
        m_block_it = nullptr;
        return;
    }

    m_block_idx = 0;
    auto block = m_sst->read_block(m_block_idx);
    m_block_it = std::make_shared<BlockIterator>(block, 0, max_tranc_id_, keep_all_versions_);
}

void SstIterator::seek(const std::string& key) {
    if (!m_sst) {
        m_block_it = nullptr;
        return;
    }

    try {
        m_block_idx = m_sst->find_block_idx(key);
        if (m_block_idx == -1 || m_block_idx >= m_sst->num_blocks()) {
            // 置为 end
            // TODO: 这个边界情况需要添加单元测试
            m_block_it = nullptr;
            m_block_idx = m_sst->num_blocks();
            return;
        }
        auto block = m_sst->read_block(m_block_idx);
        if (!block) {
            m_block_it = nullptr;
            return;
        }
        m_block_it = std::make_shared<BlockIterator>(block, key, max_tranc_id_, keep_all_versions_);
        if (m_block_it->is_end()) {
            // block 中找不到
            m_block_idx = m_sst->num_blocks();
            m_block_it = nullptr;
            return;
        }
    } catch (const std::exception&) {
        m_block_it = nullptr;
        return;
    }
}

std::string SstIterator::key() {
    if (!m_block_it) {
        throw std::runtime_error("Iterator is invalid");
    }
    return (*m_block_it)->first;
}

std::string SstIterator::value() {
    if (!m_block_it) {
        throw std::runtime_error("Iterator is invalid");
    }
    return m_sst->resolve_value((*m_block_it)->second);
}

BaseIterator& SstIterator::operator++() {
    if (!m_block_it) { // 添加空指针检查
        return *this;
    }
    ++(*m_block_it);
    if (m_block_it->is_end()) {
        m_block_idx++;
        if (m_block_idx < m_sst->num_blocks()) {
            // 读取下一个block
            auto next_block = m_sst->read_block(m_block_idx);
            BlockIterator new_blk_it(next_block, 0, max_tranc_id_, keep_all_versions_);
            (*m_block_it) = new_blk_it;
        } else {
            // 没有下一个block
            m_block_it = nullptr;
        }
    }
    return *this;
}

bool SstIterator::operator==(const BaseIterator& other) const {
    if (other.get_type() != IteratorType::SstIterator) {
        return false;
    }
    auto other2 = dynamic_cast<const SstIterator&>(other);
    if (m_sst != other2.m_sst || m_block_idx != other2.m_block_idx) {
        return false;
    }

    if (!m_block_it && !other2.m_block_it) {
        return true;
    }

    if (!m_block_it || !other2.m_block_it) {
        return false;
    }

    return *m_block_it == *other2.m_block_it;
}

bool SstIterator::operator!=(const BaseIterator& other) const {
    return !(*this == other);
}

SstIterator::value_type SstIterator::operator*() const {
    if (!m_block_it) {
        throw std::runtime_error("Iterator is invalid");
    }
    auto raw = (**m_block_it);
    raw.second = m_sst->resolve_value(raw.second);
    return raw;
}

IteratorType SstIterator::get_type() const {
    return IteratorType::SstIterator;
}

uint64_t SstIterator::get_tranc_id() const {
    if (keep_all_versions_ && m_block_it) {
        return m_block_it->get_cur_tranc_id();
    }
    return max_tranc_id_;
}
bool SstIterator::is_end() const {
    return !m_block_it;
}

bool SstIterator::is_valid() const {
    return m_block_it && !m_block_it->is_end() && m_block_idx < m_sst->num_blocks();
}
SstIterator::pointer SstIterator::operator->() const {
    update_current();
    return &(*cached_value);
}

void SstIterator::update_current() const {
    if (!cached_value && m_block_it && !m_block_it->is_end()) {
        auto raw = *(*m_block_it);
        raw.second = m_sst->resolve_value(raw.second);
        cached_value = raw;
    }
}

uint64_t SstIterator::get_cur_tranc_id() const {
    if (!m_block_it) {
        return 0;
    }
    return m_block_it->get_cur_tranc_id();
}

std::pair<HeapIterator, HeapIterator>
SstIterator::merge_sst_iterator(std::vector<SstIterator> iter_vec, uint64_t tranc_id,
                                bool keep_all_versions) {
    if (iter_vec.empty()) {
        return std::make_pair(HeapIterator(), HeapIterator());
    }

    HeapIterator it_begin(false, keep_all_versions); // 不跳过删除元素
    for (auto& iter : iter_vec) {
        while (iter.is_valid() && !iter.is_end()) {
            it_begin.items.emplace(
                iter.key(), iter.m_sst->resolve_value(iter.m_block_it->operator*().second),
                -iter.m_sst->get_sst_id(), 0,
                iter.get_cur_tranc_id()); // ! 此处的level暂时没有作用, 都作用于同一层的比较
            ++iter;
        }
    }
    return std::make_pair(it_begin, HeapIterator());
}
} // namespace tiny_lsm
