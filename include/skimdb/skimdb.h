#ifndef SKIMDB_H
#define SKIMDB_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <generator>
#include <optional>
#include <string>
#include <vector>

#include <zip.h>

#include <bbhash/bbhash.h>

#include <cereal/archives/binary.hpp>
#include <cereal/types/string.hpp>
#include <cereal/types/vector.hpp>

#include "detail/skimdb_definitions.h"
#include "detail/skimdb_encoding.h"
#include "detail/skimdb_util.h"

#include "skimdb_config.h"


namespace skim {

namespace fs = std::filesystem;


struct skim_runtime_config {
  unsigned int grpc_connect_timeout{5};                   // gRPC connection timeout (seconds)
  unsigned int grpc_download_timeout{900};                // gRPC data download timeout (seconds)
  std::string siper_server_store_dir{".skimdb-server"};   // path to directory where server stores hint data
  std::string siper_client_metadata_dir{".skimdb-cache"}; // path to directory to store metadata on client's side
  std::string siper_client_hint_c_dir{".skimdb-cache"};   // path to directory to store hint_c on client's side
};

skim_runtime_config g_skim_config;


class skimdb final {
public:
  using parameters_type = skimdb_parameters;

  static_assert(sizeof(std::size_t) == sizeof(bbh::bbhash<kmer_binary_t>::size_type) &&
                    std::is_unsigned_v<bbh::bbhash<kmer_binary_t>::size_type>,
                "size_type must be equivalent to std::size_t");

  struct kmer_index {
    kmer_index() = default;

    bitmap_t kmers{};
    bbh::bbhash<kmer_binary_t> hash;

    [[nodiscard]] auto find(kmer_binary_t kmer) const -> std::optional<std::size_t> {
      if (kmers.contains(kmer)) {
        return hash.find(kmer);
      }
      return std::nullopt;
    }

    template <class Archive>
    void serialize(Archive& ar) {
      ar(CEREAL_NVP(kmers));
      ar(CEREAL_NVP(hash));
    }
  };

  struct skimdb_components {
    std::vector<detail::encoding> data;
    std::vector<std::string> labels;
    kmer_index index;
  };


  skimdb() = default;


  [[nodiscard]] auto parameters() const -> parameters_type { return parameters_type{.k = k_, .s = s_, .t = t_}; }


  [[nodiscard]] auto explode() && -> skimdb_components {
    return {.data = std::move(data_), .labels = std::move(labels_), .index = std::move(index_)};
  }


  // given a kmer, returns a generator over annotated labels
  [[nodiscard]] auto query(std::string s) const -> std::generator<const std::string&> {
    auto pos = m_find_kmer_pos_(s);

    if (!pos.has_value()) {
      co_return;
    }

    for (auto label_idx : m_traverse_kmer_(pos.value())) {
      if (label_idx >= labels_.size()) {
        break;
      }
      co_yield labels_[label_idx];
    }
  }


  auto load(const fs::path& path) -> std::expected<void, std::string> {
    struct zip_deleter {
      void operator()(zip_t* za) const noexcept {
        if (za)
          zip_close(za);
      }
    };

    struct zip_streambuf : std::streambuf {
      zip_file_t* zf;
      std::array<char, 64 * 1024> buffer{};

      zip_streambuf(zip_file_t* zf_) : zf(zf_) { setg(buffer.data(), buffer.data(), buffer.data()); }

      auto underflow() -> int_type override {
        zip_int64_t n = zip_fread(zf, buffer.data(), buffer.size());
        if (n <= 0) {
          return traits_type::eof();
        }
        setg(buffer.data(), buffer.data(), buffer.data() + n);
        return traits_type::to_int_type(*gptr());
      }
    };

    using zip_ptr_type = std::unique_ptr<zip_t, zip_deleter>;

    int err = 0;
    zip_ptr_type za{zip_open(path.c_str(), ZIP_RDONLY, &err)};

    if (!za) {
      return std::unexpected{"could not open zip archive"};
    }

    std::size_t data_size = 0;

    // get metadata
    {
      auto meta_path{path};
      meta_path.replace_extension(".meta");

      zip_file_t* zf = zip_fopen(za.get(), meta_path.c_str(), 0);

      if (!zf) {
        std::string msg{zip_strerror(za.get())};
        return std::unexpected{std::format("failed to open {}, {}", meta_path.string(), msg)};
      }

      zip_streambuf sb{zf};
      std::istream is{&sb};

      try {
        cereal::BinaryInputArchive archive(is);
        skimdb_version_t ver;
        archive(ver, k_, s_, t_, labels_, index_, data_size);
      } catch (const std::exception& e) {
        zip_fclose(zf);
        return std::unexpected{std::format("deserialization of {} failed, {}", meta_path.string(), e.what())};
      }

      zip_fclose(zf);
    }

    // enumerate data parts
    std::vector<std::string> parts;

    for (std::size_t i = 0;; ++i) {
      auto name{path};
      auto part{std::format("{:05d}", i)};

      name.replace_extension(".data." + part);

      if (zip_name_locate(za.get(), name.c_str(), 0) < 0) {
        break;
      }
      parts.push_back(name);
    }

    // read data parts
    using value_type = typename decltype(data_)::value_type;

    data_.clear();
    data_.resize(data_size);

    std::atomic<bool> failed{false};
    std::string err_msg;
    std::mutex err_mtx;

    std::for_each(std::execution::par, parts.begin(), parts.end(), [&](const std::string& part) {
      if (failed.load()) {
        return;
      }

      int err = 0;
      zip_t* za_local = zip_open(path.c_str(), ZIP_RDONLY, &err);

      if (!za_local) {
        failed = true;
        std::lock_guard lock(err_mtx);
        err_msg = "could not open zip archive";
        return;
      }

      auto za_guard = std::unique_ptr<zip_t, decltype(&zip_close)>(za_local, zip_close);

      zip_file_t* zf = zip_fopen(za_local, part.c_str(), 0);

      if (!zf) {
        std::string msg{zip_strerror(za_local)};
        failed = true;
        std::lock_guard lock(err_mtx);
        err_msg = std::format("failed to open {}, {}", part, msg);
        return;
      }

      zip_streambuf sb(zf);
      std::istream is(&sb);

      std::size_t idx = std::stoul(part.substr(part.size() - 5));

      std::size_t pos = idx * batch_size_;
      std::size_t end = std::min(pos + batch_size_, data_size);

      try {
        cereal::BinaryInputArchive archive(is);

        for (; pos < end; ++pos) {
          archive(data_[pos]);
        }
      } catch (const std::exception& e) {
        failed = true;
        std::lock_guard lock(err_mtx);
        err_msg = std::format("deserialization of {} failed {}", part, e.what());
      }

      zip_fclose(zf);
    });

    if (failed) {
      return std::unexpected{err_msg};
    }

    return {};
  }

  auto save(const fs::path& path) const -> std::expected<std::uintmax_t, std::string> {
    struct zip_deleter {
      void operator()(zip_t* za) const noexcept {
        if (za) {
          zip_discard(za);
        }
      }
    };

    using zip_ptr_type = std::unique_ptr<zip_t, zip_deleter>;

    // we wrap everything into ZIP
    int err = 0;
    zip_ptr_type za{zip_open(path.c_str(), ZIP_CREATE | ZIP_TRUNCATE, &err)};

    if (!za) {
      return std::unexpected{"could not create zip archive"};
    }

    auto add_to_zip = [&](const std::string& filename) -> bool {
      zip_source_t* src = zip_source_file(za.get(), filename.c_str(), 0, 0);
      if (!src) {
        return false;
      }

      zip_int64_t idx = zip_file_add(za.get(), fs::path(filename).filename().c_str(), src, ZIP_FL_ENC_UTF_8);
      if (idx < 0) {
        zip_source_free(src);
        return false;
      }

      zip_set_file_compression(za.get(), idx, ZIP_CM_STORE, 0); // no compression

      return true;
    };

    std::vector<fs::path> cleanup_list;

    // write metadata
    {
      auto meta_path{path};
      meta_path.replace_extension(".meta");

      std::ofstream of{meta_path, std::ios::binary};
      if (!of) {
        return std::unexpected{"could not create meta file"};
      }

      cleanup_list.push_back(meta_path);

      try {
        cereal::BinaryOutputArchive archive{of};
        skimdb_version_t ver;
        archive(ver, k_, s_, t_, labels_, index_, data_.size());
      } catch (const std::exception& e) {
        return std::unexpected{std::format("serialization failed {}", e.what())};
      }

      of.close();

      if (!add_to_zip(meta_path)) {
        std::string msg{zip_strerror(za.get())};
        return std::unexpected{std::format("failed to zip {}, {}", meta_path.string(), msg)};
      }
    }

    // here we pack data
    std::size_t num_batches = (data_.size() + batch_size_ - 1) / batch_size_;
    auto indices = std::views::iota(std::size_t{0}, num_batches);

    if (num_batches > 99999) {
      return std::unexpected{"too many kmers, run everyone!!!"};
    }

    std::atomic<bool> failed{false};

    std::string err_msg;
    std::mutex err_mtx;

    std::for_each(std::execution::par, indices.begin(), indices.end(), [&](std::size_t i) {
      if (failed.load()) {
        return;
      }

      auto name{path};
      auto part{std::format("{:05d}", i)}; // should be matching num_batches

      name.replace_extension(".data." + part);

      std::ofstream of(name, std::ios::binary);
      if (!of) {
        failed.store(true);
        std::lock_guard lock(err_mtx);
        err_msg = std::format("could not create partition {} file", name.string());
        return;
      }

      auto first = std::next(data_.begin(), i * batch_size_);
      auto last = std::min(first + batch_size_, data_.end());

      try {
        cereal::BinaryOutputArchive archive{of};
        for (; first != last; ++first) {
          archive(*first);
        }
      } catch (const std::exception& e) {
        failed.store(true);
        std::lock_guard lock(err_mtx);
        err_msg = std::format("serialization of {} failed {}", name.string(), e.what());
        return;
      }
    });

    if (failed.load()) {
      return std::unexpected{err_msg};
    }

    failed = false;
    err_msg = {};

    std::ranges::for_each(indices.begin(), indices.end(), [&](std::size_t i) {
      if (failed) {
        return;
      }

      auto name{path};
      auto part{std::format("{:05d}", i)}; // should be matching num_batches
      name.replace_extension(".data." + part);

      cleanup_list.push_back(name);

      if (!add_to_zip(name)) {
        std::string msg{zip_strerror(za.get())};
        failed = true;
        err_msg = std::format("failed to zip {}, {}", name.string(), msg);
      }
    });

    if (failed) {
      return std::unexpected{err_msg};
    }

    if (zip_close(za.get()) < 0) {
      std::string msg{zip_strerror(za.get())};
      return std::unexpected{"failed to finalize zip archive, " + msg};
    }

    za.release();

    // now is time for cleanup!!!
    for (const auto& name : cleanup_list) {
      std::error_code ec;
      fs::remove(name, ec);
    }

    return fs::file_size(path);
  }

  private:
    friend class builder;

    // batching factor for load/save
    static const std::size_t batch_size_{1'000'000};

    [[nodiscard]] auto m_find_kmer_pos_(const std::string& s) const -> std::optional<std::size_t> {
      if (!detail::is_valid(s, k_)) {
        return std::nullopt;
      }

      auto kmer = detail::kmer_to_binary(s);
      auto kmer_idx = std::min(kmer, detail::reverse_complement(kmer, k_));

      return index_.find(kmer_idx);
    }

  [[nodiscard]] auto m_traverse_kmer_(std::size_t kmer_pos) const -> std::generator<std::size_t> {
    for (auto label_idx : data_[kmer_pos].select_idxs()) {
      co_yield label_idx;
    }
  }

  std::size_t k_{0};
  std::size_t s_{0};
  std::size_t t_{0};

  std::vector<detail::encoding> data_;
  std::vector<std::string> labels_;
  kmer_index index_;
};

} // namespace skim

#endif // SKIMDB_H
