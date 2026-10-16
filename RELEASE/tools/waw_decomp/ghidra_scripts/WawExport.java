// Exports everything the World at War decompile needs from an analyzed Ghidra program:
// functions.tsv, calls.tsv, strings.tsv, globals.tsv and the decompiled C of every function
// (decomp/<range>.c, one file per 64 KB of code).
//
// Headless use: analyzeHeadless <proj> <name> -process <file> -noanalysis
//               -scriptPath tools/waw_decomp/ghidra_scripts -postScript WawExport.java <out_dir> [decompile=1]
//
// Output is derived from the game's code: it goes to dev/ (never published).
// @category WaW

import java.io.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.util.*;
import java.util.concurrent.*;

import ghidra.app.decompiler.*;
import ghidra.app.decompiler.parallel.*;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.data.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import ghidra.util.task.TaskMonitor;

public class WawExport extends GhidraScript {

    private static String esc(String s) {
        if (s == null) return "";
        StringBuilder b = new StringBuilder(s.length());
        for (char c : s.toCharArray()) {
            switch (c) {
                case '\t': b.append("\\t"); break;
                case '\n': b.append("\\n"); break;
                case '\r': b.append("\\r"); break;
                case '\\': b.append("\\\\"); break;
                default:
                    if (c < 0x20) b.append(String.format("\\x%02x", (int) c));
                    else b.append(c);
            }
        }
        return b.toString();
    }

    private static String hex(Address a) {
        return String.format("%08X", a.getOffset());
    }

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 1) {
            printerr("usage: WawExport.java <out_dir> [decompile=1]");
            return;
        }
        Path out = Paths.get(args[0]);
        boolean decompile = args.length < 2 || !args[1].equals("0");
        Files.createDirectories(out);
        Listing listing = currentProgram.getListing();
        FunctionManager fm = currentProgram.getFunctionManager();
        ReferenceManager rm = currentProgram.getReferenceManager();

        // functions.tsv
        List<Function> funcs = new ArrayList<>();
        try (PrintWriter w = new PrintWriter(Files.newBufferedWriter(out.resolve("functions.tsv"), StandardCharsets.UTF_8))) {
            w.println("entry\tname\tsize\tthunk\tsource\tsignature");
            for (Function f : fm.getFunctions(true)) {
                if (monitor.isCancelled()) return;
                if (f.isExternal()) continue;
                funcs.add(f);
                w.println(hex(f.getEntryPoint()) + "\t" + esc(f.getName()) + "\t" + f.getBody().getNumAddresses() + "\t"
                        + (f.isThunk() ? 1 : 0) + "\t" + f.getSymbol().getSource() + "\t"
                        + esc(f.getSignature().getPrototypeString()));
            }
        }
        println("functions: " + funcs.size());

        // calls.tsv (caller entry, call site, callee entry or import name)
        try (PrintWriter w = new PrintWriter(Files.newBufferedWriter(out.resolve("calls.tsv"), StandardCharsets.UTF_8))) {
            w.println("caller\tsite\tcallee\tkind");
            for (Function f : funcs) {
                if (monitor.isCancelled()) return;
                for (Address a : f.getBody().getAddresses(true)) {
                    for (Reference r : rm.getReferencesFrom(a)) {
                        RefType t = r.getReferenceType();
                        if (!t.isCall() && !(t.isJump() && t.isComputed())) continue;
                        Address to = r.getToAddress();
                        String callee;
                        if (to.isExternalAddress()) {
                            Symbol s = currentProgram.getSymbolTable().getPrimarySymbol(to);
                            callee = "ext:" + (s == null ? to.toString() : s.getName(true));
                        } else {
                            Function g = fm.getFunctionAt(to);
                            if (g == null) {
                                Data d = listing.getDataAt(to);
                                // call through an import pointer
                                if (d != null && d.isPointer()) {
                                    Object v = d.getValue();
                                    if (v instanceof Address && ((Address) v).isExternalAddress()) {
                                        Symbol s = currentProgram.getSymbolTable().getPrimarySymbol((Address) v);
                                        callee = "ext:" + (s == null ? v.toString() : s.getName(true));
                                        w.println(hex(f.getEntryPoint()) + "\t" + hex(a) + "\t" + esc(callee) + "\t" + t);
                                        continue;
                                    }
                                }
                                Symbol s = currentProgram.getSymbolTable().getPrimarySymbol(to);
                                callee = "ptr:" + hex(to) + (s != null ? ":" + s.getName() : "");
                            } else {
                                callee = hex(g.getEntryPoint());
                            }
                        }
                        w.println(hex(f.getEntryPoint()) + "\t" + hex(a) + "\t" + esc(callee) + "\t" + t);
                    }
                }
            }
        }

        // strings.tsv (string address, referencing function entries, text)
        try (PrintWriter w = new PrintWriter(Files.newBufferedWriter(out.resolve("strings.tsv"), StandardCharsets.UTF_8))) {
            w.println("addr\tfuncs\tsites\ttext");
            DataIterator it = listing.getDefinedData(true);
            while (it.hasNext()) {
                if (monitor.isCancelled()) return;
                Data d = it.next();
                if (!d.hasStringValue()) continue;
                Object v = d.getValue();
                String text = v == null ? "" : v.toString();
                StringBuilder fs = new StringBuilder(), sites = new StringBuilder();
                Set<String> seen = new TreeSet<>();
                for (Reference r : rm.getReferencesTo(d.getAddress())) {
                    Address from = r.getFromAddress();
                    Function f = fm.getFunctionContaining(from);
                    if (sites.length() > 0) sites.append(',');
                    sites.append(hex(from));
                    if (f != null && seen.add(hex(f.getEntryPoint()))) {
                        if (fs.length() > 0) fs.append(',');
                        fs.append(hex(f.getEntryPoint()));
                    }
                }
                w.println(hex(d.getAddress()) + "\t" + fs + "\t" + sites + "\t" + esc(text));
            }
        }

        // globals.tsv (labels in data sections and who touches them)
        try (PrintWriter w = new PrintWriter(Files.newBufferedWriter(out.resolve("globals.tsv"), StandardCharsets.UTF_8))) {
            w.println("addr\tname\tsource\tfuncs");
            SymbolIterator si = currentProgram.getSymbolTable().getAllSymbols(true);
            while (si.hasNext()) {
                if (monitor.isCancelled()) return;
                Symbol s = si.next();
                if (s.getSymbolType() != SymbolType.LABEL || s.isExternal()) continue;
                if (fm.getFunctionContaining(s.getAddress()) != null) continue;
                Set<String> fs = new TreeSet<>();
                for (Reference r : rm.getReferencesTo(s.getAddress())) {
                    Function f = fm.getFunctionContaining(r.getFromAddress());
                    if (f != null) fs.add(hex(f.getEntryPoint()));
                    if (fs.size() >= 64) break;
                }
                w.println(hex(s.getAddress()) + "\t" + esc(s.getName()) + "\t" + s.getSource() + "\t" + String.join(",", fs));
            }
        }

        if (!decompile) return;

        // Decompile every function, in parallel, grouped into 64 KB ranges.
        Path dir = out.resolve("decomp");
        Files.createDirectories(dir);
        final ConcurrentHashMap<Long, String> results = new ConcurrentHashMap<>();
        DecompileConfigurer cfg = d -> {
            DecompileOptions o = new DecompileOptions();
            o.grabFromProgram(currentProgram);
            d.setOptions(o);
            d.toggleCCode(true);
            d.toggleSyntaxTree(false);
            d.setSimplificationStyle("decompile");
        };
        DecompilerCallback<Void> dcb = new DecompilerCallback<Void>(currentProgram, cfg) {
            @Override
            public Void process(DecompileResults r, TaskMonitor m) throws Exception {
                Function f = r.getFunction();
                String body;
                if (r.decompileCompleted() && r.getDecompiledFunction() != null) {
                    body = r.getDecompiledFunction().getC();
                } else {
                    body = "/* decompile failed: " + esc(r.getErrorMessage()) + " */\n";
                }
                results.put(f.getEntryPoint().getOffset(),
                        "// " + hex(f.getEntryPoint()) + " " + f.getName() + " (" + f.getBody().getNumAddresses() + " bytes)\n" + body + "\n");
                return null;
            }
        };
        dcb.setTimeout(60);
        List<Function> todo = new ArrayList<>();
        for (Function f : funcs) if (!f.isThunk()) todo.add(f);
        println("decompiling " + todo.size() + " functions");
        ParallelDecompiler.decompileFunctions(dcb, todo, monitor);
        dcb.dispose();

        TreeMap<Long, StringBuilder> files = new TreeMap<>();
        for (Map.Entry<Long, String> e : new TreeMap<>(results).entrySet()) {
            long key = e.getKey() & ~0xFFFFL;
            files.computeIfAbsent(key, k -> new StringBuilder()).append(e.getValue());
        }
        for (Map.Entry<Long, StringBuilder> e : files.entrySet()) {
            Files.write(dir.resolve(String.format("%08X.c", e.getKey())), e.getValue().toString().getBytes(StandardCharsets.UTF_8));
        }
        println("decompiled: " + results.size() + " functions into " + files.size() + " files");
    }
}
