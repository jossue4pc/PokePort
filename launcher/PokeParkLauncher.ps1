# PokePark Wii PC Port - launcher (Windows PowerShell 5.1 + WPF, no installation required).
# Original artwork only: no Nintendo / Pokemon logos or characters are used or shipped.
param(
    # Set by the launcher itself after an update: rebuild the game automatically.
    [switch]$AfterUpdate
)
Add-Type -AssemblyName PresentationFramework, PresentationCore, WindowsBase, System.Windows.Forms, System.Net.Http

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


$S.CheckUpdates  = T 'Buscar actualizaciones' 'Check for updates'
$S.Checking      = T 'Buscando...' 'Checking...'
$S.NewVersion    = T 'Nueva versión disponible: {0}' 'New version available: {0}'
$S.SeeNotes      = T 'Ver notas' 'Release notes'
$S.UpdateNow     = T 'Actualizar' 'Update'
$S.Later         = T 'Ahora no' 'Not now'
$S.Skip          = T 'Omitir esta versión' 'Skip this version'
$S.UpToDate      = T 'Ya tienes la última versión ({0}).' 'You have the latest version ({0}).'
$S.CheckFailed   = T 'No se pudo consultar GitHub: {0}' 'Could not reach GitHub: {0}'
$S.Updating      = T 'Actualizando PokePort...' 'Updating PokePort...'
$S.UpdateDone    = T 'Actualización descargada. El lanzador se reiniciará y recompilará el juego (tu partida y ajustes se conservan).' 'Update downloaded. The launcher will restart and rebuild the game (your saves and settings are kept).'
$S.UpdateFailed  = T 'La actualización falló' 'The update failed'
$S.CloseGame     = T 'Cierra PokéPark antes de continuar.' 'Close PokéPark before continuing.'
$S.Busy          = T 'Espera a que termine la tarea en curso.' 'Wait for the current task to finish.'
$S.Rebuilding    = T 'Aplicando la actualización: recompilando solo lo que cambió...' 'Applying the update: rebuilding only what changed...'
$S.NotesTitle    = T 'Novedades de {0}' 'What is new in {0}'
$S.Close         = T 'Cerrar' 'Close'

$S.GraphicsApi   = T 'API gráfica' 'Graphics API'
$S.Diagnostic    = T 'Modo diagnóstico' 'Diagnostic mode'
$S.DiagStart     = T 'El juego se abrirá registrando el rendimiento. Juega normalmente unos minutos (pasa por donde notes tirones) y ciérralo: se mostrará un resumen y el archivo para enviarlo.' 'The game will open while recording performance. Play normally for a few minutes (go where you notice stutters) and close it: a summary and the file to share will be shown.'
$S.DiagTitle     = T 'Resumen del diagnóstico' 'Diagnostic summary'
$S.OpenLogFolder = T 'Abrir carpeta del registro' 'Open log folder'

$S.ShareShaders  = T 'Compartir caché de shaders' 'Share shader cache'
$S.ShareDone     = T 'Listo: {0}{1}{1}Envíalo al autor del port (o adjúntalo en un issue de GitHub). Solo contiene la lista de efectos gráficos que ya se prepararon en tu PC; nada de tus partidas ni del juego. Con estas listas el port compila los shaders al arrancar y nadie tiene tirones la primera vez.' 'Done: {0}{1}{1}Send it to the port author (or attach it to a GitHub issue). It only lists the graphics effects already prepared on your PC; no saves or game data. These lists let the port compile shaders at start so nobody stutters the first time.'
$S.ShareEmpty    = T 'Todavía no hay caché de shaders: juega un rato primero.' 'There is no shader cache yet: play for a while first.'

$S.NeedsRebuild  = T 'Hay archivos de la versión {0}: pulsa Reinstalar para aplicarla (unos minutos; tu partida se conserva). Mientras tanto puedes jugar con la versión anterior.' 'Files of version {0} are present: press Rebuild to apply it (a few minutes; your saves are kept). Until then you can keep playing the previous build.'

$S.FrameMode     = T 'Fotogramas' 'Frame rate'
$S.FrameModes    = [ordered]@{
    native60 = T '60 nativos (PC potente)' '60 native (strong PC)'
    interp60 = T '30 + interpolación a 60 (portátiles)' '30 + interpolated to 60 (laptops)'
    original = T '30 (como en Wii)' '30 (as on Wii)'
}

$RepoApi = 'https://api.github.com/repos/jossue4pc/PokePort/releases/latest'
# Test hook: point the update check at another endpoint (e.g. a local server with a fake release).
if ($env:POKEPORT_UPDATE_API) { $RepoApi = $env:POKEPORT_UPDATE_API }
$VersionFile = Join-Path $Root 'VERSION'
$LocalVersion = $(if (Test-Path $VersionFile) { (Get-Content -LiteralPath $VersionFile -Raw).Trim() } else { '0.0.0' })
$UpdateScript = Join-Path $PSScriptRoot 'Update-PokePort.ps1'

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
      <Grid.RowDefinitions><RowDefinition Height="Auto"/><RowDefinition Height="Auto"/><RowDefinition Height="*"/><RowDefinition Height="Auto"/></Grid.RowDefinitions>
      <!-- Header -->
      <Border Grid.Row="0" CornerRadius="26" Background="#FFD93B" BorderBrush="#E3A000" BorderThickness="0,0,0,5" Padding="20,10">
        <DockPanel>
          <StackPanel DockPanel.Dock="Right" VerticalAlignment="Center" HorizontalAlignment="Right">
            <TextBlock x:Name="VersionText" HorizontalAlignment="Right" FontSize="12" FontWeight="Bold" Foreground="#5A4300" Margin="0,0,4,4"/>
            <Button x:Name="CheckUpdatesButton" Style="{StaticResource Pill}" Background="#2B4CA3" FontSize="12" Padding="12,5"/>
          </StackPanel>
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
        </DockPanel>
      </Border>
      <!-- Update banner -->
      <Border x:Name="UpdateBanner" Grid.Row="1" Visibility="Collapsed" CornerRadius="18" Background="#2B4CA3" Padding="16,10" Margin="0,12,0,0">
        <DockPanel>
          <StackPanel DockPanel.Dock="Right" Orientation="Horizontal">
            <Button x:Name="NotesButton" Style="{StaticResource Pill}" Background="#7A8CC4" FontSize="12" Padding="12,5" Margin="6,0,0,0"/>
            <Button x:Name="UpdateButton" Style="{StaticResource Pill}" Background="#2FA84F" FontSize="12" Padding="14,5" Margin="6,0,0,0"/>
            <Button x:Name="LaterButton" Style="{StaticResource Pill}" Background="#5C6F9E" FontSize="12" Padding="12,5" Margin="6,0,0,0"/>
            <Button x:Name="SkipButton" Style="{StaticResource Pill}" Background="#5C6F9E" FontSize="12" Padding="12,5" Margin="6,0,0,0"/>
          </StackPanel>
          <TextBlock x:Name="UpdateText" VerticalAlignment="Center" Foreground="White" FontSize="14" FontWeight="Bold" TextWrapping="Wrap"/>
        </DockPanel>
      </Border>
      <!-- Body -->
      <Grid Grid.Row="2" Margin="0,16,0,10">
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
              <DockPanel Margin="0,2,0,4">
                <TextBlock x:Name="FrameModeLabel" VerticalAlignment="Center" Margin="0,0,8,0"/>
                <ComboBox x:Name="FrameModeBox" Width="215" HorizontalAlignment="Left"/>
              </DockPanel>
              <CheckBox x:Name="Native60Box" Margin="0,3" Visibility="Collapsed"/>
              <CheckBox x:Name="WidescreenBox" Margin="0,3"/>
              <CheckBox x:Name="FullscreenBox" Margin="0,3"/>
              <DockPanel Margin="0,6,0,0">
                <TextBlock x:Name="ResolutionLabel" VerticalAlignment="Center" Margin="0,0,8,0"/>
                <ComboBox x:Name="ResolutionBox" Width="150" HorizontalAlignment="Left"/>
              </DockPanel>
              <DockPanel Margin="0,6,0,0">
                <TextBlock x:Name="ApiLabel" VerticalAlignment="Center" Margin="0,0,8,0"/>
                <ComboBox x:Name="ApiBox" Width="150" HorizontalAlignment="Left"/>
              </DockPanel>
              <TextBlock x:Name="InGameText" Margin="0,10,0,0" FontSize="11.5" Foreground="#6B7A90" TextWrapping="Wrap"/>
              <UniformGrid Columns="2" Margin="0,12,0,0">
                <Button x:Name="DiagButton" Style="{StaticResource Pill}" Background="#E08A2E" FontSize="12" Padding="6,5" Margin="0,0,4,0"/>
                <Button x:Name="LogButton" Style="{StaticResource Pill}" Background="#9AA7BD" FontSize="12" Padding="6,5" Margin="4,0,0,0"/>
              </UniformGrid>
              <Button x:Name="ShareButton" Style="{StaticResource Pill}" Background="#8E6CC8" FontSize="12" Padding="6,5" Margin="0,8,0,0"/>
            </StackPanel>
            <TextBox x:Name="LogBox" Visibility="Collapsed" Margin="0,8,0,0" FontFamily="Consolas" FontSize="10.5" IsReadOnly="True"
                     VerticalScrollBarVisibility="Auto" HorizontalScrollBarVisibility="Auto" Background="#F8FAFD"/>
          </DockPanel>
        </Border>
      </Grid>
      <TextBlock x:Name="LegalText" Grid.Row="3" FontSize="11" Foreground="#F4FFF0" TextWrapping="Wrap" TextAlignment="Center"/>
    </Grid>
  </Grid>
</Window>
"@
$window = [Windows.Markup.XamlReader]::Load((New-Object System.Xml.XmlNodeReader $xaml))
$ui = @{}
foreach ($name in @('TitleText','SubtitleText','DiscHeader','DiscBox','BrowseButton','BrowseDirButton','DiscHint','StepsHeader','StepsPanel',
                    'ProgressFill','ProgressText','StatusText','PlayButton','InstallButton','FolderButton','ShortcutButton','OptionsHeader',
                    'Native60Box','WidescreenBox','FullscreenBox','ResolutionLabel','ResolutionBox','InGameText','LogButton','LogBox','LegalText',
                    'VersionText','CheckUpdatesButton','UpdateBanner','UpdateText','NotesButton','UpdateButton','LaterButton','SkipButton',
                    'ApiLabel','ApiBox','DiagButton','ShareButton','FrameModeLabel','FrameModeBox')) {
    $ui[$name] = $window.FindName($name)
}
$iconPath = Join-Path $PSScriptRoot 'assets\icon.ico'
if (Test-Path $iconPath) {
    # Decode from memory: a file-backed BitmapFrame keeps icon.ico locked, and the updater must be able to replace it.
    $iconStream = New-Object IO.MemoryStream(, [IO.File]::ReadAllBytes($iconPath))
    $window.Icon = [Windows.Media.Imaging.BitmapFrame]::Create($iconStream, [Windows.Media.Imaging.BitmapCreateOptions]::None,
        [Windows.Media.Imaging.BitmapCacheOption]::OnLoad)
}

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
$script:skippedVersion = ''
function Get-LauncherSettings {
    if (Test-Path $SettingsPath) {
        try {
            $loaded = Get-Content $SettingsPath -Raw | ConvertFrom-Json
            if ($loaded.PSObject.Properties['skip']) { $script:skippedVersion = [string]$loaded.skip }
            return $loaded
        } catch {}
    }
    return [pscustomobject]@{ disc = '' }
}
function Save-LauncherSettings {
    [pscustomobject]@{ disc = $ui.DiscBox.Text; skip = $script:skippedVersion } | ConvertTo-Json | Set-Content $SettingsPath
}

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
$apis = [ordered]@{ 'Auto' = 'auto'; 'Direct3D 12' = 'd3d12'; 'Vulkan' = 'vulkan' }
foreach ($k in $S.FrameModes.Keys) { [void]$ui.FrameModeBox.Items.Add($S.FrameModes[$k]) }
foreach ($k in $apis.Keys) { [void]$ui.ApiBox.Items.Add($k) }
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
    $native = (Get-ConfigValue 'native_60fps') -ne 'false'
    $interp = Get-ConfigValue 'frame_interpolation_fps'
    $mode = $(if ($native) { 'native60' } elseif ($interp -and [int]$interp -ge 60) { 'interp60' } else { 'original' })
    $ui.FrameModeBox.SelectedItem = $S.FrameModes[$mode]
    $ui.FrameModeBox.IsEnabled = $installed
    $api = Get-ConfigValue 'graphics_api'
    $apiSelected = 'Auto'
    foreach ($k in $apis.Keys) { if ($api -eq $apis[$k]) { $apiSelected = $k } }
    $ui.ApiBox.SelectedItem = $apiSelected
    $ui.ApiBox.IsEnabled = $installed
    $script:loadingOptions = $false
}
$ui.Native60Box.Add_Click({ if (-not $loadingOptions) { Set-ConfigValue 'video' 'native_60fps' ($(if ($ui.Native60Box.IsChecked) { 'true' } else { 'false' })) } })
$ui.WidescreenBox.Add_Click({ if (-not $loadingOptions) { Set-ConfigValue 'video' 'force_aspect' ($(if ($ui.WidescreenBox.IsChecked) { 'true' } else { 'false' })) } })
$ui.FullscreenBox.Add_Click({ if (-not $loadingOptions) { Set-ConfigValue 'video' 'display_mode' ($(if ($ui.FullscreenBox.IsChecked) { '"borderless"' } else { '"windowed"' })) } })
$ui.FrameModeBox.Add_SelectionChanged({
    if ($loadingOptions -or -not $ui.FrameModeBox.SelectedItem) { return }
    $label = [string]$ui.FrameModeBox.SelectedItem
    $mode = @($S.FrameModes.Keys | Where-Object { $S.FrameModes[$_] -eq $label })[0]
    switch ($mode) {
        'native60' { Set-ConfigValue 'video' 'native_60fps' 'true';  Set-ConfigValue 'video' 'frame_interpolation_fps' '0' }
        'interp60' { Set-ConfigValue 'video' 'native_60fps' 'false'; Set-ConfigValue 'video' 'frame_interpolation_fps' '60' }
        'original' { Set-ConfigValue 'video' 'native_60fps' 'false'; Set-ConfigValue 'video' 'frame_interpolation_fps' '0' }
    }
})
$ui.ApiBox.Add_SelectionChanged({
    if (-not $loadingOptions -and $ui.ApiBox.SelectedItem) {
        Set-ConfigValue 'video' 'graphics_api' ('"' + $apis[[string]$ui.ApiBox.SelectedItem] + '"')
    }
})
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
    $ui.DiagButton.IsEnabled = $installed
    $ui.ShareButton.IsEnabled = $installed
    $ui.InstallButton.Content = $(if ($installed) { $S.Reinstall } else { $S.Install })
    if ($installed) {
        foreach ($id in $S.Steps.Keys) { Set-StepState $id 'done' }
        Set-Progress 100
        $ui.StatusText.Text = $S.Ready
        # The game was built from another version (e.g. new files copied by hand): ask for a rebuild.
        $builtFile = Join-Path $LogDir 'state\built_version.txt'
        $built = $(if (Test-Path $builtFile) { (Get-Content -LiteralPath $builtFile -Raw).Trim() } else { '' })
        if ($built -ne $LocalVersion) {
            foreach ($id in @('translate', 'build', 'install')) { Set-StepState $id 'pending' }
            $ui.StatusText.Text = $S.NeedsRebuild -f "v$LocalVersion"
        }
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
                $label = $(if ($S.Steps.Contains($parts[0])) { $S.Steps[$parts[0]] } elseif ($parts.Count -gt 1) { $parts[1] } else { $parts[0] })
                $ui.StatusText.Text = $label + '...'
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
                $script:finished = $true
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
        $script:exitCode = $script:process.ExitCode
        $script:process = $null
        if (-not $script:failed -and -not $script:finished) {
            # The installer stopped without reporting success or a specific error.
            $script:failed = $true
            $code = $(try { $script:exitCode } catch { '?' })
            $ui.StatusText.Text = (T 'El instalador se detuvo sin terminar' 'The installer stopped before finishing') + " (exit $code). " + (T 'Mira el registro.' 'See the log.')
            if ($script:currentStep) { Set-StepState $script:currentStep 'error' }
        }
        Set-Busy $false
        if ($script:mode -eq 'update') {
            if (-not $script:failed) {
                [Windows.MessageBox]::Show($window, $S.UpdateDone, $S.Title) | Out-Null
                # The launcher's own files were replaced: start the new one and let it rebuild.
                Start-Process -FilePath 'powershell.exe' -WindowStyle Hidden -ArgumentList @(
                    '-NoProfile', '-ExecutionPolicy', 'Bypass', '-STA', '-WindowStyle', 'Hidden',
                    '-File', (Format-Arg (Join-Path $Root 'launcher\PokeParkLauncher.ps1')), '-AfterUpdate')
                $script:restarting = $true
                $window.Close()
                return
            }
            $ui.LogBox.Visibility = 'Visible'
            $ui.StatusText.Text = "$($S.UpdateFailed): " + $ui.StatusText.Text
            $ui.PlayButton.IsEnabled = (Test-Path $GameExe)
            return
        }
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

# Command-line quoting for a Windows path (paths cannot contain '"'; a trailing '\' would escape the quote).
function Format-Arg([string]$Value) { '"' + $Value.TrimEnd('\') + '"' }

function Test-GameRunning {
    $running = Get-Process -Name 'PokePark' -ErrorAction SilentlyContinue |
        Where-Object { $_.Path -and $_.Path.StartsWith($InstallDir, [StringComparison]::OrdinalIgnoreCase) }
    return [bool]$running
}

function Set-Busy([bool]$Busy) {
    foreach ($b in @($ui.InstallButton, $ui.BrowseButton, $ui.BrowseDirButton, $ui.CheckUpdatesButton, $ui.UpdateButton)) { $b.IsEnabled = -not $Busy }
    if ($Busy) { $ui.PlayButton.IsEnabled = $false }
}

# Runs a PowerShell script hidden; its "##" lines are followed through -LogFile by the timer.
function Start-Worker([string]$Mode, [string]$ScriptFile, [string]$Arguments) {
    $script:mode = $Mode
    $script:logPath = Join-Path $LogDir ("{0}_{1:yyyyMMdd_HHmmss}.log" -f $Mode, (Get-Date))
    $script:logOffset = 0
    $script:finished = $false
    $script:failed = $false
    $script:currentStep = $null
    $ui.LogBox.Clear()
    $startInfo = New-Object Diagnostics.ProcessStartInfo 'powershell.exe'
    # -File passes each value verbatim (no PowerShell re-parsing), so spaces and apostrophes in names are safe.
    $startInfo.Arguments = '-NoProfile -ExecutionPolicy Bypass -File ' + (Format-Arg $ScriptFile) + ' ' + $Arguments + ' -LogFile ' + (Format-Arg $script:logPath)
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    Set-Busy $true
    try {
        $script:process = [Diagnostics.Process]::Start($startInfo)
    } catch {
        $script:failed = $true
        $ui.StatusText.Text = "$($S.Failed): $($_.Exception.Message)"
        Set-Busy $false
        $ui.PlayButton.IsEnabled = (Test-Path $GameExe)
        return $false
    }
    $timer.Start()
    return $true
}

function Start-Build {
    if ($script:process) { [Windows.MessageBox]::Show($window, $S.Busy, $S.Title) | Out-Null; return }
    if (Test-GameRunning) { [Windows.MessageBox]::Show($window, $S.CloseGame, $S.Title) | Out-Null; return }
    $disc = $ui.DiscBox.Text.Trim()
    if (-not $disc) { [Windows.MessageBox]::Show($window, $S.NeedDisc, $S.Title) | Out-Null; return }
    Save-LauncherSettings
    foreach ($id in $S.Steps.Keys) { Set-StepState $id 'pending' }
    Set-Progress 0
    $ui.StatusText.Text = $S.Working
    $discArg = $(if (Test-Path -LiteralPath $disc -PathType Container) { '-ExtractedDir ' + (Format-Arg $disc) } else { '-Disc ' + (Format-Arg $disc) })
    [void](Start-Worker 'build' $BuildScript $discArg)
}

$ui.InstallButton.Add_Click({ Start-Build })

# --- Updates (GitHub releases) ------------------------------------------------------------------------
function ConvertTo-VersionObject([string]$Text) {
    $clean = ($Text -replace '^[vV]', '') -replace '[^0-9.].*$', ''
    try { return [version]$clean } catch { return [version]'0.0' }
}

function ConvertFrom-ReleaseNotes([string]$Markdown) {
    # Plain text for the notes window: drop the markdown that would only add noise.
    $text = $Markdown -replace '\*\*', '' -replace '`', '' -replace '(?m)^#+\s*', '' -replace '\[([^\]]+)\]\([^)]+\)', '$1'
    return $text.Trim()
}

$script:latestRelease = $null
$script:updateTask = $null
$script:updateManual = $false
$updateTimer = New-Object Windows.Threading.DispatcherTimer
$updateTimer.Interval = [TimeSpan]::FromMilliseconds(200)

function Start-UpdateCheck([bool]$Manual) {
    if ($script:updateTask) { return }
    $script:updateManual = $Manual
    if ($Manual) { $ui.CheckUpdatesButton.Content = $S.Checking; $ui.CheckUpdatesButton.IsEnabled = $false }
    try {
        [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
        $client = New-Object Net.Http.HttpClient
        $client.Timeout = [TimeSpan]::FromSeconds(15)
        $client.DefaultRequestHeaders.UserAgent.ParseAdd('PokePort-launcher/' + $LocalVersion)
        $client.DefaultRequestHeaders.Accept.ParseAdd('application/vnd.github+json')
        $script:updateTask = $client.GetStringAsync($RepoApi)
        $updateTimer.Start()
    } catch {
        Complete-UpdateCheck $null $_.Exception.Message
    }
}

function Complete-UpdateCheck($Json, [string]$ErrorText) {
    $updateTimer.Stop()
    $script:updateTask = $null
    if ($env:POKEPARK_LAUNCHER_SNAPSHOT) {
        try { Complete-UpdateCheckCore $Json $ErrorText } finally { Save-Snapshot }
        return
    }
    Complete-UpdateCheckCore $Json $ErrorText
}

function Complete-UpdateCheckCore($Json, [string]$ErrorText) {
    $ui.CheckUpdatesButton.Content = $S.CheckUpdates
    $ui.CheckUpdatesButton.IsEnabled = -not $script:process
    if ($ErrorText) {
        if ($script:updateManual) { [Windows.MessageBox]::Show($window, ($S.CheckFailed -f $ErrorText), $S.Title) | Out-Null }
        return
    }
    try { $release = $Json | ConvertFrom-Json } catch { if ($script:updateManual) { [Windows.MessageBox]::Show($window, ($S.CheckFailed -f 'JSON'), $S.Title) | Out-Null }; return }
    $asset = @($release.assets | Where-Object { $_.name -like 'PokePort-*.zip' }) | Select-Object -First 1
    if (-not $asset) { if ($script:updateManual) { [Windows.MessageBox]::Show($window, ($S.UpToDate -f "v$LocalVersion"), $S.Title) | Out-Null }; return }
    $sha = ''
    if ($asset.PSObject.Properties['digest'] -and $asset.digest -like 'sha256:*') { $sha = $asset.digest.Substring(7) }
    $script:latestRelease = [pscustomobject]@{
        Tag = [string]$release.tag_name; Body = [string]$release.body; Url = [string]$asset.browser_download_url; Sha256 = $sha
    }
    $isNewer = (ConvertTo-VersionObject $release.tag_name) -gt (ConvertTo-VersionObject $LocalVersion)
    if ($isNewer -and ($script:updateManual -or $script:skippedVersion -ne $release.tag_name)) {
        $ui.UpdateText.Text = $S.NewVersion -f $release.tag_name
        $ui.UpdateBanner.Visibility = 'Visible'
    } elseif ($script:updateManual) {
        [Windows.MessageBox]::Show($window, ($S.UpToDate -f "v$LocalVersion"), $S.Title) | Out-Null
    }
}

$updateTimer.Add_Tick({
    $task = $script:updateTask
    if (-not $task -or -not $task.IsCompleted) { return }
    if ($task.IsFaulted -or $task.IsCanceled) {
        $message = $(if ($task.Exception) { $task.Exception.GetBaseException().Message } else { 'timeout' })
        Complete-UpdateCheck $null $message
    } else {
        Complete-UpdateCheck $task.Result $null
    }
})

function Show-ReleaseNotes {
    $release = $script:latestRelease
    if (-not $release) { return }
    $notes = New-Object Windows.Window
    $notes.Title = $S.NotesTitle -f $release.Tag
    $notes.Width = 640; $notes.Height = 520; $notes.Owner = $window; $notes.WindowStartupLocation = 'CenterOwner'
    $panel = New-Object Windows.Controls.DockPanel
    $panel.Margin = '14'
    $buttons = New-Object Windows.Controls.StackPanel
    $buttons.Orientation = 'Horizontal'; $buttons.HorizontalAlignment = 'Right'; $buttons.Margin = '0,10,0,0'
    [Windows.Controls.DockPanel]::SetDock($buttons, 'Bottom')
    $updateNow = New-Object Windows.Controls.Button
    $updateNow.Content = $S.UpdateNow; $updateNow.Padding = '16,6'; $updateNow.Margin = '0,0,8,0'
    $close = New-Object Windows.Controls.Button
    $close.Content = $S.Close; $close.Padding = '16,6'
    [void]$buttons.Children.Add($updateNow); [void]$buttons.Children.Add($close)
    $box = New-Object Windows.Controls.TextBox
    $box.Text = ConvertFrom-ReleaseNotes $release.Body
    $box.IsReadOnly = $true; $box.TextWrapping = 'Wrap'; $box.VerticalScrollBarVisibility = 'Auto'; $box.FontSize = 13; $box.Padding = '8'
    [void]$panel.Children.Add($buttons); [void]$panel.Children.Add($box)
    $notes.Content = $panel
    $close.Add_Click({ $notes.Close() })
    $updateNow.Add_Click({ $notes.Close(); Start-Update })
    [void]$notes.ShowDialog()
}

function Start-Update {
    $release = $script:latestRelease
    if (-not $release) { return }
    if ($script:process) { [Windows.MessageBox]::Show($window, $S.Busy, $S.Title) | Out-Null; return }
    if (Test-GameRunning) { [Windows.MessageBox]::Show($window, $S.CloseGame, $S.Title) | Out-Null; return }
    # Run a copy of the updater: the original is one of the files being replaced.
    $runDir = Join-Path $LogDir 'updates\run'
    [IO.Directory]::CreateDirectory($runDir) | Out-Null
    $runner = Join-Path $runDir 'Update-PokePort.ps1'
    Copy-Item -LiteralPath $UpdateScript -Destination $runner -Force
    $ui.UpdateBanner.Visibility = 'Collapsed'
    $ui.StatusText.Text = $S.Updating
    Set-Progress 0
    $arguments = '-Root ' + (Format-Arg $Root) + ' -Tag ' + (Format-Arg $release.Tag) + ' -AssetUrl ' + (Format-Arg $release.Url)
    if ($release.Sha256) { $arguments += ' -Sha256 ' + $release.Sha256 }
    [void](Start-Worker 'update' $runner $arguments)
}

$ui.CheckUpdatesButton.Add_Click({ Start-UpdateCheck $true })
$ui.NotesButton.Add_Click({ Show-ReleaseNotes })
$ui.UpdateButton.Add_Click({ Start-Update })
$ui.LaterButton.Add_Click({ $ui.UpdateBanner.Visibility = 'Collapsed' })
$ui.SkipButton.Add_Click({
    if ($script:latestRelease) { $script:skippedVersion = $script:latestRelease.Tag; Save-LauncherSettings }
    $ui.UpdateBanner.Visibility = 'Collapsed'
})

# --- Diagnostic mode ----------------------------------------------------------------------------------
$script:diagProcess = $null
$script:diagLog = $null
$diagTimer = New-Object Windows.Threading.DispatcherTimer
$diagTimer.Interval = [TimeSpan]::FromSeconds(1)

function Get-DiagnosticSummary([string]$LogPath) {
    $lines = Get-Content -LiteralPath $LogPath -ErrorAction SilentlyContinue
    $cpu = ($lines | Where-Object { $_ -match 'CPU model: (.+)$' } | Select-Object -First 1) -replace '.*CPU model: ', ''
    $gpus = @($lines | Where-Object { $_ -match 'Device: (.+)$' } | ForEach-Object { ($_ -replace '.*Device: ', '').Trim() }) | Select-Object -Unique
    # The backend actually used ("[runtime] graphics backend: X"), not the one requested.
    $api = ($lines | Where-Object { $_ -match '^\s+API: (.+)$' } | Select-Object -First 1) -replace '^\s+API: ', ''
    $presentMode = ($lines | Where-Object { $_ -match 'present mode (\S+)' } | Select-Object -First 1) -replace '.*present mode ', ''
    if ($presentMode) { $api += ", present $presentMode" }
    if ($lines | Where-Object { $_ -match 'Requested graphics backend (\S+) is unavailable' }) { $api += (T ' (la API elegida no está disponible)' ' (the chosen API is unavailable)') }
    $frames = 0
    foreach ($l in $lines) { if ($l -match 'presented frames=(\d+)') { $frames = [int]$Matches[1] } }
    $busy = @(); foreach ($l in $lines) { if ($l -match 'busy ms/frame over \d+ frames: avg ([\d.]+)') { $busy += [double]$Matches[1] } }
    $hitches = @(); $shader = 0; $render = 0; $game = 0; $worst = 0.0
    foreach ($l in $lines) {
        if ($l -notmatch '\[hitch\] ([\d.]+) ms: guest ([\d.]+).*endFrame ([\d.]+) \| pipelines \+(\d+)') { continue }
        $ms = [double]$Matches[1]; $end = [double]$Matches[3]; $pipes = [int]$Matches[4]
        if ($ms -lt 25) { continue }   # a 50 fps scene is slow, not a stutter
        $hitches += $ms
        if ($ms -gt $worst) { $worst = $ms }
        if ($pipes -gt 0) { $shader++ } elseif ($end -gt 8) { $render++ } else { $game++ }
    }
    $avgBusy = $(if ($busy.Count) { [Math]::Round(($busy | Measure-Object -Average).Average, 1) } else { 'n/d' })
    $text = @()
    $text += (T 'Procesador: ' 'CPU: ') + $cpu
    $text += (T 'Tarjeta gráfica: ' 'GPU: ') + ($gpus -join ', ') + "  ($api)"
    $retraces = 0
    foreach ($l in $lines) { if ($l -match 'presented frames=\d+ retrace=(\d+)') { $retraces = [int]$Matches[1] } }
    $fps = $(if ($retraces -gt 0) { [Math]::Round(59.94 * $frames / $retraces, 1) } else { 'n/d' })
    $text += (T 'Fotogramas mostrados: ' 'Frames presented: ') + $frames + (T '  (media ~' '  (average ~') + $fps + ' fps)'
    $text += (T 'Trabajo de CPU por fotograma (media): ' 'CPU work per frame (average): ') + "$avgBusy ms " + (T '(16,7 ms = límite para 60 fps)' '(16.7 ms = budget for 60 fps)')
    $text += (T 'Tirones de más de 25 ms: ' 'Stutters over 25 ms: ') + $hitches.Count + (T '  (peor: ' '  (worst: ') + [Math]::Round($worst, 1) + ' ms)'
    $text += (T '  - por compilación de shaders: ' '  - shader compilation: ') + $shader
    $text += (T '  - por la GPU / dibujado: ' '  - GPU / rendering: ') + $render
    $text += (T '  - por el juego (CPU, cargas): ' '  - game work (CPU, loading): ') + $game
    $text += ''
    $text += (T 'Envía este archivo para analizarlo: ' 'Send this file for analysis: ') + $LogPath
    return ($text -join "`r`n")
}

$diagTimer.Add_Tick({
    if (-not $script:diagProcess -or -not $script:diagProcess.HasExited) { return }
    $diagTimer.Stop()
    $script:diagProcess = $null
    Set-Busy $false
    $ui.PlayButton.IsEnabled = (Test-Path $GameExe)
    $summary = Get-DiagnosticSummary $script:diagLog
    Set-Content -LiteralPath ($script:diagLog -replace '\.log$', '_resumen.txt') -Value $summary -Encoding UTF8
    $answer = [Windows.MessageBox]::Show($window, $summary + "`r`n`r`n" + (T '¿Abrir la carpeta del registro?' 'Open the log folder?'), $S.DiagTitle, 'YesNo')
    if ($answer -eq 'Yes') { Start-Process explorer.exe ('/select,"' + $script:diagLog + '"') }
})

$ui.DiagButton.Add_Click({
    if (-not (Test-Path $GameExe)) { return }
    if ($script:process -or $script:diagProcess) { [Windows.MessageBox]::Show($window, $S.Busy, $S.Title) | Out-Null; return }
    if (Test-GameRunning) { [Windows.MessageBox]::Show($window, $S.CloseGame, $S.Title) | Out-Null; return }
    if ([Windows.MessageBox]::Show($window, $S.DiagStart, $S.Diagnostic, 'OKCancel') -ne 'OK') { return }
    $diagDir = Join-Path $LogDir 'diagnostics'
    [IO.Directory]::CreateDirectory($diagDir) | Out-Null
    $script:diagLog = Join-Path $diagDir ("diagnostico_{0:yyyyMMdd_HHmmss}.log" -f (Get-Date))
    $startInfo = New-Object Diagnostics.ProcessStartInfo 'cmd.exe'
    $startInfo.Arguments = '/d /c ""' + $GameExe + '" > "' + $script:diagLog + '" 2>&1"'
    $startInfo.WorkingDirectory = $InstallDir
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.EnvironmentVariables['MKW_HITCHLOG'] = '1'
    $startInfo.EnvironmentVariables['MKW_PERF_STATS'] = '1'
    $script:diagProcess = [Diagnostics.Process]::Start($startInfo)
    Set-Busy $true
    $diagTimer.Start()
})

# --- Share the shader cache ---------------------------------------------------------------------------
$ui.ShareButton.Add_Click({
    if (Test-GameRunning) { [Windows.MessageBox]::Show($window, $S.CloseGame, $S.Title) | Out-Null; return }
    $cacheDir = Join-Path $InstallDir 'UserData\Cache'
    $db = Join-Path $cacheDir 'pipeline_cache.db'
    if (-not (Test-Path -LiteralPath $db)) { [Windows.MessageBox]::Show($window, $S.ShareEmpty, $S.Title) | Out-Null; return }
    $stage = Join-Path $LogDir ('share_' + [Guid]::NewGuid().ToString('N'))
    [IO.Directory]::CreateDirectory($stage) | Out-Null
    try {
        # The -wal file holds the most recent entries until SQLite checkpoints it: ship all three.
        Get-ChildItem -LiteralPath $cacheDir -Filter 'pipeline_cache.db*' | Copy-Item -Destination $stage -Force
        $gpu = ''
        $info = @("PokePort v$LocalVersion", "Date: $(Get-Date -Format o)", "CPU threads: $([Environment]::ProcessorCount)") -join "`r`n"
        Set-Content -LiteralPath (Join-Path $stage 'info.txt') -Value $info -Encoding UTF8
        $zip = Join-Path ([Environment]::GetFolderPath('Desktop')) ("PokePort-shaders-{0:yyyyMMdd_HHmm}.zip" -f (Get-Date))
        if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip -Force }
        Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip
        $answer = [Windows.MessageBox]::Show($window, ($S.ShareDone -f $zip, "`r`n") + "`r`n`r`n" + (T '¿Abrir la carpeta?' 'Open the folder?'), $S.ShareShaders, 'YesNo')
        if ($answer -eq 'Yes') { Start-Process explorer.exe ('/select,"' + $zip + '"') }
    } catch {
        [Windows.MessageBox]::Show($window, "$($S.Failed): $($_.Exception.Message)", $S.Title) | Out-Null
    } finally {
        Remove-Item -LiteralPath $stage -Recurse -Force -ErrorAction SilentlyContinue
    }
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

$ui.VersionText.Text = "v$LocalVersion"
$ui.ApiLabel.Text = $S.GraphicsApi
$ui.FrameModeLabel.Text = $S.FrameMode
$ui.DiagButton.Content = $S.Diagnostic
$ui.ShareButton.Content = $S.ShareShaders
$ui.CheckUpdatesButton.Content = $S.CheckUpdates
$ui.NotesButton.Content = $S.SeeNotes
$ui.UpdateButton.Content = $S.UpdateNow
$ui.LaterButton.Content = $S.Later
$ui.SkipButton.Content = $S.Skip

function Save-Snapshot {
    # POKEPARK_LAUNCHER_SNAPSHOT=<file.png>: render the window to an image and close (for screenshots/tests).
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

$window.Add_ContentRendered({
    Update-InstalledState
    if (-not $env:POKEPARK_LAUNCHER_SNAPSHOT) {
        if ($AfterUpdate -and $ui.DiscBox.Text.Trim()) {
            # Rebuild with the new sources; the disc is already extracted and is not read again.
            Start-Build
            $ui.StatusText.Text = $S.Rebuilding
        } else {
            Start-UpdateCheck $false
        }
    }
    if ($env:POKEPARK_LAUNCHER_SNAPSHOT) {
        # With a test endpoint the snapshot waits for the update check (to show the banner).
        if ($env:POKEPORT_UPDATE_API) { Start-UpdateCheck $false } else { Save-Snapshot }
    }
})
$window.Add_Closing({
    if (-not $script:restarting -and $script:process -and -not $script:process.HasExited) {
        $answer = [Windows.MessageBox]::Show($window, (T 'La instalación sigue en marcha. Cerrar la detendrá. ¿Cerrar igualmente?' 'The installation is still running. Closing stops it. Close anyway?'), $S.Title, 'YesNo')
        if ($answer -ne 'Yes') { $_.Cancel = $true; return }
        try { Stop-Process -Id $script:process.Id -Force } catch {}
    }
})
[void]$window.ShowDialog()
