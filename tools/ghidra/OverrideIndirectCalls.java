// Post-analysis: give every call through a pointer whose target is unknown the full set of argument
// registers, r4 to r7 and fr4 to fr7, as a call-site override.
//
// Without a prototype the decompiler guesses a call's arguments from which registers the caller
// sets. A register the caller passes on unchanged is invisible to that guess: gdFsReqRd32 calls the
// GD driver's read through the table in its work area with the buffer still in r6 from its own
// arguments, and the C dropped the buffer and shifted the rest. With every argument register passed,
// each callee reads the ones it takes, as on the console. Calls whose target Ghidra knows (a typed
// pointer, a literal-pool slot) are left alone.
//
// A call of a variadic function gets the four word registers and the stack words that its format
// takes, or eight where the format is not a constant. SHC passes sprintf's format and values on
// the stack, and with sprintf's own prototype the decompiler took those words as arguments at
// some calls only: FUN_8c07235c printed its countdown with sprintf(buffer, r5) and no format, so
// the text was garbage.
// Usage (headless): -postScript OverrideIndirectCalls.java [indirect|variadic]
//@category Dreamcast

import java.util.Iterator;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.data.DataType;
import ghidra.program.model.data.FloatDataType;
import ghidra.program.model.data.FunctionDefinitionDataType;
import ghidra.program.model.data.ParameterDefinition;
import ghidra.program.model.data.ParameterDefinitionImpl;
import ghidra.program.model.data.Undefined4DataType;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.pcode.HighFunctionDBUtil;
import ghidra.program.model.symbol.Reference;

public class OverrideIndirectCalls extends GhidraScript {

    @Override
    public void run() throws Exception {
        FunctionDefinitionDataType sig = new FunctionDefinitionDataType("indirect_call");
        DataType u4 = Undefined4DataType.dataType, f4 = FloatDataType.dataType;
        sig.setArguments(new ParameterDefinition[] {
            new ParameterDefinitionImpl("r4", u4, null), new ParameterDefinitionImpl("r5", u4, null),
            new ParameterDefinitionImpl("r6", u4, null), new ParameterDefinitionImpl("r7", u4, null),
            new ParameterDefinitionImpl("fr4", f4, null), new ParameterDefinitionImpl("fr5", f4, null),
            new ParameterDefinitionImpl("fr6", f4, null), new ParameterDefinitionImpl("fr7", f4, null) });
        sig.setReturnType(u4);

        // "indirect" or "variadic" does one half: the calls through unknown pointers before
        // InputParams, which then sees the registers a function passes on through them, and the
        // variadic calls after it, whose leftover r5 to r7 are no one's arguments.
        String only = getScriptArgs().length > 0 ? getScriptArgs()[0] : "";
        boolean doIndirect = !only.equals("variadic"), doVariadic = !only.equals("indirect");
        int overridden = 0, known = 0, variadic = 0;
        java.util.Map<Function, java.util.List<Instruction>> variadicCalls = new java.util.LinkedHashMap<>();
        for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
            if (monitor.isCancelled()) {
                return;
            }
            for (Instruction i : currentProgram.getListing().getInstructions(f.getBody(), true)) {
                String m = i.getMnemonicString();
                if (m.equals("bsr") || m.equals("jmp")) {
                    Function callee = target(i);
                    if (doVariadic && callee != null && callee.hasVarArgs()) {
                        HighFunctionDBUtil.writeOverride(f, i.getAddress(), withStackWords(callee, 8));
                        variadicCalls.computeIfAbsent(f, k -> new java.util.ArrayList<>()).add(i);
                        variadic++;
                    }
                    continue;
                }
                if (!m.equals("jsr")) {
                    continue;
                }
                Function callee = target(i);
                if (callee != null && callee.hasVarArgs()) {
                    if (doVariadic) {
                        HighFunctionDBUtil.writeOverride(f, i.getAddress(), withStackWords(callee, 8));
                        variadicCalls.computeIfAbsent(f, k -> new java.util.ArrayList<>()).add(i);
                        variadic++;
                    }
                    continue;
                }
                boolean target = false;
                for (Reference r : i.getReferencesFrom()) {
                    if (r.getReferenceType().isCall() && getFunctionAt(r.getToAddress()) != null) {
                        target = true;
                    }
                }
                if (target || loadedFromPool(i)) {
                    known++;
                    continue;
                }
                if (doIndirect) {
                    HighFunctionDBUtil.writeOverride(f, i.getAddress(), sig);
                    overridden++;
                }
            }
        }
        int exact = fitFormats(variadicCalls);
        println("OverrideIndirectCalls: " + overridden + " calls through unknown pointers given all "
                + "argument registers, " + known + " with a known target left alone, " + variadic
                + " calls of variadic functions given stack words, " + exact
                + " of them as many as their format takes");
    }

    // A call whose format is a constant string gets the stack words that the format takes. Eight
    // words at every call cover the caller's own locals above the arguments: FUN_8c071418 writes
    // the CONTINUE text's position there after its sprintf, and the decompiler dropped the writes.
    private int fitFormats(java.util.Map<Function, java.util.List<Instruction>> calls) throws Exception {
        ghidra.app.decompiler.DecompInterface di = new ghidra.app.decompiler.DecompInterface();
        ghidra.app.decompiler.DecompileOptions o = new ghidra.app.decompiler.DecompileOptions();
        o.grabFromProgram(currentProgram);
        di.setOptions(o);
        di.setSimplificationStyle("decompile");
        di.openProgram(currentProgram);
        int exact = 0;
        for (java.util.Map.Entry<Function, java.util.List<Instruction>> e : calls.entrySet()) {
            ghidra.program.model.pcode.HighFunction hf = di.decompileFunction(e.getKey(), 60, monitor).getHighFunction();
            if (hf == null) {
                continue;
            }
            for (Instruction call : e.getValue()) {
                Function callee = target(call);
                Iterator<ghidra.program.model.pcode.PcodeOpAST> ops = hf.getPcodeOps(call.getAddress());
                while (ops.hasNext()) {
                    ghidra.program.model.pcode.PcodeOpAST op = ops.next();
                    int code = op.getOpcode();
                    if ((code != ghidra.program.model.pcode.PcodeOp.CALL
                            && code != ghidra.program.model.pcode.PcodeOp.CALLIND) || op.getNumInputs() < 6) {
                        continue;
                    }
                    // Input 0 is the target, 1 to 4 are r4 to r7, 5 is the first stack word.
                    String format = constantString(op.getInput(5));
                    if (format != null) {
                        HighFunctionDBUtil.writeOverride(e.getKey(), call.getAddress(),
                            withStackWords(callee, 1 + conversions(format)));
                        exact++;
                    }
                }
            }
        }
        di.dispose();
        return exact;
    }

    // The string a constant pointer or a pointer to a labelled string names.
    private String constantString(ghidra.program.model.pcode.Varnode v) {
        long a;
        if (v.isConstant()) {
            a = v.getOffset();
        }
        else if (v.getDef() != null && v.getDef().getOpcode() == ghidra.program.model.pcode.PcodeOp.PTRSUB
                && v.getDef().getInput(1).isConstant()) {
            a = v.getDef().getInput(1).getOffset();
        }
        else if (v.getDef() != null && v.getDef().getOpcode() == ghidra.program.model.pcode.PcodeOp.COPY
                && v.getDef().getInput(0).isConstant()) {
            a = v.getDef().getInput(0).getOffset();
        }
        else {
            return null;
        }
        try {
            ghidra.program.model.address.Address at = currentProgram.getAddressFactory()
                .getDefaultAddressSpace().getAddress(a & 0xFFFFFFFFL);
            StringBuilder b = new StringBuilder();
            for (int k = 0; k < 256; k++) {
                byte c = currentProgram.getMemory().getByte(at.add(k));
                if (c == 0) {
                    return b.toString();
                }
                b.append((char) (c & 0xFF));
            }
        }
        catch (Exception ex) {
            return null;
        }
        return null;
    }

    // The words a printf format takes after itself: one per conversion and per * width.
    private static int conversions(String f) {
        int n = 0;
        for (int i = 0; i < f.length(); i++) {
            if (f.charAt(i) != '%') {
                continue;
            }
            i++;
            if (i < f.length() && f.charAt(i) == '%') {
                continue;
            }
            while (i < f.length() && "-+ #0123456789.*hlL".indexOf(f.charAt(i)) >= 0) {
                if (f.charAt(i) == '*') {
                    n++;
                }
                i++;
            }
            n++;
        }
        return n;
    }

    // A variadic function's named parameters, filled up to r7, then `words` stack words.
    private FunctionDefinitionDataType withStackWords(Function callee, int words) {
        FunctionDefinitionDataType sig = new FunctionDefinitionDataType(callee.getName() + "_words");
        DataType u4 = Undefined4DataType.dataType;
        java.util.List<ParameterDefinition> ps = new java.util.ArrayList<>();
        for (ghidra.program.model.listing.Parameter p : callee.getParameters()) {
            ps.add(new ParameterDefinitionImpl(p.getName(), p.getDataType(), null));
        }
        while (ps.size() < 4) {
            ps.add(new ParameterDefinitionImpl("r" + (4 + ps.size()), u4, null));
        }
        for (int k = 0; k < words; k++) {
            ps.add(new ParameterDefinitionImpl("stack" + k, u4, null));
        }
        sig.setArguments(ps.toArray(new ParameterDefinition[0]));
        sig.setReturnType(callee.getReturnType());
        return sig;
    }

    // The function a call reaches: by a call reference, or through a literal-pool slot.
    private Function target(Instruction call) {
        for (Reference r : call.getReferencesFrom()) {
            if (r.getReferenceType().isCall() && getFunctionAt(r.getToAddress()) != null) {
                return getFunctionAt(r.getToAddress());
            }
        }
        Object[] ops = call.getOpObjects(0);
        if (ops.length == 0 || !(ops[0] instanceof ghidra.program.model.lang.Register reg)) {
            return null;
        }
        Instruction p = call;
        for (int n = 0; n < 8; n++) {
            p = p.getPrevious();
            if (p == null) {
                return null;
            }
            boolean writes = false;
            for (Object o : p.getResultObjects()) {
                if (o.equals(reg)) {
                    writes = true;
                }
            }
            if (!writes) {
                continue;
            }
            for (Reference r : p.getReferencesFrom()) {
                if (r.getReferenceType().isData()) {
                    try {
                        long v = currentProgram.getMemory().getInt(r.getToAddress()) & 0xFFFFFFFFL;
                        return getFunctionAt(r.getToAddress().getNewAddress(v));
                    }
                    catch (Exception e) {
                        return null;
                    }
                }
            }
            return null;
        }
        return null;
    }

    // jsr @rN with rN loaded from a literal-pool slot shortly before: the slot's type gives the
    // target's prototype already.
    private boolean loadedFromPool(Instruction jsr) {
        Object[] ops = jsr.getOpObjects(0);
        if (ops.length == 0 || !(ops[0] instanceof ghidra.program.model.lang.Register reg)) {
            return false;
        }
        Instruction p = jsr;
        for (int n = 0; n < 8; n++) {
            p = p.getPrevious();
            if (p == null) {
                return false;
            }
            Object[] res = p.getResultObjects();
            boolean writes = false;
            for (Object o : res) {
                if (o.equals(reg)) {
                    writes = true;
                }
            }
            if (!writes) {
                continue;
            }
            for (Reference r : p.getReferencesFrom()) {
                if (r.getReferenceType().isData()) {
                    return true;  // mov.l @(disp,pc),rN
                }
            }
            return false;
        }
        return false;
    }
}
