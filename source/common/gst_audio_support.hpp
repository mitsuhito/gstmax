#pragma once

#include "ext.h"

#include <gst/gst.h>

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace gstmax {

std::string atoms_to_string(long argc, t_atom* argv);
std::string unescape_max_text(const std::string& value);
long parse_channel_argument(long argc, t_atom* argv, long default_channels, long* consumed);
GstClockTime frames_to_clock_time(std::size_t frames, double sample_rate);

class InterleavedRingBuffer {
public:
    InterleavedRingBuffer();

    void reset(long channels, std::size_t capacity_frames);
    void clear();

    std::size_t push_interleaved(const float* data, std::size_t frames);
    std::size_t push_from_deinterleaved(double** inputs, std::size_t frames);
    std::size_t pop_interleaved(float* dest, std::size_t frames);
    std::size_t pop_to_deinterleaved(double** outputs, std::size_t frames);
    std::size_t size_frames() const;

private:
    void write_frame_block_unlocked(const float* data, std::size_t start_frame, std::size_t frames);
    void drop_oldest_unlocked(std::size_t frames);

    mutable std::mutex mutex_;
    long channels_;
    std::size_t capacity_frames_;
    std::size_t size_frames_;
    std::size_t read_pos_;
    std::size_t write_pos_;
    std::vector<float> storage_;
};

} // namespace gstmax
