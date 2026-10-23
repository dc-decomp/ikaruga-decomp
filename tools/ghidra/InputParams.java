// Post-analysis: argument registers that a function reads without a parameter for them.
//
// Katana code moves two floats at once with fmov.d, so a function that takes x and y in fr4 and
// fr5 can read them as dr4. The calling convention has no pairs, so the decompiler shows that read
// as an unknown input, in_dr4, and every caller drops the two arguments: the game's screen
// position helper FUN_8c06b02c lost its x and y. Dispatchers do the same with single registers:
// FUN_8c068da0 passes r7 and fr4 to fr7 on to a draw function from a table, and showed them as
// in_r7 and in_fr4, so every sprite it drew lost its position.
//
// A function with an unknown input in r5 to r7, fr4 to fr11, or a pair dr4 to dr10 gets parameters
// up to the highest such register, words for r and floats for fr, after the ones it has. Its
// callers then pass what they hold in those registers, and a caller that only passes them on
// shows them as unknown inputs of its own, so the pass repeats until nothing changes. Prototypes
// from the SDK headers and from prototypes.txt are left alone.
//
// It runs twice, around TypeFunctionPointers: a call through a literal-pool slot passes the
// arguments of the slot's type, which that pass sets. Before it, FUN_8c082918 passed no floats on
// to the function in its slot, so its own fr4 to fr6 showed as no input at all.
//
// Usage (headless): -postScript InputParams.java
//@category Dreamcast

import java.util.ArrayList;
import java.util.Iterator;
import java.util.List;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileOptions;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.data.AbstractFloatDataType;
import ghidra.program.model.data.FloatDataType;
import ghidra.program.model.data.Undefined4DataType;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Parameter;
import ghidra.program.model.listing.ParameterImpl;
import ghidra.program.model.listing.ReturnParameterImpl;
import ghidra.program.model.listing.Variable;
import ghidra.program.model.pcode.HighFunction;
import ghidra.program.model.pcode.HighSymbol;
import ghidra.program.model.symbol.SourceType;

public class InputParams extends GhidraScript {

    private static final Pattern INPUT = Pattern.compile("in_(r|fr|dr)(\\d+)");

    @Override
    public void run() throws Exception {
        DecompInterface di = new DecompInterface();
        DecompileOptions o = new DecompileOptions();
        o.grabFromProgram(currentProgram);
        di.setOptions(o);
        di.setSimplificationStyle("decompile");
        di.openProgram(currentProgram);
        int changed = 0, reverted = 0;
        for (int round = 0; round < 4; round++) {
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
            println("InputParams: round " + (round + 1) + ", " + (changed - before) + " functions changed");
            if (changed == before) {
                break;
            }
        }
        di.dispose();
        println("InputParams: " + changed + " functions given the parameters they read, " + reverted
            + " left as they were");
    }

    // 1 if the function got parameters, -1 if they broke its decompilation, 0 if it needs none.
    private int apply(DecompInterface di, Function f) throws Exception {
        SourceType src = f.getSignatureSource();
        if (f.hasCustomVariableStorage() || src == SourceType.USER_DEFINED || src == SourceType.IMPORTED) {
            return 0;
        }
        DecompileResults r = di.decompileFunction(f, 60, monitor);
        HighFunction hf = r.getHighFunction();
        if (hf == null) {
            return 0;
        }
        // The number of word and float argument registers the function reads.
        int neededInts = 0, neededFloats = 0;
        Iterator<HighSymbol> it = hf.getLocalSymbolMap().getSymbols();
        while (it.hasNext()) {
            Matcher m = INPUT.matcher(it.next().getName());
            if (!m.matches()) {
                continue;
            }
            int n = Integer.parseInt(m.group(2));
            if (m.group(1).equals("r") && n >= 4 && n <= 7) {
                neededInts = Math.max(neededInts, n - 3);
            }
            else if (m.group(1).equals("fr") && n >= 4 && n <= 11) {
                neededFloats = Math.max(neededFloats, n - 3);
            }
            else if (m.group(1).equals("dr") && n >= 4 && n <= 10 && n % 2 == 0) {
                neededFloats = Math.max(neededFloats, n - 2);
            }
        }
        if (neededInts == 0 && neededFloats == 0) {
            return 0;
        }
        List<Variable> old = new ArrayList<>();
        int ints = 0, floats = 0;
        for (Parameter p : f.getParameters()) {
            old.add(new ParameterImpl(p.getName(), p.getDataType(), currentProgram));
            if (p.getDataType() instanceof AbstractFloatDataType) {
                floats++;
            }
            else {
                ints += p.getLength() > 4 ? 2 : 1;
            }
        }
        if (ints >= neededInts && floats >= neededFloats) {
            return 0;
        }
        List<Variable> params = new ArrayList<>(old);
        for (int k = ints; k < neededInts; k++) {
            params.add(new ParameterImpl("arg_r" + (4 + k), Undefined4DataType.dataType, currentProgram));
        }
        for (int k = floats; k < neededFloats; k++) {
            params.add(new ParameterImpl("arg_fr" + (4 + k), new FloatDataType(), currentProgram));
        }
        SourceType oldSource = f.getSignatureSource();
        update(f, params, SourceType.ANALYSIS);
        // A pair read can also be a write of one half that keeps the other: then the parameters
        // overlap the pair, and the decompiler fails. Such a function stays as it was.
        DecompileResults again = di.decompileFunction(f, 60, monitor);
        if (!again.decompileCompleted()) {
            update(f, old, oldSource);
            return -1;
        }
        return 1;
    }

    private void update(Function f, List<Variable> params, SourceType source) throws Exception {
        f.updateFunction(f.getCallingConventionName(),
            new ReturnParameterImpl(f.getReturnType(), currentProgram), params,
            Function.FunctionUpdateType.DYNAMIC_STORAGE_ALL_PARAMS, true, source);
    }
}
