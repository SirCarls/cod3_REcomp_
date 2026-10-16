// Prepares an Xbox 360 image (the plain PE made by xex2pe.py, imported with the Binary loader at its
// load address and the PowerPC:BE:64:Xenon-32addr language) for analysis:
//   - splits memory into the PE sections with their permissions,
//   - creates every function listed in .pdata (exact starts and lengths from the compiler),
//   - names the kernel/XAM import thunks and import slots (imports.tsv from xex2pe.py --imports),
//   - marks the entry point.
//
// Headless: -preScript WawXenonSetup.java <imports.tsv|-> <entry hex|-> <load hex>
// @category WaW

import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.util.*;

import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.app.cmd.function.CreateFunctionCmd;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.mem.*;
import ghidra.program.model.symbol.*;

public class WawXenonSetup extends GhidraScript {

    private int le32(Memory m, Address a) throws Exception {
        byte[] b = new byte[4];
        m.getBytes(a, b);
        return (b[0] & 0xFF) | (b[1] & 0xFF) << 8 | (b[2] & 0xFF) << 16 | (b[3] & 0xFF) << 24;
    }

    private int le16(Memory m, Address a) throws Exception {
        byte[] b = new byte[2];
        m.getBytes(a, b);
        return (b[0] & 0xFF) | (b[1] & 0xFF) << 8;
    }

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        Memory mem = currentProgram.getMemory();
        // The Binary loader doesn't always put the image at the requested base for this language:
        // move the (single) block to the load address given on the command line.
        long load = args.length > 2 && !args[2].equals("-") ? Long.parseLong(args[2], 16) : 0x82000000L;
        Address base = toAddr(load);
        MemoryBlock first = mem.getBlocks()[0];
        if (!first.getStart().equals(base)) {
            mem.moveBlock(first, base, monitor);
        }
        // Leave the image base at 0: for this 64-bit language with 32-bit addresses, an image base
        // of 0x82000000 is added twice inside Ghidra (the image then shows up at 0x04000000).
        Listing listing = currentProgram.getListing();
        SymbolTable st = currentProgram.getSymbolTable();

        // --- sections from the PE header (little-endian fields)
        int pe = le32(mem, base.add(0x3C));
        int nsec = le16(mem, base.add(pe + 6));
        int optsz = le16(mem, base.add(pe + 20));
        Address sec = base.add(pe + 24 + optsz);
        List<Object[]> secs = new ArrayList<>();
        for (int i = 0; i < nsec; i++) {
            byte[] nb = new byte[8];
            mem.getBytes(sec, nb);
            String name = new String(nb, StandardCharsets.US_ASCII).replace("\0", "");
            int vsize = le32(mem, sec.add(8));
            int va = le32(mem, sec.add(12));
            int ch = le32(mem, sec.add(36));
            secs.add(new Object[] { name, va, vsize, ch });
            sec = sec.add(40);
        }
        secs.sort(Comparator.comparingInt(o -> (Integer) o[1]));
        // Split at every section start first: Xbox 360 sections need not start on a page, so each
        // section's block runs to the next section's start (the last one to its rounded end).
        for (Object[] s : secs) {
            Address start = base.add(((Integer) s[1]) & 0xFFFFFFFFL);
            MemoryBlock blk = mem.getBlock(start);
            if (blk != null && !blk.getStart().equals(start)) mem.split(blk, start);
        }
        Object[] last = secs.get(secs.size() - 1);
        Address lastStart = base.add(((Integer) last[1]) & 0xFFFFFFFFL);
        long lastEnd = (lastStart.getOffset() + Math.max(1, (Integer) last[2]) + 0xFFF) & ~0xFFFL;
        MemoryBlock lb = mem.getBlock(lastStart);
        if (lb != null && lastEnd <= lb.getEnd().getOffset()) mem.split(lb, base.getNewAddress(lastEnd));
        for (Object[] s : secs) {
            MemoryBlock blk = mem.getBlock(base.add(((Integer) s[1]) & 0xFFFFFFFFL));
            if (blk == null) continue;
            int ch = (Integer) s[3];
            boolean x = (ch & 0x20000000) != 0, w = (ch & 0x80000000) != 0;
            blk.setName((String) s[0]);
            blk.setPermissions(true, w, x);
        }
        MemoryBlock hdr = mem.getBlock(base);
        if (hdr != null && hdr.getStart().equals(base) && !hdr.getName().startsWith(".")) {
            hdr.setName("headers");
            hdr.setPermissions(true, false, false);
        }
        println("sections: " + secs.size());

        // --- imports
        int imps = 0;
        if (args.length > 0 && !args[0].equals("-")) {
            for (String line : Files.readAllLines(Paths.get(args[0]), StandardCharsets.UTF_8)) {
                String[] c = line.split("\t");
                if (c.length < 5 || c[0].equals("addr")) continue;
                Address a = toAddr(Long.parseLong(c[0], 16));
                String name = c[4];
                try {
                    if (c[1].equals("func")) {
                        // The thunk holds the import record until the console's loader patches it.
                        // In this analysis copy it becomes "blr; nop; nop; nop" so callers decompile
                        // as calls to a function named after the import.
                        mem.setInt(a, 0x4E800020);
                        mem.setInt(a.add(4), 0x60000000);
                        mem.setInt(a.add(8), 0x60000000);
                        mem.setInt(a.add(12), 0x60000000);
                        new DisassembleCommand(a, null, true).applyTo(currentProgram, monitor);
                        if (new CreateFunctionCmd(a).applyTo(currentProgram, monitor)) {
                            Function f = getFunctionAt(a);
                            if (f != null) f.setName(name, SourceType.IMPORTED);
                        }
                    } else {
                        st.createLabel(a, "__imp_" + name, SourceType.IMPORTED).setPrimary();
                    }
                    imps++;
                } catch (Exception e) {
                    printerr("import " + name + ": " + e.getMessage());
                }
            }
        }
        println("imports named: " + imps);

        // --- .pdata functions
        MemoryBlock pdata = mem.getBlock(".pdata");
        int made = 0, bad = 0;
        if (pdata != null) {
            Address p = pdata.getStart();
            long n = pdata.getSize() / 8;
            for (long i = 0; i < n; i++, p = p.add(8)) {
                long begin = mem.getInt(p) & 0xFFFFFFFFL;
                long info = mem.getInt(p.add(4)) & 0xFFFFFFFFL;
                if (begin == 0) continue;
                long len = ((info >> 8) & 0x3FFFFF) * 4;
                Address a = toAddr(begin);
                if (!mem.contains(a) || len == 0) { bad++; continue; }
                AddressSet body = new AddressSet(a, a.add(len - 1));
                new DisassembleCommand(a, body, true).applyTo(currentProgram, monitor);
                if (getFunctionAt(a) == null) {
                    CreateFunctionCmd cf = new CreateFunctionCmd(null, a, body, SourceType.ANALYSIS);
                    if (cf.applyTo(currentProgram, monitor)) made++; else bad++;
                } else made++;
            }
        }
        println("pdata functions: " + made + " (" + bad + " not made)");

        if (args.length > 1 && !args[1].equals("-")) {
            Address e = toAddr(Long.parseLong(args[1], 16));
            st.createLabel(e, "xex_entry", SourceType.IMPORTED);
            st.addExternalEntryPoint(e);
        }
    }
}
