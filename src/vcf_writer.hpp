//
//  vcf_writer.hpp
//  Octopus
//
//  Created by Daniel Cooke on 29/07/2015.
//  Copyright (c) 2015 Oxford University. All rights reserved.
//

#ifndef __Octopus__vcf_writer__
#define __Octopus__vcf_writer__

#include <memory>
#include <mutex>

#include <boost/filesystem/path.hpp>

#include "htslib_bcf_facade.hpp"

class VcfHeader;
class VcfRecord;
class GenomicRegion;

class VcfWriter
{
public:
    using Path = boost::filesystem::path;
    
    VcfWriter() = default;
    explicit VcfWriter(Path file_path);
    explicit VcfWriter(Path file_path, const VcfHeader& header);
    ~VcfWriter() = default;
    
    VcfWriter(const VcfWriter&)            = delete;
    VcfWriter& operator=(const VcfWriter&) = delete;
    VcfWriter(VcfWriter&&);
    VcfWriter& operator=(VcfWriter&&)      = default;
    
    friend void swap(VcfWriter& lhs, VcfWriter& rhs) noexcept;
    
    bool is_open() const noexcept;
    void open(Path file_path) noexcept;
    void close() noexcept;
    
    bool is_header_written() const noexcept;
    
    const Path& path() const noexcept;
    
    void write(const VcfHeader& header);
    void write(const VcfRecord& record);
    
private:
    Path file_path_;
    bool is_header_written_;
    
    std::unique_ptr<HtslibBcfFacade> writer_;
    
    mutable std::mutex mutex_;
};

VcfWriter& operator<<(VcfWriter& dst, const VcfHeader& header);
VcfWriter& operator<<(VcfWriter& dst, const VcfRecord& record);

template <typename Container>
void write(const Container& records, VcfWriter& dst)
{
    for (const auto& record : records) {
        dst << record;
    }
}

bool operator==(const VcfWriter& lhs, const VcfWriter& rhs);

namespace std {
    template <> struct hash<VcfWriter>
    {
        size_t operator()(const VcfWriter& writer) const
        {
            return hash<string>()(writer.path().string());
        }
    };
} // namespace std

#endif /* defined(__Octopus__vcf_writer__) */
