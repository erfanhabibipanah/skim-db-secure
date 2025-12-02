
#ifndef SKIMDB_UTIL_H
#define SKIMDB_UTIL_H

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <utility>
#include <cstdint>

#include <roaring.hh>

#include <fastxrd/fasta_simple_reader.h>
#include <fastxrd/fastx_files_reader.h>

namespace skim {

    namespace fs = std::filesystem;

    // Parse file_to_labels file into pair of vectors (file_names, labels)
    // Returns empty on failure, skips ill-formatted lines
    std::pair<std::vector<std::string>, std::vector<std::string>> load_file_to_labels(const fs::path& path) {
        std::vector<std::string> file_names;
        std::vector<std::string> labels;

        std::ifstream infile(path);
        if (!infile.is_open()) {
            return {file_names, labels};
        }

        std::string line;
        while (std::getline(infile, line)) {
            std::istringstream iss(line);
            std::string name;
            std::string id;

            if (iss >> name >> id) {
                file_names.push_back(name);
                labels.push_back(id);
            }
        }

        return {file_names, labels};
    } // load_file_to_labels

    bool kmer_is_valid(std::size_t k, const std::string& kmer) {
        if (kmer.length() != k) {
            return false;
        }

        for (char c : kmer) {
            switch (c) {
                case 'A': case 'a':
                case 'C': case 'c':
                case 'G': case 'g':
                case 'T': case 't':
                    continue;
                default:
                    return false;
            }
        }

        return true;
    } // kmer_is_valid

    int to_base2 (char c) {
        switch(c) {
        case 'A': case 'a': return 0;
        case 'C': case 'c': return 1;
        case 'G': case 'g': return 2;
        case 'T': case 't': return 3;
        default: return -1;
        }
    } // to_base2

    std::uint32_t kmer_to_uint32(const std::string& kmer) {
        if (kmer.length() > 16) {
            return 0; // k-mers longer than 16 not supported
        }
        
        std::uint32_t result = 0;
        for (char c : kmer) {
            int base = to_base2(c);
            if (base == -1) {
                return 0; // invalid character
            }
            result = (result << 2) | static_cast<std::uint32_t>(base);
        }

        return result;
    } // kmer_to_uint32

    std::uint32_t reverse_complement (std::size_t k, std::uint32_t kmer) {
        std::uint32_t rev_comp = 0;

        for (std::size_t i = 0; i < k; ++i) {
            rev_comp = (rev_comp << 2) | (3 - (kmer & 3));
            kmer >>= 2;
        }

        return rev_comp;
    } // reverse_complement

    bool check_syncmer(std::size_t k,
                        std::size_t s,
                        std::size_t t,
                        std::uint32_t kmer) {
        
        if (s == 0 || s >= k) { return true; }

        std::uint32_t smer_mask = (1ULL << (2 * s)) - 1;
        std::size_t num_smers = k - s + 1;

        std::size_t tmer_shift = 2 * (k - s - t);
        std::uint32_t tmer = (kmer >> tmer_shift) & smer_mask;

        for (std::size_t i = 0; i < num_smers; ++i) {
            std::uint32_t smer = kmer & smer_mask;
            if (smer < tmer) { return false; }
            kmer >>= 2;
        }

        return true;
    } // check_syncmer

    // Opens fasta file and processes canonical syncmers into a roaring bitmap
    void populate_bitmap(std::size_t k,
                        std::size_t s,
                        std::size_t t,
                        const fs::path& dir, 
                        const std::string& filename, 
                        roaring::Roaring& bitmap) {
        
        fs::path full_path = dir / filename;
        fastx::fastx_files_reader<fastx::fasta_simple_reader> ffr{full_path};

        for (auto seq : ffr.sequences()) {
            std::string read = std::get<1>(seq);
            std::uint32_t kmer = 0;
            std::uint32_t rev_comp = 0;
            std::uint32_t kmer_mask = (1ULL << (2 * k)) - 1;
            std::size_t base_count = 0;

            for (auto i = 0; i < read.length(); ++i) {
                auto base = to_base2(read[i]);

                if (base < 0) {
                    base_count = 0;
                    continue;
                }

                kmer = (kmer << 2) | base;
                rev_comp = (rev_comp >> 2) | (std::uint32_t(3 - base) << ((k - 1) * 2));
                base_count++;

                if (base_count >= k) {
                    std::uint32_t canonical = std::min((kmer & kmer_mask), (rev_comp & kmer_mask));
                    if (check_syncmer(k, s, t, canonical)) { bitmap.add(canonical); }
                }
            }
        }
    } //populate_bitmap

    std::size_t total_kmer_count (std::size_t k, std::size_t s, std::size_t t) {
        std::uint64_t num_kmers = 1ULL << (2 * k);
        if (s == 0 || s >= k) {
            std::uint64_t num_palindromes = (1ULL << (2 * (k / 2))) * ((k + 1) % 2);
            return static_cast<std::size_t>((num_kmers + num_palindromes ) / 2);
        }

        std::size_t count = 0;
        for (std::uint32_t kmer = 0; kmer < num_kmers; ++kmer) {
            std::uint32_t canonical = std::min(kmer, reverse_complement(k, kmer));

            if (kmer == canonical && check_syncmer(k, s, t, kmer)) { count += 1; }
        }

        return count;
    } // total_kmer_count
}

#endif // SKIMDB_UTIL_H