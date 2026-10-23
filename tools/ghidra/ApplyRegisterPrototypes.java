// Post-analysis: give the SHC runtime's helpers their real prototypes, with arguments in r0 to r2.
//
// SHC compiles division and modulo to calls of _divls and its relatives (divisor in r0, dividend
// in r1, result in r0), structure copies to _quick_mvn and its relatives (byte count in r0,
// destination in r1, source in r2), and string copies to _quick_strcpy (destination in r0, source
// in r1). These are not the argument registers of the calling convention, so without these
// prototypes every caller decompiles with the wrong arguments, or with none, and a structure copy
// vanishes from the C. The port implements the helpers natively.
// Usage (headless): -postScript ApplyRegisterPrototypes.java
//@category Dreamcast

import java.util.ArrayList;
import java.util.List;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.data.DataType;
import ghidra.program.model.data.IntegerDataType;
import ghidra.program.model.data.PointerDataType;
import ghidra.program.model.data.ShortDataType;
import ghidra.program.model.data.UnsignedIntegerDataType;
import ghidra.program.model.data.VoidDataType;
import ghidra.program.model.lang.Register;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Function.FunctionUpdateType;
import ghidra.program.model.listing.Parameter;
import ghidra.program.model.listing.ParameterImpl;
import ghidra.program.model.listing.ReturnParameterImpl;
import ghidra.program.model.listing.VariableStorage;
import ghidra.program.model.symbol.SourceType;

public class ApplyRegisterPrototypes extends GhidraScript {

    private DataType i32 = IntegerDataType.dataType, u32 = UnsignedIntegerDataType.dataType;
    private DataType ptr = new PointerDataType(VoidDataType.dataType), s16 = ShortDataType.dataType;

    @Override
    public void run() throws Exception {
        int n = 0;
        n += apply("_divls", i32, new Object[][] { { "divisor", i32, "r0" }, { "dividend", i32, "r1" } });
        n += apply("_divlu", u32, new Object[][] { { "divisor", u32, "r0" }, { "dividend", u32, "r1" } });
        n += apply("_modls", i32, new Object[][] { { "divisor", i32, "r0" }, { "dividend", i32, "r1" } });
        n += apply("_modlu", u32, new Object[][] { { "divisor", u32, "r0" }, { "dividend", u32, "r1" } });
        n += apply("_divws", i32, new Object[][] { { "divisor", s16, "r0" }, { "dividend", s16, "r1" } });
        for (String mvn : new String[] { "_quick_mvn", "_quick_odd_mvn", "_quick_evn_mvn", "_slow_mvn" }) {
            n += apply(mvn, VoidDataType.dataType,
                new Object[][] { { "size", u32, "r0" }, { "dst", ptr, "r1" }, { "src", ptr, "r2" } });
        }
        // Like strcpy, they return the destination in r0, and callers use it.
        for (String cpy : new String[] { "_quick_strcpy", "quick_strcpy1", "_slow_strcpy" }) {
            n += apply(cpy, ptr,
                new Object[][] { { "dst", ptr, "r0" }, { "src", ptr, "r1" } });
        }
        println("ApplyRegisterPrototypes: " + n + " helpers given their register prototypes");
    }

    private int apply(String name, DataType ret, Object[][] params) throws Exception {
        int count = 0;
        for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
            if (!f.getName().equals(name)) {
                continue;
            }
            List<Parameter> ps = new ArrayList<>();
            for (Object[] p : params) {
                Register r = currentProgram.getRegister((String) p[2]);
                DataType t = (DataType) p[1];
                VariableStorage st = new VariableStorage(currentProgram,
                    t.getLength() < 4 ? new ghidra.program.model.pcode.Varnode(r.getAddress(), 4) : new ghidra.program.model.pcode.Varnode(r.getAddress(), 4));
                ps.add(new ParameterImpl((String) p[0], t.getLength() < 4 ? i32 : t, st, currentProgram));
            }
            Register r0 = currentProgram.getRegister("r0");
            ReturnParameterImpl rp = ret instanceof VoidDataType
                    ? new ReturnParameterImpl(ret, currentProgram)
                    : new ReturnParameterImpl(ret, new VariableStorage(currentProgram, r0), currentProgram);
            f.updateFunction(null, rp, ps, FunctionUpdateType.CUSTOM_STORAGE, true,
                SourceType.USER_DEFINED);
            count++;
        }
        return count;
    }
}
