// FindGeDisplayXrefs.java
// Headless Ghidra script: locate sceGeListEnqueue / sceDisplaySetFrameBuf
// and report every call site (function + address) plus the import stubs.
//
// @category MidnightClub
// @author hermes

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;
import ghidra.program.model.symbol.Symbol;
import ghidra.program.model.symbol.SymbolIterator;

public class FindGeDisplayXrefs extends GhidraScript {

    private static final String[] TARGETS = {
        "sceGeListEnqueue",
        "sceDisplaySetFrameBuf",
        "sceDisplaySetDisplayBuf",
        "sceGeListEnQueue",
        "sceDisplaySetFrameBuf",
        "sceGeInit",
        "sceGuDrawArray",
        "sceGuDrawList",
        "sceGuSetInitialFillColor",
        "sceGuSetFrameBuf",
        "sceGeCreateContext",
        "sceDisplaySetMode",
    };

    @Override
    protected void run() throws Exception {
        println("=== GE/Display symbol scan ===");

        // 1) Symbols whose name matches a target.
        SymbolIterator syms = currentProgram.getSymbolTable().getSymbolIterator();
        int symHits = 0;
        while (syms.hasNext()) {
            Symbol s = syms.next();
            String name = s.getName();
            for (String t : TARGETS) {
                if (name.contains(t)) {
                    println(String.format("SYM  %s  addr=%s  nsym=%d",
                        name, s.getAddress(), s.getSymbolType()));
                    symHits++;
                    break;
                }
            }
        }
        println("symbol hits: " + symHits);

        // 2) Every instruction that references those symbol addresses.
        println("--- xrefs to matched symbols ---");
        syms = currentProgram.getSymbolTable().getSymbolIterator();
        while (syms.hasNext()) {
            Symbol s = syms.next();
            String name = s.getName();
            boolean match = false;
            for (String t : TARGETS) {
                if (name.contains(t)) { match = true; break; }
            }
            if (!match) continue;

            Address target = s.getAddress();
            ReferenceIterator refs = currentProgram.getReferenceManager().getReferencesTo(target);
            while (refs.hasNext()) {
                Reference r = refs.next();
                Address from = r.getFromAddress();
                Function f = getFunctionContaining(from);
                String fname = (f != null) ? f.getName() : "<none>";
                String fentry = (f != null) ? f.getEntryPoint().toString() : "-";
                println(String.format("XREF %-28s from=%s  fn=%s@%s  type=%s",
                    name, from, fname, fentry, r.getReferenceType()));
            }
        }

        // 3) All JAL/CALL instructions that point at stub-like addresses
        //    (small thunk functions near the module entry).
        println("--- all call instructions to unnamed/thunk targets ---");
        InstructionIterator ins = currentProgram.getListing().getInstructions(true);
        int jal = 0;
        while (ins.hasNext()) {
            Instruction i = ins.next();
            String mn = i.getMnemonicString();
            if (!mn.equalsIgnoreCase("jal") && !mn.equalsIgnoreCase("call")
                && !mn.equalsIgnoreCase("jalr")) continue;
            jal++;
            Address[] flows = i.getFlows();
            if (flows == null || flows.length == 0) continue;
            Address tgt = flows[0];
            Function tf = getFunctionAt(tgt);
            String tname = (tf != null) ? tf.getName() : null;
            // report only calls whose target is a tiny stub (1-4 instrs) => imports
            if (tf != null && tf.getBody().getNumAddresses() <= 6) {
                Function caller = getFunctionContaining(i.getAddress());
                String cname = (caller != null) ? caller.getName() : "<none>";
                println(String.format("CALL fn=%s@%s  ->  %s@%s  (stub %d instrs)  site=%s",
                    cname,
                    (caller != null ? caller.getEntryPoint().toString() : "-"),
                    (tname != null ? tname : "<noname>"), tgt,
                    tf.getBody().getNumAddresses(), i.getAddress()));
            }
        }
        println("total call instructions: " + jal);

        // 4) Dump the import stub region: functions with 1-4 instructions
        println("--- all small thunk functions (likely import stubs) ---");
        FunctionIterator fi = currentProgram.getFunctionManager().getFunctions(true);
        while (fi.hasNext()) {
            Function f = fi.next();
            long n = f.getBody().getNumAddresses();
            if (n > 4) continue;
            // is it called from anywhere?
            int nrefs = 0;
            ReferenceIterator refs = currentProgram.getReferenceManager().getReferencesTo(f.getEntryPoint());
            while (refs.hasNext()) { refs.next(); nrefs++; }
            println(String.format("STUB %-34s @%s  instrs=%d  refs=%d",
                f.getName(), f.getEntryPoint(), n, nrefs));
        }
    }
}
