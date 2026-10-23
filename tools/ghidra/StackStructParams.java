// Post-analysis: a structure or union passed by value goes on the stack, as SHC passes it.
//
// The SDK headers declare kmSetFogTableColor(KMPACKEDARGB), a four-byte union. Ghidra's convention
// puts it in r4, but SHC passes every structure and union argument in memory at the stack pointer,
// and the function reads it from there. With r4 storage the C read an unknown stack word, and the
// fog color was 0. Such a parameter gets stack storage from offset 0 in order; the others keep the
// registers the convention gives them.
// Usage (headless): -postScript StackStructParams.java
//@category Dreamcast

import java.util.ArrayList;
import java.util.List;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.data.AbstractFloatDataType;
import ghidra.program.model.data.DataType;
import ghidra.program.model.data.Structure;
import ghidra.program.model.data.TypeDef;
import ghidra.program.model.data.Union;
import ghidra.program.model.lang.Register;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Parameter;
import ghidra.program.model.listing.ParameterImpl;
import ghidra.program.model.listing.ReturnParameterImpl;
import ghidra.program.model.listing.Variable;
import ghidra.program.model.listing.VariableStorage;
import ghidra.program.model.symbol.SourceType;

public class StackStructParams extends GhidraScript {

    private static DataType resolve(DataType t) {
        while (t instanceof TypeDef) {
            t = ((TypeDef) t).getBaseDataType();
        }
        return t;
    }

    @Override
    public void run() throws Exception {
        int changed = 0;
        for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
            boolean any = false;
            for (Parameter p : f.getParameters()) {
                DataType t = resolve(p.getDataType());
                any |= t instanceof Structure || t instanceof Union;
            }
            if (!any) {
                continue;
            }
            List<Variable> params = new ArrayList<>();
            int nr = 0, nfr = 0, stack = 0;
            for (Parameter p : f.getParameters()) {
                DataType t = resolve(p.getDataType());
                VariableStorage s;
                if (t instanceof Structure || t instanceof Union) {
                    Address a = currentProgram.getAddressFactory().getStackSpace().getAddress(stack);
                    s = new VariableStorage(currentProgram, a, p.getLength());
                    stack += (p.getLength() + 3) & ~3;
                }
                else if (t instanceof AbstractFloatDataType && nfr < 8) {
                    Register r = currentProgram.getRegister("fr" + (4 + nfr++));
                    s = new VariableStorage(currentProgram, r);
                }
                else if (nr < 4) {
                    Register r = currentProgram.getRegister("r" + (4 + nr++));
                    s = new VariableStorage(currentProgram, r);
                }
                else {
                    Address a = currentProgram.getAddressFactory().getStackSpace().getAddress(stack);
                    s = new VariableStorage(currentProgram, a, 4);
                    stack += 4;
                }
                params.add(new ParameterImpl(p.getName(), p.getDataType(), s, currentProgram));
            }
            f.updateFunction(f.getCallingConventionName(),
                new ReturnParameterImpl(f.getReturnType(), f.getReturn().getVariableStorage(), currentProgram),
                params, Function.FunctionUpdateType.CUSTOM_STORAGE, true, SourceType.IMPORTED);
            println("StackStructParams: " + f.getName() + " " + f.getSignature().getPrototypeString());
            changed++;
        }
        println("StackStructParams: " + changed + " prototypes with a structure on the stack");
    }
}
