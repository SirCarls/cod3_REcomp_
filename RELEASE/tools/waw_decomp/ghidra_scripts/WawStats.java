// Prints counts that show how well a program was analyzed: functions, instructions, and
// "Bad Instruction" error bookmarks (bytes the language could not decode).
// Headless: -postScript WawStats.java
// @category WaW

import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;

public class WawStats extends GhidraScript {
    @Override
    public void run() throws Exception {
        int funcs = currentProgram.getFunctionManager().getFunctionCount();
        long insns = currentProgram.getListing().getNumInstructions();
        int bad = 0;
        BookmarkManager bm = currentProgram.getBookmarkManager();
        java.util.Iterator<Bookmark> it = bm.getBookmarksIterator(BookmarkType.ERROR);
        java.util.Map<String, Integer> cats = new java.util.TreeMap<>();
        while (it.hasNext()) {
            Bookmark b = it.next();
            cats.merge(b.getCategory(), 1, Integer::sum);
            if (b.getCategory().contains("Bad Instruction")) {
                if (bad < 15) println("bad instruction at " + b.getAddress() + ": " + b.getComment());
                bad++;
            }
        }
        int vmx128 = 0;
        InstructionIterator ii = currentProgram.getListing().getInstructions(true);
        while (ii.hasNext()) {
            if (ii.next().getMnemonicString().replace(".", "").endsWith("128")) vmx128++;
        }
        println("WawStats: vmx128_instructions=" + vmx128);
        println("WawStats: functions=" + funcs + " instructions=" + insns + " bad_instructions=" + bad + " error_bookmarks=" + cats);
    }
}
