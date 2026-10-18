// audio -> beat grid + song structure. pure fn of samples (deterministic, no I/O), ~1.6 s for 3.5 min on a laptop,
// 2-7 s on console (PS5). needs the WHOLE song: not realtime. input mono float @ kSampleRate=22050 (library::toAnalysisRate).
// pipeline: stft (1024 fft / 256 hop => kFps~86.1 frames/s) -> per-band flux (low<200 Hz, mid<2 kHz, high) -> weighted
//   onset -> detrend -> peaks | tempo (autocorr, prior ~125 BPM, 60..200) -> DP beat track -> bpm=median beat gap ->
//   downbeat phase (most low-band onset per i%4) -> bars (4 beats) -> normalised bar features -> sections.
// !: results feed mapper.cpp and the visuals; changing thresholds changes maps => bump library::kMapperVersion.
#include "analysis.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <numeric>

namespace analysis {

const char *name(Section s) {
  switch (s) {
    case Section::Intro: return "intro";
    case Section::Steady: return "steady";
    case Section::Build: return "build";
    case Section::Drop: return "drop";
    case Section::Break: return "break";
    case Section::Outro: return "outro";
  }
  return "?";
}

namespace {

using cplx = std::complex<float>;

// in-place radix-2 FFT. pre: a.size() power of two (kFft).
void fft(std::vector<cplx> &a) {
  const size_t n = a.size();
  for (size_t i = 1, j = 0; i < n; i++) {
    size_t bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) std::swap(a[i], a[j]);
  }
  for (size_t len = 2; len <= n; len <<= 1) {
    const float ang = -2 * (float)M_PI / len;
    const cplx wl(std::cos(ang), std::sin(ang));
    for (size_t i = 0; i < n; i += len) {
      cplx w(1);
      for (size_t k = 0; k < len / 2; k++) {
        const cplx u = a[i + k], v = a[i + k + len / 2] * w;
        a[i + k] = u + v;
        a[i + k + len / 2] = u - v;
        w *= wl;
      }
    }
  }
}

// p in 0..1, nearest rank; copies v.
float percentile(std::vector<float> v, float p) {
  if (v.empty()) return 0;
  std::sort(v.begin(), v.end());
  return v[std::min(v.size() - 1, (size_t)(p * (v.size() - 1) + 0.5f))];
}

// divide by the 95th percentile and clamp to 1 (robust to outliers).
void normalise01(std::vector<float> &v) {
  if (v.empty()) return;
  const float hi = std::max(percentile(v, 0.95f), 1e-9f);
  for (float &x : v) x = std::min(x / hi, 1.0f);
}

// box filter +-radius samples (edges average what exists).
std::vector<float> smooth(const std::vector<float> &v, int radius) {
  std::vector<float> o(v.size());
  for (int i = 0; i < (int)v.size(); i++) {
    float s = 0;
    int n = 0;
    for (int k = std::max(0, i - radius); k <= std::min((int)v.size() - 1, i + radius); k++) s += v[k], n++;
    o[i] = s / n;
  }
  return o;
}

// Band edges in FFT bins (bin width = rate / kFft, about 21.5 Hz): low to 200 Hz, mid to 2 kHz.
constexpr int kLowEnd = 9, kMidEnd = 93;

// STFT, then per band: log-compressed spectral flux (onset strength) and linear energy.
void stft(const std::vector<float> &x, Result &r) {
  std::vector<float> win(kFft);
  for (int i = 0; i < kFft; i++) win[i] = 0.5f - 0.5f * std::cos(2 * (float)M_PI * i / (kFft - 1));
  const int frames = x.size() > (size_t)kFft ? (int)((x.size() - kFft) / kHop) + 1 : 0;
  for (int b = 0; b < 3; b++) r.band[b].assign(frames, 0), r.energy[b].assign(frames, 0);
  std::vector<float> prev(kFft / 2 + 1, 0), cur(kFft / 2 + 1);
  std::vector<cplx> buf(kFft);
  for (int t = 0; t < frames; t++) {
    for (int i = 0; i < kFft; i++) buf[i] = x[(size_t)t * kHop + i] * win[i];
    fft(buf);
    for (int k = 0; k <= kFft / 2; k++) cur[k] = std::log1p(50.0f * std::abs(buf[k]) / kFft * 8);
    for (int k = 1; k <= kFft / 2; k++) {
      const int b = k < kLowEnd ? 0 : k < kMidEnd ? 1 : 2;
      r.band[b][t] += std::max(0.0f, cur[k] - prev[k]);
      const float m = std::abs(buf[k]) / kFft;
      r.energy[b][t] += m * m;
    }
    prev = cur;
  }
}

// Peak-pick an onset envelope with an adaptive threshold (local mean + a fraction of the local max).
void pickPeaks(const std::vector<float> &o, const std::vector<float> &env, Result &r) {
  const int n = (int)o.size(), w = (int)(0.08 * kFps);
  for (int i = 1; i + 1 < n; i++) {
    if (!(o[i] > o[i - 1] && o[i] >= o[i + 1])) continue;
    float mx = 0, mean = 0;
    int c = 0;
    for (int k = std::max(0, i - w); k <= std::min(n - 1, i + w); k++) mx = std::max(mx, o[k]), mean += o[k], c++;
    if (o[i] < mx) continue;
    mean /= c;
    if (o[i] < mean * 1.3f + 0.05f * env[i]) continue;
    // Parabolic interpolation for a sub-frame time.
    const float a = o[i - 1], b = o[i], cc = o[i + 1], d = a - 2 * b + cc;
    const float off = d != 0 ? 0.5f * (a - cc) / d : 0;
    r.peaks.push_back((i + off) / kFps);
    r.peakStrength.push_back(o[i]);
  }
}

// Tempo from the autocorrelation of the onset envelope, weighted toward 60..200 BPM around 125.
double estimateTempo(const std::vector<float> &o, double &confidence) {
  const int n = (int)o.size();
  std::vector<float> m = o;
  const float mean = std::accumulate(m.begin(), m.end(), 0.0f) / std::max(1, n);
  for (float &v : m) v -= mean;
  const int lo = (int)(60.0 * kFps / 200), hi = (int)(60.0 * kFps / 60);
  std::vector<double> ac(hi + 2, 0);
  for (int lag = lo - 1; lag <= hi + 1 && lag < n; lag++) {
    double s = 0;
    for (int i = lag; i < n; i++) s += (double)m[i] * m[i - lag];
    ac[lag] = s / (n - lag);
  }
  double zero = 0;
  for (int i = 0; i < n; i++) zero += (double)m[i] * m[i];
  zero = zero / n + 1e-12;
  double best = -1e9, bestLag = lo;
  for (int lag = lo; lag <= hi && lag < n - 1; lag++) {
    const double bpm = 60.0 * kFps / lag;
    const double prior = std::exp(-0.5 * std::pow(std::log2(bpm / 125.0) / 0.8, 2));
    // A tempo also repeats at twice the lag; summing the two favours the true period over a subdivision.
    const double v = (ac[lag] + 0.5 * (2 * lag < (int)ac.size() ? ac[2 * lag] : 0)) * prior;
    if (v > best) best = v, bestLag = lag;
  }
  confidence = std::clamp(ac[(int)bestLag] / zero * 2, 0.0, 1.0);
  const int l = (int)bestLag;
  const double a = ac[l - 1], b = ac[l], c = ac[l + 1], d = a - 2 * b + c;
  const double refined = l + (d != 0 ? 0.5 * (a - c) / d : 0);
  return 60.0 * kFps / refined;
}

// Dynamic-programming beat tracker (Ellis 2007): beats land on onsets and about one period apart.
std::vector<double> trackBeats(const std::vector<float> &o, double bpm) {
  const int n = (int)o.size();
  const double period = 60.0 * kFps / bpm;
  std::vector<double> score(n), back(n, -1);
  const double tight = 80;
  for (int t = 0; t < n; t++) {
    double bestPrev = 0;
    int arg = -1;
    for (int p = (int)(t - 2 * period); p <= (int)(t - period / 2); p++) {
      if (p < 0) continue;
      const double dev = std::log((double)(t - p) / period);
      const double v = score[p] - tight * dev * dev;
      if (arg < 0 || v > bestPrev) bestPrev = v, arg = p;
    }
    score[t] = o[t] + (arg >= 0 ? bestPrev : 0);
    back[t] = arg;
  }
  int t = n - 1;
  for (int i = std::max(0, n - (int)period - 1); i < n; i++)
    if (score[i] > score[t]) t = i;
  std::vector<double> beats;
  while (t >= 0) {
    beats.push_back(t / kFps);
    t = (int)back[t];
  }
  std::reverse(beats.begin(), beats.end());
  return beats;
}

}  // namespace

Result analyse(const std::vector<float> &x, double) {
  Result r;
  r.duration = x.size() / (double)kSampleRate;
  stft(x, r);
  const int frames = (int)r.band[0].size();
  if (frames < 100) return r;

  // Each band's flux is scaled by its own typical level so the hats don't drown the kick.
  std::vector<float> weight = {1.0f, 0.8f, 0.5f};
  r.onset.assign(frames, 0);
  for (int b = 0; b < 3; b++) {
    const float scale = std::max(percentile(r.band[b], 0.9f), 1e-6f);
    for (int t = 0; t < frames; t++) r.onset[t] += weight[b] * r.band[b][t] / scale;
  }
  // Remove the slow trend so builds and drops don't change the threshold unevenly.
  const std::vector<float> trend = smooth(r.onset, (int)(0.5 * kFps));
  std::vector<float> flat(frames);
  for (int t = 0; t < frames; t++) flat[t] = std::max(0.0f, r.onset[t] - 0.6f * trend[t]);
  pickPeaks(flat, trend, r);

  r.bpm = estimateTempo(flat, r.beatConfidence);
  r.beats = trackBeats(flat, r.bpm);
  if (r.beats.size() > 8) {
    std::vector<double> d;
    for (size_t i = 1; i < r.beats.size(); i++) d.push_back(r.beats[i] - r.beats[i - 1]);
    std::sort(d.begin(), d.end());
    r.bpm = 60.0 / d[d.size() / 2];
  }

  // Downbeat: of the four phases, the one whose beats carry the most low-band onset.
  double acc[4] = {0, 0, 0, 0};
  for (size_t i = 0; i < r.beats.size(); i++) {
    const int f = std::clamp((int)(r.beats[i] * kFps), 0, frames - 1);
    acc[i % 4] += r.band[0][f] + 0.3 * r.band[1][f];
  }
  r.downbeatPhase = (int)(std::max_element(acc, acc + 4) - acc);

  // Bars: four beats each, from the first downbeat.
  for (size_t i = r.downbeatPhase; i + 4 < r.beats.size(); i += 4) {
    Bar b{};
    b.start = r.beats[i], b.end = r.beats[i + 4];
    const int f0 = std::clamp((int)(b.start * kFps), 0, frames - 1), f1 = std::clamp((int)(b.end * kFps), f0 + 1, frames);
    double e[3] = {0, 0, 0}, dn = 0;
    for (int f = f0; f < f1; f++) {
      for (int k = 0; k < 3; k++) e[k] += r.energy[k][f];
      dn += flat[f];
    }
    const int n = f1 - f0;
    b.low = (float)std::sqrt(e[0] / n), b.mid = (float)std::sqrt(e[1] / n), b.high = (float)std::sqrt(e[2] / n);
    b.energy = (float)std::sqrt((e[0] + e[1] + e[2]) / n);
    b.density = (float)(dn / n);
    r.bars.push_back(b);
  }
  const int nb = (int)r.bars.size();
  if (nb < 4) return r;
  auto column = [&](float Bar::*m) {
    std::vector<float> v;
    for (auto &b : r.bars) v.push_back(b.*m);
    return v;
  };
  std::vector<float> lo = column(&Bar::low), mi = column(&Bar::mid), hi = column(&Bar::high), en = column(&Bar::energy),
                     de = column(&Bar::density);
  normalise01(lo), normalise01(mi), normalise01(hi), normalise01(en), normalise01(de);
  std::vector<float> inten(nb);
  for (int i = 0; i < nb; i++) inten[i] = 0.5f * en[i] + 0.25f * lo[i] + 0.25f * de[i];
  inten = smooth(inten, 1);
  for (int i = 0; i < nb; i++) {
    r.bars[i].low = lo[i], r.bars[i].mid = mi[i], r.bars[i].high = hi[i];
    r.bars[i].energy = en[i], r.bars[i].density = de[i], r.bars[i].intensity = inten[i];
  }

  // Structure. Breaks: the kick is gone. Drops: the kick returns hard after a break or build-up.
  // Builds: the bars before a drop whose intensity is climbing, or that sit in a break with
  // dense onsets (risers, snare rolls). The rest is steady, with an intro and outro at the ends.
  const float lowRef = std::max(percentile(lo, 0.75f), 0.2f);
  std::vector<bool> isBreak(nb), isDrop(nb);
  for (int i = 0; i < nb; i++) isBreak[i] = lo[i] < 0.35f * lowRef;
  for (int i = 1; i < nb; i++) {
    const float before = i >= 2 ? 0.5f * (lo[i - 1] + lo[i - 2]) : lo[i - 1];
    if (lo[i] > 0.6f * lowRef && lo[i] > 1.8f * before + 0.05f && (isBreak[i - 1] || before < 0.5f * lowRef)) isDrop[i] = true;
  }
  for (int i = 0; i < nb; i++) r.bars[i].section = isBreak[i] ? Section::Break : Section::Steady;
  for (int i = 0; i < nb; i++) {
    if (!isDrop[i]) continue;
    r.bars[i].section = Section::Drop;
    int j = i - 1, count = 0;
    while (j >= 0 && count < 8 && (isBreak[j] || inten[j] <= inten[j + 1] + 0.02f) && !isDrop[j]) {
      if (isBreak[j] && de[j] < 0.25f && count > 0 && inten[j] < 0.15f) break;
      r.bars[j].section = Section::Build;
      j--, count++;
    }
  }
  // Intro: from the start until the energy first reaches a third of its peak. Outro: the tail that has fallen below it.
  int intro = 0;
  while (intro < nb && inten[intro] < 0.35f && r.bars[intro].section == Section::Steady) intro++;
  // Quiet or kick-less bars before the first build or drop are still the intro, if that comes early.
  for (int i = 0; i < nb; i++)
    if (r.bars[i].section == Section::Build || r.bars[i].section == Section::Drop) {
      if (i < nb / 4) intro = std::max(intro, i);
      break;
    }
  for (int i = 0; i < intro; i++) r.bars[i].section = Section::Intro;
  // A lone break bar between steady bars is a fill, not a break.
  for (int i = intro + 1; i + 1 < nb; i++)
    if (r.bars[i].section == Section::Break && r.bars[i - 1].section == Section::Steady && r.bars[i + 1].section == Section::Steady)
      r.bars[i].section = Section::Steady;
  int outro = nb;
  while (outro > intro && inten[outro - 1] < 0.35f && r.bars[outro - 1].section != Section::Drop) outro--;
  for (int i = outro; i < nb; i++) r.bars[i].section = Section::Outro;
  return r;
}

}  // namespace analysis
