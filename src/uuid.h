/**
 * @file src/uuid.h
 * @brief Declarations for UUID generation.
 */
#pragma once

// standard includes
#include <cstdint>
#include <cstdio>
#include <random>
#include <stdexcept>
#include <string>

/**
 * @brief UUID utilities.
 */
namespace uuid_util {
  /**
   * @brief UUID value exposed through multiple integer views.
   */
  union uuid_t {
    std::uint8_t b8[16];  ///< UUID bytes.
    std::uint16_t b16[8];  ///< UUID viewed as 16-bit words.
    std::uint32_t b32[4];  ///< UUID viewed as 32-bit words.
    std::uint64_t b64[2];  ///< UUID viewed as 64-bit words.

    /**
     * @brief Generate a UUID value.
     *
     * @param engine Random-number engine used to generate UUID bytes.
     * @return Random UUID generated from the supplied engine.
     */
    static uuid_t generate(std::default_random_engine &engine) {
      std::uniform_int_distribution<std::uint8_t> dist(0, std::numeric_limits<std::uint8_t>::max());

      uuid_t buf;
      for (auto &el : buf.b8) {
        el = dist(engine);
      }

      buf.b8[7] &= (std::uint8_t) 0b00101111;
      buf.b8[9] &= (std::uint8_t) 0b10011111;

      return buf;
    }

    /**
     * @brief Generate a UUID value.
     *
     * @return Random UUID generated from std::random_device seeding.
     */
    static uuid_t generate() {
      std::random_device r;

      std::default_random_engine engine {r()};

      return generate(engine);
    }

    /**
     * @brief Parse a canonical UUID string `xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx`.
     *
     * @param uuid_str Canonical UUID text (length 36).
     * @return UUID with the same byte layout as a Win32 `GUID` on little-endian.
     */
    static uuid_t parse(const std::string &uuid_str) {
      if (uuid_str.length() != 36) {
        throw std::invalid_argument("Invalid UUID string length");
      }

      uuid_t uuid {};
      unsigned int data0 = 0;
      unsigned int data1 = 0;
      unsigned int data2 = 0;
      unsigned int data3 = 0;
      unsigned int data4a = 0;
      unsigned int data4b = 0;

      std::sscanf(
        uuid_str.c_str(),
        "%8x-%4x-%4x-%4x-%8x%4x",
        &data0,
        &data1,
        &data2,
        &data3,
        &data4a,
        &data4b
      );

      uuid.b32[0] = data0;
      uuid.b16[2] = static_cast<std::uint16_t>(data1);
      uuid.b16[3] = static_cast<std::uint16_t>(data2);
      uuid.b8[8] = (data3 >> 8) & 0xFF;
      uuid.b8[9] = data3 & 0xFF;
      uuid.b8[10] = (data4a >> 24) & 0xFF;
      uuid.b8[11] = (data4a >> 16) & 0xFF;
      uuid.b8[12] = (data4a >> 8) & 0xFF;
      uuid.b8[13] = data4a & 0xFF;
      uuid.b8[14] = (data4b >> 8) & 0xFF;
      uuid.b8[15] = data4b & 0xFF;

      return uuid;
    }

    /**
     * @brief Format the UUID using canonical text form.
     *
     * @return Canonical lowercase UUID string.
     */
    [[nodiscard]] std::string string() const {
      std::string result;

      result.reserve(sizeof(uuid_t) * 2 + 4);

      auto hex = util::hex(*this, true);
      auto hex_view = hex.to_string_view();

      std::string_view slices[] = {
        hex_view.substr(0, 8),
        hex_view.substr(8, 4),
        hex_view.substr(12, 4),
        hex_view.substr(16, 4)
      };
      auto last_slice = hex_view.substr(20, 12);

      for (auto &slice : slices) {
        std::copy(std::begin(slice), std::end(slice), std::back_inserter(result));

        result.push_back('-');
      }

      std::copy(std::begin(last_slice), std::end(last_slice), std::back_inserter(result));

      return result;
    }

    /**
     * @brief Compare two UUID values for equality.
     *
     * @param other UUID value to compare against.
     * @return True when both UUID values contain identical bytes.
     */
    constexpr bool operator==(const uuid_t &other) const {
      return b64[0] == other.b64[0] && b64[1] == other.b64[1];
    }

    /**
     * @brief Order UUID values by their 64-bit word representation.
     *
     * @param other UUID value to compare against.
     * @return True when this UUID sorts before `other`.
     */
    constexpr bool operator<(const uuid_t &other) const {
      return (b64[0] < other.b64[0] || (b64[0] == other.b64[0] && b64[1] < other.b64[1]));
    }

    /**
     * @brief Order UUID values by their 64-bit word representation.
     *
     * @param other UUID value to compare against.
     * @return True when this UUID sorts after `other`.
     */
    constexpr bool operator>(const uuid_t &other) const {
      return (b64[0] > other.b64[0] || (b64[0] == other.b64[0] && b64[1] > other.b64[1]));
    }
  };
}  // namespace uuid_util
