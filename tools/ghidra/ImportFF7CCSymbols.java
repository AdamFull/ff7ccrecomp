// Creates named functions from a `psprx symbols ... <base> <ghidra.txt>` file ("<name> <address> f" per line).
// Usable from the GUI (Script Manager, asks for the file) or headless:
//   analyzeHeadless ... -postScript ImportFF7CCSymbols.java <ghidra.txt> -scriptPath tools/ghidra
//@category FF7CCRECOMP

import java.io.BufferedReader;
import java.io.File;
import java.io.FileReader;

import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.SourceType;

public class ImportFF7CCSymbols extends GhidraScript {
    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        File file = args.length > 0 ? new File(args[0]) : askFile("psprx symbols file (Ghidra format)", "Import");
        int created = 0, renamed = 0, failed = 0;
        try (BufferedReader in = new BufferedReader(new FileReader(file))) {
            String line;
            while ((line = in.readLine()) != null) {
                String[] f = line.trim().split("\\s+");
                if (f.length < 2 || f[0].startsWith("#")) continue;
                Address addr = toAddr(Long.decode(f[1]));
                new DisassembleCommand(addr, null, true).applyTo(currentProgram, monitor);
                Function fn = getFunctionAt(addr);
                if (fn == null) {
                    fn = createFunction(addr, f[0]);
                    if (fn == null) { ++failed; continue; }
                    ++created;
                } else if (!f[0].startsWith("FUN_")) {
                    fn.setName(f[0], SourceType.IMPORTED);
                    ++renamed;
                }
            }
        }
        println(String.format("FF7CC symbols: %d functions created, %d renamed, %d failed", created, renamed, failed));
    }
}
