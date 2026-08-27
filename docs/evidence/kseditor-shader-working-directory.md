# ksEditor shader working-directory evidence

## Scope

This note records how the installed editor selects the process directory for
relative shader paths. It separates observed binary behavior from Windows
launch behavior that is not encoded in the binaries.

Installed files:

- `ksEditor.exe` SHA-256:
  `7df6a75e7b8be9c6aae7f0ac09a66ac904a06f2a7e22fdbef635aec96c5144a0`
- `ksNet.dll` SHA-256:
  `b38dcb826a3311d7233cf0a6a58e5da16b6c8679f8490091e7b434bf730091ca`
- `ksEditor.bat` SHA-256:
  `283a3dcf7f207d405d560b693716c9ead02c880f497b72ba6f6cfff37a600178`

## Batch-file launch

The installed `ksEditor.bat` contains one command:

```bat
start "" "D:\SteamLibrary\SteamLibrary\steamapps\common\assettocorsa\sdk\editor\ksEditor.exe"
```

The file does not call `cd`, `SetCurrentDirectory`, or an equivalent command.
The command gives Windows an absolute executable path. It does not give the
process an explicit working directory.

## Managed startup

The managed `ksEditor.Main` method has metadata RVA `0x24ab8`. Its startup IL
performs these operations:

1. Enable application visual styles.
2. Set compatible text rendering.
3. Construct the main form.
4. Call `Application.Run`.

This startup sequence does not read a command-line shader root. It does not
change the process current directory.

After `Application.Run` returns, the same method prepares a restart process.
It sets these `ProcessStartInfo` fields:

- `FileName`
- `UseShellExecute = true`
- `WorkingDirectory = FileInfo.Directory`

The `FileInfo` value comes from `Assembly.GetExecutingAssembly().Location`.
The call to `set_WorkingDirectory` uses MemberRef token `0x0A000396`. The call
is near IL offset `0x2f6`, and `Process.Start` follows near `0x2fd`.

This code sets the child process directory during a restart. It does not
change the current directory of the running editor.

The managed image has one `Directory.GetCurrentDirectory` reference. Its use
is in a non-`Main` constructor with method-body metadata RVA near `0x19b38`.
The call site is near `0x00419b51`. The managed image has no
`Directory.SetCurrentDirectory` MemberRef.

## Native startup

`ksNet.dll` imports `GetCurrentDirectoryA`. Its only import reference is in
`StackWalker::LoadModules` near `0x1006798a`. This code is not part of shader
discovery.

`ksNet.dll` does not import `SetCurrentDirectory`. The inspected native image
also has no direct call reference from startup code to
`GraphicsManager::initShaders` at `0x100456e9`.

The shader initializer therefore still consumes its process-relative literal:

```text
system/shaders/*.shader
```

## Verdict

The inspected editor code does not establish a separate shader root before
normal shader discovery. The process supplies the absolute root through its
inherited Windows current directory.

The batch file does not prove what that inherited directory is. Normal shell
or shortcut launch behavior can select it, but that behavior is external to
the inspected binaries. A claim that the batch file always makes the editor
directory current would therefore exceed the evidence.

The C++ port accepts an explicit shader-package path. This is a portable
application boundary. It is not labeled as an exact reproduction of the
original working-directory behavior.
