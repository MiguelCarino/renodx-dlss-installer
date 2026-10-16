// SPDX-License-Identifier: GPL-3.0-only
// kind.h -- is a picture or video 2D animation or 3D/camera footage? (nrstill --classify)
//
// MobileNetV3-Large (torchvision's ImageNet weights, BSD-3-Clause, batch norm folded into the
// convolutions) as a feature extractor, then a logistic head trained on up to 10 frames from each
// of 123 labelled clips (kind-clips.txt): hand-drawn and cel animation, anime, 2D and painted games
// vs 3D animation, Blender films, demoscene, 3D games, film, TV and camera footage. Clip-grouped
// 6-fold cross-validation, five splits: about 82% of clips called right, 7.5% wrong, 10% "unsure"
// (|logit| <= 0.5). The misses: cel-shaded 3D games (Zelda, Sonic), smooth stylised 3D, painted
// anime scenery, pixel art. Smooth untextured abstract CG reads as 2D.
//
// Weights: still/bin/kind.bin -- "NRKIND01", uint32 tensor count, then per tensor uint32 n and n
// fp16 values, in forward order (made by kind-train.py). Each frame is squashed
// to 398x224 like the training frames (see input()), normalised with the ImageNet mean/std, and its logit is the
// head applied to the pooled 960 features; a clip's logit is the mean over its frames (> 0 = 2D).
#pragma once
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <vector>
#include <string>
#include <algorithm>

namespace kind {

static const int IW = 398, IH = 224;

struct Conv { int cin = 0, cout = 0, k = 1, stride = 1, act = 0; bool dw = false; std::vector<float> w, b; };  // act 0 none 1 relu 2 hswish
struct Se { int c = 0, s = 0; std::vector<float> w1, b1, w2, b2; };
struct Block { bool expand = false, se = false, res = false; Conv ex, dw, pr; Se sq; };
struct Net { Conv stem, last; std::vector<Block> blocks; std::vector<float> hw; float hb = 0; bool ok = false; };

static float h2f(uint16_t h) {
  uint32_t sign = (uint32_t)(h & 0x8000) << 16, e = (h >> 10) & 0x1f, m = h & 0x3ff, x;
  if (e == 0) {
    if (!m) x = sign;
    else { int s = -1; do { ++s; m <<= 1; } while (!(m & 0x400)); x = sign | ((uint32_t)(127 - 15 - s) << 23) | ((m & 0x3ff) << 13); }
  } else if (e == 31) x = sign | 0x7f800000 | (m << 13);
  else x = sign | ((e - 15 + 127) << 23) | (m << 13);
  float f; memcpy(&f, &x, 4); return f;
}

// torchvision's mobilenet_v3_large table: in, kernel, expanded, out, SE, hardswish, stride
static const int CFG[15][7] = {
    {16, 3, 16, 16, 0, 0, 1},    {16, 3, 64, 24, 0, 0, 2},    {24, 3, 72, 24, 0, 0, 1},
    {24, 5, 72, 40, 1, 0, 2},    {40, 5, 120, 40, 1, 0, 1},   {40, 5, 120, 40, 1, 0, 1},
    {40, 3, 240, 80, 0, 1, 2},   {80, 3, 200, 80, 0, 1, 1},   {80, 3, 184, 80, 0, 1, 1},
    {80, 3, 184, 80, 0, 1, 1},   {80, 3, 480, 112, 1, 1, 1},  {112, 3, 672, 112, 1, 1, 1},
    {112, 5, 672, 160, 1, 1, 2}, {160, 5, 960, 160, 1, 1, 1}, {160, 5, 960, 160, 1, 1, 1}};

static int squeeze(int c) { int v = (c / 4 + 4) / 8 * 8; if (v < 8) v = 8; if (v < 0.9 * (c / 4)) v += 8; return v; }

static bool load(const std::string& path, Net& n) {
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) { printf("classify: cannot open %s\n", path.c_str()); return false; }
  char magic[8]; uint32_t count = 0;
  bool ok = fread(magic, 1, 8, f) == 8 && !memcmp(magic, "NRKIND01", 8) && fread(&count, 4, 1, f) == 1;
  std::vector<std::vector<float>> t;
  for (uint32_t i = 0; ok && i < count; ++i) {
    uint32_t len = 0;
    if (fread(&len, 4, 1, f) != 1 || len > (1u << 24)) { ok = false; break; }
    std::vector<uint16_t> h(len);
    if (fread(h.data(), 2, len, f) != len) { ok = false; break; }
    std::vector<float> v(len);
    for (uint32_t j = 0; j < len; ++j) v[j] = h2f(h[j]);
    t.push_back(std::move(v));
  }
  fclose(f);
  size_t ti = 0;
  auto take = [&](size_t want) -> std::vector<float> {
    if (!ok || ti >= t.size() || t[ti].size() != want) { ok = false; return {}; }
    return std::move(t[ti++]);
  };
  auto conv = [&](Conv& c, int cin, int cout, int k, int stride, int act, bool dw) {
    c.cin = cin; c.cout = cout; c.k = k; c.stride = stride; c.act = act; c.dw = dw;
    c.w = take((size_t)cout * (dw ? 1 : cin) * k * k); c.b = take(cout);
  };
  conv(n.stem, 3, 16, 3, 2, 2, false);
  for (const auto& r : CFG) {
    Block b; int in = r[0], k = r[1], ex = r[2], out = r[3], act = r[5] ? 2 : 1;
    b.expand = ex != in; b.se = r[4]; b.res = r[6] == 1 && in == out;
    if (b.expand) conv(b.ex, in, ex, 1, 1, act, false);
    conv(b.dw, ex, ex, k, r[6], act, true);
    if (b.se) {
      b.sq.c = ex; b.sq.s = squeeze(ex);
      b.sq.w1 = take((size_t)b.sq.s * ex); b.sq.b1 = take(b.sq.s);
      b.sq.w2 = take((size_t)ex * b.sq.s); b.sq.b2 = take(ex);
    }
    conv(b.pr, ex, out, 1, 1, 0, false);
    n.blocks.push_back(std::move(b));
  }
  conv(n.last, 160, 960, 1, 1, 2, false);
  n.hw = take(960);
  std::vector<float> hb = take(1);
  if (ok) n.hb = hb[0];
  n.ok = ok && ti == t.size();
  if (!n.ok) printf("classify: %s is not a matching weights file\n", path.c_str());
  return n.ok;
}

static inline float act(float v, int a) {
  if (a == 1) return v > 0 ? v : 0;
  if (a == 2) { float r = v + 3; r = r < 0 ? 0 : r > 6 ? 6 : r; return v * r / 6; }
  return v;
}

// x: cin planes of h*w; returns cout planes of oh*ow (same padding (k-1)/2).
static std::vector<float> run(const Conv& c, const std::vector<float>& x, int h, int w, int& oh, int& ow) {
  const int p = (c.k - 1) / 2, k = c.k, s = c.stride;
  oh = (h + 2 * p - k) / s + 1; ow = (w + 2 * p - k) / s + 1;
  std::vector<float> y((size_t)c.cout * oh * ow);
  const size_t hw = (size_t)h * w, ohw = (size_t)oh * ow;
  if (k == 1 && s == 1 && !c.dw) {  // pointwise: y[o] = b + sum_i w[o][i] x[i]
    for (int o = 0; o < c.cout; ++o) {
      float* yo = &y[o * ohw];
      for (size_t j = 0; j < ohw; ++j) yo[j] = c.b[o];
      const float* wo = &c.w[(size_t)o * c.cin];
      for (int i = 0; i < c.cin; ++i) {
        const float wv = wo[i]; const float* xi = &x[i * hw];
        for (size_t j = 0; j < ohw; ++j) yo[j] += wv * xi[j];
      }
      for (size_t j = 0; j < ohw; ++j) yo[j] = act(yo[j], c.act);
    }
    return y;
  }
  for (int o = 0; o < c.cout; ++o) {
    float* yo = &y[o * ohw];
    const int i0 = c.dw ? o : 0, i1 = c.dw ? o + 1 : c.cin;
    for (int oy = 0; oy < oh; ++oy)
      for (int ox = 0; ox < ow; ++ox) {
        float acc = c.b[o];
        for (int i = i0; i < i1; ++i) {
          const float* wk = &c.w[((size_t)o * (c.dw ? 1 : c.cin) + (c.dw ? 0 : i)) * k * k];
          const float* xi = &x[i * hw];
          for (int ky = 0; ky < k; ++ky) {
            int iy = oy * s - p + ky;
            if (iy < 0 || iy >= h) continue;
            for (int kx = 0; kx < k; ++kx) {
              int ix = ox * s - p + kx;
              if (ix < 0 || ix >= w) continue;
              acc += wk[ky * k + kx] * xi[(size_t)iy * w + ix];
            }
          }
        }
        yo[(size_t)oy * ow + ox] = act(acc, c.act);
      }
  }
  return y;
}

// The training frames went through ffmpeg's area scaler to 480x270, then torch's area pooling
// (integer windows, equal weights) to 398x224; both steps are repeated here so the input matches.
static std::vector<float> input(const uint8_t* rgba, int sw, int sh) {
  static const float mean[3] = {0.485f, 0.456f, 0.406f}, sd[3] = {0.229f, 0.224f, 0.225f};
  const int MW = 480, MH = 270;
  std::vector<float> mid((size_t)3 * MW * MH);  // 0..255, CHW
  for (int y = 0; y < MH; ++y) {
    double y0 = (double)y * sh / MH, y1 = (double)(y + 1) * sh / MH;
    for (int xo = 0; xo < MW; ++xo) {
      double x0 = (double)xo * sw / MW, x1 = (double)(xo + 1) * sw / MW, acc[3] = {0, 0, 0}, wsum = 0;
      for (int sy = (int)y0; sy < (int)ceil(y1) && sy < sh; ++sy) {
        double wy = std::min(y1, sy + 1.0) - std::max(y0, (double)sy);
        for (int sx = (int)x0; sx < (int)ceil(x1) && sx < sw; ++sx) {
          double wv = wy * (std::min(x1, sx + 1.0) - std::max(x0, (double)sx));
          const uint8_t* p = rgba + ((size_t)sy * sw + sx) * 4;
          for (int c = 0; c < 3; ++c) acc[c] += wv * p[c];
          wsum += wv;
        }
      }
      // ffmpeg hands over 8-bit frames: round like it does
      for (int c = 0; c < 3; ++c) mid[(size_t)c * MW * MH + (size_t)y * MW + xo] = (float)floor(acc[c] / wsum + 0.5);
    }
  }
  std::vector<float> x((size_t)3 * IW * IH);
  for (int c = 0; c < 3; ++c)
    for (int y = 0; y < IH; ++y) {
      int ya = y * MH / IH, yb = ((y + 1) * MH + IH - 1) / IH;
      for (int xo = 0; xo < IW; ++xo) {
        int xa = xo * MW / IW, xb = ((xo + 1) * MW + IW - 1) / IW;
        double s = 0;
        for (int sy = ya; sy < yb; ++sy) for (int sx = xa; sx < xb; ++sx) s += mid[(size_t)c * MW * MH + (size_t)sy * MW + sx];
        x[(size_t)c * IW * IH + (size_t)y * IW + xo] = (float)((s / ((yb - ya) * (xb - xa)) / 255.0 - mean[c]) / sd[c]);
      }
    }
  return x;
}

// The 2D-vs-3D logit of one frame (> 0 leans 2D).
static float logit(const Net& n, const uint8_t* rgba, int sw, int sh) {
  std::vector<float> x = input(rgba, sw, sh);
  int h = IH, w = IW, oh, ow;
  x = run(n.stem, x, h, w, oh, ow); h = oh; w = ow;
  for (const Block& b : n.blocks) {
    std::vector<float> t = b.expand ? run(b.ex, x, h, w, oh, ow) : x;
    int dh, dwid;
    t = run(b.dw, t, h, w, dh, dwid);
    if (b.se) {
      const size_t hw = (size_t)dh * dwid;
      std::vector<float> m(b.sq.c), z(b.sq.s);
      for (int c = 0; c < b.sq.c; ++c) { double s = 0; for (size_t j = 0; j < hw; ++j) s += t[c * hw + j]; m[c] = (float)(s / hw); }
      for (int j = 0; j < b.sq.s; ++j) { float s = b.sq.b1[j]; for (int c = 0; c < b.sq.c; ++c) s += b.sq.w1[(size_t)j * b.sq.c + c] * m[c]; z[j] = s > 0 ? s : 0; }
      for (int c = 0; c < b.sq.c; ++c) {
        float s = b.sq.b2[c]; for (int j = 0; j < b.sq.s; ++j) s += b.sq.w2[(size_t)c * b.sq.s + j] * z[j];
        float g = (s + 3) / 6; g = g < 0 ? 0 : g > 1 ? 1 : g;
        for (size_t j = 0; j < hw; ++j) t[c * hw + j] *= g;
      }
    }
    int ph, pw;
    t = run(b.pr, t, dh, dwid, ph, pw);
    if (b.res) for (size_t j = 0; j < t.size(); ++j) t[j] += x[j];
    x.swap(t); h = ph; w = pw;
  }
  x = run(n.last, x, h, w, oh, ow);
  const size_t hw = (size_t)oh * ow;
  float z = n.hb;
  for (int c = 0; c < 960; ++c) { double s = 0; for (size_t j = 0; j < hw; ++j) s += x[c * hw + j]; z += n.hw[c] * (float)(s / hw); }
  return z;
}

}  // namespace kind
