// cod3 - the Rust renderer's library (see rust_renderer.h).

#include "render/rust_renderer.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/gpu_plugin.h>  // LoadGpuPlugin; RexHostRenderer stays opaque here

#if defined(COD3_HAS_RUST_RENDERER)
#include "cod3_render.h"
#endif

REXCVAR_DEFINE_STRING(cod3_renderer, "xenos", "cod3/Display",
                      "Who draws: \"xenos\", the emulated Xbox 360 GPU, or \"rust\", the Rust "
                      "renderer (in development): the GPU emulation is off and every draw goes to "
                      "it. It doesn't draw yet, so the picture stays black")
    .allowed({"xenos", "rust"})
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace cod3::render {

namespace {

#if defined(COD3_HAS_RUST_RENDERER)
// The library's log lines, into the game's log (from any thread).
void LogFromRust(void* /*user*/, uint32_t level, const char* message, size_t length) {
  const std::string_view text(message, length);
  if (level == RR_LOG_ERROR)
    REXLOG_ERROR("{}", text);
  else if (level == RR_LOG_WARNING)
    REXLOG_WARN("{}", text);
  else if (level == RR_LOG_DEBUG)
    REXLOG_DEBUG("{}", text);
  else
    REXLOG_INFO("{}", text);
}

RrHostApi MakeHostApi() {
  RrHostApi host{};
  host.struct_size = sizeof(host);
  host.abi_version = RR_ABI_VERSION;
  host.user = nullptr;
  host.log = &LogFromRust;
  return host;
}

// Whether the linked library speaks this game's ABI; says so if not. A game
// and a library from different builds never call further in.
bool LibraryMatches() {
  const uint32_t library_abi = rr_abi_version();
  if (library_abi == RR_ABI_VERSION) return true;
  REXLOG_ERROR("cod3: Rust renderer library {} speaks ABI {}, this game ABI {}; not used", rr_version(),
               library_abi, RR_ABI_VERSION);
  return false;
}

// The renderer the host backend calls, made once; it lives for the program
// (the plugin may call it until the process ends).
RrRenderer* g_host_renderer = nullptr;
#endif

}  // namespace

bool StartHostBackend(rex::RuntimeConfig& config) {
  if (REXCVAR_GET(cod3_renderer) != "rust" || config.graphics || config.gpu_plugin.empty()) return false;
  const RexHostRenderer* table = nullptr;
#if defined(COD3_HAS_RUST_RENDERER)
  if (!g_host_renderer && LibraryMatches()) {
    static const RrHostApi host = MakeHostApi();  // must outlive the renderer
    g_host_renderer = rr_create(&host);
  }
  table = rr_host_renderer(g_host_renderer);
#endif
  config.graphics = rex::system::LoadGpuPlugin(config.gpu_plugin, "host", table);
  if (!config.graphics) {
    REXLOG_ERROR("cod3: renderer rust: the GPU plugin's host backend didn't load; the emulated GPU draws");
    return false;
  }
  if (table)
    REXLOG_INFO("cod3: renderer rust: the GPU emulation is off and the Rust renderer gets every draw "
                "(it doesn't draw yet: the picture stays black)");
  else
    REXLOG_WARN("cod3: renderer rust: the GPU emulation is off, but this build has no Rust renderer, so "
                "nothing draws (the picture stays black)");
  return true;
}

void ReportRustRenderer() {
#if defined(COD3_HAS_RUST_RENDERER)
  if (!LibraryMatches()) return;
  if (g_host_renderer) {
    REXLOG_INFO("cod3: Rust renderer library {} linked (ABI {}); the host backend hands it the game's "
                "GPU work",
                rr_version(), RR_ABI_VERSION);
    return;
  }
  // Not in use: started and stopped once, to know it works.
  const RrHostApi host = MakeHostApi();
  RrRenderer* renderer = rr_create(&host);
  REXLOG_INFO("cod3: Rust renderer library {} linked (ABI {}){}", rr_version(), RR_ABI_VERSION,
              renderer ? "; not in use (cod3_renderer xenos)" : ", but it didn't start");
  rr_destroy(renderer);
#else
  REXLOG_INFO("cod3: Rust renderer library not in this build (the build found no Rust for its target)");
#endif
}

}  // namespace cod3::render
