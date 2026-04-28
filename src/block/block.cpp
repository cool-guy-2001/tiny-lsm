#include "block/block.h"
#include "block/block_iterator.h"
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

namespace tiny_lsm {
Block::Block(size_t capacity) : capacity(capacity) {}

uint16_t read_u16t(const std::vector<uint8_t>& buf, size_t pos) {
    if (pos + sizeof(uint16_t) > buf.size()) {
        throw std::runtime_error("read_u16t out of range");
    }
    return static_cast<uint16_t>(buf[pos]) | (static_cast<uint16_t>(buf[pos + 1]) << 8);
}

uint64_t read_u64t(const std::vector<uint8_t>& buf, size_t pos) {
    if (pos + sizeof(uint64_t) > buf.size()) {
        throw std::runtime_error("readu164_t out of range");
    }
    uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        value |= static_cast<uint64_t>(buf[pos + i]) << (8 * i);
    }
    return value;
}

std::vector<uint8_t> Block::encode(bool with_hash) {
    // TODO: Lab 3.1 编码单个类实例形成一段字节数组
    // ? 格式: [data段] + [offsets数组, 每项uint16_t] + [元素个数 uint16_t]
    // ? 若 with_hash == true, 末尾额外追加 uint32_t 的 CRC 校验值
    // ? CRC 覆盖除自身之外的所有字节
    std::vector<uint8_t> encoded;
    size_t total_size = data.size() + offsets.size() * sizeof(uint16_t) + sizeof(uint16_t);
    if (with_hash) {
        total_size += sizeof(uint32_t);
    }
    encoded.reserve(total_size);
    // 1.写入data section
    encoded.insert(encoded.end(), data.begin(), data.end());

    // 2.写入offsets section
    for (uint16_t offset : offsets) {
        encoded.push_back(static_cast<uint8_t>(offset & 0xFF));
        encoded.push_back(static_cast<uint8_t>((offset >> 8) & 0xFF));
    }
    // 4. 写入 Entry 数量
    if (offsets.size() > std::numeric_limits<uint16_t>::max()) {
        throw std::runtime_error("too many entries in block");
    }

    uint16_t num_of_entries = static_cast<uint16_t>(offsets.size());
    encoded.push_back(static_cast<uint8_t>(num_of_entries & 0xFF));
    encoded.push_back(static_cast<uint8_t>((num_of_entries >> 8) & 0xFF));

    if (with_hash) {
        /*
        把 encoded 这段二进制字节内容当作一段 string_view，
        用 std::hash<std::string_view> 计算哈希值，
        然后截断/转换成 uint32_t。
        */
        uint32_t hash_value = static_cast<uint32_t>(std::hash<std::string_view>{}(
            std::string_view(reinterpret_cast<const char*>(encoded.data()), encoded.size())));
        // TODO:等价写法
        /*
        const char* bytes = reinterpret_cast<const char*>(encoded.data());

        std::string_view view(bytes, encoded.size());

        std::hash<std::string_view> hasher;

        std::size_t full_hash = hasher(view);

        uint32_t hash_value = static_cast<uint32_t>(full_hash);
        */
        encoded.push_back(static_cast<uint8_t>(hash_value & 0xFF));
        encoded.push_back(static_cast<uint8_t>((hash_value >> 8) & 0xFF));
        encoded.push_back(static_cast<uint8_t>((hash_value >> 16) & 0xFF));
        encoded.push_back(static_cast<uint8_t>((hash_value >> 24) & 0xFF));
    }
    return encoded;
}

std::shared_ptr<Block> Block::decode(const std::vector<uint8_t>& encoded, bool with_hash) {
    // TODO: Lab 3.1 解码字节数组形成类实例
    // ? 从末尾读取元素个数, 若 with_hash 为 true 先校验 CRC
    // ? 然后依次读取 offsets 和 data 段
    size_t encoded_size = encoded.size();

    // 最小长度：
    // 不带 hash：至少 2B entry_num
    // 带 hash：至少 2B entry_num + 4B crc
    if (encoded_size < (with_hash ? sizeof(uint16_t) + sizeof(uint32_t) : sizeof(uint16_t))) {
        throw std::runtime_error("encoded block is too small");
    }
    size_t endpos = (with_hash ? encoded_size - sizeof(uint32_t) : encoded_size);
    uint16_t num_entries = read_u16t(encoded, endpos - sizeof(uint16_t));

    size_t offset_size = num_entries * sizeof(uint16_t);

    if (endpos < offset_size + 2) {
        throw std::runtime_error("invalid encoded block data");
    }
    size_t offsets_end = endpos - sizeof(uint16_t);
    size_t offsets_start = offsets_end - offset_size;
    size_t dataend = offsets_start;

    auto block = std::make_shared<Block>(endpos);
    block->data.assign(encoded.begin(), encoded.begin() + dataend);
    block->offsets.reserve(num_entries);
    for (size_t i = 0; i < num_entries; ++i) {
        size_t pos = offsets_start + i * 2;
        if (pos + sizeof(uint16_t) > offsets_end) {
            throw std::runtime_error("invalid encoded block offset position");
        }
        uint16_t offset = read_u16t(encoded, pos);
        if (offset >= dataend) {
            throw std::runtime_error("invalid offset value");
        }
        if (!block->offsets.empty() && offset <= block->offsets.back()) {
            throw std::runtime_error("encoded block offsets are not sorted");
        }
        block->offsets.push_back(offset);
    }
    if (with_hash) {
        uint32_t stored_hash = static_cast<uint32_t>(encoded[encoded_size - 4]) |
                               (static_cast<uint32_t>(encoded[encoded_size - 3]) << 8) |
                               (static_cast<uint32_t>(encoded[encoded_size - 2]) << 16) |
                               (static_cast<uint32_t>(encoded[encoded_size - 1]) << 24);

        uint32_t actual_hash = static_cast<uint32_t>(std::hash<std::string_view>{}(std::string_view(
            reinterpret_cast<const char*>(encoded.data()), encoded_size - sizeof(uint32_t))));

        if (stored_hash != actual_hash) {
            throw std::runtime_error("block hash mismatch");
        }
    }
    return block;
}

std::string Block::get_first_key() {
    if (data.empty() || offsets.empty()) {
        return "";
    }

    // 读取第一个key的长度（前2字节）
    uint16_t key_len;
    memcpy(&key_len, data.data(), sizeof(uint16_t));

    // 读取key
    std::string key(reinterpret_cast<char*>(data.data() + sizeof(uint16_t)), key_len);
    return key;
}

size_t Block::get_offset_at(size_t idx) const {
    if (idx > offsets.size()) {
        throw std::runtime_error("idx out of offsets range");
    }
    return offsets[idx];
}

bool Block::add_entry(const std::string& key, const std::string& value, uint64_t tranc_id,
                      bool force_write) {
    // TODO: Lab 3.1 添加一个键值对到block中
    // ? 每条 entry 格式: [key_len:uint16_t][key][value_len:uint16_t][value][tranc_id:uint64_t]
    // ? 若 !force_write 且当前容量不足则返回 false
    // ? 成功添加后记录偏移到 offsets, 返回 true
    if (key.size() > std::numeric_limits<uint16_t>::max() ||
        value.size() > std::numeric_limits<uint16_t>::max()) {
        throw std::runtime_error("key or value too large");
    }
    size_t offset = data.size();
    uint16_t key_len = static_cast<uint16_t>(key.size());
    uint16_t value_len = static_cast<uint16_t>(value.size());

    // 当前 entry 编码后的大小
    size_t entry_size =
        sizeof(uint16_t) + key_len + sizeof(uint16_t) + value_len + sizeof(uint64_t);
    size_t new_size = data.size() + (offsets.size() + 1) * sizeof(uint16_t) + sizeof(uint16_t);
    if (!force_write && new_size > capacity) {
        return false;
    }
    offsets.push_back(offset);
    // 1.写入key_len
    data.push_back(key_len & 0xFF);
    data.push_back((key_len >> 8) & 0xFF);
    // 2.写入key
    data.insert(data.end(), key.begin(), key.end());

    data.push_back(value_len & 0xFF);
    data.push_back((value_len >> 8) & 0xFF);

    data.insert(data.end(), value.begin(), value.end());

    for (size_t i = 0; i < 8; ++i) {
        data.push_back(static_cast<uint8_t>((tranc_id >> (i * 8)) & 0xFF));
    }

    return true;
}

// 从指定偏移量获取entry的key
std::string Block::get_key_at(size_t offset) const {
    // TODO: Lab 3.1 从指定偏移量获取entry的key
    // ? 读取 data[offset] 处的 uint16_t key_len, 再取后续 key_len 个字节
    if (offset >= data.size()) {
        throw std::runtime_error("invalid entry offset");
    }
    uint16_t key_len = read_u16t(data, offset); //从data中读取offset,offset+1部分两个字节的数据
    size_t key_start = offset + sizeof(uint16_t);
    if (key_start + key_len > data.size()) {
        throw std::runtime_error("invalid key length");
    }
    return std::string(data.begin() + key_start, data.begin() + key_start + key_len);
}

// 从指定偏移量获取entry的value
std::string Block::get_value_at(size_t offset) const {
    // TODO: Lab 3.1 从指定偏移量获取entry的value
    // ? 先跳过 key_len + key, 再读取 uint16_t value_len, 最后取 value
    if (offset >= data.size()) {
        throw std::runtime_error("invalid entry offset");
    }
    uint16_t key_len = read_u16t(data, offset);
    size_t value_len_pos = offset + sizeof(uint16_t) + key_len;
    uint16_t value_len = read_u16t(data, value_len_pos);
    size_t value_start = value_len_pos + sizeof(uint16_t);
    if (value_start + value_len > data.size()) {
        throw std::runtime_error("invalid value length");
    }

    return std::string(data.begin() + value_start, data.begin() + value_start + value_len);
}

uint64_t Block::get_tranc_id_at(size_t offset) const {
    // TODO: Lab 3.1 从指定偏移量获取entry的tranc_id
    // ? 先跳过 key 和 value, 读取末尾的 uint64_t tranc_id
    uint16_t key_len = read_u16t(data, offset);
    size_t value_len_pos = offset + sizeof(uint16_t) + key_len;
    uint16_t value_len = read_u16t(data, value_len_pos);
    size_t value_start = value_len_pos + sizeof(uint16_t);

    size_t tranc_pos = value_start + value_len;
    uint64_t tranc_id = read_u64t(data, tranc_pos);
    return tranc_id;
}

// 比较指定偏移量处的key与目标key
int Block::compare_key_at(size_t offset, const std::string& target) const {
    std::string key = get_key_at(offset);
    return key.compare(target);
}

// 相同的key连续分布, 且相同的key的事务id从大到小排布
// 这里的逻辑是找到最接近 tranc_id 的键值对的索引位置
int Block::adjust_idx_by_tranc_id(size_t idx, uint64_t tranc_id) {
    // TODO: Lab3.1 不需要在Lab3.1中实现, 只是进行标记
    // ? 后续实现事务后需要更新这里的实现
    // ? tranc_id == 0: 向前找最小索引 (最大事务id) 版本
    // ? tranc_id != 0: 找满足 tranc_id_ <= tranc_id 的最新版本
    if (idx >= offsets.size()) {
        return -1;
    }
    std::string target_key = get_key_at(offsets[idx]);
    if (tranc_id == 0) {
        return static_cast<int>(idx);
    }
    for (size_t i = idx; i < offsets.size(); ++i) {
        if (!is_same_key(i, target_key)) {
            break;
        }
        uint64_t entry_tranc_id = get_tranc_id_at(offsets[i]);
        if (entry_tranc_id <= tranc_id) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

bool Block::is_same_key(size_t idx, const std::string& target_key) const {
    if (idx >= offsets.size()) {
        return false; // 索引超出范围
    }
    return get_key_at(offsets[idx]) == target_key;
}

// 使用二分查找获取value
// 要求在插入数据时有序插入
std::optional<std::string> Block::get_value_binary(const std::string& key, uint64_t tranc_id) {
    auto idx = get_idx_binary(key, tranc_id);
    if (!idx.has_value()) {
        return std::nullopt;
    }

    return get_value_at(offsets[*idx]);
}

std::optional<size_t> Block::get_idx_binary(const std::string& key, uint64_t tranc_id) {
    // TODO: Lab 3.1 使用二分查找获取key对应的索引
    // ? 在 offsets 数组上做二分查找, 利用 compare_key_at 比较
    // ? 找到后调用 adjust_idx_by_tranc_id 进行事务可见性修正
    size_t l = 0, r = offsets.size();
    while (l < r) {
        size_t mid = l + (r - l) / 2;
        int cmp = compare_key_at(offsets[mid], key);
        if (cmp >= 0) {
            r = mid;
        } else if (cmp < 0) {
            l = mid + 1;
        }
    }
    if (l == offsets.size()) {
        return std::nullopt;
    }
    if (!is_same_key(l, key)) {
        return std::nullopt;
    }
    int adjusted_idx = adjust_idx_by_tranc_id(l, tranc_id);
    if (adjusted_idx < 0) {
        return std::nullopt;
    }
    return static_cast<size_t>(adjusted_idx);
}

std::optional<std::pair<std::shared_ptr<BlockIterator>, std::shared_ptr<BlockIterator>>>
Block::iters_preffix(uint64_t tranc_id, const std::string& preffix) {
    // TODO: Lab 3.3 获取前缀匹配的区间迭代器
    // ? 将前缀匹配转化为单调谓词, 调用 get_monotony_predicate_iters
    // ? 谓词: -key.compare(0, preffix.size(), preffix)

    return std::nullopt;
}

// 返回第一个满足谓词的位置和最后一个满足谓词的位置
// 如果不存在, 返回nullopt
// 谓词作用于key, 且保证满足谓词的结果只在一段连续的区间内, 例如前缀匹配的谓词
// 返回的区间是闭区间, 开区间需要手动对返回值自增
// predicate返回值:
//   0: 满足谓词
//   >0: 不满足谓词, 需要向右移动
//   <0: 不满足谓词, 需要向左移动
std::optional<std::pair<std::shared_ptr<BlockIterator>, std::shared_ptr<BlockIterator>>>
Block::get_monotony_predicate_iters(uint64_t tranc_id,
                                    std::function<int(const std::string&)> predicate) {
    // TODO: Lab 3.3 使用二分查找获取满足谓词的区间迭代器
    // ? 第一次二分: 找到 first (满足谓词的最左边索引)
    // ? 第二次二分: 找到 last  (满足谓词的最右边索引)
    // ? 返回 [BlockIterator(first), BlockIterator(last+1)]
    return std::nullopt;
}

Block::Entry Block::get_entry_at(size_t offset) const {
    Entry entry;
    entry.key = get_key_at(offset);
    entry.value = get_value_at(offset);
    entry.tranc_id = get_tranc_id_at(offset);
    return entry;
}

size_t Block::size() const {
    return offsets.size();
}

size_t Block::cur_size() const {
    return data.size() + offsets.size() * sizeof(uint16_t) + sizeof(uint16_t);
}

bool Block::is_empty() const {
    return offsets.empty();
}

BlockIterator Block::begin(uint64_t tranc_id) {
    // TODO: Lab 3.2 获取begin迭代器
    // ? 返回指向第 0 个 entry 的迭代器: BlockIterator(shared_from_this(), 0, tranc_id)
    
    return BlockIterator(nullptr, 0, 0);
}

BlockIterator Block::end() {
    // TODO: Lab 3.2 获取end迭代器
    // ? 返回指向末尾 (offsets.size()) 的迭代器
    return BlockIterator(nullptr, 0, 0);
}
} // namespace tiny_lsm
