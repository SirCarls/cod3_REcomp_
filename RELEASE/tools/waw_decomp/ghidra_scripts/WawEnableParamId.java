// Turns on Ghidra's "Decompiler Parameter ID" analyzer (off by default for 32-bit Windows code).
// World at War PC was built with whole-program optimization: many functions take their
// arguments in registers (edi, esi, eax...), which Ghidra otherwise shows as (void) with
// "unaff_EDI" inside. Parameter ID gives those functions their real register parameters.
//
// Headless: -preScript WawEnableParamId.java   (then let analysis run)
// @category WaW

import java.util.Map;

import ghidra.app.script.GhidraScript;

public class WawEnableParamId extends GhidraScript {
    @Override
    public void run() throws Exception {
        Map<String, String> opts = getCurrentAnalysisOptionsAndValues(currentProgram);
        for (String k : opts.keySet()) {
            if (k.startsWith("Decompiler Parameter ID")) println("option: " + k + " = " + opts.get(k));
        }
        setAnalysisOption(currentProgram, "Decompiler Parameter ID", "true");
        println("Decompiler Parameter ID: on");
    }
}
