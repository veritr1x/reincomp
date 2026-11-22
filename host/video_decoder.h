#pragma once
// Movie decoding for the AVPro host (story movies, MP4 H.264).
//
// A Decoder runs on its own thread and keeps a short queue of decoded RGBA
// frames (bottom row first, GL texture order) with presentation times. The consumer takes the
// newest frame due at its playback clock. Windows uses Media Foundation's
// Source Reader with its video processor converting to RGB32. Other platforms
// have no backend yet: Open() returns null and the AVPro host keeps its
// no-decoder behaviour (the movie reports its duration and finishes at once).
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#endif

namespace bunker::windows::video {

struct Frame {
  int64_t pts_ms = 0;
  std::vector<uint8_t> rgba;  // width*height*4, bottom row first (GL texture order)
};

class Decoder {
 public:
  virtual ~Decoder() = default;
  int width = 0, height = 0;
  int64_t duration_ms = 0;
  float frame_rate = 0;
  // Newest queued frame with pts <= clock (older due frames are dropped).
  // Returns false when no frame is due yet.
  bool TakeDue(int64_t clock_ms, Frame &out) {
    std::lock_guard lock(mutex_);
    bool got = false;
    while (!queue_.empty() && queue_.front().pts_ms <= clock_ms) {
      out = std::move(queue_.front());
      queue_.pop_front();
      got = true;
    }
    if (got) space_.notify_all();
    return got;
  }
  bool Ended() {
    std::lock_guard lock(mutex_);
    return ended_ && queue_.empty();
  }
  // Restart decoding from `ms` (drops queued frames).
  void Seek(int64_t ms) {
    std::lock_guard lock(mutex_);
    seek_ms_ = ms;
    queue_.clear();
    ended_ = false;
    space_.notify_all();
  }

 protected:
  static constexpr size_t kQueue = 4;
  std::mutex mutex_;
  std::condition_variable space_;
  std::deque<Frame> queue_;
  bool ended_ = false, stop_ = false;
  int64_t seek_ms_ = -1;
  // Producer side: blocks while the queue is full; false when stopping or a seek is pending.
  bool Push(Frame frame) {
    std::unique_lock lock(mutex_);
    space_.wait(lock, [&] { return stop_ || seek_ms_ >= 0 || queue_.size() < kQueue; });
    if (stop_ || seek_ms_ >= 0) return false;
    queue_.push_back(std::move(frame));
    return true;
  }
};

#ifdef _WIN32
class MediaFoundationDecoder final : public Decoder {
 public:
  static std::unique_ptr<Decoder> Open(const std::filesystem::path &path) {
    static const bool started = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE));
    if (!started) return nullptr;
    auto d = std::unique_ptr<MediaFoundationDecoder>(new MediaFoundationDecoder);
    if (!d->Init(path)) return nullptr;
    d->thread_ = std::thread([p = d.get()] { p->Run(); });
    return d;
  }
  ~MediaFoundationDecoder() override {
    {
      std::lock_guard lock(mutex_);
      stop_ = true;
      space_.notify_all();
    }
    if (thread_.joinable()) thread_.join();
    if (reader_) reader_->Release();
  }

 private:
  IMFSourceReader *reader_ = nullptr;
  std::thread thread_;
  LONG stride_ = 0;
  UINT32 frame_w_ = 0, frame_h_ = 0;

  bool Init(const std::filesystem::path &path) {
    IMFAttributes *attrs = nullptr;
    if (FAILED(MFCreateAttributes(&attrs, 2))) return false;
    // The chapter title is 1334 pixels wide on a padded H.264 surface.
    // The advanced processor exposes the actual RGB row pitch via Lock2D.
    attrs->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
    const HRESULT hr = MFCreateSourceReaderFromURL(path.wstring().c_str(), attrs, &reader_);
    attrs->Release();
    if (FAILED(hr)) { std::fprintf(stderr, "Video decoder: open failed hr=%#lx\n", (unsigned long)hr); return false; }
    reader_->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
    reader_->SetStreamSelection(MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);
    IMFMediaType *want = nullptr;
    MFCreateMediaType(&want);
    want->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    want->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    const HRESULT set = reader_->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, want);
    want->Release();
    if (FAILED(set)) { std::fprintf(stderr, "Video decoder: RGB32 output unavailable hr=%#lx\n", (unsigned long)set); return false; }
    IMFMediaType *got = nullptr;
    if (FAILED(reader_->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &got))) return false;
    MFGetAttributeSize(got, MF_MT_FRAME_SIZE, &frame_w_, &frame_h_);
    width = int(frame_w_);
    height = int(frame_h_);
    MFVideoArea area{};
    if (SUCCEEDED(got->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE, reinterpret_cast<UINT8 *>(&area), sizeof area, nullptr)) &&
        area.Area.cx > 0 && area.Area.cy > 0) {
      width = int(area.Area.cx);
      height = int(area.Area.cy);
    }
    UINT32 stride = 0;
    stride_ = SUCCEEDED(got->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride)) ? LONG(int32_t(stride)) : LONG(frame_w_ * 4);
    UINT32 num = 0, den = 0;
    if (SUCCEEDED(MFGetAttributeRatio(got, MF_MT_FRAME_RATE, &num, &den)) && den) frame_rate = float(num) / float(den);
    got->Release();
    PROPVARIANT var;
    PropVariantInit(&var);
    if (SUCCEEDED(reader_->GetPresentationAttribute(MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &var)))
      duration_ms = int64_t(var.uhVal.QuadPart / 10000);
    PropVariantClear(&var);
    std::fprintf(stderr, "Video decoder: Media Foundation %dx%d (buffer %ux%u stride %ld) %.3f fps duration_ms=%lld\n", width, height,
                 frame_w_, frame_h_, long(stride_), double(frame_rate), (long long)duration_ms);
    return width > 0 && height > 0;
  }

  void Run() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    for (;;) {
      {
        std::lock_guard lock(mutex_);
        if (stop_) break;
        if (seek_ms_ >= 0) {
          PROPVARIANT var;
          PropVariantInit(&var);
          var.vt = VT_I8;
          var.hVal.QuadPart = seek_ms_ * 10000;
          reader_->SetCurrentPosition(GUID_NULL, var);
          seek_ms_ = -1;
        }
      }
      DWORD flags = 0;
      LONGLONG ts = 0;
      IMFSample *sample = nullptr;
      if (FAILED(reader_->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, &flags, &ts, &sample)) ||
          (flags & MF_SOURCE_READERF_ENDOFSTREAM)) {
        if (sample) sample->Release();
        std::unique_lock lock(mutex_);
        ended_ = true;
        space_.wait(lock, [&] { return stop_ || seek_ms_ >= 0; });
        continue;
      }
      if (!sample) continue;
      Frame frame;
      frame.pts_ms = int64_t(ts / 10000);
      if (Convert(sample, frame.rgba)) Push(std::move(frame));
      sample->Release();
    }
    CoUninitialize();
  }

  // RGB32 (B,G,R,X) -> RGBA, cropped to width x height, bottom row first.
  bool Convert(IMFSample *sample, std::vector<uint8_t> &out) {
    IMFMediaBuffer *buffer = nullptr;
    if (FAILED(sample->ConvertToContiguousBuffer(&buffer))) return false;
    BYTE *data = nullptr;
    LONG pitch = stride_;
    IMF2DBuffer *two_d = nullptr;
    bool locked2d = false;
    if (SUCCEEDED(buffer->QueryInterface(IID_PPV_ARGS(&two_d))) && SUCCEEDED(two_d->Lock2D(&data, &pitch))) locked2d = true;
    else {
      DWORD max = 0, len = 0;
      if (FAILED(buffer->Lock(&data, &max, &len))) { if (two_d) two_d->Release(); buffer->Release(); return false; }
      if (pitch < 0) data += size_t(-pitch) * (frame_h_ - 1);  // bottom-up: Lock returns the last row first in memory
    }
    out.resize(size_t(width) * height * 4);
    for (int y = 0; y < height; ++y) {
      const BYTE *src = data + ptrdiff_t(y) * pitch;
      uint8_t *dst = out.data() + size_t(height - 1 - y) * width * 4;  // bottom row first
      for (int x = 0; x < width; ++x) {
        dst[0] = src[2]; dst[1] = src[1]; dst[2] = src[0]; dst[3] = 255;
        src += 4; dst += 4;
      }
    }
    if (locked2d) { two_d->Unlock2D(); } else buffer->Unlock();
    if (two_d) two_d->Release();
    buffer->Release();
    return true;
  }
};
#endif

inline std::unique_ptr<Decoder> Open(const std::filesystem::path &path) {
#ifdef _WIN32
  return MediaFoundationDecoder::Open(path);
#else
  (void)path;
  return nullptr;
#endif
}

}  // namespace bunker::windows::video
