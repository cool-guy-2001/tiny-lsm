#include "sst/sst.h"
#include "block/block_cache.h"
#include "block/blockmeta.h"
#include "config/config.h"
#include "consts.h"
#include "sst/sst_iterator.h"
#include "utils/bloom_filter.h"
#include "utils/files.h"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <stdexcept>
#include <type_traits>

namespace tiny_lsm {

// Magic byte identifying a WiscKey SST footer
static constexpr uint8_t WISCKEY_MAGIC = 0x4B;
// Old footer size (24 bytes)
static constexpr size_t OLD_FOOTER_SIZE = sizeof(uint32_t) * 2 + sizeof(uint64_t) * 2;
// New WiscKey footer size (26 bytes)
static constexpr size_t WISCKEY_FOOTER_SIZE = OLD_FOOTER_SIZE + 2;

// **************************************************
// SST
// **************************************************
namespace {
template <typename T> void append_fixed(std::vector<uint8_t>& buf, const T& value) {
    static_assert(std::is_trivially_copyable_v<T>,
                  "append_fixed only supports trivially copyable types");
    size_t old_size = buf.size();
    buf.resize(old_size + sizeof(T));
    std::memcpy(buf.data() + old_size, &value, sizeof(T));
}
template <typename T> T read_fixed_from_slice(const std::vector<uint8_t>& buf, size_t pos) {
    static_assert(std::is_trivially_copyable_v<T>,
                  "read_fixed_from_slice only supports trivially copyable types");
    if (pos + sizeof(T) > buf.size()) {
        throw std::runtime_error("read_fixed_from_slice out of range");
    }
    T value{};
    std::memcpy(&value, buf.data() + pos, sizeof(T));
    return value;
}
} // namespace

std::shared_ptr<SST> SST::open(size_t sst_id, FileObj file, std::shared_ptr<BlockCache> block_cache,
                               std::shared_ptr<VLog> vlog) {
    // TODO: Lab 3.6 打开一个SST文件, 返回一个描述类
    // ? 步骤:
    // ?   0. 检测文件末尾 magic byte 判断是否为 WiscKey 格式 (WISCKEY_MAGIC = 0x4B)
    // ?      footer 共 24 字节 (老格式) 或 26 字节 (WiscKey, 末尾多 storage_mode + magic)
    // ?   1. 从文件末尾读取 footer: meta_block_offset, bloom_offset, min_tranc_id, max_tranc_id
    // ?      如为 WiscKey 格式, 还需读取 storage_mode_
    // ?   2. 读取并解码 Bloom Filter (bloom_offset ~ meta_block_offset 之间)
    // ?   3. 读取并解码元数据块 (meta_block_offset ~ bloom_offset 之间)
    // ?      调用 BlockMeta::decode_meta_from_slice
    // ?   4. 设置 first_key 和 last_key
    // ?   注: vlog 用于 WiscKey 模式下的 value 读取, 直接赋值给 sst->vlog_
    auto sst = std::make_shared<SST>();
    sst->sst_id = sst_id;
    sst->file = std::move(file);
    sst->block_cache = block_cache;
    sst->vlog_ = vlog;

    size_t file_size = sst->sst_size();

    if (file_size < OLD_FOOTER_SIZE) {
        throw std::runtime_error("Invalid SST file:too small");
    }
    size_t footer_size = OLD_FOOTER_SIZE;
    if (file_size >= WISCKEY_FOOTER_SIZE && sst->file.read_uint8(file_size - 1) == WISCKEY_MAGIC) {

        size_t candidate_footer_start = file_size - WISCKEY_FOOTER_SIZE;

        auto candidate_footer =
            sst->file.read_to_slice(candidate_footer_start, WISCKEY_FOOTER_SIZE);

        uint32_t meta_offset = read_fixed_from_slice<uint32_t>(candidate_footer, 0);

        uint32_t bloom_offset = read_fixed_from_slice<uint32_t>(candidate_footer, sizeof(uint32_t));

        if (meta_offset <= bloom_offset && bloom_offset <= candidate_footer_start) {
            footer_size = WISCKEY_FOOTER_SIZE;
        }
    }

    size_t footer_start = file_size - footer_size;
    auto footer = sst->file.read_to_slice(footer_start, footer_size);
    //解析footer
    sst->meta_block_offset = read_fixed_from_slice<uint32_t>(footer, 0);
    sst->bloom_offset = read_fixed_from_slice<uint32_t>(footer, sizeof(uint32_t));
    sst->min_tranc_id_ = read_fixed_from_slice<uint64_t>(footer, sizeof(uint32_t) * 2);
    sst->max_tranc_id_ =
        read_fixed_from_slice<uint64_t>(footer, sizeof(uint32_t) * 2 + sizeof(uint64_t));
    if (footer_size == WISCKEY_FOOTER_SIZE) {
        sst->storage_mode_ = read_fixed_from_slice<uint8_t>(footer, OLD_FOOTER_SIZE);
    } else {
        sst->storage_mode_ = 0;
    }
    //判断合法性
    if (sst->meta_block_offset > sst->bloom_offset || sst->bloom_offset > footer_start) {
        throw std::runtime_error("Invalid SST file: corrupted footer offsets");
    }
    //读取并解码bloom filter
    //文件布局[block section] [meta_section] [bloom_section] [footer]
    if (sst->bloom_offset < footer_start) {
        size_t bloom_size = footer_start - sst->bloom_offset;
        if (bloom_size > 0) {
            auto bloom_bytes = sst->file.read_to_slice(sst->bloom_offset, bloom_size);
            auto bloom = BloomFilter::decode(bloom_bytes);
            sst->bloom_filter = std::make_shared<BloomFilter>(std::move(bloom));
            // std::move,本身并不会移动数据，而是做一个类型转换，把一个左值转换成右值引用。
            //真正发生移动的是后面调用的移动构造函数或者移动赋值函数。
        }
    }
    //读取并解码meta section

    size_t meta_size = sst->bloom_offset - sst->meta_block_offset;
    auto meta_bytes = sst->file.read_to_slice(sst->meta_block_offset, meta_size);
    sst->meta_entries = BlockMeta::decode_meta_from_slice(meta_bytes);
    if (sst->meta_entries.empty()) {
        throw std::runtime_error("Invalid SST file: empty meta entries");
    }
    //空vector调用front和back是未定义行为，应该先判断是否为空。
    sst->first_key = sst->meta_entries.front().first_key;
    sst->last_key = sst->meta_entries.back().last_key;

    return sst;
}

void SST::del_sst() {
    file.del_file();
}

std::shared_ptr<Block> SST::read_block(int64_t block_idx) {
    // TODO: Lab 3.6 根据 block 的 id 读取一个 Block
    // ? 先从 block_cache 查找; 未命中则计算该 block 的偏移和大小
    // ? 读取数据后调用 Block::decode(data, true) 解码
    // ? 解码后存入 block_cache 并返回
    // ? block 大小: 相邻 meta_entries 的 offset 差值; 最后一个 block 到 meta_block_offset
    if (block_idx < 0 || block_idx >= static_cast<uint64_t>(meta_entries.size())) {
        throw std::out_of_range("SST::read_block: invalid block_idx");
    }
    int cache_sst_id = static_cast<int>(sst_id);
    int cache_block_id = static_cast<int>(block_idx);
    if (block_cache) {
        auto cached_block = block_cache->get(cache_sst_id, cache_block_id);
        if (cached_block != nullptr) {
            return cached_block;
        }
    }
    size_t block_start = meta_entries[block_idx].offset;
    size_t block_end = (block_idx + 1 < static_cast<int64_t>(meta_entries.size())
                            ? meta_entries[block_idx + 1].offset
                            : meta_block_offset);
    if (block_start >= meta_block_offset || block_end > meta_block_offset ||
        block_end <= block_start) {
        throw std::runtime_error("SST::read_block: invalid block offset range");
    }
    size_t block_size = block_end - block_start;
    auto block_bytes = file.read_to_slice(block_start, block_size);
    auto block = Block::decode(block_bytes, true);
    if (block_cache) {
        block_cache->put(cache_sst_id, cache_block_id, block);
    }
    return block;
}

int64_t SST::find_block_idx(const std::string& key) {
    // TODO: Lab 3.6 二分查找
    // ? 先用布隆过滤器快速排除 (bloom_filter->possibly_contains(key))
    // ? 再在 meta_entries 上二分查找: first_key <= key <= last_key
    // ? 若未找到合适 block 返回 -1
    if (meta_entries.empty()) {
        return -1;
    }
    if (key < first_key || key > last_key) {
        return -1;
    }
    if (bloom_filter && !bloom_filter->possibly_contains(key)) {
        return -1;
    }
    int64_t left = 0;
    int64_t right = static_cast<int64_t>(meta_entries.size()) - 1;
    int64_t ans = -1;
    while (left <= right) {
        int64_t mid = left + (right - left) / 2;
        if (meta_entries[mid].last_key < key) {
            left = mid + 1;
        } else {
            ans = mid;
            right = mid - 1;
        }
    }
    if (ans == -1) {
        return -1;
    }
    if (meta_entries[ans].first_key <= key && key <= meta_entries[ans].last_key) {
        return ans;
    }
    return -1;
}

SstIterator SST::get(const std::string& key, uint64_t tranc_id) {
    // TODO: Lab 3.6 根据查询 key 返回一个迭代器
    // ? 先检查 key 是否在 [first_key, last_key] 范围内, 否则返回 end()
    // ? 再用 bloom_filter 快速排除
    // ? 返回 SstIterator(shared_from_this(), key, tranc_id)
    if (find_block_idx(key) == -1) {
        return end();
    }

    return SstIterator(shared_from_this(), key, tranc_id);
    // throw std::runtime_error("Not implemented");
}

size_t SST::num_blocks() const {
    return meta_entries.size();
}

std::string SST::get_first_key() const {
    return first_key;
}

std::string SST::get_last_key() const {
    return last_key;
}

size_t SST::sst_size() const {
    return file.size();
}

size_t SST::get_sst_id() const {
    return sst_id;
}

std::string SST::resolve_value(const std::string& raw_value) const {
    // WiscKey 模式下: raw_value 是 12 字节的 vlog 引用 [offset:8][size:4]
    // 普通模式下直接返回 raw_value
    if (storage_mode_ == 0 || raw_value.empty()) {
        return raw_value;
    }
    if (raw_value.size() < 12) {
        return raw_value;
    }
    uint64_t off = 0;
    uint32_t sz = 0;
    memcpy(&off, raw_value.data(), sizeof(uint64_t));
    memcpy(&sz, raw_value.data() + sizeof(uint64_t), sizeof(uint32_t));
    if (!vlog_) {
        throw std::runtime_error("SST::resolve_value: vlog is null for WiscKey SST");
    }
    return vlog_->read_value(off, sz);
}

bool SST::is_wisckey() const {
    return storage_mode_ == 1;
}

SstIterator SST::begin(uint64_t tranc_id, bool keep_all_versions) {
    // TODO: Lab 3.6 返回起始位置迭代器
    // ? 返回 SstIterator(shared_from_this(), tranc_id, keep_all_versions)
    // throw std::runtime_error("Not implemented");
    return SstIterator(shared_from_this(), tranc_id, keep_all_versions);
}

SstIterator SST::end() {
    // TODO: Lab 3.6 返回终止位置迭代器
    // ? 构造一个 SstIterator 并将 m_block_idx 设为 meta_entries.size(), m_block_it 设为 nullptr
    // throw std::runtime_error("Not implemented");
    SstIterator iter(shared_from_this(), 0, false);

    iter.m_block_idx = static_cast<int64_t>(meta_entries.size());
    iter.m_block_it = nullptr;

    return iter;
}

std::pair<uint64_t, uint64_t> SST::get_tranc_id_range() const {
    return std::make_pair(min_tranc_id_, max_tranc_id_);
}

// **************************************************
// SSTBuilder
// **************************************************

SSTBuilder::SSTBuilder(size_t block_size, bool has_bloom)
    : block(block_size), block_size(block_size) {
    // 初始化第一个block
    if (has_bloom) {
        bloom_filter = std::make_shared<BloomFilter>(
            TomlConfig::getInstance().getBloomFilterExpectedSize(),
            TomlConfig::getInstance().getBloomFilterExpectedErrorRate());
    }
    meta_entries.clear();
    data.clear();
    first_key.clear();
    last_key.clear();
}

SSTBuilder::SSTBuilder(size_t block_size, bool has_bloom, std::shared_ptr<VLog> vlog,
                       size_t wisckey_threshold)
    : block(block_size), block_size(block_size), vlog_(std::move(vlog)),
      wisckey_threshold_(wisckey_threshold), storage_mode_(1) {
    // WiscKey 模式构造函数: vlog 用于大 value 分离存储
    if (has_bloom) {
        bloom_filter = std::make_shared<BloomFilter>(
            TomlConfig::getInstance().getBloomFilterExpectedSize(),
            TomlConfig::getInstance().getBloomFilterExpectedErrorRate());
    }
    meta_entries.clear();
    data.clear();
    first_key.clear();
    last_key.clear();
}

void SSTBuilder::add(const std::string& key, const std::string& value, uint64_t tranc_id) {
    // TODO: Lab 3.5 添加键值对
    // ? 记录 first_key (第一次调用时)
    // ? 向 bloom_filter 中 add key
    // ? 更新 max_tranc_id_ / min_tranc_id_
    // ? WiscKey 模式下: 若 value 非空且超过 wisckey_threshold_, 将 value 写入 vlog
    // ?   并将 vlog 引用 [offset:8][size:4] 作为 actual_value
    // ? 尝试向 block 添加 entry; 若返回 false (block满) 先调用 finish_block() 再添加
    // ? 注意: 相同 key 必须在同一个 block 中 (force_write = key == last_key)
    // ? 更新 last_key

    if (first_key.empty()) {
        first_key = key;
    }
    if (bloom_filter) {
        bloom_filter->add(key);
    }
    min_tranc_id_ = std::min(min_tranc_id_, tranc_id);
    max_tranc_id_ = std::max(max_tranc_id_, tranc_id);

    std::string actual_value = value;
    if (storage_mode_ == 1 && !value.empty() && value.size() > wisckey_threshold_) {
        if (!vlog_) {
            throw std::runtime_error("SSTBuilder::add: vlog is null");
        }
        uint64_t offset = vlog_->append(key, value);
        uint32_t value_size = static_cast<uint32_t>(value.size());
        actual_value.resize(sizeof(uint64_t) + sizeof(uint32_t));
        memcpy(actual_value.data(), &offset, sizeof(uint64_t));
        memcpy(actual_value.data() + sizeof(uint64_t), &value_size, sizeof(uint32_t));
    }

    bool force_write = key == last_key;
    if (!block.add_entry(key, actual_value, tranc_id, force_write)) {
        finish_block();
        block.add_entry(key, actual_value, tranc_id, true);
    }
    last_key = key;
}

size_t SSTBuilder::real_size() const {
    return data.size() + block.cur_size();
}

size_t SSTBuilder::estimated_size() const {
    return data.size();
}

void SSTBuilder::finish_block() {
    // TODO: Lab 3.5 构建块
    // ? 将当前 block 编码并追加到 data, 同时向 meta_entries 添加元数据
    // ? 然后重置 block 为新的空 Block
    // ? meta_entries 记录: (当前data起始偏移, first_key, last_key)

    if (block.is_empty()) {
        return;
    }

    size_t offset = data.size();
    meta_entries.emplace_back(offset, block.get_first_key(), last_key);

    std::vector<uint8_t> encoded = block.encode();
    data.insert(data.end(), encoded.begin(), encoded.end());
    block = Block(block_size);
}

std::shared_ptr<SST> SSTBuilder::build(size_t sst_id, const std::string& path,
                                       std::shared_ptr<BlockCache> block_cache) {
    // TODO: Lab 3.5 构建一个SST
    // ? 1. 若 block 非空则调用 finish_block()
    // ? 2. 若 meta_entries 为空则抛出异常
    // ? 3. 编码元数据块并追加到 data (BlockMeta::encode_meta_to_slice)
    // ? 4. 追加 Bloom Filter 编码
    // ? 5. 写入 footer (老格式 24B 或 WiscKey 26B):
    // ?    [meta_offset:uint32][bloom_offset:uint32][min_tranc_id:uint64][max_tranc_id:uint64]
    // ?    WiscKey 额外: [storage_mode_:uint8][WISCKEY_MAGIC:uint8]
    // ? 6. 调用 FileObj::create_and_write 写文件
    // ? 7. 构造并返回 SST 对象
    // 1.如果当前block非空，就先finsh,最后一个block可能没满，但数据遍历完了。
    if (!block.is_empty()) {
        finish_block();
    }
    // 2.若meta为空，则抛出异常。
    if (meta_entries.empty()) {
        throw std::runtime_error("cannot build empty SST");
    }
    // 3.紧接着写meta_section
    uint32_t meta_offset = static_cast<uint32_t>(data.size());
    std::vector<uint8_t> metadata;
    BlockMeta::encode_meta_to_slice(meta_entries, metadata);
    data.insert(data.end(), metadata.begin(), metadata.end());

    // 4.记录并写入bloom section
    uint32_t bloom_offset = static_cast<uint32_t>(data.size());
    auto bloom_data = bloom_filter->encode();
    data.insert(data.end(), bloom_data.begin(), bloom_data.end());

    // 5.写footer
    append_fixed(data, meta_offset);
    append_fixed(data, bloom_offset);
    append_fixed(data, min_tranc_id_);
    append_fixed(data, max_tranc_id_);
    // 6.写文件
    FileObj file = FileObj::create_and_write(path, data);

    // auto sst = std::make_shared<SST>(sst_id, std::move(file), block_cache, vlog_);
    //报错：SST类并没有这个构造函数，用已经实现的open就行。

    return SST::open(sst_id, std::move(file), block_cache, vlog_);
}
} // namespace tiny_lsm
