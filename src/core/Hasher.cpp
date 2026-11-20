#include "Hasher.hpp"
#include <algorithm>
#include <cstring>
#include <stdexcept>
namespace UltraArch {
namespace {
std::string hex(const uint8_t *p, size_t n) {
    constexpr char digits[] = "0123456789abcdef";
    std::string s(n * 2, '0');
    for (size_t i = 0; i < n; ++i) {
        s[i * 2] = digits[p[i] >> 4];
        s[i * 2 + 1] = digits[p[i] & 15];
    }
    return s;
}
#ifdef _WIN32
void check(NTSTATUS status) {
    if (status < 0)
        throw std::runtime_error("CNG SHA-256 failure: " + std::to_string(status));
}
struct Provider {
    BCRYPT_ALG_HANDLE handle = nullptr;
    ULONG length = 0;
    Provider() {
        check(BCryptOpenAlgorithmProvider(&handle, BCRYPT_SHA256_ALGORITHM, nullptr, 0));
        ULONG got = 0;
        auto status = BCryptGetProperty(handle, BCRYPT_OBJECT_LENGTH,
                                        reinterpret_cast<PUCHAR>(&length), sizeof(length), &got, 0);
        if (status < 0) {
            BCryptCloseAlgorithmProvider(handle, 0);
            handle = nullptr;
            check(status);
        }
    }
    ~Provider() {
        if (handle)
            BCryptCloseAlgorithmProvider(handle, 0);
    }
};
Provider &provider() {
    static Provider p;
    return p;
}
#else
uint32_t ror(uint32_t x, unsigned n) {
    return (x >> n) | (x << (32 - n));
}
constexpr uint32_t k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
#endif
} // namespace
Hasher::Hasher() {
#ifdef _WIN32
    auto &p = provider();
    object_.resize(p.length);
    check(BCryptCreateHash(p.handle, &hash_, object_.data(), p.length, nullptr, 0, 0));
#endif
}
Hasher::~Hasher() {
#ifdef _WIN32
    if (hash_)
        BCryptDestroyHash(hash_);
#endif
}
void Hasher::update(const void *bytes, size_t count) {
    if (finished_)
        throw std::logic_error("Hash already finished");
    auto p = static_cast<const uint8_t *>(bytes);
#ifdef _WIN32
    while (count) {
        ULONG n = static_cast<ULONG>(std::min<size_t>(count, 0xffffffffu));
        check(BCryptHashData(hash_, const_cast<PUCHAR>(p), n, 0));
        p += n;
        count -= n;
    }
#else
    length_ += count;
    while (count) {
        size_t n = std::min(count, 64 - used_);
        std::memcpy(pending_.data() + used_, p, n);
        used_ += n;
        p += n;
        count -= n;
        if (used_ == 64) {
            block(pending_.data());
            used_ = 0;
        }
    }
#endif
}
#ifndef _WIN32
void Hasher::block(const uint8_t *p) {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i)
        w[i] = (uint32_t(p[i * 4]) << 24) | (uint32_t(p[i * 4 + 1]) << 16) |
               (uint32_t(p[i * 4 + 2]) << 8) | p[i * 4 + 3];
    for (int i = 16; i < 64; ++i)
        w[i] = w[i - 16] + (ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3)) + w[i - 7] +
               (ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10));
    auto a = state_[0], b = state_[1], c = state_[2], d = state_[3], e = state_[4], f = state_[5],
         g = state_[6], h = state_[7];
    for (int i = 0; i < 64; ++i) {
        uint32_t t1 =
            h + (ror(e, 6) ^ ror(e, 11) ^ ror(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
        uint32_t t2 = (ror(a, 2) ^ ror(a, 13) ^ ror(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
    state_[5] += f;
    state_[6] += g;
    state_[7] += h;
}
#endif
std::string Hasher::finish() {
    if (finished_)
        throw std::logic_error("Hash already finished");
    std::array<uint8_t, 32> result{};
#ifdef _WIN32
    check(BCryptFinishHash(hash_, result.data(), static_cast<ULONG>(result.size()), 0));
#else
    uint64_t bits = length_ * 8;
    pending_[used_++] = 0x80;
    if (used_ > 56) {
        std::fill(pending_.begin() + used_, pending_.end(), 0);
        block(pending_.data());
        used_ = 0;
    }
    std::fill(pending_.begin() + used_, pending_.begin() + 56, 0);
    for (int i = 0; i < 8; ++i)
        pending_[63 - i] = static_cast<uint8_t>(bits >> (i * 8));
    block(pending_.data());
    for (int i = 0; i < 8; ++i)
        for (int j = 0; j < 4; ++j)
            result[i * 4 + j] = static_cast<uint8_t>(state_[i] >> (24 - j * 8));
#endif
    finished_ = true;
    return hex(result.data(), result.size());
}
std::string Hasher::text(std::string_view bytes) {
    Hasher h;
    h.update(bytes.data(), bytes.size());
    return h.finish();
}
} // namespace UltraArch
