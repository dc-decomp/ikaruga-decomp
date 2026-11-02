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
//
// A seed inside a function whose code before it belongs to another function was taken in through a
// tail jump. Ghidra 12.2 resolves `mov.l @(disp,pc),rN; jmp @rN` during analysis, before any seed is
// a function, and then follows such a jump (and the `bra` to a shared tail) into the target:
// vmuImageClear took in syMemSet2, FUN_8c090e70 the tail ch1BossMotion. Here the unconditional jumps
// to the seed become call-returns, as they were in 12.1.4, and the seed becomes a function.
// Usage (headless): -postScript SeedFunctions.java <seeds.txt>
//@category Dreamcast
import java.nio.file.Files;
import java.nio.file.Path;

import ghidra.app.cmd.function.CreateFunctionCmd;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.data.Undefined;
import ghidra.program.model.listing.Data;
import ghidra.program.model.listing.FlowOverride;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.RefType;

public class SeedFunctions extends GhidraScript {

    @Override
    public void run() throws Exception {
        int seeded = 0, failed = 0, inside = 0, cleared = 0, split = 0;
        for (String line : Files.readAllLines(Path.of(getScriptArgs()[0]))) {
            line = line.trim();
            if (line.isEmpty() || line.startsWith("#")) {
                continue;
            }
            Address a = toAddr(Long.parseLong(line.replace("0x", ""), 16));
            if (getFunctionAt(a) != null) {
                continue;
            }
            Function taker = getFunctionContaining(a);
            if (taker != null) {
                Instruction before = getInstructionBefore(a);
                if (before == null || getFunctionContaining(before.getAddress()) == taker) {
                    inside++;
                    continue;
                }
                for (Reference r : getReferencesTo(a)) {
                    RefType type = r.getReferenceType();
                    Instruction jump = getInstructionAt(r.getFromAddress());
                    if (jump != null && type.isJump() && !type.isConditional()) {
                        jump.setFlowOverride(FlowOverride.CALL_RETURN);
                    }
                }
                // The command takes the seed's code out of the taker's body.
                CreateFunctionCmd create = new CreateFunctionCmd(a);
                if (create.applyTo(currentProgram, monitor)) {
                    println("SeedFunctions: " + a + " split from " + taker.getName());
                    split++;
                }
                else {
                    println("SeedFunctions: " + a + " not split from " + taker.getName() + ": " + create.getStatusMsg());
                    failed++;
                }
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
        println("SeedFunctions: " + seeded + " functions seeded, " + split + " split from a function that took"
                + " them in through a tail jump, " + inside + " skipped inside a function, " + failed
                + " failed; undefined data cleared at " + cleared);
    }
}
