// Analysis options for the PC Call of Duty executables (run as -preScript at import).
//
// - Decompiler Parameter ID: these games were built with MSVC's whole-program
//   optimization, so many functions take arguments in registers (EAX, ECX, EDX).
//   This analyzer decompiles each function once and commits the parameters the
//   decompiler finds, so callers show real arguments instead of in_EAX.
// - Non-returning functions: Com_Error and friends never return; finding them early
//   keeps the code after their calls from being glued onto the next function.
//
// - Disc copy-protection sections (SafeDisc's "stxt*" loader sections, found in one of the
//   singleplayer executables) are removed from the program before analysis: their code is
//   not the game's, and the tools never read or run it.
//
// No arguments.
//@category PcDecomp
import ghidra.app.script.GhidraScript;
import ghidra.program.model.mem.MemoryBlock;

public class PcSetup extends GhidraScript {
    @Override
    protected void run() throws Exception {
        for (MemoryBlock b : currentProgram.getMemory().getBlocks()) {
            if (b.getName().startsWith("stxt")) {
                println("PcSetup: removing copy-protection section " + b.getName());
                currentProgram.getMemory().removeBlock(b, monitor);
            }
        }
        String[][] options = {
            {"Decompiler Parameter ID", "true"},
            {"Non-Returning Functions - Discovered", "true"},
            {"Function ID", "true"},
        };
        for (String[] o : options) {
            try {
                setAnalysisOption(currentProgram, o[0], o[1]);
                println("PcSetup: " + o[0] + " = " + o[1]);
            } catch (Exception e) {
                println("PcSetup: cannot set " + o[0] + ": " + e.getMessage());
            }
        }
    }
}
