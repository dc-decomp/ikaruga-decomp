// Post-analysis, after ApplySdkHeaders.java parsed the SDK's own headers: write the part of them
// that the analysis uses as one C header, game/sdk.h. That is the prototype of each SDK function
// that game/symbols.tsv names, and the types that these prototypes, game/types.h and the type
// column of game/globals.tsv need. The analysis then parses the header instead of the SDK.
// Usage (headless): -postScript ExportSdkHeader.java <out.h> <symbols.tsv> [files that name types]
//@category Dreamcast
import java.io.File;
import java.io.PrintWriter;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.HashSet;
import java.util.Iterator;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.TreeMap;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.data.Array;
import ghidra.program.model.data.BuiltInDataType;
import ghidra.program.model.data.DataType;
import ghidra.program.model.data.DataTypeManager;
import ghidra.program.model.data.DataTypeWriter;
import ghidra.program.model.data.FunctionDefinition;
import ghidra.program.model.data.ParameterDefinition;
import ghidra.program.model.data.Pointer;

public class ExportSdkHeader extends GhidraScript {

    private static final Pattern IDENT = Pattern.compile("[A-Za-z_][A-Za-z0-9_]*");

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        File out = new File(args[0]);
        DataTypeManager dtm = currentProgram.getDataTypeManager();

        // The prototypes, by the same rule as ApplySdkHeaders: the first definition of a name.
        Map<String, FunctionDefinition> all = new HashMap<>();
        Iterator<DataType> it = dtm.getAllDataTypes();
        while (it.hasNext()) {
            if (it.next() instanceof FunctionDefinition fd) {
                all.putIfAbsent(fd.getName(), fd);
            }
        }
        Map<String, FunctionDefinition> used = new TreeMap<>();
        for (String line : Files.readAllLines(Path.of(args[1]))) {
            if (line.isBlank() || line.startsWith("#")) {
                continue;
            }
            String[] f = line.split("\t");
            String name = f[1].startsWith("_") ? f[1].substring(1) : f[1];
            FunctionDefinition fd = all.get(name);
            if (fd != null) {
                used.put(name, fd);
            }
        }

        // The types: those of the prototypes, then the SDK types that the other files name.
        Map<String, DataType> types = new LinkedHashMap<>();
        for (FunctionDefinition fd : used.values()) {
            addSignature(types, fd);
        }
        for (int i = 2; i < args.length; i++) {
            Matcher m = IDENT.matcher(Files.readString(Path.of(args[i])));
            Set<String> seen = new HashSet<>();
            while (m.find()) {
                if (!seen.add(m.group())) {
                    continue;
                }
                List<DataType> found = new ArrayList<>();
                dtm.findDataTypes(m.group(), found);
                for (DataType t : found) {
                    if (!(t instanceof FunctionDefinition) && !t.getCategoryPath().isRoot()) {
                        add(types, t);
                    }
                }
            }
        }

        try (PrintWriter w = new PrintWriter(out)) {
            w.println("/* The part of the Katana SDK's headers that tools/ghidra/analyze.sh uses: the");
            w.println(" * prototypes of the SDK functions that game/symbols.tsv names, and the types that");
            w.println(" * they and the game's own types need. tools/ghidra/sdk_header.sh writes it from the");
            w.println(" * SDK's headers. Do not edit. */");
            w.println();
            new DataTypeWriter(dtm, w).write(new ArrayList<>(types.values()), monitor);
            w.println();
            for (FunctionDefinition fd : used.values()) {
                w.println(declare(fd.getReturnType(), fd.getName() + parameters(fd)) + ";");
            }
        }
        println("ExportSdkHeader: " + used.size() + " prototypes and " + types.size() + " types to " + out);
    }

    // A declaration of `name` with type `t`, in C's syntax: a pointer to a function the header
    // declares no name for is written out, void (*name)(int), and so is an array, T name[3].
    private static String declare(DataType t, String name) {
        if (t instanceof Pointer p && p.getDataType() instanceof FunctionDefinition fd) {
            return declare(fd.getReturnType(), "(*" + name + ")" + parameters(fd));
        }
        if (t instanceof Array a) {
            return declare(a.getDataType(), name + "[" + a.getNumElements() + "]");
        }
        return name.isEmpty() ? t.getDisplayName() : t.getDisplayName() + " " + name;
    }

    private static String parameters(FunctionDefinition fd) {
        List<String> out = new ArrayList<>();
        ParameterDefinition[] args = fd.getArguments();
        for (int i = 0; i < args.length; i++) {
            // A parameter the headers leave unnamed stays unnamed, so that no local takes its name.
            out.add(declare(args[i].getDataType(), args[i].getName() == null ? "" : args[i].getName()));
        }
        if (fd.hasVarArgs()) {
            out.add("...");
        }
        return "(" + (out.isEmpty() ? "void" : String.join(", ", out)) + ")";
    }

    private static void addSignature(Map<String, DataType> types, FunctionDefinition fd) {
        add(types, fd.getReturnType());
        for (ParameterDefinition p : fd.getArguments()) {
            add(types, p.getDataType());
        }
    }

    private static void add(Map<String, DataType> types, DataType t) {
        if (t == null || t instanceof BuiltInDataType) {
            return;
        }
        if (t instanceof Pointer p && p.getDataType() instanceof FunctionDefinition fd) {
            addSignature(types, fd);
            return;
        }
        types.putIfAbsent(t.getPathName(), t);
    }
}
