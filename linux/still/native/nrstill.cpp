// SPDX-License-Identifier: GPL-3.0-only
// nrstill -- run DLSS 5 Neural Rendering (nvngx_dlssnr.dll, NGX feature 18) on a still image,
// headless, D3D12 under wine/vkd3d-proton. No window, no swap chain.
//
// Sequence (mirrors OptiScaler-dlssnr's forwarder, which is the reference):
//   core _nvngx.dll: Init_Ext -> GetCapabilityParameters (the block carrying snippet callbacks)
//   snippet nvngx_dlssnr.dll loaded directly, every snippet call made through nvngx.dll_nrshim.dll
//   snippet Init_Ext(appId, dataPath, dev, 0x15, caps) -> set create params -> CreateFeature(cmd, 18)
//   submit + fence the create, then N evaluates of the same frame (Reset=1 on the first), dumping
//   the output after selected frames.
//
// Inputs are raw files written by the caller (dlss5-gui.py): RGBA8 colour (sRGB-encoded, display-referred) and
// optionally float32 device depth (reverse-Z). Outputs: raw RGBA8 frames + a log line per frame.
//
// What the DLL (v310.8.0) reads, measured: one network config whatever the preset; Intensity is a 0..1 blend
// back toward the input; the effect's strength is LocalStructureStrength / LocalToneStrength (and Style).
// Feature_Flags, SDR/HDR hints, PreExposure, Sharpness, the AppID and a flat depth's value change nothing.
// --grade/--detail post-process the output (see Post); --mv flow estimates motion vectors (see Flow).
//
// NVSDK_NGX_Parameter is an MSVC object; its overloads sit in reverse declaration order in the
// vtable (checked by a start-up self-test): Set void*=0 D3D12=1 D3D11=2 int=3 uint=4 double=5 float=6 ull=7,
// Gets = +8, Reset = 16.
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#include <dxgi1_6.h>
#include <d3d12.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <cstdint>
#include <cmath>
#include <vector>
#include <string>
#include <initializer_list>
#include <algorithm>
#include <deque>
#include "kind.h"

typedef int NGXResult;
static const NGXResult NGX_OK = 1;
static const int NGX_VERSION_API = 0x15;

struct PathListInfo { const wchar_t* const* Path; unsigned Length; };
typedef void (*AppLogCallback)(const char* msg, int level, int component);
struct LoggingInfo { AppLogCallback cb; int minLevel; bool disableOtherSinks; };
struct FeatureCommonInfo { PathListInfo paths; void* internal; LoggingInfo log; };
struct ProjectIdDesc { const char* id; int engineType; const char* engineVersion; };
struct AppIdentifier { int type; union { ProjectIdDesc proj; unsigned long long appId; } v; };
struct FeatureDiscoveryInfo { int sdkVersion; int featureId; AppIdentifier ident; const wchar_t* dataPath; const FeatureCommonInfo* fci; };
struct FeatureRequirement { int supported; unsigned minHWArch; char minOS[255]; };

typedef NGXResult (*PFN_Init_Ext)(unsigned long long, const wchar_t*, ID3D12Device*, int, const FeatureCommonInfo*);
typedef NGXResult (*PFN_GetParams)(void** out);
typedef NGXResult (*PFN_Shutdown0)();
typedef NGXResult (*PFN_GetFeatureReq)(IDXGIAdapter*, const FeatureDiscoveryInfo*, FeatureRequirement*);
typedef long long (*PFN_ShimCall)(void* fn, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);

enum {
  S_VOIDP = 0, S_D3D12 = 1, S_D3D11 = 2, S_I = 3, S_UI = 4, S_D = 5, S_F = 6, S_ULL = 7,
  G_VOIDP = 8, G_D3D12 = 9, G_D3D11 = 10, G_I = 11, G_UI = 12, G_D = 13, G_F = 14, G_ULL = 15,
};
template <typename T> static NGXResult pget(void* p, int slot, const char* n, T* out) {
  typedef NGXResult (*F)(void*, const char*, T*);
  return ((F)((*(void***)p)[slot]))(p, n, out);
}
template <typename T> static void pset(void* p, int slot, const char* n, T v) {
  typedef void (*F)(void*, const char*, T);
  ((F)((*(void***)p)[slot]))(p, n, v);
}
static int g_uintSlot = S_UI;
static int g_resSlot = S_VOIDP;
static void setU(void* p, const char* n, unsigned v) { pset<unsigned>(p, g_uintSlot, n, v); }
static void setF(void* p, const char* n, float v) { pset<float>(p, S_F, n, v); }
static void setR(void* p, const char* n, ID3D12Resource* r) {
  if (g_resSlot == S_D3D12) pset<ID3D12Resource*>(p, S_D3D12, n, r);
  else pset<void*>(p, S_VOIDP, n, (void*)r);
}

static int g_ngxLogLevel = 1;
static void logcb(const char* msg, int level, int comp) {
  fprintf(stdout, "[ngx l%d c%d] %s", level, comp, msg);
  size_t n = strlen(msg);
  if (!n || msg[n - 1] != '\n') fputc('\n', stdout);
}

static const char* ngxName(unsigned r) {
  switch (r) {
    case 0x1: return "Success";
    case 0xBAD00000: return "Fail";
    case 0xBAD00001: return "FeatureNotSupported";
    case 0xBAD00002: return "PlatformError";
    case 0xBAD00003: return "FeatureAlreadyExists";
    case 0xBAD00004: return "FeatureNotFound";
    case 0xBAD00005: return "InvalidParameter";
    case 0xBAD00006: return "ScratchBufferTooSmall";
    case 0xBAD00007: return "NotInitialized";
    case 0xBAD00008: return "UnsupportedInputFormat";
    case 0xBAD00009: return "RWFlagMissing";
    case 0xBAD0000A: return "MissingInput";
    case 0xBAD0000B: return "UnableToInitializeFeature";
    case 0xBAD0000C: return "OutOfDate";
    case 0xBAD0000D: return "OutOfGPUMemory";
    case 0xBAD0000E: return "UnsupportedFormat";
    case 0xBAD0000F: return "UnableToWriteToAppDataPath";
    case 0xBAD00010: return "UnsupportedParameter";
    case 0xBAD00011: return "Denied";
    case 0xBAD00012: return "NotImplemented";
  }
  return "?";
}

#define CHECK(hr, what) do { HRESULT _h = (hr); if (FAILED(_h)) { printf("FAILED %s hr=0x%08lx\n", what, (unsigned long)_h); return 1; } } while (0)

struct Args {
  std::string color, depthFile, outDir = "out", mode = "flat";  // depth mode: flat|file|none
  std::string mv = "zero";                                       // zero|none|flow (flow: --stream only)
  unsigned w = 0, h = 0, frames = 32;
  std::vector<unsigned> dump = {1, 2, 4, 8, 16, 32};
  // preset: the DLL holds one network config and maps every preset to it; kept for experiments.
  int preset = 1, style = 0, autoMask = 1, uiCorr = 0, depthInverted = 1;
  float mvx = 0, mvy = 0;
  float intensity = 1.0f, lstruct = 1.0f, ltone = 1.0f, skin = -1.0f, flatDepth = 0.01f;
  float grade = 1.0f, detail = 1.0f;  // post split of the model's change, see Post; 1,1 = model output as is
  bool resetEvery = false, verbose = false, noTunables = false, queryRatio = true, stream = false;
  std::string classify;  // --classify f.rgba: N frames of --size W H, 2D-vs-3D only (no GPU)
  unsigned count = 1;
  std::string fmt = "rgba8";  // rgba8 | rgba16f (colour + output format)
};

static std::vector<unsigned> parseList(const char* s) {
  std::vector<unsigned> v; const char* p = s;
  while (*p) { v.push_back((unsigned)strtoul(p, (char**)&p, 10)); if (*p == ',') ++p; else break; }
  return v;
}

static bool readFile(const std::string& path, std::vector<uint8_t>& out, size_t expect) {
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) { printf("cannot open %s\n", path.c_str()); return false; }
  out.resize(expect);
  size_t n = fread(out.data(), 1, expect, f);
  fclose(f);
  if (n != expect) { printf("%s: read %zu, expected %zu\n", path.c_str(), n, expect); return false; }
  return true;
}

static uint16_t f2h(float f) {  // float -> half (tiny values flush to 0; no NaN care needed)
  uint32_t x; memcpy(&x, &f, 4);
  uint32_t sign = (x >> 16) & 0x8000;
  int e = (int)((x >> 23) & 0xff) - 127 + 15;
  uint32_t m = x & 0x7fffff;
  if (e <= 0) return (uint16_t)sign;
  if (e >= 31) return (uint16_t)(sign | 0x7c00);
  return (uint16_t)(sign | (e << 10) | (m >> 13));
}
static float h2f(uint16_t h) {
  uint32_t sign = (h & 0x8000) << 16, e = (h >> 10) & 0x1f, m = h & 0x3ff, x;
  if (e == 0) x = sign; else if (e == 31) x = sign | 0x7f800000 | (m << 13);
  else x = sign | ((e - 15 + 127) << 23) | (m << 13);
  float f; memcpy(&f, &x, 4); return f;
}

struct Gpu {
  ID3D12Device* dev = nullptr;
  ID3D12CommandQueue* q = nullptr;
  ID3D12CommandAllocator* alloc = nullptr;
  ID3D12GraphicsCommandList* cl = nullptr;
  ID3D12Fence* fence = nullptr;
  HANDLE ev = nullptr;
  UINT64 fv = 0;
  void begin() { alloc->Reset(); cl->Reset(alloc, nullptr); }
  void submit() {
    cl->Close();
    ID3D12CommandList* l[] = {cl};
    q->ExecuteCommandLists(1, l);
    q->Signal(fence, ++fv);
  }
  double wait() {  // ms spent waiting for the last submit
    LARGE_INTEGER t0, t1, fq; QueryPerformanceCounter(&t0); QueryPerformanceFrequency(&fq);
    if (fence->GetCompletedValue() < fv) { fence->SetEventOnCompletion(fv, ev); WaitForSingleObject(ev, 20000); }
    QueryPerformanceCounter(&t1);
    return (t1.QuadPart - t0.QuadPart) * 1000.0 / fq.QuadPart;
  }
  double submitWait() {
    LARGE_INTEGER t0, t1, fq; QueryPerformanceCounter(&t0); QueryPerformanceFrequency(&fq);
    submit(); wait();
    QueryPerformanceCounter(&t1);
    return (t1.QuadPart - t0.QuadPart) * 1000.0 / fq.QuadPart;
  }
};

static ID3D12Resource* makeTex(ID3D12Device* d, DXGI_FORMAT f, unsigned w, unsigned h, bool uav, D3D12_RESOURCE_STATES st) {
  D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC rd = {};
  rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; rd.Width = w; rd.Height = h; rd.DepthOrArraySize = 1;
  rd.MipLevels = 1; rd.Format = f; rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  rd.Flags = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
  ID3D12Resource* r = nullptr;
  HRESULT hr = d->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, st, nullptr, IID_PPV_ARGS(&r));
  if (FAILED(hr)) printf("CreateCommittedResource(fmt %d %ux%u) hr=0x%08lx\n", (int)f, w, h, (unsigned long)hr);
  return r;
}
static ID3D12Resource* makeBuf(ID3D12Device* d, UINT64 size, D3D12_HEAP_TYPE t) {
  D3D12_HEAP_PROPERTIES hp = {}; hp.Type = t;
  D3D12_RESOURCE_DESC rd = {};
  rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; rd.Width = size; rd.Height = 1; rd.DepthOrArraySize = 1;
  rd.MipLevels = 1; rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  ID3D12Resource* r = nullptr;
  D3D12_RESOURCE_STATES st = t == D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_COPY_DEST;
  HRESULT hr = d->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, st, nullptr, IID_PPV_ARGS(&r));
  if (FAILED(hr)) printf("CreateCommittedResource(buffer %llu) hr=0x%08lx\n", (unsigned long long)size, (unsigned long)hr);
  return r;
}
static void barrier(ID3D12GraphicsCommandList* cl, ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
  D3D12_RESOURCE_BARRIER br = {};
  br.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; br.Transition.pResource = r;
  br.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  br.Transition.StateBefore = a; br.Transition.StateAfter = b;
  cl->ResourceBarrier(1, &br);
}
static void uavBarrier(ID3D12GraphicsCommandList* cl, ID3D12Resource* r) {
  D3D12_RESOURCE_BARRIER br = {}; br.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV; br.UAV.pResource = r;
  cl->ResourceBarrier(1, &br);
}

// Upload tightly packed pixel rows (bpp bytes per pixel) into tex (which must be in COPY_DEST).
static ID3D12Resource* stageUpload(Gpu& g, ID3D12Resource* tex, const uint8_t* src, unsigned w, unsigned h, unsigned bpp) {
  D3D12_RESOURCE_DESC rd = tex->GetDesc();
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp; UINT64 total = 0;
  g.dev->GetCopyableFootprints(&rd, 0, 1, 0, &fp, nullptr, nullptr, &total);
  ID3D12Resource* up = makeBuf(g.dev, total, D3D12_HEAP_TYPE_UPLOAD);
  uint8_t* m = nullptr; up->Map(0, nullptr, (void**)&m);
  for (unsigned y = 0; y < h; ++y) memcpy(m + fp.Offset + (size_t)y * fp.Footprint.RowPitch, src + (size_t)y * w * bpp, (size_t)w * bpp);
  up->Unmap(0, nullptr);
  D3D12_TEXTURE_COPY_LOCATION dst = {}; dst.pResource = tex; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  D3D12_TEXTURE_COPY_LOCATION s = {}; s.pResource = up; s.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; s.PlacedFootprint = fp;
  g.cl->CopyTextureRegion(&dst, 0, 0, 0, &s, nullptr);
  return up;  // caller releases after the fence
}

// --stream: RGBA8 frames in on stdin, each preceded by a 16-byte header ("NRINPUT1", uint32 flags,
// uint32 reserved; flag 1 = reset the history, e.g. at a scene cut). Processed frames out on stdout,
// each preceded by "NRFRAME1", uint32 frame number from 1, uint32 NGX result. Everything else this
// process (and anything it loads) prints goes to stderr, so the frame pipe carries frames only.
static HANDLE g_frameIn = nullptr, g_frameOut = nullptr;

// Reads one input header + frame. Returns 1 ok, 0 clean end of stream, -1 error.
static bool readExact(HANDLE h, uint8_t* p, size_t n, bool* eofAtStart);
static int readFrame(std::vector<uint8_t>& px, uint32_t* flags) {
  uint8_t hdr[16]; bool eof = false;
  if (!readExact(g_frameIn, hdr, sizeof hdr, &eof)) return eof ? 0 : -1;
  if (memcmp(hdr, "NRINPUT1", 8)) { printf("stream: bad input header\n"); return -1; }
  memcpy(flags, hdr + 8, 4);
  return readExact(g_frameIn, px.data(), px.size(), nullptr) ? 1 : -1;
}

static bool readExact(HANDLE h, uint8_t* p, size_t n, bool* eofAtStart) {
  size_t got = 0;
  while (got < n) {
    DWORD chunk = (DWORD)((n - got) > (1u << 24) ? (1u << 24) : (n - got)), rd = 0;
    if (!ReadFile(h, p + got, chunk, &rd, nullptr) || rd == 0) break;
    got += rd;
  }
  if (eofAtStart) *eofAtStart = got == 0;
  return got == n;
}
static double nowMs() {
  LARGE_INTEGER t, f; QueryPerformanceCounter(&t); QueryPerformanceFrequency(&f);
  return t.QuadPart * 1000.0 / f.QuadPart;
}
static bool writeExact(HANDLE h, const uint8_t* p, size_t n) {
  while (n) {
    DWORD chunk = (DWORD)(n > (1u << 24) ? (1u << 24) : n), wr = 0;
    if (!WriteFile(h, p, chunk, &wr, nullptr) || wr == 0) return false;
    p += wr; n -= wr;
  }
  return true;
}

// Runs fn(y0, y1) over row bands on a small pool of persistent threads (creating threads per call
// costs milliseconds under Wine, and this runs several times per video frame).
struct RowPool {
  static const unsigned N = 8;
  HANDLE start[N] = {}, done[N] = {};
  void (*call)(void* ctx, unsigned y0, unsigned y1) = nullptr;
  void* ctx = nullptr;
  unsigned rows = 0;
  bool ready = false, failed = false;
  struct Arg { RowPool* pool; unsigned i; } args[N];
  static DWORD WINAPI worker(void* p) {
    Arg* a = (Arg*)p; RowPool* pool = a->pool;
    for (;;) {
      WaitForSingleObject(pool->start[a->i], INFINITE);
      pool->call(pool->ctx, pool->rows * a->i / N, pool->rows * (a->i + 1) / N);
      SetEvent(pool->done[a->i]);
    }
  }
  bool init() {
    if (ready || failed) return ready;
    for (unsigned i = 0; i < N; ++i) {
      start[i] = CreateEventW(nullptr, FALSE, FALSE, nullptr); done[i] = CreateEventW(nullptr, FALSE, FALSE, nullptr);
      args[i] = {this, i};
      HANDLE t = CreateThread(nullptr, 0, worker, &args[i], 0, nullptr);
      if (!t) { failed = true; return false; }  // threads already started just wait forever
      CloseHandle(t);
    }
    return ready = true;
  }
};
static RowPool g_pool, g_writerPool;  // one per calling thread: a pool runs one job at a time
template <typename F> static void parallelRows(RowPool& pool, unsigned H, F fn) {
  if (!pool.init()) { fn(0, H); return; }
  pool.call = [](void* c, unsigned y0, unsigned y1) { if (y1 > y0) (*(F*)c)(y0, y1); };
  pool.ctx = &fn; pool.rows = H;
  for (unsigned i = 0; i < RowPool::N; ++i) SetEvent(pool.start[i]);
  WaitForMultipleObjects(RowPool::N, pool.done, TRUE, INFINITE);
}

// ---- Post: split the model's change into a global colour map and the rest ----
// Most of what the model changes on ordinary footage is one colour map for the whole frame (darker
// midtones, less saturation, a hue shift), which reads as a colour grade. A least-squares cubic
// polynomial in (r,g,b) fitted from input to output captures that map; whatever it cannot explain
// is the spatially varying part: relit faces and materials, local contrast, detail.
//   final = in + grade * map(in) + detail * (out - in - map(in))      (map = fitted change)
// grade 1, detail 1 is the model's output unchanged. In a stream the fit is averaged over recent
// frames (reset with the history) so the removed grade cannot flicker.
struct Post {
  static const int K = 20;
  double A[K][K] = {}, B[K][3] = {}, c[K][3] = {};
  bool have = false;
  std::vector<float> lut;
  RowPool* pool = &g_pool;
  static void monos(float r, float g, float b, float* m) {
    m[0] = 1; m[1] = r; m[2] = g; m[3] = b;
    m[4] = r * r; m[5] = r * g; m[6] = r * b; m[7] = g * g; m[8] = g * b; m[9] = b * b;
    m[10] = r * r * r; m[11] = r * r * g; m[12] = r * r * b; m[13] = r * g * g; m[14] = r * g * b;
    m[15] = r * b * b; m[16] = g * g * g; m[17] = g * g * b; m[18] = g * b * b; m[19] = b * b * b;
  }
  // in/out: RGBA8 W*H. Rewrites out in place.
  void apply(const uint8_t* in, uint8_t* out, unsigned W, unsigned H, float grade, float detail, bool reset) {
    // Normal equations from every 6th pixel of every 6th row, one partial sum per thread band.
    struct Part { double a[K][K], b[K][3]; size_t n; };
    std::vector<Part> parts(RowPool::N);
    const unsigned rows = (H + 3) / 6;
    for (auto& pt : parts) memset(&pt, 0, sizeof pt);
    parallelRows(*pool, rows, [&](unsigned r0, unsigned r1) {
      unsigned band = 0;
      while (band < RowPool::N - 1 && rows * (band + 1) / RowPool::N <= r0) ++band;  // the band parallelRows gave us
      Part& pt = parts[band];
      float m[K];
      for (unsigned y = 3 + r0 * 6; y < H && y < 3 + r1 * 6; y += 6)
        for (unsigned x = 3; x < W; x += 6) {
          const uint8_t* pi = in + ((size_t)y * W + x) * 4; const uint8_t* po = out + ((size_t)y * W + x) * 4;
          monos(pi[0] / 255.0f, pi[1] / 255.0f, pi[2] / 255.0f, m);
          float d[3] = {(po[0] - pi[0]) / 255.0f, (po[1] - pi[1]) / 255.0f, (po[2] - pi[2]) / 255.0f};
          for (int i = 0; i < K; ++i) {
            for (int j = i; j < K; ++j) pt.a[i][j] += (double)m[i] * m[j];
            for (int ch = 0; ch < 3; ++ch) pt.b[i][ch] += (double)m[i] * d[ch];
          }
          ++pt.n;
        }
    });
    double a[K][K] = {}, b[K][3] = {}; size_t n = 0;
    for (auto& pt : parts) {
      n += pt.n;
      for (int i = 0; i < K; ++i) {
        for (int j = i; j < K; ++j) a[i][j] += pt.a[i][j];
        for (int ch = 0; ch < 3; ++ch) b[i][ch] += pt.b[i][ch];
      }
    }
    if (!n) return;
    const double keep = (have && !reset) ? 0.5 : 0.0;
    for (int i = 0; i < K; ++i) {
      for (int j = i; j < K; ++j) A[i][j] = A[j][i] = keep * A[i][j] + (1 - keep) * a[i][j] / n;
      for (int ch = 0; ch < 3; ++ch) B[i][ch] = keep * B[i][ch] + (1 - keep) * b[i][ch] / n;
    }
    have = true;
    // Solve (A + ridge) c = B by Gaussian elimination; the ridge keeps rare colours from swinging.
    double M[K][K + 3];
    for (int i = 0; i < K; ++i) {
      for (int j = 0; j < K; ++j) M[i][j] = A[i][j] + (i == j ? 1e-3 : 0);
      for (int ch = 0; ch < 3; ++ch) M[i][K + ch] = B[i][ch];
    }
    for (int col = 0; col < K; ++col) {
      int piv = col;
      for (int r = col + 1; r < K; ++r) if (fabs(M[r][col]) > fabs(M[piv][col])) piv = r;
      if (piv != col) for (int j = 0; j < K + 3; ++j) { double t = M[col][j]; M[col][j] = M[piv][j]; M[piv][j] = t; }
      if (fabs(M[col][col]) < 1e-12) continue;
      for (int r = 0; r < K; ++r) {
        if (r == col) continue;
        double f = M[r][col] / M[col][col];
        if (f != 0) for (int j = col; j < K + 3; ++j) M[r][j] -= f * M[col][j];
      }
    }
    for (int i = 0; i < K; ++i)
      for (int ch = 0; ch < 3; ++ch) c[i][ch] = fabs(M[i][i]) < 1e-12 ? 0 : M[i][K + ch] / M[i][i];
    // The map sampled on a 33^3 grid (in 0..255 units, pre-scaled by grade - detail), then
    // trilinear per pixel: a fraction of the cost of the polynomial, and the map is smooth.
    const int G = 33;
    const float gd = (grade - detail) * 255.0f;
    lut.resize((size_t)G * G * G * 3);
    for (int ri = 0; ri < G; ++ri)
      for (int gi = 0; gi < G; ++gi)
        for (int bi = 0; bi < G; ++bi) {
          float mm[K]; monos(ri / (G - 1.0f), gi / (G - 1.0f), bi / (G - 1.0f), mm);
          float* L = &lut[(((size_t)ri * G + gi) * G + bi) * 3];
          for (int ch = 0; ch < 3; ++ch) {
            double v = 0;
            for (int i = 0; i < K; ++i) v += mm[i] * c[i][ch];
            L[ch] = (float)v * gd;
          }
        }
    const float* lt = lut.data();
    parallelRows(*pool, H, [&](unsigned y0, unsigned y1) {
      for (size_t p = (size_t)y0 * W; p < (size_t)y1 * W; ++p) {
        const uint8_t* pi = in + p * 4; uint8_t* po = out + p * 4;
        float fr = pi[0] * ((G - 1) / 255.0f), fg = pi[1] * ((G - 1) / 255.0f), fb = pi[2] * ((G - 1) / 255.0f);
        int r0 = (int)fr, g0 = (int)fg, b0 = (int)fb;
        r0 -= r0 == G - 1; g0 -= g0 == G - 1; b0 -= b0 == G - 1;
        float tr = fr - r0, tg = fg - g0, tb = fb - b0;
        const float* c000 = lt + (((size_t)r0 * G + g0) * G + b0) * 3;
        const size_t sR = (size_t)G * G * 3, sG = (size_t)G * 3, sB = 3;
        for (int ch = 0; ch < 3; ++ch) {
          const float* q = c000 + ch;
          float x00 = q[0] + (q[sB] - q[0]) * tb, x01 = q[sG] + (q[sG + sB] - q[sG]) * tb;
          float x10 = q[sR] + (q[sR + sB] - q[sR]) * tb, x11 = q[sR + sG] + (q[sR + sG + sB] - q[sR + sG]) * tb;
          float y0v = x00 + (x01 - x00) * tg, y1v = x10 + (x11 - x10) * tg;
          float v = pi[ch] + detail * (float)(po[ch] - pi[ch]) + y0v + (y1v - y0v) * tr;
          v = v < 0 ? 0 : v > 255 ? 255 : v;
          po[ch] = (uint8_t)(v + 0.5f);
        }
      }
    });
  }
};

// ---- Flow: motion vectors estimated from the frames (--stream --mv flow) ----
// Hierarchical block matching on half-resolution luma (levels 1/2, 1/4, 1/8, 1/16 of the frame, 12x12
// blocks): a full +-4 search on the coarsest level, then each finer level tries the doubled vectors of
// the parent block and its four neighbours, and zero, and refines the best by +-1 steps; sub-pixel by
// a parabola through the costs. Every cost carries a small penalty for leaving the parent's vector, so
// flat areas follow their surroundings instead of noise. Then a 3x3 median and bilinear upsampling to
// every pixel, with a 0.5 px dead zone. Vectors follow the DLSS convention: previous position minus
// current position, in pixels (MVecScale 1); content that came from 8 px to the left gets (-8, 0).
// Checked against OpenCV's Farneback flow on live-action pairs: the same or lower warp error.
struct Flow {
  static const int L = 4, BS = 12;
  unsigned W = 0, H = 0, lw[L] = {}, lh[L] = {}, bw[L] = {}, bh[L] = {};
  std::vector<float> cur[L], prev[L], vx[L], vy[L];
  std::vector<float> fx, fy;  // level-0 vectors after the median, in level-0 pixels
  bool havePrev = false;
  double meanX = 0, meanY = 0, still = 0;  // mean vector (px) and share of blocks without motion
  void init(unsigned w, unsigned h) {
    W = w; H = h;
    for (int l = 0; l < L; ++l) {
      lw[l] = l ? (lw[l - 1] + 1) / 2 : (W + 1) / 2; lh[l] = l ? (lh[l - 1] + 1) / 2 : (H + 1) / 2;
      bw[l] = (lw[l] + BS - 1) / BS; bh[l] = (lh[l] + BS - 1) / BS;
      cur[l].assign((size_t)lw[l] * lh[l], 0); prev[l] = cur[l];
      vx[l].assign((size_t)bw[l] * bh[l], 0); vy[l] = vx[l];
    }
  }
  void pyramid(const uint8_t* rgba) {
    std::vector<float>& c0 = cur[0];
    parallelRows(g_pool, lh[0], [&](unsigned y0, unsigned y1) {
      for (unsigned y = y0; y < y1; ++y)
        for (unsigned x = 0; x < lw[0]; ++x) {
          float s = 0;
          for (unsigned dy = 0; dy < 2; ++dy)
            for (unsigned dx = 0; dx < 2; ++dx) {
              unsigned sx = x * 2 + dx < W ? x * 2 + dx : W - 1, sy = y * 2 + dy < H ? y * 2 + dy : H - 1;
              const uint8_t* p = rgba + ((size_t)sy * W + sx) * 4;
              s += 0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2];
            }
          c0[(size_t)y * lw[0] + x] = s * 0.25f;
        }
    });
    for (int l = 1; l < L; ++l)
      for (unsigned y = 0; y < lh[l]; ++y)
        for (unsigned x = 0; x < lw[l]; ++x) {
          float s = 0;
          for (unsigned dy = 0; dy < 2; ++dy)
            for (unsigned dx = 0; dx < 2; ++dx) {
              unsigned sx = x * 2 + dx < lw[l - 1] ? x * 2 + dx : lw[l - 1] - 1, sy = y * 2 + dy < lh[l - 1] ? y * 2 + dy : lh[l - 1] - 1;
              s += cur[l - 1][(size_t)sy * lw[l - 1] + sx];
            }
          cur[l][(size_t)y * lw[l] + x] = s * 0.25f;
        }
  }
  float cost(int l, unsigned bx, unsigned by, int dx, int dy) const {
    const int w = (int)lw[l], h = (int)lh[l];
    const float* c = cur[l].data(); const float* p = prev[l].data();
    float s = 0;
    for (int y = (int)by * BS; y < (int)by * BS + BS && y < h; ++y) {
      int py = y + dy; py = py < 0 ? 0 : py >= h ? h - 1 : py;
      for (int x = (int)bx * BS; x < (int)bx * BS + BS && x < w; ++x) {
        int px = x + dx; px = px < 0 ? 0 : px >= w ? w - 1 : px;
        s += fabsf(c[y * w + x] - p[py * w + px]);
      }
    }
    return s;
  }
  // Fills mv (R16G16 halves, W*H*2) for the frame; zero when there is no usable previous frame.
  void estimate(const uint8_t* rgba, bool reset, std::vector<uint16_t>& mv) {
    for (int l = 0; l < L; ++l) cur[l].swap(prev[l]);
    pyramid(rgba);
    std::fill(mv.begin(), mv.end(), (uint16_t)0);
    meanX = meanY = still = 0;
    if (reset || !havePrev) { havePrev = true; return; }
    for (int l = L - 1; l >= 0; --l)
      parallelRows(g_pool, bh[l], [&](unsigned b0, unsigned b1) {
        for (unsigned by = b0; by < b1; ++by)
          for (unsigned bx = 0; bx < bw[l]; ++bx) {
            int cx = 0, cy = 0;
            unsigned px = 0, py = 0;
            if (l < L - 1) {
              px = bx / 2 < bw[l + 1] ? bx / 2 : bw[l + 1] - 1; py = by / 2 < bh[l + 1] ? by / 2 : bh[l + 1] - 1;
              cx = (int)lroundf(vx[l + 1][py * bw[l + 1] + px] * 2); cy = (int)lroundf(vy[l + 1][py * bw[l + 1] + px] * 2);
            }
            auto pen = [&](int dx, int dy) { return 8.0f * (abs(dx - cx) + abs(dy - cy)); };
            int bestX = cx, bestY = cy; float best = cost(l, bx, by, cx, cy);
            auto tryv = [&](int dx, int dy) { float cc = cost(l, bx, by, dx, dy) + pen(dx, dy); if (cc < best) { best = cc; bestX = dx; bestY = dy; } };
            tryv(0, 0);
            if (l == L - 1) {
              for (int dy = -4; dy <= 4; ++dy) for (int dx = -4; dx <= 4; ++dx) tryv(dx, dy);
            } else {
              // the parent level's neighbours too, so a block on a motion edge can pick the right side
              for (int k = 0; k < 4; ++k) {
                int nx = (int)px + (k == 0 ? -1 : k == 1 ? 1 : 0), ny = (int)py + (k == 2 ? -1 : k == 3 ? 1 : 0);
                if (nx < 0 || ny < 0 || nx >= (int)bw[l + 1] || ny >= (int)bh[l + 1]) continue;
                tryv((int)lroundf(vx[l + 1][ny * bw[l + 1] + nx] * 2), (int)lroundf(vy[l + 1][ny * bw[l + 1] + nx] * 2));
              }
            }
            for (int it = 0; it < 3; ++it) {
              int ox = bestX, oy = bestY;
              for (int dy = -1; dy <= 1; ++dy) for (int dx = -1; dx <= 1; ++dx) if (dx || dy) tryv(ox + dx, oy + dy);
              if (ox == bestX && oy == bestY) break;
            }
            float fxv = (float)bestX, fyv = (float)bestY;
            if (l == 0) {  // sub-pixel: parabola through the costs either side
              float c0 = cost(l, bx, by, bestX, bestY);
              float cl = cost(l, bx, by, bestX - 1, bestY), cr = cost(l, bx, by, bestX + 1, bestY);
              float cu = cost(l, bx, by, bestX, bestY - 1), cd = cost(l, bx, by, bestX, bestY + 1);
              float ddx = cl + cr - 2 * c0, ddy = cu + cd - 2 * c0;
              if (ddx > 0) fxv += fmaxf(-0.5f, fminf(0.5f, 0.5f * (cl - cr) / ddx));
              if (ddy > 0) fyv += fmaxf(-0.5f, fminf(0.5f, 0.5f * (cu - cd) / ddy));
            }
            vx[l][by * bw[l] + bx] = fxv; vy[l][by * bw[l] + bx] = fyv;
          }
      });
    // 3x3 median per component on level 0, then bilinear between block centres, scaled to full res.
    const unsigned BW = bw[0], BH = bh[0];
    fx.assign((size_t)BW * BH, 0); fy = fx;
    for (unsigned by = 0; by < BH; ++by)
      for (unsigned bx = 0; bx < BW; ++bx)
        for (int comp = 0; comp < 2; ++comp) {
          const std::vector<float>& v = comp ? vy[0] : vx[0];
          float t[9]; int k = 0;
          for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
              int x = (int)bx + dx, y = (int)by + dy;
              x = x < 0 ? 0 : x >= (int)BW ? BW - 1 : x; y = y < 0 ? 0 : y >= (int)BH ? BH - 1 : y;
              t[k++] = v[y * BW + x];
            }
          std::nth_element(t, t + 4, t + 9);
          (comp ? fy : fx)[by * BW + bx] = t[4];
        }
    const float blk = 2.0f * BS;  // block size in full-res pixels
    double sx = 0, sy = 0;
    for (size_t i = 0; i < fx.size(); ++i) { sx += fx[i]; sy += fy[i]; still += fx[i] * fx[i] + fy[i] * fy[i] < 0.0625f; }
    meanX = 2 * sx / fx.size(); meanY = 2 * sy / fy.size(); still /= fx.size();
    parallelRows(g_pool, H, [&](unsigned y0, unsigned y1) {
      for (unsigned y = y0; y < y1; ++y) {
        float gy = (y + 0.5f) / blk - 0.5f; gy = gy < 0 ? 0 : gy > BH - 1 ? (float)(BH - 1) : gy;
        unsigned y0b = (unsigned)gy, y1b = y0b + 1 < BH ? y0b + 1 : y0b; float ty = gy - y0b;
        for (unsigned x = 0; x < W; ++x) {
          float gx = (x + 0.5f) / blk - 0.5f; gx = gx < 0 ? 0 : gx > BW - 1 ? (float)(BW - 1) : gx;
          unsigned x0b = (unsigned)gx, x1b = x0b + 1 < BW ? x0b + 1 : x0b; float tx = gx - x0b;
          float out[2];
          for (int comp = 0; comp < 2; ++comp) {
            const float* v = (comp ? fy : fx).data();
            float a = v[y0b * BW + x0b] + (v[y0b * BW + x1b] - v[y0b * BW + x0b]) * tx;
            float b = v[y1b * BW + x0b] + (v[y1b * BW + x1b] - v[y1b * BW + x0b]) * tx;
            out[comp] = 2.0f * (a + (b - a) * ty);
          }
          if (out[0] * out[0] + out[1] * out[1] < 0.25f) out[0] = out[1] = 0;  // 0.5 px dead zone
          mv[((size_t)y * W + x) * 2] = f2h(out[0]); mv[((size_t)y * W + x) * 2 + 1] = f2h(out[1]);
        }
      }
    });
  }
};

// --stream output on its own thread: the grade/detail pass and the pipe write of one frame overlap
// the motion estimate and evaluate of the next.
// --stream input on its own thread, up to two frames ahead: moving a frame through the Wine pipe
// costs as much CPU time as the motion estimate, so the two run side by side.
struct Reader {
  CRITICAL_SECTION cs; CONDITION_VARIABLE cv;
  std::deque<std::pair<std::vector<uint8_t>, uint32_t>> ready;
  std::vector<std::vector<uint8_t>> spare;
  size_t size = 0;
  int end = 1;  // readFrame's result once the input is over: 0 clean end, -1 error
  bool start(size_t frameBytes) {
    size = frameBytes; InitializeCriticalSection(&cs); InitializeConditionVariable(&cv);
    HANDLE t = CreateThread(nullptr, 0, run, this, 0, nullptr);
    if (t) CloseHandle(t);
    return t != nullptr;
  }
  static DWORD WINAPI run(void* p) {
    Reader* rd = (Reader*)p;
    for (;;) {
      std::vector<uint8_t> buf;
      EnterCriticalSection(&rd->cs);
      while (rd->ready.size() >= 2) SleepConditionVariableCS(&rd->cv, &rd->cs, INFINITE);
      if (!rd->spare.empty()) { buf.swap(rd->spare.back()); rd->spare.pop_back(); }
      LeaveCriticalSection(&rd->cs);
      buf.resize(rd->size);
      uint32_t flags = 0;
      int got = readFrame(buf, &flags);
      EnterCriticalSection(&rd->cs);
      if (got == 1) rd->ready.emplace_back(std::move(buf), flags);
      else rd->end = got;
      WakeAllConditionVariable(&rd->cv);
      LeaveCriticalSection(&rd->cs);
      if (got != 1) return 0;
    }
  }
  // Swaps the next frame into px (px's old buffer is reused); returns 1, or 0/-1 at the end.
  int take(std::vector<uint8_t>& px, uint32_t* flags) {
    EnterCriticalSection(&cs);
    while (ready.empty() && end == 1) SleepConditionVariableCS(&cv, &cs, INFINITE);
    int got = 1;
    if (ready.empty()) got = end;
    else {
      px.swap(ready.front().first); *flags = ready.front().second;
      spare.push_back(std::move(ready.front().first)); ready.pop_front();
      WakeAllConditionVariable(&cv);
    }
    LeaveCriticalSection(&cs);
    return got;
  }
};

struct Writer {
  std::vector<uint8_t> in, out;
  uint32_t idx = 0, res = 0;
  bool reset = false, post = false, busy = false;  // busy: handed a frame, idle not yet seen
  float grade = 1, detail = 1;
  unsigned W = 0, H = 0;
  Post fx;
  HANDLE full = nullptr, idle = nullptr;
  volatile LONG closed = 0;
  double ms = 0;
  bool start(unsigned w, unsigned h) {
    W = w; H = h; in.resize((size_t)W * H * 4); out.resize(in.size()); fx.pool = &g_writerPool;
    full = CreateEventW(nullptr, FALSE, FALSE, nullptr); idle = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    HANDLE t = CreateThread(nullptr, 0, run, this, 0, nullptr);
    if (t) CloseHandle(t);
    return t != nullptr;
  }
  static DWORD WINAPI run(void* p) {
    Writer* w = (Writer*)p;
    for (;;) {
      WaitForSingleObject(w->full, INFINITE);
      double t0 = nowMs();
      if (w->post) w->fx.apply(w->in.data(), w->out.data(), w->W, w->H, w->grade, w->detail, w->reset);
      w->ms += nowMs() - t0;
      uint8_t hdr[16] = {'N', 'R', 'F', 'R', 'A', 'M', 'E', '1'};
      memcpy(hdr + 8, &w->idx, 4); memcpy(hdr + 12, &w->res, 4);
      if (!writeExact(g_frameOut, hdr, sizeof hdr) || !writeExact(g_frameOut, w->out.data(), w->out.size())) w->closed = 1;
      SetEvent(w->idle);
    }
  }
};

int main(int argc, char** argv) {
  for (int i = 1; i < argc; ++i)
    if (!strcmp(argv[i], "--stream")) {
      // Keep the real stdout for frames, then point fd 1 and STD_OUTPUT_HANDLE at stderr.
      HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE), err = GetStdHandle(STD_ERROR_HANDLE);
      DuplicateHandle(GetCurrentProcess(), out, GetCurrentProcess(), &g_frameOut, 0, FALSE, DUPLICATE_SAME_ACCESS);
      g_frameIn = GetStdHandle(STD_INPUT_HANDLE);
      _setmode(_fileno(stdin), _O_BINARY);
      _dup2(_fileno(stderr), _fileno(stdout));
      SetStdHandle(STD_OUTPUT_HANDLE, err);
    }
  setvbuf(stdout, nullptr, _IONBF, 0);
  {  // headless-wine deadlock workaround (without it the first D3D12 call can hang)
    HWND desk = GetDesktopWindow();
    DISPLAY_DEVICEW dd = {}; dd.cb = sizeof(dd);
    BOOL ok = EnumDisplayDevicesW(nullptr, 0, &dd, 0);
    printf("desktop=%p EnumDisplayDevices=%d '%ls'\n", (void*)desk, ok, ok ? dd.DeviceString : L"");
  }
  Args a;
  for (int i = 1; i < argc; ++i) {
    std::string k = argv[i];
    auto nx = [&]() -> const char* { return i + 1 < argc ? argv[++i] : ""; };
    if (k == "--color") a.color = nx();
    else if (k == "--size") { a.w = atoi(nx()); a.h = atoi(nx()); }
    else if (k == "--depth") { a.mode = nx(); if (a.mode != "flat" && a.mode != "none") { a.depthFile = a.mode; a.mode = "file"; } }
    else if (k == "--flat-depth") a.flatDepth = (float)atof(nx());
    else if (k == "--depth-inverted") a.depthInverted = atoi(nx());
    else if (k == "--mv") a.mv = nx();
    else if (k == "--mv-const") { a.mvx = (float)atof(nx()); a.mvy = (float)atof(nx()); }
    else if (k == "--frames") a.frames = atoi(nx());
    else if (k == "--dump") a.dump = parseList(nx());
    else if (k == "--out") a.outDir = nx();
    else if (k == "--preset") a.preset = atoi(nx());
    else if (k == "--style") a.style = atoi(nx());
    else if (k == "--intensity") a.intensity = (float)atof(nx());
    else if (k == "--local-structure") a.lstruct = (float)atof(nx());
    else if (k == "--local-tone") a.ltone = (float)atof(nx());
    else if (k == "--skin") a.skin = (float)atof(nx());
    else if (k == "--automask") a.autoMask = atoi(nx());
    else if (k == "--ui-correction") a.uiCorr = atoi(nx());
    else if (k == "--grade") a.grade = (float)atof(nx());
    else if (k == "--detail") a.detail = (float)atof(nx());
    else if (k == "--reset-every") a.resetEvery = true;
    else if (k == "--stream") a.stream = true;
    else if (k == "--classify") a.classify = nx();
    else if (k == "--count") a.count = atoi(nx());
    else if (k == "--no-tunables") a.noTunables = true;
    else if (k == "--no-ratio") a.queryRatio = false;
    else if (k == "--fmt") a.fmt = nx();
    else if (k == "--uint-slot") g_uintSlot = atoi(nx());
    else if (k == "--res-slot") g_resSlot = atoi(nx());
    else if (k == "--verbose") { a.verbose = true; g_ngxLogLevel = 2; }
    else { printf("unknown arg %s\n", k.c_str()); return 64; }
  }
  if (!a.classify.empty()) {  // 2D-vs-3D guess only: CPU, no D3D12 or NGX (see kind.h)
    if (!a.w || !a.h || !a.count || a.count > 64) { printf("usage: nrstill --classify f.rgba --size W H --count N\n"); return 64; }
    std::vector<uint8_t> px;
    if (!readFile(a.classify, px, (size_t)a.w * a.h * 4 * a.count)) return 2;
    char exe[MAX_PATH]; GetModuleFileNameA(nullptr, exe, MAX_PATH); *strrchr(exe, '\\') = 0;
    static kind::Net net;
    if (!kind::load(std::string(exe) + "\\kind.bin", net)) return 3;
    struct Job { const uint8_t* px; int w, h; float z; };
    std::vector<Job> jobs(a.count);
    for (unsigned i = 0; i < a.count; ++i) jobs[i] = {px.data() + (size_t)a.w * a.h * 4 * i, (int)a.w, (int)a.h, 0};
    auto work = [](void* p) -> DWORD { Job* j = (Job*)p; j->z = kind::logit(net, j->px, j->w, j->h); return 0; };
    double t0 = nowMs();
    for (unsigned i = 0; i < a.count; i += 8) {  // a frame per thread, eight at a time
      std::vector<HANDLE> th;
      for (unsigned j = i; j < a.count && j < i + 8; ++j) th.push_back(CreateThread(nullptr, 0, work, &jobs[j], 0, nullptr));
      WaitForMultipleObjects((DWORD)th.size(), th.data(), TRUE, INFINITE);
      for (HANDLE t : th) CloseHandle(t);
    }
    double z = 0;
    for (unsigned i = 0; i < a.count; ++i) { printf("classify: frame %u logit %.3f\n", i + 1, jobs[i].z); z += jobs[i].z; }
    z /= a.count;
    // |logit| < 0.5 is where most cross-validated misses sat: say so rather than guess.
    printf("content: %s logit %.3f (%u frames, %.0f ms)\n", z > 0.5 ? "2d" : z < -0.5 ? "3d" : "unsure", z, a.count, nowMs() - t0);
    fflush(stdout);
    ExitProcess(0);
  }
  if ((a.color.empty() && !a.stream) || !a.w || !a.h) {
    printf("usage: nrstill --color f.rgba --size W H [--depth flat|none|f.f32] ...\n"
           "       nrstill --stream --size W H [--mv zero|none|flow] [...]   (RGBA8 frames on stdin -> stdout)\n"
           "  effect: --style 0|1|2 --local-structure S --local-tone T --intensity 0..1 --grade 0..1 --detail D\n");
    return 64;
  }
  if (a.stream && a.fmt != "rgba8") { printf("--stream supports --fmt rgba8 only\n"); return 64; }
  if (a.mv != "zero" && a.mv != "none" && a.mv != "flow") { printf("--mv takes zero, none or flow\n"); return 64; }
  if (a.mv == "flow" && !a.stream) a.mv = "zero";  // one still has no motion
  // The model treats intensity as a blend toward the input: values above 1 act as 1.
  a.intensity = a.intensity < 0 ? 0 : a.intensity > 1 ? 1 : a.intensity;
  const bool post = a.grade != 1.0f || a.detail != 1.0f;
  const unsigned W = a.w, H = a.h;
  printf("config: color=%s %ux%u depth=%s mv=%s mvconst=%g,%g frames=%u preset=%d style=%d intensity=%g lstruct=%g ltone=%g skin=%g automask=%d uicorr=%d grade=%g detail=%g fmt=%s uintSlot=%d resSlot=%d\n",
         a.color.c_str(), W, H, a.mode == "file" ? a.depthFile.c_str() : a.mode.c_str(), a.mv.c_str(), a.mvx, a.mvy, a.frames, a.preset,
         a.style, a.intensity, a.lstruct, a.ltone, a.skin, a.autoMask, a.uiCorr, a.grade, a.detail, a.fmt.c_str(), g_uintSlot, g_resSlot);

  std::vector<uint8_t> rgba, depthRaw;
  uint32_t inFlags = 0;
  if (a.stream) {
    rgba.resize((size_t)W * H * 4);
    int got = readFrame(rgba, &inFlags);
    if (got != 1) { printf(got == 0 ? "stream: no frames\n" : "stream: bad first frame\n"); return 2; }
  } else if (!readFile(a.color, rgba, (size_t)W * H * 4)) return 2;
  if (a.mode == "file" && !readFile(a.depthFile, depthRaw, (size_t)W * H * 4)) return 2;
  CreateDirectoryA(a.outDir.c_str(), nullptr);

  wchar_t exeDir[MAX_PATH];
  GetModuleFileNameW(nullptr, exeDir, MAX_PATH);
  *wcsrchr(exeDir, L'\\') = 0;

  // ---- D3D12 ----
  IDXGIFactory4* factory = nullptr;
  CHECK(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "CreateDXGIFactory2");
  IDXGIAdapter1* adapter = nullptr;
  for (UINT i = 0; factory->EnumAdapters1(i, &adapter) == S_OK; ++i) {
    DXGI_ADAPTER_DESC1 d; adapter->GetDesc1(&d);
    if (d.VendorId == 0x10de) { printf("adapter: %ls (0x%04x)\n", d.Description, d.DeviceId); break; }
    adapter->Release(); adapter = nullptr;
  }
  if (!adapter) { printf("no NVIDIA adapter\n"); return 1; }
  Gpu g;
  CHECK(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&g.dev)), "D3D12CreateDevice");
  D3D12_COMMAND_QUEUE_DESC qd = {}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  CHECK(g.dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&g.q)), "CreateCommandQueue");
  CHECK(g.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g.alloc)), "CreateCommandAllocator");
  CHECK(g.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g.alloc, nullptr, IID_PPV_ARGS(&g.cl)), "CreateCommandList");
  g.cl->Close();
  CHECK(g.dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g.fence)), "CreateFence");
  g.ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);

  // ---- NGX core ----
  HMODULE ngx = LoadLibraryW(L"_nvngx.dll");
  if (!ngx) { printf("LoadLibrary(_nvngx.dll) failed %lu\n", GetLastError()); return 3; }
  auto coreInit = (PFN_Init_Ext)GetProcAddress(ngx, "NVSDK_NGX_D3D12_Init_Ext");
  auto coreCaps = (PFN_GetParams)GetProcAddress(ngx, "NVSDK_NGX_D3D12_GetCapabilityParameters");
  auto coreShutdown = (PFN_Shutdown0)GetProcAddress(ngx, "NVSDK_NGX_D3D12_Shutdown");
  auto coreReq = (PFN_GetFeatureReq)GetProcAddress(ngx, "NVSDK_NGX_D3D12_GetFeatureRequirements");
  const wchar_t* paths[] = {exeDir};
  FeatureCommonInfo fci = {};
  fci.paths.Path = paths; fci.paths.Length = 1;
  fci.log.cb = logcb; fci.log.minLevel = g_ngxLogLevel; fci.log.disableOtherSinks = false;
  if (coreReq) {
    FeatureDiscoveryInfo di = {}; di.sdkVersion = NGX_VERSION_API; di.featureId = 18;
    di.ident.type = 0; di.ident.v.appId = 0x24480451ull; di.dataPath = exeDir; di.fci = &fci;
    FeatureRequirement fr = {};
    NGXResult r = coreReq(adapter, &di, &fr);
    printf("core GetFeatureRequirements(18) -> 0x%08x %s supported=0x%x minArch=0x%x\n", (unsigned)r, ngxName(r), fr.supported, fr.minHWArch);
  }
  NGXResult r = coreInit(0x24480451ull, exeDir, g.dev, NGX_VERSION_API, &fci);
  printf("core Init_Ext -> 0x%08x %s\n", (unsigned)r, ngxName(r));
  if (r != NGX_OK) return 3;
  void* caps = nullptr;
  r = coreCaps(&caps);
  printf("core GetCapabilityParameters -> 0x%08x %p\n", (unsigned)r, caps);
  if (r != NGX_OK || !caps) return 3;
  for (const char* k : {"DLSSNR.Available", "DLSSNR.FeatureInitResult", "DLSSNR.NeedsUpdatedDriver", "SuperSampling.Available"}) {
    int iv = -1; unsigned uv = 0;
    NGXResult ri = pget<int>(caps, G_I, k, &iv); NGXResult ru = pget<unsigned>(caps, G_UI, k, &uv);
    printf("  caps %-28s int=%d(0x%x) uint=0x%x(0x%x)\n", k, iv, (unsigned)ri, uv, (unsigned)ru);
  }
  {  // block typing probe: does a uint written via slot 3 (int) read back via Get(uint) and vice versa?
    unsigned u = 0; int i = 0; float f = 0;
    pset<int>(caps, S_I, "nrstill.probe.i", 7); NGXResult a1 = pget<unsigned>(caps, G_UI, "nrstill.probe.i", &u);
    pset<unsigned>(caps, S_UI, "nrstill.probe.u", 9); NGXResult a2 = pget<int>(caps, G_I, "nrstill.probe.u", &i);
    pset<float>(caps, S_F, "nrstill.probe.f", 0.375f); NGXResult a3 = pget<float>(caps, G_F, "nrstill.probe.f", &f);
    void* pv = nullptr; pset<void*>(caps, S_VOIDP, "nrstill.probe.p", (void*)g.dev);
    ID3D12Resource* pr = nullptr; NGXResult a4 = pget<ID3D12Resource*>(caps, G_D3D12, "nrstill.probe.p", &pr);
    NGXResult a5 = pget<void*>(caps, G_VOIDP, "nrstill.probe.p", &pv);
    printf("block probe: int->getUint %u (0x%x)  uint->getInt %d (0x%x)  float slot6->14 %g (0x%x)  void*->getD3D12 %p (0x%x) ->getVoid %p (0x%x)\n",
           u, (unsigned)a1, i, (unsigned)a2, f, (unsigned)a3, (void*)pr, (unsigned)a4, pv, (unsigned)a5);
  }

  // ---- snippet + shim ----
  std::wstring snipPath = std::wstring(exeDir) + L"\\nvngx_dlssnr.dll";
  HMODULE shim = LoadLibraryW((std::wstring(exeDir) + L"\\nvngx.dll_nrshim.dll").c_str());
  if (!shim) { printf("LoadLibrary(nvngx.dll_nrshim.dll) failed %lu\n", GetLastError()); return 4; }
  auto call = (PFN_ShimCall)GetProcAddress(shim, "nrshim_call");
  HMODULE snip = LoadLibraryExW(snipPath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  if (!snip) { printf("LoadLibraryEx(nvngx_dlssnr.dll) failed %lu\n", GetLastError()); return 4; }
  void* sInit = (void*)GetProcAddress(snip, "NVSDK_NGX_D3D12_Init_Ext");
  void* sCreate = (void*)GetProcAddress(snip, "NVSDK_NGX_D3D12_CreateFeature");
  void* sEval = (void*)GetProcAddress(snip, "NVSDK_NGX_D3D12_EvaluateFeature");
  void* sRelease = (void*)GetProcAddress(snip, "NVSDK_NGX_D3D12_ReleaseFeature");
  void* sPopulate = (void*)GetProcAddress(snip, "NVSDK_NGX_D3D12_PopulateParameters_Impl");
  printf("snippet %ls: init=%p create=%p eval=%p release=%p populate=%p\n", snipPath.c_str(), sInit, sCreate, sEval, sRelease, sPopulate);
  if (!sInit || !sCreate || !sEval) return 4;

  r = (NGXResult)call(sInit, 0x24480451ull, (uintptr_t)exeDir, (uintptr_t)g.dev, NGX_VERSION_API, (uintptr_t)caps);
  printf("snippet Init_Ext -> 0x%08x %s\n", (unsigned)r, ngxName(r));
  if (r != NGX_OK) return 5;

  if (a.queryRatio && sPopulate) {
    NGXResult pr = (NGXResult)call(sPopulate, (uintptr_t)caps, 0, 0, 0, 0);
    unsigned long long cb = 0;
    NGXResult gr = pget<unsigned long long>(caps, G_VOIDP, "DLSSNRComputeScalingRatioCallback", &cb);
    if (!cb) gr = pget<unsigned long long>(caps, G_ULL, "DLSSNRComputeScalingRatioCallback", &cb);
    printf("snippet PopulateParameters_Impl -> 0x%08x; ratio callback=%llx (0x%x)\n", (unsigned)pr, cb, (unsigned)gr);
    if (cb) {
      static const char* qn[] = {"MaxPerf", "Balanced", "MaxQuality", "UltraPerf", "UltraQuality", "DLAA"};
      for (unsigned q = 0; q < 6; ++q) {
        setU(caps, "PerfQualityValue", q);
        setF(caps, "DLSSNR.ScalingRatio", -1.0f);
        NGXResult rr = (NGXResult)call((void*)cb, (uintptr_t)caps, 0, 0, 0, 0);
        float ratio = -2; pget<float>(caps, G_F, "DLSSNR.ScalingRatio", &ratio);
        printf("  scaling ratio %-12s -> 0x%08x ratio=%g\n", qn[q], (unsigned)rr, ratio);
      }
    }
  }

  // ---- resources ----
  const bool f16 = a.fmt == "rgba16f";
  const DXGI_FORMAT cfmt = f16 ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM;
  const unsigned cbpp = f16 ? 8 : 4;
  std::vector<uint8_t> colorBytes;
  if (f16) {
    colorBytes.resize((size_t)W * H * 8);
    uint16_t* hp = (uint16_t*)colorBytes.data();
    for (size_t i = 0; i < (size_t)W * H * 4; ++i) hp[i] = f2h(rgba[i] / 255.0f);
  } else colorBytes = rgba;
  std::vector<float> depth((size_t)W * H, a.flatDepth);
  if (a.mode == "file") memcpy(depth.data(), depthRaw.data(), depthRaw.size());
  std::vector<uint16_t> mv((size_t)W * H * 2, 0);
  for (size_t i = 0; i < (size_t)W * H; ++i) { mv[i * 2] = f2h(a.mvx); mv[i * 2 + 1] = f2h(a.mvy); }

  ID3D12Resource* tColor = makeTex(g.dev, cfmt, W, H, false, D3D12_RESOURCE_STATE_COPY_DEST);
  ID3D12Resource* tDepth = makeTex(g.dev, DXGI_FORMAT_R32_FLOAT, W, H, false, D3D12_RESOURCE_STATE_COPY_DEST);
  ID3D12Resource* tMv = makeTex(g.dev, DXGI_FORMAT_R16G16_FLOAT, W, H, false, D3D12_RESOURCE_STATE_COPY_DEST);
  ID3D12Resource* tOut = makeTex(g.dev, cfmt, W, H, true, D3D12_RESOURCE_STATE_COPY_DEST);
  if (!tColor || !tDepth || !tMv || !tOut) return 6;
  // Stream mode re-uploads the colour every frame from one persistent upload buffer.
  D3D12_RESOURCE_DESC cd = tColor->GetDesc();
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT cfp; UINT64 ctotal = 0;
  g.dev->GetCopyableFootprints(&cd, 0, 1, 0, &cfp, nullptr, nullptr, &ctotal);
  // Two of each: the next frame is written into one while the GPU still copies from the other.
  ID3D12Resource* upColor[2] = {};
  if (a.stream) for (auto*& u : upColor) u = makeBuf(g.dev, ctotal, D3D12_HEAP_TYPE_UPLOAD);
  // ... and, with --mv flow, the motion vectors.
  const bool flow = a.mv == "flow";
  D3D12_RESOURCE_DESC md = tMv->GetDesc();
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT mfp; UINT64 mtotal = 0;
  g.dev->GetCopyableFootprints(&md, 0, 1, 0, &mfp, nullptr, nullptr, &mtotal);
  ID3D12Resource* upMv[2] = {};
  if (flow) for (auto*& u : upMv) u = makeBuf(g.dev, mtotal, D3D12_HEAP_TYPE_UPLOAD);
  Flow fl; if (flow) fl.init(W, H);
  Post postFx;
  Writer writer;
  Reader reader;
  if (a.stream) {
    writer.post = post; writer.grade = a.grade; writer.detail = a.detail;
    if (!writer.start(W, H)) { printf("stream: cannot start the writer thread\n"); return 6; }
    if (!reader.start(rgba.size())) { printf("stream: cannot start the reader thread\n"); return 6; }
  }
  // Output pre-filled with a sentinel (pure magenta, alpha 0) so "model wrote nothing" is detectable.
  std::vector<uint8_t> sentinel((size_t)W * H * cbpp);
  for (size_t i = 0; i < (size_t)W * H; ++i) {
    if (f16) { uint16_t* p = (uint16_t*)&sentinel[i * 8]; p[0] = f2h(1); p[1] = 0; p[2] = f2h(1); p[3] = 0; }
    else { sentinel[i * 4] = 255; sentinel[i * 4 + 1] = 0; sentinel[i * 4 + 2] = 255; sentinel[i * 4 + 3] = 0; }
  }
  D3D12_RESOURCE_DESC od = tOut->GetDesc();
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT ofp; UINT64 ototal = 0;
  g.dev->GetCopyableFootprints(&od, 0, 1, 0, &ofp, nullptr, nullptr, &ototal);
  ID3D12Resource* rb = makeBuf(g.dev, ototal, D3D12_HEAP_TYPE_READBACK);

  const D3D12_RESOURCE_STATES SRV = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
  g.begin();
  ID3D12Resource* ups[4] = {
      stageUpload(g, tColor, colorBytes.data(), W, H, cbpp), stageUpload(g, tDepth, (uint8_t*)depth.data(), W, H, 4),
      stageUpload(g, tMv, (uint8_t*)mv.data(), W, H, 4), stageUpload(g, tOut, sentinel.data(), W, H, cbpp)};
  barrier(g.cl, tColor, D3D12_RESOURCE_STATE_COPY_DEST, SRV);
  barrier(g.cl, tDepth, D3D12_RESOURCE_STATE_COPY_DEST, SRV);
  barrier(g.cl, tMv, D3D12_RESOURCE_STATE_COPY_DEST, SRV);
  barrier(g.cl, tOut, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  printf("upload: %.1f ms\n", g.submitWait());
  for (auto* u : ups) u->Release();

  auto setTunables = [&]() {
    setU(caps, "DLSSNR.Hint.Render.Preset", (unsigned)a.preset);
    if (a.noTunables) return;
    setF(caps, "DLSSNR.Intensity", a.intensity);
    setU(caps, "DLSSNR.Style", (unsigned)a.style);
    setF(caps, "DLSSNR.LocalStructureStrength", a.lstruct);
    setF(caps, "DLSSNR.LocalToneStrength", a.ltone);
    setF(caps, "DLSSNR.SkinStructureStrength", a.skin);
    setU(caps, "DLSSNR.UseAutoMask", (unsigned)a.autoMask);
    setU(caps, "DLSSNR.UICorrection", (unsigned)a.uiCorr);
  };

  // ---- create (own command list, fenced) ----
  setU(caps, "DLSSNR.Enabled", 1);
  setU(caps, "DLSSNR.Width", W);
  setU(caps, "DLSSNR.Height", H);
  setU(caps, "CreationNodeMask", 1);
  setU(caps, "VisibilityNodeMask", 1);
  setTunables();
  g.begin();
  void* feature = nullptr;
  r = (NGXResult)call(sCreate, (uintptr_t)g.cl, 18, (uintptr_t)caps, (uintptr_t)&feature, 0);
  printf("snippet CreateFeature(18, %ux%u) -> 0x%08x %s handle=%p\n", W, H, (unsigned)r, ngxName(r), feature);
  double ct = g.submitWait();
  printf("create submit+fence: %.1f ms\n", ct);
  if (r != NGX_OK || !feature) { if (coreShutdown) coreShutdown(); return 7; }
  if (FAILED(g.dev->GetDeviceRemovedReason())) { printf("device removed after create: 0x%08lx\n", (unsigned long)g.dev->GetDeviceRemovedReason()); return 7; }

  // ---- evaluate: N times on the same still, or once per --stream frame ----
  std::vector<uint8_t> frame((size_t)W * H * cbpp);
  double sumMs = 0, flowMs = 0, postMs = 0, waitMs = 0, readMs = 0;
  unsigned fi = 1;
  auto setEval = [&](bool reset) {
    setR(caps, "DLSSNR.Color", tColor);
    setR(caps, "DLSSNR.Output", tOut);
    setR(caps, "DLSSNR.Depth", a.mode == "none" ? nullptr : tDepth);
    setR(caps, "DLSSNR.MVec", a.mv == "none" ? nullptr : tMv);
    setR(caps, "DLSSNR.UI", nullptr);
    setR(caps, "DLSSNR.UIAlpha", nullptr);
    setR(caps, "DLSSNR.Backbuffer", nullptr);
    setU(caps, "DLSSNR.Enabled", 1);
    setU(caps, "DLSSNR.Width", W);
    setU(caps, "DLSSNR.Height", H);
    setU(caps, "DLSSNR.DepthInverted", (unsigned)a.depthInverted);
    setU(caps, "DLSSNR.Reset", reset ? 1u : 0u);
    for (const char* base : {"Color", "Output", "Depth", "MVec"}) {
      char k[96];
      snprintf(k, sizeof k, "DLSSNR.%sSubrectBaseX", base); setU(caps, k, 0);
      snprintf(k, sizeof k, "DLSSNR.%sSubrectBaseY", base); setU(caps, k, 0);
      snprintf(k, sizeof k, "DLSSNR.%sSubrectWidth", base); setU(caps, k, W);
      snprintf(k, sizeof k, "DLSSNR.%sSubrectHeight", base); setU(caps, k, H);
    }
    setF(caps, "DLSSNR.MVecScaleX", 1.0f);
    setF(caps, "DLSSNR.MVecScaleY", 1.0f);
    setTunables();
  };
  auto copyIn = [&](ID3D12Resource* up, ID3D12Resource* tex, const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& fp) {
    barrier(g.cl, tex, SRV, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION dst = {}; dst.pResource = tex; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION src = {}; src.pResource = up; src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; src.PlacedFootprint = fp;
    g.cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    barrier(g.cl, tex, D3D12_RESOURCE_STATE_COPY_DEST, SRV);
  };
  auto fill = [&](ID3D12Resource* up, const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& fp, const uint8_t* src) {
    uint8_t* m = nullptr;
    up->Map(0, nullptr, (void**)&m);
    for (unsigned y = 0; y < H; ++y) memcpy(m + fp.Offset + (size_t)y * fp.Footprint.RowPitch, src + (size_t)y * W * 4, (size_t)W * 4);
    up->Unmap(0, nullptr);
  };
  auto readOut = [&]() {
    barrier(g.cl, tOut, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION dst = {}; dst.pResource = rb; dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint = ofp;
    D3D12_TEXTURE_COPY_LOCATION src = {}; src.pResource = tOut; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    g.cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    barrier(g.cl, tOut, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  };

  if (a.stream) {
    // Pipelined: while the GPU evaluates frame fi, frame fi+1 is taken from the reader thread, its motion
    // estimated and both written into the other upload slot; only then is fi's fence waited on.
    std::vector<uint8_t> next(rgba.size());
    std::vector<uint16_t> nextMv(flow ? mv.size() : 0);
    bool curReset = true;
    if (flow) { double t0 = nowMs(); fl.estimate(rgba.data(), true, mv); flowMs += nowMs() - t0; }  // seeds the pyramid; vectors zero
    unsigned slot = 0;
    for (;; ++fi) {
      setEval(curReset);
      g.begin();
      if (fi > 1) {
        copyIn(upColor[slot], tColor, cfp);
        if (flow) copyIn(upMv[slot], tMv, mfp);
      }
      r = (NGXResult)call(sEval, (uintptr_t)g.cl, (uintptr_t)feature, (uintptr_t)caps, 0, 0);
      uavBarrier(g.cl, tOut);
      readOut();
      g.submit();
      double t0 = nowMs();
      uint32_t nextFlags = 0;
      int got = r == NGX_OK ? reader.take(next, &nextFlags) : 0;
      const bool nextReset = a.resetEvery || (nextFlags & 1);
      double t1 = nowMs(); readMs += t1 - t0; t0 = t1;
      if (got == 1) {
        if (flow) { fl.estimate(next.data(), nextReset, nextMv); fill(upMv[slot ^ 1], mfp, (uint8_t*)nextMv.data()); }
        fill(upColor[slot ^ 1], cfp, next.data());
      }
      flowMs += nowMs() - t0;
      double ms = g.wait();
      HRESULT dr = g.dev->GetDeviceRemovedReason();
      if (fi > 1) waitMs += ms;
      if (r != NGX_OK || FAILED(dr) || fi <= 2 || fi % 300 == 0)
        printf("frame %3u: evaluate -> 0x%08x %s, gpu wait %.2f ms%s%s\n", fi, (unsigned)r, ngxName(r), ms,
               FAILED(dr) ? " DEVICE REMOVED" : "", curReset ? " (reset)" : "");
      if (flow && (fi <= 3 || fi % 300 == 0)) printf("frame %3u: mean motion vector %.2f, %.2f px, %.0f%% of blocks still\n", fi, fl.meanX, fl.meanY, fl.still * 100);
      if (FAILED(dr)) { printf("device removed reason 0x%08lx\n", (unsigned long)dr); return 8; }
      if (r != NGX_OK) break;
      if (writer.busy) WaitForSingleObject(writer.idle, INFINITE);  // the previous frame is out
      writer.busy = false;
      if (writer.closed) { printf("stream: output closed at frame %u\n", fi - 1); break; }
      uint8_t* m = nullptr; D3D12_RANGE rr = {0, (SIZE_T)ototal};
      rb->Map(0, &rr, (void**)&m);
      for (unsigned y = 0; y < H; ++y) memcpy(writer.out.data() + (size_t)y * W * 4, m + ofp.Offset + (size_t)y * ofp.Footprint.RowPitch, (size_t)W * 4);
      D3D12_RANGE wr = {0, 0}; rb->Unmap(0, &wr);
      if (post) memcpy(writer.in.data(), rgba.data(), rgba.size());
      // The grade-fit average restarts at real discontinuities only: under --reset-every the model's
      // history resets every frame, but the removed colour map must still be averaged or it flickers.
      writer.idx = fi; writer.res = (uint32_t)r; writer.reset = fi == 1 || (inFlags & 1);
      writer.busy = true;
      SetEvent(writer.full);
      if (got != 1) {
        if (got < 0) printf("stream: bad frame %u\n", fi + 1);
        printf("stream: %u frames\n", fi);
        ++fi;
        break;
      }
      rgba.swap(next); inFlags = nextFlags; curReset = nextReset; slot ^= 1;
    }
    if (writer.busy) WaitForSingleObject(writer.idle, INFINITE);  // the last frame is out
    if (writer.closed) printf("stream: output closed at the last frame\n");
    postMs = writer.ms;
    if (fi > 1) printf("per frame, overlapping the GPU: input wait %.1f ms, motion %.1f ms; then gpu wait %.1f ms; grade/detail %.1f ms (writer thread)\n",
                       readMs / (fi - 1), flowMs / (fi - 1), waitMs / (fi > 2 ? fi - 2 : 1), postMs / (fi - 1));
  }

  for (; !a.stream && fi <= a.frames; ++fi) {
    bool dumpIt = fi == a.frames;
    for (unsigned d : a.dump) if (d == fi) dumpIt = true;
    const bool reset = fi == 1 || a.resetEvery;
    setEval(reset);
    g.begin();
    r = (NGXResult)call(sEval, (uintptr_t)g.cl, (uintptr_t)feature, (uintptr_t)caps, 0, 0);
    uavBarrier(g.cl, tOut);
    if (dumpIt) readOut();
    double ms = g.submitWait();
    HRESULT dr = g.dev->GetDeviceRemovedReason();
    if (fi > 1) sumMs += ms;
    if (r != NGX_OK || FAILED(dr) || dumpIt || fi <= 2)
      printf("frame %3u: evaluate -> 0x%08x %s, gpu+wait %.2f ms%s%s\n", fi, (unsigned)r, ngxName(r), ms,
             FAILED(dr) ? " DEVICE REMOVED" : "", reset ? " (reset)" : "");
    if (FAILED(dr)) { printf("device removed reason 0x%08lx\n", (unsigned long)dr); return 8; }
    if (r != NGX_OK) break;
    if (dumpIt) {
      uint8_t* m = nullptr; D3D12_RANGE rr = {0, (SIZE_T)ototal};
      rb->Map(0, &rr, (void**)&m);
      for (unsigned y = 0; y < H; ++y) memcpy(frame.data() + (size_t)y * W * cbpp, m + ofp.Offset + (size_t)y * ofp.Footprint.RowPitch, (size_t)W * cbpp);
      D3D12_RANGE wr = {0, 0}; rb->Unmap(0, &wr);
      // convert to RGBA8 for the file + stats vs input
      std::vector<uint8_t> o8((size_t)W * H * 4);
      size_t sentinelPx = 0; double mad = 0;
      for (size_t i = 0; i < (size_t)W * H; ++i) {
        for (int c = 0; c < 4; ++c) {
          float v = f16 ? h2f(((uint16_t*)frame.data())[i * 4 + c]) : frame[i * 4 + c] / 255.0f;
          v = v < 0 ? 0 : v > 1 ? 1 : v;
          o8[i * 4 + c] = (uint8_t)(v * 255.0f + 0.5f);
        }
        if (o8[i * 4] == 255 && o8[i * 4 + 1] == 0 && o8[i * 4 + 2] == 255 && o8[i * 4 + 3] == 0) ++sentinelPx;
        for (int c = 0; c < 3; ++c) mad += fabsf((float)o8[i * 4 + c] - (float)rgba[i * 4 + c]);
      }
      mad /= (double)W * H * 3;
      if (post) {
        postFx.apply(rgba.data(), o8.data(), W, H, a.grade, a.detail, true);
        double pm = 0;
        for (size_t i = 0; i < (size_t)W * H; ++i) for (int c = 0; c < 3; ++c) pm += fabsf((float)o8[i * 4 + c] - (float)rgba[i * 4 + c]);
        printf("  model mean|out-in| = %.3f; after grade %g / detail %g:\n", mad, a.grade, a.detail);
        mad = pm / ((double)W * H * 3);
      }
      char fn[512]; snprintf(fn, sizeof fn, "%s\\frame_%03u.rgba", a.outDir.c_str(), fi);
      FILE* f = fopen(fn, "wb"); if (f) { fwrite(o8.data(), 1, o8.size(), f); fclose(f); }
      printf("  dumped %s  mean|out-in| (0..255) = %.3f  untouched(sentinel) px = %zu\n", fn, mad, sentinelPx);
    }
  }
  if (!a.stream && a.frames > 1) printf("mean per-evaluate submit+fence time (frames 2..%u): %.2f ms\n", a.frames, sumMs / (a.frames - 1));

  if (sRelease) { NGXResult rr = (NGXResult)call(sRelease, (uintptr_t)feature, 0, 0, 0, 0); printf("snippet ReleaseFeature -> 0x%08x\n", (unsigned)rr); }
  g.begin(); g.submitWait();
  if (coreShutdown) { NGXResult sr = coreShutdown(); printf("core Shutdown -> 0x%08x\n", (unsigned)sr); }
  printf("done\n");
  fflush(stdout);
  // Skip teardown of D3D objects: the process exits right after, and NGX's own teardown order on
  // wine is the part we have least confidence in (Shutdown1 already AVs in _nvngx.dll).
  ExitProcess(0);
}
