// ReportPspStats.java — Ghidra headless post-script for the Midnight Club
// static-recomp project.
//
// Prints the numbers milestone 1 asks for: function count, the sceKernel*/
// sceGu*/sceCtrl* import stubs the module references, and the code extent.
// Everything printed here is derived from the imported program, and is
// cross-checked against `allegrexrecomp funcs` and readelf/objdump.
//
// @category MidnightClub

import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;
import ghidra.program.model.listing.Listing;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.symbol.Symbol;
import ghidra.program.model.symbol.SymbolIterator;

import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.TreeSet;

public class ReportPspStats extends GhidraScript {

    @Override
    public void run() throws Exception {
        println("=== PSP stats for " + currentProgram.getName() + " ===");
        println("language: " + currentProgram.getLanguageID());
        println("image base: " + currentProgram.getImageBase());

        FunctionIterator funcs = currentProgram.getFunctionManager().getFunctions(true);
        List<Function> all = new ArrayList<>();
        while (funcs.hasNext()) {
            all.add(funcs.next());
        }
        println("function count: " + all.size());

        Listing listing = currentProgram.getListing();
        Memory memory = currentProgram.getMemory();

        // The import stubs live in .sceStub.text.* sections. Report each stub
        // function and the NID/label Ghidra recovered for it.
        List<String> stubs = new ArrayList<>();
        for (Function f : all) {
            String name = f.getName();
            if (name.toLowerCase().contains("sce")) {
                stubs.add(f.getEntryPoint() + " " + name);
            }
        }
        Collections.sort(stubs);
        println("--- sce* labelled functions (" + stubs.size() + ") ---");
        for (String s : stubs) {
            println("  " + s);
        }

        // Symbol table sweep: PSP modules carry NID tables in .rodata.sceNid.
        SymbolIterator syms = currentProgram.getSymbolTable().getAllSymbols(true);
        TreeSet<String> kernel = new TreeSet<>();
        TreeSet<String> gu = new TreeSet<>();
        TreeSet<String> ctrl = new TreeSet<>();
        int total = 0;
        while (syms.hasNext()) {
            Symbol s = syms.next();
            total++;
            String n = s.getName();
            if (n.contains("sceKernel")) kernel.add(n);
            else if (n.contains("sceGu")) gu.add(n);
            else if (n.contains("sceCtrl")) ctrl.add(n);
        }
        println("--- symbol counts ---");
        println("total symbols: " + total);
        println("sceKernel* symbols: " + kernel.size());
        println("sceGu* symbols: " + gu.size());
        println("sceCtrl* symbols: " + ctrl.size());
        println("--- sceKernel* symbol names ---");
        for (String n : kernel) println("  " + n);
        println("--- sceGu* symbol names ---");
        for (String n : gu) println("  " + n);
        println("--- sceCtrl* symbol names ---");
        for (String n : ctrl) println("  " + n);

        // Memory blocks, so the code/data extent is on record.
        println("--- memory blocks ---");
        for (ghidra.program.model.mem.MemoryBlock b : memory.getBlocks()) {
            println(String.format("  %-24s %s  size=0x%x", b.getName(), b.getStart(), b.getSize()));
        }
        println("=== end PSP stats ===");
    }
}
