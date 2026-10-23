// Post-analysis: a function that leaves r1, r2 or r3 alone keeps them for its callers.
//
// SHC knows which registers a function in the same unit writes, and a caller keeps a value in r1 to
// r3 across a call to a function that leaves it alone. FUN_8c09b634 keeps the object it created in
// r3 while FUN_8c09b60c fills it in, then returns r3. The calling convention kills r0 to r3 at every
// call, so the decompiler read the result as extraout_r3, a value the C++ does not have. This finds
// each register of r1 to r3 that a caller reads after a call, checks that the callee and everything
// it calls never write it, or push it on the stack and pop it again as the SHC runtime's __divls
// does with r2 and r3, and gives the callee the calling convention __keep_r1 ... __keep_r1r2r3
// (tools/ghidra/build_language.py) for the registers it keeps. It runs after the last
// MarkConstantPools: before that, the literal pool after FUN_8c09b60c decoded as a bsr, and an
// unknown callee may write every register.
//
// Usage (headless): -postScript KeptRegisters.java
//@category Dreamcast

import java.util.HashMap;
import java.util.HashSet;
import java.util.Iterator;
import java.util.Map;
import java.util.Set;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileOptions;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.lang.Register;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.pcode.HighFunction;
import ghidra.program.model.pcode.PcodeOp;
import ghidra.program.model.pcode.PcodeOpAST;
import ghidra.program.model.pcode.Varnode;
import ghidra.program.model.symbol.FlowType;

public class KeptRegisters extends GhidraScript {

    private static final String[] KEPT = { "r1", "r2", "r3" };
    private static final int ALL = (1 << KEPT.length) - 1;
    private static final Pattern SAVE = Pattern.compile("_?mov\\.l (r[123]),@-r15|_?mov\\.l @r15\\+,(r[123])");

    // function -> bit i set when it or a function it calls writes KEPT[i]
    private final Map<Function, Integer> writes = new HashMap<>();

    @Override
    public void run() throws Exception {
        DecompInterface di = new DecompInterface();
        DecompileOptions o = new DecompileOptions();
        o.grabFromProgram(currentProgram);
        di.setOptions(o);
        di.setSimplificationStyle("decompile");
        di.openProgram(currentProgram);
        // callee -> the registers of KEPT that a caller reads after calling it
        Map<Function, Integer> read = new HashMap<>();
        for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
            if (monitor.isCancelled()) {
                break;
            }
            DecompileResults r = di.decompileFunction(f, 60, monitor);
            HighFunction hf = r.getHighFunction();
            if (hf == null) {
                continue;
            }
            Iterator<PcodeOpAST> ops = hf.getPcodeOps();
            while (ops.hasNext()) {
                PcodeOpAST op = ops.next();
                Varnode out = op.getOutput();
                if (op.getOpcode() != PcodeOp.INDIRECT || !out.isRegister() || !out.getDescendants().hasNext()) {
                    continue;
                }
                int bit = bit(currentProgram.getRegister(out));
                PcodeOp call = hf.getOpRef((int) op.getInput(1).getOffset());
                if (bit == 0 || call == null || call.getOpcode() != PcodeOp.CALL) {
                    continue;
                }
                Function g = getFunctionAt(call.getInput(0).getAddress());
                if (g != null) {
                    read.merge(g, bit, (a, b) -> a | b);
                }
            }
        }
        di.dispose();

        int kept = 0, written = 0;
        for (Map.Entry<Function, Integer> e : read.entrySet()) {
            Function g = e.getKey();
            int keep = ALL & ~writes(g, new HashSet<>());
            if ((keep & e.getValue()) == 0) {
                written++;
                continue;
            }
            StringBuilder name = new StringBuilder("__keep_");
            for (int i = 0; i < KEPT.length; i++) {
                if ((keep & (1 << i)) != 0) {
                    name.append(KEPT[i]);
                }
            }
            g.setCallingConvention(name.toString());
            kept++;
        }
        println("KeptRegisters: " + kept + " functions keep r1 to r3 for their callers, " + written
            + " write the register their callers read after them");
    }

    private static int bit(Register reg) {
        for (int i = 0; reg != null && i < KEPT.length; i++) {
            if (reg.getName().equals(KEPT[i])) {
                return 1 << i;
            }
        }
        return 0;
    }

    // The registers of KEPT that f, or a function it calls or jumps to, may write. A call or jump
    // whose target is unknown may write all of them.
    private int writes(Function f, Set<Function> active) {
        Integer known = writes.get(f);
        if (known != null) {
            return known;
        }
        if (!active.add(f)) {
            return ALL;
        }
        int w = 0, pushed = 0, popped = 0;
        for (Instruction in : currentProgram.getListing().getInstructions(f.getBody(), true)) {
            Matcher m = SAVE.matcher(in.toString());
            if (m.matches()) {
                int b = bit(currentProgram.getRegister(m.group(1) != null ? m.group(1) : m.group(2)));
                if (m.group(1) != null) {
                    pushed |= b;
                }
                else {
                    popped |= b;
                }
            }
            for (PcodeOp op : in.getPcode()) {
                Varnode out = op.getOutput();
                if (out != null && out.isRegister()) {
                    w |= bit(currentProgram.getRegister(out.getAddress(), out.getSize()));
                }
            }
            FlowType flow = in.getFlowType();
            if (flow.isCall() || flow.isJump()) {
                Address[] targets = in.getFlows();
                if (targets.length == 0 && (flow.isCall() || flow.isComputed())) {
                    w = ALL;
                }
                for (Address t : targets) {
                    if (f.getBody().contains(t)) {
                        continue;
                    }
                    Function h = getFunctionAt(t);
                    w |= h == null ? ALL : writes(h, active);
                }
            }
        }
        w &= ~(pushed & popped);
        active.remove(f);
        writes.put(f, w);
        return w;
    }
}
