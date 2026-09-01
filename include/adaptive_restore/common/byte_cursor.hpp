#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>
#include "adaptive_restore/common/error.hpp"

namespace adaptive_restore {

// Appends little-endian integers and raw bytes to a growable buffer.
// No bounds concerns on write (vector grows); the invariant is byte order.
class ByteWriter {
public:
    explicit ByteWriter(std::vector<std::byte>& out) : out_(out) {}

    void u8(std::uint8_t v)  { out_.push_back(std::byte{v}); }
    void u16(std::uint16_t v) { put(v); }
    void u32(std::uint32_t v) { put(v); }
    void u64(std::uint64_t v) { put(v); }

    void bytes(std::span<const std::byte> b) { out_.insert(out_.end(), b.begin(), b.end()); }
    void pad(std::size_t n) { out_.insert(out_.end(), n, std::byte{0}); }

    std::size_t size() const { return out_.size(); }

private:
    template <class T>
    void put(T v) {
        for (std::size_t i = 0; i < sizeof(T); ++i)
            out_.push_back(std::byte(static_cast<std::uint8_t>(v >> (8 * i))));
    }
    std::vector<std::byte>& out_;
};

// Reads little-endian integers and raw bytes from a fixed buffer.
// Every accessor throws ImageError rather than reading out of bounds.
class ByteReader {
public:
    explicit ByteReader(std::span<const std::byte> data) : data_(data) {}

    std::uint8_t  u8()  { return get<std::uint8_t>(); }
    std::uint16_t u16() { return get<std::uint16_t>(); }
    std::uint32_t u32() { return get<std::uint32_t>(); }
    std::uint64_t u64() { return get<std::uint64_t>(); }

    std::span<const std::byte> bytes(std::size_t n) {
        need(n);
        auto s = data_.subspan(pos_, n);
        pos_ += n;
        return s;
    }
    void skip(std::size_t n) { need(n); pos_ += n; }

    std::size_t offset() const { return pos_; }
    std::size_t remaining() const { return data_.size() - pos_; }

private:
    void need(std::size_t n) const {
        if (n > data_.size() - pos_) throw ImageError("byte cursor: read past end of buffer");
    }
    template <class T>
    T get() {
        need(sizeof(T));
        T v = 0;
        for (std::size_t i = 0; i < sizeof(T); ++i)
            v = T(v | (T(std::to_integer<std::uint8_t>(data_[pos_ + i])) << (8 * i)));
        pos_ += sizeof(T);
        return v;
    }
    std::span<const std::byte> data_;
    std::size_t pos_ = 0;
};

}  // namespace adaptive_restore
