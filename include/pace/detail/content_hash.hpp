#pragma once
// Compact seed-zero XXH64 adaptation of xxHash v0.8.3. No runtime dependency.
// https://github.com/Cyan4973/xxHash/tree/v0.8.3
/*
 * xxHash - Extremely Fast Hash algorithm
 * Header File
 * Copyright (C) 2012-2023 Yann Collet
 *
 * BSD 2-Clause License (https://www.opensource.org/licenses/bsd-license.php)
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *    * Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *    * Redistributions in binary form must reproduce the above
 *      copyright notice, this list of conditions and the following disclaimer
 *      in the documentation and/or other materials provided with the
 *      distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * You can contact the author at:
 *   - xxHash homepage: https://www.xxhash.com
 *   - xxHash source repository: https://github.com/Cyan4973/xxHash
 */


#include <bit>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <string_view>
namespace pace::detail {
inline std::uint64_t content_hash(std::string_view input) noexcept {
    constexpr std::uint64_t p1=0x9E3779B185EBCA87ULL, p2=0xC2B2AE3D27D4EB4FULL,
        p3=0x165667B19E3779F9ULL, p4=0x85EBCA77C2B2AE63ULL, p5=0x27D4EB2F165667C5ULL;
    const auto read64 = [](const char* p) {
        std::uint64_t value;
        if constexpr (std::endian::native == std::endian::little) std::memcpy(&value, p, 8);
        else { value=0; for (unsigned i=0; i<8; ++i) value |= std::uint64_t(static_cast<unsigned char>(p[i])) << (8*i); }
        return value;
    };
    const auto read32 = [](const char* p) {
        std::uint32_t value;
        if constexpr (std::endian::native == std::endian::little) std::memcpy(&value, p, 4);
        else { value=0; for (unsigned i=0; i<4; ++i) value |= std::uint32_t(static_cast<unsigned char>(p[i])) << (8*i); }
        return value;
    };
    const auto round = [=](std::uint64_t a, std::uint64_t b) { return std::rotl(a + b*p2, 31)*p1; };
    std::uint64_t hash=p5;
    std::size_t offset=0;
    if (input.size() >= 32) {
        std::uint64_t a=p1, b=p2, c=0, d=0;
        a+=p2; d-=p1;
        do {
            a=round(a,read64(input.data()+offset)); b=round(b,read64(input.data()+offset+8));
            c=round(c,read64(input.data()+offset+16)); d=round(d,read64(input.data()+offset+24));
            offset+=32;
        } while (input.size()-offset >= 32);
        hash=std::rotl(a,1)+std::rotl(b,7)+std::rotl(c,12)+std::rotl(d,18);
        for (auto value : {a,b,c,d}) hash=(hash^round(0,value))*p1+p4;
    }
    hash+=input.size();
    while (input.size()-offset >= 8) { hash=std::rotl(hash^round(0,read64(input.data()+offset)),27)*p1+p4; offset+=8; }
    if (input.size()-offset >= 4) { hash=std::rotl(hash^(std::uint64_t(read32(input.data()+offset))*p1),23)*p2+p3; offset+=4; }
    while (offset<input.size()) { hash=std::rotl(hash^(static_cast<unsigned char>(input[offset++])*p5),11)*p1; }
    hash^=hash>>33; hash*=p2; hash^=hash>>29; hash*=p3; return hash^(hash>>32);
}
} // namespace pace::detail
