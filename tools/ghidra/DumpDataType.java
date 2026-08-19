// Dump a PDB-backed structure or enum by its exact or trailing name.
// Usage: -postScript DumpDataType.java Material
import ghidra.app.script.GhidraScript;
import ghidra.program.model.data.*;
import java.util.Iterator;

public class DumpDataType extends GhidraScript {
  @Override public void run() throws Exception {
    String needle = getScriptArgs().length == 0 ? "" : getScriptArgs()[0];
    Iterator<DataType> types = currentProgram.getDataTypeManager().getAllDataTypes();
    while (types.hasNext()) {
      DataType type = types.next();
      if (!(type.getName().equals(needle) || type.getPathName().endsWith("/" + needle))) continue;
      println("===== " + type.getPathName() + " length=" + type.getLength() + " =====");
      if (type instanceof Structure structure) {
        for (DataTypeComponent component : structure.getDefinedComponents()) {
          println(String.format("0x%04x %4d %-28s %s", component.getOffset(), component.getLength(),
            component.getFieldName(), component.getDataType().getDisplayName()));
        }
      } else if (type instanceof ghidra.program.model.data.Enum enumType) {
        for (String name : enumType.getNames()) println(name + " = " + enumType.getValue(name));
      } else println(type.getDescription());
    }
  }
}
