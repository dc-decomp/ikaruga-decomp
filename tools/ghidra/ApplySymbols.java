// Post-analysis: name functions and data from symbols.tsv files (address, name, source; tab
// separated; addresses in either RAM spelling). One leading underscore, the SHC linker prefix, is
// dropped.
//
// A name at a function entry renames the function. A name where no function is yet becomes a
// function only when the address is known to hold code: the SDK library code matched there byte for
// byte, or Ghidra already decoded an instruction there. Anything else becomes a label, because a
// name from a relocation or from a hand-made row can be data. A file given as labels:<path> only
// makes labels.
// Usage (headless): -postScript ApplySymbols.java <symbols.tsv> [labels:<globals.tsv> ...]
//@category Dreamcast
import java.nio.file.Files;
import java.nio.file.Path;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.SourceType;
import ghidra.util.exception.DuplicateNameException;

public class ApplySymbols extends GhidraScript {

    @Override
    public void run() throws Exception {
        int renamed = 0, created = 0, labels = 0;
        for (String arg : getScriptArgs()) {
            boolean labelsOnly = arg.startsWith("labels:");
            Path file = Path.of(labelsOnly ? arg.substring(7) : arg);
            if (!Files.exists(file)) {
                continue;
            }
            for (String line : Files.readAllLines(file)) {
                if (line.isBlank() || line.startsWith("#")) {
                    continue;
                }
                String[] f = line.split("\t");
                long raw = Long.parseLong(f[0].replace("0x", ""), 16);
                Address a = toAddr((raw & 0x1FFFFFFFL) | 0x80000000L);
                String name = f[1].startsWith("_") ? f[1].substring(1) : f[1];
                String source = f.length > 2 ? f[2] : "";

                Function fn = labelsOnly ? null : getFunctionAt(a);
                MemoryBlock blk = currentProgram.getMemory().getBlock(a);
                if (fn == null && !labelsOnly && getFunctionContaining(a) == null && blk != null
                        && blk.isInitialized()) {
                    boolean libraryCode = source.contains(".obj.elf");
                    if (getInstructionAt(a) != null || (libraryCode && disassemble(a))) {
                        fn = createFunction(a, null);
                        if (fn != null) {
                            created++;
                        }
                    }
                }
                if (fn != null) {
                    try {
                        fn.setName(name, SourceType.IMPORTED);
                    }
                    catch (DuplicateNameException e) {
                        fn.setName(name + "_" + a, SourceType.IMPORTED);
                    }
                    renamed++;
                }
                else {
                    createLabel(a, name, true, SourceType.IMPORTED);
                    labels++;
                }
            }
        }
        analyzeChanges(currentProgram);
        println("ApplySymbols: " + renamed + " functions named (" + created + " created), " + labels
                + " labels");
    }
}
