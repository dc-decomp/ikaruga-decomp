// Post-analysis: prototypes with custom storage in the argument registers become ordinary ones.
//
// The decompiler commits the registers a function reads. njCnkSimpleTextureDbNearClip reads fr10
// and fr11 and r4 to r7, so it got a prototype with custom storage: its first two parameters in
// fr10 and fr11. The pool slots that point at such a function stay untyped (a signature type has no
// registers), and every call through them passes only the arguments the decompiler sees set: the
// vertex pointer in r7 was lost, and Ninja wrote its vertices to address 0.
//
// fr10 and fr11 are the seventh and eighth float arguments of the calling convention. A prototype
// with parameters for every argument register up to the highest one used, ints in r4 to r7 and
// floats in fr4 to fr11, puts each parameter where the function reads it, with ordinary storage.
// Words on the stack stay after r7, where the convention puts them: FUN_8c080578 takes a float in
// fr6, four words and a stack word, and its callers passed the float as an integer word.
// FUN_8c082918 read r4 and two stack words and passed r5 to r7 on unread; with only r4 and the
// stack words as parameters, every caller dropped the parent object in r5, and the task
// it made read its parent at address -23. A function with stack words gets all four word
// registers. A parameter without a register the function reads is named arg_rN or arg_frN.
// Functions with a register outside these (the SHC runtime's helpers) keep their storage.
//
// Usage (headless): -postScript NormalizeStorage.java
//@category Dreamcast

import java.util.ArrayList;
import java.util.List;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileOptions;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.data.AbstractFloatDataType;
import ghidra.program.model.data.FloatDataType;
import ghidra.program.model.data.Undefined4DataType;
import ghidra.program.model.lang.Register;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Parameter;
import ghidra.program.model.listing.ParameterImpl;
import ghidra.program.model.listing.ReturnParameterImpl;
import ghidra.program.model.listing.Variable;
import ghidra.program.model.symbol.SourceType;

public class NormalizeStorage extends GhidraScript {

    @Override
    public void run() throws Exception {
        DecompInterface di = new DecompInterface();
        DecompileOptions o = new DecompileOptions();
        o.grabFromProgram(currentProgram);
        di.setOptions(o);
        di.openProgram(currentProgram);
        int changed = 0, kept = 0, reverted = 0;
        for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
            if (monitor.isCancelled()) {
                break;
            }
            if (!f.hasCustomVariableStorage() || f.getSignatureSource() == SourceType.USER_DEFINED) {
                continue;
            }
            Parameter[] ints = new Parameter[4];
            Parameter[] floats = new Parameter[8];
            List<Parameter> stack = new ArrayList<>();
            boolean ordinary = f.getReturn().getRegister() == null
                    || f.getReturn().getRegister().getName().matches("r0|fr0");
            for (Parameter p : f.getParameters()) {
                if (p.isStackVariable() && p.getVariableStorage().getVarnodeCount() == 1 && p.getLength() <= 4
                        && p.getStackOffset() == 4 * stack.size()) {
                    stack.add(p);  // the words after r7, in order
                    continue;
                }
                Register r = p.getRegister();
                if (r == null || p.getVariableStorage().getVarnodeCount() != 1) {
                    ordinary = false;
                    break;
                }
                String n = r.getName();
                if (n.matches("r[4-7]") && p.getLength() <= 4) {
                    ints[Integer.parseInt(n.substring(1)) - 4] = p;
                }
                else if (n.matches("fr([4-9]|1[01])") && p.getLength() == 4) {
                    floats[Integer.parseInt(n.substring(2)) - 4] = p;
                }
                else {
                    ordinary = false;
                    break;
                }
            }
            if (!ordinary) {
                kept++;
                continue;
            }
            List<Variable> old = new ArrayList<>();
            for (Parameter p : f.getParameters()) {
                old.add(p);
            }
            List<Variable> params = new ArrayList<>();
            // Stack words come after all four word registers, so a function with stack words gets
            // a parameter for each of them.
            int lastInt = stack.isEmpty() ? -1 : 3, lastFloat = -1;
            for (int i = 0; i < 4; i++) {
                if (ints[i] != null) {
                    lastInt = Math.max(lastInt, i);
                }
            }
            for (int i = 0; i < 8; i++) {
                if (floats[i] != null) {
                    lastFloat = i;
                }
            }
            // Dynamic storage places a parameter by its type, so a word in a float register is a float.
            for (int i = 0; i <= lastInt; i++) {
                params.add(ints[i] != null && !(ints[i].getDataType() instanceof AbstractFloatDataType)
                    ? new ParameterImpl(ints[i].getName(), ints[i].getDataType(), currentProgram)
                    : new ParameterImpl(ints[i] != null ? ints[i].getName() : "arg_r" + (4 + i),
                        Undefined4DataType.dataType, currentProgram));
            }
            for (Parameter p : stack) {
                params.add(new ParameterImpl(p.getName(), p.getDataType(), currentProgram));
            }
            for (int i = 0; i <= lastFloat; i++) {
                params.add(floats[i] != null && floats[i].getDataType() instanceof AbstractFloatDataType
                    ? new ParameterImpl(floats[i].getName(), floats[i].getDataType(), currentProgram)
                    : new ParameterImpl(floats[i] != null ? floats[i].getName() : "arg_fr" + (4 + i),
                        FloatDataType.dataType, currentProgram));
            }
            ReturnParameterImpl ret = new ReturnParameterImpl(f.getReturnType(), currentProgram);
            f.updateFunction(f.getCallingConventionName(), ret, params,
                Function.FunctionUpdateType.DYNAMIC_STORAGE_ALL_PARAMS, true, SourceType.ANALYSIS);
            if (!di.decompileFunction(f, 60, monitor).decompileCompleted()) {
                f.updateFunction(f.getCallingConventionName(), ret, old,
                    Function.FunctionUpdateType.CUSTOM_STORAGE, true, SourceType.ANALYSIS);
                reverted++;
                continue;
            }
            changed++;
        }
        di.dispose();
        println("NormalizeStorage: " + changed + " prototypes given ordinary storage, " + kept
            + " with other registers kept, " + reverted + " left as they were");
    }
}
