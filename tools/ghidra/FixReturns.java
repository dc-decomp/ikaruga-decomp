// Post-analysis: give a return value to every function whose callers use one.
//
// SHC leaves a result in r0 (fr0 for a float) without anything in the function that marks it as
// returned, so the decompiler decides such a function is void: the game's random number generator
// randomNext computes the new value into r0, and 202 call sites read it. The callers, decompiled
// without the callee's prototype, do use r0. This finds each call whose output a caller uses and
// gives the callee a 4-byte return in that register, unless its prototype was written by hand.
// Header prototypes are corrected too: the libraries in the game are newer than the headers. A
// caller of a void header prototype reads the result as extraout_r0, which counts as a use too.
//
// The decompiler also joins r1 to the result when r1 holds something at the return, which makes
// the result 8 bytes in r1:r0. njCnkModelClip returns its 0 or 1 in r0 with a pointer left in r1,
// and the game's modulo helpers mods32 and modu32 leave another value there. Their
// callers then read r0 as extraout_r0, a value the C++ does not have: no model was ever clipped,
// so objects that wait to be off screen were never deleted and drew from freed data. Such a
// result becomes 4 bytes in r0 when no caller reads its upper word.
//
// Usage (headless): -postScript FixReturns.java
//@category Dreamcast

import java.util.HashMap;
import java.util.Map;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileOptions;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.data.DataType;
import ghidra.program.model.data.FloatDataType;
import ghidra.program.model.data.Undefined4DataType;
import ghidra.program.model.data.VoidDataType;
import ghidra.program.model.lang.Register;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.VariableStorage;
import ghidra.program.model.pcode.HighFunction;
import ghidra.program.model.pcode.PcodeOp;
import ghidra.program.model.pcode.PcodeOpAST;
import ghidra.program.model.pcode.Varnode;
import ghidra.program.model.symbol.SourceType;

public class FixReturns extends GhidraScript {

    @Override
    public void run() throws Exception {
        DecompInterface di = new DecompInterface();
        di.setOptions(new DecompileOptions());
        di.setSimplificationStyle("decompile");
        di.openProgram(currentProgram);
        // callee entry -> true for a float result
        Map<Address, Boolean> used = new HashMap<>();
        // callee entry -> true when a caller reads the upper word of an 8-byte result
        Map<Address, Boolean> upper = new HashMap<>();
        for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
            if (monitor.isCancelled()) {
                break;
            }
            DecompileResults r = di.decompileFunction(f, 60, monitor);
            HighFunction hf = r.getHighFunction();
            if (hf == null) {
                continue;
            }
            java.util.Iterator<PcodeOpAST> ops = hf.getPcodeOps();
            while (ops.hasNext()) {
                PcodeOpAST op = ops.next();
                // A read of r0 or fr0 after a call to a function without that result: extraout_r0.
                if (op.getOpcode() == PcodeOp.INDIRECT && op.getOutput().isRegister()
                        && op.getOutput().getDescendants().hasNext()) {
                    PcodeOp call = hf.getOpRef((int) op.getInput(1).getOffset());
                    Register reg = currentProgram.getRegister(op.getOutput());
                    if (reg != null && reg.getName().matches("f?r0") && call != null
                            && call.getOpcode() == PcodeOp.CALL) {
                        used.putIfAbsent(call.getInput(0).getAddress(), reg.getName().equals("fr0"));
                    }
                    continue;
                }
                if (op.getOpcode() != PcodeOp.CALL || op.getOutput() == null) {
                    continue;
                }
                Varnode out = op.getOutput();
                if (out.getDescendants() == null || !out.getDescendants().hasNext()) {
                    continue;
                }
                Address target = op.getInput(0).getAddress();
                if (out.getSize() == 8) {
                    boolean high = false;
                    java.util.Iterator<PcodeOp> uses = out.getDescendants();
                    while (uses.hasNext()) {
                        PcodeOp u = uses.next();
                        high |= u.getOpcode() != PcodeOp.SUBPIECE || u.getInput(1).getOffset() != 0
                                || u.getOutput().getSize() > 4;
                    }
                    upper.merge(target, high, Boolean::logicalOr);
                }
                boolean isFloat = out.isRegister()
                        && currentProgram.getRegister(out).getName().startsWith("fr");
                used.put(target, isFloat);
            }
        }
        di.dispose();

        int fixed = 0;
        for (Map.Entry<Address, Boolean> e : used.entrySet()) {
            Function g = getFunctionAt(e.getKey());
            if (g == null || g.getSignatureSource() == SourceType.USER_DEFINED || g.hasCustomVariableStorage()) {
                continue;
            }
            DataType rt = g.getReturnType();
            if (!(rt instanceof VoidDataType) && rt != DataType.DEFAULT && rt.getLength() >= 4) {
                continue;
            }
            DataType nt = e.getValue() ? FloatDataType.dataType : Undefined4DataType.dataType;
            g.setReturnType(nt, SourceType.ANALYSIS);
            fixed++;
        }
        int narrowed = 0;
        for (Function g : currentProgram.getFunctionManager().getFunctions(true)) {
            Varnode[] v = g.getReturn().getVariableStorage().getVarnodes();
            if (g.getSignatureSource() == SourceType.USER_DEFINED || v.length != 2 || !v[0].isRegister()
                    || !v[1].isRegister() || !currentProgram.getRegister(v[0]).getName().equals("r1")
                    || !currentProgram.getRegister(v[1]).getName().equals("r0")
                    || upper.getOrDefault(g.getEntryPoint(), false)) {
                continue;
            }
            if (g.hasCustomVariableStorage()) {
                g.setReturn(Undefined4DataType.dataType,
                    new VariableStorage(currentProgram, currentProgram.getRegister("r0")), SourceType.ANALYSIS);
            }
            else {
                g.setReturnType(Undefined4DataType.dataType, SourceType.ANALYSIS);
            }
            narrowed++;
        }
        println("FixReturns: " + used.size() + " functions have callers that use a result, "
                + fixed + " got a return value, " + narrowed + " results in r1:r0 narrowed to r0");
    }
}
