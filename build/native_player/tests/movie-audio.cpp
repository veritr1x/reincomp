// First input: a stereo test tone lasting at least two seconds. Second input:
// its video-only copy. An optional third input exercises an original game clip.
// ffmpeg -f lavfi -i color=s=160x90:d=2 -f lavfi -i sine=d=2 -ac 2 -c:a aac tone.mp4
// ffmpeg -i tone.mp4 -c:v copy -an silent.mp4
#include "../../../host/movie_audio.h"
#include <cassert>
#include <iostream>

using namespace bunker::windows;
namespace {
class Capture final : public opensles::Output {
 public:
  void Submit(std::vector<uint8_t> block) override {
    for (size_t i = 0; i < block.size(); i += sizeof(int16_t)) {
      int16_t value;
      std::memcpy(&value, block.data() + i, sizeof value);
      if (value) ++audible[(i / sizeof value) % channels];
    }
    frames += block.size() / (channels * sizeof(int16_t));
  }
  void SetPlaying(bool value) override { playing = value; }
  void Clear() override { audible[0] = audible[1] = frames = 0; }
  size_t Finished() override { return 0; }
  void Wait() override {}
  void Wake() override {}
  unsigned channels = 0, rate = 0, audible[2]{}, frames = 0;
  bool playing = false;
};
Capture *capture;
std::unique_ptr<opensles::Output> device(unsigned channels, unsigned rate) {
  auto output = std::make_unique<Capture>();
  capture = output.get();
  capture->channels = channels;
  capture->rate = rate;
  return output;
}
}

int wmain(int argc, wchar_t **argv) {
  _set_error_mode(_OUT_TO_STDERR);
  assert(argc == 3 || argc == 4);
  assert(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)));
  assert(SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE)));
  {
    auto audio = video::MovieAudio::Open(argv[1], device);
    assert(audio && capture->channels == 2);
    audio->Play(true);
    audio->Update(0, 1, 0);
    assert(capture->playing && capture->frames == capture->rate / 10);
    assert(capture->audible[0] && capture->audible[1]);
    audio->Play(false);
    assert(!capture->playing);
    audio->Seek(500);
    audio->Update(500, 0, 0);
    assert(capture->frames && !capture->audible[0] && !capture->audible[1]);
    audio->Seek(1000);
    audio->Play(true);
    audio->Update(1000, 1, 1);
    assert(capture->frames && !capture->audible[0] && capture->audible[1]);
    assert(!video::MovieAudio::Open(argv[2], device));
    if (argc == 4) {
      auto movie = video::MovieAudio::Open(argv[3], device);
      assert(movie);
      movie->Play(true);
      for (int ms = 0; ms < 10000 && !capture->audible[0]; ms += 50) movie->Update(ms, 1, 0);
      assert(capture->audible[0]);
      std::cout << "Game movie decoded at " << capture->rate << " Hz, " << capture->channels << " channels\n";
    }
  }
  MFShutdown();
  CoUninitialize();
  std::cout << "Passed movie PCM, pause, seek, mute, pan and video-only checks\n";
  return 0;
}
