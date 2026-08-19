// Ghidra headless post-script. Lists functions whose qualified names contain a token.
// @category Apex Editor

import java.util.Locale;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;

public class ExportFunctions extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] arguments = getScriptArgs();
        if (arguments.length == 0) throw new IllegalArgumentException("Usage: name-token [name-token...]");
        FunctionIterator functions = currentProgram.getFunctionManager().getFunctions(true);
        while (functions.hasNext() && !monitor.isCancelled()) {
            Function function = functions.next();
            String qualified = function.getName(true);
            String lowered = qualified.toLowerCase(Locale.ROOT);
            for (String argument : arguments) {
                if (!lowered.contains(argument.toLowerCase(Locale.ROOT))) continue;
                println("APEX_FUNCTION|" + function.getEntryPoint() + "|" + qualified);
                break;
            }
        }
    }
}
