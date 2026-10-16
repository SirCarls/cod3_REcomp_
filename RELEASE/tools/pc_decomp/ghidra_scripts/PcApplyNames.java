// Apply names.tsv (addr, name, kind, pass, evidence) made by tools/pc_decomp/name_pc.py.
//
// kind "function": the function at addr gets the name; when there is no function there yet
// (a named address the analysis missed), one is created first. kind "global": a primary label.
// Names are USER_DEFINED so a later analysis keeps them. Names an earlier run gave that are no
// longer in names.tsv are removed, so running it again gives the same result.
//
// Arguments: <names.tsv>
//@category PcDecomp
import java.io.*;
import java.nio.charset.StandardCharsets;
import java.util.*;

import ghidra.app.cmd.function.CreateFunctionCmd;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;

public class PcApplyNames extends GhidraScript {
    @Override
    protected void run() throws Exception {
        String path = getScriptArgs()[0];
        FunctionManager fm = currentProgram.getFunctionManager();
        SymbolTable st = currentProgram.getSymbolTable();
        int funcs = 0, created = 0, globals = 0, failed = 0, cleared = 0;
        Set<Address> listed = new HashSet<>();
        try (BufferedReader r = new BufferedReader(new InputStreamReader(new FileInputStream(path), StandardCharsets.UTF_8))) {
            String line = r.readLine();
            while ((line = r.readLine()) != null) {
                String[] p = line.split("\t", -1);
                if (p.length >= 3) {
                    listed.add(toAddr(Long.parseLong(p[0], 16)));
                }
            }
        }
        List<Symbol> stale = new ArrayList<>();
        for (Symbol s : st.getAllSymbols(false)) {
            if (s.getSource() == SourceType.USER_DEFINED && !s.isExternal() && !listed.contains(s.getAddress())) {
                stale.add(s);
            }
        }
        for (Symbol s : stale) {
            Function f = fm.getFunctionAt(s.getAddress());
            if (f != null && s.getSymbolType() == SymbolType.FUNCTION) {
                f.setName(null, SourceType.DEFAULT);
            } else {
                s.delete();
            }
            cleared++;
        }
        try (BufferedReader r = new BufferedReader(new InputStreamReader(new FileInputStream(path), StandardCharsets.UTF_8))) {
            String line = r.readLine();
            while ((line = r.readLine()) != null) {
                String[] p = line.split("\t", -1);
                if (p.length < 3) {
                    continue;
                }
                Address a = toAddr(Long.parseLong(p[0], 16));
                String name = p[1];
                try {
                    if (p[2].equals("function")) {
                        Function f = fm.getFunctionAt(a);
                        if (f == null) {
                            if (currentProgram.getListing().getInstructionAt(a) == null) {
                                disassemble(a);
                            }
                            if (new CreateFunctionCmd(a).applyTo(currentProgram, monitor)) {
                                created++;
                            }
                            f = fm.getFunctionAt(a);
                        }
                        if (f == null) {
                            failed++;
                            println("PcApplyNames: no function at " + p[0] + " for " + name);
                            continue;
                        }
                        f.setName(name, SourceType.USER_DEFINED);
                        funcs++;
                    } else {
                        Symbol s = st.createLabel(a, name, SourceType.USER_DEFINED);
                        s.setPrimary();
                        globals++;
                    }
                } catch (Exception e) {
                    failed++;
                    println("PcApplyNames: " + p[0] + " " + name + ": " + e.getMessage());
                }
            }
        }
        println("PcApplyNames: " + funcs + " functions (" + created + " created), " + globals + " globals, " + failed
            + " failed, " + cleared + " old names removed");
    }
}
