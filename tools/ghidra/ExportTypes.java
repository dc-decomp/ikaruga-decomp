// Post-analysis: write every structure, union, typedef, enum and function definition in the program
// to a JSON file, with each field's offset and size.
//
// tools/decomp/convert.py builds the C++ declarations from it. Ghidra's own C header writer drops the
// offsets, and the port needs every structure laid out exactly as the game has it in memory.
// Usage (headless): -postScript ExportTypes.java <types.json>
//@category Dreamcast

import java.io.File;
import java.io.PrintWriter;
import java.util.Iterator;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.data.Array;
import ghidra.program.model.data.BitFieldDataType;
import ghidra.program.model.data.Composite;
import ghidra.program.model.data.DataType;
import ghidra.program.model.data.DataTypeComponent;
import ghidra.program.model.data.Enum;
import ghidra.program.model.data.FunctionDefinition;
import ghidra.program.model.data.ParameterDefinition;
import ghidra.program.model.data.Structure;
import ghidra.program.model.data.TypeDef;
import ghidra.program.model.data.Union;

public class ExportTypes extends GhidraScript {

    @Override
    public void run() throws Exception {
        int n = 0;
        try (PrintWriter w = new PrintWriter(new File(getScriptArgs()[0]))) {
            w.println("[");
            Iterator<DataType> it = currentProgram.getDataTypeManager().getAllDataTypes();
            boolean first = true;
            while (it.hasNext()) {
                String j = describe(it.next());
                if (j == null) {
                    continue;
                }
                w.print(first ? " " : ",\n ");
                w.print(j);
                first = false;
                n++;
            }
            w.println("\n]");
        }
        println("ExportTypes: " + n + " types -> " + getScriptArgs()[0]);
    }

    private String describe(DataType dt) {
        StringBuilder b = new StringBuilder();
        b.append("{\"name\": ").append(q(dt.getDisplayName()))
            .append(", \"path\": ").append(q(dt.getPathName()))
            .append(", \"size\": ").append(dt.getLength());
        if (dt instanceof Structure || dt instanceof Union) {
            Composite c = (Composite) dt;
            b.append(", \"kind\": ").append(q(dt instanceof Structure ? "struct" : "union"))
                .append(", \"align\": ").append(c.getAlignment())
                .append(", \"fields\": [");
            boolean first = true;
            for (DataTypeComponent f : c.getDefinedComponents()) {
                DataType ft = f.getDataType();
                b.append(first ? "" : ", ")
                    .append("{\"name\": ").append(q(f.getFieldName() == null
                        ? "field_0x" + Integer.toHexString(f.getOffset()) : f.getFieldName()))
                    .append(", \"offset\": ").append(f.getOffset())
                    .append(", \"size\": ").append(f.getLength())
                    .append(", \"type\": ").append(q(ft.getDisplayName()));
                if (ft instanceof BitFieldDataType bf) {
                    b.append(", \"bit_offset\": ").append(bf.getBitOffset())
                        .append(", \"bit_size\": ").append(bf.getBitSize())
                        .append(", \"base\": ").append(q(bf.getBaseDataType().getDisplayName()));
                }
                b.append("}");
                first = false;
            }
            return b.append("]}").toString();
        }
        if (dt instanceof TypeDef td) {
            return b.append(", \"kind\": \"typedef\", \"base\": ")
                .append(q(td.getDataType().getDisplayName())).append("}").toString();
        }
        if (dt instanceof Enum e) {
            b.append(", \"kind\": \"enum\", \"values\": {");
            boolean first = true;
            for (String name : e.getNames()) {
                b.append(first ? "" : ", ").append(q(name)).append(": ").append(e.getValue(name));
                first = false;
            }
            return b.append("}}").toString();
        }
        if (dt instanceof FunctionDefinition fd) {
            b.append(", \"kind\": \"funcdef\", \"ret\": ")
                .append(q(fd.getReturnType().getDisplayName())).append(", \"args\": [");
            boolean first = true;
            for (ParameterDefinition p : fd.getArguments()) {
                b.append(first ? "" : ", ").append(q(p.getDataType().getDisplayName()));
                first = false;
            }
            return b.append("], \"varargs\": ").append(fd.hasVarArgs()).append("}").toString();
        }
        if (dt instanceof Array) {
            return null;
        }
        return null;
    }

    private static String q(String s) {
        StringBuilder b = new StringBuilder("\"");
        for (char c : s.toCharArray()) {
            if (c == '"' || c == '\\') {
                b.append('\\').append(c);
            }
            else if (c < 0x20) {
                b.append(String.format("\\u%04x", (int) c));
            }
            else {
                b.append(c);
            }
        }
        return b.append('"').toString();
    }
}
