#include "../../include/block/block.h"
#include "../../include/block/block_iterator.h"
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace tiny_lsm {
Block::Block(size_t capacity) : capacity(capacity) {}

std::vector<uint8_t> Block::encode() {
  // TODO Lab 3.1 编码单个类实例形成一段字节数组
  std::vector<uint8_t> buffer;
  uint16_t entry_nums = static_cast<uint16_t>(offsets.size());
  //size_t total_size = data.size() + entry_nums * 2 + 2; 
  size_t total_size=data.size()+entry_nums*sizeof(uint16_t)+sizeof(uint16_t); //用sizeof才是严谨的写法
  buffer.reserve(total_size);

  // Data Section
  buffer.insert(buffer.end(), data.begin(), data.end());

  // Offset Section
  for (uint16_t offset : offsets) {
    buffer.push_back(static_cast<uint8_t>(offset & 0xFF));
    buffer.push_back(static_cast<uint8_t>((offset >> 8) & 0xFF));
  }
  // Num of elements
  buffer.push_back(static_cast<uint8_t>(entry_nums & 0xFF));
  buffer.push_back(static_cast<uint8_t>((entry_nums >> 8) & 0xFF));
  return buffer;
}

std::shared_ptr<Block> Block::decode(const std::vector<uint8_t> &encoded,
                                     bool with_hash) {
  // TODO Lab 3.1 解码字节数组形成类实例
  size_t size = encoded.size();
  if (size < (with_hash ? 6 : 2)) {
    throw std::runtime_error("encoded block is too small");
  }

  // with_hash=true 时，末尾 4B 为 hash
  size_t end_pos = with_hash ? size - 4 : size;
  if (end_pos < 2) {
    throw std::runtime_error("invalid encoded block tail");
  }

  // 从最后两字节获得实体数量（小端）
  uint16_t entry_nums =
      encoded[end_pos - 2] | (static_cast<uint16_t>(encoded[end_pos - 1]) << 8);
  size_t offset_size = entry_nums * 2;

  if (end_pos < 2 + offset_size) {
    throw std::runtime_error("invalid encoded block offsets");
  }

  size_t data_end = end_pos - 2 - offset_size;
  if (entry_nums > 0 && data_end == 0) {
    throw std::runtime_error("invalid encoded block data");
  }

  // 解码 Data Section
  auto block = std::make_shared<Block>(size);
  block->data.assign(encoded.begin(), encoded.begin() + data_end);

  // 解码 Offset Section
  block->offsets.reserve(entry_nums);
  for (size_t i = 0; i < entry_nums; ++i) {
    size_t pos = data_end + i * 2;
    if (pos + 1 >= end_pos - 2) {
      throw std::runtime_error("invalid encoded block offset position");
    }

    uint16_t offset =
        encoded[pos] | (static_cast<uint16_t>(encoded[pos + 1]) << 8);
    if (offset >= data_end) {
      throw std::runtime_error("invalid encoded block offset value");
    }
    if (!block->offsets.empty() && offset < block->offsets.back()) {
      throw std::runtime_error("encoded block offsets are not sorted");
    }
    block->offsets.push_back(offset);
  }

  return block;
}

std::string Block::get_first_key() {
  if (data.empty() || offsets.empty()) {
    return "";
  }
  return get_key_at(offsets[0]);
}

size_t Block::get_offset_at(size_t idx) const {
  if (idx >= offsets.size()) {
    throw std::runtime_error("idx out of offsets range");
  }
  return offsets[idx];
}

bool Block::add_entry(const std::string &key, const std::string &value,
                      uint64_t tranc_id, bool force_write) {
  // TODO Lab 3.1 添加一个键值对到block中
  // ? 返回值说明：
  // ? true: 成功添加
  // ? false: block已满, 拒绝此次添加
  size_t entry_size = sizeof(uint16_t) + key.size() + sizeof(uint16_t) + value.size() + sizeof(uint64_t);

  size_t estimate_size =
      data.size() + (offsets.size() + 1) * sizeof(uint16_t) + entry_size + 2;
  if (!force_write && estimate_size > capacity && !is_empty()) {
    return false;
  }

  uint16_t current_offset = static_cast<uint16_t>(data.size());
  offsets.push_back(current_offset);

  uint16_t key_len = static_cast<uint16_t>(key.size());
  data.push_back(static_cast<uint8_t>(key_len & 0xFF));
  data.push_back(static_cast<uint8_t>((key_len >> 8) & 0xFF));
  data.insert(data.end(), key.begin(), key.end());

  uint16_t value_len = static_cast<uint16_t>(value.size());
  data.push_back(static_cast<uint8_t>(value_len & 0xFF));
  data.push_back(static_cast<uint8_t>((value_len >> 8) & 0xFF));
  data.insert(data.end(), value.begin(), value.end());

  for (int i = 0; i < 8; i++) {
    data.push_back(static_cast<uint8_t>((tranc_id >> (i * 8)) & 0xFF));
  }
  return true;
}

std::string Block::get_key_at(size_t offset) const {
  // TODO Lab 3.1 从指定偏移量获取entry的key
  if (offset + 1 >= data.size()) {
    throw std::runtime_error("invalid entry offset for key");
  }

  uint16_t key_len =
      data[offset] | (static_cast<uint16_t>(data[offset + 1]) << 8);
  size_t key_pos = offset + 2;
  if (key_pos + key_len > data.size()) {
    throw std::runtime_error("invalid key length in block");
  }

  return std::string(reinterpret_cast<const char *>(data.data() + key_pos),
                     key_len);
}

std::string Block::get_value_at(size_t offset) const {
  // TODO Lab 3.1 从指定偏移量获取entry的value
  if (offset + 1 >= data.size()) {
    throw std::runtime_error("invalid entry offset for value");
  }

  uint16_t key_len =
      data[offset] | (static_cast<uint16_t>(data[offset + 1]) << 8);
  size_t value_len_pos = offset + 2 + key_len;
  if (value_len_pos + 1 >= data.size()) {
    throw std::runtime_error("invalid value length position in block");
  }

  uint16_t value_len = data[value_len_pos] |
                       (static_cast<uint16_t>(data[value_len_pos + 1]) << 8);
  if (value_len_pos + 2 + value_len > data.size()) {
    throw std::runtime_error("invalid value length in block");
  }

  return std::string(
      reinterpret_cast<const char *>(data.data() + value_len_pos + 2),
      value_len);
}

uint64_t Block::get_tranc_id_at(size_t offset) const {
  // TODO Lab 3.1 从指定偏移量获取entry的tranc_id
  // ? 你不需要理解tranc_id的具体含义, 直接返回即可
  if (offset + 1 >= data.size()) {
    throw std::runtime_error("invalid entry offset for tranc_id");
  }

  uint16_t key_len =
      data[offset] | (static_cast<uint16_t>(data[offset + 1]) << 8);
  size_t value_len_pos = offset + 2 + key_len;
  if (value_len_pos + 1 >= data.size()) {
    throw std::runtime_error("invalid tranc_id length position in block");
  }

  uint16_t value_len = data[value_len_pos] |
                       (static_cast<uint16_t>(data[value_len_pos + 1]) << 8);
  size_t tranc_id_pos = value_len_pos + 2 + value_len;
  if (tranc_id_pos + 7 >= data.size()) {
    throw std::runtime_error("invalid tranc_id position in block");
  }

  uint64_t tranc_id = 0;
  for (int i = 0; i < 8; ++i) {
    tranc_id |= (static_cast<uint64_t>(data[tranc_id_pos + i]) << (i * 8));
  }
  return tranc_id;
}

int Block::compare_key_at(size_t offset, const std::string &target) const {
  // 写了get_entry_at之后，调用get_key_at会解析value
  // 而每次二分只用解析key，所以单独处理一下
  if (offset + 1 >= data.size()) {
    throw std::runtime_error("invalid entry offset for key compare");
  }

  uint16_t key_len =
      data[offset] | (static_cast<uint16_t>(data[offset + 1]) << 8);
  size_t key_pos = offset + 2;
  if (key_pos + key_len > data.size()) {
    throw std::runtime_error("invalid key length for key compare");
  }

  std::string_view key(reinterpret_cast<const char *>(data.data() + key_pos),
                       key_len);
  return key.compare(target);
}

int Block::adjust_idx_by_tranc_id(size_t idx, uint64_t tranc_id) {
  // TODO Lab3.1 不需要在Lab3.1中完整实现, 只是进行标记
  // ? 相同key连续分布, 且相同key的事务id从大到小排布
  // ? 这里的逻辑是找到最接近 tranc_id 的键值对的索引位置
  if (idx >= offsets.size()) {
    return -1;
  }
  if (tranc_id == 0) {
    return static_cast<int>(idx);
  }

  std::string target_key = get_key_at(offsets[idx]);
  for (size_t i = idx;
       i < offsets.size() && get_key_at(offsets[i]) == target_key; ++i) {
    if (get_tranc_id_at(offsets[i]) <= tranc_id) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

bool Block::is_same_key(size_t idx, const std::string &target_key) const {
  if (idx >= offsets.size()) {
    return false;
  }
  return get_key_at(offsets[idx]) == target_key;
}

std::optional<std::string> Block::get_value_binary(const std::string &key,
                                                   uint64_t tranc_id) {
  auto idx_opt = get_idx_binary(key, tranc_id);
  if (!idx_opt.has_value()) {
    return std::nullopt;
  }

  size_t idx = idx_opt.value();
  return get_value_at(get_offset_at(idx));
}

std::optional<size_t> Block::get_idx_binary(const std::string &key,
                                            uint64_t tranc_id) {
  // TODO Lab 3.1 使用二分查找获取key对应的索引
  if (offsets.empty()) {
    return std::nullopt;
  }

  size_t left = 0;
  size_t right = offsets.size();
  while (left < right) {
    size_t mid = left + (right - left) / 2;
    size_t offset = get_offset_at(mid);
    if (compare_key_at(offset, key) < 0) {
      left = mid + 1;
    } else {
      right = mid;
    }
  }

  if (left >= offsets.size() || !is_same_key(left, key)) {
    return std::nullopt;
  }

  int adjusted = adjust_idx_by_tranc_id(left, tranc_id);
  if (adjusted < 0) {
    return std::nullopt;
  }
  return static_cast<size_t>(adjusted);
}

std::optional<
    std::pair<std::shared_ptr<BlockIterator>, std::shared_ptr<BlockIterator>>>
Block::iters_preffix(uint64_t tranc_id, const std::string &preffix) {
  // TODO Lab 3.3 获取前缀匹配的区间迭代器
  return get_monotony_predicate_iters(
      tranc_id, [&preffix](const std::string &key) -> int {
        if (key.compare(0, preffix.size(), preffix) == 0) {
          return 0;
        }
        return key < preffix ? 1 : -1;
      });
}

std::optional<
    std::pair<std::shared_ptr<BlockIterator>, std::shared_ptr<BlockIterator>>>
Block::get_monotony_predicate_iters(
    uint64_t tranc_id, std::function<int(const std::string &)> predicate) {
  // TODO: Lab 3.3 使用二分查找获取满足谓词的区间迭代器
  // 这里先采用线性扫描实现正确性，后续可替换为二分优化
  BlockIterator it = begin(tranc_id);
  BlockIterator end_it = end();

  while (it != end_it && predicate(it->first) > 0) {
    ++it;
  }
  if (it == end_it || predicate(it->first) != 0) {
    return std::nullopt;
  }

  auto begin_ptr = std::make_shared<BlockIterator>(it);
  while (it != end_it && predicate(it->first) == 0) {
    ++it;
  }
  auto end_ptr = std::make_shared<BlockIterator>(it);
  return std::make_pair(begin_ptr, end_ptr);
}

Block::Entry Block::get_entry_at(size_t offset) const {
  Entry entry;
  entry.key = get_key_at(offset);
  entry.value = get_value_at(offset);
  entry.tranc_id = get_tranc_id_at(offset);
  return entry;
}

size_t Block::size() const { return offsets.size(); }

size_t Block::cur_size() const {
  return data.size() + offsets.size() * sizeof(uint16_t) + sizeof(uint16_t);
}

bool Block::is_empty() const { return offsets.empty(); }

BlockIterator Block::begin(uint64_t tranc_id) {
  // TODO Lab 3.2 获取begin迭代器
  std::shared_ptr<Block> self;
  try {
    self = shared_from_this();
  } catch (const std::bad_weak_ptr &) {
    self = std::shared_ptr<Block>(this, [](Block *) {});
  }
  return BlockIterator(self, 0, tranc_id);
}

BlockIterator Block::end() {
  // TODO Lab 3.2 获取end迭代器
  std::shared_ptr<Block> self;
  try {
    self = shared_from_this();
  } catch (const std::bad_weak_ptr &) {
    self = std::shared_ptr<Block>(this, [](Block *) {});
  }
  return BlockIterator(self, offsets.size(), 0);
}
} // namespace tiny_lsm
