# Call of Duty 3's Direct3D (Xbox 360)

This file maps the Direct3D library inside the campaign program (`default.xex`, the supported USA build). It covers:

- where the library is and how big it is;
- how the game drives it;
- where each piece of GPU state lives in the device object;
- which functions are which.

It was written for the Rust renderer, which takes over drawing at Direct3D's level (the claude-setup chat's `dev/agents/rust-renderer/`). Like [COD3_AUDIO.md](COD3_AUDIO.md), it was read from the game's own code: the disassembly, the Ghidra decompile, and the SDK's Xenos register and packet tables. Started 2026-10-16 (the owner asked whether decompiling Direct3D would help, then said "Yep").

Everything here is a fact about the program. Function names are candidates, with their evidence, in the private `dev/x360-family/d3d/d3d_names.tsv`. Nothing here edits `symbols.toml` or `names.toml`. The device layout is also a tool, `tools/x360_family/cod3_d3d_layout.py`: it prints the table, looks up a register, or decodes a dump of the device object.

## Where it is

Xbox 360 games link Direct3D into the program; there is no separate module on the disc. The disc's only code files are `default.xex`, `codmp_xenonf.xex` and the 15 level `.dll`s. `$SystemUpdate` holds the console's system update.

The program header's static-library list names the linked libraries. All are version **2.0.3424.0**: D3D9, D3DX9, XGRAPHC, XAUD, XMEDIA, XMP, XONLINE, XNET, XAPILIB, LIBCPMT, XBOXKRNL. The German, French/Spanish/Italian and Japanese builds and both multiplayer programs link the same D3D9 3424. The other Xbox 360 relatives link other SDK versions (by their XGRAPHC entry: CoD2 2099, CoD4 6132, WaW 7645, CoD Classic 7978, MW2 8276, Black Ops 11626). So no names can be carried over from them.

| Range (campaign) | What |
|---|---|
| 0x822BF000-~0x822D4000 | XAudio, then its vector mixing kernels up to 0x822E5800 (pointer tables at 0x8204C3A8) |
| **0x822E5810-0x823031A8** | **Direct3D 9 (121 KB; 537 functions by our boundary recovery, Ghidra lists 369)**. 149 of them are called by game code (not counting D3DX), from 1,639 call sites including tail branches. |
| 0x823031A8-~0x82343800 | D3DX9 texture file loading and saving (Radiance HDR, PFM, PNG, JPEG) with libpng 1.0.5, libjpeg 6b and zlib 1.1.4 (inflate and deflate); more D3DX and what looks like XGraphics code sits at 0x82360000-0x82373000 |

The library has almost no text. The exceptions are its PIX capture messages and the GPU-hang report at 0x822FD9B8 ("The GPU is hung! D3D version is %i.%i ..."), which prints 3424.0. So every name comes from behaviour:

- the Xenos registers and command packets a function writes;
- the `Vd*` graphics driver calls it makes;
- the device fields it touches;
- how the game calls it.

The kernel variable `VdGlobalDevice` (import at 0x82001144) holds the device for Direct3D's own code that gets no device argument (resource destruction, interrupts, PIX).

## How the game drives it

The game keeps its device pointer at **0x8290F24C**. The game's setup at 0x82130668 creates the device with `Direct3D_CreateDevice` (0x822EC4F0), which allocates a 0x5000-byte device object. The presentation parameters are at 0x8290EBC0, filled by 0x8212FFD8:

| Field | Value |
|---|---|
| BackBufferWidth/Height | copied from the video mode the game picked (0x8290F2BC; see *Video modes*) |
| BackBufferFormat | 0x18280186 (A8R8G8B8) |
| BackBufferCount | 1 |
| MultiSampleType | 0 (none) |
| SwapEffect | 1 (discard) |
| EnableAutoDepthStencil | 0 |
| AutoDepthStencilFormat | 0x1A220197 (D24FS8, floating-point depth; unused because EnableAutoDepthStencil is 0; the game's own depth targets use D24S8 0x2D200196, e.g. 0x8214DF44) |
| PresentationInterval | 1 (one vertical blank) |
| DisableAutoBackBuffer / DisableAutoFrontBuffer | 1 / 1 (the game makes its own) |
| FrontBufferFormat | 0x28280106 |

In the frame code:

- The game's frame code (0x8214F2A0 and 0x8214F770) uses predicated tiling, through the game's wrappers:
  - **BeginTiling** (wrapper 0x82153688, Direct3D 0x822F8FE0). The wrapper cuts the target into vertical strips, each as wide as the surface's tile size, and passes the strip rectangles with the clear values.
  - **BeginZPass / EndZPass** (wrappers 0x821537C8 / 0x82153800).
  - **EndTiling** (wrapper 0x82153750, Direct3D 0x822F94D8, which resolves).
- The end of the frame (0x82130570) does the following:
  - calls `SetShaderGPRAllocation(0, 0, 0)`;
  - **swaps** with `D3DDevice_Swap` (0x822F4C28, `VdSwap`);
  - sets the game's own split of shader registers (the value at 0x825A2154 for pixel shaders, 128 minus it for vertex shaders);
  - uploads a pending gamma ramp;
  - counts the frame.

**State is lazy.** Every Set* call writes the new value into the device object and sets a bit in one of five 64-bit "pending" masks at the start of the object. Nothing reaches the GPU until a draw:

- `DrawVertices` (0x822F3620);
- `DrawIndexedVertices` (0x822F3A28);
- the inline-vertex and tessellated draws.

Each draw first sends exactly the pending state (see the flush below), then writes `DRAW_INDX`. The **game itself inlines part of Direct3D**: at least 76 game functions write the device object directly, with no Direct3D call in between:

- shader constants (vertex constants in 31 functions, pixel constants in 9);
- texture fetch constants (22);
- the pending masks (76).

The 128 calls to `SetVertexShaderConstantF`'s out-of-line part (0x822E5F30) pass a mask the game computed itself. A few game functions also write command packets directly into Direct3D's command buffer:

- 0x82155C48, 0x821563A8, 0x821566A0, 0x82156810, 0x821569D0, in the same game module as the tiling calls;
- they reserve space through 0x82156B38 (Direct3D's 0x822F2BB8), pad it to 16 bytes with type-2 packets, and commit with 0x822E5F28.

The list is in the private `game_device_writes.tsv`.

So at the moment of a draw call the device object holds the complete state the GPU is about to get. But a capture that only watched Direct3D *calls* would miss state the game wrote itself.

**A recorded command buffer and a second device.** The game records part of its frame once and replays it:

- 0x8214FD48 (called from 0x8242C900; how often it runs is not established) creates a 4 MB command buffer (`CreateCommandBuffer` 0x822F7EA0) and a recording device (`Direct3D_CreateDevice` with device type 2 and no presentation parameters).
- It puts the recording device into 0x8290F24C, starts recording (0x822F7F48), runs the tiled frame function 0x8214F770 once, ends the recording (`EndCommandBuffer` 0x822F8B88) and puts the real device back.
- 0x8214E898 (reached through a pointer) later copies 0x100 bytes of constants into one of three buffers, chosen by frame number, and runs the recorded buffer with `RunCommandBuffer` (0x822F8590). The predicate is 1, 2 or 4, so each pass uses the matching buffer.
- While the recording device is installed, draws and the game's direct device writes go to it, so a capture keyed to `*0x8290F24C` sees them too.

## The device's state shadow

The draw-time flush (0x822F3620's prologue and its helpers) defines where each GPU register lives:

- 0x822FA700: vertex and pixel float constants, 4 per mask bit;
- 0x822FA5A0: the 32 fetch constants, followed by `SHADER_CONSTANT_FLUSH_FETCH`;
- 0x822FA360: register runs, one type-0 packet per run of set bits;
- 0x822FA420: user clip planes, after a coherency wait;
- 0x822FB4B0: both shaders, through `IM_LOAD` and the `SQ_*` registers;
- 0x822FA138: `RB_HIZCONTROL`.

I checked the table below against 133 register writes by Direct3D's own setters: each sets the pending bit the table gives for the register it changes. Pending bits use LSB numbering of the big-endian u64.

| Device offset | GPU registers | What | Pending |
|---|---|---|---|
| +0x0000 ... +0x0027 | | five pending masks (u64) | |
| +0x0028 / +0x002C / +0x0030 | | command write pointer / reserve end / kick threshold (0x822F29B8 runs past it) | |
| +0x0400 | 0x4800-0x48BF | 32 fetch constants (texture and vertex), 24 bytes each | +0x10 bit 43-k for constant k |
| +0x0700 | 0x4000-0x43FF | vertex shader float constants 0-255 | +0x00 bit 63-k for constants 4k..4k+3 |
| +0x1700 | 0x4400-0x47FF | pixel shader float constants 0-255 | +0x08, same |
| +0x2700 | 0x4900-0x4927 | bool (8 words) and loop (32) constants | +0x18 bit 1 |
| +0x27A0 | 0x2388-0x239F | user clip planes 0-5 (`PA_CL_UCP_*`) | +0x20 bit 54-k |
| +0x2800 | 0x2000-0x200F | `RB_SURFACE_INFO`, `RB_COLOR_INFO`, `RB_DEPTH_INFO`, `RB_COLOR1-3_INFO`, `COHER_DEST_BASE_0-7`, screen scissor | +0x18 bit 38-k |
| +0x284C | 0x2100-0x2114 | index limits and offset, `VGT_MULTI_PRIM_IB_RESET_INDX`, `RB_COLOR_MASK`, blend constant, fog colour, stencil ref/mask (both faces), `RB_ALPHA_REF`, viewport scale/offset | +0x18 bit 22-k |
| +0x28A0 | 0x2180-0x2184 | `SQ_PROGRAM_CNTL`, `SQ_CONTEXT_MISC`, `SQ_INTERPOLATOR_CNTL`, `SQ_WRAPPING_0/1` | +0x10 bit 48-k |
| +0x28B4 | 0x2200-0x220B | `RB_DEPTHCONTROL`, `RB_BLENDCONTROL0`, `RB_COLORCONTROL`, `RB_HIZCONTROL`, `PA_CL_CLIP_CNTL`, `PA_SU_SC_MODE_CNTL`, `PA_CL_VTE_CNTL`, `RB_MODECONTROL`, `RB_BLENDCONTROL1-3` | +0x10 bit 11-k |
| +0x28E4 | 0x2280-0x2294 | point and line state, `VGT_OUTPUT_PATH_CNTL`, tessellation (`VGT_HOS_*`), vertex grouping (`VGT_GROUP_*`), `PA_SC_VIZ_QUERY` | +0x18 bit 59-k |
| +0x2938 | 0x2300-0x2325 | `PA_SC_AA_CONFIG`, guard band, `SQ_VS/PS_CONST`, `PA_SC_AA_MASK`, the resolve registers (`RB_COPY_*`, clear values), sample counting | +0x20 bit 37-k |
| +0x29D0 | 0x2380-0x2387 | polygon offset (depth bias), point size | +0x20 bit 45-k |
| | | shaders: microcode and `SQ_*` setup | +0x10 bits 49-52 |

Device fields outside the GPU shadow (API-level state):

| Offset | What |
|---|---|
| +0x0034 | reference count |
| +0x0038 / +0x01BC / +0x020C / +0x0390 | render-state and sampler-state set / get function tables |
| +0x2A08 | owner thread (`AcquireThreadOwnership`) |
| +0x2A10 | pointer to the GPU write-back block: word 0 last completed fence, word 1 last completed segment, word 0xF ring read pointer |
| +0x2A1C / +0x2A2C | next fence value / last fence `InsertFence` returned |
| +0x2D10 | vertex declaration |
| +0x2D38 / +0x2D3C | blend state as set / `AlphaBlendEnable` (bit 31), `SeparateAlphaBlendEnable` (bit 30) |
| +0x2D40 / +0x2D44 / +0x2D54 / +0x2D58 | scissor test, colour write mask, ZEnable, StencilEnable as set |
| +0x2F84 / +0x2F88-0x2F94 / +0x2F98 / +0x2F9C | index buffer / render targets 0-3 / depth-stencil surface / stream sources |
| +0x3058 / +0x3070 | viewport (`D3DVIEWPORT9`) / scissor rectangle |
| +0x3080 / +0x3084 | pixel shader / vertex shader |
| +0x3370... | display mode (width, height, refresh rate; format at +0x33A0) |
| +0x3354 | command buffer being recorded |
| +0x3510 | gamma ramp (1,536 bytes) |
| +0x3B10 | vertical blank callback |
| +0x4FC0 | creation flags (bits 24-29 choose the hardware threads that get a Direct3D worker) |

Some states keep a copy of the value the game set, separate from the register. The register is forced off when no surface is bound:

- `ZEnable` and `StencilEnable` are forced off without a depth surface;
- `ColorWriteEnable` is forced off without render target 0;
- `AlphaBlendEnable` is applied by writing one/zero blend factors into all four `RB_BLENDCONTROL`s.

## Render states and sampler states

Each D3D render state is its own small function, and the game calls them directly. Direct3D also keeps them in two tables of {get, set, default} entries: render states at 0x825C4008 (97 entries; the state number is 4 × the index, so `D3DRS_ZENABLE` is 40, `D3DRS_ZFUNC` 44 and `D3DRS_ZWRITEENABLE` 48) and sampler states at 0x825C4498 (20). `D3D_SetDefaultState` (0x822FDC58) copies the setters into the device at +0x38 / +0x1BC and the getters at +0x20C / +0x390. Most change one register field; the blend states rewrite all four `RB_BLENDCONTROL`s, ViewportEnable two registers and the depth-bias states three. Several also set shader pending bits: AlphaTestEnable sets +0x10 bit 50, and ZEnable, ZFunc, StencilEnable, StencilFail and StencilZFail set bit 49.

| Function | State | Register field |
|---|---|---|
| 0x822E6CD8 / 0x822E6CF8 | CullMode (set/get) | `PA_SU_SC_MODE_CNTL` 0-2 |
| 0x822E6D08 | FillMode | `PA_SU_SC_MODE_CNTL` 3-10 |
| 0x822E6D38 / 0x822E7208 / 0x822E71A0 | AlphaTestEnable / AlphaFunc / AlphaRef | `RB_COLORCONTROL` 3, 0-2; `RB_ALPHA_REF` |
| 0x822E6D78, 0x822E6E98, 0x822E6F28, 0x822E6E08 | AlphaBlendEnable, SrcBlend, DestBlend, BlendOp | `RB_BLENDCONTROL0-3` |
| 0x822E7238 | BlendFactor | `RB_BLEND_RED/GREEN/BLUE/ALPHA` |
| 0x822E7350 / 0x822E7398 / 0x822E73C8 | ZEnable / ZWriteEnable / ZFunc | `RB_DEPTHCONTROL` 1, 2, 4-6 |
| 0x822E7408, 0x822E7490, 0x822E74C0, 0x822E7540, 0x822E7500 | StencilEnable, Func, Fail, Pass, ZFail | `RB_DEPTHCONTROL` 0, 8-19 |
| 0x822E7650 / 0x822E7670 / 0x822E7690 | StencilRef / Mask / WriteMask | `RB_STENCILREFMASK` bytes |
| 0x822E7830 / 0x822E7768 | DepthBias / SlopeScaleDepthBias | `PA_SU_POLY_OFFSET_*_OFFSET` / `_SCALE` (× 16) + enables |
| 0x822E78D8 | MultiSampleAntiAlias | `PA_SU_SC_MODE_CNTL` 15 |
| 0x822E7928 | ColorWriteEnable | `RB_COLOR_MASK` 0-3 |
| 0x822E82E8 | AlphaToMaskEnable | `RB_COLORCONTROL` 4 |
| 0x822E7EA8 | ViewportEnable (Xbox 360) | `PA_CL_VTE_CNTL`, clip disable |
| 0x822E8298 / 0x822E82C8 | primitive reset enable / index (Xbox 360) | `PA_SU_SC_MODE_CNTL` 21; `VGT_MULTI_PRIM_IB_RESET_INDX` |
| 0x822E81F0 / 0x822E8228 | tessellation level / mode (Xbox 360) | `VGT_HOS_*` |
| 0x822E8440, 0x822E8470, 0x822E84A0, 0x822E84D0 | the HiZ / HiStencil states (Xbox 360; which is which is not pinned down) | `RB_HIZCONTROL` bits 3, 2, 5, 8-15 |
| 0x822E8520 / 0x822E86A8 / 0x822E8920 | sampler MinFilter / MagFilter / MaxAnisotropy | fetch constant dwords 3-4 (`dev+0x400+24*sampler`) |

The rest of the render states the game never calls, such as the alpha-separate blend states and WRAP0-15, are in the names list.

## Other entry points

| Address | Function | Notes |
|---|---|---|
| 0x822EC4F0 | `Direct3D_CreateDevice` | 0x5000-byte device |
| 0x822F2CF0 | ring buffer (re)initialisation (`SetRingBufferParameters`?) | `VdInitializeRingBuffer`, `ME_INIT`; the game calls it once |
| 0x822E8FC8 / 0x822E91F0 | `SetViewport` / `GetViewport` | |
| 0x822E8F00 / 0x822E97E8 | `SetScissorRect` / scissor enable | window offset and scissor go out as `SET_CONSTANT` (0x822E69F8), not through the shadow |
| 0x822E9E08 (→0x822E97F8) / 0x822E9B28 | `SetRenderTarget` / `SetDepthStencilSurface` | |
| 0x822E9448 / 0x822E9490 | `GetRenderTarget` / `GetDepthStencilSurface` | |
| 0x822E95F8 | `SetBlendState` (Xbox 360, per target) | |
| 0x822EBE40 | `SetTexture` | writes the whole fetch constant |
| 0x822E9208 / 0x822E93B8 | `SetStreamSource` / `SetIndices` | the unbound buffer gets the current fence |
| 0x822E61A8 / 0x822E6468 / 0x822E6630 | `SetPixelShader` / `SetVertexShader` / `SetVertexDeclaration` | |
| 0x822E5F30 / 0x822E6040 / 0x822E6150 | vertex / pixel float constants, bool constants | out-of-line parts |
| 0x822F3620 / 0x822F3A28 | `DrawVertices` / `DrawIndexedVertices` | |
| 0x822F35C0 (→0x822F30F0) | inline-vertex draw (`DrawPrimitiveUP`) | |
| 0x822F3EA0 / 0x822F4338 | tessellated or grouped draws (one game call each) | `VGT_GROUP_*`, `VGT_HOS_CNTL` |
| 0x822EFFF0 | `Resolve` | `RB_COPY_*`; clears; an internal draw with its own shaders |
| 0x822F8FE0 / 0x822F94D8 | `BeginTiling` / `EndTiling` | |
| 0x822F9980 / 0x822F9C70 | `BeginZPass` / `EndZPass` (from the order of the game's wrappers) | |
| 0x822F7EA0 / 0x822F7F48 / 0x822F8B88 / 0x822F8590 | `CreateCommandBuffer` / begin recording / `EndCommandBuffer` / `RunCommandBuffer` | run = `SET_BIN_SELECT_HI` predicate, then a predicated `INDIRECT_BUFFER` per recorded segment |
| 0x822F79E0 | `Clear` | rectangle loop, internal clear draw (0x822F6DF8) |
| 0x822F51E8 / 0x822F4C28 | `Present` / `Swap` | the game swaps directly |
| 0x822E6730 | `SetShaderGPRAllocation` | `SQ_GPR_MANAGEMENT` |
| 0x822F2AA0 / 0x822F2C98 | `InsertFence` / `InsertCallback` (`INTERRUPT` packet) | |
| 0x822ED158 / 0x822ED390 | `CreateQuery` (the game makes an occlusion query, type 9) / issue | |
| 0x822E94D0 / 0x822E9558 | `SetGammaRamp` / `GetGammaRamp` | |
| 0x822E5810 | `PersistDisplay` | `VdPersistDisplay`, before loading the multiplayer program or rebooting |
| 0x822F1410 / 0x822F1548 | `CreateVertexBuffer` / `CreateIndexBuffer` | |
| 0x822F14D8 / 0x822F1538 | vertex buffer `Lock` / `Unlock` | |
| 0x822F15F8 / 0x822F1658 | index buffer `Lock` / `Unlock` | |
| 0x822EBAD8 | `CreateTexture` (all texture types; 0x34-byte object) | |
| 0x822EB880 / 0x822EB960 | `GetSurfaceLevel` / `GetCubeMapSurface` | |
| 0x822E6650 / 0x822E67E0 / 0x822E68D8 | `CreateVertexDeclaration` / `CreatePixelShader` / `CreateVertexShader` | |
| 0x822F12C8 / 0x822F1340 | resource `AddRef` / `Release` | |

Resources start with a 0x18-byte header:

- +0 `Common`: type in bits 0-3 (1 = vertex buffer), lock count in bits 8-11;
- +4 reference count;
- +8 fence (set when the GPU may still be reading it);
- +0x14 pending CPU cache-flush range (start line in the high half, end line in the low half; 0xFFFF0000 = empty), flushed on unlock;
- then the object's GPU fetch constant (vertex buffers: address|3 and size at +0x18; textures: six words from +0x1C).

The game allocates through its own `XMemAlloc` (0x824BF8A0) and `XMemFree` (0x824BF870).

## Command submission, fences, interrupts and tiling

These come from the second naming pass over the internal functions. I spot-checked two of its claims myself: the ring write-pointer store and the render-state table. Details are in the private `d3d_helper_NOTES.md`.

- **Two levels of commands.**
  - Direct3D writes commands into segments of a large command memory (0x822F2678 begins a segment, 0x822F2818 ends one).
  - Each segment ends with an `INVALIDATE_STATE` and two `EVENT_WRITE_SHD` packets. These make the GPU write its progress and the segment's fence into the write-back block (dev+0x2A10).
  - The primary ring only receives `INDIRECT_BUFFER` packets pointing at segments (0x822F1E68). After each submit, Direct3D stores the ring's write pointer **directly through MMIO** (`stw` to 0x7FC80714 at 0x822F1FF0).
- **Fences and waits.**
  - The next fence is dev+0x2A1C (+2 per segment).
  - Binding a resource puts it in a tracking list, and the fence is stamped into resource+8 later (0x822F7B50).
  - Locks wait on that fence and then have the GPU invalidate its caches for the range; unlocks flush the CPU cache.
  - Every CPU wait spins on the write-back block (fence, progress, ring read pointer). After 5 seconds without progress it reports a GPU hang (0x822FDB70).
- **Interrupts.** The graphics interrupt callback 0x822F1918 handles two sources:
  - the command processor's `INTERRUPT` packet: it runs the callback the GPU left in a shared block, then clears this CPU's bit. The GPU waits on that bit with `WAIT_REG_MEM`, which is how `InsertCallback` and the present-interval sync work;
  - vblank: it reads the display status by MMIO, keeps the vblank count and the flip countdown, and calls the vertical blank callback.
- **Tiling runs on Direct3D worker threads.**
  - `BeginTiling` opens a record: segments are collected instead of submitted, and every draw makes the GPU write its screen extent into memory.
  - `EndTiling` submits the record and ends it with a callback that wakes a worker thread (created by 0x82303000).
  - The worker reads the extents back and works out which strips each draw touches (0x82302378). It then writes `SET_BIN_SELECT` and the recorded segments as `INDIRECT_BUFFER`s straight into the ring, once per strip (0x823027F0).
- **Shaders.**
  - A vertex shader's microcode is patched for the current vertex declaration when shaders are flushed (0x822FA870, 0x822FB368). It is patched in place with a per-shader cache, or copied inline with `IM_LOAD_IMMEDIATE` when the cached copy is still in use (0x822FB150).
  - Shader literal constants are loaded by the GPU from the shader with `LOAD_ALU_CONSTANT` (0x822FB2B0).
- **Gamma and display.** The gamma lookup table, the display gamma mode and the display scaler are all programmed with packets in the ring. The ramp is converted through the sRGB or BT.709 curve that `VdGetCurrentDisplayGamma` reports.
- **Debug-only code, dormant in a retail run:** PIX capture, performance counters, movie capture and the hang reporter. With a hang callback installed, a hang switches the device to a bypass mode in which the CPU fakes the fences.

## Video modes

The game picks the back-buffer size from a table at 0x825A2168. Each 16-byte entry holds width, height, a progressive flag (byte 8), a PAL flag (byte 9) and, in word 3, an optional alias: when it is non-zero, the entry is chosen for a display of *that* entry's size.

| Entry | Back buffer | Flags | Chosen for a display of |
|---|---|---|---|
| 1 | 640 × 480 | interlaced | its own size |
| 2 | 640 × 576 | interlaced, PAL | its own size |
| 3 | 880 × 736 | progressive | entry 4's size (640 × 480) |
| 4 | 640 × 480 | progressive | its own size |
| 5 | 1040 × 624 | progressive | entry 6's size (1280 × 720) |
| 6 | 1280 × 720 | progressive | its own size |
| 7 | 1920 × 1080 | progressive | its own size |

How the game picks an entry:

1. At start, `Sys_Main` (0x82400098, at 0x824001B4-0x824001F8) reads the setting `capture_movie` (`Cvar_Get`).
   - If it is set, the list is {6} (1280 × 720).
   - Otherwise it calls `XGetVideoMode` and uses {5} (1040 × 624) on a widescreen display or {3} (880 × 736) on a 4:3 one.
2. 0x8212FDB8 copies the list's first entry to 0x8290F2BC and its number to 0x8290F29C. It then takes the first listed entry whose display size, interlacing and PAL flag match the console's mode (`XGetVideoMode`), copies it to 0x8290F2BC and records the console's widescreen flag in the copy (+0xA).
3. The device setup 0x82130668 (and 0x8212FF88) would try {6, 4, 1}, but only when 0x8290F29C is still 0. Step 2 has already set it.
4. The presentation parameters take the width and height from 0x8290F2BC.

So, unless `capture_movie` is set, the back buffer is **1040 × 624** on a widescreen display and 880 × 736 on a 4:3 one. The game draws below 720p, and 1280 × 720 is used only for movie capture.

Device-level multisampling is off. The game makes its own render targets (`DisableAutoBackBuffer`), turns multisampling on itself (`R_SetMultisample` sets the `MultiSampleAntiAlias` state), and predicated tiling splits its targets into strips as wide as the EDRAM surface.

## What this means for a renderer at Direct3D's level

- **Draw calls are the natural capture point.** At `DrawVertices` / `DrawIndexedVertices` (and the inline and tessellated draws), the device object's shadow plus the pending masks is the full GPU state. That includes constants and fetch constants the game wrote without calling Direct3D. `cod3_d3d_layout.py --decode` reads it.
- **Not everything goes through the shadow.** These write packets directly:
  - window offset and scissor (`SET_CONSTANT` in 0x822E69F8);
  - `Resolve` (its own registers and shaders, `IM_LOAD_IMMEDIATE` + `DRAW_INDX_2`);
  - the tiling functions (`SET_BIN_MASK_LO/HI`, `SET_BIN_SELECT_LO/HI`, predicated `INDIRECT_BUFFER` replays; while tiling is on, every draw is also wrapped in `SET_BIN_MASK_LO`, `EVENT_WRITE_EXT` and `EVENT_WRITE` to capture its screen extent);
  - queries (`VIZ_QUERY`, `EVENT_WRITE_ZPD`);
  - fences and callbacks (`EVENT_WRITE_SHD`, `INTERRUPT`, `WAIT_REG_MEM`);
  - `SetShaderGPRAllocation`;
  - the game's own packets in the five functions named above.
- **Tiling and recorded buffers replay the same commands several times.**
  - Direct3D's worker thread writes each strip's `SET_BIN_SELECT` and the recorded segments into the ring.
  - `RunCommandBuffer` replays the game's recorded buffer with a predicate.
  - A renderer with no EDRAM can draw the frame once and treat the strips as one target. But it has to honour or recognise these predicates, or it will draw everything once per strip.
- **Direct3D waits on the GPU** in buffer and texture locks, resource destruction, fences, command-memory reuse, ring space, queries and `Swap`. All of these read the write-back block, so a stub GPU must keep words 0, 1 and 0xF moving (or use Direct3D's own bypass mode). Otherwise the waits spin, and after 5 seconds the hang reporter fires.
- **Direct3D also touches the GPU outside the command stream:**
  - the ring write pointer (MMIO 0x7FC80714) at every submit;
  - display status and the vertical counter in the interrupt and flip callbacks;
  - two registers in the engine-init callback (0x822FDC10).
  The interrupt callback has to be delivered for both `INTERRUPT` packets and vblank.
- **Vertex shader microcode changes in memory at run time** (declaration patching), so translated shaders have to be cached by contents (or by shader plus declaration), not by address alone.

## Function names

Candidates with evidence are in `dev/x360-family/d3d/d3d_names.tsv`, covering all 537 functions: 277 high, 167 medium and 93 low confidence.

- This chat named every function the game calls, plus the flush helpers: 159 functions (68 high, 54 medium, 37 low).
- A second pass named the 378 internal ones.
- An independent check of this write-up against the code confirmed its main claims and corrected the video modes, the auto depth format and the library versions.

`dev/x360-family/d3d/d3d_mp_twins.tsv` matches 500 of the functions in the multiplayer program (0x822B45D8-0x822D1BF0, by masked instructions). The match agrees with the existing `D3DDevice_SetGammaRamp_mp` (MP 0x822B8218 = campaign 0x822E94D0). There, the device pointer is 0x82A8C0FC and the presentation parameters are at 0x82A8BA70.

Not done yet:

- which HiZ state is which;
- the exact names of the tiling, survey and conditional-rendering helpers marked "?" in the list;
- how often the game records its command buffer.
