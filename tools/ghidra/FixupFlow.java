// Post-analysis: repair three things that stop functions from decompiling.
//
// 1. A call target typed as data. A constant pool next to a function can be read as an 8-byte value
//    whose upper half is the first instruction of that function, and the function is then lost.
//    Every call reference to bytes that are not code is cleared, disassembled and made a function.
// 2. A computed jump to memory that holds no bytes. The cache functions jump to their own code through
//    the uncached P2 alias (0xAC0...), syBtExit to the boot ROM's 0x8C008300. Ghidra 12.2 follows
//    such a jump, finds no instruction and truncates the function with halt_baddata. These jumps
//    become call-returns, so the decompiler writes a tail call to the target, as 12.1.4 did.
// 3. Flow overrides that kill the decompiler. Analysis marks a `jmp` to a constant as a call-return.
//    Together with a jump-table override in the same function the decompiler process dies. Each
//    function is decompiled once, and where the process dies its flow overrides are cleared.
//
// Usage (headless): -postScript FixupFlow.java
//@category Dreamcast

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileOptions;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressSetView;
import ghidra.program.model.listing.Data;
import ghidra.program.model.listing.FlowOverride;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.Listing;
import ghidra.program.model.symbol.Reference;

public class FixupFlow extends GhidraScript {

    @Override
    public void run() throws Exception {
        Listing listing = currentProgram.getListing();
        int recovered = 0;
        for (Instruction i : listing.getInstructions(true)) {
            for (Reference r : i.getReferencesFrom()) {
                if (!r.getReferenceType().isCall()) {
                    continue;
                }
                Address t = r.getToAddress();
                if (listing.getInstructionAt(t) != null || !currentProgram.getMemory().contains(t)
                        || (t.getOffset() & 1) != 0) {
                    continue;
                }
                Data d = listing.getDataContaining(t);
                if (d != null) {
                    listing.clearCodeUnits(d.getAddress(), d.getMaxAddress(), false);
                }
                if (disassemble(t) && getFunctionAt(t) == null && createFunction(t, null) != null) {
                    recovered++;
                }
            }
        }
        analyzeChanges(currentProgram);

        AddressSetView loaded = currentProgram.getMemory().getLoadedAndInitializedAddressSet();
        int tails = 0;
        for (Instruction i : listing.getInstructions(true)) {
            if (!i.getFlowType().isComputed() || !i.getFlowType().isJump()
                    || i.getFlowOverride() != FlowOverride.NONE) {
                continue;
            }
            for (Reference r : i.getReferencesFrom()) {
                if (r.getReferenceType().isFlow() && !loaded.contains(r.getToAddress())) {
                    i.setFlowOverride(FlowOverride.CALL_RETURN);
                    tails++;
                    break;
                }
            }
        }

        int cleared = 0, still = 0;
        DecompInterface di = new DecompInterface();
        di.setOptions(new DecompileOptions());
        di.openProgram(currentProgram);
        for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
            if (monitor.isCancelled()) {
                break;
            }
            if (!died(di.decompileFunction(f, 60, monitor))) {
                continue;
            }
            di.dispose();
            di = new DecompInterface();
            di.setOptions(new DecompileOptions());
            di.openProgram(currentProgram);
            for (Instruction i : listing.getInstructions(f.getBody(), true)) {
                if (i.getFlowOverride() != FlowOverride.NONE) {
                    i.setFlowOverride(FlowOverride.NONE);
                }
            }
            if (died(di.decompileFunction(f, 60, monitor))) {
                still++;
                println("FixupFlow: the decompiler still dies on " + f.getName());
                di.dispose();
                di = new DecompInterface();
                di.setOptions(new DecompileOptions());
                di.openProgram(currentProgram);
            }
            else {
                cleared++;
            }
        }
        di.dispose();
        println("FixupFlow: " + recovered + " functions recovered from data, " + tails
                + " jumps to memory without bytes made call-returns, flow overrides cleared in " + cleared
                + " functions, " + still + " still fail");
    }

    private static boolean died(DecompileResults r) {
        return !r.decompileCompleted() && String.valueOf(r.getErrorMessage()).contains("died");
    }
}
