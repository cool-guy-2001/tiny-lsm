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
  size_t total_size = data.size() + entry_nums * 2 + 2;
  buffer.reserve(total_size);

  buffer.insert(buffer.end(), data.begin(), data.end());

  for (uint16_t offset : offsets) {
    buffer.push_back(static_cast<uint8_t>(offset & 0xFF));
    buffer.push_back(static_cast<uint8_t>((offset >> 8) & 0xFF));
  }
  buffer.push_back(static_cast<uint8_t>(entry_nums & 0xFF));
  buffer.push_back(static_cast<uint8_t>((entry_nums >> 8) & 0xFF));
  return buffer;
}

std::shared_ptr<Block> Block::decode(const std::vector<uint8_t> &encoded,
                                     bool with_hash) {
  // TODO Lab 3.1 解码字节数组形成类实例
  size_t size = encoded.size();
  if (size < (with_hash ? 6 : 2)) {
    return std::make_shared<Block>();
  }
  size_t end_pos = with_hash ? size - 4 : size;
  //这里需要做一次类型转换
  // uint16_t entry_nums = encoded[end_pos-2]|(encoded[end_pos-1]<<8);
  //从最后两字节获得实体数量
  uint16_t entry_nums =
      encoded[end_pos - 2] | (static_cast<uint16_t>(encoded[end_pos - 1] << 8));

  size_t offset_size = entry_nums * 2;
  size_t data_end = end_pos - 2 - offset_size;
  //解码Data
  auto block = std::make_shared<Block>();
  block->data.assign(encoded.begin(), encoded.begin() + data_end);

  //解码offsets
  block->offsets.reserve(entry_nums);
  for (size_t i = 0; i < entry_nums; ++i) {
    size_t pos = data_end + i * 2;
    uint16_t offset =
        encoded[pos] | (static_cast<uint16_t>(encoded[pos + 1] << 8));
    block->offsets.push_back(offset);
  }
  // printf("Decoded entries: %zu\n", block->offsets.size());
  return block;
}

std::string Block::get_first_key() {
  if (data.empty() || offsets.empty()) {
    return "";
  }

  // // 读取第一个key的长度（前2字节）
  // uint16_t key_len;
  // memcpy(&key_len, data.data(), sizeof(uint16_t));

  // // 读取key
  // std::string key(reinterpret_cast<char *>(data.data() + sizeof(uint16_t)),
  //                 key_len);
  // return key;
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
  //新加入的实体entry序列化后的大小
  size_t entry_size = 2 + key.size() + 2 + value.size() + 8;

  //预估加入新entry后的block大小
  size_t estimate_size =
      data.size() + (offsets.size() + 1) * 2 + entry_size + 2;
  //容量检查
  if (!force_write && estimate_size > capacity && !is_empty())
    return false;
  //记录当前新entry的偏移量
  uint16_t current_offset = static_cast<uint16_t>(data.size());
  offsets.push_back(current_offset);
  uint16_t key_len = static_cast<uint16_t>(key.size());
  data.push_back(static_cast<uint8_t>(key_len & 0xFF));
  data.push_back(static_cast<uint8_t>((key_len >> 8) & 0xFF));
  //插入key
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

// 从指定偏移量获取entry的key
std::string Block::get_key_at(size_t offset) const {
  // TODO Lab 3.1 从指定偏移量获取entry的key
  uint16_t key_len = data[offset] | (data[offset + 1] << 8);
  return std::string(reinterpret_cast<const char *>(&data[offset + 2]),
                     key_len);
}

// 从指定偏移量获取entry的value
std::string Block::get_value_at(size_t offset) const {
  // TODO Lab 3.1 从指定偏移量获取entry的value
  uint16_t key_len = data[offset] | (data[offset + 1] << 8);
  size_t value_len_pos = offset + 2 + key_len;
  uint16_t value_len = data[value_len_pos] | (data[value_len_pos + 1] << 8);
  return std::string(reinterpret_cast<const char *>(data[value_len_pos + 2]),
                     value_len);
}

uint64_t Block::get_tranc_id_at(size_t offset) const {
  // TODO Lab 3.1 从指定偏移量获取entry的tranc_id
  // ? 你不需要理解tranc_id的具体含义, 直接返回即可
  uint16_t key_len = data[offset] | (data[offset + 1] << 8);
  size_t value_len_pos = offset + 2 + key_len;
  uint16_t value_len = data[value_len_pos] | (data[value_len_pos + 1] << 8);
  size_t tranc_id_pos = value_len_pos + 2 + value_len;
  uint64_t tranc_id = 0;
  for (int i = 0; i < 8; ++i) {
    tranc_id |= (static_cast<uint64_t>(data[tranc_id_pos + i]) << (i * 8));
  }
  return tranc_id;
}

// 比较指定偏移量处的key与目标key
int Block::compare_key_at(size_t offset, const std::string &target) const {
  //写了get_entry_at之后，调用get_key_at会解析value,而每次二分只用解析key,所以单独处理一下
  uint16_t key_len = data[offset] | (data[offset + 1] << 8);
  std::string_view key(reinterpret_cast<const char *>(data.data() + offset + 2),
                       key_len);
  //代替reinterpret_cast<const char
  //*>(&data[offset+2]),更具可读性，且直接使用底层指针
  //用string_view 避免内存拷贝

  return key.compare(target);
}

// 相同的key连续分布, 且相同的key的事务id从大到小排布
// 这里的逻辑是找到最接近 tranc_id 的键值对的索引位置
int Block::adjust_idx_by_tranc_id(size_t idx, uint64_t tranc_id) {
  // TODO Lab3.1 不需要在Lab3.1中实现, 只是进行标记,
  // ? 后续实现事务后需要更新这里的实现
  return -1;
}

bool Block::is_same_key(size_t idx, const std::string &target_key) const {
  if (idx >= offsets.size()) {
    return false; // 索引超出范围
  }
  return get_key_at(offsets[idx]) == target_key;
}

// 使用二分查找获取value
// 要求在插入数据时有序插入
std::optional<std::string> Block::get_value_binary(const std::string &key,
                                                   uint64_t tranc_id) {
  // auto idx = get_idx_binary(key, tranc_id);
  // if (!idx.has_value()) {
  //   return std::nullopt;
  // }

  // return get_value_at(offsets[*idx]);
  auto idx_opt = get_idx_binary(key, tranc_id);
  if (!idx_opt.has_value()) {
    return std::nullopt;
  }
  size_t idx = idx_opt.value();
  size_t offset = get_offset_at(idx);
  if (get_key_at(offset) == key) {
    return get_value_at(offset);
  }
  return std::nullopt;
}

std::optional<size_t> Block::get_idx_binary(const std::string &key,
                                            uint64_t tranc_id) {
  // TODO Lab 3.1 使用二分查找获取key对应的索引
  if (offsets.empty())
    return std::nullopt;
  size_t left = 0;
  size_t right = offsets.size();
  while (left < right) {
    size_t mid = left + (right - left) / 2;
    //查找第mid个entry的偏移量
    size_t offset = get_offset_at(mid);
    std::string mid_key = get_key_at(offset);
    if (mid_key < key) {
      left = mid + 1;
    } else {
      right = mid;
    }
  }
  if (left >= offsets.size()) {
    return std::nullopt;
  }
  return left;
}

std::optional<
    std::pair<std::shared_ptr<BlockIterator>, std::shared_ptr<BlockIterator>>>
Block::iters_preffix(uint64_t tranc_id, const std::string &preffix) {
  // TODO Lab 3.3 获取前缀匹配的区间迭代器
  return std::nullopt;
}

// 返回第一个满足谓词的位置和最后一个满足谓词的位置
// 如果不存在, 范围nullptr
// 谓词作用于key, 且保证满足谓词的结果只在一段连续的区间内, 例如前缀匹配的谓词
// 返回的区间是闭区间, 开区间需要手动对返回值自增
// predicate返回值:
//   0: 满足谓词
//   >0: 不满足谓词, 需要向右移动
//   <0: 不满足谓词, 需要向左移动
std::optional<
    std::pair<std::shared_ptr<BlockIterator>, std::shared_ptr<BlockIterator>>>
Block::get_monotony_predicate_iters(
    uint64_t tranc_id, std::function<int(const std::string &)> predicate) {
  // TODO: Lab 3.3 使用二分查找获取满足谓词的区间迭代器
  return std::nullopt;
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
  return BlockIterator(nullptr, 0, 0);
}

BlockIterator Block::end() {
  // TODO Lab 3.2 获取end迭代器
  return BlockIterator(nullptr, 0, 0);
}
} // namespace tiny_lsm