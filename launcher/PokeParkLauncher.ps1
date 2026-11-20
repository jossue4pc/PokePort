# PokePark Wii PC Port - launcher (Windows PowerShell 5.1 + WPF, no installation required).
# Original artwork only: no Nintendo / Pokemon logos or characters are used or shipped.
Add-Type -AssemblyName PresentationFramework, PresentationCore, WindowsBase, System.Windows.Forms

$ErrorActionPreference = 'Stop'
$Root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$BuildScript = Join-Path $PSScriptRoot 'Build-PokePark.ps1'
$InstallDir = Join-Path $Root 'PokePark'
$GameExe = Join-Path $InstallDir 'PokePark.exe'
$ConfigPath = Join-Path $InstallDir 'UserData\Config.toml'
$LogDir = Join-Path $Root '.build'
[IO.Directory]::CreateDirectory($LogDir) | Out-Null
$SettingsPath = Join-Path $LogDir 'launcher.json'

# --- Strings -----------------------------------------------------------------------------------------
$es = (Get-UICulture).TwoLetterISOLanguageName -eq 'es'
function T([string]$EsText, [string]$EnText) { if ($es) { $EsText } else { $EnText } }
$S = @{
    Title       = 'PokéPark Wii'
    Subtitle    = T 'Port para PC hecho por fans - necesitas tu propia copia del juego' 'Fan-made PC port - requires your own copy of the game'
    DiscHeader  = T '1. Tu disco de PokéPark Wii (USA)' '1. Your PokéPark Wii disc (USA)'
    DiscHint    = T 'ISO, RVZ, WBFS, WIA, CISO o una carpeta ya extraída. Nunca sale de tu PC.' 'ISO, RVZ, WBFS, WIA, CISO or an extracted folder. It never leaves your PC.'
    Browse      = T 'Elegir disco...' 'Choose disc...'
    BrowseDir   = T 'Carpeta...' 'Folder...'
    StepsHeader = T '2. Preparar el port' '2. Build the port'
    Install     = T 'Instalar' 'Install'
    Reinstall   = T 'Reinstalar' 'Rebuild'
    Play        = T 'JUGAR' 'PLAY'
    OpenFolder  = T 'Abrir carpeta' 'Open folder'
    Shortcut    = T 'Acceso directo' 'Desktop shortcut'
    ShowLog     = T 'Ver registro' 'Show log'
    Options     = T 'Opciones' 'Options'
    Native60    = T '60 fps nativos' 'Native 60 fps'
    Widescreen  = T 'Mantener 16:9 (bandas negras)' 'Keep 16:9 (black bars)'
    Fullscreen  = T 'Pantalla completa' 'Fullscreen'
    Resolution  = T 'Resolución' 'Resolution'
    InGame      = T 'Dentro del juego: F10 abre los ajustes (controles, gráficos, sonido).' 'In game: F10 opens the settings (controls, graphics, audio).'
    Legal       = T 'Proyecto no oficial, sin relación con Nintendo, The Pokémon Company, Creatures ni Game Freak. No incluye ningún archivo del juego.' 'Unofficial project, not affiliated with Nintendo, The Pokémon Company, Creatures or Game Freak. No game files are included.'
    Ready       = T 'Listo para jugar.' 'Ready to play.'
    NotInstalled = T 'Elige tu disco y pulsa Instalar. La primera vez tarda entre 15 y 40 minutos.' 'Choose your disc and press Install. The first build takes 15-40 minutes.'
    Working     = T 'Trabajando...' 'Working...'
    Failed      = T 'Algo falló' 'Something failed'
    NeedDisc    = T 'Primero elige tu disco de PokéPark Wii (USA).' 'Choose your PokéPark Wii (USA) disc first.'
    Done        = T '¡Instalación terminada!' 'Installation finished!'
    Steps = [ordered]@{
        toolchain = T 'Herramientas de compilación' 'Compiler toolchain'
        deps      = T 'Bibliotecas (SDL, Dawn...)' 'Libraries (SDL, Dawn...)'
        disc      = T 'Leer y verificar tu disco' 'Read and verify your disc'
        translate = T 'Traducir el código del juego' 'Translate the game code'
        build     = T 'Compilar el port' 'Compile the port'
        install   = T 'Preparar la carpeta del juego' 'Assemble the game folder'
    }
}

# --- Window ------------------------------------------------------------------------------------------
[xml]$xaml = @"
<Window xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation"
        xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml"
        Title="PokéPark Wii - PC" Width="980" Height="680" ResizeMode="CanMinimize"
        WindowStartupLocation="CenterScreen" FontFamily="Segoe UI" Background="#7EC8F2">
  <Window.Resources>
    <Style x:Key="Pill" TargetType="Button">
      <Setter Property="Foreground" Value="White"/>
      <Setter Property="FontWeight" Value="Bold"/>
      <Setter Property="FontSize" Value="14"/>
      <Setter Property="Padding" Value="18,8"/>
      <Setter Property="Cursor" Value="Hand"/>
      <Setter Property="Template">
        <Setter.Value>
          <ControlTemplate TargetType="Button">
            <Border x:Name="b" CornerRadius="18" Background="{TemplateBinding Background}" BorderBrush="#33000000" BorderThickness="0,0,0,3" Padding="{TemplateBinding Padding}">
              <ContentPresenter HorizontalAlignment="Center" VerticalAlignment="Center"/>
            </Border>
            <ControlTemplate.Triggers>
              <Trigger Property="IsMouseOver" Value="True"><Setter TargetName="b" Property="Opacity" Value="0.88"/></Trigger>
              <Trigger Property="IsEnabled" Value="False"><Setter TargetName="b" Property="Opacity" Value="0.4"/></Trigger>
            </ControlTemplate.Triggers>
          </ControlTemplate>
        </Setter.Value>
      </Setter>
    </Style>
  </Window.Resources>
  <Grid>
    <!-- Sky, clouds and hills (original artwork) -->
    <Rectangle><Rectangle.Fill><LinearGradientBrush StartPoint="0,0" EndPoint="0,1">
      <GradientStop Color="#5DB7F0" Offset="0"/><GradientStop Color="#BFE8FF" Offset="0.75"/></LinearGradientBrush></Rectangle.Fill></Rectangle>
    <Canvas IsHitTestVisible="False">
      <Ellipse Canvas.Left="40" Canvas.Top="118" Width="150" Height="46" Fill="White" Opacity="0.75"/>
      <Ellipse Canvas.Left="90" Canvas.Top="98" Width="90" Height="56" Fill="White" Opacity="0.75"/>
      <Ellipse Canvas.Left="760" Canvas.Top="128" Width="170" Height="50" Fill="White" Opacity="0.7"/>
      <Ellipse Canvas.Left="810" Canvas.Top="104" Width="100" Height="60" Fill="White" Opacity="0.7"/>
      <Ellipse Canvas.Left="-160" Canvas.Top="560" Width="760" Height="260" Fill="#7CCB4E"/>
      <Ellipse Canvas.Left="420" Canvas.Top="575" Width="820" Height="260" Fill="#5FB33F"/>
    </Canvas>
    <Grid Margin="28,18,28,18">
      <Grid.RowDefinitions><RowDefinition Height="Auto"/><RowDefinition Height="*"/><RowDefinition Height="Auto"/></Grid.RowDefinitions>
      <!-- Header -->
      <Border Grid.Row="0" CornerRadius="26" Background="#FFD93B" BorderBrush="#E3A000" BorderThickness="0,0,0,5" Padding="20,10">
        <StackPanel Orientation="Horizontal">
          <Grid Width="58" Height="58" Margin="0,0,16,0">
            <Ellipse Fill="White"/><Ellipse Margin="5" Fill="#FFB400"/>
            <Path Fill="White" Stretch="Uniform" Margin="13,9" Data="M 6,0 L 0,7 L 4,7 L 2,13 L 9,5 L 5,5 L 8,0 Z"/>
          </Grid>
          <StackPanel VerticalAlignment="Center">
            <TextBlock x:Name="TitleText" FontSize="30" FontWeight="Black" Foreground="#2B4CA3"/>
            <TextBlock x:Name="SubtitleText" FontSize="13" Foreground="#5A4300"/>
          </StackPanel>
        </StackPanel>
      </Border>
      <!-- Body -->
      <Grid Grid.Row="1" Margin="0,16,0,10">
        <Grid.ColumnDefinitions><ColumnDefinition Width="3*"/><ColumnDefinition Width="18"/><ColumnDefinition Width="2*"/></Grid.ColumnDefinitions>
        <Border Grid.Column="0" CornerRadius="22" Background="#F2FFFFFF" Padding="20">
          <StackPanel>
            <TextBlock x:Name="DiscHeader" FontSize="16" FontWeight="Bold" Foreground="#2B4CA3"/>
            <DockPanel Margin="0,8,0,2">
              <Button x:Name="BrowseDirButton" DockPanel.Dock="Right" Style="{StaticResource Pill}" Background="#7A8CC4" Margin="6,0,0,0" Padding="12,6"/>
              <Button x:Name="BrowseButton" DockPanel.Dock="Right" Style="{StaticResource Pill}" Background="#3E7BE0" Margin="8,0,0,0" Padding="14,6"/>
              <TextBox x:Name="DiscBox" FontSize="13" Padding="8,6" BorderBrush="#BFD3F5" VerticalContentAlignment="Center"/>
            </DockPanel>
            <TextBlock x:Name="DiscHint" FontSize="11.5" Foreground="#6B7A90" TextWrapping="Wrap"/>
            <TextBlock x:Name="StepsHeader" FontSize="16" FontWeight="Bold" Foreground="#2B4CA3" Margin="0,16,0,6"/>
            <StackPanel x:Name="StepsPanel"/>
            <Grid Margin="0,14,0,0" Height="22">
              <Border CornerRadius="11" Background="#E4ECF7"/>
              <Border x:Name="ProgressFill" CornerRadius="11" Background="#FFC21A" HorizontalAlignment="Left" Width="0"/>
              <TextBlock x:Name="ProgressText" HorizontalAlignment="Center" VerticalAlignment="Center" FontWeight="Bold" Foreground="#5A4300" FontSize="12"/>
            </Grid>
            <TextBlock x:Name="StatusText" Margin="0,8,0,0" FontSize="12.5" Foreground="#34495E" TextWrapping="Wrap"/>
          </StackPanel>
        </Border>
        <Border Grid.Column="2" CornerRadius="22" Background="#F2FFFFFF" Padding="20">
          <DockPanel>
            <StackPanel DockPanel.Dock="Top">
              <Button x:Name="PlayButton" Style="{StaticResource Pill}" Background="#2FA84F" FontSize="26" Padding="10,14" Margin="0,0,0,12"/>
              <Button x:Name="InstallButton" Style="{StaticResource Pill}" Background="#3E7BE0" Margin="0,0,0,8"/>
              <UniformGrid Columns="2" Margin="0,0,0,8">
                <Button x:Name="FolderButton" Style="{StaticResource Pill}" Background="#7A8CC4" FontSize="12" Padding="6,6" Margin="0,0,4,0"/>
                <Button x:Name="ShortcutButton" Style="{StaticResource Pill}" Background="#7A8CC4" FontSize="12" Padding="6,6" Margin="4,0,0,0"/>
              </UniformGrid>
              <TextBlock x:Name="OptionsHeader" FontSize="15" FontWeight="Bold" Foreground="#2B4CA3" Margin="0,8,0,6"/>
              <CheckBox x:Name="Native60Box" Margin="0,3"/>
              <CheckBox x:Name="WidescreenBox" Margin="0,3"/>
              <CheckBox x:Name="FullscreenBox" Margin="0,3"/>
              <DockPanel Margin="0,6,0,0">
                <TextBlock x:Name="ResolutionLabel" VerticalAlignment="Center" Margin="0,0,8,0"/>
                <ComboBox x:Name="ResolutionBox" Width="150" HorizontalAlignment="Left"/>
              </DockPanel>
              <TextBlock x:Name="InGameText" Margin="0,10,0,0" FontSize="11.5" Foreground="#6B7A90" TextWrapping="Wrap"/>
              <Button x:Name="LogButton" Style="{StaticResource Pill}" Background="#9AA7BD" FontSize="12" Padding="6,5" Margin="0,12,0,0"/>
            </StackPanel>
            <TextBox x:Name="LogBox" Visibility="Collapsed" Margin="0,8,0,0" FontFamily="Consolas" FontSize="10.5" IsReadOnly="True"
                     VerticalScrollBarVisibility="Auto" HorizontalScrollBarVisibility="Auto" Background="#F8FAFD"/>
          </DockPanel>
        </Border>
      </Grid>
      <TextBlock x:Name="LegalText" Grid.Row="2" FontSize="11" Foreground="#F4FFF0" TextWrapping="Wrap" TextAlignment="Center"/>
    </Grid>
  </Grid>
</Window>
"@
$window = [Windows.Markup.XamlReader]::Load((New-Object System.Xml.XmlNodeReader $xaml))
$ui = @{}
foreach ($name in @('TitleText','SubtitleText','DiscHeader','DiscBox','BrowseButton','BrowseDirButton','DiscHint','StepsHeader','StepsPanel',
                    'ProgressFill','ProgressText','StatusText','PlayButton','InstallButton','FolderButton','ShortcutButton','OptionsHeader',
                    'Native60Box','WidescreenBox','FullscreenBox','ResolutionLabel','ResolutionBox','InGameText','LogButton','LogBox','LegalText')) {
    $ui[$name] = $window.FindName($name)
}
$iconPath = Join-Path $PSScriptRoot 'assets\icon.ico'
if (Test-Path $iconPath) { $window.Icon = [Windows.Media.Imaging.BitmapFrame]::Create([Uri]$iconPath) }

$ui.TitleText.Text = $S.Title
$ui.SubtitleText.Text = $S.Subtitle
$ui.DiscHeader.Text = $S.DiscHeader
$ui.DiscHint.Text = $S.DiscHint
$ui.BrowseButton.Content = $S.Browse
$ui.BrowseDirButton.Content = $S.BrowseDir
$ui.StepsHeader.Text = $S.StepsHeader
$ui.PlayButton.Content = [char]0x25B6 + '  ' + $S.Play
$ui.FolderButton.Content = $S.OpenFolder
$ui.ShortcutButton.Content = $S.Shortcut
$ui.OptionsHeader.Text = $S.Options
$ui.Native60Box.Content = $S.Native60
$ui.WidescreenBox.Content = $S.Widescreen
$ui.FullscreenBox.Content = $S.Fullscreen
$ui.ResolutionLabel.Text = $S.Resolution
$ui.InGameText.Text = $S.InGame
$ui.LogButton.Content = $S.ShowLog
$ui.LegalText.Text = $S.Legal

# --- Steps list --------------------------------------------------------------------------------------
$stepRows = @{}
foreach ($id in $S.Steps.Keys) {
    $row = New-Object Windows.Controls.StackPanel
    $row.Orientation = 'Horizontal'
    $row.Margin = '0,3,0,3'
    $dot = New-Object Windows.Controls.TextBlock
    $dot.Width = 26; $dot.FontSize = 15; $dot.FontWeight = 'Bold'; $dot.Text = [char]0x25CB; $dot.Foreground = '#9AA7BD'
    $label = New-Object Windows.Controls.TextBlock
    $label.FontSize = 13.5; $label.Text = $S.Steps[$id]; $label.Foreground = '#34495E'
    [void]$row.Children.Add($dot); [void]$row.Children.Add($label)
    [void]$ui.StepsPanel.Children.Add($row)
    $stepRows[$id] = $dot
}
function Set-StepState([string]$Id, [string]$State) {
    $dot = $stepRows[$Id]
    if (-not $dot) { return }
    switch ($State) {
        'running' { $dot.Text = [char]0x25CF; $dot.Foreground = '#FFB400' }
        'done'    { $dot.Text = [char]0x2714; $dot.Foreground = '#2FA84F' }
        'error'   { $dot.Text = [char]0x2716; $dot.Foreground = '#D9433B' }
        default   { $dot.Text = [char]0x25CB; $dot.Foreground = '#9AA7BD' }
    }
}
function Set-Progress([int]$Percent) {
    $Percent = [Math]::Max(0, [Math]::Min(100, $Percent))
    $parent = $ui.ProgressFill.Parent
    $width = $parent.ActualWidth
    if ($width -le 0) { $width = 520 }
    $ui.ProgressFill.Width = $width * $Percent / 100.0
    $ui.ProgressText.Text = "$Percent %"
}

# --- Settings (launcher + game config) ---------------------------------------------------------------
function Get-LauncherSettings {
    if (Test-Path $SettingsPath) { try { return Get-Content $SettingsPath -Raw | ConvertFrom-Json } catch {} }
    return [pscustomobject]@{ disc = '' }
}
function Save-LauncherSettings { [pscustomobject]@{ disc = $ui.DiscBox.Text } | ConvertTo-Json | Set-Content $SettingsPath }

function Get-ConfigText { if (Test-Path $ConfigPath) { ([IO.File]::ReadAllText($ConfigPath)).TrimStart([char]0xFEFF) } else { '' } }
function Set-ConfigValue([string]$Section, [string]$Key, [string]$Value) {
    if (-not (Test-Path $ConfigPath)) { return }
    $text = Get-ConfigText
    $pattern = "(?m)^$([regex]::Escape($Key))\s*=.*$"
    if ([regex]::IsMatch($text, $pattern)) {
        $text = [regex]::Replace($text, $pattern, "$Key = $Value")
    } elseif ([regex]::IsMatch($text, "(?m)^\[$Section\]\s*$")) {
        $text = [regex]::Replace($text, "(?m)^\[$Section\]\s*$", "[$Section]`r`n$Key = $Value", 1)
    } else {
        $text = $text.TrimEnd() + "`r`n`r`n[$Section]`r`n$Key = $Value`r`n"
    }
    [IO.File]::WriteAllText($ConfigPath, $text, (New-Object Text.UTF8Encoding($false)))
}
function Get-ConfigValue([string]$Key) {
    $m = [regex]::Match((Get-ConfigText), "(?m)^$([regex]::Escape($Key))\s*=\s*(.+?)\s*$")
    if ($m.Success) { return $m.Groups[1].Value.Trim('"') }
    return $null
}

$resolutions = [ordered]@{ 'Auto' = '0'; '1x (Wii)' = '1'; '2x' = '2'; '3x' = '3'; '4x' = '4' }
foreach ($k in $resolutions.Keys) { [void]$ui.ResolutionBox.Items.Add($k) }
$loadingOptions = $false
function Update-OptionsFromConfig {
    $script:loadingOptions = $true
    $installed = Test-Path $ConfigPath
    foreach ($c in @($ui.Native60Box, $ui.WidescreenBox, $ui.FullscreenBox, $ui.ResolutionBox)) { $c.IsEnabled = $installed }
    $ui.Native60Box.IsChecked = ((Get-ConfigValue 'native_60fps') -ne 'false')
    $ui.WidescreenBox.IsChecked = ((Get-ConfigValue 'force_aspect') -ne 'false')
    $ui.FullscreenBox.IsChecked = ((Get-ConfigValue 'display_mode') -eq 'borderless')
    $res = Get-ConfigValue 'resolution_multiplier'
    $selected = 'Auto'
    foreach ($k in $resolutions.Keys) { if ($res -and [double]$res -eq [double]$resolutions[$k]) { $selected = $k } }
    $ui.ResolutionBox.SelectedItem = $selected
    $script:loadingOptions = $false
}
$ui.Native60Box.Add_Click({ if (-not $loadingOptions) { Set-ConfigValue 'video' 'native_60fps' ($(if ($ui.Native60Box.IsChecked) { 'true' } else { 'false' })) } })
$ui.WidescreenBox.Add_Click({ if (-not $loadingOptions) { Set-ConfigValue 'video' 'force_aspect' ($(if ($ui.WidescreenBox.IsChecked) { 'true' } else { 'false' })) } })
$ui.FullscreenBox.Add_Click({ if (-not $loadingOptions) { Set-ConfigValue 'video' 'display_mode' ($(if ($ui.FullscreenBox.IsChecked) { '"borderless"' } else { '"windowed"' })) } })
$ui.ResolutionBox.Add_SelectionChanged({
    if (-not $loadingOptions -and $ui.ResolutionBox.SelectedItem) {
        Set-ConfigValue 'video' 'resolution_multiplier' $resolutions[[string]$ui.ResolutionBox.SelectedItem]
    }
})

# --- State -------------------------------------------------------------------------------------------
function Update-InstalledState {
    $installed = Test-Path $GameExe
    $ui.PlayButton.IsEnabled = $installed
    $ui.FolderButton.IsEnabled = $installed
    $ui.ShortcutButton.IsEnabled = $installed
    $ui.InstallButton.Content = $(if ($installed) { $S.Reinstall } else { $S.Install })
    if ($installed) {
        foreach ($id in $S.Steps.Keys) { Set-StepState $id 'done' }
        Set-Progress 100
        $ui.StatusText.Text = $S.Ready
    } else {
        Set-Progress 0
        $ui.StatusText.Text = $S.NotInstalled
    }
    Update-OptionsFromConfig
}

$settings = Get-LauncherSettings
if ($settings.disc) { $ui.DiscBox.Text = $settings.disc }

$ui.BrowseButton.Add_Click({
    $dialog = New-Object Microsoft.Win32.OpenFileDialog
    $dialog.Filter = (T 'Imagen de disco de Wii' 'Wii disc image') + ' (*.iso;*.rvz;*.wbfs;*.wia;*.ciso;*.gcz;*.nfs)|*.iso;*.rvz;*.wbfs;*.wia;*.ciso;*.gcz;*.nfs|' + (T 'Todos los archivos' 'All files') + ' (*.*)|*.*'
    if ($dialog.ShowDialog($window)) { $ui.DiscBox.Text = $dialog.FileName; Save-LauncherSettings }
})
$ui.BrowseDirButton.Add_Click({
    $dialog = New-Object System.Windows.Forms.FolderBrowserDialog
    $dialog.Description = T 'Carpeta con sys\main.dol y files\ (extraída con Dolphin)' 'Folder with sys\main.dol and files\ (extracted with Dolphin)'
    if ($dialog.ShowDialog() -eq [System.Windows.Forms.DialogResult]::OK) { $ui.DiscBox.Text = $dialog.SelectedPath; Save-LauncherSettings }
})

# --- Build process (output polled from a log file) ----------------------------------------------------
$script:process = $null
$script:logPath = $null
$script:logOffset = 0
$script:currentStep = $null
$script:failed = $false
$timer = New-Object Windows.Threading.DispatcherTimer
$timer.Interval = [TimeSpan]::FromMilliseconds(250)

function Add-Log([string]$Line) {
    $ui.LogBox.AppendText($Line + "`r`n")
    $ui.LogBox.ScrollToEnd()
}

function Read-NewLogLines {
    if (-not $script:logPath -or -not (Test-Path $script:logPath)) { return }
    $stream = [IO.File]::Open($script:logPath, 'Open', 'Read', 'ReadWrite')
    try {
        if ($stream.Length -le $script:logOffset) { return }
        [void]$stream.Seek($script:logOffset, 'Begin')
        $reader = New-Object IO.StreamReader($stream)
        $chunk = $reader.ReadToEnd()
        # Keep a trailing partial line for the next tick.
        $last = $chunk.LastIndexOf("`n")
        if ($last -lt 0) { return }
        $complete = $chunk.Substring(0, $last + 1)
        $script:logOffset += [Text.Encoding]::UTF8.GetByteCount($complete)
        foreach ($raw in $complete -split "`r?`n") {
            if ($raw -eq '') { continue }
            if ($raw.StartsWith('##STEP ')) {
                $parts = $raw.Substring(7).Split(' ', 2)
                $script:currentStep = $parts[0]
                Set-StepState $parts[0] 'running'
                $ui.StatusText.Text = $S.Steps[$parts[0]] + '...'
            } elseif ($raw.StartsWith('##DONE ')) {
                Set-StepState $raw.Substring(7).Trim() 'done'
            } elseif ($raw.StartsWith('##PROGRESS ')) {
                Set-Progress ([int]$raw.Substring(11))
            } elseif ($raw.StartsWith('##ERROR ')) {
                $script:failed = $true
                if ($script:currentStep) { Set-StepState $script:currentStep 'error' }
                $message = $raw.Substring(8)
                $ui.StatusText.Text = "$($S.Failed): $message"
                Add-Log "ERROR: $message"
            } elseif ($raw -eq '##FINISHED') {
                $ui.StatusText.Text = $S.Done
            } else {
                Add-Log $raw
            }
        }
    } finally {
        $stream.Dispose()
    }
}

$timer.Add_Tick({
    Read-NewLogLines
    if ($script:process -and $script:process.HasExited) {
        Read-NewLogLines
        $timer.Stop()
        $script:process = $null
        $ui.InstallButton.IsEnabled = $true
        $ui.BrowseButton.IsEnabled = $true
        $ui.BrowseDirButton.IsEnabled = $true
        if (-not $script:failed) {
            Update-InstalledState
            $ui.StatusText.Text = $S.Done
            [Windows.MessageBox]::Show($window, $S.Done, $S.Title) | Out-Null
        } else {
            $ui.LogBox.Visibility = 'Visible'
            $ui.PlayButton.IsEnabled = (Test-Path $GameExe)
        }
    }
})

$ui.InstallButton.Add_Click({
    $disc = $ui.DiscBox.Text.Trim()
    if (-not $disc) { [Windows.MessageBox]::Show($window, $S.NeedDisc, $S.Title) | Out-Null; return }
    Save-LauncherSettings
    foreach ($id in $S.Steps.Keys) { Set-StepState $id 'pending' }
    Set-Progress 0
    $ui.LogBox.Clear()
    $script:failed = $false
    $script:currentStep = $null
    $ui.StatusText.Text = $S.Working
    $ui.InstallButton.IsEnabled = $false
    $ui.PlayButton.IsEnabled = $false
    $ui.BrowseButton.IsEnabled = $false
    $ui.BrowseDirButton.IsEnabled = $false
    $script:logPath = Join-Path $LogDir ("build_{0:yyyyMMdd_HHmmss}.log" -f (Get-Date))
    $script:logOffset = 0
    $discArg = $(if (Test-Path -LiteralPath $disc -PathType Container) { "-ExtractedDir `"$disc`"" } else { "-Disc `"$disc`"" })
    $command = "& '$BuildScript' $discArg *>&1 | Out-File -LiteralPath '$($script:logPath)' -Encoding utf8 -Width 400"
    $script:process = Start-Process -FilePath 'powershell.exe' -PassThru -WindowStyle Hidden `
        -ArgumentList @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-Command', $command)
    $timer.Start()
})

$ui.PlayButton.Add_Click({
    if (Test-Path $GameExe) { Start-Process -FilePath $GameExe -WorkingDirectory $InstallDir }
})
$ui.FolderButton.Add_Click({ if (Test-Path $InstallDir) { Start-Process explorer.exe $InstallDir } })
$ui.ShortcutButton.Add_Click({
    $shell = New-Object -ComObject WScript.Shell
    $link = $shell.CreateShortcut((Join-Path ([Environment]::GetFolderPath('Desktop')) 'PokePark Wii (PC).lnk'))
    $link.TargetPath = $GameExe
    $link.WorkingDirectory = $InstallDir
    if (Test-Path $iconPath) { $link.IconLocation = $iconPath }
    $link.Save()
    [Windows.MessageBox]::Show($window, (T 'Acceso directo creado en el escritorio.' 'Shortcut created on the desktop.'), $S.Title) | Out-Null
})
$ui.LogButton.Add_Click({
    $ui.LogBox.Visibility = $(if ($ui.LogBox.Visibility -eq 'Visible') { 'Collapsed' } else { 'Visible' })
})

$window.Add_ContentRendered({
    Update-InstalledState
    # POKEPARK_LAUNCHER_SNAPSHOT=<file.png>: render the window to an image and close (for screenshots/tests).
    if ($env:POKEPARK_LAUNCHER_SNAPSHOT) {
        $window.Dispatcher.Invoke([Action]{}, [Windows.Threading.DispatcherPriority]::ContextIdle)
        $content = $window.Content
        $bitmap = New-Object Windows.Media.Imaging.RenderTargetBitmap ([int]$content.ActualWidth), ([int]$content.ActualHeight), 96, 96, ([Windows.Media.PixelFormats]::Pbgra32)
        $bitmap.Render($content)
        $encoder = New-Object Windows.Media.Imaging.PngBitmapEncoder
        $encoder.Frames.Add([Windows.Media.Imaging.BitmapFrame]::Create($bitmap))
        $stream = [IO.File]::Create($env:POKEPARK_LAUNCHER_SNAPSHOT)
        $encoder.Save($stream); $stream.Dispose()
        $window.Close()
    }
})
$window.Add_Closing({
    if ($script:process -and -not $script:process.HasExited) {
        $answer = [Windows.MessageBox]::Show($window, (T 'La instalación sigue en marcha. Cerrar la detendrá. ¿Cerrar igualmente?' 'The installation is still running. Closing stops it. Close anyway?'), $S.Title, 'YesNo')
        if ($answer -ne 'Yes') { $_.Cancel = $true; return }
        try { Stop-Process -Id $script:process.Id -Force } catch {}
    }
})
[void]$window.ShowDialog()
