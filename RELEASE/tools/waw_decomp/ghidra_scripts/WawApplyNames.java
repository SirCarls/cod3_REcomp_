// Applies a names table (addr, name, kind, pass, evidence) to the program: function names and
// global labels, with the naming pass and evidence as a plate comment so every name can be traced.
// Rows of kind "note" (no name) only set the function's comment, such as the address of the
// same function in the other build.
//
// Headless: -postScript WawApplyNames.java <names.tsv>
// @category WaW

import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.util.*;

import ghidra.app.cmd.function.CreateFunctionCmd;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;

public class WawApplyNames extends GhidraScript {

    private static String clean(String n) {
        StringBuilder b = new StringBuilder();
        for (char c : n.toCharArray()) b.append(Character.isLetterOrDigit(c) || c == '_' ? c : '_');
        return b.toString();
    }

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        List<String> lines = Files.readAllLines(Paths.get(args[0]), StandardCharsets.UTF_8);
        FunctionManager fm = currentProgram.getFunctionManager();
        SymbolTable st = currentProgram.getSymbolTable();
        Listing listing = currentProgram.getListing();
        int funcs = 0, globals = 0, failed = 0, reverted = 0;
        // Names we gave earlier that the new table no longer has go back to Ghidra's default.
        Set<Address> wanted = new HashSet<>();
        for (String line : lines.subList(1, lines.size())) {
            String[] c = line.split("\t", -1);
            if (c.length >= 5 && !c[2].equals("note")) wanted.add(toAddr(Long.parseLong(c[0], 16)));
        }
        for (Function f : fm.getFunctions(true)) {
            String cm = f.getComment();
            if (cm != null && cm.startsWith("name: ") && !wanted.contains(f.getEntryPoint())) {
                f.setName(null, SourceType.DEFAULT);
                f.setComment(null);
                reverted++;
            } else if (cm != null && cm.startsWith("note: ")) {
                f.setComment(null);
            }
        }
        int notes = 0;
        for (String line : lines.subList(1, lines.size())) {
            String[] c = line.split("\t", -1);
            if (c.length < 5) continue;
            Address a = toAddr(Long.parseLong(c[0], 16));
            String name = clean(c[1]);
            String note = "name: " + c[3] + " (" + c[4] + ")";
            try {
                if (c[2].equals("note")) {
                    Function f = fm.getFunctionAt(a);
                    if (f == null) { failed++; continue; }
                    f.setComment("note: " + c[3] + " (" + c[4] + ")");
                    notes++;
                } else if (c[2].equals("function")) {
                    Function f = fm.getFunctionAt(a);
                    if (f == null) {
                        new CreateFunctionCmd(a).applyTo(currentProgram, monitor);
                        f = fm.getFunctionAt(a);
                    }
                    if (f == null) { failed++; continue; }
                    f.setName(name, SourceType.USER_DEFINED);
                    f.setComment(note);
                    funcs++;
                } else {
                    Symbol s = st.createLabel(a, name, SourceType.USER_DEFINED);
                    s.setPrimary();
                    listing.setComment(a, CodeUnit.PLATE_COMMENT, note);
                    globals++;
                }
            } catch (Exception e) {
                failed++;
            }
        }
        println("WawApplyNames: " + funcs + " functions, " + globals + " globals, " + notes + " notes, " + failed + " failed, " + reverted + " old names reverted");
    }
}
