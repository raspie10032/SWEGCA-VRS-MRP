#include "swegca_architecture/sha256.hpp"

#include <algorithm>
#include <bit>
#include <cstring>
#include <limits>
#include <stdexcept>
#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__)) && !defined(SWEGCA_SHA256_SCALAR_ONLY)
#define SWEGCA_SHA256_X86 1
#include <immintrin.h>
#include <cpuid.h>
#endif

namespace swegca::architecture {
namespace {

// FIPS 180-4 §4.2.2 round constants.
constexpr std::array<std::uint32_t, 64> round_constants{
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

// SWEGCA: src/swegca/mosaic_memory_promotion.py@5901a5a:180
constexpr std::uint32_t load_be32(const std::byte* bytes) noexcept {
    return (std::to_integer<std::uint32_t>(bytes[0]) << 24) |
           (std::to_integer<std::uint32_t>(bytes[1]) << 16) |
           (std::to_integer<std::uint32_t>(bytes[2]) << 8) |
           std::to_integer<std::uint32_t>(bytes[3]);
}

#ifdef SWEGCA_SHA256_X86
bool native_sha_available() noexcept {
    unsigned a=0,b=0,c=0,d=0;
    if(!__get_cpuid_count(1,0,&a,&b,&c,&d)||!(c&(1U<<9)))return false;
    return __get_cpuid_count(7,0,&a,&b,&c,&d) && (b & (1U<<29));
}
// SHA256RNDS2 applies exactly the same modulo-2^32 rounds. Lane order is
// [F,E,B,A] and [H,G,D,C] from low to high; the message operand carries
// consecutive W+K words. No digest framing or arithmetic policy is changed.
__attribute__((target("sha,ssse3")))
void native_compress(std::uint32_t* state,const std::byte* block,std::size_t blocks=1) noexcept {
    auto abef=_mm_set_epi32(state[0],state[1],state[4],state[5]);
    auto cdgh=_mm_set_epi32(state[2],state[3],state[6],state[7]);
    // Keep the chaining state in its native lane order across contiguous
    // blocks. Each block still performs all 64 rounds and feed-forward.
    for(std::size_t n=0;n<blocks;++n,block+=64){
        const auto prior_abef=abef,prior_cdgh=cdgh;
        __m128i words[4];
        const auto swap=_mm_set_epi8(12,13,14,15,8,9,10,11,4,5,6,7,0,1,2,3);
        for(unsigned group=0;group<16;++group){
            const auto slot=group%4;
            if(group<4)words[slot]=_mm_shuffle_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(block+group*16)),swap);
            else {
                // W[t..t+3] from the four preceding groups, preserving the exact
                // SHA-256 schedule recurrence rather than precomputing 64 scalars.
                const auto previous=words[(slot+3)%4];
                auto next=_mm_sha256msg1_epu32(words[slot],words[(slot+1)%4]);
                next=_mm_add_epi32(next,_mm_alignr_epi8(previous,words[(slot+2)%4],4));
                words[slot]=_mm_sha256msg2_epu32(next,previous);
            }
            auto message=_mm_add_epi32(words[slot],
                _mm_loadu_si128(reinterpret_cast<const __m128i*>(round_constants.data()+group*4)));
            cdgh=_mm_sha256rnds2_epu32(cdgh,abef,message);
            message=_mm_shuffle_epi32(message,0x0e);
            abef=_mm_sha256rnds2_epu32(abef,cdgh,message);
        }
        abef=_mm_add_epi32(abef,prior_abef);cdgh=_mm_add_epi32(cdgh,prior_cdgh);
    }
    std::array<std::uint32_t,4> left,right;
    _mm_storeu_si128(reinterpret_cast<__m128i*>(left.data()),abef);
    _mm_storeu_si128(reinterpret_cast<__m128i*>(right.data()),cdgh);
    state[0]=left[3];state[1]=left[2];state[4]=left[1];state[5]=left[0];
    state[2]=right[3];state[3]=right[2];state[6]=right[1];state[7]=right[0];
}
#endif

}  // namespace

// FIPS 180-4 §5.3.3 initial hash value.
Sha256::Sha256() noexcept
    : state_{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
             0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19} {}

// FIPS 180-4 §6.2.2 compression of one 512-bit block.
// SWEGCA: src/swegca/mosaic_memory_promotion.py@5901a5a:180
void Sha256::compress(const std::byte* block) noexcept {
#ifdef SWEGCA_SHA256_X86
    static const bool accelerated=native_sha_available();
    if(accelerated){native_compress(state_.data(),block);return;}
#endif
    std::array<std::uint32_t, 64> schedule{};
    for (std::size_t at = 0; at < 16; ++at)
        schedule[at] = load_be32(block + at * 4);
    for (std::size_t at = 16; at < 64; ++at) {
        const auto s0 = std::rotr(schedule[at - 15], 7) ^
                        std::rotr(schedule[at - 15], 18) ^ (schedule[at - 15] >> 3);
        const auto s1 = std::rotr(schedule[at - 2], 17) ^
                        std::rotr(schedule[at - 2], 19) ^ (schedule[at - 2] >> 10);
        schedule[at] = schedule[at - 16] + s0 + schedule[at - 7] + s1;
    }
    auto a = state_[0], b = state_[1], c = state_[2], d = state_[3];
    auto e = state_[4], f = state_[5], g = state_[6], h = state_[7];
    for (std::size_t at = 0; at < 64; ++at) {
        const auto sum1 = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
        const auto choose = (e & f) ^ (~e & g);
        const auto t1 = h + sum1 + choose + round_constants[at] + schedule[at];
        const auto sum0 = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
        const auto majority = (a & b) ^ (a & c) ^ (b & c);
        const auto t2 = sum0 + majority;
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

// SWEGCA: src/swegca/mosaic_memory_promotion.py@5901a5a:180
void Sha256::update(std::span<const std::byte> data) {
    if (finished_) throw std::logic_error("sha256_update_after_finish");
    if (data.empty()) return;
    if (data.size() >
        std::numeric_limits<std::uint64_t>::max() / 8 - total_bytes_)
        throw std::length_error("sha256_input_too_large");
    total_bytes_ += data.size();
    std::size_t at = 0;
    if (buffered_ != 0) {
        const auto take = std::min(buffer_.size() - buffered_, data.size());
        std::memcpy(buffer_.data() + buffered_, data.data(), take);
        buffered_ += take;
        at = take;
        if (buffered_ < buffer_.size()) return;
        compress(buffer_.data());
        buffered_ = 0;
    }
#ifdef SWEGCA_SHA256_X86
    static const bool accelerated=native_sha_available();
    const auto blocks=(data.size()-at)/buffer_.size();
    if(accelerated&&blocks){
        native_compress(state_.data(),data.data()+at,blocks);
        at+=blocks*buffer_.size();
    }else
#endif
    for (; data.size() - at >= buffer_.size(); at += buffer_.size())
        compress(data.data() + at);
    const auto rest = data.size() - at;
    if (rest != 0) std::memcpy(buffer_.data(), data.data() + at, rest);
    buffered_ = rest;
}

// SWEGCA: src/swegca/mosaic_memory_promotion.py@5901a5a:180
void Sha256::update(std::string_view text) {
    update(std::as_bytes(std::span<const char>(text.data(), text.size())));
}

// FIPS 180-4 §5.1.1 padding: 0x80, zeros, 64-bit big-endian bit length.
// SWEGCA: src/swegca/mosaic_memory_promotion.py@5901a5a:180
Sha256::Bytes Sha256::finish() {
    if (finished_) throw std::logic_error("sha256_finish_twice");
    const std::uint64_t bit_length = total_bytes_ * 8;
    std::array<std::byte, 72> padding{};
    padding[0] = std::byte{0x80};
    const auto used = buffered_;
    const std::size_t pad = used < 56 ? 56 - used : 120 - used;
    for (std::size_t at = 0; at < 8; ++at)
        padding[pad + at] =
            static_cast<std::byte>((bit_length >> (56 - 8 * at)) & 0xff);
    const auto total = total_bytes_;
    update(std::span<const std::byte>(padding.data(), pad + 8));
    total_bytes_ = total;
    finished_ = true;
    Bytes out{};
    for (std::size_t word = 0; word < 8; ++word)
        for (std::size_t at = 0; at < 4; ++at)
            out[word * 4 + at] =
                static_cast<std::byte>((state_[word] >> (24 - 8 * at)) & 0xff);
    return out;
}

// SWEGCA: src/swegca/mosaic_memory_promotion.py@5901a5a:180
Sha256::Bytes Sha256::of(std::span<const std::byte> data) {
    Sha256 hash;
    hash.update(data);
    return hash.finish();
}

}  // namespace swegca::architecture
