// Copyright (c) 2015-2019 Daniel Cooke
// Use of this source code is governed by the MIT license that can be found in the LICENSE file.

#include "somatic_random_forest_filter.hpp"

#include <utility>

#include "../measures/is_somatic.hpp"
#include "../measures/is_refcall.hpp"

namespace octopus { namespace csr {

const std::string SomaticRandomForestVariantCallFilter::call_quality_name_ = "RFQUAL_ALL";

SomaticRandomForestVariantCallFilter::SomaticRandomForestVariantCallFilter(FacetFactory facet_factory,
                                                                           std::vector<MeasureWrapper> measures,
                                                                           Path germline_forest, Path somatic_forest,
                                                                           OutputOptions output_config,
                                                                           ConcurrencyPolicy threading,
                                                                           Path temp_directory,
                                                                           boost::optional<ProgressMeter&> progress)
: ConditionalRandomForestFilter {
    std::move(facet_factory),
    std::move(measures),
    {make_wrapped_measure<IsSomatic>(true), make_wrapped_measure<IsRefcall>(true)},
    [] (const MeasureVector& measures) -> std::int8_t {
        assert(measures.size() == 2);
        if (boost::get<bool>(measures.front())) {
            return 1;
        } else if (boost::get<bool>(measures.back())) {
            return 1;
        } else {
            return 0;
        }},
    {std::move(germline_forest), std::move(somatic_forest)},
    std::move(output_config),
    std::move(threading),
    std::move(temp_directory),
    progress
} {}

SomaticRandomForestVariantCallFilter::SomaticRandomForestVariantCallFilter(FacetFactory facet_factory,
                                                                           std::vector<MeasureWrapper> measures,
                                                                           Path germline_forest, Path somatic_forest,
                                                                           Phred<double> min_forest_quality,
                                                                           OutputOptions output_config,
                                                                           ConcurrencyPolicy threading,
                                                                           Path temp_directory,
                                                                           boost::optional<ProgressMeter&> progress)
: ConditionalRandomForestFilter {
    std::move(facet_factory),
    std::move(measures),
    {make_wrapped_measure<IsSomatic>(true), make_wrapped_measure<IsRefcall>(true)},
    [] (const MeasureVector& measures) -> std::int8_t {
     assert(measures.size() == 2);
        if (boost::get<bool>(measures.front())) {
            return 1;
        } else if (boost::get<bool>(measures.back())) {
            return 1;
        } else {
            return 0;
        }},
    {std::move(germline_forest), std::move(somatic_forest)},
    min_forest_quality,
    std::move(output_config),
    std::move(threading),
    std::move(temp_directory),
    progress
} {}

SomaticRandomForestVariantCallFilter::SomaticRandomForestVariantCallFilter(FacetFactory facet_factory,
                                                                           std::vector<MeasureWrapper> measures,
                                                                           Path somatic_forest,
                                                                           OutputOptions output_config,
                                                                           ConcurrencyPolicy threading,
                                                                           Path temp_directory,
                                                                           boost::optional<ProgressMeter&> progress)
: ConditionalRandomForestFilter {
    std::move(facet_factory),
    std::move(measures),
    {make_wrapped_measure<IsSomatic>(false)},
    [] (const MeasureVector& measures) -> std::int8_t {
        assert(measures.size() == 1);
        return !boost::get<bool>(measures.front());
        },
    {std::move(somatic_forest)},
    std::move(output_config),
    std::move(threading),
    std::move(temp_directory),
    progress
} {}

SomaticRandomForestVariantCallFilter::SomaticRandomForestVariantCallFilter(FacetFactory facet_factory,
                                                                           std::vector<MeasureWrapper> measures,
                                                                           Path somatic_forest,
                                                                           Phred<double> min_forest_quality,
                                                                           OutputOptions output_config,
                                                                           ConcurrencyPolicy threading,
                                                                           Path temp_directory,
                                                                           boost::optional<ProgressMeter&> progress)
: ConditionalRandomForestFilter {
    std::move(facet_factory),
    std::move(measures),
    {make_wrapped_measure<IsSomatic>(false)},
    [] (const MeasureVector& measures) -> std::int8_t {
        assert(measures.size() == 1);
        return !boost::get<bool>(measures.front());
    },
    {std::move(somatic_forest)},
    min_forest_quality,
    std::move(output_config),
    std::move(threading),
    std::move(temp_directory),
    progress
} {}

void SomaticRandomForestVariantCallFilter::annotate(VcfHeader::Builder& header) const
{
    ConditionalRandomForestFilter::annotate(header);
    header.add_info(call_quality_name_, "1", "Float", "Combined quality score for call using product of sample RFQUAL");
}

bool SomaticRandomForestVariantCallFilter::is_soft_filtered(const ClassificationList& sample_classifications,
                                                            const MeasureVector& measures) const
{
    return std::any_of(std::cbegin(sample_classifications), std::cend(sample_classifications),
                       [] (const auto& c) { return c.category != Classification::Category::unfiltered; });
}

boost::optional<std::string> SomaticRandomForestVariantCallFilter::call_quality_name() const
{
    return call_quality_name_;
}

} // namespace csr
} // namespace octopus
