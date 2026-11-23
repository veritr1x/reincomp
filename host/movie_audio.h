#pragma once
// Decode the selected movie's audio separately from the video frame queue. A
// full video queue must never prevent a sound sample from reaching the device.
#include "opensles_audio.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#ifdef _WIN32
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>
#endif

namespace bunker::windows::video {
class MovieAudio {
 public:
#ifdef _WIN32
  using OutputFactory = std::unique_ptr<opensles::Output> (*)(unsigned, unsigned);
  static std::unique_ptr<MovieAudio> Open(const std::filesystem::path &path, OutputFactory output = OpenDevice) {
    auto track = std::make_unique<MovieAudio>();
    if (FAILED(MFCreateSourceReaderFromURL(path.c_str(), nullptr, &track->reader_))) return {};
    auto &reader = track->reader_;
    constexpr DWORD stream = MF_SOURCE_READER_FIRST_AUDIO_STREAM;
    Microsoft::WRL::ComPtr<IMFMediaType> native, pcm, actual;
    if (FAILED(reader->GetNativeMediaType(stream, 0, &native)) ||
        FAILED(reader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE)) ||
        FAILED(reader->SetStreamSelection(stream, TRUE)) || FAILED(MFCreateMediaType(&pcm))) return {};
    UINT32 channels = 0;
    if (FAILED(native->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &channels))) return {};
    pcm->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    pcm->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    pcm->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    pcm->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, std::min<UINT32>(channels, 2));
    if (FAILED(reader->SetCurrentMediaType(stream, nullptr, pcm.Get())) ||
        FAILED(reader->GetCurrentMediaType(stream, &actual)) ||
        FAILED(actual->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &track->rate_)) ||
        FAILED(actual->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &track->channels_)) ||
        !track->rate_ || (track->channels_ != 1 && track->channels_ != 2)) return {};
    track->output_ = output(track->channels_, track->rate_);
    if (!track->output_) return {};
    return track;
  }
  void Play(bool playing) { output_->SetPlaying(playing); }
  void Seek(int64_t ms) {
    output_->Clear();
    pending_.clear();
    next_ = ms * rate_ / 1000;
    PROPVARIANT position{};
    position.vt = VT_I8;
    position.hVal.QuadPart = ms * 10000;
    ended_ = FAILED(reader_->SetCurrentPosition(GUID_NULL, position));
  }
  void Update(int64_t ms, float volume, float pan) {
    output_->Finished();
    const int64_t now = ms * rate_ / 1000;
    next_ = std::max(next_, now);
    const int64_t until = now + rate_ / 10;
    for (int reads = 0; next_ < until && reads < 32;) {
      if (pending_.empty()) {
        if (ended_) break;
        ++reads;
        if (!Read()) continue;
      }
      const size_t stride = channels_ * sizeof(int16_t);
      const int64_t offset = std::max<int64_t>(0, next_ - pts_);
      const int64_t frames = static_cast<int64_t>(pending_.size() / stride);
      if (offset >= frames) { pending_.clear(); continue; }
      if (next_ < pts_) {
        const auto gap = std::min(pts_, until) - next_;
        output_->Submit(std::vector<uint8_t>(size_t(gap) * stride, 0));
        next_ += gap;
        continue;
      }
      const int64_t count = std::min(until - next_, frames - offset);
      std::vector<uint8_t> block(size_t(count) * stride);
      std::memcpy(block.data(), pending_.data() + size_t(offset) * stride, block.size());
      float gains[] = {volume, volume};
      if (channels_ == 2) {
        gains[0] *= 1.f - std::max(pan, 0.f);
        gains[1] *= 1.f + std::min(pan, 0.f);
      }
      if (gains[0] != 1 || gains[1] != 1) {
        for (size_t i = 0; i < block.size() / sizeof(int16_t); ++i) {
          int16_t sample;
          std::memcpy(&sample, block.data() + i * 2, 2);
          sample = int16_t(std::lround(sample * gains[i % channels_]));
          std::memcpy(block.data() + i * 2, &sample, 2);
        }
      }
      output_->Submit(std::move(block));
      next_ += count;
      if (offset + count == frames) pending_.clear();
    }
  }
 private:
  static std::unique_ptr<opensles::Output> OpenDevice(unsigned channels, unsigned rate) {
    auto device = std::make_unique<opensles::WaveOutput>(channels, rate, 16, false);
    if (!device->Open()) return {};
    return device;
  }
  bool Read() {
    DWORD flags = 0;
    LONGLONG stamp = 0;
    Microsoft::WRL::ComPtr<IMFSample> sample;
    HRESULT hr = reader_->ReadSample(MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, nullptr, &flags, &stamp, &sample);
    ended_ = FAILED(hr) || (flags & (MF_SOURCE_READERF_ENDOFSTREAM | MF_SOURCE_READERF_ERROR));
    if (FAILED(hr) || !sample) return false;
    Microsoft::WRL::ComPtr<IMFMediaBuffer> buffer;
    BYTE *data;
    DWORD size;
    if (FAILED(sample->ConvertToContiguousBuffer(&buffer)) || FAILED(buffer->Lock(&data, nullptr, &size))) return false;
    pending_.assign(data, data + size);
    buffer->Unlock();
    pts_ = stamp * rate_ / 10000000;
    return !pending_.empty();
  }
  Microsoft::WRL::ComPtr<IMFSourceReader> reader_;
  std::unique_ptr<opensles::Output> output_;
  UINT32 channels_ = 0, rate_ = 0;
  bool ended_ = false;
  int64_t next_ = 0, pts_ = 0;
  std::vector<uint8_t> pending_;
#else
  static std::unique_ptr<MovieAudio> Open(const std::filesystem::path &) { return {}; }
  void Play(bool) {}
  void Seek(int64_t) {}
  void Update(int64_t, float, float) {}
#endif
};
}
