
#ifndef SKIMDB_QUERY_H
#define SKIMDB_QUERY_H

#include <filesystem>
#include <generator>
#include <string>
#include <unordered_map>
#include <vector>
#include <fstream>

#include <cereal/archives/binary.hpp>
#include <cereal/types/vector.hpp>
#include <cereal/types/string.hpp>
#include <cereal/types/unordered_map.hpp>

#include "skimdb/skimdb_util.h"
#include "skimdb/skimdb_encoding.h"

namespace skim {

    namespace fs = std::filesystem;

    class skim_db_builder;

    class skim_db {
    public:
        skim_db() = default;

        // given a kmer, returns an iterator over the annotations
        std::generator<std::string> query(std::string kmer) {
            if(!kmer_is_valid(k_, kmer)) {
                co_return;
            }

            auto kmer_rec = kmer_to_index_.find(kmer_to_uint32(kmer));
            if (kmer_rec == kmer_to_index_.end()) {
                co_return;
            }

            std::size_t kmer_idx = kmer_rec->second;
            if (kmer_idx >= data_.size()) {
                co_return;
            }

            for (std::size_t label_idx : data_[kmer_idx].select_idxs()) {
                if (label_idx >= labels_.size()) {
                    break;
                }

                co_yield labels_[label_idx];
            }
        } // query

        // save data to skim_db file
        bool save(const fs::path& path) {
            try {
                std::ofstream os(path, std::ios::binary);
                if (!os) return false;
                
                cereal::BinaryOutputArchive archive(os);
                archive(*this);  // Uses serialize() internally
                return true;
            } catch (...) {
                return false;
            }
        } // save

        // load data from skim_db file
        bool load(const fs::path& path) {
            try {
                std::ifstream is(path, std::ios::binary);
                if (!is) return false;
                
                cereal::BinaryInputArchive archive(is);
                archive(*this);
                return true;
            } catch (...) {
                return false;
            }
        } // load

        template<class Archive>
        void serialize(Archive& archive) {
            archive(k_, s_, t_, labels_, kmer_to_index_, data_);
        } // serialize

    private:
        friend class skim_db_builder;

        std::size_t k_;
        std::size_t s_;
        std::size_t t_;
        std::vector<std::string> labels_;
        std::unordered_map<std::uint32_t, std::size_t> kmer_to_index_;
        std::vector<skim::encoding> data_;
    
    };

}

#endif // SKIMDB_QUERY_H
