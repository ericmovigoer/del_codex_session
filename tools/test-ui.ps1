param([ValidateSet('Debug','Release')][string]$Configuration = 'Release')
$ErrorActionPreference = 'Stop'
$workspace = Split-Path -Parent $PSScriptRoot
Add-Type @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class CleanerUI {
  private delegate bool WindowCallback(IntPtr w, IntPtr data);
  [DllImport("user32.dll")] private static extern bool EnumWindows(WindowCallback callback, IntPtr data);
  [DllImport("user32.dll")] private static extern uint GetWindowThreadProcessId(IntPtr w, out uint pid);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] private static extern int GetWindowText(IntPtr w, StringBuilder text, int count);
  [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr w, int id);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr SendMessage(IntPtr w, uint m, IntPtr a, IntPtr b);
  [DllImport("user32.dll", CharSet=CharSet.Unicode, EntryPoint="SendMessageW")] public static extern IntPtr GetText(IntPtr w, uint m, IntPtr a, StringBuilder b);
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr w, uint m, IntPtr a, IntPtr b);
  [DllImport("user32.dll")] public static extern bool IsWindowEnabled(IntPtr w);
  public static IntPtr FindProcessWindow(int pid, string title) {
    IntPtr found=IntPtr.Zero;
    EnumWindows(delegate(IntPtr w, IntPtr data) {
      uint owner; GetWindowThreadProcessId(w, out owner);
      if (owner!=(uint)pid) return true;
      var text=new StringBuilder(512); GetWindowText(w, text, text.Capacity);
      if (text.ToString()!=title) return true;
      found=w; return false;
    }, IntPtr.Zero);
    return found;
  }
  public static string Text(IntPtr w) {
    var n=(int)SendMessage(w, 0xE, IntPtr.Zero, IntPtr.Zero);
    var text=new StringBuilder(n+1); GetText(w, 0xD, (IntPtr)(n+1), text); return text.ToString();
  }
}
'@
function Wait-Until([scriptblock]$Condition, [string]$Description) {
    $timer = [Diagnostics.Stopwatch]::StartNew()
    while (!( & $Condition )) {
        if ($timer.Elapsed.TotalSeconds -gt 15) { throw "超时：$Description" }
        Start-Sleep -Milliseconds 40
    }
}
function Check([bool]$Value, [string]$Description) {
    if (!$Value) { throw "失败：$Description" }
    $script:checks++
    Write-Output "PASS: $Description"
}
function Select-File([int]$Index) {
    [void][CleanerUI]::SendMessage($script:list, 0x186, [IntPtr]$Index, [IntPtr]::Zero)
    [void][CleanerUI]::SendMessage($script:window, 0x111, [IntPtr](1001 + (1 -shl 16)), $script:list)
}
function Start-Delete {
    [void][CleanerUI]::PostMessage($script:window, 0x111, [IntPtr]1004, [IntPtr]::Zero)
    Wait-Until { [CleanerUI]::FindProcessWindow($process.Id, '确认永久删除') -ne [IntPtr]::Zero } '删除确认弹窗'
    return [CleanerUI]::FindProcessWindow($process.Id, '确认永久删除')
}
function Close-Dialog([IntPtr]$Dialog, [int]$Button) {
    $buttonWindow = [CleanerUI]::GetDlgItem($Dialog, $Button)
    if ($buttonWindow -eq [IntPtr]::Zero) { $buttonWindow = [CleanerUI]::GetDlgItem($Dialog, 2) }
    if ($buttonWindow -eq [IntPtr]::Zero) { throw '找不到弹窗按钮。' }
    [void][CleanerUI]::PostMessage($buttonWindow, 0xF5, [IntPtr]::Zero, [IntPtr]::Zero)
    Wait-Until { [CleanerUI]::IsWindowEnabled($script:window) } '弹窗关闭'
}
function Message([string]$Text) {
    return @{ type='response_item'; payload=@{ type='message'; role='user'; content=@(@{ type='input_text'; text=$Text }) } } | ConvertTo-Json -Depth 6 -Compress
}
function Metadata([string]$Id, [string]$ParentId = '') {
    $source = 'cli'
    if ($ParentId) { $source = @{ subagent=@{ thread_spawn=@{ parent_thread_id=$ParentId; depth=1 } } } }
    return @{ type='session_meta'; payload=@{ id=$Id; source=$source } } | ConvertTo-Json -Depth 8 -Compress
}
$checks = 0
$tempParent = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
$fixture = Join-Path $tempParent ('codex-cleaner-ui-' + [guid]::NewGuid().ToString('N'))
$process = $null
try {
    New-Item -ItemType Directory -Path $fixture | Out-Null
    $slow = Join-Path $fixture '01-中文慢预览.jsonl'
    $fast = Join-Path $fixture '02-快速预览.jsonl'
    $child = Join-Path $fixture '03-child.jsonl'
    $grandchild = Join-Path $fixture 'next-day\04-grandchild.jsonl'
    New-Item -ItemType Directory -Path (Split-Path -Parent $grandchild) | Out-Null
    [IO.File]::WriteAllText($child, (Metadata 'child' 'fast'), [Text.UTF8Encoding]::new($false))
    [IO.File]::WriteAllText($grandchild, (Metadata 'grandchild' 'child'), [Text.UTF8Encoding]::new($false))
    # 较大文件让切换时有机会取消仍在工作的旧预览。
    $stream = [IO.File]::CreateText($slow)
    try {
        $stream.WriteLine((Metadata 'slow'))
        $irrelevant = '{"type":"event_msg","payload":{"message":"ignore"}}'
        for ($i=0; $i -lt 150000; $i++) { $stream.WriteLine($irrelevant) }
        $stream.WriteLine((Message 'OLD_PREVIEW'))
    } finally { $stream.Dispose() }
    [IO.File]::WriteAllText($fast, ((Metadata 'fast') + "`n" + (Message "CURRENT_PREVIEW 中文`n第二行")), [Text.UTF8Encoding]::new($false))
    [IO.File]::SetLastWriteTimeUtc($slow, [DateTime]::UtcNow.AddMinutes(1))
    $exe = Join-Path $workspace "bin\x64\$Configuration\CodexSessionCleaner.exe"
    $process = Start-Process -FilePath $exe -ArgumentList @('--sessions-root', ('"' + $fixture + '"')) -WindowStyle Hidden -PassThru
    Wait-Until { [CleanerUI]::FindProcessWindow($process.Id, 'Codex 会话删除工具') -ne [IntPtr]::Zero } '主窗口'
    $script:window = [CleanerUI]::FindProcessWindow($process.Id, 'Codex 会话删除工具')
    $script:list = [CleanerUI]::GetDlgItem($window, 1001)
    $preview = [CleanerUI]::GetDlgItem($window, 1002)
    $remove = [CleanerUI]::GetDlgItem($window, 1004)
    $path = [CleanerUI]::GetDlgItem($window, 1005)
    Wait-Until { [CleanerUI]::SendMessage($list, 0x18B, [IntPtr]::Zero, [IntPtr]::Zero).ToInt32() -eq 2 } '扫描完成'
    Check ((Get-ChildItem -LiteralPath $fixture -Filter *.jsonl -File -Recurse).Count -eq 4) '四个文件仅显示两个主会话'
    Check (![CleanerUI]::IsWindowEnabled($remove)) '未选择时禁用删除'
    for ($i=0; $i -lt 20; $i++) { Select-File 0; Select-File 1 }
    Wait-Until { [CleanerUI]::Text($preview).Contains('CURRENT_PREVIEW') } '当前预览'
    Start-Sleep -Milliseconds 600
    Check (![CleanerUI]::Text($preview).Contains('OLD_PREVIEW')) '旧预览没有覆盖新选择'
    Check ([CleanerUI]::Text($preview).Contains("中文`r`n第二行")) '中文及多行预览'
    Check ([CleanerUI]::Text($path) -eq $fast) '完整路径'
    $dialog = Start-Delete
    $default = [CleanerUI]::SendMessage($dialog, 0x400, [IntPtr]::Zero, [IntPtr]::Zero).ToInt32() -band 0xffff
    Check ($default -eq 2) '删除默认选择取消'
    Check ([CleanerUI]::Text([CleanerUI]::GetDlgItem($dialog, 65535)).Contains($fast)) '弹窗显示完整路径'
    Check ([CleanerUI]::Text([CleanerUI]::GetDlgItem($dialog, 65535)).Contains('所有层级的子 Agent')) '弹窗说明级联删除范围'
    Close-Dialog $dialog 2
    Check ((Test-Path -LiteralPath $fast) -and (Test-Path -LiteralPath $child) -and (Test-Path -LiteralPath $grandchild)) '取消后主会话和所有子文件仍存在'
    # 最深层子文件只读，失败时主会话和其余子文件应保留。
    [IO.File]::SetAttributes($grandchild, [IO.FileAttributes]::ReadOnly)
    $dialog = Start-Delete
    [void][CleanerUI]::PostMessage([CleanerUI]::GetDlgItem($dialog, 1), 0xF5, [IntPtr]::Zero, [IntPtr]::Zero)
    Wait-Until { [CleanerUI]::FindProcessWindow($process.Id, '删除失败') -ne [IntPtr]::Zero } '失败提示'
    $failure = [CleanerUI]::FindProcessWindow($process.Id, '删除失败')
    Close-Dialog $failure 1
    Check ((Test-Path -LiteralPath $fast) -and (Test-Path -LiteralPath $child) -and (Test-Path -LiteralPath $grandchild) -and [CleanerUI]::SendMessage($list, 0x18B, [IntPtr]::Zero, [IntPtr]::Zero).ToInt32() -eq 2) '子文件删除失败时保留整组和列表项'
    [IO.File]::SetAttributes($grandchild, [IO.FileAttributes]::Normal)
    $dialog = Start-Delete
    Close-Dialog $dialog 1
    Wait-Until { [CleanerUI]::SendMessage($list, 0x18B, [IntPtr]::Zero, [IntPtr]::Zero).ToInt32() -eq 1 } '成功删除后更新列表'
    Check (!(Test-Path -LiteralPath $fast) -and !(Test-Path -LiteralPath $child) -and !(Test-Path -LiteralPath $grandchild)) '确认后删除主会话及所有层级子文件'
    Check (Test-Path -LiteralPath $slow) '另一主会话不受影响'
    Check ([CleanerUI]::Text($preview) -eq '') '删除成功清空预览'
    Check (![CleanerUI]::IsWindowEnabled($remove)) '删除成功禁用按钮'
    Select-File 0
    [IO.File]::Delete($slow)
    $dialog = Start-Delete
    Close-Dialog $dialog 1
    Wait-Until { [CleanerUI]::SendMessage($list, 0x18B, [IntPtr]::Zero, [IntPtr]::Zero).ToInt32() -eq 0 } '外部删除同步列表'
    Check (![CleanerUI]::IsWindowEnabled($remove)) '外部删除后禁用按钮'
    [void][CleanerUI]::SendMessage($window, 0x111, [IntPtr]1003, [IntPtr]::Zero)
    Wait-Until { [CleanerUI]::Text($preview).Contains('没有找到') } '空目录提示'
    Check $true '刷新空目录'
    [void][CleanerUI]::PostMessage($window, 0x10, [IntPtr]::Zero, [IntPtr]::Zero)
    Check ($process.WaitForExit(10000)) '后台任务正常退出'
    Write-Output "PASS: $checks UI checks ($Configuration); all deletion fixtures were temporary."
} finally {
    if ($process -and !$process.HasExited) { Stop-Process -Id $process.Id -Force }
    $resolvedFixture = [IO.Path]::GetFullPath($fixture)
    if (!$resolvedFixture.StartsWith($tempParent, [StringComparison]::OrdinalIgnoreCase) -or !(Split-Path -Leaf $resolvedFixture).StartsWith('codex-cleaner-ui-')) {
        throw '测试清理路径核对失败。'
    }
    if (Test-Path -LiteralPath $resolvedFixture) {
        Get-ChildItem -LiteralPath $resolvedFixture -File -Recurse | ForEach-Object { $_.IsReadOnly = $false }
        Remove-Item -LiteralPath $resolvedFixture -Recurse -Force
    }
}
