//
//  snv_error_model.hpp
//  Octopus
//
//  Created by Daniel Cooke on 15/06/2016.
//  Copyright © 2016 Oxford University. All rights reserved.
//

#ifndef snv_error_model_hpp
#define snv_error_model_hpp

#include <vector>
#include <array>
#include <cstdint>

class Haplotype;

namespace Octopus
{
    class SnvErrorModel
    {
    public:
        using MutationVector = std::vector<char>;
        using PenaltyType    = std::int8_t;
        using PenaltyVector  = std::vector<PenaltyType>;
        
        SnvErrorModel() = default;
        
        virtual ~SnvErrorModel() = default;
        
        void evaluate(const Haplotype& haplotype,
                      MutationVector& forward_snv_mask, PenaltyVector& forward_snv_priors,
                      MutationVector& reverse_snv_mask, PenaltyVector& reverse_snv_priors) const;
        
    private:
        static constexpr std::array<std::array<PenaltyType, 51>, 3> Max_qualities_ =
        {{
            {
                125,125,60,55,40,25,20,15,12,11,9,8,7,7,6,6,6,6,6,6,
                6,6,5,5,5,5,5,5,5,5,5,5,5,4,4,4,3,3,3,3,2,2,2,2,2,1,1,1,1,1,1
            },
            {
                125,125,60,60,52,52,38,38,22,22,17,17,15,15,13,13,10,10,10,10,
                8,8,7,6,6,6,6,6,6,5,5,5,5,4,4,4,3,3,3,3,2,2,2,2,2,1,1,1,1,1,1
            },
            {
                125,125,125,55,55,55,40,40,40,25,25,25,19,19,19,11,11,11,9,9,
                9,7,7,6,6,6,6,6,6,5,5,5,5,4,4,4,3,3,3,3,2,2,2,2,2,1,1,1,1,1,1
            }
        }};
    };
} // namespace Octopus

#endif /* snv_error_model_hpp */
