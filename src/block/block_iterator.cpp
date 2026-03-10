#include "../../include/block/block_iterator.h"
#include "../../include/block/block.h"
#include <cstdint>
#include <memory>
#include <stdexcept>

class Block;

namespace tiny_lsm {
BlockIterator::BlockIterator(std::shared_ptr<Block> b, size_t index,
                             uint64_t tranc_id)
    : block(b), current_index(index), tranc_id_(tranc_id),
      cached_value(std::nullopt) {
  skip_by_tranc_id();
}

BlockIterator::BlockIterator(std::shared_ptr<Block> b, const std::string &key,
                             uint64_t tranc_id)
    : block(b), tranc_id_(tranc_id), cached_value(std::nullopt) {
  // TODO: Lab3.2 创建迭代器时直接移动到指定的key位置
  // ? 你需要借助之前实现的 Block 类的成员函数
  if (!block) {
    return;
  }
  auto idx_opt = block->get_idx_binary(key, tranc_id);
  if (idx_opt.has_value()) {
    this->current_index = idx_opt.value();
  } else {
    this->current_index = block->size();
  }
}

// BlockIterator::BlockIterator(std::shared_ptr<Block> b, uint64_t tranc_id)
//     : block(b), current_index(0), tranc_id_(tranc_id),
//       cached_value(std::nullopt) {
//   skip_by_tranc_id();
// }

BlockIterator::pointer
BlockIterator::operator->() const { // using pointer = const value_type *;
  // TODO: Lab3.2 -> 重载
  update_current();
  if (!cached_value.has_value()) {
    return nullptr;
  }
  return &(*cached_value); //先解析cached_value里的对象，再取地址返回
}

BlockIterator &BlockIterator::operator++() {
  // TODO: Lab3.2 ++ 重载
  // ? 在后续的Lab实现事务后，你可能需要对这个函数进行返修
  current_index++;
  cached_value = std::nullopt;

  return *this;
}

bool BlockIterator::operator==(const BlockIterator &other) const {
  // TODO: Lab3.2 == 重载

  return this->block==other.block && this->current_index==other.current_index;
}

bool BlockIterator::operator!=(const BlockIterator &other) const {
  // TODO: Lab3.2 != 重载
  return !(*this==other);
}

BlockIterator::value_type BlockIterator::operator*() const { //返回迭代器当前指向的值
  // TODO: Lab3.2 * 重载
  update_current();
  return *cached_value;
}

bool BlockIterator::is_end() { return current_index == block->offsets.size(); }

void BlockIterator::update_current() const { //用来更新缓存的键值对变量
  // TODO: Lab3.2 更新当前指针
  // ? 该函数是可选的实现, 你可以采用自己的其他方案实现->, 而不是使用
  // ? cached_value 来缓存当前指针
  //如果已经有了缓存值，就不用解析
  if (cached_value.has_value()) {
    return;
  }
  //边界检查
  if (!block || current_index >= block->offsets.size()) {
    cached_value = std::nullopt;
    return;
  }
  auto offset = block->offsets[current_index];
  cached_value =
      std::make_pair(block->get_key_at(offset), block->get_value_at(offset));
}

void BlockIterator::skip_by_tranc_id() {
  // TODO: Lab3.2 * 跳过事务ID
  // ? 只是进行标记以供你在后续Lab实现事务功能后修改
  // ? 现在你不需要考虑这个函数
  
}
} // namespace tiny_lsm