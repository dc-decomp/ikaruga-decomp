// Post-analysis: remove the functions that analysis made in the data past the end of the code, and
// those at the addresses of a list.
//
// The pointer scan and the symbol matcher make functions out of data there, for example the C
// library's character table _ctype. Words that point at them then get typed as code pointers, and
// the C indexes a code pointer. The code ends at 0x8C0CA2F4, with the literal pool of the function
// at 0x8C0CA2B4; the SDK's data follows (_kmtex_CodebookSize at 0x8C0CA830). dream-translate puts
// the end at 0x8C0C0574, but the game runs code after it: the rankings call 0x8C0C9510.
// The list (tools/ghidra/not_functions.txt) holds entries inside the code that are none: a piece of
// a function and a literal pool. A code end of - removes only the listed functions.
// Usage (headless): -postScript DropDataFunctions.java [code_end|-] [list]
//@category Dreamcast

import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;

public class DropDataFunctions extends GhidraScript {

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        boolean range = args.length == 0 || !args[0].equals("-");
        long end = range && args.length > 0 ? Long.decode(args[0]) : 0x8C0CA2F4L;
        List<Function> drop = new ArrayList<>();
        if (range) {
            for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
                if (f.getEntryPoint().getOffset() >= end && f.getEntryPoint().getOffset() < 0x8D000000L) {
                    drop.add(f);
                }
            }
        }
        int listed = 0;
        if (args.length > 1) {
            for (String line : Files.readAllLines(Path.of(args[1]))) {
                line = line.replaceAll("#.*", "").trim();
                if (line.isEmpty()) {
                    continue;
                }
                Function f = getFunctionAt(toAddr(Long.decode(line) | 0x80000000L));
                if (f != null && !drop.contains(f)) {
                    drop.add(f);
                    listed++;
                }
            }
        }
        for (Function f : drop) {
            currentProgram.getFunctionManager().removeFunction(f.getEntryPoint());
        }
        println("DropDataFunctions: " + (drop.size() - listed) + " functions past 0x"
                + Long.toHexString(end) + " and " + listed + " listed ones removed");
    }
}
