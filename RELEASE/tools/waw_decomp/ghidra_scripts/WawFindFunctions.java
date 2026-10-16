// Finds x86 functions Ghidra's auto-analysis missed in an MSVC-built executable (World at War PC):
// code reached only through function-pointer tables. MSVC pads between functions with 0xCC
// (int3), so a 16-byte-aligned address right after padding or after a function's last
// instruction, holding valid code that isn't a pointer table, starts a function.
//
// Headless: -postScript WawFindFunctions.java   (then re-run analysis)
// @category WaW

import java.util.*;

import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.app.cmd.function.CreateFunctionCmd;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.mem.*;

public class WawFindFunctions extends GhidraScript {

    private boolean looksLikePointerTable(Memory mem, Address a, AddressSetView text) {
        try {
            int hits = 0;
            for (int i = 0; i < 4; i++) {
                long v = mem.getInt(a.add(i * 4)) & 0xFFFFFFFFL;
                if (text.contains(a.getNewAddress(v))) hits++;
            }
            return hits >= 3;
        } catch (Exception e) {
            return false;
        }
    }

    @Override
    public void run() throws Exception {
        Memory mem = currentProgram.getMemory();
        MemoryBlock textBlock = mem.getBlock(".text");
        if (textBlock == null) {
            printerr("no .text block");
            return;
        }
        AddressSet text = new AddressSet(textBlock.getStart(), textBlock.getEnd());
        Listing listing = currentProgram.getListing();
        FunctionManager fm = currentProgram.getFunctionManager();
        int total = 0;
        // Phase 1: addresses stored in the data sections that point at the start of padded code.
        int fromData = 0, failedData = 0;
        for (MemoryBlock blk : mem.getBlocks()) {
            String bn = blk.getName();
            if (!(bn.equals(".data") || bn.equals(".rdata")) || !blk.isInitialized()) continue;
            Address p = blk.getStart();
            long n = blk.getSize() / 4;
            for (long i = 0; i < n; i++, p = p.add(4)) {
                if (monitor.isCancelled()) return;
                long v;
                try { v = mem.getInt(p) & 0xFFFFFFFFL; } catch (Exception e) { continue; }
                if ((v & 0xF) != 0) continue;
                Address t;
                try { t = toAddr(v); } catch (Exception e) { continue; }
                if (!text.contains(t) || fm.getFunctionContaining(t) != null) continue;
                byte prev = mem.getByte(t.subtract(1));
                if (prev != (byte) 0xCC && prev != (byte) 0xC3 && prev != (byte) 0x90) continue;
                Data d = listing.getDataContaining(t);
                if (d != null) {
                    if (d.isDefined()) continue;
                }
                if (listing.getInstructionAt(t) == null) {
                    if (listing.getDefinedDataContaining(t) != null) continue;
                    new DisassembleCommand(t, null, true).applyTo(currentProgram, monitor);
                }
                if (listing.getInstructionAt(t) == null) { failedData++; continue; }
                if (new CreateFunctionCmd(t).applyTo(currentProgram, monitor) && fm.getFunctionAt(t) != null) fromData++;
                else {
                    if (failedData < 10) println("could not create function at " + t);
                    failedData++;
                }
            }
        }
        println("phase 1 (pointers in data): " + fromData + " functions created, " + failedData + " failed");
        total += fromData;
        for (int round = 0; round < 6; round++) {
            int made = 0;
            Address a = textBlock.getStart();
            while (a != null && a.compareTo(textBlock.getEnd()) < 0) {
                if (monitor.isCancelled()) return;
                long off = a.getOffset();
                if ((off & 0xF) != 0 || fm.getFunctionContaining(a) != null || listing.getDefinedDataContaining(a) != null) {
                    a = a.add(1);
                    continue;
                }
                byte b0 = mem.getByte(a);
                if (b0 == (byte) 0xCC || b0 == 0x00) {
                    a = a.add(16);
                    continue;
                }
                byte prev = mem.getByte(a.subtract(1));
                Function before = fm.getFunctionContaining(a.subtract(1));
                Instruction pi = listing.getInstructionContaining(a.subtract(1));
                boolean afterPadding = prev == (byte) 0xCC || prev == (byte) 0x90;
                boolean afterEnd = pi != null && (pi.getFlowType().isTerminal() || pi.getFlowType().isUnConditional());
                if (!(afterPadding || afterEnd || before == null) || looksLikePointerTable(mem, a, text)) {
                    a = a.add(16);
                    continue;
                }
                if (listing.getInstructionAt(a) == null) {
                    DisassembleCommand dc = new DisassembleCommand(a, null, true);
                    dc.applyTo(currentProgram, monitor);
                }
                Instruction ins = listing.getInstructionAt(a);
                if (ins == null) {
                    a = a.add(16);
                    continue;
                }
                CreateFunctionCmd cf = new CreateFunctionCmd(a);
                if (cf.applyTo(currentProgram, monitor) && fm.getFunctionAt(a) != null) {
                    made++;
                    Function f = fm.getFunctionAt(a);
                    Address end = f.getBody().getMaxAddress();
                    a = end.add(1);
                    continue;
                }
                a = a.add(16);
            }
            total += made;
            println("round " + round + ": " + made + " functions created");
            if (made == 0) break;
        }
        println("WawFindFunctions: created " + total + " functions");
    }
}
