#ifndef SKIMDB_SPIR_RLWE_H
#define SKIMDB_SPIR_RLWE_H

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <random>
#include <stdexcept>
#include <vector>

#include <seal/seal.h>
#include <seal/util/polyarithsmallmod.h>
#include <seal/util/rlwe.h>

#include "skimdb_spir_matrix.h"
#include "skimdb_spir_definitions.h"

namespace skim {
namespace spir {
namespace rlwe {

inline constexpr std::size_t SEAL_SEED_LENGTH = 8;  // 512-bit PRNG seed


class RLWEContext {
public:
    explicit RLWEContext(std::uint64_t poly_degree, std::uint32_t log_q, std::uint64_t plaintext_mod)
        : poly_degree_{poly_degree}, log_q_{log_q}, p_mod_{plaintext_mod}
    {
        seal::EncryptionParameters parms(seal::scheme_type::bfv);
        parms.set_poly_modulus_degree(poly_degree);
        parms.set_plain_modulus(plaintext_mod);

        switch (log_q) {
        case 32:
            parms.set_coeff_modulus(seal::CoeffModulus::Create(poly_degree, {33}));
            break;
        case 64:
            parms.set_coeff_modulus(seal::CoeffModulus::Create(poly_degree, {32, 33, 33}));
            break;
        default:
            throw std::invalid_argument("unsupported log_q for RLWE: must be 32 or 64");
        }

        context_ = std::make_unique<seal::SEALContext>(parms);
        if (!context_->parameters_set()) {
            throw std::runtime_error(
                std::string("SEAL parameter error: ") + context_->parameter_error_message());
        }

        evaluator_ = std::make_unique<seal::Evaluator>(*context_);
    }

    [[nodiscard]] auto poly_degree() const -> std::uint64_t { return poly_degree_; }
    [[nodiscard]] auto log_q() const -> std::uint32_t { return log_q_; }
    [[nodiscard]] auto plaintext_mod() const -> std::uint64_t { return p_mod_; }
    [[nodiscard]] auto seal_context() -> seal::SEALContext& { return *context_; }
    [[nodiscard]] auto seal_context() const -> const seal::SEALContext& { return *context_; }

    void mod_switch(
        const std::uint64_t* inp,
        std::uint64_t* out,
        std::uint64_t samples) const
    {
        auto ctx_data = context_->first_context_data();
        auto num_moduli = ctx_data->parms().coeff_modulus().size();
        auto q_rns_base = ctx_data->rns_tool()->base_q();
        auto inv_punc_prods = q_rns_base->inv_punctured_prod_mod_base_array();
        auto moduli = q_rns_base->base();

        for (std::uint64_t i = 0; i < samples; ++i) {
            __int128 result = 0;
            for (std::size_t j = 0; j < num_moduli; ++j) {
                std::uint64_t t1 = seal::util::multiply_uint_mod(
                    inp[j * samples + i], inv_punc_prods[j], moduli[j]);
                result += (static_cast<__int128>(t1) << log_q_) / static_cast<__int128>(moduli[j].value());
            }
            out[i] = static_cast<std::uint64_t>(result);
        }
    }

    void round_lwe(std::uint64_t* lwe, std::size_t length) const {
        std::uint64_t delta = (log_q_ >= 64) ? (0xFFFFFFFFFFFFFFFFULL / p_mod_ + 1) : ((1ULL << log_q_) / p_mod_);
        std::uint64_t half_delta = delta / 2;

        for (std::size_t i = 0; i < length; ++i) {
            lwe[i] = ((lwe[i] + half_delta) / delta) % p_mod_;
        }
    }

private:
    std::uint64_t poly_degree_;
    std::uint32_t log_q_;
    std::uint64_t p_mod_;

    std::unique_ptr<seal::SEALContext> context_;
    std::unique_ptr<seal::Evaluator> evaluator_;
};


class RLWEKey {
public:
    explicit RLWEKey(RLWEContext& ctx)
        : ctx_{ctx},
          keygen_{ctx.seal_context(), true, true},
          secret_key_{keygen_.secret_key()},
          encryptor_{ctx.seal_context(), secret_key_},
          decryptor_{ctx.seal_context(), secret_key_}
    {}

    [[nodiscard]] auto extract_lwe_key() const -> spir_matrix {
        auto ctx_data = ctx_.seal_context().first_context_data();
        auto poly_degree = ctx_data->parms().poly_modulus_degree();
        auto q0 = ctx_data->parms().coeff_modulus()[0].value();
        auto boundary = (q0 - 1) / 2;

        auto raw_sk = keygen_.secret_key_coeff().data().data();

        spir_matrix lwe_s{poly_degree, ctx_.log_q()};
        auto s_data = lwe_s.span();

        for (std::uint64_t i = 0; i < poly_degree; ++i) {
            if (raw_sk[i] > boundary) {
                s_data[i] = static_cast<std::uint64_t>(0) - (q0 - raw_sk[i]);
            } else {
                s_data[i] = raw_sk[i];
            }
        }

        return lwe_s;
    }

    [[nodiscard]] auto encrypt(
        const std::uint64_t* pt_data,
        std::uint64_t num_slots,
        seal::DynArray<std::uint64_t>& a_poly) -> std::vector<std::uint8_t>
    {
        seal::Plaintext pt;
        pt.resize(num_slots);
        for (std::uint64_t i = 0; i < num_slots; ++i) {
            pt[i] = pt_data[i];
        }

        seal::Ciphertext ct(seal::MemoryPoolHandle::Global());
        encryptor_.preprocess_encrypt_symmetric(ct, a_poly);
        encryptor_.encrypt_symmetric_preprocessed(pt, ct);

        if (num_slots < ctx_.poly_degree()) {
            truncate_ct(ct, num_slots);
        }

        auto& ct_array = ct.dyn_array();
        std::size_t sz = ct_array.save_size(seal::compr_mode_type::none);
        std::vector<std::uint8_t> buf(sz);
        ct_array.save(reinterpret_cast<seal::seal_byte*>(buf.data()), sz, seal::compr_mode_type::none);

        return buf;
    }

    [[nodiscard]] auto get_a_poly(const std::vector<std::uint64_t>& seed) -> seal::DynArray<std::uint64_t> {
        seal::prng_seed_type seal_seed;
        std::copy(seed.begin(), seed.end(), seal_seed.begin());

        seal::DynArray<std::uint64_t> a(seal::MemoryPoolHandle::Global());
        encryptor_.get_a(a, seal_seed);
        return a;
    }

    auto encryptor() -> seal::Encryptor& { return encryptor_; }
    auto keygen() -> seal::KeyGenerator& { return keygen_; }

private:
    void truncate_ct(seal::Ciphertext& ct, std::uint64_t size) {
        auto ctx_data = ctx_.seal_context().first_context_data();
        auto coeff_modulus_size = ctx_data->parms().coeff_modulus().size();
        auto coeff_count = ctx_data->parms().poly_modulus_degree();

        seal::DynArray<std::uint64_t> tmp(size * coeff_modulus_size);
        const auto& orig = ct.dyn_array();

        for (std::size_t i = 0; i < coeff_modulus_size; ++i) {
            for (std::uint64_t j = 0; j < size; ++j) {
                tmp[i * size + j] = orig[i * coeff_count + j];
            }
        }

        ct.set_array(tmp);
    }

    RLWEContext& ctx_;
    seal::KeyGenerator keygen_;
    seal::SecretKey secret_key_;
    seal::Encryptor encryptor_;
    seal::Decryptor decryptor_;
};


inline auto ct_extract_lwe(
    RLWEContext& ctx,
    const std::vector<std::uint8_t>& ct_bytes,
    std::uint64_t num_samples) -> spir_matrix
{
    seal::DynArray<std::uint64_t> ct_array;
    ct_array.load(
        reinterpret_cast<const seal::seal_byte*>(ct_bytes.data()),
        ct_bytes.size());

    spir_matrix lwe_ct{num_samples, ctx.log_q()};
    ctx.mod_switch(ct_array.begin(), lwe_ct.span().data(), num_samples);

    return lwe_ct;
}


inline void a_transpose(
    std::shared_ptr<const seal::SEALContext::ContextData> ctx_data,
    seal::DynArray<std::uint64_t>& a)
{
    auto poly_degree = ctx_data->parms().poly_modulus_degree();
    auto moduli = ctx_data->parms().coeff_modulus();

    seal::util::inverse_ntt_negacyclic_harvey_lazy(
        seal::util::RNSIter(a.begin(), poly_degree),
        moduli.size(),
        seal::util::iter(ctx_data->small_ntt_tables()));

    for (std::size_t j = 0; j < moduli.size(); ++j) {
        for (std::uint64_t i = 0; i < poly_degree; ++i) {
            a[j * poly_degree + i] %= moduli[j].value();
        }
    }

    // x -> x^{-1} in the negacyclic ring: swap a[i] <-> -a[n-i]
    for (std::size_t j = 0; j < moduli.size(); ++j) {
        auto* limb = &a[j * poly_degree];
        auto q_j = moduli[j].value();
        for (std::uint64_t i = 1; i <= poly_degree / 2; ++i) {
            if (i == poly_degree - i) {
                limb[i] = q_j - limb[i];
            } else {
                std::uint64_t neg_t1 = q_j - limb[i];
                limb[i] = q_j - limb[poly_degree - i];
                limb[poly_degree - i] = neg_t1;
            }
        }
    }

    seal::util::ntt_negacyclic_harvey_lazy(
        seal::util::RNSIter(a.begin(), poly_degree),
        moduli.size(),
        seal::util::iter(ctx_data->small_ntt_tables()));
}


inline auto compute_hint_ntt(
    RLWEContext& ctx,
    RLWEKey& key,
    const skimdb_matrix& db,
    const std::vector<std::uint64_t>& seeds,
    std::uint64_t num_seeds,
    std::uint64_t sqrt_N,
    std::uint64_t rle_blocks) -> spir_matrix
{
    auto ctx_data = ctx.seal_context().first_context_data();
    auto poly_degree = ctx_data->parms().poly_modulus_degree();
    auto moduli = ctx_data->parms().coeff_modulus();
    auto num_moduli = moduli.size();
    auto log_q = ctx.log_q();

    std::uint64_t n_lwe = poly_degree * num_seeds;

    std::vector<seal::DynArray<std::uint64_t>> As(num_seeds);
    for (std::size_t i = 0; i < num_seeds; ++i) {
        std::vector<std::uint64_t> seed(
            seeds.begin() + i * SEAL_SEED_LENGTH,
            seeds.begin() + (i + 1) * SEAL_SEED_LENGTH);

        As[i] = key.get_a_poly(seed);
        a_transpose(ctx_data, As[i]);
    }

    spir_matrix hint{sqrt_N, n_lwe, log_q};

    auto rns_size = poly_degree * num_moduli;
    std::vector<std::uint64_t> accum(rns_size);
    std::vector<std::uint64_t> tmp(rns_size);
    std::vector<std::uint64_t> row_pt(rns_size);

    auto [db_rows, db_cols] = db.dimensions();

    for (std::uint64_t row = 0; row < db_rows; ++row) {
        std::fill(accum.begin(), accum.end(), 0);

        for (std::size_t seed_idx = 0; seed_idx < num_seeds; ++seed_idx) {
            std::fill(row_pt.begin(), row_pt.end(), 0);

            std::uint64_t stop = std::min(poly_degree, db_cols - seed_idx * poly_degree);
            std::uint64_t rle_row = row / rle_blocks;
            std::uint64_t rle_offset = row % rle_blocks;

            for (std::size_t j_mod = 0; j_mod < num_moduli; ++j_mod) {
                for (std::uint64_t z = 0; z < stop; ++z) {
                    std::uint64_t col_idx = seed_idx * poly_degree + z;
                    auto rle = db.get_rle_in_col(rle_row, col_idx);
                    std::uint64_t val = 0;

                    switch (db.get_block_len()) {
                    case 1: {
                        auto ptr = reinterpret_cast<const std::uint8_t*>(rle.data());
                        if (rle_offset < rle.size() * 2) {
                            val = ptr[rle_offset];
                        }
                        break;
                    }
                    case 2: {
                        if (rle_offset < rle.size()) {
                            val = rle.data()[rle_offset];
                        }
                        break;
                    }
                    case 3: {
                        auto ptr = reinterpret_cast<const std::uint8_t*>(rle.data());
                        auto byte_idx = rle_offset * 3;
                        auto byte_len = rle.size() * 2;
                        if (byte_idx + 2 < byte_len) {
                            val = (static_cast<std::uint64_t>(ptr[byte_idx]) << 16)
                                | (static_cast<std::uint64_t>(ptr[byte_idx + 1]) << 8)
                                | static_cast<std::uint64_t>(ptr[byte_idx + 2]);
                        }
                        break;
                    }
                    }

                    row_pt[j_mod * poly_degree + z] = val % moduli[j_mod].value();
                }
            }

            seal::util::ntt_negacyclic_harvey_lazy(
                seal::util::RNSIter(row_pt.data(), poly_degree),
                num_moduli,
                seal::util::iter(ctx_data->small_ntt_tables()));

            seal::util::dyadic_product_coeffmod(
                seal::util::RNSIter(As[seed_idx].begin(), poly_degree),
                seal::util::ConstRNSIter(row_pt.data(), poly_degree),
                num_moduli,
                moduli,
                seal::util::RNSIter(tmp.data(), poly_degree));

            for (std::size_t k = 0; k < rns_size; ++k) {
                accum[k] += tmp[k];
            }
        }

        for (std::size_t j = 0; j < num_moduli; ++j) {
            for (std::uint64_t z = 0; z < poly_degree; ++z) {
                accum[j * poly_degree + z] %= moduli[j].value();
            }
        }

        seal::util::inverse_ntt_negacyclic_harvey_lazy(
            seal::util::RNSIter(accum.data(), poly_degree),
            num_moduli,
            seal::util::iter(ctx_data->small_ntt_tables()));

        std::vector<std::uint64_t> row_out(poly_degree);
        ctx.mod_switch(accum.data(), row_out.data(), poly_degree);

        for (std::uint64_t j = 0; j < poly_degree; ++j) {
            hint.set(row, j, row_out[j]);
        }
    }

    return hint;
}


inline auto gen_a_seeds(
    std::mt19937_64& rng,
    std::uint64_t sqrt_N,
    std::uint64_t poly_degree) -> std::pair<std::vector<std::uint64_t>, std::uint64_t>
{
    std::uint64_t num_seeds = (sqrt_N + poly_degree - 1) / poly_degree;
    std::vector<std::uint64_t> seeds(num_seeds * SEAL_SEED_LENGTH);

    for (auto& s : seeds) {
        s = rng();
    }

    return {std::move(seeds), num_seeds};
}


inline auto prepare_query_hybrid(
    RLWEContext& ctx,
    RLWEKey& key,
    const std::vector<std::uint64_t>& a_seeds,
    std::uint64_t num_seeds,
    const std::uint64_t* pt_data,
    std::uint64_t sqrt_N) -> spir_matrix
{
    auto poly_degree = ctx.poly_degree();
    auto log_q = ctx.log_q();

    spir_matrix qu{sqrt_N, log_q};
    auto qu_data = qu.span();

    for (std::uint64_t i = 0; i < num_seeds; ++i) {
        std::vector<std::uint64_t> seed(
            a_seeds.begin() + i * SEAL_SEED_LENGTH,
            a_seeds.begin() + (i + 1) * SEAL_SEED_LENGTH);
        auto a_poly = key.get_a_poly(seed);

        std::uint64_t start = i * poly_degree;
        std::uint64_t num_slots = std::min(poly_degree, sqrt_N - start);

        auto ct_bytes = key.encrypt(pt_data + start, num_slots, a_poly);
        auto lwe_ct = ct_extract_lwe(ctx, ct_bytes, num_slots);
        auto lwe_data = lwe_ct.span();

        for (std::uint64_t j = 0; j < num_slots; ++j) {
            qu_data[start + j] = lwe_data[j];
        }
    }

    return qu;
}


inline auto recover_hybrid(
    RLWEContext& ctx,
    const spir_matrix& ans,
    const spir_matrix& hint,
    const spir_matrix& lwe_secret,
    std::uint32_t log_q,
    std::uint64_t i_row,
    std::uint64_t n_rows) -> spir_matrix
{
    auto d = sub_mat_vec_rows(ans, hint, lwe_secret.span(), log_q, i_row, n_rows);
    ctx.round_lwe(d.span().data(), n_rows);
    return d;
}


} // namespace rlwe
} // namespace spir
} // namespace skim

#endif // SKIMDB_SPIR_RLWE_H
