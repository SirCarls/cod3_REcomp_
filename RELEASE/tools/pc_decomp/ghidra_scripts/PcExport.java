// Export one program's tables, or the C of every function.
//
//   -postScript PcExport.java tables <out dir>
//       functions.tsv  entry, name, size, source, thunk, section, ranges, convention, signature
//       calls.tsv      caller, site, callee, kind
//                      callee: hex address, "ext:<dll>::<name>" for an import, "ptr:<hex>" for a
//                      call through a pointer stored at that address
//                      kind: UNCONDITIONAL_CALL, COMPUTED_CALL, or TAILJUMP (a jump to another
//                      function's entry)
//       strings.tsv    addr, text, funcs (the functions that use it, comma separated)
//       globals.tsv    addr, name, section, type, nrefs, funcs (non-string data that code uses)
//   -postScript PcExport.java decomp <out dir> [names.tsv] [skip.tsv] [max bytes per file]
//       <first entry>.c files in address order, each up to max bytes (default 2.5 MB).
//       Every function starts with a comment: entry, size, name, and the evidence for the name
//       from names.tsv (addr, name, kind, pass, evidence). Functions listed in skip.tsv
//       (first column: hex entry) are listed by name only, without their C.
//
// Text in the tables is escaped: backslash, tab, newline, carriage return, and other control
// bytes as \xHH.
//@category PcDecomp
import java.io.*;
import java.nio.charset.StandardCharsets;
import java.util.*;

import ghidra.app.decompiler.*;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.data.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.mem.*;
import ghidra.program.model.scalar.Scalar;
import ghidra.program.model.symbol.*;

public class PcExport extends GhidraScript {
    private Listing listing;
    private Memory mem;
    private FunctionManager fm;

    static String esc(String s) {
        StringBuilder b = new StringBuilder();
        for (char c : s.toCharArray()) {
            switch (c) {
                case '\\': b.append("\\\\"); break;
                case '\t': b.append("\\t"); break;
                case '\n': b.append("\\n"); break;
                case '\r': b.append("\\r"); break;
                default:
                    if (c < 0x20 || c == 0x7f) {
                        b.append(String.format("\\x%02x", (int) c));
                    } else {
                        b.append(c);
                    }
            }
        }
        return b.toString();
    }

    static String hex(Address a) {
        return String.format("%08X", a.getOffset());
    }

    private MemoryBlock blockOf(Address a) {
        return mem.getBlock(a);
    }

    private boolean isDataAddr(Address a) {
        MemoryBlock b = blockOf(a);
        return b != null && !b.isExecute() && !b.isExternalBlock() && !b.getName().equals("Headers");
    }

    /** The C string at a: a defined string's value, else bytes up to a NUL that read as text. */
    private String stringAt(Address a) {
        Data d = listing.getDataAt(a);
        if (d != null && d.hasStringValue()) {
            Object v = d.getValue();
            return v == null ? null : v.toString();
        }
        if (d != null && d.isDefined() && !(d.getDataType() instanceof Undefined)) {
            return null;
        }
        MemoryBlock b = blockOf(a);
        if (b == null || !b.isInitialized()) {
            return null;
        }
        byte[] buf = new byte[1024];
        int n;
        try {
            n = mem.getBytes(a, buf);
        } catch (Exception e) {
            return null;
        }
        int len = 0;
        int letters = 0;
        while (len < n && buf[len] != 0) {
            int c = buf[len] & 0xff;
            boolean ok = (c >= 0x20 && c != 0x7f) || c == '\t' || c == '\n' || c == '\r' || c == 0x15 || c == 0x14;
            if (!ok) {
                return null;
            }
            if (Character.isLetterOrDigit(c) && c < 0x80) {
                letters++;
            }
            len++;
        }
        if (len == n || len < 2 || letters == 0) {
            return null;
        }
        return new String(buf, 0, len, StandardCharsets.ISO_8859_1);
    }

    private void tables(File out) throws Exception {
        out.mkdirs();
        Map<Address, String> strings = new TreeMap<>();
        Map<Address, TreeSet<Address>> strFuncs = new HashMap<>();
        Map<Address, TreeSet<Address>> globFuncs = new HashMap<>();
        Map<Address, Integer> globRefs = new HashMap<>();
        try (PrintWriter fw = new PrintWriter(new OutputStreamWriter(new FileOutputStream(new File(out, "functions.tsv")), StandardCharsets.UTF_8));
             PrintWriter cw = new PrintWriter(new OutputStreamWriter(new FileOutputStream(new File(out, "calls.tsv")), StandardCharsets.UTF_8))) {
            fw.println("entry\tname\tsize\tsource\tthunk\tsection\tranges\tconvention\tsignature");
            cw.println("caller\tsite\tcallee\tkind");
            for (Function f : fm.getFunctions(true)) {
                monitor.checkCancelled();
                if (f.isExternal()) {
                    continue;
                }
                AddressSetView body = f.getBody();
                StringBuilder ranges = new StringBuilder();
                for (AddressRange r : body) {
                    if (ranges.length() > 0) {
                        ranges.append(',');
                    }
                    ranges.append(hex(r.getMinAddress())).append('-').append(hex(r.getMaxAddress()));
                }
                String thunk = "";
                if (f.isThunk()) {
                    Function t = f.getThunkedFunction(false);
                    if (t != null) {
                        thunk = t.isExternal() ? "ext:" + extName(t) : hex(t.getEntryPoint());
                    }
                }
                MemoryBlock blk = blockOf(f.getEntryPoint());
                fw.println(hex(f.getEntryPoint()) + "\t" + f.getName() + "\t" + body.getNumAddresses() + "\t"
                    + f.getSymbol().getSource() + "\t" + thunk + "\t" + (blk == null ? "" : blk.getName()) + "\t"
                    + ranges + "\t" + f.getCallingConventionName() + "\t" + esc(f.getPrototypeString(false, false)));
                InstructionIterator it = listing.getInstructions(body, true);
                while (it.hasNext()) {
                    Instruction ins = it.next();
                    FlowType ft = ins.getFlowType();
                    boolean isCall = ft.isCall();
                    for (Reference r : ins.getReferencesFrom()) {
                        Address to = r.getToAddress();
                        RefType rt = r.getReferenceType();
                        if (isCall && rt.isCall()) {
                            String callee;
                            if (to.isExternalAddress()) {
                                Symbol s = currentProgram.getSymbolTable().getPrimarySymbol(to);
                                callee = "ext:" + (s == null ? to.toString() : extSymName(s));
                            } else {
                                callee = hex(to);
                            }
                            cw.println(hex(f.getEntryPoint()) + "\t" + hex(ins.getAddress()) + "\t" + callee + "\t" + rt.getName());
                        } else if (isCall && (rt.isRead() || rt.isData()) && !to.isExternalAddress() && ft.isComputed()) {
                            // call [pointer]: an import slot or a function pointer in data
                            Reference ext = null;
                            for (Reference r2 : currentProgram.getReferenceManager().getReferencesFrom(to)) {
                                if (r2.getToAddress().isExternalAddress()) {
                                    ext = r2;
                                }
                            }
                            String callee;
                            if (ext != null) {
                                Symbol s = currentProgram.getSymbolTable().getPrimarySymbol(ext.getToAddress());
                                callee = "ext:" + (s == null ? ext.getToAddress().toString() : extSymName(s));
                            } else {
                                callee = "ptr:" + hex(to);
                            }
                            cw.println(hex(f.getEntryPoint()) + "\t" + hex(ins.getAddress()) + "\t" + callee + "\tCOMPUTED_CALL");
                        } else if (!isCall && ft.isJump() && rt.isFlow() && !to.isExternalAddress()) {
                            Function g = fm.getFunctionAt(to);
                            if (g != null && g != f) {
                                cw.println(hex(f.getEntryPoint()) + "\t" + hex(ins.getAddress()) + "\t" + hex(to) + "\tTAILJUMP");
                            }
                        } else if (!rt.isFlow() && !to.isExternalAddress() && isDataAddr(to)) {
                            noteData(f, to, strings, strFuncs, globFuncs, globRefs);
                        }
                    }
                    // scalar operands that are data addresses Ghidra made no reference for
                    for (int op = 0; op < ins.getNumOperands(); op++) {
                        for (Object o : ins.getOpObjects(op)) {
                            if (o instanceof Scalar) {
                                long v = ((Scalar) o).getUnsignedValue();
                                if (v < 0x10000 || v > 0xffffffffL) {
                                    continue;
                                }
                                Address a;
                                try {
                                    a = toAddr(v);
                                } catch (Exception e) {
                                    continue;
                                }
                                if (isDataAddr(a) && ins.getOperandReferences(op).length == 0) {
                                    noteData(f, a, strings, strFuncs, globFuncs, globRefs);
                                }
                            }
                        }
                    }
                }
            }
        }
        // every defined string, used or not (tables of names point at them)
        DataIterator di = listing.getDefinedData(true);
        while (di.hasNext()) {
            Data d = di.next();
            if (d.hasStringValue() && isDataAddr(d.getAddress()) && !strings.containsKey(d.getAddress())) {
                Object v = d.getValue();
                if (v != null) {
                    strings.put(d.getAddress(), v.toString());
                }
            }
        }
        try (PrintWriter sw = new PrintWriter(new OutputStreamWriter(new FileOutputStream(new File(out, "strings.tsv")), StandardCharsets.UTF_8))) {
            sw.println("addr\ttext\tfuncs");
            for (Map.Entry<Address, String> e : strings.entrySet()) {
                StringBuilder fs = new StringBuilder();
                for (Address f : strFuncs.getOrDefault(e.getKey(), new TreeSet<>())) {
                    if (fs.length() > 0) {
                        fs.append(',');
                    }
                    fs.append(hex(f));
                }
                sw.println(hex(e.getKey()) + "\t" + esc(e.getValue()) + "\t" + fs);
            }
        }
        try (PrintWriter gw = new PrintWriter(new OutputStreamWriter(new FileOutputStream(new File(out, "globals.tsv")), StandardCharsets.UTF_8))) {
            gw.println("addr\tname\tsection\ttype\tnrefs\tfuncs");
            for (Address a : new TreeSet<>(globRefs.keySet())) {
                Symbol s = currentProgram.getSymbolTable().getPrimarySymbol(a);
                Data d = listing.getDataAt(a);
                String type = d != null && d.isDefined() ? d.getDataType().getName() : "";
                StringBuilder fs = new StringBuilder();
                int k = 0;
                for (Address f : globFuncs.get(a)) {
                    if (k++ == 50) {
                        fs.append(",...");
                        break;
                    }
                    if (fs.length() > 0) {
                        fs.append(',');
                    }
                    fs.append(hex(f));
                }
                gw.println(hex(a) + "\t" + (s == null ? "" : s.getName()) + "\t" + blockOf(a).getName() + "\t" + esc(type)
                    + "\t" + globRefs.get(a) + "\t" + fs);
            }
        }
        println("PcExport: tables: " + fm.getFunctionCount() + " functions, " + strings.size() + " strings, "
            + globRefs.size() + " globals");
    }

    private void noteData(Function f, Address to, Map<Address, String> strings, Map<Address, TreeSet<Address>> strFuncs,
                          Map<Address, TreeSet<Address>> globFuncs, Map<Address, Integer> globRefs) {
        String s = strings.containsKey(to) ? strings.get(to) : stringAt(to);
        if (s != null) {
            strings.put(to, s);
            strFuncs.computeIfAbsent(to, k -> new TreeSet<>()).add(f.getEntryPoint());
        } else {
            globFuncs.computeIfAbsent(to, k -> new TreeSet<>()).add(f.getEntryPoint());
            globRefs.merge(to, 1, Integer::sum);
        }
    }

    private String extName(Function t) {
        return extSymName(t.getSymbol());
    }

    private String extSymName(Symbol s) {
        String lib = s.getParentNamespace() == null ? "" : s.getParentNamespace().getName();
        return lib + "::" + s.getName();
    }

    private void decomp(File out, String namesPath, String skipPath, long maxBytes) throws Exception {
        out.mkdirs();
        Map<Long, String[]> names = new HashMap<>();
        if (namesPath != null && !namesPath.equals("-")) {
            try (BufferedReader r = new BufferedReader(new InputStreamReader(new FileInputStream(namesPath), StandardCharsets.UTF_8))) {
                String line = r.readLine();
                while ((line = r.readLine()) != null) {
                    String[] p = line.split("\t", -1);
                    if (p.length >= 5) {
                        names.put(Long.parseLong(p[0], 16), p);
                    }
                }
            }
        }
        Set<Long> skip = new HashSet<>();
        if (skipPath != null && !skipPath.equals("-")) {
            try (BufferedReader r = new BufferedReader(new InputStreamReader(new FileInputStream(skipPath), StandardCharsets.UTF_8))) {
                String line;
                while ((line = r.readLine()) != null) {
                    String a = line.split("\t", -1)[0].trim();
                    if (a.matches("[0-9A-Fa-f]{6,8}")) {
                        skip.add(Long.parseLong(a, 16));
                    }
                }
            }
        }
        DecompInterface ifc = new DecompInterface();
        DecompileOptions opts = new DecompileOptions();
        opts.grabFromProgram(currentProgram);
        ifc.setOptions(opts);
        ifc.toggleCCode(true);
        ifc.toggleSyntaxTree(false);
        ifc.setSimplificationStyle("decompile");
        if (!ifc.openProgram(currentProgram)) {
            throw new RuntimeException("decompiler: " + ifc.getLastMessage());
        }
        int done = 0, failed = 0, skipped = 0;
        long t0 = System.currentTimeMillis();
        PrintWriter w = null;
        long written = 0;
        for (Function f : fm.getFunctions(true)) {
            monitor.checkCancelled();
            if (f.isExternal()) {
                continue;
            }
            long entry = f.getEntryPoint().getOffset();
            if (w == null || written > maxBytes) {
                if (w != null) {
                    w.close();
                }
                w = new PrintWriter(new OutputStreamWriter(new FileOutputStream(new File(out, String.format("%08X.c", entry))), StandardCharsets.UTF_8));
                written = 0;
            }
            String[] n = names.get(entry);
            StringBuilder head = new StringBuilder();
            head.append(String.format("/* %08X size 0x%X %s", entry, f.getBody().getNumAddresses(), f.getName()));
            if (n != null) {
                head.append("\n   name: ").append(n[3]).append(" (").append(n[4].replace("*/", "* /")).append(")");
            } else if (f.getSymbol().getSource() != SourceType.DEFAULT) {
                head.append("\n   name: ghidra (").append(f.getSymbol().getSource()).append(")");
            }
            if (f.isThunk()) {
                head.append("\n   thunk");
            }
            String text;
            if (skip.contains(entry)) {
                head.append("\n   library code: C not exported */\n\n");
                text = "";
                skipped++;
            } else {
                head.append(" */\n");
                DecompileResults res = ifc.decompileFunction(f, 90, monitor);
                if (res != null && res.decompileCompleted() && res.getDecompiledFunction() != null) {
                    text = res.getDecompiledFunction().getC() + "\n";
                    done++;
                } else {
                    text = "/* decompile failed: " + (res == null ? "?" : res.getErrorMessage()) + " */\n\n";
                    failed++;
                }
            }
            String all = head + text;
            w.print(all);
            written += all.length();
            if ((done + failed) % 1000 == 0 && done > 0) {
                println(String.format("PcExport: %d decompiled, %d failed (%d s)", done, failed, (System.currentTimeMillis() - t0) / 1000));
            }
        }
        if (w != null) {
            w.close();
        }
        ifc.dispose();
        println(String.format("PcExport: decompiled: %d, failed: %d, listed only: %d, %d s", done, failed, skipped,
            (System.currentTimeMillis() - t0) / 1000));
    }

    @Override
    protected void run() throws Exception {
        listing = currentProgram.getListing();
        mem = currentProgram.getMemory();
        fm = currentProgram.getFunctionManager();
        String[] a = getScriptArgs();
        if (a.length < 2) {
            throw new IllegalArgumentException("usage: tables <out dir> | decomp <out dir> [names.tsv] [skip.tsv] [max bytes]");
        }
        if (a[0].equals("tables")) {
            tables(new File(a[1]));
        } else if (a[0].equals("decomp")) {
            decomp(new File(a[1]), a.length > 2 ? a[2] : null, a.length > 3 ? a[3] : null,
                a.length > 4 ? Long.parseLong(a[4]) : 2_500_000L);
        } else {
            throw new IllegalArgumentException("unknown mode " + a[0]);
        }
    }
}
