// TraceGeDisplay.java -- find GE/Display call sites and decompile the boot path.
//
// @category MidnightClub
// @author hermes

import java.util.*;
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import ghidra.program.model.address.*;

public class TraceGeDisplay extends GhidraScript {

    private DecompInterface decomp;

    @Override
    public void run() throws Exception {
        decomp = new DecompInterface();
        decomp.openProgram(currentProgram);

        println("=== IMPORTS (GE / DISPLAY / KERNEL-THREAD) ===");
        SymbolTable st = currentProgram.getSymbolTable();
        SymbolIterator it = st.getExternalSymbols();
        int geImports = 0;
        while (it.hasNext()) {
            Symbol s = it.next();
            String n = s.getName();
            if (n.contains("Ge") || n.contains("Display") ||
                n.contains("sceGe") || n.contains("sceDisplay")) {
                println("  IMPORT " + n + "  @ " + s.getAddress());
                geImports++;
            }
        }
        println("  total GE/Display imports: " + geImports);

        println("");
        println("=== ALL EXTERNAL IMPORTS (count) ===");
        int total = 0;
        it = st.getExternalSymbols();
        while (it.hasNext()) { it.next(); total++; }
        println("  total external symbols: " + total);

        println("");
        println("=== XREFS to GE/Display import targets ===");
        ReferenceManager rm = currentProgram.getReferenceManager();
        it = st.getExternalSymbols();
        while (it.hasNext()) {
            Symbol s = it.next();
            String n = s.getName();
            if (!(n.contains("sceGe") || n.contains("sceDisplay"))) continue;
            Address target = s.getAddress();
            java.util.Iterator<Reference> ri = rm.getReferencesTo(target);
            boolean any = false;
            while (ri.hasNext()) {
                any = true;
                Reference r = ri.next();
                Address from = r.getFromAddress();
                Function f = currentProgram.getFunctionManager().getFunctionContaining(from);
                println("  " + n + " @ " + target + " <- called from " + from +
                        " in " + (f != null ? f.getName() + "@" + f.getEntryPoint() : "??"));
            }
            if (!any) {
                println("  " + n + " @ " + target + " -> NO XREFS (never called!)");
            }
        }

        println("");
        println("=== DECOMPILE of boot-path functions ===");
        long[] addrs = new long[] {
            0x000000D8L, 0x000000DCL, 0x00000134L, 0x00000148L, 0x0000014CL,
            0x000005A4L, 0x00000008L
        };
        for (long a : addrs) {
            Function f = currentProgram.getFunctionManager().getFunctionAt(toAddr(a));
            if (f == null) {
                println("  -- no function at " + Long.toHexString(a));
                continue;
            }
            println("");
            println("--- " + f.getName() + " @ " + f.getEntryPoint() +
                    " (body " + f.getBody().getNumAddresses() + " bytes) ---");
            DecompileResults res = decomp.decompileFunction(f, 60, monitor);
            if (res != null && res.decompileCompleted()) {
                String code = res.getDecompiledFunction().getC();
                println(code);
            } else {
                println("  (decompile failed)");
            }
        }

        println("");
        println("=== FUNCTIONS CALLED FROM module_start (0x5A4) REGION ===");
        Function ms = currentProgram.getFunctionManager().getFunctionAt(toAddr(0x5A4L));
        if (ms != null) {
            for (Function callee : ms.getCalledFunctions(monitor)) {
                println("  " + callee.getName() + " @ " + callee.getEntryPoint());
            }
        }
    }
}
