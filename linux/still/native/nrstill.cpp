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
  std::string mv = "zero";                                       // zero|none
  unsigned w = 0, h = 0, frames = 32;
  std::vector<unsigned> dump = {1, 2, 4, 8, 16, 32};
  int preset = 0, style = 0, autoMask = 1, uiCorr = 1, depthInverted = 1;
  float mvx = 0, mvy = 0;
  float intensity = 1.0f, lstruct = 1.0f, ltone = 1.0f, skin = -1.0f, flatDepth = 0.01f;
  bool resetEvery = false, verbose = false, noTunables = false, queryRatio = true, stream = false;
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

static uint16_t f2h(float f) {  // float -> half (no denormal/NaN care needed for [0,1])
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
  double submitWait() {
    cl->Close();
    ID3D12CommandList* l[] = {cl};
    LARGE_INTEGER t0, t1, fq; QueryPerformanceCounter(&t0); QueryPerformanceFrequency(&fq);
    q->ExecuteCommandLists(1, l);
    q->Signal(fence, ++fv);
    if (fence->GetCompletedValue() < fv) { fence->SetEventOnCompletion(fv, ev); WaitForSingleObject(ev, 20000); }
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
static bool writeExact(HANDLE h, const uint8_t* p, size_t n) {
  while (n) {
    DWORD chunk = (DWORD)(n > (1u << 24) ? (1u << 24) : n), wr = 0;
    if (!WriteFile(h, p, chunk, &wr, nullptr) || wr == 0) return false;
    p += wr; n -= wr;
  }
  return true;
}

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
    else if (k == "--reset-every") a.resetEvery = true;
    else if (k == "--stream") a.stream = true;
    else if (k == "--no-tunables") a.noTunables = true;
    else if (k == "--no-ratio") a.queryRatio = false;
    else if (k == "--fmt") a.fmt = nx();
    else if (k == "--uint-slot") g_uintSlot = atoi(nx());
    else if (k == "--res-slot") g_resSlot = atoi(nx());
    else if (k == "--verbose") { a.verbose = true; g_ngxLogLevel = 2; }
    else { printf("unknown arg %s\n", k.c_str()); return 64; }
  }
  if ((a.color.empty() && !a.stream) || !a.w || !a.h) {
    printf("usage: nrstill --color f.rgba --size W H [--depth flat|none|f.f32] ...\n"
           "       nrstill --stream --size W H [...]   (RGBA8 frames on stdin -> stdout)\n");
    return 64;
  }
  if (a.stream && a.fmt != "rgba8") { printf("--stream supports --fmt rgba8 only\n"); return 64; }
  const unsigned W = a.w, H = a.h;
  printf("config: color=%s %ux%u depth=%s mv=%s mvconst=%g,%g frames=%u preset=%d style=%d intensity=%g lstruct=%g ltone=%g skin=%g automask=%d fmt=%s uintSlot=%d resSlot=%d\n",
         a.color.c_str(), W, H, a.mode == "file" ? a.depthFile.c_str() : a.mode.c_str(), a.mv.c_str(), a.mvx, a.mvy, a.frames, a.preset,
         a.style, a.intensity, a.lstruct, a.ltone, a.skin, a.autoMask, a.fmt.c_str(), g_uintSlot, g_resSlot);

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
  ID3D12Resource* upColor = a.stream ? makeBuf(g.dev, ctotal, D3D12_HEAP_TYPE_UPLOAD) : nullptr;
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

  // ---- evaluate N times on the same still ----
  std::vector<uint8_t> frame((size_t)W * H * cbpp);
  double sumMs = 0;
  for (unsigned fi = 1; a.stream || fi <= a.frames; ++fi) {
    bool dumpIt = a.stream;
    for (unsigned d : a.dump) if (d == fi) dumpIt = true;
    if (fi == a.frames) dumpIt = true;
    if (a.stream && fi > 1) {
      int got = readFrame(rgba, &inFlags);
      if (got != 1) {
        if (got < 0) printf("stream: bad frame %u\n", fi);
        printf("stream: %u frames\n", fi - 1);
        break;
      }
    }
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
    setU(caps, "DLSSNR.Reset", (fi == 1 || a.resetEvery || (inFlags & 1)) ? 1u : 0u);
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
    g.begin();
    if (a.stream && fi > 1) {
      uint8_t* m = nullptr;
      upColor->Map(0, nullptr, (void**)&m);
      for (unsigned y = 0; y < H; ++y) memcpy(m + cfp.Offset + (size_t)y * cfp.Footprint.RowPitch, rgba.data() + (size_t)y * W * 4, (size_t)W * 4);
      upColor->Unmap(0, nullptr);
      barrier(g.cl, tColor, SRV, D3D12_RESOURCE_STATE_COPY_DEST);
      D3D12_TEXTURE_COPY_LOCATION dst = {}; dst.pResource = tColor; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      D3D12_TEXTURE_COPY_LOCATION src = {}; src.pResource = upColor; src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; src.PlacedFootprint = cfp;
      g.cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
      barrier(g.cl, tColor, D3D12_RESOURCE_STATE_COPY_DEST, SRV);
    }
    r = (NGXResult)call(sEval, (uintptr_t)g.cl, (uintptr_t)feature, (uintptr_t)caps, 0, 0);
    uavBarrier(g.cl, tOut);
    if (dumpIt) {
      barrier(g.cl, tOut, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
      D3D12_TEXTURE_COPY_LOCATION dst = {}; dst.pResource = rb; dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint = ofp;
      D3D12_TEXTURE_COPY_LOCATION src = {}; src.pResource = tOut; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      g.cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
      barrier(g.cl, tOut, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    double ms = g.submitWait();
    HRESULT dr = g.dev->GetDeviceRemovedReason();
    if (fi > 1) sumMs += ms;
    if (r != NGX_OK || FAILED(dr) || (dumpIt && !a.stream) || fi <= 2 || (a.stream && fi % 300 == 0))
      printf("frame %3u: evaluate -> 0x%08x %s, gpu+wait %.2f ms%s\n", fi, (unsigned)r, ngxName(r), ms,
             FAILED(dr) ? " DEVICE REMOVED" : "");
    if (FAILED(dr)) { printf("device removed reason 0x%08lx\n", (unsigned long)dr); return 8; }
    if (r != NGX_OK) break;
    if (a.stream) {
      uint8_t* m = nullptr; D3D12_RANGE rr = {0, (SIZE_T)ototal};
      rb->Map(0, &rr, (void**)&m);
      for (unsigned y = 0; y < H; ++y) memcpy(frame.data() + (size_t)y * W * 4, m + ofp.Offset + (size_t)y * ofp.Footprint.RowPitch, (size_t)W * 4);
      D3D12_RANGE wr = {0, 0}; rb->Unmap(0, &wr);
      uint8_t hdr[16] = {'N', 'R', 'F', 'R', 'A', 'M', 'E', '1'};
      uint32_t idx = fi, res = (uint32_t)r;
      memcpy(hdr + 8, &idx, 4); memcpy(hdr + 12, &res, 4);
      if (!writeExact(g_frameOut, hdr, sizeof hdr) || !writeExact(g_frameOut, frame.data(), frame.size())) {
        printf("stream: output closed at frame %u\n", fi);
        break;
      }
      continue;
    }
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
