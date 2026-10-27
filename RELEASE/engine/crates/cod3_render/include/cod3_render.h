/*
 * cod3_render: the C ABI of the Rust renderer library (engine/crates/cod3_render/src/lib.rs;
 * the plan is in dev/agents/rust-renderer/). M1 step 2: it takes the SDK host backend's draws
 * (rr_host_renderer) and counts them; it draws nothing yet.
 *
 * Kept in step with lib.rs by hand: the same names, the same struct layout (its tests check the
 * layout this header promises), the same RR_ABI_VERSION. Every struct starts with struct_size
 * and abi_version; the library refuses one it doesn't know. What the library allocates it frees
 * itself (rr_destroy).
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The ABI this header describes; rr_abi_version() says the library's. 2: rr_host_renderer. */
#define RR_ABI_VERSION 2u

/* Log levels of RrHostApi.log. */
#define RR_LOG_ERROR 0u
#define RR_LOG_WARNING 1u
#define RR_LOG_INFO 2u
#define RR_LOG_DEBUG 3u

typedef struct RrRenderer RrRenderer; /* opaque: made by rr_create, freed by rr_destroy */

/* The SDK's host renderer table (rex/graphics/host_renderer.h, SDK patch 0100). */
struct RexHostRenderer;

/* The game's log: message is UTF-8, length bytes, not NUL-terminated. Any thread. */
typedef void (*RrLogFn)(void* user, uint32_t level, const char* message, size_t length);

/* What the game gives the library. Grows at its end only. */
typedef struct RrHostApi {
  uint32_t struct_size; /* sizeof(RrHostApi) */
  uint32_t abi_version; /* RR_ABI_VERSION */
  void* user;           /* passed back to every callback */
  RrLogFn log;          /* may be null */
} RrHostApi;

uint32_t rr_abi_version(void);
/* "cod3_render <version>", static. */
const char* rr_version(void);
/* Null (with a log line when it can) if host is null, too small or for another ABI.
 * host->log and host->user must stay valid until rr_destroy. */
RrRenderer* rr_create(const RrHostApi* host);
/* The table for GpuCreateInfo::host_renderer (the SDK plugin's "host" backend); null for a null
 * renderer. It lives as long as the renderer, which must then not be destroyed while the plugin
 * may still call it. */
const struct RexHostRenderer* rr_host_renderer(RrRenderer* renderer);
/* Null does nothing. */
void rr_destroy(RrRenderer* renderer);

#ifdef __cplusplus
}
#endif
