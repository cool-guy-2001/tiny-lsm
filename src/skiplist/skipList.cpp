#include "../../include/skiplist/skiplist.h"
#include "iterator/iterator.h"
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <spdlog/spdlog.h>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace tiny_lsm {

// ************************ SkipListIterator ************************

// SkipListIterator就是"跳表版本"的迭代器，把"怎么从一个SkipListNode走到下一个节点"封装起来

// BaseIterator里将这些函数定义为虚函数,查看iterator.h
BaseIterator &SkipListIterator::operator++() {
  // TODO: Lab1.2 任务：实现SkipListIterator的++操作符
  if (is_end())
    return *this;
  current = current->forward_[0];
  return *this;
}

bool SkipListIterator::operator==(const BaseIterator &other) const {

  // TODO: Lab1.2 任务：实现SkipListIterator的==操作符

  // other是基类BaseIterator的引用，没有current,只有把它转回子类SkipListIterator才能访问成员current
  // c++中基类引用->子类引用的安全方式是:dynamic_cast
  auto p = dynamic_cast<const SkipListIterator *>(&other);
  // p指向other本身，只不过类型从BaseIterator*变成了SkipListIterator*
  if (p ==
      nullptr) // cast失败，ohter是别的子类对象，如(SstIterator),但用SkipListIterator去cast它
    return false;
  return this->current == p->current;
}

bool SkipListIterator::operator!=(const BaseIterator &other) const {
  // TODO: Lab1.2 任务：实现SkipListIterator的!=操作符
  return !(*this == other);
}

SkipListIterator::value_type SkipListIterator::operator*() const {
  // TODO: Lab1.2 任务：实现SkipListIterator的*操作符
  if (is_valid()) {
    return {current->key_, current->value_};
  }
  return {"", ""};
}

IteratorType SkipListIterator::get_type() const {
  // TODO: Lab1.2 任务：实现SkipListIterator的get_type
  // ? 主要是为了熟悉基类的定义和继承关系
  return IteratorType::SkipListIterator;
}

bool SkipListIterator::is_valid() const {
  // return current && !current->key_.empty();
  // 如果是空key的话，key_.empty()会出错
  return current != nullptr;
}
bool SkipListIterator::is_end() const { return current == nullptr; }

std::string SkipListIterator::get_key() const {
  // return current->key_; 这种情况如果current为nullptr则会SE
  if (current != nullptr) {
    return current->key_;
  }
  return "";
}
std::string SkipListIterator::get_value() const {
  // return current->value_
  if (current != nullptr) {
    return current->value_;
  }
  return "";
}
uint64_t SkipListIterator::get_tranc_id() const { return current->tranc_id_; }

// ************************ SkipList ************************
// 构造函数
SkipList::SkipList(int max_lvl) : max_level(max_lvl), current_level(1) {
  head = std::make_shared<SkipListNode>("", "", max_level, 0);
  dis_01 = std::uniform_int_distribution<>(0, 1);
  dis_level = std::uniform_int_distribution<>(0, (1 << max_lvl) - 1);
  gen = std::mt19937(std::random_device()());
}

int SkipList::random_level() {
  // ? 通过"抛硬币"的方式随机生成层数：
  // ? - 每次有50%的概率增加一层
  // ? - 确保层数分布为：第1层100%，第2层50%，第3层25%，以此类推
  // ? - 层数范围限制在[1, max_level]之间，避免浪费内存
  // TODO: Lab1.1 任务：插入时随机为这一次操作确定其最高连接的链表层数
  int level = 1;
  while (level < max_level && dis_01(gen) != 0) {
    level++;
  }
  return level;
}

// 插入或更新键值对
void SkipList::put(const std::string &key, const std::string &value,
                   uint64_t tranc_id) {
  spdlog::trace("SkipList--put({}, {}, {})", key, value, tranc_id);

  // TODO: Lab1.1  任务：实现插入或更新键值对
  // ? Hint: 你需要保证不同`Level`的步长从底层到高层逐渐增加
  // ? 你可能需要使用到`random_level`函数以确定层数, 其注释中为你提供一种思路
  // ? tranc_id 为事务id, 现在你不需要关注它, 直接将其传递到
  // SkipListNode的构造函数中即可

  std::vector<std::shared_ptr<SkipListNode>> pre(
      max_level); //每层插入位置的前驱节点
  std::shared_ptr<SkipListNode> p;
  p = head;
  for (int i = current_level - 1; i >= 0; i--) {
    while (p->forward_[i] != nullptr && p->forward_[i]->key_ < key) {
      p = p->forward_[i];
    }
    pre[i] = p;
  }
  p = p->forward_[0];
  if (p != nullptr && p->key_ == key) {
    size_bytes-=p->value_.size();
    p->value_ = value;
    size_bytes+=p->value_.size();
  } else {
    int new_level = random_level();
    std::shared_ptr<SkipListNode> new_node;
    new_node = std::make_shared<SkipListNode>(key, value, new_level, tranc_id);
    size_bytes += new_node->key_.size() + new_node->value_.size() + sizeof(uint64_t);

    if (new_level > current_level) {
      for (int i = current_level; i < new_level; i++) {
        pre[i] = head;
      }
      current_level = new_level;
    }
    for (int i = 0; i < new_level; i++) {
      new_node->forward_[i] = pre[i]->forward_[i];
      pre[i]->forward_[i] = new_node;
    }
  }
}

// 查找键值对
SkipListIterator SkipList::get(const std::string &key, uint64_t tranc_id) {
  // spdlog::trace("SkipList--get({}) called", key);
  // ? 你可以参照上面的注释完成日志输出以便于调试
  // ? 日志为输出到你执行二进制所在目录下的log文件夹

  // TODO: Lab1.1 任务：实现查找键值对,
  // TODO: 并且你后续需要额外实现SkipListIterator中的TODO部分(Lab1.2)

  std::shared_ptr<SkipListNode> current = head;

  for (int i = current_level - 1; i >= 0; i--) {
    while (current->forward_[i] != nullptr &&
           current->forward_[i]->key_ < key) {
      current = current->forward_[i];
    }
  }
  current = current->forward_[0];
  if (current != nullptr && current->key_ == key) {
    return SkipListIterator(current);
  }

  return SkipListIterator{};
}

// 删除键值对
// ! 这里的 remove 是跳表本身真实的 remove,  lsm 应该使用 put 空值表示删除,
// ! 这里只是为了实现完整的 SkipList 不会真正被上层调用
void SkipList::remove(const std::string &key) {
  // TODO: Lab1.1 任务：实现删除键值对
  auto p = head;
  std::vector<std::shared_ptr<SkipListNode>> pre(max_level);
  for (int i = current_level - 1; i >= 0; i--) {
    while (p->forward_[i] != nullptr && p->forward_[i]->key_ < key) {
      p = p->forward_[i];
    }
    pre[i] = p;
  }
  auto target = pre[0]->forward_[0];
  if (target != nullptr && target->key_ == key) { //找到了待删除节点位置
    size_bytes -= target->key_.size() + target->value_.size() + sizeof(uint64_t);
    for (int i = 0; i < current_level; i++) {
      if (pre[i]->forward_[i] == target) {
        pre[i]->forward_[i] = target->forward_[i];
      } else {
        break;
      }
    }
  }
  while (current_level > 1 && head->forward_[current_level - 1] == nullptr) {
    current_level--;
  }
}

// 刷盘时可以直接遍历最底层链表
std::vector<std::tuple<std::string, std::string, uint64_t>> SkipList::flush() {
  // std::shared_lock<std::shared_mutex> slock(rw_mutex);
  spdlog::debug("SkipList--flush(): Starting to flush skiplist data");
  std::vector<std::tuple<std::string, std::string, uint64_t>> data;
  auto node = head->forward_[0];
  while (node) {
    data.emplace_back(node->key_, node->value_, node->tranc_id_);
    node = node->forward_[0];
  }

  spdlog::debug("SkipList--flush(): Flushed {} entries", data.size());

  return data;
}

size_t SkipList::get_size() {
  // std::shared_lock<std::shared_mutex> slock(rw_mutex);

  return size_bytes;
}

// 清空跳表，释放内存
void SkipList::clear() {
  // std::unique_lock<std::shared_mutex> lock(rw_mutex);
  head = std::make_shared<SkipListNode>("", "", max_level, 0);
  size_bytes = 0;
}

SkipListIterator SkipList::begin() {
  // return SkipListIterator(head->forward[0], rw_mutex);
  return SkipListIterator(head->forward_[0]);
}

SkipListIterator SkipList::end() {
  return SkipListIterator(); // 使用空构造函数
}

// 找到前缀的起始位置
// 返回第一个前缀匹配或者大于前缀的迭代器
SkipListIterator SkipList::begin_preffix(const std::string &preffix) {
  // TODO: Lab1.3 任务：实现前缀查询的起始位置
  //找>=preffix的第一个位置
  auto current = head;
  for (int i = current_level - 1; i >= 0; i--) {
    while (current->forward_[i] != nullptr &&
           current->forward_[i]->key_ < preffix) {
      current = current->forward_[i];
    }
  }
  return SkipListIterator{current->forward_[0]}; /*  */
}

// 找到前缀的终结位置(第一个不再满足前缀匹配的迭代器)
SkipListIterator SkipList::end_preffix(const std::string &preffix) {
  // TODO: Lab1.3 任务：实现前缀查询的终结位置
  auto it = begin_preffix(preffix);
  auto check = [&](const std::string &key) -> bool {
    if (key.size() < preffix.size())
      return false;
    return key.compare(0, preffix.size(), preffix) == 0;
  };

  while (it.is_valid() && check(it.get_key()))
    ++it;

  return it;
}

// ? 这里单调谓词的含义是, 整个数据库只会有一段连续区间满足此谓词
// ? 例如之前特化的前缀查询，以及后续可能的范围查询，都可以转化为谓词查询
// ? 返回第一个满足谓词的位置和最后一个满足谓词的迭代器
// ? 如果不存在, 范围nullptr
// ? 谓词作用于key, 且保证满足谓词的结果只在一段连续的区间内, 例如前缀匹配的谓词
// ? predicate返回值:
// ?   0: 满足谓词
// ?   >0: 不满足谓词, 需要向右移动
// ?   <0: 不满足谓词, 需要向左移动
// ! Skiplist 中的谓词查询不会进行事务id的判断, 需要上层自己进行判断
// std::optional,c++ 17中的“可空返回值”

std::optional<std::pair<SkipListIterator, SkipListIterator>>
SkipList::iters_monotony_predicate(
    std::function<int(const std::string &)> predicate) {
  // TODO: Lab1.3 任务：实现谓词查询的起始位置
  auto current = head;

  //打印当前跳表状态
  // print_skiplist();

  for (int i = current_level - 1; i >= 0; i--) {
    while (current->forward_[i] != nullptr &&
           predicate(current->forward_[i]->key_) >
               0) { //按照test_skiplist.cpp中predicate的逻辑
      current = current->forward_[i];
    }
  }
  auto node0 = current->forward_[0];
  if (node0 == nullptr || predicate(node0->key_) != 0)
    return std::nullopt;

  // std::cout<<"Fount start node:"<<node0->key_<<std::endl;

  auto left = node0;
  while (left != nullptr && predicate(left->key_) == 0) {
    left = left->backward_[0].lock();
  }
  // if(left!=nullptr){
  //   std::cout<<"Found node2:"<<left->key_<<std::endl;
  // }else{
  //   std::cout<<"node2 is nullptr"<<std::endl;
  // }
  auto right = node0;
  while (right != nullptr && predicate(right->key_) == 0) {
    right = right->forward_[0];
  }

  auto begin_it = SkipListIterator(left->forward_[0]);
  auto end_it = SkipListIterator(right);
  return std::make_pair(begin_it, end_it);
}

// ? 打印跳表, 你可以在出错时调用此函数进行调试
void SkipList::print_skiplist() {
  for (int level = 0; level < current_level; level++) {
    std::cout << "Level " << level << ": ";
    auto current = head->forward_[level];
    while (current) {
      std::cout << current->key_;
      current = current->forward_[level];
      if (current) {
        std::cout << " -> ";
      }
    }
    std::cout << std::endl;
  }
  std::cout << std::endl;
}
} // namespace tiny_lsm