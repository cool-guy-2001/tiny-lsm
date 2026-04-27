# Lab 1: SkipList 实现

## 问题 1：execv failed(-1)
- **描述**：在运行 `xmake run test_skiplist` 时出现 `execv failed(-1)` 错误,未维护backward指针数组时，返回的left->forward[0]为空

- **解决方案**：通过检查执行权限、使用 `gdb` 调试并确认是段错误，最终解决了问题。

## 问题 2：跳表的 backward_ 未正确维护
- **描述**：在 `put` 和 `remove` 操作中未正确维护 `backward_` 指针，导致崩溃。
- **解决方案**：在每次修改 `forward_` 时，添加相应的 `backward_` 更新，保证双向链表一致性。


## 回顾
- 通过 `size_bytes` 变量跟踪跳表的内存使用，确保在 `put` 和 `remove` 时正确更新内存大小。
- 智能指针的理解:shared_ptr允许多个指针引用同一个节点，节点生命周期由引用计数管理,weak_ptr指向由一个shared_ptr管理的对象，不改变shared_ptr的引用计数。
