// Post-analysis: type a literal-pool slot that holds the start of a typed array or structure as a
// pointer to its element, where every use of it stays inside that data.
//
// The decompiler folds an untyped pool constant into a number, and an access by a variable index at
// the start of an array then prints as *(Task **)(list * 8 + 0x8c15c594). Typed as TaskList *, the
// slot gives taskLists[list].head. A use outside the data, as where SHC takes one global's address
// as the base of its neighbours, would print as an element past the end. Such a slot keeps its plain
// word, and so does a slot in a function that no longer decompiles with it.
// Runs after the last MarkConstantPools.java.
// Usage (headless): -postScript TypeIndexedPools.java
//@category Dreamcast
import java.util.HashMap;
import java.util.HashSet;
import java.util.Iterator;
import java.util.Map;
import java.util.Set;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileOptions;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.data.Array;
import ghidra.program.model.data.DWordDataType;
import ghidra.program.model.data.DataType;
import ghidra.program.model.data.MutabilitySettingsDefinition;
import ghidra.program.model.data.PointerDataType;
import ghidra.program.model.data.Structure;
import ghidra.program.model.listing.Data;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.Listing;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.pcode.HighFunction;
import ghidra.program.model.pcode.PcodeOp;
import ghidra.program.model.pcode.PcodeOpAST;
import ghidra.program.model.pcode.Varnode;

public class TypeIndexedPools extends GhidraScript {

    // A candidate slot: the address that it holds, the element type and the data's size.
    private record Slot(Address at, long value, DataType element, long size) {}

    private Listing lst;

    @Override
    public void run() throws Exception {
        lst = currentProgram.getListing();
        Map<Address, Slot> slots = new HashMap<>();
        Map<Function, Set<Slot>> byFunction = new HashMap<>();
        for (Instruction ins : lst.getInstructions(true)) {
            byte[] b = ins.getBytes();
            int op = b[0] & 0xFF | (b[1] & 0xFF) << 8;
            if ((op & 0xF000) != 0xD000) {             // mov.l @(disp,PC),Rn
                continue;
            }
            Address at = toAddr((ins.getAddress().getOffset() & ~3L) + 4 + (op & 0xFF) * 4L);
            Data d = lst.getDataAt(at);
            Function f = getFunctionContaining(ins.getAddress());
            if (f == null || d == null || !(d.getDataType() instanceof DWordDataType)) {
                continue;
            }
            Slot s = slots.get(at);
            if (s == null) {
                long v = currentProgram.getMemory().getInt(at) & 0xFFFFFFFFL;
                Data target = data(v);
                if (target == null) {
                    continue;
                }
                DataType t = target.getDataType();
                if (t instanceof Array a) {
                    s = new Slot(at, v, a.getDataType(), t.getLength());
                }
                else if (t instanceof Structure) {
                    s = new Slot(at, v, t, t.getLength());
                }
                else {
                    continue;
                }
                slots.put(at, s);
            }
            byFunction.computeIfAbsent(f, k -> new HashSet<>()).add(s);
        }
        for (Slot s : slots.values()) {
            type(s.at(), new PointerDataType(s.element()));
        }

        DecompInterface di = new DecompInterface();
        DecompileOptions o = new DecompileOptions();
        o.grabFromProgram(currentProgram);
        di.setOptions(o);
        di.setSimplificationStyle("decompile");
        di.openProgram(currentProgram);
        Set<Slot> bad = new HashSet<>();
        for (Map.Entry<Function, Set<Slot>> e : byFunction.entrySet()) {
            HighFunction hf = decompile(di, e.getKey());
            if (hf == null) {
                bad.addAll(e.getValue());
                continue;
            }
            for (Slot s : e.getValue()) {
                if (!inside(hf, s)) {
                    bad.add(s);
                }
            }
        }
        for (Slot s : bad) {
            type(s.at(), new DWordDataType());
        }
        // A function that lost some of its typed slots must still decompile with the rest.
        di.dispose();
        di = new DecompInterface();
        di.setOptions(o);
        di.setSimplificationStyle("decompile");
        di.openProgram(currentProgram);
        int reverted = 0;
        for (Map.Entry<Function, Set<Slot>> e : byFunction.entrySet()) {
            if (e.getValue().stream().noneMatch(bad::contains) || decompile(di, e.getKey()) != null) {
                continue;
            }
            for (Slot s : e.getValue()) {
                if (bad.add(s)) {
                    type(s.at(), new DWordDataType());
                    reverted++;
                }
            }
        }
        di.dispose();
        println("TypeIndexedPools: " + (slots.size() - bad.size()) + " of " + slots.size()
            + " slots typed as pointers into their data, " + reverted + " reverted for a failed decompile");
    }

    private Data data(long v) {
        try {
            Address a = toAddr(v);
            MemoryBlock blk = currentProgram.getMemory().getBlock(a);
            return blk == null ? null : lst.getDataAt(a);
        }
        catch (Exception e) {
            return null;
        }
    }

    private void type(Address at, DataType t) throws Exception {
        lst.clearCodeUnits(at, at.add(3), false);
        Data d = lst.createData(at, t);
        MutabilitySettingsDefinition.DEF.setChoice(d, MutabilitySettingsDefinition.CONSTANT);
    }

    private HighFunction decompile(DecompInterface di, Function f) {
        DecompileResults r = di.decompileFunction(f, 60, monitor);
        return r.decompileCompleted() ? r.getHighFunction() : null;
    }

    // Whether every use of the slot's value in the function stays within the data.
    private boolean inside(HighFunction hf, Slot s) {
        for (Iterator<PcodeOpAST> it = hf.getPcodeOps(); it.hasNext();) {
            PcodeOpAST op = it.next();
            for (int i = 0; i < op.getNumInputs(); i++) {
                Varnode in = op.getInput(i);
                if (!in.isConstant() || in.getOffset() != s.value()) {
                    continue;
                }
                boolean symbol = op.getOpcode() == PcodeOp.PTRSUB && i == 1 && op.getInput(0).isConstant()
                    && op.getInput(0).getOffset() == 0;
                boolean ok = symbol ? uses(op.getOutput(), 0, s, new HashSet<>()) : use(op, i, 0, s, new HashSet<>());
                if (!ok) {
                    return false;
                }
            }
        }
        return true;
    }

    private boolean uses(Varnode v, long offset, Slot s, Set<PcodeOp> seen) {
        if (v == null) {
            return true;
        }
        for (Iterator<PcodeOp> it = v.getDescendants(); it.hasNext();) {
            PcodeOp d = it.next();
            for (int i = 0; i < d.getNumInputs(); i++) {
                if (d.getInput(i) == v && !use(d, i, offset, s, seen)) {
                    return false;
                }
            }
        }
        return true;
    }

    // One use of a pointer `offset` bytes into the data. The end of the data counts as inside, for
    // loops that stop there.
    private boolean use(PcodeOp op, int slot, long offset, Slot s, Set<PcodeOp> seen) {
        if (!seen.add(op)) {
            return true;
        }
        if (offset < 0 || offset > s.size()) {
            return false;
        }
        Varnode other = op.getNumInputs() > 1 ? op.getInput(1 - Math.min(slot, 1)) : null;
        switch (op.getOpcode()) {
            case PcodeOp.PTRADD: {
                Varnode index = op.getInput(1);
                if (slot != 0 || !index.isConstant()) {
                    return true;
                }
                return uses(op.getOutput(), offset + index.getOffset() * op.getInput(2).getOffset(), s, seen);
            }
            case PcodeOp.PTRSUB:
                return slot != 0 || !op.getInput(1).isConstant()
                    || uses(op.getOutput(), offset + op.getInput(1).getOffset(), s, seen);
            case PcodeOp.INT_ADD:
                return other == null || !other.isConstant()
                    || uses(op.getOutput(), offset + signed(other), s, seen);
            case PcodeOp.INT_SUB:
                return slot != 0 || !op.getInput(1).isConstant()
                    || uses(op.getOutput(), offset - signed(op.getInput(1)), s, seen);
            case PcodeOp.COPY:
            case PcodeOp.CAST:
                return uses(op.getOutput(), offset, s, seen);
            case PcodeOp.LOAD:
            case PcodeOp.STORE:
                return slot != 1 || offset < s.size();
            default:
                return true;
        }
    }

    private static long signed(Varnode v) {
        return v.getSize() == 4 ? (int) v.getOffset() : v.getOffset();
    }
}
