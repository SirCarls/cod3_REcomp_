// [acct2-decomp] xenos_diff's other side: the SDK's own shader interpreter
// (src/graphics/pipeline/shader/interpreter.cpp, Xenia's, BSD 3-Clause), compiled
// unchanged next to this file by build.sh, behind a small C interface for the Rust
// checker. Nothing here changes how the interpreter computes anything: this file
// only fills its register file and guest memory and records what it exports.

#include <cstdint>
#include <cstring>
#include <new>

#include <rex/graphics/pipeline/shader/interpreter.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/xenos.h>
#include <rex/system/xmemory.h>

using namespace rex::graphics;

extern "C" {

// Must match src/interp.rs.
struct XiCase {
  uint32_t shader_type;  // 0 vertex, 1 pixel
  float consts[512][4];  // c0-c255 vertex, c256-c511 pixel
  uint32_t bools[8];
  uint32_t loops[32];
  uint32_t fetch[192];
  float regs[64][4];  // the temporary registers before the shader runs
};

struct XiExport {
  uint32_t reg;
  uint32_t mask;
  float v[4];
};

struct XiResult {
  uint32_t count;
  uint32_t overflow;
  XiExport ex[256];
  float regs[64][4];  // after
};

}  // extern "C"

namespace {

class Sink : public ShaderInterpreter::ExportSink {
 public:
  XiResult* out = nullptr;
  void Export(ucode::ExportRegister reg, const float* value, uint32_t mask) override {
    if (out->count >= 256) {
      out->overflow = 1;
      return;
    }
    XiExport& e = out->ex[out->count++];
    e.reg = uint32_t(reg);
    e.mask = mask;
    std::memcpy(e.v, value, sizeof(e.v));
  }
};

struct Ctx {
  rex::memory::Memory memory;
  RegisterFile regs;
  ShaderInterpreter interp{regs, memory};
  Sink sink;
};

}  // namespace

extern "C" {

void* xi_new() {
  Ctx* c = new (std::nothrow) Ctx();
  if (!c || !c->memory.Initialize()) {
    return nullptr;
  }
  return c;
}

uint8_t* xi_guest(void* ctx) { return static_cast<Ctx*>(ctx)->memory.physical_membase(); }

void xi_run(void* ctx, const uint32_t* ucode, const XiCase* in, XiResult* out) {
  Ctx* c = static_cast<Ctx*>(ctx);
  RegisterFile& rf = c->regs;
  std::memcpy(&rf.values[XE_GPU_REG_SHADER_CONSTANT_000_X], in->consts, sizeof(in->consts));
  std::memcpy(&rf.values[XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031], in->bools, sizeof(in->bools));
  std::memcpy(&rf.values[XE_GPU_REG_SHADER_CONSTANT_LOOP_00], in->loops, sizeof(in->loops));
  std::memcpy(&rf.values[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0], in->fetch, sizeof(in->fetch));
  // Vertex shader constants c0-c255, pixel shader constants c256-c511, 256 each.
  reg::SQ_VS_CONST vs{};
  vs.base = 0;
  vs.size = 255;
  reg::SQ_PS_CONST ps{};
  ps.base = 256;
  ps.size = 255;
  std::memcpy(&rf.values[XE_GPU_REG_SQ_VS_CONST], &vs, 4);
  std::memcpy(&rf.values[XE_GPU_REG_SQ_PS_CONST], &ps, 4);
  std::memcpy(c->interp.temp_registers(), in->regs, sizeof(in->regs));
  std::memset(out, 0, sizeof(*out));
  c->sink.out = out;
  c->interp.SetExportSink(&c->sink);
  c->interp.SetShader(in->shader_type ? xenos::ShaderType::kPixel : xenos::ShaderType::kVertex, ucode);
  c->interp.Execute();
  std::memcpy(out->regs, c->interp.temp_registers(), sizeof(out->regs));
}

}  // extern "C"
