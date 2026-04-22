#include "gst_audio_support.hpp"

#include "ext_obex_util.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace gstmax {

std::string atoms_to_string(long argc, t_atom* argv)
{
    if (argc <= 0 || !argv) {
        return {};
    }

    long text_size = 0;
    char* text = nullptr;
    std::string result;

    if (atom_gettext(argc, argv, &text_size, &text, 0) == MAX_ERR_NONE && text) {
        result.assign(text, text_size > 0 ? static_cast<std::size_t>(text_size - 1) : 0);
        sysmem_freeptr(text);
    }

    return result;
}

std::string unescape_max_text(const std::string& value)
{
    std::string result;
    result.reserve(value.size());

    bool escaping = false;
    for (const char ch : value) {
        if (escaping) {
            result.push_back(ch);
            escaping = false;
            continue;
        }

        if (ch == '\\') {
            escaping = true;
            continue;
        }

        result.push_back(ch);
    }

    if (escaping) {
        result.push_back('\\');
    }

    return result;
}

long parse_channel_argument(long argc, t_atom* argv, long default_channels, long* consumed)
{
    long parsed_channels = default_channels;
    long parsed_consumed = 0;

    if (argc > 0 && argv) {
        const auto atom_type = atom_gettype(argv);

        if (atom_type == A_LONG || atom_type == A_FLOAT) {
            parsed_channels = std::max(1L, atom_getlong(argv));
            parsed_consumed = 1;
        }
    }

    if (consumed) {
        *consumed = parsed_consumed;
    }

    return parsed_channels;
}

GstClockTime frames_to_clock_time(std::size_t frames, double sample_rate)
{
    if (sample_rate <= 0.0) {
        return 0;
    }

    return gst_util_uint64_scale(static_cast<guint64>(frames), GST_SECOND, static_cast<guint64>(std::llround(sample_rate)));
}

InterleavedRingBuffer::InterleavedRingBuffer()
: channels_(1)
, capacity_frames_(0)
, size_frames_(0)
, read_pos_(0)
, write_pos_(0)
{
}

void InterleavedRingBuffer::reset(long channels, std::size_t capacity_frames)
{
    const auto safe_channels = std::max(1L, channels);
    const auto safe_capacity = std::max<std::size_t>(1, capacity_frames);

    std::lock_guard<std::mutex> lock(mutex_);

    channels_ = safe_channels;
    capacity_frames_ = safe_capacity;
    size_frames_ = 0;
    read_pos_ = 0;
    write_pos_ = 0;
    storage_.assign(capacity_frames_ * static_cast<std::size_t>(channels_), 0.0f);
}

void InterleavedRingBuffer::clear()
{
    std::lock_guard<std::mutex> lock(mutex_);
    size_frames_ = 0;
    read_pos_ = 0;
    write_pos_ = 0;
}

void InterleavedRingBuffer::write_frame_block_unlocked(const float* data, std::size_t start_frame, std::size_t frames)
{
    if (frames == 0) {
        return;
    }

    const auto channel_count = static_cast<std::size_t>(channels_);
    const auto contiguous_frames = std::min(frames, capacity_frames_ - start_frame);

    std::memcpy(
        storage_.data() + (start_frame * channel_count),
        data,
        contiguous_frames * channel_count * sizeof(float));

    if (frames > contiguous_frames) {
        std::memcpy(
            storage_.data(),
            data + (contiguous_frames * channel_count),
            (frames - contiguous_frames) * channel_count * sizeof(float));
    }
}

void InterleavedRingBuffer::drop_oldest_unlocked(std::size_t frames)
{
    if (frames >= size_frames_) {
        size_frames_ = 0;
        read_pos_ = 0;
        write_pos_ = 0;
        return;
    }

    read_pos_ = (read_pos_ + frames) % capacity_frames_;
    size_frames_ -= frames;
}

std::size_t InterleavedRingBuffer::push_interleaved(const float* data, std::size_t frames)
{
    if (!data || frames == 0) {
        return 0;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    if (frames >= capacity_frames_) {
        data += (frames - capacity_frames_) * static_cast<std::size_t>(channels_);
        frames = capacity_frames_;
        size_frames_ = 0;
        read_pos_ = 0;
        write_pos_ = 0;
    }

    const auto free_frames = capacity_frames_ - size_frames_;
    if (frames > free_frames) {
        drop_oldest_unlocked(frames - free_frames);
    }

    write_frame_block_unlocked(data, write_pos_, frames);
    write_pos_ = (write_pos_ + frames) % capacity_frames_;
    size_frames_ += frames;
    return frames;
}

std::size_t InterleavedRingBuffer::push_from_deinterleaved(double** inputs, std::size_t frames)
{
    if (!inputs || frames == 0) {
        return 0;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    if (frames >= capacity_frames_) {
        const auto skip = frames - capacity_frames_;
        frames = capacity_frames_;
        size_frames_ = 0;
        read_pos_ = 0;
        write_pos_ = 0;

        for (std::size_t frame = 0; frame < frames; ++frame) {
            const auto dst_frame = frame;
            for (std::size_t channel = 0; channel < static_cast<std::size_t>(channels_); ++channel) {
                storage_[(dst_frame * static_cast<std::size_t>(channels_)) + channel] =
                    static_cast<float>(inputs[channel][frame + skip]);
            }
        }

        write_pos_ = frames % capacity_frames_;
        size_frames_ = frames;
        return frames;
    }

    const auto free_frames = capacity_frames_ - size_frames_;
    if (frames > free_frames) {
        drop_oldest_unlocked(frames - free_frames);
    }

    for (std::size_t frame = 0; frame < frames; ++frame) {
        const auto dst_frame = (write_pos_ + frame) % capacity_frames_;
        for (std::size_t channel = 0; channel < static_cast<std::size_t>(channels_); ++channel) {
            storage_[(dst_frame * static_cast<std::size_t>(channels_)) + channel] =
                static_cast<float>(inputs[channel][frame]);
        }
    }

    write_pos_ = (write_pos_ + frames) % capacity_frames_;
    size_frames_ += frames;
    return frames;
}

std::size_t InterleavedRingBuffer::pop_interleaved(float* dest, std::size_t frames)
{
    if (!dest || frames == 0) {
        return 0;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    const auto available_frames = std::min(frames, size_frames_);
    const auto channel_count = static_cast<std::size_t>(channels_);
    const auto contiguous_frames = std::min(available_frames, capacity_frames_ - read_pos_);

    if (available_frames > 0) {
        std::memcpy(
            dest,
            storage_.data() + (read_pos_ * channel_count),
            contiguous_frames * channel_count * sizeof(float));

        if (available_frames > contiguous_frames) {
            std::memcpy(
                dest + (contiguous_frames * channel_count),
                storage_.data(),
                (available_frames - contiguous_frames) * channel_count * sizeof(float));
        }
    }

    if (available_frames < frames) {
        std::memset(
            dest + (available_frames * channel_count),
            0,
            (frames - available_frames) * channel_count * sizeof(float));
    }

    if (available_frames > 0) {
        read_pos_ = (read_pos_ + available_frames) % capacity_frames_;
        size_frames_ -= available_frames;
    }

    return available_frames;
}

std::size_t InterleavedRingBuffer::pop_to_deinterleaved(double** outputs, std::size_t frames)
{
    if (!outputs || frames == 0) {
        return 0;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    const auto available_frames = std::min(frames, size_frames_);
    const auto channel_count = static_cast<std::size_t>(channels_);

    for (std::size_t frame = 0; frame < available_frames; ++frame) {
        const auto src_frame = (read_pos_ + frame) % capacity_frames_;
        for (std::size_t channel = 0; channel < channel_count; ++channel) {
            outputs[channel][frame] = static_cast<double>(storage_[(src_frame * channel_count) + channel]);
        }
    }

    for (std::size_t channel = 0; channel < channel_count; ++channel) {
        std::fill(outputs[channel] + available_frames, outputs[channel] + frames, 0.0);
    }

    if (available_frames > 0) {
        read_pos_ = (read_pos_ + available_frames) % capacity_frames_;
        size_frames_ -= available_frames;
    }

    return available_frames;
}

std::size_t InterleavedRingBuffer::size_frames() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return size_frames_;
}

} // namespace gstmax
