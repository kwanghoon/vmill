/*
 * Copyright (c) 2017 Trail of Bits, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#ifndef TOOLS_VMILL_VMILL_UTIL_HASH_H_
#define TOOLS_VMILL_VMILL_UTIL_HASH_H_

#include <cstdint>
#include <functional>
#include <string>
#include <type_traits>

namespace vmill {

uint64_t Hash(const void *data, size_t size);

template <typename T>
inline uint64_t Hash(const T *data, size_t size) {
  return Hash(reinterpret_cast<const void *>(data), size);
}

inline uint64_t Hash(const std::string &data) {
  return Hash(data.data(), data.size());
}

template <typename T>
inline uint64_t Hash(
    const T &data,
    typename std::enable_if<std::is_pod<T>::value, int>::type=0) {
  return Hash(reinterpret_cast<const void *>(&data), sizeof(T));
}

}  // namespace vmill

#define VMILL_MAKE_STD_HASH_OVERRIDE(type) \
    namespace std { \
    template <> \
    struct hash<type> { \
     public: \
      using result_type = uint64_t; \
      using argument_type = type; \
      inline result_type operator()(const argument_type &val) const { \
        return vmill::Hash(val); \
      } \
    }; \
    }  // namespace std

#endif  // TOOLS_VMILL_VMILL_UTIL_HASH_H_
