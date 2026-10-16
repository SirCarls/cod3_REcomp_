// Clears "Bad Instruction" bookmarks and disassembles those places again, after the language
// learned new instructions (for example once ghidra_xenon/install_xenon.py added one).
// Headless: -postScript WawFixBadInstructions.java
// @category WaW

import java.util.*;

import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;

public class WawFixBadInstructions extends GhidraScript {
    @Override
    public void run() throws Exception {
        BookmarkManager bm = currentProgram.getBookmarkManager();
        List<Address> bad = new ArrayList<>();
        Iterator<Bookmark> it = bm.getBookmarksIterator(BookmarkType.ERROR);
        while (it.hasNext()) {
            Bookmark b = it.next();
            if (b.getCategory().contains("Bad Instruction")) bad.add(b.getAddress());
        }
        int fixed = 0;
        for (Address a : bad) {
            for (Bookmark b : bm.getBookmarks(a, BookmarkType.ERROR)) bm.removeBookmark(b);
            currentProgram.getListing().clearCodeUnits(a, a.add(3), false);
            new DisassembleCommand(a, null, true).applyTo(currentProgram, monitor);
            if (currentProgram.getListing().getInstructionAt(a) != null) fixed++;
        }
        println("WawFixBadInstructions: " + fixed + " of " + bad.size() + " decode now");
    }
}
