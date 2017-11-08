// Copyright (c) 2016 Daniel Cooke
// Use of this source code is governed by the MIT license that can be found in the LICENSE file.

#include "allele_frequency.hpp"

#include <algorithm>
#include <iterator>

#include <boost/variant.hpp>

#include "core/tools/read_assigner.hpp"
#include "core/types/allele.hpp"
#include "io/variant/vcf_record.hpp"
#include "io/variant/vcf_spec.hpp"
#include "../facets/read_assignments.hpp"

namespace octopus { namespace csr {

std::unique_ptr<Measure> AlleleFrequency::do_clone() const
{
    return std::make_unique<AlleleFrequency>(*this);
}

void remove_partial_alleles(std::vector<VcfRecord::NucleotideSequence>& genotype)
{
    genotype.erase(std::remove_if(std::begin(genotype), std::end(genotype),
                                  [] (const auto& seq) {
                                      static const std::string deleted_sequence {vcfspec::deletedBase};
                                      return seq == deleted_sequence || seq == vcfspec::missingValue;
                                  }), std::end(genotype));
}

auto num_matching_lhs_bases(const VcfRecord::NucleotideSequence& lhs, const VcfRecord::NucleotideSequence& rhs) noexcept
{
    auto p = std::mismatch(std::cbegin(lhs), std::cend(lhs), std::cbegin(rhs));
    return static_cast<int>(std::distance(std::cbegin(lhs), p.first));
}

auto get_called_alleles(const VcfRecord& call, const VcfRecord::SampleName& sample,
                        const bool trim_padding = false)
{
    auto genotype = get_genotype(call, sample);
    remove_partial_alleles(genotype);
    std::sort(std::begin(genotype), std::end(genotype));
    genotype.erase(std::unique(std::begin(genotype), std::end(genotype)), std::end(genotype));
    const auto call_region = mapped_region(call);
    std::vector<Allele> result {};
    result.reserve(genotype.size());
    if (trim_padding) {
        for (auto& allele : genotype) {
            const auto num_bases_to_remove = num_matching_lhs_bases(call.ref(), allele);
            allele.erase(std::cbegin(allele), std::next(std::cbegin(allele), num_bases_to_remove));
            auto allele_region = expand_lhs(call_region, -num_bases_to_remove);
            result.emplace_back(std::move(allele_region), std::move(allele));
        }
    } else {
        std::transform(std::cbegin(genotype), std::cend(genotype), std::back_inserter(result),
                       [&] (const auto& alt_seq) { return Allele {call_region, alt_seq}; });
    }
    return result;
}

Measure::ResultType AlleleFrequency::do_evaluate(const VcfRecord& call, const FacetMap& facets) const
{
    const auto assignments = boost::get<ReadAssignments::ResultType>(facets.at("ReadAssignments").get());
    double min_freq {1.0}, max_freq {0.0};
    for (const auto& p : assignments) {
        if (call.is_heterozygous(p.first)) {
            auto alleles = get_called_alleles(call, p.first, true);
            if (alleles.size() > 1) {
                // This might not be the case if there are unknown or deleted alleles in the genotype
                auto allele_support = compute_allele_support(alleles, p.second);
                std::vector<double> allele_counts(alleles.size());
                std::size_t read_count {0};
                std::transform(std::cbegin(allele_support), std::cend(allele_support), std::begin(allele_counts),
                               [&] (const auto& p) {
                                   read_count += p.second.size();
                                   return p.second.size();
                               });
                std::sort(std::begin(allele_counts), std::end(allele_counts), std::greater<> {});
                auto major_af = allele_counts.front() / read_count;
                auto minor_af = allele_counts.back() / read_count;
                if (major_af > max_freq) max_freq = major_af;
                if (minor_af < min_freq) min_freq = minor_af;
            }
        }
    }
    return std::min(min_freq, 1.0 - max_freq);
}

std::string AlleleFrequency::do_name() const
{
    return "AF";
}

std::vector<std::string> AlleleFrequency::do_requirements() const
{
    return {"ReadAssignments"};
}

} // namespace csr
} // namespace octopus