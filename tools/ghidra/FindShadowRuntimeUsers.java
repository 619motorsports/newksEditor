import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.HashSet;
import java.util.Map;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/** Finds functions that access several CSP runtime-light shadow fields through one register. */
public class FindShadowRuntimeUsers extends GhidraScript {
  private static final Pattern FIELD = Pattern.compile(
      "\\[([a-z0-9]+) \\+ 0x(260|261|262|263|284|288|28c|290|294|298)\\]",
      Pattern.CASE_INSENSITIVE);

  @Override
  public void run() throws Exception {
    String[] args = getScriptArgs();
    Address start = toAddr(args[0]);
    Address end = toAddr(args[1]);
    int minimumFields = args.length > 2 ? Integer.parseInt(args[2]) : 3;
    Map<String, HashSet<String>> hits = new HashMap<>();
    Map<String, ArrayList<String>> lines = new HashMap<>();
    Map<String, Function> functions = new HashMap<>();

    InstructionIterator instructions = currentProgram.getListing().getInstructions(start, true);
    while (instructions.hasNext()) {
      Instruction instruction = instructions.next();
      if (instruction.getAddress().compareTo(end) >= 0) break;
      Function function = getFunctionContaining(instruction.getAddress());
      if (function == null) continue;
      Matcher matcher = FIELD.matcher(instruction.toString());
      while (matcher.find()) {
        String base = matcher.group(1).toLowerCase();
        if (base.equals("rsp") || base.equals("rbp")) continue;
        String key = function.getEntryPoint() + ":" + base;
        hits.computeIfAbsent(key, ignored -> new HashSet<>()).add(matcher.group(2).toLowerCase());
        lines.computeIfAbsent(key, ignored -> new ArrayList<>())
            .add(instruction.getAddress() + "  " + instruction);
        functions.put(key, function);
      }
    }

    for (Map.Entry<String, HashSet<String>> entry : hits.entrySet()) {
      if (entry.getValue().size() < minimumFields) continue;
      Function function = functions.get(entry.getKey());
      String base = entry.getKey().substring(entry.getKey().indexOf(':') + 1);
      println("FUNCTION " + function.getEntryPoint() + " " + function.getName()
          + " BASE " + base + " HITS " + entry.getValue());
      for (String line : lines.get(entry.getKey())) println("  " + line);
    }
  }
}
