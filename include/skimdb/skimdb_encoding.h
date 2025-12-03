
#ifndef SKIMDB_ENCODING_H
#define SKIMDB_ENCODING_H

#include <cstdint>
#include <generator>
#include <vector>

#include <cereal/types/vector.hpp>


namespace skim {

// Block format (16 bits):
// 1xxxxxxxxxxxxxxx = Uncompressed: 15-bit literal value
// 00xxxxxxxxxxxxxx = Run of zeros: 14-bit count
// 01xxxxxxxxxxxxxx = Run of ones:  14-bit count

constexpr uint16_t UNCOMPRESSED_FLAG = 0x8000;  // 1 in MSB
constexpr uint16_t RUN_OF_ZEROS_FLAG = 0x0000;  // 00 in top 2 bits
constexpr uint16_t RUN_OF_ONES_FLAG  = 0x4000;  // 01 in top 2 bits

constexpr uint16_t LITERAL_MASK = 0x7FFF;       // 15 bits for value
constexpr uint16_t COUNT_MASK   = 0x3FFF;       // 14 bits for count
constexpr uint16_t MAX_LITERAL  = 0x7FFF;       // Max 15-bit value
constexpr uint16_t MAX_RUN      = 0x3FFF;       // Max 14-bit count

enum class BlockType { Uncompressed, ZeroRun, OneRun };

inline BlockType getBlockType(uint16_t block) {
  if (block & UNCOMPRESSED_FLAG) { return BlockType::Uncompressed; }
  if (block & RUN_OF_ONES_FLAG) { return BlockType::OneRun; }
  return BlockType::ZeroRun;
} // getBlockType

class encoding {
public:
  encoding() = default;

  void push(std::size_t idx) {
    // ignore out-of-order/backward inserts
    if (idx < next_seq_) { return; }

    // Case: append a 1 at next_seq_
    if (idx == next_seq_) {
      if (!blocks_.empty() && getBlockType(blocks_.back()) == BlockType::OneRun) {
        uint16_t cur = blocks_.back() & COUNT_MASK;
        if (cur < MAX_RUN) {
          ++cur;
          blocks_.back() = static_cast<uint16_t>(RUN_OF_ONES_FLAG | cur);
        } else {
          blocks_.push_back(static_cast<uint16_t>(RUN_OF_ONES_FLAG | 1));
        }
      } else {
        blocks_.push_back(static_cast<uint16_t>(RUN_OF_ONES_FLAG | 1));
      }

      ++next_seq_;
      return;
    }

    // Case: idx > next_seq_, need to emit zeros for [next_seq_, idx-1] then a one
    std::size_t gap = idx - next_seq_;
    while (gap > 0) {
      // try to extend existing zero run if present
      if (!blocks_.empty() && getBlockType(blocks_.back()) == BlockType::ZeroRun) {
        uint16_t cur = blocks_.back() & COUNT_MASK;
        std::size_t space = static_cast<std::size_t>(MAX_RUN - cur);
        if (space > 0) {
          std::size_t take = (gap < space) ? gap : space;
          cur = static_cast<uint16_t>(cur + take);
          blocks_.back() = static_cast<uint16_t>(RUN_OF_ZEROS_FLAG | cur);
          gap -= take;
          continue;
        }
      }

      // need to push a new zero run block
      uint16_t take = static_cast<uint16_t>((gap > MAX_RUN) ? MAX_RUN : gap);
      blocks_.push_back(static_cast<uint16_t>(RUN_OF_ZEROS_FLAG | take));
      gap -= take;
    }

    // insert the one at idx
    blocks_.push_back(static_cast<uint16_t>(RUN_OF_ONES_FLAG | 1));
    next_seq_ = idx + 1;
  } // push

  void attempt_compress() {
    if (prev_compressed_) { return; }

    std::vector<std::uint16_t> compressed;

    for (std::size_t i = 0; i < blocks_.size(); ++i) {
      uint16_t block = blocks_[i];
      BlockType type = getBlockType(block);

      // try to merge runs
      uint16_t count = block & COUNT_MASK;
      if (count < 15 && i + 1 < blocks_.size()) {
        uint16_t literal = 1 << 15;
        std::size_t inserted = count;

        for (std::size_t bit = 0; bit < count; ++bit) {
          literal |= (type == BlockType::OneRun) ? (1u << (14 - bit)) : 0;
        }

        std::size_t j = i + 1;
        while (inserted < 15) {
          // no more blocks to merge
          if (j >= blocks_.size()) {
            compressed.push_back(literal);
            i = j;
            break;
          }

          uint16_t next_block = blocks_[j];
          BlockType next_type = getBlockType(next_block);
          uint16_t next_count = next_block & COUNT_MASK;

          if (inserted + next_count < 16) {
            for (std::size_t bit = 0; bit < next_count; ++bit) {
              literal |= (next_type == BlockType::OneRun) ? (1u << (14 - (inserted + bit))) : 0;
            }

            inserted += next_count;

            if (inserted == 15) {
              compressed.push_back(literal);
              i = j;
              break;
            }

            ++j;
          } else {
            // Add as much as possible from next_block
            std::size_t can_take = 15 - inserted;
            for (std::size_t bit = 0; bit < can_take; ++bit) {
              literal |= (next_type == BlockType::OneRun) ? (1u << (14 - (inserted + bit))) : 0;
            }

            compressed.push_back(literal);

            // Update next_block to reflect remaining count
            uint16_t remaining = static_cast<uint16_t>(next_count - can_take);

            blocks_[j] = static_cast<uint16_t>((next_type == BlockType::OneRun ? RUN_OF_ONES_FLAG : RUN_OF_ZEROS_FLAG) | remaining);
            inserted += can_take;
            i = j - 1; // will be incremented in outer loop
          }
        }
      } else {
        compressed.push_back(block);
      }
    }

    blocks_ = std::move(compressed);
    prev_compressed_ = true;
  } // attempt_compress

  std::generator<std::size_t> select_idxs() {
    std::size_t pos = 0;
    for (auto block : blocks_) {
      switch (getBlockType(block)) {
        case BlockType::Uncompressed: {
          uint16_t val = block & LITERAL_MASK;
          for (int bit = 0; bit < 15; ++bit) {
            if (val & (1u << (14 - bit))) { co_yield pos + bit; }
          }
          pos += 15;
          break;
        }
        case BlockType::ZeroRun: {
          pos += static_cast<std::size_t>(block & COUNT_MASK);
          break;
        }
        case BlockType::OneRun: {
          std::size_t cnt = static_cast<std::size_t>(block & COUNT_MASK);
          for (std::size_t i = 0; i < cnt; ++i) { co_yield pos + i; }
          pos += cnt;
          break;
        }
      }
    }
  } // select_idxs

  template<class Archive>
  void serialize(Archive& archive) {
    archive(blocks_, prev_compressed_, next_seq_);
  } // serialize

private:
  std::vector<std::uint16_t> blocks_;
  bool prev_compressed_;
  std::size_t next_seq_;
};

} // namespace skim

#endif // SKIMDB_ENCODING_H
