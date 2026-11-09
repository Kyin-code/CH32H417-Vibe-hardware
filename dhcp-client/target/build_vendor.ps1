#
# build_vendor.ps1 -- rebuild the VENDOR DHCP example from source so the build
# flow itself can be validated against demo/ETH_DHCP.Bin.
#
#   powershell -File target/build_vendor.ps1 -RunCore 2 -Tree muse
#
# Key finding this is based on: every string in demo/ETH_DHCP.Bin
# ("DHCP Test", "WCHNET_LibInit Success", "IPAddr:", "DHCP Success") sits in
# the V3F region (0xB100..0xB2D0), and the demo's V5F region is only ~10 KB.
# So the vendor demo runs Run_Core = Run_Core_V3FandV5F with the Ethernet stack
# on V3F, and V5F only does the HSEM handshake.
#
param(
    [int]$RunCore = 2,
    [string]$Tree = 'muse'
)

$root = 'I:/CH32H417'
$tc   = $root + '/tools/xpack-riscv-none-elf-gcc-15.2.0-1/xpack-riscv-none-elf-gcc-15.2.0-1/bin'
$dhcp = $root + '/dhcp-client'
$bld  = $dhcp + '/target/build'

if ($Tree -eq 'wch') {
    $evt = $root + '/nanoCH32H417-main/wch/EVT/EXAM'
} else {
    $evt = $root + '/nanoCH32H417-main/doc/EVT/EXAM'
}

$gcc     = $tc + '/riscv-none-elf-gcc.exe'
$objcopy = $tc + '/riscv-none-elf-objcopy.exe'
$size    = $tc + '/riscv-none-elf-size.exe'

New-Item -ItemType Directory -Force -Path $bld | Out-Null

$arch   = @('-march=rv32imafc_zicsr', '-mabi=ilp32')
$common = @('-msmall-data-limit=8', '-Os', '-fmessage-length=0', '-fsigned-char',
            '-ffunction-sections', '-fdata-sections', '-fno-common',
            '-Wno-attributes')

function Run-Gcc {
    param([string[]]$GccArgs)
    $out = & $gcc @GccArgs 2>&1
    return @{ Code = $LASTEXITCODE; Text = ($out | Out-String) }
}

function Build-Core {
    param([string]$Core, [string]$DefsRun, [string]$Ld, [string]$Startup, [string]$Out)

    $objDir = $bld + '/obj_vendor_' + $Out
    New-Item -ItemType Directory -Force -Path $objDir | Out-Null

    $flags = $arch + $common + @('-DCore_' + $Core, ('-DRun_Core=' + $DefsRun))

    $inc = @()
    foreach ($d in @(
        ($evt + '/SRC/Core'), ($evt + '/SRC/Debug'), ($evt + '/SRC/Peripheral/inc'),
        ($evt + '/ETH/DHCP/Common'), ($evt + '/ETH/NetLib'),
        ($evt + '/ETH/DHCP/' + $Core + '/User')
    )) { $inc += ('-I' + $d) }

    $periph = (Get-ChildItem ($evt + '/SRC/Peripheral/src') -Filter *.c | ForEach-Object { $_.FullName })

    # core_riscv.c from our copy: the MuseLab tree still spells the WCH private
    # CSR "mucounteren", unknown to current binutils. WCH renamed it to the
    # standard "mcounteren" (= 0x306), so the copy is semantically identical.
    $coreSrc = $dhcp + '/target/core_riscv.c'
    if (-not (Test-Path $coreSrc)) { $coreSrc = $evt + '/SRC/Core/core_riscv.c' }

    $srcs = @($coreSrc, ($evt + '/SRC/Debug/debug.c')) + $periph + @(
        ($evt + '/ETH/DHCP/' + $Core + '/User/system_ch32h417.c'),
        ($evt + '/ETH/DHCP/' + $Core + '/User/ch32h417_it.c'),
        ($evt + '/ETH/DHCP/' + $Core + '/User/main.c'),
        ($evt + '/ETH/DHCP/Common/hardware.c'),
        ($evt + '/ETH/NetLib/eth_driver_100M.c')
    )

    $objs = @()
    $fail = 0
    foreach ($s in $srcs) {
        $n = [IO.Path]::GetFileNameWithoutExtension($s)
        $o = $objDir + '/' + $n + '.o'
        $objs += $o
        $r = Run-Gcc (@($flags) + @($inc) + @('-c', $s, '-o', $o))
        if ($r.Code -ne 0) {
            Write-Output ('  FAILED ' + [IO.Path]::GetFileName($s))
            Write-Output ('    ' + ($r.Text -split "`n" | Select-Object -First 6) -join "`n    ")
            $fail = 1
        }
    }
    $so = $objDir + '/startup.o'
    $objs += $so
    $r = Run-Gcc (@($flags) + @('-c', $Startup, '-o', $so))
    if ($r.Code -ne 0) { $fail = 1 }
    if ($fail -ne 0) { throw ('compile failed: ' + $Out) }

    $elf = $bld + '/' + $Out + '.elf'
    $lnk = @($arch) + @('-T', $Ld, '-nostartfiles') + $objs +
           @(($evt + '/ETH/NetLib/libwchnet.a')) +
           @('-Wl,--gc-sections', ('-Wl,-Map=' + $bld + '/' + $Out + '.map'),
             '-o', $elf, '-lm', '-lc', '-Wl,--no-warn-rwx-segments')
    $r = Run-Gcc $lnk
    if ($r.Code -ne 0) {
        Write-Output ('  LINK FAILED ' + $Out)
        Write-Output ($r.Text -split "`n" | Select-Object -First 10)
        throw ('link failed: ' + $Out)
    }

    & $objcopy -O binary $elf ($bld + '/' + $Out + '.bin') 2>&1 | Out-Null
    Write-Output ('--- ' + $Out)
    & $size $elf 2>&1 | ForEach-Object { Write-Output ('    ' + $_) }
    Write-Output ('    bin = ' + (Get-Item ($bld + '/' + $Out + '.bin')).Length + ' B')
}

Build-Core -Core 'V3F' -DefsRun $RunCore `
    -Ld ($evt + '/ETH/DHCP/Common/Ld/V3F/Link_v3f.ld') `
    -Startup ($evt + '/SRC/Startup/startup_ch32h417_v3f.S') -Out 'vendor_v3f'

Build-Core -Core 'V5F' -DefsRun $RunCore `
    -Ld ($evt + '/ETH/DHCP/Common/Ld/V5F/Link_v5f.ld') `
    -Startup ($evt + '/SRC/Startup/startup_ch32h417_v5f.S') -Out 'vendor_v5f'

$v3 = [IO.File]::ReadAllBytes($bld + '/vendor_v3f.bin')
$v5 = [IO.File]::ReadAllBytes($bld + '/vendor_v5f.bin')
$split = 0x10000
$merge = New-Object byte[] ($split + $v5.Length)
for ($i = 0; $i -lt $merge.Length; $i++) { $merge[$i] = 0xFF }
[Array]::Copy($v3, 0, $merge, 0, [Math]::Min($v3.Length, $split))
[Array]::Copy($v5, 0, $merge, $split, $v5.Length)
[IO.File]::WriteAllBytes($bld + '/vendor_Merge.bin', $merge)

Write-Output ''
Write-Output '=== merge ==='
Write-Output ('  v3f   = ' + $v3.Length + ' B')
Write-Output ('  v5f   = ' + $v5.Length + ' B')
Write-Output ('  merge = ' + $merge.Length + ' B')

$demoPath = $root + '/nanoCH32H417-main/demo/ETH_DHCP.Bin'
if (Test-Path $demoPath) {
    $demo = [IO.File]::ReadAllBytes($demoPath)
    Write-Output ''
    Write-Output '=== compare with demo/ETH_DHCP.Bin ==='
    Write-Output ('  demo = ' + $demo.Length + ' B   mine = ' + $merge.Length + ' B')
    $d3 = 0; $d5 = 0
    for ($i = 0; $i -lt $demo.Length; $i++) { if ($demo[$i] -ne 0xFF) { if ($i -lt $split) { $d3++ } else { $d5++ } } }
    $m3 = 0; $m5 = 0
    for ($i = 0; $i -lt $merge.Length; $i++) { if ($merge[$i] -ne 0xFF) { if ($i -lt $split) { $m3++ } else { $m5++ } } }
    Write-Output ('  demo non-FF: V3F=' + $d3 + '  V5F=' + $d5)
    Write-Output ('  mine non-FF: V3F=' + $m3 + '  V5F=' + $m5)
    $same = 0
    $n = [Math]::Min($demo.Length, $merge.Length)
    for ($i = 0; $i -lt $n; $i++) { if ($demo[$i] -eq $merge[$i]) { $same++ } }
    Write-Output ('  byte-identical: ' + $same + '/' + $n + ' = ' + [Math]::Round(100.0 * $same / $n, 1) + '%')
}
