/*
   Copyright 2022 The Silkworm Authors

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.
*/

#pragma once

#include <array>

#include <evmc/evmc.hpp>

#include <silkworm/common/base.hpp>
#include <silkworm/common/decoding_result.hpp>

namespace silkworm::ssz {

void encode(uint32_t from, Bytes& to) noexcept;

void encode(uint64_t from, Bytes& to) noexcept;

template <class T>
void encode(const T& from, Bytes& to) noexcept;

// TODO(canepat) it doesn't work: why?
// template <class T, int N>
// requires std::convertible_to<T, uint8_t>
// void encode(T const (&from)[N], Bytes& to) noexcept;

template <>
void encode(uint8_t const (&from)[96], Bytes& to) noexcept;

template <>
void encode(const evmc::bytes32& from, Bytes& to) noexcept;

DecodingResult decode(ByteView& from, uint32_t& to) noexcept;

DecodingResult decode(ByteView& from, uint64_t& to) noexcept;

template <class T>
DecodingResult decode(ByteView& from, T& to) noexcept;

// template <class T, std::size_t N>
// DecodingResult decode(ByteView& from, T (&to)[N]) noexcept;

template <>
DecodingResult decode(ByteView& from, uint8_t (&to)[96]) noexcept;

template <>
DecodingResult decode(ByteView& from, evmc::bytes32& to) noexcept;

void encode_offset(uint32_t from, Bytes& to) noexcept;

DecodingResult decode_offset(ByteView& from, uint32_t& to) noexcept;

}  // namespace silkworm::ssz