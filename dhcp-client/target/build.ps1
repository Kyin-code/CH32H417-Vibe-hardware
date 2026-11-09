#
# build.ps1 -- CH32H417 dual-core firmware build.
#
#   powershell -File target/build.ps1 -What all     # V3F + V5F + Merge.bin
#   powershell -File target/build.ps1 -What v3f
#   powershell -File target/build.ps1 -What v5f
#   powershell -File target/build.ps1 -What diag    # V3F-only print test
#
# Layout of Merge.bin (burn at 0x08000000):
#   0x00000000..0x0000FFFF  V3F  application (DHCP + HTTP + mDNS), 64K window
#   0x00010000..            V5F  HSEM stub
#
# Only the peripherals actually used are compiled: pulling in all 38 vendor
# peripheral drivers overflows the 64K RAM_CODE region.
#
# IMPORTANT: paths use forward slashes and every path expression is wrapped in
# parentheses. PowerShell treats ',' as a higher-precedence operator than '+',
# so `@($a + '/x', $b + '/y')` silently builds ONE concatenated string.
#
param(
    [string]$What = 'all'
)

$root = 'I:/CH32H417'
$tc   = $root + '/tools/xpack-riscv-none-elf-gcc-15.2.0-1/xpack-riscv-none-elf-gcc-15.2.0-1/bin'
$evt  = $root + '/nanoCH32H417-main/doc/EVT/EXAM'
$dhcp = $root + '/dhcp-client'
$bld  = $dhcp + '/target/build'

$gcc     = $tc + '/riscv-none-elf-gcc.exe'
$objcopy = $tc + '/riscv-none-elf-objcopy.exe'
$size    = $tc + '/riscv-none-elf-size.exe'

if (-not (Test-Path $gcc)) { throw ('toolchain not found: ' + $gcc) }
New-Item -ItemType Directory -Force -Path $bld | Out-Null

$arch   = @('-march=rv32imafc_zicsr', '-mabi=ilp32')
$common = @('-msmall-data-limit=8', '-Os', '-fmessage-length=0', '-fsigned-char',
            '-ffunction-sections', '-fdata-sections', '-fno-common',
            '-Wno-attributes')

# Peripherals actually used.
$periphNames = @('ch32h417_gpio.c', 'ch32h417_rcc.c', 'ch32h417_usart.c',
                 'ch32h417_tim.c', 'ch32h417_exti.c', 'ch32h417_pwr.c',
                 'ch32h417_flash.c', 'ch32h417_hsem.c', 'ch32h417_eth.c',
                 'ch32h417_dbgmcu.c', 'ch32h417_dma.c')

function Run-Gcc {
    param([string[]]$GccArgs)
    $out = & $gcc @GccArgs 2>&1
    return @{ Code = $LASTEXITCODE; Text = ($out | Out-String) }
}

function Compile-Core {
    param(
        [string]$Tag,
        [string]$Core,
        [string]$RunCore,
        [string[]]$ExtraDefs,
        [string[]]$Srcs,
        [string[]]$IncDirs,
        [string]$Startup,
        [string]$Ld,
        [string]$OutName,
        [string[]]$Libs
    )

    $objDir = $bld + '/obj_' + $Tag
    New-Item -ItemType Directory -Force -Path $objDir | Out-Null

    $flags = $arch + $common + @('-DCore_' + $Core, ('-DRun_Core=' + $RunCore)) + $ExtraDefs

    # Our own net_config.h (2 UDP + 2 TCP + 1 listen) must be found before the
    # vendor one, so target/ goes first.
    $incDirsAll = @(($dhcp + '/target'), ($dhcp + '/target/ui')) + $IncDirs
    $inc = @()
    foreach ($d in $incDirsAll) { $inc += ('-I' + $d) }

    $objs = @()
    $fail = 0
    foreach ($s in $Srcs) {
        $n = [IO.Path]::GetFileNameWithoutExtension($s)
        $o = $objDir + '/' + $n + '.o'
        $objs += $o
        $r = Run-Gcc (@($flags) + @($inc) + @('-c', $s, '-o', $o))
        if ($r.Code -ne 0) {
            Write-Output ('  FAILED ' + [IO.Path]::GetFileName($s))
            ($r.Text -split "`n") | Where-Object { $_ -match 'error|Error' } |
                Select-Object -First 8 | ForEach-Object { Write-Output ('    ' + $_.Trim()) }
            $fail = 1
        }
    }
    $so = $objDir + '/startup.o'
    $objs += $so
    $r = Run-Gcc (@($flags) + @('-c', $Startup, '-o', $so))
    if ($r.Code -ne 0) { $fail = 1 }
    if ($fail -ne 0) { throw ('compile failed: ' + $Tag) }

    $elf = $bld + '/' + $OutName + '.elf'
    $map = $bld + '/' + $OutName + '.map'
    $lnk = @($arch) + @('-T', $Ld, '-nostartfiles') + $objs + $Libs +
           @('-Wl,--gc-sections', ('-Wl,-Map=' + $map), '-o', $elf, '-lm', '-lc',
             '-Wl,--no-warn-rwx-segments')
    $r = Run-Gcc $lnk
    if ($r.Code -ne 0) {
        Write-Output ('  LINK FAILED ' + $Tag)
        ($r.Text -split "`n") | Where-Object { $_ -match 'overflow|undefined|error' } |
            Select-Object -First 10 | ForEach-Object { Write-Output ('    ' + $_.Trim()) }
        throw ('link failed: ' + $Tag)
    }

    & $objcopy -O binary $elf ($bld + '/' + $OutName + '.bin') 2>&1 | Out-Null

    Write-Output ('--- ' + $OutName)
    & $size $elf 2>&1 | ForEach-Object { Write-Output ('    ' + $_) }
    Write-Output ('    bin = ' + (Get-Item ($bld + '/' + $OutName + '.bin')).Length + ' B')
}

$periph = @()
foreach ($n in $periphNames) {
    $p = $evt + '/SRC/Peripheral/src/' + $n
    if (Test-Path $p) { $periph += $p } else { Write-Output ('  (missing peripheral ' + $n + ')') }
}

$baseSrc = @(($dhcp + '/target/core_riscv.c'), ($evt + '/SRC/Debug/debug.c')) + $periph
$baseInc = @(($evt + '/SRC/Core'), ($evt + '/SRC/Debug'), ($evt + '/SRC/Peripheral/inc'),
             ($evt + '/ETH/NetLib'))

# --------------------------------------------------------------------------
if ($What -eq 'diag') {
    $srcs = $baseSrc + @(($dhcp + '/target/main_v3f_diag.c'), ($dhcp + '/target/uprint.c'))
    Compile-Core -Tag 'diag' -Core 'V3F' -RunCore '0' -ExtraDefs @() `
        -Srcs $srcs -IncDirs ($baseInc + @(($evt + '/ETH/DHCP/V3F/User'))) `
        -Startup ($evt + '/SRC/Startup/startup_ch32h417_v3f.S') `
        -Ld ($evt + '/ETH/DHCP/Common/Ld/V3F/Link_v3f.ld') -OutName 'diag' -Libs @()
    return
}

# --------------------------------------------------------------------------
# V3F: DHCP + HTTP upload + mDNS
# --------------------------------------------------------------------------
if ($What -eq 'v3f' -or $What -eq 'all') {
    $srcs = $baseSrc + @(
        ($evt + '/ETH/DHCP/V3F/User/system_ch32h417.c'),
        ($dhcp + '/target/ch32h417_it.c'),
        ($dhcp + '/target/main_v3f_eth.c'),
        ($dhcp + '/target/uprint.c'),
        ($dhcp + '/target/fw_upgrade.c'),
        ($dhcp + '/target/http_srv.c'),
        ($dhcp + '/target/mdns.c'),
        ($dhcp + '/examples/lease_apply_policy.c'),
        ($evt + '/ETH/NetLib/eth_driver_100M.c')
    )
    Compile-Core -Tag 'v3f' -Core 'V3F' -RunCore '2' `
        -ExtraDefs @('-DDEBUG=DEBUG_UART1') `
        -Srcs $srcs `
        -IncDirs ($baseInc + @(($evt + '/ETH/DHCP/V3F/User'), ($evt + '/ETH/DHCP/Common'),
                               ($dhcp + '/include'), ($dhcp + '/ports/ch32h417'),
                               ($dhcp + '/examples'))) `
        -Startup ($evt + '/SRC/Startup/startup_ch32h417_v3f.S') `
        -Ld ($evt + '/ETH/DHCP/Common/Ld/V3F/Link_v3f.ld') -OutName 'v3f' `
        -Libs @(($evt + '/ETH/NetLib/libwchnet.a'))
}

# --------------------------------------------------------------------------
# V5F: HSEM stub only
# --------------------------------------------------------------------------
if ($What -eq 'v5f' -or $What -eq 'all') {
    $srcs = $baseSrc + @(
        ($evt + '/ETH/DHCP/V5F/User/system_ch32h417.c'),
        ($dhcp + '/target/ch32h417_it.c'),
        ($dhcp + '/target/uprint.c'),
        ($dhcp + '/target/main_v5f_helper.c')
    )
    Compile-Core -Tag 'v5f' -Core 'V5F' -RunCore '2' `
        -ExtraDefs @('-DDEBUG=DEBUG_UART1') `
        -Srcs $srcs `
        -IncDirs ($baseInc + @(($evt + '/ETH/DHCP/V5F/User'))) `
        -Startup ($evt + '/SRC/Startup/startup_ch32h417_v5f.S') `
        -Ld ($evt + '/ETH/DHCP/Common/Ld/V5F/Link_v5f.ld') -OutName 'v5f' -Libs @()
}

# --------------------------------------------------------------------------
if ($What -eq 'all') {
    $v3 = [IO.File]::ReadAllBytes($bld + '/v3f.bin')
    $v5 = [IO.File]::ReadAllBytes($bld + '/v5f.bin')
    $split = 0x10000

    if ($v3.Length -gt $split) { throw ('V3F image too big: ' + $v3.Length) }

    $merge = New-Object byte[] ($split + $v5.Length)
    for ($i = 0; $i -lt $merge.Length; $i++) { $merge[$i] = 0xFF }
    [Array]::Copy($v3, 0, $merge, 0, $v3.Length)
    [Array]::Copy($v5, 0, $merge, $split, $v5.Length)
    [IO.File]::WriteAllBytes($bld + '/Merge.bin', $merge)

    Write-Output ''
    Write-Output '=== Merge ==='
    Write-Output ('  v3f   = ' + $v3.Length + ' B   (@0x00000000)')
    Write-Output ('  v5f   = ' + $v5.Length + ' B   (@0x00010000)')
    Write-Output ('  Merge = ' + $merge.Length + ' B   -> burn at 0x08000000')
}
