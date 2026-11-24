# win-exit.ps1 - run a Windows program and print its FULL 32-bit exit code to stderr (WIN_EXIT=...).
# From WSL only the low 8 bits of a Windows exit code are visible, so an abnormal termination
# status such as 0xC0000005 (access violation) looks like an ordinary exit code 5.
# scripts/qemu.sh starts QEMU through this script when QEMU_WINEXIT=1. Diagnostic use only.
# (ASCII only on purpose: Windows PowerShell reads a script without a BOM in the ANSI code page.)
# -WinExitFile <path>: write the WIN_EXIT line to that file instead of stderr. Step 73: when stderr and the
# guest serial output (stdout) went to the same log, the two writers interleaved byte by byte and once
# split "exit: code 1" into "eWIN_EXIT=3 ..." + "xit: code 1". A separate file cannot interleave.
param([string]$WinExitFile = "",
      [Parameter(Mandatory = $true)][string]$Exe,
      [Parameter(ValueFromRemainingArguments = $true)][string[]]$Rest)
& $Exe @Rest
$code = $LASTEXITCODE
$hex = '{0:X8}' -f $code
$line = "WIN_EXIT=$code (0x$hex)"
if ($WinExitFile -ne "") {
    [System.IO.File]::WriteAllText($WinExitFile, "$line`n")
} else {
    [Console]::Error.WriteLine($line)
}
exit ($code -band 0xFF)
