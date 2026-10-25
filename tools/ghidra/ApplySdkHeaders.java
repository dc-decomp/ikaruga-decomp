// Post-analysis: parse Katana SDK headers into the program's data types, and give every function
// whose name matches a parsed prototype that signature, so calls into the SDK decompile with real
// argument types and the right float registers.
//
// The analysis reads game/sdk.h, the part of the headers that it uses, which ExportSdkHeader.java
// writes from an SDK copy. With an include directory, the script reads the SDK's own headers.
// Usage (headless): -postScript ApplySdkHeaders.java <header file>
//                   -postScript ApplySdkHeaders.java <include dir> <header,header,...> [-Dname ...]
// Without the file or the directory the script does nothing.
//@category Dreamcast
import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashMap;
import java.util.Iterator;
import java.util.List;
import java.util.Map;
import java.util.regex.Pattern;

import ghidra.app.cmd.function.ApplyFunctionSignatureCmd;
import ghidra.app.script.GhidraScript;
import ghidra.app.util.cparser.C.CParserUtils;
import ghidra.app.util.cparser.C.CParserUtils.CParseResults;
import ghidra.program.model.data.DataType;
import ghidra.program.model.data.DataTypeManager;
import ghidra.program.model.data.Array;
import ghidra.program.model.data.FunctionDefinition;
import ghidra.program.model.data.ParameterDefinition;
import ghidra.program.model.data.PointerDataType;
import ghidra.program.model.data.TypeDef;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.SourceType;

public class ApplySdkHeaders extends GhidraScript {

    // The SDK spells an empty parameter list with its void typedefs, which the parser accepts only
    // as a literal void.
    private static final Pattern EMPTY_PARAMS = Pattern.compile("\\(\\s*(?:Void|VOID|KMVOID)\\s*\\)");

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        File include = new File(args[0]);
        List<String> files = new ArrayList<>();
        String[] defines = new String[0];
        if (include.isFile()) {
            files.add(include.getPath());
            include = include.getAbsoluteFile().getParentFile();
        }
        else if (include.isDirectory()) {
            // Some headers end with a DOS end-of-file byte (0x1A), which the C lexer rejects, and
            // spell empty parameter lists with a typedef. Parse a corrected copy in the temp
            // directory, so the SDK copy itself stays as it is.
            include = cleanCopy(include);
            // The SDK's file names vary in case; the include directives inside it do too, which the
            // preprocessor resolves against the directory on a case-insensitive file system.
            for (String h : args[1].split(",")) {
                File dir = include;
                File[] match = dir.listFiles((d, n) -> n.equalsIgnoreCase(h));
                if (match == null || match.length == 0) {
                    println("ApplySdkHeaders: no " + h + " in " + include);
                    continue;
                }
                files.add(match[0].getPath());
            }
            defines = Arrays.copyOfRange(args, 2, args.length);
        }
        else {
            println("ApplySdkHeaders: no header or include directory " + include + ", skipped");
            return;
        }

        DataTypeManager dtm = currentProgram.getDataTypeManager();
        CParseResults r = CParserUtils.parseHeaderFiles(null, files.toArray(new String[0]),
            new String[] { include.getPath() }, defines, dtm, monitor);
        println("ApplySdkHeaders: parse " + (r.successful() ? "succeeded" : "had errors"));
        for (String m : new String[] { r.cppParseMessages(), r.cParseMessages() }) {
            if (m != null && !m.isBlank()) {
                println(m.length() > 4000 ? m.substring(0, 4000) + "..." : m);
            }
        }

        Map<String, FunctionDefinition> prototypes = new HashMap<>();
        Iterator<DataType> it = dtm.getAllDataTypes();
        while (it.hasNext()) {
            if (it.next() instanceof FunctionDefinition fd) {
                prototypes.putIfAbsent(fd.getName(), fd);
            }
        }
        int applied = 0, failed = 0;
        for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
            FunctionDefinition fd = prototypes.get(f.getName());
            if (fd == null) {
                continue;
            }
            fd = decayArrays(fd);
            if (new ApplyFunctionSignatureCmd(f.getEntryPoint(), fd, SourceType.IMPORTED)
                    .applyTo(currentProgram, monitor)) {
                applied++;
            }
            else {
                failed++;
            }
        }
        println("ApplySdkHeaders: " + prototypes.size() + " prototypes parsed, " + applied
            + " functions given one, " + failed + " failed");
    }

    // An array parameter is a pointer in C. Ghidra keeps the array type and then cannot place the
    // parameter: njGenerateFogTable3(NJS_FOG_TABLE fog, Float n, Float f) takes a Float[128].
    private static FunctionDefinition decayArrays(FunctionDefinition fd) {
        ParameterDefinition[] args = fd.getArguments();
        boolean changed = false;
        for (ParameterDefinition p : args) {
            DataType t = p.getDataType();
            DataType base = t instanceof TypeDef td ? td.getBaseDataType() : t;
            if (base instanceof Array a) {
                p.setDataType(new PointerDataType(a.getDataType()));
                changed = true;
            }
        }
        if (!changed) {
            return fd;
        }
        FunctionDefinition copy = (FunctionDefinition) fd.copy(fd.getDataTypeManager());
        copy.setArguments(args);
        return copy;
    }

    private static File cleanCopy(File include) throws Exception {
        Path out = Path.of(System.getProperty("java.io.tmpdir"), "sdk-include");
        Files.createDirectories(out);
        File[] headers = include.listFiles(File::isFile);
        if (headers == null) {
            return include;
        }
        for (File f : headers) {
            byte[] b = Files.readAllBytes(f.toPath());
            int n = 0;
            for (byte x : b) {
                if (x != 0x1A) {
                    b[n++] = x;
                }
            }
            // Latin-1 keeps every byte, including the Shift-JIS comments, as it is.
            String text = new String(b, 0, n, StandardCharsets.ISO_8859_1);
            text = EMPTY_PARAMS.matcher(text).replaceAll("(void)");
            Files.write(out.resolve(f.getName()), text.getBytes(StandardCharsets.ISO_8859_1));
        }
        return out.toFile();
    }
}
