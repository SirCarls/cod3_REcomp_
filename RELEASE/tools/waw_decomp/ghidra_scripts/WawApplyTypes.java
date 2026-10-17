// Parses a C header of World at War's types into the program and applies them:
//   - globals named dvar_* become dvar_s pointers,
//   - globals and functions listed in symbols tables (addr, kind, name, type[, ...]) get those
//     types (function types become prototypes; only for functions whose name matches the table).
//
// Headless: -postScript WawApplyTypes.java <header.h> [symbols.tsv ...]
// The header and the first table are made by tools/waw_decomp/t4sp_types.py (private use only);
// more tables, such as the typed globals found with struct_globals.py, can follow.
// @category WaW

import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.util.*;

import ghidra.app.cmd.function.ApplyFunctionSignatureCmd;
import ghidra.app.script.GhidraScript;
import ghidra.app.util.cparser.C.CParserUtils;
import ghidra.app.util.cparser.C.CParserUtils.CParseResults;
import ghidra.app.util.parser.FunctionSignatureParser;
import ghidra.program.model.address.*;
import ghidra.program.model.data.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;

public class WawApplyTypes extends GhidraScript {

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        DataTypeManager dtm = currentProgram.getDataTypeManager();
        File h = new File(args[0]);
        try {
            CParseResults res = CParserUtils.parseHeaderFiles(new DataTypeManager[0], new String[] { h.getAbsolutePath() },
                    new String[] { h.getParent() }, new String[0], dtm, monitor);
            for (String msg : new String[] { res.cppParseMessages(), res.cParseMessages() }) {
                if (msg == null || msg.isBlank()) continue;
                String[] lines = msg.split("\n");
                for (int i = 0; i < Math.min(lines.length, 40); i++) println("cparse: " + lines[i]);
            }
            println("parse successful: " + res.successful());
        } catch (Exception e) {
            println("cparse error: " + e.getMessage());
            return;
        }

        DataType dvar = findType(dtm, "dvar_s");
        int dv = 0;
        if (dvar != null) {
            PointerDataType ptr = new PointerDataType(dvar, dtm);
            for (Symbol s : currentProgram.getSymbolTable().getAllSymbols(true)) {
                if (s.getSymbolType() == SymbolType.LABEL && s.getName().startsWith("dvar_")) {
                    try {
                        Address a = s.getAddress();
                        currentProgram.getListing().clearCodeUnits(a, a.add(3), false);
                        currentProgram.getListing().createData(a, ptr);
                        dv++;
                    } catch (Exception e) {
                        // leave it untyped
                    }
                }
            }
        }
        println("dvar globals typed: " + dv);

        if (args.length < 2) return;
        int gl = 0, fn = 0, fail = 0;
        FunctionSignatureParser fsp = new FunctionSignatureParser(dtm, null);
        List<String> lines = new ArrayList<>();
        for (int i = 1; i < args.length; i++) lines.addAll(Files.readAllLines(Paths.get(args[i]), StandardCharsets.UTF_8));
        for (String line : lines) {
            String[] c = line.split("\t");
            if (c.length < 4 || c[0].equals("addr") || c[3].isBlank()) continue;
            Address a = toAddr(Long.parseLong(c[0], 16));
            try {
                if (c[1].equals("function")) {
                    Function f = getFunctionAt(a);
                    if (f == null || !f.getName().equals(c[2])) continue;
                    String t = c[3].replaceAll("\\bconst\\b\\s*", "");  // the signature parser rejects const
                    int p = t.indexOf('(');
                    String sig = t.substring(0, p) + " " + c[2] + t.substring(p);
                    FunctionDefinitionDataType def = fsp.parse(f.getSignature(), sig);
                    if (new ApplyFunctionSignatureCmd(a, def, SourceType.USER_DEFINED).applyTo(currentProgram, monitor)) fn++;
                    else failed(++fail, c, "prototype not applied");
                } else {
                    DataType t = resolve(dtm, c[3]);
                    if (t == null) { failed(++fail, c, "type not found"); continue; }
                    currentProgram.getListing().clearCodeUnits(a, a.add(Math.max(1, t.getLength()) - 1), false);
                    currentProgram.getListing().createData(a, t);
                    gl++;
                }
            } catch (Exception e) {
                failed(++fail, c, e.getMessage());
            }
        }
        println("typed: " + fn + " function prototypes, " + gl + " globals, " + fail + " failed");
    }

    // the first few failures, so a wrong row can be found
    private void failed(int n, String[] row, String why) {
        if (n <= 30) println("failed: " + row[0] + " " + row[2] + " " + row[3] + ": " + why);
    }

    private DataType findType(DataTypeManager dtm, String name) {
        List<DataType> l = new ArrayList<>();
        dtm.findDataTypes(name, l);
        for (DataType d : l) if (!(d instanceof TypeDef) && d.getLength() > 0) return d;
        return l.isEmpty() ? null : l.get(0);
    }

    // "gentity_s[1024]", "scrVmPub_t", "dvar_s*"
    private DataType resolve(DataTypeManager dtm, String spec) {
        spec = spec.replace("const ", "").trim();
        int arr = 1;
        java.util.regex.Matcher m = java.util.regex.Pattern.compile("^(.*)\\[(\\d+)\\]$").matcher(spec);
        if (m.matches()) { spec = m.group(1).trim(); arr = Integer.parseInt(m.group(2)); }
        int ptrs = 0;
        while (spec.endsWith("*")) { ptrs++; spec = spec.substring(0, spec.length() - 1).trim(); }
        DataType t = findType(dtm, spec);
        if (t == null) {
            switch (spec) {
                case "int": t = IntegerDataType.dataType; break;
                case "unsigned int": t = UnsignedIntegerDataType.dataType; break;
                case "char": t = CharDataType.dataType; break;
                case "unsigned char": t = UnsignedCharDataType.dataType; break;
                case "float": t = FloatDataType.dataType; break;
                case "bool": t = BooleanDataType.dataType; break;
                case "short": t = ShortDataType.dataType; break;
                case "unsigned short": t = UnsignedShortDataType.dataType; break;
                case "void": t = VoidDataType.dataType; break;
                default: return null;
            }
        }
        for (int i = 0; i < ptrs; i++) t = new PointerDataType(t, dtm);
        if (arr > 1) t = new ArrayDataType(t, arr, t.getLength(), dtm);
        return t;
    }
}
