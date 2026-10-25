// Post-analysis: the game's own types. Parses game/types.h into the program's data types, gives
// every function whose name matches a prototype there that signature, and types the data that
// game/globals.tsv names with a type in its fourth column, such as TaskList[7].
// Usage (headless): -postScript ApplyGameTypes.java <types.h> <globals.tsv>
//@category Dreamcast
import java.io.File;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.HashMap;
import java.util.Iterator;
import java.util.Map;
import java.util.Set;

import ghidra.app.cmd.function.ApplyFunctionSignatureCmd;
import ghidra.app.script.GhidraScript;
import ghidra.app.util.cparser.C.CParserUtils;
import ghidra.app.util.cparser.C.CParserUtils.CParseResults;
import ghidra.program.model.address.Address;
import ghidra.program.model.data.BuiltInDataTypeManager;
import ghidra.program.model.data.DataType;
import ghidra.program.model.data.DataTypeManager;
import ghidra.program.model.data.FunctionDefinition;
import ghidra.program.model.data.InvalidDataTypeException;
import ghidra.program.model.symbol.SourceType;
import ghidra.program.model.listing.Function;
import ghidra.util.data.DataTypeParser;
import ghidra.util.data.DataTypeParser.AllowedDataTypes;

public class ApplyGameTypes extends GhidraScript {

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        File header = new File(args[0]);
        DataTypeManager dtm = currentProgram.getDataTypeManager();
        Set<String> before = functionDefinitions(dtm).keySet();
        CParseResults r = CParserUtils.parseHeaderFiles(null, new String[] { header.getPath() },
            new String[] { header.getParent() }, new String[0], dtm, monitor);
        println("ApplyGameTypes: parse " + (r.successful() ? "succeeded" : "had errors"));
        for (String m : new String[] { r.cppParseMessages(), r.cParseMessages() }) {
            if (m != null && !m.isBlank()) {
                println(m);
            }
        }

        // The prototypes: the function definitions that the header added, by name.
        Map<String, FunctionDefinition> prototypes = new HashMap<>();
        for (FunctionDefinition fd : functionDefinitions(dtm).values()) {
            if (!before.contains(fd.getPathName())) {
                prototypes.put(fd.getName(), fd);
            }
        }
        int applied = 0;
        for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
            FunctionDefinition fd = prototypes.get(f.getName());
            if (fd == null) {
                continue;
            }
            ApplyFunctionSignatureCmd cmd =
                new ApplyFunctionSignatureCmd(f.getEntryPoint(), fd, SourceType.USER_DEFINED);
            if (cmd.applyTo(currentProgram, monitor)) {
                applied++;
            } else {
                println("ApplyGameTypes: " + f.getName() + " failed: " + cmd.getStatusMsg());
            }
        }

        // The data with a type in the fourth column.
        DataTypeParser parser = new DataTypeParser(dtm, dtm, null, AllowedDataTypes.ALL);
        // Ghidra's own types, such as uchar, which the program's types need not declare.
        DataTypeParser builtin = new DataTypeParser(BuiltInDataTypeManager.getDataTypeManager(), dtm, null,
            AllowedDataTypes.ALL);
        int typed = 0;
        for (String line : Files.readAllLines(Path.of(args[1]))) {
            if (line.isBlank() || line.startsWith("#")) {
                continue;
            }
            String[] f = line.split("\t");
            if (f.length < 4 || f[3].isBlank()) {
                continue;
            }
            DataType t;
            try {
                t = parser.parse(f[3].trim());
            }
            catch (InvalidDataTypeException e) {
                t = builtin.parse(f[3].trim());
            }
            Address a = toAddr(Long.decode(f[0]) | 0x80000000L);
            clearListing(a, a.add(t.getLength() - 1));
            createData(a, t);
            typed++;
        }
        println("ApplyGameTypes: " + prototypes.size() + " prototypes, " + applied + " functions given one, "
            + typed + " data typed");
    }

    private static Map<String, FunctionDefinition> functionDefinitions(DataTypeManager dtm) {
        Map<String, FunctionDefinition> all = new HashMap<>();
        Iterator<DataType> it = dtm.getAllDataTypes();
        while (it.hasNext()) {
            if (it.next() instanceof FunctionDefinition fd) {
                all.put(fd.getPathName(), fd);
            }
        }
        return all;
    }
}
