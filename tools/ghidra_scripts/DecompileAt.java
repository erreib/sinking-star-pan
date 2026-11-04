// Decompile the functions containing the given addresses and write C to the output file.
// args: <outfile> <addr> [<addr> ...]   (hex, with or without 0x)
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import java.io.*;

public class DecompileAt extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        PrintWriter out = new PrintWriter(new FileWriter(args[0]));
        DecompInterface ifc = new DecompInterface();
        DecompileOptions opts = new DecompileOptions();
        opts.setMaxPayloadMBytes(1024);
        ifc.setOptions(opts);
        ifc.openProgram(currentProgram);
        for (int i = 1; i < args.length; i++) {
            String a = args[i].startsWith("0x") ? args[i].substring(2) : args[i];
            Address addr = currentProgram.getAddressFactory().getDefaultAddressSpace().getAddress(Long.parseUnsignedLong(a, 16));
            Function f = getFunctionContaining(addr);
            if (f == null) {
                // try to create one at the address
                f = createFunction(addr, null);
                if (f == null) { out.println("// no function at " + addr); continue; }
            }
            out.println("// ===== " + f.getName() + " @ " + f.getEntryPoint() + " (contains " + addr + ") size=" + f.getBody().getNumAddresses());
            out.flush();
            try {
                DecompileResults res = ifc.decompileFunction(f, 600, monitor);
                if (res.decompileCompleted()) out.println(res.getDecompiledFunction().getC());
                else out.println("// decompile failed: " + res.getErrorMessage());
            } catch (Throwable t) { out.println("// exception: " + t); }
            out.flush();
        }
        out.close();
        ifc.dispose();
    }
}
