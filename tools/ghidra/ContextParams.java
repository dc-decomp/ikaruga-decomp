// Post-analysis: callee-saved registers that a function reads as inputs become parameters.
//
// SHC calls some local helpers with an object still in a callee-saved register of the caller. The
// stage 1 boss's state machine (FUN_8c08fe04) keeps the boss in r12, and its helpers FUN_8c091180,
// FUN_8c090d14 and FUN_8c090e70 read it there. The decompiler shows such a read as unaff_r12, an
// input without a value, so the C++ helper animated whatever was at address 0, the animation never
// ended, and the boss never came. A function that reads r8 to r14 before writing it gets a
// parameter ctx_rN in that register, with custom storage; its callers then pass what they hold in
// it. A caller that only passes the register on gets the parameter in turn, so the pass repeats.
// Prototypes from the SDK headers and from prototypes.txt are left alone.
//
// Usage (headless): -postScript ContextParams.java
//@category Dreamcast

import java.util.ArrayList;
import java.util.Iterator;
import java.util.List;
import java.util.TreeSet;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileOptions;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.data.Undefined4DataType;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Parameter;
import ghidra.program.model.listing.ParameterImpl;
import ghidra.program.model.listing.ReturnParameterImpl;
import ghidra.program.model.listing.Variable;
import ghidra.program.model.listing.VariableStorage;
import ghidra.program.model.pcode.HighFunction;
import ghidra.program.model.pcode.HighSymbol;
import ghidra.program.model.symbol.SourceType;

public class ContextParams extends GhidraScript {

    private static final Pattern CONTEXT = Pattern.compile("unaff_r(8|9|1[0-4])");

    @Override
    public void run() throws Exception {
        DecompInterface di = new DecompInterface();
        DecompileOptions o = new DecompileOptions();
        o.grabFromProgram(currentProgram);
        di.setOptions(o);
        di.setSimplificationStyle("decompile");
        di.openProgram(currentProgram);
        int changed = 0, reverted = 0;
        for (int round = 0; round < 6; round++) {
            int before = changed;
            for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
                if (monitor.isCancelled()) {
                    break;
                }
                int r = apply(di, f);
                if (r > 0) {
                    changed++;
                }
                else if (r < 0) {
                    reverted++;
                }
            }
            println("ContextParams: round " + (round + 1) + ", " + (changed - before) + " functions changed");
            if (changed == before) {
                break;
            }
        }
        di.dispose();
        println("ContextParams: " + changed + " functions given the callee-saved registers they read, " + reverted
            + " left as they were");
    }

    // 1 if the function got parameters, -1 if they broke its decompilation, 0 if it needs none.
    private int apply(DecompInterface di, Function f) throws Exception {
        SourceType src = f.getSignatureSource();
        if (src == SourceType.USER_DEFINED || src == SourceType.IMPORTED) {
            return 0;
        }
        DecompileResults r = di.decompileFunction(f, 60, monitor);
        HighFunction hf = r.getHighFunction();
        if (hf == null) {
            return 0;
        }
        TreeSet<Integer> regs = new TreeSet<>();
        Iterator<HighSymbol> it = hf.getLocalSymbolMap().getSymbols();
        while (it.hasNext()) {
            Matcher m = CONTEXT.matcher(it.next().getName());
            if (m.matches()) {
                regs.add(Integer.parseInt(m.group(1)));
            }
        }
        if (regs.isEmpty()) {
            return 0;
        }
        List<Variable> old = new ArrayList<>();
        List<Variable> params = new ArrayList<>();
        for (Parameter p : f.getParameters()) {
            old.add(new ParameterImpl(p.getName(), p.getDataType(), p.getVariableStorage(), currentProgram));
            params.add(new ParameterImpl(p.getName(), p.getDataType(), p.getVariableStorage(), currentProgram));
            if (p.getRegister() != null && p.getRegister().getName().matches("r(8|9|1[0-4])")) {
                regs.remove(Integer.parseInt(p.getRegister().getName().substring(1)));
            }
        }
        if (regs.isEmpty()) {
            return 0;
        }
        for (int n : regs) {
            VariableStorage s = new VariableStorage(currentProgram, currentProgram.getRegister("r" + n));
            params.add(new ParameterImpl("ctx_r" + n, Undefined4DataType.dataType, s, currentProgram));
        }
        boolean wasCustom = f.hasCustomVariableStorage();
        SourceType oldSource = f.getSignatureSource();
        ReturnParameterImpl ret = new ReturnParameterImpl(f.getReturnType(), f.getReturn().getVariableStorage(),
            currentProgram);
        f.updateFunction(f.getCallingConventionName(), ret, params, Function.FunctionUpdateType.CUSTOM_STORAGE,
            true, SourceType.ANALYSIS);
        if (!di.decompileFunction(f, 60, monitor).decompileCompleted()) {
            f.updateFunction(f.getCallingConventionName(), ret, old, wasCustom
                ? Function.FunctionUpdateType.CUSTOM_STORAGE
                : Function.FunctionUpdateType.DYNAMIC_STORAGE_ALL_PARAMS, true, oldSource);
            return -1;
        }
        return 1;
    }
}
