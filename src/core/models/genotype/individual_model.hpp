//
//  individual_model.hpp
//  octopus
//
//  Created by Daniel Cooke on 01/04/2016.
//  Copyright © 2016 Oxford University. All rights reserved.
//

#ifndef individual_model_hpp
#define individual_model_hpp

#include <vector>
#include <functional>

#include <boost/optional.hpp>

#include <config/common.hpp>
#include <core/types/haplotype.hpp>
#include <core/models/genotype/coalescent_model.hpp>
#include <core/models/haplotype_likelihood_cache.hpp>
#include <core/types/genotype.hpp>
#include <logging/logging.hpp>

namespace octopus { namespace model {

class IndividualModel
{
public:
    struct Latents
    {
        using GenotypeProbabilityVector = std::vector<double>;
        
        GenotypeProbabilityVector genotype_probabilities;
    };
    
    struct InferredLatents
    {
        Latents posteriors;
        double log_evidence;
    };
    
    IndividualModel() = delete;
    
    IndividualModel(const CoalescentModel& genotype_prior_model,
                    boost::optional<logging::DebugLogger> debug_log = boost::none);
    
    IndividualModel(const IndividualModel&)            = delete;
    IndividualModel& operator=(const IndividualModel&) = delete;
    IndividualModel(IndividualModel&&)                 = delete;
    IndividualModel& operator=(IndividualModel&&)      = delete;
    
    ~IndividualModel() = default;
    
    InferredLatents infer_latents(const std::vector<Genotype<Haplotype>>& genotypes,
                                  const HaplotypeLikelihoodCache& haplotype_likelihoods) const;
    
private:
    const CoalescentModel& genotype_prior_model_;
    
    mutable boost::optional<logging::DebugLogger> debug_log_;
};

} // namesapce model
} // namespace octopus

#endif /* individual_model */
