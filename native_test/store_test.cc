// TrackStore: window, spill, and MP3 backing store (ADR D11, I11).
#include <sys/stat.h>

#include <cstring>
#include <filesystem>
#include <random>

#include "harness.h"
#include "minimp3_ex.h"
#include "track_store.h"

using tsnx::Retention;
using tsnx::Retirer;
using tsnx::StoreLimits;
using tsnx::TrackStore;

namespace {
int16_t Pattern(int64_t frame) { return static_cast<int16_t>((frame * 7) % 30011); }

std::vector<uint8_t> ReadFile(const std::string& path) {
  std::vector<uint8_t> v;
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return v;
  std::fseek(f, 0, SEEK_END);
  v.resize(static_cast<size_t>(std::ftell(f)));
  std::fseek(f, 0, SEEK_SET);
  if (std::fread(v.data(), 1, v.size(), f) != v.size()) v.clear();
  std::fclose(f);
  return v;
}
}  // namespace

TEST(StoreUnplayedRefusesWritesPastAheadLimit) {
  StoreLimits l;
  l.ahead_seconds = 3;
  Retirer r;
  TrackStore s(8000, 1, Retention::kUnplayed, "", l);
  std::vector<int16_t> one_s(8000, 1);
  for (int i = 0; i < 3; ++i) CHECK_EQ(s.Write(one_s.data(), 8000, 0, r), 8000);
  CHECK_EQ(s.Write(one_s.data(), 1, 0, r), tsnx::kStoreFull);
  // Playing 1.5 s frees chunk 0 and makes room.
  s.Maintain(12000, r);
  r.Reclaim();
  CHECK_EQ(s.ResidentChunks(), 2);
  CHECK_EQ(s.Write(one_s.data(), 8000, 12000, r), 8000);
  std::vector<int16_t> out(100);
  CHECK_EQ(s.Read(0, out.data(), 100), 0);       // freed
  CHECK_EQ(s.Read(12000, out.data(), 100), 100);  // resident
}

TEST(StoreAllSpillsAndReloadsExactly) {
  const std::string dir = tsnx_test::TempDir("spill");
  std::filesystem::create_directories(dir);
  const std::string path = dir + "/t.pcm";
  StoreLimits l;
  l.ahead_seconds = 2;
  l.behind_seconds = 2;
  l.max_seconds = 120;
  Retirer r;
  {
    TrackStore s(8000, 1, Retention::kAll, path, l);
    std::mt19937 rng(1);
    int64_t w = 0;
    const int64_t total = 8000 * 40 + 1234;
    std::vector<int16_t> buf;
    while (w < total) {
      const int64_t n = std::min<int64_t>(total - w, 1 + rng() % 3000);
      buf.resize(static_cast<size_t>(n));
      for (int64_t i = 0; i < n; ++i) buf[i] = Pattern(w + i);
      CHECK_EQ(s.Write(buf.data(), n, 0, r), n);
      w += n;
      s.Maintain(0, r);
    }
    s.EndOfStream();
    s.Maintain(0, r);
    r.Reclaim();
    CHECK_MSG(s.ResidentChunks() <= 4, "resident %lld",
              (long long)s.ResidentChunks());
    struct stat st{};
    CHECK(stat(path.c_str(), &st) == 0);
    std::vector<int16_t> out(3000);
    for (int64_t target : {int64_t{35 * 8000 + 17}, int64_t{5 * 8000},
                           int64_t{20 * 8000 + 7999}, int64_t{0},
                           total - 3000}) {
      s.Maintain(target, r);
      r.Reclaim();
      CHECK_MSG(s.ResidentChunks() <= 6, "resident %lld",
                (long long)s.ResidentChunks());
      const int64_t got = s.Read(target, out.data(), 3000);
      CHECK_EQ(got, 3000);
      bool same = true;
      for (int64_t i = 0; i < got; ++i)
        if (out[i] != Pattern(target + i)) same = false;
      CHECK_MSG(same, "content differs at %lld", (long long)target);
    }
  }
  struct stat st{};
  CHECK_MSG(stat(path.c_str(), &st) != 0, "spill file not deleted");
  std::filesystem::remove_all(dir);
}

TEST(StoreMp3FarSeekMatchesFullDecode) {
  const auto bytes =
      ReadFile(std::string(TSNX_FIXTURES) + "/shine_24k_mono_64k.mp3");
  CHECK(!bytes.empty());
  mp3dec_ex_t full;
  CHECK_EQ(mp3dec_ex_open_buf(&full, bytes.data(), bytes.size(),
                              MP3D_SEEK_TO_SAMPLE),
           0);
  std::vector<int16_t> ref(full.samples);
  CHECK_EQ(mp3dec_ex_read(&full, ref.data(), ref.size()), ref.size());
  const int ch = full.info.channels;
  mp3dec_ex_close(&full);

  StoreLimits l;
  l.ahead_seconds = 2;
  l.behind_seconds = 1;
  auto s = TrackStore::OpenMp3(bytes.data(), bytes.size(), l);
  CHECK(s != nullptr);
  if (!s) return;
  CHECK_EQ(s->sample_rate(), 24000);
  CHECK(s->ended());
  CHECK_EQ(s->written(), static_cast<int64_t>(ref.size()) / ch);
  Retirer r;
  std::mt19937 rng(7);
  std::vector<int16_t> out(2400 * ch);
  int exact = 0;
  for (int i = 0; i < 40; ++i) {
    const int64_t pos = rng() % (s->written() - 2400);
    s->Maintain(pos, r);
    r.Reclaim();
    const int64_t got = s->Read(pos, out.data(), 2400);
    if (got == 2400 &&
        std::memcmp(out.data(), ref.data() + pos * ch, 2400 * ch * 2) == 0)
      ++exact;
  }
  CHECK_EQ(exact, 40);
  CHECK(s->ResidentChunks() <= 5);
}
