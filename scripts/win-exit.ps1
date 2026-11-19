# win-exit.ps1 - run a Windows program and print its FULL 32-bit exit code to stderr (WIN_EXIT=...).
# From WSL only the low 8 bits of a Windows exit code are visible, so an abnormal termination
# status such as 0xC0000005 (access violation) looks like an ordinary exit code 5.
# scripts/qemu.sh starts QEMU through this script when QEMU_WINEXIT=1. Diagnostic use only.
# (ASCII only on purpose: Windows PowerShell reads a script without a BOM in the ANSI code page.)
param([Parameter(Mandatory = $true)][string]$Exe,
      [Parameter(ValueFromRemainingArguments = $true)][string[]]$Rest)
& $Exe @Rest
$code = $LASTEXITCODE
$hex = '{0:X8}' -f $code
[Console]::Error.WriteLine("WIN_EXIT=$code (0x$hex)")
exit ($code -band 0xFF)
