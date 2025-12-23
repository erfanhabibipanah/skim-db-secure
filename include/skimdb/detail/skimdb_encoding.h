#ifndef SKIMDB_ENCODING_H
#define SKIMDB_ENCODING_H

#include <cstdint>
#include <generator>
#include <vector>

#include <cereal/types/vector.hpp>


namespace skim {
namespace detail {

// Block format (16 bits):
// 1xxxxxxxxxxxxxxx = Uncompressed: 15-bit literal value
// 00xxxxxxxxxxxxxx = Run of zeros: 14-bit count
// 01xxxxxxxxxxxxxx = Run of ones:  14-bit count

inline constexpr std::uint16_t g_uncompressed_flag = 0x8000;  // 1 in MSB
inline constexpr std::uint16_t g_run_of_zeros_flag = 0x0000;  // 00 in top 2 bits
inline constexpr std::uint16_t g_run_of_ones_flag  = 0x4000;  // 01 in top 2 bits

inline constexpr std::uint16_t g_literal_mask = 0x7FFF;       // 15 bits for value
inline constexpr std::uint16_t g_count_mask   = 0x3FFF;       // 14 bits for count
inline constexpr std::uint16_t g_max_literal  = 0x7FFF;       // Max 15-bit value
inline constexpr std::uint16_t g_max_run      = 0x3FFF;       // Max 14-bit count

inline constexpr std::uint16_t g_encoding_uncompressed = 2;
inline constexpr std::uint16_t g_encoding_one_run      = 1;
inline constexpr std::uint16_t g_encoding_zero_run     = 0;


inline auto get_block_encoding(std::uint16_t block) -> std::uint16_t { return ((block >> 14) & ~(block >> 15)); }

class encoding {
public:
  encoding() = default;

  auto push(std::size_t idx) -> void {
    if (idx < next_seq_) { return; }

    if (idx == next_seq_) {
      if (!blocks_.empty() && get_block_encoding(blocks_.back()) == g_encoding_one_run) {
        std::uint16_t cur = blocks_.back() & g_count_mask;
        if (cur < g_max_run) {
          ++cur;
          blocks_.back() = static_cast<std::uint16_t>(g_run_of_ones_flag | cur);
        } else {
          blocks_.push_back(static_cast<std::uint16_t>(g_run_of_ones_flag | 1));
        }
      } else {
        blocks_.push_back(static_cast<std::uint16_t>(g_run_of_ones_flag | 1));
      }

      ++next_seq_;
      return;
    }

    std::size_t gap = idx - next_seq_;

    while (gap > 0) {
      if (!blocks_.empty() && get_block_encoding(blocks_.back()) == g_encoding_zero_run) {
        std::uint16_t cur = blocks_.back() & g_count_mask;
        std::size_t space = static_cast<std::size_t>(g_max_run - cur);
        if (space > 0) {
          std::size_t take = (gap < space) ? gap : space;
          cur = static_cast<std::uint16_t>(cur + take);
          blocks_.back() = static_cast<std::uint16_t>(g_run_of_zeros_flag | cur);
          gap -= take;
          continue;
        }
      }

      std::uint16_t take = static_cast<std::uint16_t>((gap > g_max_run) ? g_max_run : gap);
      blocks_.push_back(static_cast<std::uint16_t>(g_run_of_zeros_flag | take));
      gap -= take;
    }

    blocks_.push_back(static_cast<std::uint16_t>(g_run_of_ones_flag | 1));
    next_seq_ = idx + 1;
  }

  auto attempt_compress() -> void {
    std::vector<std::uint16_t> compressed;

    for (std::size_t i = 0, end = blocks_.size(); i < end; ++i) {
      std::uint16_t block = blocks_[i];
      std::uint16_t type = get_block_encoding(block);

      std::uint16_t count = block & g_count_mask;

      if ((count < 15) && (i + 1 < blocks_.size())) {
        std::uint16_t literal = 1 << 15;
        std::size_t inserted = count;

        for (std::size_t bit = 0; bit < count; ++bit) {
          literal |= (type == g_encoding_one_run) ? (1u << (14 - bit)) : 0;
        }

        std::size_t j = i + 1;

        while (inserted < 15) {
          if (j >= blocks_.size()) {
            compressed.push_back(literal);
            i = j;
            break;
          }

          std::uint16_t next_block = blocks_[j];
          std::uint16_t next_type = get_block_encoding(next_block);
          std::uint16_t next_count = next_block & g_count_mask;

          if (inserted + next_count < 16) {
            for (std::size_t bit = 0; bit < next_count; ++bit) {
              literal |= (next_type == g_encoding_one_run) ? (1u << (14 - (inserted + bit))) : 0;
            }

            inserted += next_count;

            if (inserted == 15) {
              compressed.push_back(literal);
              i = j;
              break;
            }

            ++j;
          } else {
            std::size_t can_take = 15 - inserted;
            for (std::size_t bit = 0; bit < can_take; ++bit) {
              literal |= (next_type == g_encoding_one_run) ? (1u << (14 - (inserted + bit))) : 0;
            }

            compressed.push_back(literal);

            std::uint16_t remaining = static_cast<std::uint16_t>(next_count - can_take);

            blocks_[j] = static_cast<std::uint16_t>((next_type == g_encoding_one_run ? g_run_of_ones_flag : g_run_of_zeros_flag) | remaining);
            inserted += can_take;
            i = j - 1;
          }
        }
      } else {
        compressed.push_back(block);
      }
    }

    blocks_ = std::move(compressed);
  }

  auto select_idxs() -> std::generator<std::size_t> {
    std::size_t pos = 0;
    for (auto block : blocks_) {
      switch (get_block_encoding(block)) {
        case g_encoding_uncompressed: {
          std::uint16_t val = block & g_literal_mask;
          for (int bit = 0; bit < 15; ++bit) {
            if (val & (1u << (14 - bit))) { co_yield pos + bit; }
          }
          pos += 15;
          break;
        }
        case g_encoding_zero_run: {
          pos += static_cast<std::size_t>(block & g_count_mask);
          break;
        }
        case g_encoding_one_run: {
          std::size_t num_ones = static_cast<std::size_t>(block & g_count_mask);
          for (std::size_t i = 0; i < num_ones; ++i) { co_yield pos + i; }
          pos += num_ones;
          break;
        }
      }
    }
  }

  template <class Archive>
  void serialize(Archive& archive) {
    archive(blocks_);
  }

private:
  std::vector<std::uint16_t> blocks_;
  std::size_t next_seq_{0};
};

} // namespace detail
} // namespace skim

#endif // SKIMDB_ENCODING_H
