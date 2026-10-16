// Find functions that Ghidra's analysis missed, then analyze what they add.
//
// Ghidra finds functions by following calls from the entry point and exports. Code that is
// only ever reached through a pointer (script builtins, console command handlers, menu
// parsers, callbacks, virtual tables) is often left as undefined bytes, or disassembled
// without a function around it. This script looks for such code in four ways, in rounds,
// until a round finds nothing new:
//
//   data     a 4-byte word in a data section whose value is an address in the code section
//   ref      an instruction operand that is the address of code (push offset handler)
//   call     a call target with no function (Ghidra skips some made by these rounds)
//   orphan   instructions that no function contains
//   gap      undefined bytes after alignment padding that disassemble into a clean subroutine
//   chunk    a piece of a function's body that sits apart from the rest of it, after padding:
//            Ghidra pulls a function reached only by a tail jump ("jmp CG_CalcFov") into the
//            body of the function that jumps to it. MSVC 7.1 never splits a function, so such
//            a piece is a function of its own.
//
// A target inside an existing function becomes a function of its own only when it looks like
// a function start: 16-byte aligned, preceded by padding or by the previous function's last
// jump/return, and not a branch target of the function it is in (this undoes Ghidra running
// one function into the next after a call that never returns).
//
// Code sections are the executable, initialized memory blocks; blocks whose name starts with
// "stxt" (disc copy-protection loader sections) are skipped.
//
// Arguments: none. Prints "PcFindFunctions: ..." summary lines.
//@category PcDecomp
import java.util.*;

import ghidra.app.cmd.function.CreateFunctionCmd;
import ghidra.app.script.GhidraScript;
import ghidra.app.util.PseudoDisassembler;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.mem.*;
import ghidra.program.model.symbol.*;

public class PcFindFunctions extends GhidraScript {
    private final List<MemoryBlock> codeBlocks = new ArrayList<>();
    private final List<MemoryBlock> dataBlocks = new ArrayList<>();
    private PseudoDisassembler pseudo;
    private Listing listing;
    private FunctionManager fm;
    private Memory mem;

    private boolean inCode(Address a) {
        for (MemoryBlock b : codeBlocks) {
            if (b.contains(a)) {
                return true;
            }
        }
        return false;
    }

    private int byteAt(Address a) {
        try {
            return mem.getByte(a) & 0xff;
        } catch (Exception e) {
            return -1;
        }
    }

    /** Bytes of the multi-byte no-op forms MSVC pads with, matched at a; returns the length or 0. */
    private int paddingLength(Address a) {
        int b0 = byteAt(a);
        if (b0 == 0xCC || b0 == 0x90) {
            return 1;
        }
        int b1 = byteAt(a.add(1));
        int b2 = byteAt(a.add(2));
        if (b0 == 0x8B && b1 == 0xFF) {
            return 2; // mov edi, edi
        }
        if (b0 == 0x8D) {
            if (b1 == 0x49 && b2 == 0x00) {
                return 3; // lea ecx, [ecx+0]
            }
            if (b1 == 0x64 && b2 == 0x24 && byteAt(a.add(3)) == 0) {
                return 4; // lea esp, [esp+0]
            }
            if (b1 == 0xA4 && b2 == 0x24) {
                return 7; // lea esp, [esp+0x00000000]
            }
            if (b1 == 0x9B) {
                return 6; // lea ebx, [ebx+0x00000000]
            }
        }
        return 0;
    }

    /** True if what comes right before a is padding, or the end of a jump or return. */
    private boolean looksLikeStart(Address a) {
        Address prev = a.previous();
        if (prev == null) {
            return true;
        }
        int pb = byteAt(prev);
        if (pb == 0xCC || pb == 0x90) {
            return true;
        }
        Instruction pi = listing.getInstructionContaining(prev);
        if (pi != null) {
            FlowType ft = pi.getFlowType();
            return (ft.isTerminal() || (ft.isJump() && ft.isUnConditional())) && !ft.isCall()
                || paddingInstruction(pi);
        }
        return false;
    }

    private boolean paddingInstruction(Instruction i) {
        return paddingLength(i.getAddress()) == i.getLength();
    }

    /** True if some flow (jump) into a comes from inside function f. */
    private boolean branchTargetInside(Address a, Function f) {
        for (Reference r : getReferencesTo(a)) {
            if (r.getReferenceType().isFlow() && !r.getReferenceType().isCall()) {
                Function from = fm.getFunctionContaining(r.getFromAddress());
                if (from == f) {
                    return true;
                }
            }
        }
        return false;
    }

    private final Map<String, Integer> made = new TreeMap<>();
    private final List<Address> createdHere = new ArrayList<>();

    private boolean tryCreate(Address t, String why, boolean strict) throws Exception {
        if (!inCode(t) || fm.getFunctionAt(t) != null) {
            return false;
        }
        Instruction ins = listing.getInstructionAt(t);
        Function f = fm.getFunctionContaining(t);
        if (f != null) {
            // inside another function: split only at something that looks like a function start
            if (ins == null || (t.getOffset() & 0xF) != 0 || !looksLikeStart(t) || branchTargetInside(t, f)) {
                return false;
            }
            why = why + "-split";
        } else if (ins == null) {
            int b0 = byteAt(t);
            if (b0 == 0xCC || b0 == 0x90) {
                return false; // padding
            }
            if (listing.getInstructionContaining(t) != null) {
                return false; // the middle of an instruction
            }
            Data d = listing.getDefinedDataContaining(t);
            if (d != null && d.isDefined()) {
                return false;
            }
            if (strict && (t.getOffset() & 0xF) != 0 && !looksLikeStart(t)) {
                return false;
            }
            if (!pseudo.isValidSubroutine(t, true)) {
                return false;
            }
            disassemble(t);
            if (listing.getInstructionAt(t) == null) {
                return false;
            }
        } else {
            why = why + "-orphan";
        }
        CreateFunctionCmd cmd = new CreateFunctionCmd(t);
        if (cmd.applyTo(currentProgram, monitor) && fm.getFunctionAt(t) != null) {
            made.merge(why, 1, Integer::sum);
            createdHere.add(t);
            return true;
        }
        return false;
    }

    private int pass() throws Exception {
        int n = 0;
        // 1. pointers stored in data sections
        for (MemoryBlock b : dataBlocks) {
            long size = b.getSize() & ~3L;
            byte[] buf = new byte[(int) size];
            b.getBytes(b.getStart(), buf);
            for (int o = 0; o + 4 <= buf.length; o += 4) {
                monitor.checkCancelled();
                long v = (buf[o] & 0xffL) | (buf[o + 1] & 0xffL) << 8 | (buf[o + 2] & 0xffL) << 16
                    | (buf[o + 3] & 0xffL) << 24;
                Address t;
                try {
                    t = toAddr(v);
                } catch (Exception e) {
                    continue;
                }
                if (inCode(t) && tryCreate(t, "data", true)) {
                    n++;
                }
            }
        }
        // 2. code addresses used as instruction operands (non-flow references)
        List<Address> refTargets = new ArrayList<>();
        for (MemoryBlock b : codeBlocks) {
            AddressIterator it = currentProgram.getReferenceManager().getReferenceDestinationIterator(
                new AddressSet(b.getStart(), b.getEnd()), true);
            while (it.hasNext()) {
                Address to = it.next();
                for (Reference r : getReferencesTo(to)) {
                    if (!r.getReferenceType().isFlow() && listing.getInstructionAt(r.getFromAddress()) != null) {
                        refTargets.add(to);
                        break;
                    }
                }
            }
        }
        for (Address t : refTargets) {
            monitor.checkCancelled();
            if (tryCreate(t, "ref", true)) {
                n++;
            }
        }
        // 3. call targets without a function
        List<Address> callTargets = new ArrayList<>();
        for (MemoryBlock b : codeBlocks) {
            AddressIterator it = currentProgram.getReferenceManager().getReferenceDestinationIterator(
                new AddressSet(b.getStart(), b.getEnd()), true);
            while (it.hasNext()) {
                Address to = it.next();
                if (fm.getFunctionAt(to) != null) {
                    continue;
                }
                for (Reference r : getReferencesTo(to)) {
                    if (r.getReferenceType().isCall()) {
                        callTargets.add(to);
                        break;
                    }
                }
            }
        }
        for (Address t : callTargets) {
            monitor.checkCancelled();
            if (tryCreate(t, "call", false)) {
                n++;
            }
        }
        // 4. pieces of a body apart from the rest
        List<Address> chunks = new ArrayList<>();
        for (Function f : fm.getFunctions(true)) {
            AddressSetView body = f.getBody();
            if (body.getNumAddressRanges() < 2) {
                continue;
            }
            Address entry = f.getEntryPoint();
            // the span: ranges joined to the entry's range through padding only
            List<AddressRange> rs = new ArrayList<>();
            for (AddressRange r : body) {
                rs.add(r);
            }
            int k0 = 0;
            for (int k = 0; k < rs.size(); k++) {
                if (rs.get(k).contains(entry)) {
                    k0 = k;
                }
            }
            // ranges after the entry joined to it through padding are its own (aligned labels);
            // MSVC never puts a function's code before its entry, so every range below the entry
            // belongs to some other function
            int hi = k0;
            while (hi + 1 < rs.size() && onlyPadding(rs.get(hi).getMaxAddress().next(), rs.get(hi + 1).getMinAddress())) {
                hi++;
            }
            for (int k = 0; k < rs.size(); k++) {
                if (k >= k0 && k <= hi) {
                    continue;
                }
                Address s0 = rs.get(k).getMinAddress();
                boolean groupStart = k == 0 || k == hi + 1 || k == k0
                    || !onlyPadding(rs.get(k - 1).getMaxAddress().next(), s0);
                if (groupStart && (s0.getOffset() & 0xF) == 0 && looksLikeStart(s0)) {
                    chunks.add(s0);
                }
            }
        }
        for (Address t : chunks) {
            monitor.checkCancelled();
            if (fm.getFunctionAt(t) == null && listing.getInstructionAt(t) != null
                && new CreateFunctionCmd(t).applyTo(currentProgram, monitor) && fm.getFunctionAt(t) != null) {
                made.merge("chunk", 1, Integer::sum);
                createdHere.add(t);
                n++;
            }
        }
        // 5. orphan instructions and 6. gaps after padding
        for (MemoryBlock b : codeBlocks) {
            Address a = b.getStart();
            while (a != null && a.compareTo(b.getEnd()) <= 0) {
                monitor.checkCancelled();
                Function f = fm.getFunctionContaining(a);
                if (f != null) {
                    Address end = f.getBody().getRangeContaining(a).getMaxAddress();
                    a = end.next();
                    continue;
                }
                Instruction ins = listing.getInstructionAt(a);
                if (ins != null) {
                    if (!paddingInstruction(ins) && looksLikeStart(a) && !hasJumpFromOrphanBefore(a) && onlyTailJumpsIn(a)
                        && tryCreate(a, "orphan", false)) {
                        n++;
                        continue;
                    }
                    a = ins.getMaxAddress().next();
                    continue;
                }
                Data d = listing.getDefinedDataContaining(a);
                if (d != null && d.isDefined()) {
                    a = d.getMaxAddress().next();
                    continue;
                }
                int pad = paddingLength(a);
                if (pad > 0) {
                    a = a.add(pad);
                    continue;
                }
                if ((a.getOffset() & 0xF) == 0 && looksLikeStart(a) && !looksLikePointerPair(a)
                    && pseudo.isValidSubroutine(a, false) && tryCreate(a, "gap", false)) {
                    n++;
                    continue;
                }
                a = a.next();
            }
        }
        return n;
    }

    /**
     * True if a jump comes to a from code before it that no function contains: a label inside
     * that code. Jumps from functions are tail calls; jumps from later code are loops.
     */
    private boolean hasJumpFromOrphanBefore(Address a) {
        for (Reference r : getReferencesTo(a)) {
            RefType rt = r.getReferenceType();
            Address from = r.getFromAddress();
            if (rt.isFlow() && !rt.isCall() && from.compareTo(a) < 0 && fm.getFunctionContaining(from) == null) {
                return true;
            }
        }
        return false;
    }

    /**
     * True if every jump into a is a plain "jmp a" from some function (a tail call), and no jump
     * table points at it: switch cases and branch targets are labels, not functions.
     */
    private boolean onlyTailJumpsIn(Address a) {
        for (Reference r : getReferencesTo(a)) {
            RefType rt = r.getReferenceType();
            Address from = r.getFromAddress();
            if (rt.isFlow() && !rt.isCall() && (rt != RefType.UNCONDITIONAL_JUMP || fm.getFunctionContaining(from) == null)) {
                return false;
            }
            if (!rt.isFlow() && inCode(from) && listing.getInstructionContaining(from) == null) {
                return false; // a jump table in the code section
            }
        }
        return true;
    }

    /** True if [from, to) holds nothing but padding bytes (or is empty). */
    private boolean onlyPadding(Address from, Address to) {
        Address a = from;
        while (a != null && a.compareTo(to) < 0) {
            int p = paddingLength(a);
            if (p == 0) {
                return false;
            }
            a = a.add(p);
        }
        return true;
    }

    /** Two code addresses in a row: a jump table, not code. */
    private boolean looksLikePointerPair(Address a) {
        try {
            return inCode(toAddr(mem.getInt(a) & 0xffffffffL)) && inCode(toAddr(mem.getInt(a.add(4)) & 0xffffffffL));
        } catch (Exception e) {
            return false;
        }
    }

    private void coverage(String when) {
        long total = 0, inFunc = 0, data = 0, undef = 0;
        for (MemoryBlock b : codeBlocks) {
            total += b.getSize();
        }
        AddressSet code = new AddressSet();
        for (MemoryBlock b : codeBlocks) {
            code.add(b.getStart(), b.getEnd());
        }
        for (Function f : fm.getFunctions(code, true)) {
            inFunc += f.getBody().intersect(code).getNumAddresses();
        }
        DataIterator di = listing.getDefinedData(code, true);
        while (di.hasNext()) {
            data += di.next().getLength();
        }
        undef = total - inFunc - data;
        println(String.format("PcFindFunctions: %s: %d functions; code bytes %d: %.1f%% in functions, %.1f%% data, %.1f%% other",
            when, fm.getFunctionCount(), total, 100.0 * inFunc / total, 100.0 * data / total, 100.0 * undef / total));
    }

    @Override
    protected void run() throws Exception {
        listing = currentProgram.getListing();
        fm = currentProgram.getFunctionManager();
        mem = currentProgram.getMemory();
        pseudo = new PseudoDisassembler(currentProgram);
        for (MemoryBlock b : mem.getBlocks()) {
            if (!b.isInitialized() || b.isExternalBlock() || b.getName().startsWith("stxt")) {
                continue;
            }
            if (b.isExecute()) {
                codeBlocks.add(b);
            } else if (!b.getName().equals("Headers") && !b.getName().startsWith(".rsrc")
                && !b.getName().startsWith(".reloc")) {
                dataBlocks.add(b);
            }
        }
        coverage("before");
        for (int round = 1; round <= 6; round++) {
            int n = pass();
            println("PcFindFunctions: round " + round + ": " + n + " new functions " + made);
            if (n == 0) {
                break;
            }
            analyzeChanges(currentProgram);
            // switch tables resolved by that analysis add their cases to the new functions
            int fixed = 0;
            for (Address t : createdHere) {
                Function f = fm.getFunctionAt(t);
                if (f != null) {
                    long before = f.getBody().getNumAddresses();
                    CreateFunctionCmd.fixupFunctionBody(currentProgram, f, monitor);
                    if (f.getBody().getNumAddresses() != before) {
                        fixed++;
                    }
                }
            }
            println("PcFindFunctions: round " + round + ": " + fixed + " function bodies grew after analysis");
        }
        coverage("after");
    }
}
