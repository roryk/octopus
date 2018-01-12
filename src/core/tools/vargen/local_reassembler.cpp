// Copyright (c) 2017 Daniel Cooke
// Use of this source code is governed by the MIT license that can be found in the LICENSE file.

#include "local_reassembler.hpp"

#include <algorithm>
#include <iterator>
#include <deque>
#include <stdexcept>
#include <thread>
#include <future>
#include <cassert>

#include "basics/cigar_string.hpp"
#include "concepts/mappable_range.hpp"
#include "utils/mappable_algorithms.hpp"
#include "utils/sequence_utils.hpp"
#include "utils/append.hpp"
#include "io/reference/reference_genome.hpp"
#include "logging/logging.hpp"
#include "utils/global_aligner.hpp"

namespace octopus { namespace coretools {

namespace {

void remove_duplicates(std::vector<unsigned>& kmer_sizes)
{
    std::sort(std::begin(kmer_sizes), std::end(kmer_sizes));
    kmer_sizes.erase(std::unique(std::begin(kmer_sizes), std::end(kmer_sizes)), std::end(kmer_sizes));
}

auto generate_fallback_kmer_sizes(std::vector<unsigned>& result,
                                  const std::vector<unsigned>& default_kmer_sizes,
                                  const unsigned num_fallbacks, const unsigned interval_size)
{
    assert(!default_kmer_sizes.empty());
    result.resize(num_fallbacks);
    auto k = default_kmer_sizes.back();
    std::generate_n(std::begin(result), num_fallbacks,
                    [&k, interval_size] () noexcept -> decltype(k) {
                        return k += interval_size;
                    });
}

} // namespace

LocalReassembler::LocalReassembler(const ReferenceGenome& reference, Options options)
: execution_policy_ {options.execution_policy}
, reference_ {reference}
, default_kmer_sizes_ {std::move(options.kmer_sizes)}
, fallback_kmer_sizes_ {}
, read_buffer_ {}
, max_bin_size_ {options.bin_size}
, max_bin_overlap_ {options.bin_overlap}
, bins_ {}
, masked_sequence_buffer_ {}
, mask_threshold_ {options.mask_threshold}
, min_kmer_observations_ {options.min_kmer_observations}
, max_bubbles_ {options.max_bubbles}
, min_bubble_score_ {options.min_bubble_score}
, max_variant_size_ {options.max_variant_size}
, active_region_generator_ {reference}
{
    if (max_bin_size_ == 0) {
        throw std::runtime_error {"bin size must be greater than zero"};
    }
    if (max_bin_overlap_ >= max_bin_size_) {
        max_bin_overlap_ = max_bin_size_ - 1;
    }
    if (options.fallback_interval_size == 0) {
        throw std::runtime_error {"fallback interval size must be greater than zero"};
    }
    if (default_kmer_sizes_.empty()) return;
    remove_duplicates(default_kmer_sizes_);
    generate_fallback_kmer_sizes(fallback_kmer_sizes_, default_kmer_sizes_,
                                 options.num_fallbacks, options.fallback_interval_size);
}

std::unique_ptr<VariantGenerator> LocalReassembler::do_clone() const
{
    return std::make_unique<LocalReassembler>(*this);
}

LocalReassembler::Bin::Bin(GenomicRegion region)
: region {std::move(region)}
{}

const GenomicRegion& LocalReassembler::Bin::mapped_region() const noexcept
{
    return region;
}

void LocalReassembler::Bin::add(const AlignedRead& read)
{
    if (read_region) {
        read_region = encompassing_region(*read_region, contig_region(read));
    } else {
        read_region = contig_region(read);
    }
    read_sequences.emplace_back(read.sequence());
}

void LocalReassembler::Bin::add(const GenomicRegion& read_region, const NucleotideSequence& read_sequence)
{
    if (this->read_region) {
        this->read_region = encompassing_region(*this->read_region, contig_region(read_region));
    } else {
        this->read_region = contig_region(read_region);
    }
    read_sequences.emplace_back(read_sequence);
}

void LocalReassembler::Bin::clear() noexcept
{
    read_sequences.clear();
    read_sequences.shrink_to_fit();
}

bool LocalReassembler::Bin::empty() const noexcept
{
    return read_sequences.empty();
}

bool LocalReassembler::do_requires_reads() const noexcept
{
    return true;
}

namespace {

bool has_low_quality_flank(const AlignedRead& read, const AlignedRead::BaseQuality good_quality) noexcept
{
    if (is_soft_clipped(read)) {
        if (is_front_soft_clipped(read) && read.base_qualities().front() < good_quality) {
            return true;
        } else {
            return is_back_soft_clipped(read) && read.base_qualities().back() < good_quality;
        }
    } else {
        return false;
    }
}

bool has_low_quality_match(const AlignedRead& read, const AlignedRead::BaseQuality good_quality) noexcept
{
    if (good_quality == 0) return false;
    auto quality_itr = std::cbegin(read.base_qualities());
    return std::any_of(std::cbegin(read.cigar()), std::cend(read.cigar()),
                       [&] (const auto& op) {
                           if (is_match(op)) {
                               auto result = std::any_of(quality_itr, std::next(quality_itr, op.size()),
                                                         [=] (auto q) { return q < good_quality; });
                               std::advance(quality_itr, op.size());
                               return result;
                           } else if (op.advances_sequence()) {
                               std::advance(quality_itr, op.size());
                           }
                           return false;
                       });
}

bool requires_masking(const AlignedRead& read, const AlignedRead::BaseQuality good_quality) noexcept
{
    return has_low_quality_flank(read, good_quality) || has_low_quality_match(read, good_quality);
}

using ExpandedCigarString = std::vector<CigarOperation::Flag>;

auto expand_cigar(const CigarString& cigar, const std::size_t size_hint = 0)
{
    ExpandedCigarString result {};
    result.reserve(size_hint);
    for (const auto& op : cigar) {
        utils::append(result, op.size(), op.flag());
    }
    return result;
}

auto expand_cigar(const AlignedRead& read)
{
    return expand_cigar(read.cigar(), sequence_size(read));
}

auto find_first_sequence_op(const ExpandedCigarString& cigar) noexcept
{
    return std::find_if_not(std::cbegin(cigar), std::cend(cigar),
                            [] (auto op) { return op == CigarOperation::Flag::hardClipped; });
}

bool is_match(const CigarOperation::Flag op) noexcept
{
    switch (op) {
        case CigarOperation::Flag::alignmentMatch:
        case CigarOperation::Flag::sequenceMatch:
        case CigarOperation::Flag::substitution: return true;
        default: return false;
    }
}

template <typename T>
auto make_optional(bool b, T&& value)
{
    if (b) {
        return boost::optional<T> {std::forward<T>(value)};
    } else {
        return boost::optional<T> {};
    }
}

auto transform_low_quality_matches_to_reference(AlignedRead::NucleotideSequence read_sequence,
                                                const AlignedRead::BaseQualityVector& base_qualities,
                                                const AlignedRead::NucleotideSequence& reference_sequence,
                                                const ExpandedCigarString& cigar,
                                                const AlignedRead::BaseQuality min_quality)
{
    auto ref_itr   = std::cbegin(reference_sequence);
    auto cigar_itr = find_first_sequence_op(cigar);
    bool has_masked {false};
    std::transform(std::cbegin(read_sequence), std::cend(read_sequence), std::cbegin(base_qualities),
                   std::begin(read_sequence), [&] (const auto read_base, const auto base_quality) {
        using Flag = CigarOperation::Flag;
        // Deletions are excess reference sequence so we need to move the
        // reference iterator to the next non-deleted read base
        while (cigar_itr != std::cend(cigar) && *cigar_itr == Flag::deletion) {
            ++cigar_itr;
            ++ref_itr;
        }
        const auto op = *cigar_itr++;
        if (is_match(op)) {
            const auto ref_base = *ref_itr++; // Don't forget to increment ref_itr!
            if (base_quality >= min_quality) {
                return read_base;
            } else {
                has_masked = true;
                return ref_base;
            }
        } else {
            if (op != Flag::insertion) ++ref_itr;
            return read_base;
        }
    });
    return make_optional(has_masked, std::move(read_sequence));
}

auto transform_low_quality_matches_to_reference(const AlignedRead& read,
                                                const AlignedRead::BaseQuality min_quality,
                                                const ReferenceGenome& reference)
{
    return transform_low_quality_matches_to_reference(read.sequence(), read.base_qualities(),
                                                      reference.fetch_sequence(mapped_region(read)),
                                                      expand_cigar(read), min_quality);
}

auto get_removable_flank_sizes(const AlignedRead& read, const AlignedRead::BaseQuality min_quality) noexcept
{
    CigarOperation::Size front_clip, back_clip;
    std::tie(front_clip, back_clip) = get_soft_clipped_sizes(read);
    AlignedRead::NucleotideSequence::size_type front {0}, back {0};
    const auto is_low_quality = [min_quality] (auto q) { return q < min_quality; };
    const auto& base_qualities = read.base_qualities();
    if (front_clip > 0) {
        const auto begin = std::cbegin(base_qualities);
        const auto first_good = std::find_if_not(begin, std::next(begin, front_clip), is_low_quality);
        front = std::distance(begin, first_good);
    }
    if (back_clip > 0) {
        const auto begin = std::crbegin(base_qualities);
        const auto first_good = std::find_if_not(begin, std::next(begin, back_clip), is_low_quality);
        back = std::distance(begin, first_good);
    }
    return std::make_pair(front, back);
}

auto mask(const AlignedRead& read, const AlignedRead::BaseQuality min_quality, const ReferenceGenome& reference)
{
    auto result = transform_low_quality_matches_to_reference(read, min_quality, reference);
    if (result && has_low_quality_flank(read, min_quality)) {
        const auto p = get_removable_flank_sizes(read, min_quality);
        assert(p.first + p.second < sequence_size(read));
        result->erase(std::prev(std::cend(*result), p.second), std::cend(*result));
        result->erase(std::cbegin(*result), std::next(std::cbegin(*result), p.first));
    }
    return result;
}

} // namespace

template <typename Container, typename M>
auto overlapped_bins(Container& bins, const M& mappable)
{
    return bases(overlap_range(std::begin(bins), std::end(bins), mappable, BidirectionallySortedTag {}));
}

void LocalReassembler::do_add_read(const SampleName& sample, const AlignedRead& read)
{
    active_region_generator_.add(sample, read);
    read_buffer_[sample].insert(read);
}

void LocalReassembler::do_add_reads(const SampleName& sample, VectorIterator first, VectorIterator last)
{
    active_region_generator_.add(sample, first, last);
    read_buffer_[sample].insert(first, last);
}

void LocalReassembler::do_add_reads(const SampleName& sample, FlatSetIterator first, FlatSetIterator last)
{
    active_region_generator_.add(sample, first, last);
    read_buffer_[sample].insert(first, last);
}

template <typename Container>
void remove_nonoverlapping(Container& candidates, const GenomicRegion& region)
{
    const auto it = std::remove_if(std::begin(candidates), std::end(candidates),
                                   [&region] (const Variant& candidate) {
                                       return !overlaps(candidate, region);
                                   });
    candidates.erase(it, std::end(candidates));
}

auto extract_unique(std::deque<Variant>&& variants)
{
    using std::make_move_iterator;
    std::vector<Variant> result {make_move_iterator(std::begin(variants)), make_move_iterator(std::end(variants))};
    std::sort(std::begin(result), std::end(result));
    result.erase(std::unique(std::begin(result), std::end(result)), std::end(result));
    return result;
}

void remove_oversized(std::vector<Variant>& variants, const Variant::MappingDomain::Size max_size)
{
    variants.erase(std::remove_if(std::begin(variants), std::end(variants),
                                  [max_size] (const auto& variant) {
                                      return region_size(variant) > max_size;
                                  }),
                   std::end(variants));
}

auto extract_final(std::deque<Variant>&& variants, const GenomicRegion& extract_region,
                   const Variant::MappingDomain::Size max_size)
{
    auto result = extract_unique(std::move(variants));
    remove_oversized(result, max_size);
    remove_nonoverlapping(result, extract_region); // as we expanded original region
    return result;
}

namespace debug {

template <typename Range>
void log_active_regions(const Range& regions, boost::optional<logging::DebugLogger>& log)
{
    if (log) {
        auto log_stream = stream(*log);
        log_stream << "Assembler active regions are: ";
        for (const auto& region : regions) log_stream << region << ' ';
    }
}

} // namespace debug

std::vector<Variant> LocalReassembler::do_generate_variants(const GenomicRegion& region)
{
    const auto active_regions = active_region_generator_.generate(region);
    debug::log_active_regions(active_regions, debug_log_);
    for (const auto& active_region : active_regions) {
        prepare_bins(active_region);
        for (const auto& p : read_buffer_) {
            for (const auto& read : overlap_range(p.second, active_region)) {
                auto active_bins = overlapped_bins(bins_, read);
                assert(!active_bins.empty());
                if (requires_masking(read, mask_threshold_)) {
                    auto masked_sequence = mask(read, mask_threshold_, reference_);
                    if (masked_sequence) {
                        masked_sequence_buffer_.emplace_back(std::move(*masked_sequence));
                        for (auto& bin : active_bins) {
                            bin.add(mapped_region(read), std::cref(masked_sequence_buffer_.back()));
                        }
                    }
                } else {
                    for (auto& bin : active_bins) bin.add(read);
                }
            }
        }
    }
    read_buffer_.clear();
    finalise_bins();
    if (bins_.empty()) return {};
    const auto active_bins = overlapped_bins(bins_, region);
	const auto num_bins = size(active_bins);
    std::deque<Variant> candidates {};
    if (execution_policy_ == ExecutionPolicy::seq || num_bins < 2) {
        for (auto& bin : active_bins) {
            if (debug_log_) {
                stream(*debug_log_) << "Assembling " << bin.read_sequences.size()
                                    << " reads in bin " << mapped_region(bin);
            }
            const auto num_default_failures = try_assemble_with_defaults(bin, candidates);
            if (num_default_failures == default_kmer_sizes_.size()) {
                try_assemble_with_fallbacks(bin, candidates);
            }
            bin.clear();
        }
    } else {
        const std::size_t num_threads {4};
        std::vector<std::future<std::deque<Variant>>> bin_futures(std::min(num_bins, num_threads));
        for (auto first_bin = std::begin(active_bins), last_bin = std::end(active_bins); first_bin != last_bin; ) {
            const auto batch_size = std::min(num_threads, static_cast<std::size_t>(std::distance(first_bin, last_bin)));
            const auto next_bin = std::next(first_bin, batch_size);
            auto last_future = std::transform(first_bin, next_bin, std::begin(bin_futures), [&] (Bin& bin) {
                if (debug_log_) {
                    stream(*debug_log_) << "Assembling " << bin.read_sequences.size()
                                            << " reads in bin " << mapped_region(bin);
                }
                return std::async([&] () {
                    std::deque<Variant> result {};
                    const auto num_default_failures = try_assemble_with_defaults(bin, result);
                    if (num_default_failures == default_kmer_sizes_.size()) {
                        try_assemble_with_fallbacks(bin, result);
                    }
                    bin.clear();
                    return result;
                });
            });
            std::for_each(std::begin(bin_futures), last_future, [&] (auto& f) { utils::append(f.get(), candidates); });
            first_bin = next_bin;
        }
    }
    bins_.clear();
    bins_.shrink_to_fit();
    return extract_final(std::move(candidates), region, max_variant_size_);
}

void LocalReassembler::do_clear() noexcept
{
    read_buffer_.clear();
    masked_sequence_buffer_.clear();
    masked_sequence_buffer_.shrink_to_fit();
    bins_.clear();
    bins_.shrink_to_fit();
    active_region_generator_.clear();
}

std::string LocalReassembler::name() const
{
    return "LocalReassembler";
}

// private methods

template <typename MappableTp>
auto decompose(const MappableTp& mappable, const GenomicRegion::Position n,
               const GenomicRegion::Size overlap = 0)
{
    if (overlap >= n) {
        throw std::runtime_error {"decompose: overlap must be less than n"};
    }
    std::vector<GenomicRegion> result {};
    if (n == 0) return result;
    const auto num_elements = region_size(mappable) / (n - overlap);
    if (num_elements == 0) return result;
    result.reserve(num_elements);
    const auto& contig = contig_name(mappable);
    auto curr = mapped_begin(mappable);
    std::generate_n(std::back_inserter(result), num_elements, [&contig, &curr, n, overlap] () {
        auto tmp = curr;
        curr += (n - overlap);
        return GenomicRegion {contig, tmp, tmp + n};
    });
    return result;
}

void LocalReassembler::prepare_bins(const GenomicRegion& region)
{
    assert(bins_.empty() || is_after(region, bins_.back()));
    if (size(region) > max_bin_size_) {
        auto bin_region = expand_rhs(head_region(region), max_bin_size_);
        while (ends_before(bin_region, region)) {
            bins_.push_back(bin_region);
            bin_region = shift(bin_region, max_bin_overlap_);
        }
        if (overlap_size(region, bin_region) > 0) {
            bins_.push_back(*overlapped_region(region, bin_region));
        }
    } else {
        bins_.push_back(region);
    }
}

bool LocalReassembler::should_assemble_bin(const Bin& bin) const
{
    return !bin.empty();
}

void LocalReassembler::finalise_bins()
{
    auto itr = std::remove_if(std::begin(bins_), std::end(bins_),
                              [this] (const Bin& bin) { return !should_assemble_bin(bin); });
    bins_.erase(itr, std::end(bins_));
    for (auto& bin : bins_) {
        if (bin.read_region) {
            bin.region = GenomicRegion {bin.region.contig_name(), *bin.read_region};
        }
    }
    // unique in reverse order as we want to keep bigger bins, which
    // are sorted after smaller bins with the same starting point
    itr = std::unique(std::rbegin(bins_), std::rend(bins_),
                      [] (const Bin& lhs, const Bin& rhs) noexcept {
                          return begins_equal(lhs, rhs);
                      }).base();
    bins_.erase(std::begin(bins_), itr);
}

namespace {

template <typename L>
void log_success(L& log, const char* type, const unsigned k)
{
    if (log) stream(*log, 8) << type << " assembler with kmer size " << k << " completed";
}

template <typename L>
void log_partial_success(L& log, const char* type, const unsigned k)
{
    if (log) stream(*log, 8) << type << " assembler with kmer size " << k << " partially completed";
}

template <typename L>
void log_failure(L& log, const char* type, const unsigned k)
{
    if (log) stream(*log, 8) << type << " assembler with kmer size " << k << " failed";
}

} // namespace

unsigned LocalReassembler::try_assemble_with_defaults(const Bin& bin, std::deque<Variant>& result) const
{
    unsigned num_failures {0};
    for (const auto k : default_kmer_sizes_) {
        const auto status = assemble_bin(k, bin, result);
        switch (status) {
            case AssemblerStatus::success:
                log_success(debug_log_, "Default", k);
                break;
            case AssemblerStatus::partial_success:
                log_partial_success(debug_log_, "Default", k);
                ++num_failures;
                break;
            default:
                log_failure(debug_log_, "Default", k);
                ++num_failures;
        }
    }
    return num_failures;
}

void LocalReassembler::try_assemble_with_fallbacks(const Bin& bin, std::deque<Variant>& result) const
{
    for (const auto k : fallback_kmer_sizes_) {
        const auto status = assemble_bin(k, bin, result);
        switch (status) {
            case AssemblerStatus::success:
                log_success(debug_log_, "Fallback", k);
                return;
            case AssemblerStatus::partial_success:
                log_partial_success(debug_log_, "Fallback", k);
                break;
            default:
                log_failure(debug_log_, "Fallback", k);
        }
    }
}

GenomicRegion LocalReassembler::propose_assembler_region(const GenomicRegion& input_region, unsigned kmer_size) const
{
    if (input_region.begin() < kmer_size) {
        const auto& contig = input_region.contig_name();
        if (reference_.get().contig_size(contig) >= kmer_size) {
            return GenomicRegion {contig, 0, input_region.end() + kmer_size};
        } else {
            return reference_.get().contig_region(contig);
        }
    } else {
        auto ideal_proposal = expand(input_region, kmer_size);
        if (reference_.get().contains(ideal_proposal)) {
            return ideal_proposal;
        } else {
            const auto& contig = input_region.contig_name();
            const auto end = reference_.get().contig_size(contig);
            return GenomicRegion {contig, input_region.begin() - kmer_size, end};
        }
    }
}

LocalReassembler::AssemblerStatus
LocalReassembler::assemble_bin(const unsigned kmer_size, const Bin& bin, std::deque<Variant>& result) const
{
    if (bin.empty()) return AssemblerStatus::success;
    const auto assemble_region = propose_assembler_region(bin.region, kmer_size);
    if (size(assemble_region) < kmer_size) return AssemblerStatus::failed;
    const auto reference_sequence = reference_.get().fetch_sequence(assemble_region);
    if (!utils::is_canonical_dna(reference_sequence)) return AssemblerStatus::failed;
    Assembler assembler {kmer_size, reference_sequence};
    if (assembler.is_unique_reference()) {
        for (const auto& sequence : bin.read_sequences) {
            assembler.insert_read(sequence);
        }
        return try_assemble_region(assembler, reference_sequence, assemble_region, result);
    } else {
        return AssemblerStatus::failed;
    }
}

bool is_inversion(const Assembler::Variant& v) noexcept
{
    return v.ref.size() > 2
           && utils::are_reverse_complements(v.ref, v.alt)
           && !utils::is_homopolymer(v.ref)
           && !std::equal(std::next(std::cbegin(v.ref)), std::prev(std::cend(v.ref)), std::next(std::cbegin(v.alt)));
}

void trim_reference(Assembler::Variant& v)
{
    using std::cbegin; using std::cend; using std::crbegin; using std::crend;
    const auto p1 = std::mismatch(crbegin(v.ref), crend(v.ref), crbegin(v.alt), crend(v.alt));
    v.ref.erase(p1.first.base(), cend(v.ref));
    v.alt.erase(p1.second.base(), cend(v.alt));
    const auto p2 = std::mismatch(cbegin(v.ref), cend(v.ref), cbegin(v.alt), cend(v.alt));
    v.begin_pos += std::distance(cbegin(v.ref), p2.first);
    v.ref.erase(cbegin(v.ref), p2.first);
    v.alt.erase(cbegin(v.alt), p2.second);
}

void trim_reference(std::deque<Assembler::Variant>& variants)
{
    for (auto& v : variants) trim_reference(v);
}

bool is_complex(const Assembler::Variant& v) noexcept
{
    return (v.ref.size() > 1 && !v.alt.empty()) || (v.alt.size() > 1 && !v.ref.empty());
}

bool is_decomposable(const Assembler::Variant& v) noexcept
{
    return is_complex(v) && !is_inversion(v);
}

auto partition_decomposable(std::deque<Assembler::Variant>& variants)
{
    return std::stable_partition(std::begin(variants), std::end(variants),
                                 [] (const auto& candidate) { return !is_decomposable(candidate); });
}

bool is_mnv(const Assembler::Variant& v) noexcept
{
    return v.ref.size() == v.alt.size()
           && (v.ref.size() <= 2
               || std::equal(std::next(std::cbegin(v.ref)), std::prev(std::cend(v.ref)), std::next(std::cbegin(v.alt))));
}

auto split_mnv(Assembler::Variant&& mnv)
{
    assert(mnv.ref.size() > 1 && mnv.alt.size() > 1);
    assert(mnv.ref.front() != mnv.alt.front() && mnv.ref.back() != mnv.alt.back());
    std::vector<Assembler::Variant> result {};
    result.reserve(4);
    // Need to allocate new memory for all but the last SNV
    result.emplace_back(mnv.begin_pos, mnv.ref.front(), mnv.alt.front());
    const auto first_ref_itr       = std::cbegin(mnv.ref);
    const auto penultimate_ref_itr = std::prev(std::cend(mnv.ref));
    const auto first_alt_itr       = std::cbegin(mnv.alt);
    const auto penultimate_alt_itr = std::prev(std::cend(mnv.alt));
    auto p = std::mismatch(std::next(first_ref_itr), penultimate_ref_itr, std::next(first_alt_itr));
    while (p.first != penultimate_ref_itr) {
        assert(p.first < penultimate_ref_itr && p.second < penultimate_alt_itr);
        result.emplace_back(mnv.begin_pos + std::distance(first_ref_itr, p.first), *p.first, *p.second);
        p = std::mismatch(std::next(p.first), penultimate_ref_itr, std::next(p.second));
    }
    // So just need to remove the unwanted sequence from the last one
    const auto last_snv_begin = mnv.begin_pos + mnv.ref.size() - 1;
    mnv.ref.erase(first_ref_itr, penultimate_ref_itr);
    mnv.alt.erase(first_alt_itr, penultimate_alt_itr);
    result.emplace_back(last_snv_begin, std::move(mnv.ref), std::move(mnv.alt));
    return result;
}

auto extract_variants(const Assembler::NucleotideSequence& ref, const Assembler::NucleotideSequence& alt,
                      const CigarString& cigar, std::size_t ref_offset)
{
    std::vector<Assembler::Variant> result {};
    result.reserve(cigar.size());
    auto ref_itr = std::cbegin(ref);
    auto alt_itr = std::cbegin(alt);
    for (const auto& op : cigar) {
        using Flag = CigarOperation::Flag;
        using NucleotideSequence = Assembler::NucleotideSequence;
        switch(op.flag()) {
            case Flag::sequenceMatch:
            {
                ref_offset += op.size();
                ref_itr += op.size();
                alt_itr += op.size();
                break;
            }
            case Flag::substitution:
            {
                const auto next_ref_itr = std::next(ref_itr, op.size());
                std::transform(ref_itr, next_ref_itr, alt_itr, std::back_inserter(result),
                               [&ref_offset] (const auto ref, const auto alt) {
                                   return Assembler::Variant {ref_offset++, ref, alt};
                               });
                ref_itr = next_ref_itr;
                alt_itr += op.size();
                break;
            }
            case Flag::insertion:
            {
                const auto next_alt_itr = std::next(alt_itr, op.size());
                result.emplace_back(ref_offset, "", NucleotideSequence {alt_itr, next_alt_itr});
                alt_itr = next_alt_itr;
                break;
            }
            case Flag::deletion:
            {
                const auto next_ref_itr = std::next(ref_itr, op.size());
                result.emplace_back(ref_offset, NucleotideSequence {ref_itr, next_ref_itr}, "");
                ref_offset += op.size();
                ref_itr = next_ref_itr;
                break;
            }
            default:
                throw std::runtime_error {"LocalReassembler: unexpected cigar op"};
        }
        assert(ref_itr <= std::cend(ref) && alt_itr <= std::cend(alt));
    }
    return result;
}

auto align(const Assembler::Variant& v)
{
    constexpr Model model {1, -4, -6, -1};
    return align(v.ref, v.alt, model).cigar;
}

auto count_variant_types(const CigarString& cigar) noexcept
{
    bool has_snv {false}, has_insertion {false}, has_deletion {false};
    for (const auto& op : cigar) {
        switch (op.flag()) {
            case CigarOperation::Flag::substitution: has_snv = true; break;
            case CigarOperation::Flag::insertion: has_insertion = true; break;
            case CigarOperation::Flag::deletion: has_deletion = true; break;
            default: break;
        }
    }
    return has_snv + has_insertion + has_deletion;
}

bool is_complex_alignment(const CigarString& cigar, const Assembler::Variant& v) noexcept
{
    const auto min_allele_size = std::min(v.ref.size(), v.alt.size());
    return (min_allele_size > 5 && cigar.size() >= min_allele_size)
           || (min_allele_size > 8 && cigar.size() > 2 * min_allele_size / 3 && count_variant_types(cigar) > 1);
}

bool is_good_alignment(const CigarString& cigar, const Assembler::Variant& v) noexcept
{
    return !is_complex_alignment(cigar, v);
}

std::vector<Assembler::Variant> decompose(Assembler::Variant v)
{
    if (is_mnv(v)) {
        return split_mnv(std::move(v));
    } else {
        const auto cigar = align(v);
        if (is_good_alignment(cigar, v)) {
            return extract_variants(v.ref, v.alt, cigar, v.begin_pos);
        } else {
            return {std::move(v)};
        }
    }
}

struct VariantLess
{
    using Variant = Assembler::Variant;
    bool operator()(const Variant& lhs, const Variant& rhs) const noexcept
    {
        if (lhs.begin_pos == rhs.begin_pos) {
            if (lhs.ref.size() == rhs.ref.size()) {
                return lhs.alt < rhs.alt;
            }
            return lhs.ref.size() < rhs.ref.size();
        }
        return lhs.begin_pos < rhs.begin_pos;
    }
};

using VariantIterator = std::deque<Assembler::Variant>::iterator;

auto decompose(VariantIterator first, VariantIterator last)
{
    using std::begin; using std::end; using std::make_move_iterator;
    std::deque<Assembler::Variant> result {};
    std::for_each(make_move_iterator(first), make_move_iterator(last), [&result] (auto&& complex) {
        utils::append(decompose(std::move(complex)), result);
    });
    std::sort(begin(result), end(result), VariantLess {});
    result.erase(std::unique(begin(result), end(result)), end(result));
    return result;
}

void merge(std::deque<Assembler::Variant>&& decomposed, std::deque<Assembler::Variant>& variants,
           std::deque<Assembler::Variant>::iterator first_complex)
{
    using std::begin; using std::end; using std::make_move_iterator;
    assert(!decomposed.empty());
    assert(begin(variants) <= first_complex && first_complex <= end(variants));
    const auto num_complex = static_cast<std::size_t>(std::distance(first_complex, end(variants)));
    // The variants in [first_complex, end(variants)) where moved from so can now be assigned to
    if (decomposed.size() <= num_complex) {
        variants.erase(std::move(begin(decomposed), end(decomposed), first_complex), end(variants));
    } else {
        const auto last_assignable = std::next(begin(decomposed), num_complex);
        assert(last_assignable <= end(decomposed));
        std::move(begin(decomposed), last_assignable, first_complex);
        first_complex = variants.insert(end(variants),
                                        make_move_iterator(last_assignable),
                                        make_move_iterator(end(decomposed)));
        first_complex -= num_complex;
    }
    assert(begin(variants) <= first_complex && first_complex <= end(variants));
    std::inplace_merge(begin(variants), first_complex, end(variants), VariantLess {});
}

void decompose(std::deque<Assembler::Variant>& variants)
{
    const auto first_decomposable = partition_decomposable(variants);
    if (first_decomposable != std::end(variants)) {
        merge(decompose(first_decomposable, std::end(variants)), variants, first_decomposable);
    }
}

void add_to_mapped_variants(std::deque<Assembler::Variant>&& variants, std::deque<Variant>& result,
                            const GenomicRegion& assemble_region)
{
    for (auto& variant : variants) {
        result.emplace_back(contig_name(assemble_region), assemble_region.begin() + variant.begin_pos,
                            std::move(variant.ref), std::move(variant.alt));
    }
}

void remove_large_deletions(std::deque<Assembler::Variant>& variants, const unsigned max_size)
{
    variants.erase(std::remove_if(std::begin(variants), std::end(variants),
                                  [=] (const auto& variant) { return variant.ref.size() >= max_size && variant.alt.empty(); }),
                   std::end(variants));
}

LocalReassembler::AssemblerStatus
LocalReassembler::try_assemble_region(Assembler& assembler, const NucleotideSequence& reference_sequence,
                                      const GenomicRegion& assemble_region, std::deque<Variant>& result) const
{
    assert(assembler.is_unique_reference());
    assembler.try_recover_dangling_branches();
    assembler.prune(min_kmer_observations_);
    auto status = AssemblerStatus::success;
    if (!assembler.is_acyclic()) {
        assembler.remove_nonreference_cycles();
        status = AssemblerStatus::partial_success;
    }
    assembler.cleanup();
    if (assembler.is_empty() || assembler.is_all_reference()) {
        return status;
    }
    auto variants = assembler.extract_variants(max_bubbles_, min_bubble_score_);
    assembler.clear();
    if (!variants.empty()) {
        trim_reference(variants);
        std::sort(std::begin(variants), std::end(variants), VariantLess {});
        variants.erase(std::unique(std::begin(variants), std::end(variants)), std::end(variants));
        decompose(variants);
        if (status == AssemblerStatus::partial_success) {
            // TODO: Some false positive large deletions are being generated for small kmer sizes.
            // Until Assembler is better able to remove these automatically, filter them here.
            if (assembler.kmer_size() <= 10) {
                remove_large_deletions(variants, 100);
            } else if (assembler.kmer_size() <= 15) {
                remove_large_deletions(variants, 150);
            } else if (assembler.kmer_size() <= 20) {
                remove_large_deletions(variants, 200);
            } else if (assembler.kmer_size() <= 30) {
                remove_large_deletions(variants, 250);
            }
        }
        add_to_mapped_variants(std::move(variants), result, assemble_region);
    }
    return status;
}

} // namespace coretools
} // namespace octopus
