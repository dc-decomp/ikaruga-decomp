// Post-analysis: type every word in data that points at a function as a pointer to that function's
// signature.
//
// The SDK calls through tables in data: gdFsInit2Ovl calls gdFsInit2G through the word at
// 0x8C0CAD8C, passing its own four arguments on in r4 to r7 and a fifth on the stack. Without a
// type at that word the decompiler guesses the arguments, drops the four it cannot see being set,
// and passes the stack word as the first. With the pointer typed it uses the target's prototype.
// Literal-pool slots are typed the same way by MarkConstantPools.java; this covers the rest of data.
//
// Runs after the prototypes are final (CommitPrototypes, FixReturns).
// Usage (headless): -postScript TypeFunctionPointers.java
//@category Dreamcast

import java.util.HashMap;
import java.util.HashSet;
import java.util.Map;
import java.util.Set;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileOptions;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.address.Address;
import ghidra.program.model.data.DataType;
import ghidra.program.model.data.FunctionDefinitionDataType;
import ghidra.program.model.data.Pointer;
import ghidra.program.model.data.PointerDataType;
import ghidra.program.model.data.Undefined;
import ghidra.program.model.listing.Data;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Listing;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.mem.MemoryBlock;

public class TypeFunctionPointers extends GhidraScript {

    @Override
    public void run() throws Exception {
        Memory mem = currentProgram.getMemory();
        Listing listing = currentProgram.getListing();
        int typed = 0, skipped = 0;
        for (MemoryBlock blk : mem.getBlocks()) {
            if (!blk.isInitialized()) {
                continue;
            }
            long lo = (blk.getStart().getOffset() + 3) & ~3L;
            for (long off = lo; off + 4 <= blk.getEnd().getOffset() + 1; off += 4) {
                if (monitor.isCancelled()) {
                    return;
                }
                Address a = toAddr(off);
                if (listing.getInstructionContaining(a) != null) {
                    continue;
                }
                long v = mem.getInt(a) & 0xFFFFFFFFL;
                if ((v & 1) != 0 || v < 0x8C010000L || v >= 0x8D000000L) {
                    continue;
                }
                Function f = getFunctionAt(toAddr(v));
                if (f == null || f.hasCustomVariableStorage()) {
                    continue;  // a signature type carries no registers; see MarkConstantPools
                }
                Data d = listing.getDataContaining(a);
                if (d != null) {
                    DataType dt = d.getDataType();
                    // Undefined bytes are data of length 1: a table of function pointers that no
                    // mov.l names (Ninja's strip functions, reached by an offset) is all such bytes.
                    boolean replaceable = d.getAddress().equals(a)
                            && (Undefined.isUndefined(dt) || (d.getLength() == 4 && dt instanceof Pointer));
                    if (!replaceable) {
                        skipped++;
                        continue;
                    }
                    listing.clearCodeUnits(a, a.add(3), false);
                }
                try {
                    listing.createData(a, new PointerDataType(new FunctionDefinitionDataType(f, false)));
                    typed++;
                }
                catch (Exception e) {
                    skipped++;
                }
            }
        }
        println("TypeFunctionPointers: " + typed + " words typed as function pointers, " + skipped
                + " left as they were");

        // A typed word that a function reads at another size, or that lies inside larger data,
        // stops that function from decompiling. Such words lose their type again.
        DecompileOptions options = new DecompileOptions();
        options.grabFromProgram(currentProgram);
        DecompInterface di = new DecompInterface();
        di.setOptions(options);
        di.openProgram(currentProgram);
        int reverted = 0, still = 0;
        for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
            if (monitor.isCancelled()) {
                break;
            }
            if (di.decompileFunction(f, 60, monitor).decompileCompleted()) {
                continue;
            }
            Set<Address> words = new HashSet<>();
            for (Instruction i : listing.getInstructions(f.getBody(), true)) {
                for (Reference r : i.getReferencesFrom()) {
                    Data d = listing.getDataAt(r.getToAddress());
                    if (d != null && d.getDataType() instanceof Pointer p
                            && p.getDataType() instanceof ghidra.program.model.data.FunctionDefinition) {
                        words.add(d.getAddress());
                    }
                    // A literal-pool slot holding the address of a typed word.
                    if (d != null && d.getLength() == 4 && mem.getBlock(d.getAddress()) != null
                            && mem.getBlock(d.getAddress()).isInitialized()) {
                        long v = mem.getInt(d.getAddress()) & 0xFFFFFFFFL;
                        Data t = listing.getDataAt(toAddr(v));
                        if (t != null && t.getDataType() instanceof Pointer pp
                                && pp.getDataType() instanceof ghidra.program.model.data.FunctionDefinition) {
                            words.add(t.getAddress());
                        }
                    }
                }
            }
            // Untype them all, then type them again one at a time: only the words that break the
            // decompile stay untyped (njStartVertex's slot kills the decompiler on FUN_8c019cd0,
            // the njEndVertex slot next to it is needed).
            Map<Address, DataType> types = new HashMap<>();
            for (Address w : words) {
                types.put(w, listing.getDataAt(w).getDataType());
                listing.clearCodeUnits(w, w.add(3), false);
            }
            int kept = 0;
            for (Address w : words) {
                listing.createData(w, types.get(w));
                di.dispose();
                di = new DecompInterface();
                di.setOptions(options);
                di.openProgram(currentProgram);
                if (di.decompileFunction(f, 60, monitor).decompileCompleted()) {
                    kept++;
                }
                else {
                    listing.clearCodeUnits(w, w.add(3), false);
                    reverted++;
                }
            }
            println("TypeFunctionPointers: " + f.getName() + " fails with typed words; " + (words.size() - kept)
                    + " of " + words.size() + " untyped again");
            di.dispose();
            di = new DecompInterface();
            di.setOptions(options);
            di.openProgram(currentProgram);
            if (!di.decompileFunction(f, 60, monitor).decompileCompleted()) {
                still++;
                println("TypeFunctionPointers: " + f.getName() + " still fails");
            }
        }
        di.dispose();
        println("TypeFunctionPointers: " + reverted + " words untyped again, " + still + " functions still fail");
    }
}
