// Copyright (c) 2016 Daniel Cooke
// Use of this source code is governed by the MIT license that can be found in the LICENSE file.

#include "denovo_model.hpp"

#include <memory>
#include <iterator>
#include <algorithm>
#include <numeric>
#include <cmath>
#include <cstdint>
#include <cassert>

#include "basics/phred.hpp"
#include "core/types/variant.hpp"
//#include "../pairhmm/pair_hmm.hpp"

#include <iostream>

namespace octopus {

DeNovoModel::DeNovoModel(Parameters parameters, std::size_t num_haplotypes_hint, CachingStrategy caching)
: parameters_ {parameters}
, num_haplotypes_hint_ {num_haplotypes_hint}
, caching_ {caching}
, value_cache_ {}
, address_cache_ {}
{
    if (caching_ == CachingStrategy::address) {
        address_cache_.reserve(num_haplotypes_hint_);
    } else if (caching == CachingStrategy::value) {
        value_cache_.reserve(num_haplotypes_hint_);
    }
}

//auto make_hmm_model(const double denovo_mutation_rate) noexcept
//{
//    const auto p = static_cast<std::int8_t>(probability_to_phred(denovo_mutation_rate).score());
//    return hmm::BasicMutationModel {p, p, p};
//}
//
//auto pad(const Haplotype::NucleotideSequence& given, const std::size_t target_size)
//{
//    auto required_pad = 2 * hmm::min_flank_pad();
//    const auto given_size = given.size();
//    if (target_size > given_size) {
//        required_pad += target_size - given_size;
//    } else if (given_size > target_size) {
//        const auto excess = given_size - target_size;
//        if (excess >= required_pad) {
//            return given;
//        } else {
//            required_pad -= excess;
//        }
//    }
//    Haplotype::NucleotideSequence result(given.size() + required_pad, 'N');
//    std::copy(std::cbegin(given), std::cend(given),
//              std::next(std::begin(result), hmm::min_flank_pad()));
//    return result;
//}

double DeNovoModel::evaluate(const Haplotype& target, const Haplotype& given) const
{
    if (caching_ == CachingStrategy::address) {
        return evaluate_address_cache(target, given);
    } else if (caching_ == CachingStrategy::value) {
        return evaluate_basic_cache(target, given);
    } else {
        return evaluate_uncached(target, given);
    }
}

// private methods

double DeNovoModel::evaluate_uncached(const Haplotype& target, const Haplotype& given) const
{
    // TODO: make indel errors context based
    const auto variants = target.difference(given);
    return std::accumulate(std::cbegin(variants), std::cend(variants), 0.0,
                           [this] (const double curr, const Variant& v) {
                               double penalty {std::log(parameters_.mutation_rate)};
//                               if (is_insertion(v)) {
//                                   penalty *= alt_sequence_size(v);
//                               } else if (is_deletion(v)) {
//                                   penalty *= region_size(v);
//                               }
                               return curr + penalty;
                           });
//    const auto model = make_hmm_model(parameters_.mutation_rate);
//    const auto result = hmm::evaluate(target.sequence(), pad(given.sequence(), sequence_size(target)), model);
}

double DeNovoModel::evaluate_basic_cache(const Haplotype& target, const Haplotype& given) const
{
    const auto target_iter = value_cache_.find(target);
    if (target_iter != std::cend(value_cache_)) {
        const auto given_iter = target_iter->second.find(given);
        if (given_iter != std::cend(target_iter->second)) {
            return given_iter->second;
        }
    }
    const auto result = evaluate_uncached(target, given);
    if (target_iter != std::cend(value_cache_)) {
        target_iter->second.emplace(given, result);
    } else {
        auto p = value_cache_.emplace(std::piecewise_construct,
                                      std::forward_as_tuple(target),
                                      std::forward_as_tuple(num_haplotypes_hint_));
        assert(p.second);
        p.first->second.emplace(given, result);
    }
    return result;
}

double DeNovoModel::evaluate_address_cache(const Haplotype& target, const Haplotype& given) const
{
    const auto target_iter = address_cache_.find(std::addressof(target));
    if (target_iter != std::cend(address_cache_)) {
        const auto given_iter = target_iter->second.find(std::addressof(given));
        if (given_iter != std::cend(target_iter->second)) {
            return given_iter->second;
        }
    }
    const auto result = evaluate_uncached(target, given);
    if (target_iter != std::cend(address_cache_)) {
        target_iter->second.emplace(std::addressof(given), result);
    } else {
        auto p = address_cache_.emplace(std::piecewise_construct,
                                        std::forward_as_tuple(std::addressof(target)),
                                        std::forward_as_tuple(num_haplotypes_hint_));
        assert(p.second);
        p.first->second.emplace(std::addressof(given), result);
    }
    return result;
}

} // namespace octopus
