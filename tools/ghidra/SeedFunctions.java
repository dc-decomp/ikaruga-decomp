// Post-analysis: create a function at every address listed in a file (one hex address per line) that
// no function contains yet, then analyse what that added.
//
// Only holes are seeded. An address inside a function is a branch target, a switch case or a shared
// tail; making it a function would cut the enclosing one short. Running after analysis lets Ghidra
// recover the switch tables first, so their case labels are already inside a function here.
//
// A seed that undefined data covers is code all the same: the analyzer reads a 4-byte fmov.s from a
// literal pool as 8 bytes (it shares its constructor with the pair move), and the data it leaves
// runs over the code that follows the pool. The stub at 0x8C0A01A0 started 4 bytes into such data.
// Usage (headless): -postScript SeedFunctions.java <seeds.txt>
//@category Dreamcast
import java.nio.file.Files;
import java.nio.file.Path;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.data.Undefined;
import ghidra.program.model.listing.Data;

public class SeedFunctions extends GhidraScript {

    @Override
    public void run() throws Exception {
        int seeded = 0, failed = 0, inside = 0, cleared = 0;
        for (String line : Files.readAllLines(Path.of(getScriptArgs()[0]))) {
            line = line.trim();
            if (line.isEmpty() || line.startsWith("#")) {
                continue;
            }
            Address a = toAddr(Long.parseLong(line.replace("0x", ""), 16));
            if (getFunctionAt(a) != null) {
                continue;
            }
            if (getFunctionContaining(a) != null) {
                inside++;
                continue;
            }
            Data covering = getDataContaining(a);
            if (covering != null && Undefined.isUndefined(covering.getDataType())) {
                removeData(covering);
                cleared++;
            }
            if (getInstructionAt(a) == null && !disassemble(a)) {
                failed++;
                continue;
            }
            if (createFunction(a, null) != null) {
                seeded++;
            }
            else {
                failed++;
            }
        }
        analyzeChanges(currentProgram);
        println("SeedFunctions: " + seeded + " functions seeded, " + inside
                + " skipped inside a function, " + failed + " failed; undefined data cleared at " + cleared);
    }
}
