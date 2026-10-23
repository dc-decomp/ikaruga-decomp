// Post-analysis: type every literal-pool slot that a PC-relative mov.l or mov.w reads, and mark it
// constant.
//
// The image block also holds the program's initialised data, so it has to stay writable, and the
// decompiler then cannot assume a pool word still holds its loaded value. Calls through the pool come
// out as (*(code *)PTR_FUN_x)() and pool constants as global reads. Marking only the pool slots
// constant fixes that without freezing the real globals next to them. A slot that points at a
// function becomes a pointer to that function's signature, so the call resolves to the function by
// name and decompiles with its prototype.
//@category Dreamcast
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.data.DataType;
import ghidra.program.model.data.DWordDataType;
import ghidra.program.model.data.FunctionDefinitionDataType;
import ghidra.program.model.data.MutabilitySettingsDefinition;
import ghidra.program.model.data.PointerDataType;
import ghidra.program.model.data.WordDataType;
import ghidra.program.model.listing.Data;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.listing.Listing;
import ghidra.program.model.mem.MemoryBlock;

public class MarkConstantPools extends GhidraScript {

    @Override
    public void run() throws Exception {
        Listing lst = currentProgram.getListing();
        int marked = 0, pointers = 0, inCode = 0, conflicts = 0;
        InstructionIterator it = lst.getInstructions(true);
        while (it.hasNext() && !monitor.isCancelled()) {
            Instruction ins = it.next();
            byte[] b = ins.getBytes();
            if (b.length != 2) {
                continue;
            }
            int op = (b[0] & 0xFF) | ((b[1] & 0xFF) << 8);
            long pc = ins.getAddress().getOffset();
            Address slot;
            int size;
            if ((op & 0xF000) == 0xD000) {          // mov.l @(disp,PC),Rn
                slot = toAddr((pc & ~3L) + 4 + (op & 0xFF) * 4L);
                size = 4;
            }
            else if ((op & 0xF000) == 0x9000) {     // mov.w @(disp,PC),Rn
                slot = toAddr(pc + 4 + (op & 0xFF) * 2L);
                size = 2;
            }
            else {
                continue;
            }
            MemoryBlock blk = currentProgram.getMemory().getBlock(slot);
            if (blk == null || !blk.isInitialized()) {
                continue;
            }
            // The aggressive instruction finder sometimes decodes pool words as code. Leave those for
            // a later cleanup rather than clear code from under a function.
            if (lst.getInstructionContaining(slot) != null) {
                inCode++;
                continue;
            }
            DataType type = new WordDataType();
            if (size == 4) {
                long v = currentProgram.getMemory().getInt(slot) & 0xFFFFFFFFL;
                MemoryBlock target = currentProgram.getMemory().getBlock(toAddr(v));
                boolean code = target != null && target.isInitialized()
                        && getInstructionAt(toAddr(v)) != null;
                type = code ? new PointerDataType() : new DWordDataType();
                if (code) {
                    pointers++;
                    // SH-4 code calls through the pool with jsr @rN, an indirect call. The decompiler
                    // takes its prototype from the pointer's type, so point at the target's signature.
                    // A function with its arguments in other registers (the SHC runtime's helpers)
                    // keeps an untyped constant: a signature type has no registers, and the call
                    // would get the default ones.
                    Function fn = getFunctionAt(toAddr(v));
                    if (fn != null && !fn.hasCustomVariableStorage()) {
                        type = new PointerDataType(new FunctionDefinitionDataType(fn, false));
                    }
                }
            }
            Data d = lst.getDataAt(slot);
            if (d == null || d.getLength() != size || (d.isDefined() && !d.getDataType().isEquivalent(type))) {
                try {
                    lst.clearCodeUnits(slot, slot.add(size - 1), false);
                    d = lst.createData(slot, type);
                }
                catch (Exception e) {
                    conflicts++;
                    continue;
                }
            }
            MutabilitySettingsDefinition.DEF.setChoice(d, MutabilitySettingsDefinition.CONSTANT);
            marked++;
        }
        println("MarkConstantPools: " + marked + " pool slots constant (" + pointers + " code pointers), "
                + inCode + " slots inside decoded instructions, " + conflicts + " conflicts");
    }
}
