#include "../../include/block/blockmeta.h"
#include <cassert>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <sys/types.h>

namespace tiny_lsm {
BlockMeta::BlockMeta() : offset(0), first_key(""), last_key("") {}

BlockMeta::BlockMeta(size_t offset, const std::string& first_key, const std::string& last_key)
    : offset(offset), first_key(first_key), last_key(last_key) {}

void BlockMeta::encode_meta_to_slice(std::vector<BlockMeta>& meta_entries,
                                     std::vector<uint8_t>& metadata) {
    // TODO: Lab 3.4 将内存中所有`Blcok`的元数据编码为二进制字节数组
    // ? 输入输出都由参数中的引用给定, 你不需要自己创建`vector`
    metadata.clear();
    //预分配内存，避免循环的时候频繁扩容
    uint32_t num = meta_entries.size();
    //这个实现有误，num表示元素的个数，但是所占的字节只是4B,无论有多少数据都是如此
    // size_t total_size = num + 4; //加上末尾hash的4字节
    size_t total_size = sizeof(uint32_t) + sizeof(uint32_t);
    for (const auto& meta : meta_entries) {
        total_size += 4 + 2 + meta.first_key.size() + 2 + meta.last_key.size();
        // Meta[i]所占的字节
    }
    metadata.reserve(total_size); //预分配内存

    for (int i = 0; i < 4; i++) {
        metadata.push_back((num >> (i * 8)) & 0XFF);
    }
    for (const auto& meta : meta_entries) {
        //存offset部分，占4B
        uint32_t offset = meta.offset;
        for (int i = 0; i < 4; ++i) {
            metadata.push_back((offset >> (i * 8)) & 0xFF);
        }
        //防止长度超过 2 字节表示范围导致静默截断
        assert(meta.first_key.size() <= 0xFFFF && "first_key length exceeds65535!");
        uint16_t first_keylen = meta.first_key.size();

        metadata.push_back(first_keylen & 0xFF);
        metadata.push_back((first_keylen >> 8) & 0xFF);
        metadata.insert(metadata.end(), meta.first_key.begin(), meta.first_key.end());

        assert(meta.last_key.size() <= 0xFFFF && "last_key length exceeds 65535!");
        uint16_t last_keylen = meta.last_key.size();

        metadata.push_back(last_keylen & 0xFF);
        metadata.push_back((last_keylen >> 8) & 0xFF);
        metadata.insert(metadata.end(), meta.last_key.begin(), meta.last_key.end());
    }
    auto data_start = metadata.data();
    size_t data_len = total_size - sizeof(uint32_t);
    uint32_t hash_v = static_cast<uint32_t>(std::hash<std::string_view>{}(
        std::string_view(reinterpret_cast<const char*>(data_start), data_len)));
    for (int i = 0; i < 4; i++) {
        metadata.push_back((hash_v >> (i * 8)) & 0xFF);
    }
}

std::vector<BlockMeta> BlockMeta::decode_meta_from_slice(const std::vector<uint8_t>& metadata) {
    // TODO: Lab 3.4 将二进制字节数组解码为内存中的`Blcok`元数据
    std::vector<BlockMeta> result;
    size_t pos = 0;
    const size_t total_size = metadata.size();
    if (total_size < 8) { // 一个合法的Blockmeta至少有8B数据
        throw std::runtime_error("Invalid meta");
    }
    //用lambda函数实现数据的读取
    auto read_uint16 = [&]() -> uint16_t { //读2B数据
        if (pos + 2 > total_size) {
            throw std::runtime_error("Invalid metadata: unexpected end while reading key length");
        }
        uint16_t val =
            static_cast<uint16_t>(metadata[pos]) | (static_cast<uint16_t>(metadata[pos + 1]) << 8);
        pos += 2;
        return val;
    };
    auto read_uint32 = [&]() -> uint32_t { //读4B数据
        if (pos + 4 > total_size) {
            throw std::runtime_error("Invalid metadata: unexpected end while reading key length");
        }
        uint32_t val = 0;
        for (int i = 0; i < 4; ++i) {
            val |= (static_cast<uint32_t>(metadata[pos + i]) << (i * 8)); //正确做法，先转换再移位
            // val |= static_cast<uint32_t>(metadata[pos + i] << (i *8));
            //这种做法不对，metadata[pos+i]是uint8_t类型,左移运算会发生整数提升,隐式转换成有符号的int
        }
        pos += 4;
        return val;
    };
    auto read_string = [&](size_t len) -> std::string {
        if (pos + len > total_size) {
            throw std::runtime_error("Invalid metadata: unexpected end while reading key length");
        }
        std::string key(metadata.begin() + pos, metadata.begin() + pos + len);
        pos += len;
        return key;
    };
    size_t num = read_uint32();
    size_t cnt_meta = 0;
    //末尾4B是hash,但是可能会有损坏的数据,不能依赖pos+4来判断
    // while (pos + 4 < total_size) {
    //     BlockMeta meta;
    //     //解析offset(4B)
    //     meta.offset = read_uint32();

    //     uint16_t first_keylen = read_uint16();
    //     meta.first_key = read_string(first_keylen);

    //     uint16_t last_keylen = read_uint16();
    //     meta.last_key = read_string(last_keylen);

    //     result.push_back(std::move(meta));
    //     cnt_meta++;
    // }
    for (int i = 0; i < num; i++) { //读取num个meta
        BlockMeta meta;
        meta.offset = read_uint32();

        uint16_t first_keylen = read_uint16();
        meta.first_key = read_string(first_keylen);

        uint16_t last_keylen = read_uint16();
        meta.last_key = read_string(last_keylen);
        result.push_back(std::move(meta));
        cnt_meta++;
    }

    if (cnt_meta != num) {
        throw std::runtime_error("Invalid metadata: parsed count does not match declared num");
    }
    // debug
    // std::cout << "Debug: Total size = " << total_size << ", Final pos = " << pos
    //           << ", Remaining bytes = " << (total_size - pos) << std::endl;
    if (pos + 4 != total_size) {
        throw std::runtime_error("Invalid hashval");
    }
    uint32_t stored_hash = 0;
    for (int i = 0; i < 4; ++i) {
        stored_hash |= (static_cast<uint32_t>(metadata[pos + i]) << (i * 8));
    }
    const uint8_t* data_start = metadata.data();
    size_t data_len = total_size - 4;
    uint32_t computed_hash = static_cast<uint32_t>(std::hash<std::string_view>{}(
        std::string_view(reinterpret_cast<const char*>(data_start), data_len)));
    if (computed_hash != stored_hash) {
        throw std::runtime_error("Invalid metadata: Hash mismatch (data corrupted)");
    }
    return result;
}
} // namespace tiny_lsm
