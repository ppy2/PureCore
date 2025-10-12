<?php
require_once 'config.php';
// Path to configuration file
$config_file = '/etc/i2s.conf';

/**
 * Function for reading the current mode (MODE), MCLK and SUBMODE from configuration file.
 */
function readConfig($filePath) {
    $result = ['mode' => '', 'mclk' => '', 'submode' => '', 'pcm_swap' => '0', 'dsd_swap' => '1'];
    if (file_exists($filePath)) {
        $contents = file_get_contents($filePath);
        if ($contents !== false) {
            if (preg_match('/^MODE=(\w+)/m', $contents, $matches)) {
                $result['mode'] = strtolower($matches[1]);
            }
            if (preg_match('/^MCLK=(\d+)/m', $contents, $matches)) {
                $result['mclk'] = $matches[1];
            }
            if (preg_match('/^SUBMODE=(\w+)/m', $contents, $matches)) {
                $result['submode'] = strtolower($matches[1]);
            } else {
                $result['submode'] = 'std';
            }
            if (preg_match('/^PCM_SWAP=([01])/m', $contents, $matches)) {
                $result['pcm_swap'] = $matches[1];
            }
            if (preg_match('/^DSD_SWAP=([01])/m', $contents, $matches)) {
                $result['dsd_swap'] = $matches[1];
            }
        }
    }
    
    // Read current values from sysfs if available
    if (file_exists('/sys/devices/platform/ffae0000.i2s/pcm_channel_swap')) {
        $pcm_current = trim(file_get_contents('/sys/devices/platform/ffae0000.i2s/pcm_channel_swap'));
        if ($pcm_current !== false) {
            $result['pcm_swap'] = $pcm_current;
        }
    }
    
    if (file_exists('/sys/devices/platform/ffae0000.i2s/dsd_physical_swap')) {
        $dsd_current = trim(file_get_contents('/sys/devices/platform/ffae0000.i2s/dsd_physical_swap'));
        if ($dsd_current !== false) {
            $result['dsd_swap'] = $dsd_current;
        }
    }
    
    return $result;
}

/**
 * Update configuration file with new value
 */
function updateConfigValue($filePath, $key, $value) {
    $contents = '';
    if (file_exists($filePath)) {
        $contents = file_get_contents($filePath);
    }
    
    $pattern = "/^$key=.*$/m";
    $replacement = "$key=$value";
    
    if (preg_match($pattern, $contents)) {
        $contents = preg_replace($pattern, $replacement, $contents);
    } else {
        $contents .= "\n$replacement\n";
    }
    
    file_put_contents($filePath, $contents);
}

/**
 * Update S01RkLunch init script with new swap settings
 */
function updateInitScript($value, $type) {
    $initScript = '/etc/init.d/S01RkLunch';
    
    if (!file_exists($initScript)) {
        return;
    }
    
    $contents = file_get_contents($initScript);
    
    if ($type === 'pcm') {
        // Update PCM swap line
        $pattern = '/^#?echo [01] > \/sys\/devices\/platform\/ffae0000\.i2s\/pcm_channel_swap$/m';
        $replacement = "echo $value > /sys/devices/platform/ffae0000.i2s/pcm_channel_swap";
        
        if (preg_match($pattern, $contents)) {
            $contents = preg_replace($pattern, $replacement, $contents);
        } else {
            // Add after DSD swap line
            $contents = preg_replace(
                '/(echo [01] > \/sys\/devices\/platform\/ffae0000\.i2s\/dsd_physical_swap)/',
                "$1\necho $value > /sys/devices/platform/ffae0000.i2s/pcm_channel_swap",
                $contents
            );
        }
    } else if ($type === 'dsd') {
        // Update DSD swap line
        $pattern = '/^echo [01] > \/sys\/devices\/platform\/ffae0000\.i2s\/dsd_physical_swap$/m';
        $replacement = "echo $value > /sys/devices/platform/ffae0000.i2s/dsd_physical_swap";
        $contents = preg_replace($pattern, $replacement, $contents);
    }
    
    file_put_contents($initScript, $contents);
}

// --- Processing AJAX request to get current state ---
if (isset($_GET['action']) && $_GET['action'] === 'getStatus') {
    $config = readConfig($config_file);
    header('Content-Type: application/json');
    echo json_encode($config);
    exit;
}

// --- Processing POST request ---
if ($_SERVER['REQUEST_METHOD'] === 'POST') {
    // If button for changing mode (PLL/EXT) is pressed
    if (isset($_POST['mode'])) {
        $mode = $_POST['mode'];
        if (in_array($mode, ['pll', 'ext'])) {
            $script = ($mode === 'pll') ? '/opt/2pll.sh' : '/opt/2ext.sh';
            exec('/usr/bin/sudo ' . escapeshellcmd($script) . ' 2>&1', $output, $returnVar);
        }
    }

    // If button for changing SUBMODE (STD/L/R/±L/±R/8CH) is pressed
    if (isset($_POST['submode'])) {
        $submode = $_POST['submode'];
        if (in_array($submode, ['std', 'lr', 'plr', '8ch'])) {
            $script = "/opt/2_$submode.sh";
            exec('/usr/bin/sudo ' . escapeshellcmd($script) . ' 2>&1', $output, $returnVar);
        }
    }

    // If button for changing MCLK is pressed
    if (isset($_POST['mclk'])) {
        $mclk = $_POST['mclk'];
        if (in_array($mclk, ['512', '1024'])) {
            // Choose script depending on current mode
            $config = readConfig($config_file);
            $mode = $config['mode'];
            
            if ($mode === 'pll') {
                $script = ($mclk === '1024') ? '/opt/2_1024_pll.sh' : '/opt/2_512_pll.sh';
            } else {
                $script = ($mclk === '1024') ? '/opt/2_1024_ext.sh' : '/opt/2_512_ext.sh';
            }
            exec('/usr/bin/sudo ' . escapeshellcmd($script) . ' 2>&1', $output, $returnVar);
        }
    }

    // If PCM swap setting is changed
    if (isset($_POST['pcm_swap'])) {
        $pcm_swap = $_POST['pcm_swap'];
        if (in_array($pcm_swap, ['0', '1'])) {
            // Write to sysfs
            file_put_contents('/sys/devices/platform/ffae0000.i2s/pcm_channel_swap', $pcm_swap);
            
            // Update config file
            updateConfigValue($config_file, 'PCM_SWAP', $pcm_swap);
            
            // Update S01RkLunch script
            updateInitScript($pcm_swap, 'pcm');
        }
    }

    // If DSD swap setting is changed
    if (isset($_POST['dsd_swap'])) {
        $dsd_swap = $_POST['dsd_swap'];
        if (in_array($dsd_swap, ['0', '1'])) {
            // Write to sysfs
            file_put_contents('/sys/devices/platform/ffae0000.i2s/dsd_physical_swap', $dsd_swap);
            
            // Update config file
            updateConfigValue($config_file, 'DSD_SWAP', $dsd_swap);
            
            // Update S01RkLunch script
            updateInitScript($dsd_swap, 'dsd');
        }
    }

    
    // Reboot processing removed - now using reboot.php

    // After performing actions, redirect (PRG)
    header("Location: " . $_SERVER['PHP_SELF']);
    exit;
}

// --- GET request: reading current state ---
$config = readConfig($config_file);
$current_mode = $config['mode'];
$current_mclk = $config['mclk'];
$current_submode = $config['submode'];
$current_pcm_swap = $config['pcm_swap'];
$current_dsd_swap = $config['dsd_swap'];
?>
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title data-lang="i2s_title">I2S Settings</title>
    <link rel="stylesheet" href="assets/css/style.css?v=<?php echo VERSION; ?>">
    <style>
        .group {
            margin-bottom: 15px;
            text-align: center;
            padding: 15px 0;
            border-bottom: 1px solid #3d3d3d;
        }
        .group:last-of-type {
            border-bottom: none;
        }
        .group-header {
            display: flex;
            align-items: center;
            justify-content: space-between;
            gap: 20px;
        }
        .group h2 {
            margin: 0;
            font-size: 16px;
            color: #e0e0e0;
            flex-shrink: 0;
        }
        .group .row {
            display: flex;
            justify-content: space-around;
            flex-wrap: nowrap;
            gap: 5px;
        }
        .group .row button {
            padding: 3px 10px;
            font-size: 14px;
            margin: 0 2px;
            min-width: 60px;
            height: 32px;
        }
        .group .submode-rows {
            display: flex;
            flex-direction: column;
            gap: 8px;
            width: 100%;
            margin: 0 auto;
        }
        .group .submode-row {
            display: flex;
            justify-content: space-between;
            width: 220px;
            margin: 0 auto;
        }
        .group .group-header {
            width: 220px;
            margin: 0 auto;
            display: flex;
            justify-content: space-between;
            align-items: center;
        }
        .group .submode-row button {
            padding: 3px 15px;
            font-size: 14px;
            width: 105px;
            height: 32px;
            text-align: center;
            box-sizing: border-box;
        }
        
        /* Compact toggle switches for I2S */
        .toggle-switch-compact {
            position: relative;
            display: inline-block;
        }
        
        .toggle-input-compact {
            position: absolute;
            opacity: 0;
            width: 0;
            height: 0;
            margin: 0;
            pointer-events: none;
        }
        
        .toggle-label-compact {
            position: relative;
            display: flex;
            align-items: center;
            width: 120px;
            height: 32px;
            background-color: transparent;
            border-radius: 16px;
            cursor: pointer;
            transition: all 0.3s ease;
            border: 2px solid #3d3d3d;
            user-select: none;
            -webkit-tap-highlight-color: transparent;
            touch-action: manipulation;
            box-sizing: border-box;
        }
        
        .toggle-label-compact:hover,
        .toggle-input-compact:hover + .toggle-label-compact {
            border-color: #4d4d4d;
            transform: translateY(-1px);
            box-shadow: 0 2px 4px rgba(0,0,0,0.2);
        }
        
        .toggle-option-compact {
            position: absolute;
            top: 50%;
            transform: translateY(-50%);
            font-size: 14px;
            font-weight: normal;
            color: #e0e0e0;
            pointer-events: auto;
            z-index: 3;
            transition: color 0.3s ease;
            cursor: pointer;
            padding: 4px 8px;
        }
        
        .toggle-option-compact.left {
            left: 25%;
            transform: translateX(-50%) translateY(-50%);
        }
        
        .toggle-option-compact.right {
            right: 25%;
            transform: translateX(50%) translateY(-50%);
        }
        
        .toggle-slider-compact {
            position: absolute;
            width: 62px;
            height: 32px;
            background-color: #5d5d5d;
            border-radius: 16px;
            transition: left 0.5s ease;
            left: -2px;
            z-index: 1;
            filter: brightness(1.3);
            box-sizing: border-box;
        }
        
        .toggle-input-compact:checked + .toggle-label-compact .toggle-slider-compact {
            left: 56px;
        }
        
        input[name="mode"][value="pll"]:checked ~ .toggle-label-compact .toggle-slider-compact {
            left: -2px;
        }
        
        input[name="mode"][value="ext"]:checked ~ .toggle-label-compact .toggle-slider-compact {
            left: 56px;
        }
        
        input[name="mclk"][value="512"]:checked ~ .toggle-label-compact .toggle-slider-compact {
            left: -2px;
        }
        
        input[name="mclk"][value="1024"]:checked ~ .toggle-label-compact .toggle-slider-compact {
            left: 56px;
        }
        
        input[name="pcm_swap"][value="0"]:checked ~ .toggle-label-compact .toggle-slider-compact {
            left: -2px;
        }
        
        input[name="pcm_swap"][value="1"]:checked ~ .toggle-label-compact .toggle-slider-compact {
            left: 56px;
        }
        
        input[name="dsd_swap"][value="0"]:checked ~ .toggle-label-compact .toggle-slider-compact {
            left: -2px;
        }
        
        input[name="dsd_swap"][value="1"]:checked ~ .toggle-label-compact .toggle-slider-compact {
            left: 56px;
        }
        
        input[name="mode"][value="pll"]:checked ~ .toggle-label-compact .toggle-option-compact.left {
            color: #fff;
            left: 29px;
            transform: translateX(-50%) translateY(-50%);
        }
        
        input[name="mode"][value="pll"]:checked ~ .toggle-label-compact .toggle-option-compact.right {
            color: #888;
        }
        
        input[name="mode"][value="ext"]:checked ~ .toggle-label-compact .toggle-option-compact.left {
            color: #888;
        }
        
        input[name="mode"][value="ext"]:checked ~ .toggle-label-compact .toggle-option-compact.right {
            color: #fff;
            right: 29px;
            transform: translateX(50%) translateY(-50%);
        }
        
        input[name="mclk"][value="512"]:checked ~ .toggle-label-compact .toggle-option-compact.left {
            color: #fff;
            left: 29px;
            transform: translateX(-50%) translateY(-50%);
        }
        
        input[name="mclk"][value="512"]:checked ~ .toggle-label-compact .toggle-option-compact.right {
            color: #888;
        }
        
        input[name="mclk"][value="1024"]:checked ~ .toggle-label-compact .toggle-option-compact.left {
            color: #888;
        }
        
        input[name="mclk"][value="1024"]:checked ~ .toggle-label-compact .toggle-option-compact.right {
            color: #fff;
            right: 29px;
            transform: translateX(50%) translateY(-50%);
        }
        
        input[name="pcm_swap"][value="0"]:checked ~ .toggle-label-compact .toggle-option-compact.left {
            color: #fff;
            left: 29px;
            transform: translateX(-50%) translateY(-50%);
        }
        
        input[name="pcm_swap"][value="0"]:checked ~ .toggle-label-compact .toggle-option-compact.right {
            color: #888;
        }
        
        input[name="pcm_swap"][value="1"]:checked ~ .toggle-label-compact .toggle-option-compact.left {
            color: #888;
        }
        
        input[name="pcm_swap"][value="1"]:checked ~ .toggle-label-compact .toggle-option-compact.right {
            color: #fff;
            right: 29px;
            transform: translateX(50%) translateY(-50%);
        }
        
        input[name="dsd_swap"][value="0"]:checked ~ .toggle-label-compact .toggle-option-compact.left {
            color: #fff;
            left: 29px;
            transform: translateX(-50%) translateY(-50%);
        }
        
        input[name="dsd_swap"][value="0"]:checked ~ .toggle-label-compact .toggle-option-compact.right {
            color: #888;
        }
        
        input[name="dsd_swap"][value="1"]:checked ~ .toggle-label-compact .toggle-option-compact.left {
            color: #888;
        }
        
        input[name="dsd_swap"][value="1"]:checked ~ .toggle-label-compact .toggle-option-compact.right {
            color: #fff;
            right: 29px;
            transform: translateX(50%) translateY(-50%);
        }
        
        /* Reduce overall container width */
        .container {
            max-width: 350px !important;
        }
        
        /* Spinner overlay */
        .spinner-overlay {
            position: fixed;
            top: 0;
            left: 0;
            width: 100%;
            height: 100%;
            background: rgba(0, 0, 0, 0.8);
            display: flex;
            align-items: center;
            justify-content: center;
            z-index: 9999;
            opacity: 0;
            visibility: hidden;
            transition: opacity 0.3s, visibility 0.3s;
        }
        
        .spinner-overlay.show {
            opacity: 1;
            visibility: visible;
        }
        
        .spinner-container {
            display: flex;
            flex-direction: column;
            align-items: center;
            text-align: center;
        }
        
        .spinner {
            width: 40px;
            height: 40px;
            border: 4px solid #3d3d3d;
            border-top: 4px solid #e0e0e0;
            border-radius: 50%;
            animation: spin 1s linear infinite;
            margin-bottom: 20px;
        }
        
        .spinner-text {
            color: #e0e0e0;
            font-size: 16px;
            font-weight: 500;
        }
        
        @keyframes spin {
            0% { transform: rotate(0deg); }
            100% { transform: rotate(360deg); }
        }
        
        /* Confirm dialog */
        .confirm-overlay {
            position: fixed;
            top: 0;
            left: 0;
            width: 100%;
            height: 100%;
            background: rgba(0, 0, 0, 0.8);
            display: flex;
            align-items: center;
            justify-content: center;
            z-index: 10000;
            opacity: 0;
            visibility: hidden;
            transition: opacity 0.3s, visibility 0.3s;
        }
        
        .confirm-overlay.show {
            opacity: 1;
            visibility: visible;
        }
        
        .confirm-content {
            background: #2a2a2a;
            border-radius: 8px;
            padding: 20px;
            max-width: 400px;
            width: 90%;
            text-align: center;
            box-shadow: 0 8px 32px rgba(0, 0, 0, 0.6);
        }
        
        .confirm-message {
            color: #e0e0e0;
            margin-bottom: 20px;
            font-size: 16px;
            line-height: 1.4;
        }
        
        .confirm-buttons {
            display: flex;
            gap: 10px;
            justify-content: center;
        }
        
        .confirm-btn {
            padding: 10px 20px;
            border: none;
            border-radius: 4px;
            font-size: 14px;
            font-weight: 500;
            cursor: pointer;
            transition: background-color 0.2s;
        }
        
        .confirm-btn-yes {
            background-color: #dc3545;
            color: white;
        }
        
        .confirm-btn-yes:hover {
            background-color: #c82333;
        }
        
        .confirm-btn-no {
            background-color: #6c757d;
            color: white;
        }
        
        .confirm-btn-no:hover {
            background-color: #5a6268;
        }
        .reboot-btn {
            margin-top: 12px;
            text-align: right;
        }
        .reboot-link {
            display: inline-flex;
            align-items: center;
            justify-content: center;
            text-decoration: none;
        }
        .warning {
            margin-top: 12px;
            text-align: left;
            color: #e0e0e0;
            font-size: 13px;
            font-weight: 500;
            line-height: 1.3;
        }
        .pulse {
            color: #ff4444;
            animation: pulse 2s infinite ease-in-out;
            font-size: 20px;
            font-weight: bold;
            display: block;
            text-align: center;
            margin-bottom: 5px;
        }
        @keyframes pulse {
            0%, 100% { opacity: 1; }
            50% { opacity: 0.5; }
        }
        .status-indicator {
            position: fixed;
            top: 10px;
            right: 10px;
            width: 10px;
            height: 10px;
            border-radius: 50%;
            background-color: #28a745;
            opacity: 0;
            transition: opacity 0.3s;
        }
        .status-indicator.active {
            opacity: 1;
        }
    </style>
    <script>
        // Simple language detection - defined globally
        function detectLanguage() {
            const lang = navigator.language || navigator.userLanguage;
            if (lang.startsWith('ru')) return 'ru';
            if (lang.startsWith('de')) return 'de';
            if (lang.startsWith('fr')) return 'fr';
            if (lang.startsWith('zh')) return 'zh';
            return 'en';
        }
        
        function showOverlay() {
            document.querySelector('.spinner-overlay').classList.add('show');
        }
        // Custom confirm dialog
        function customConfirm(message, callback) {
            document.getElementById('confirm-message').textContent = message;
            document.getElementById('custom-confirm').classList.add('show');
            
            // Button handlers
            document.getElementById('confirm-yes').onclick = function() {
                document.getElementById('custom-confirm').classList.remove('show');
                callback(true);
            };
            
            document.getElementById('confirm-no').onclick = function() {
                document.getElementById('custom-confirm').classList.remove('show');
                callback(false);
            };
        }

        function confirmReboot(event) {
            event.preventDefault();
            
            // Get current language once
            const currentLang = detectLanguage();
            
            // Get reboot confirmation text in current language
            const rebootMessages = {
                'ru': 'Вы уверены, что хотите перезагрузить устройство?',
                'en': 'Are you sure you want to reboot the device?',
                'de': 'Sind Sie sicher, dass Sie das Gerät neu starten möchten?',
                'fr': 'Êtes-vous sûr de vouloir redémarrer l\'appareil?',
                'zh': '您确定要重启设备吗？'
            };
            const message = rebootMessages[currentLang] || rebootMessages['en'];
            
            // Get rebooting text in current language
            const rebootingTexts = {
                'ru': 'Перезагрузка...',
                'en': 'Rebooting...',
                'de': 'Neustart...',
                'fr': 'Redémarrage...',
                'zh': '重启中...'
            };
            const rebootingText = rebootingTexts[currentLang] || rebootingTexts['en'];
            
            customConfirm(message, function(confirmed) {
                if (confirmed) {
                    document.querySelector('.spinner-overlay').classList.add('show');
                    document.querySelector('.spinner-text').textContent = rebootingText;
                    
                    fetch('reboot.php', {
                        method: 'POST'
                    }).then(() => {
                        // After reboot, wait for connection restoration
                        setTimeout(checkConnection, 3000);
                    }).catch(() => {
                        // If request failed, still wait for restoration
                        setTimeout(checkConnection, 3000);
                    });
                }
            });
            return false;
        }
        
        function checkConnection() {
            fetch('?action=getStatus')
                .then(response => {
                    if (response.ok) {
                        location.reload();
                    } else {
                        setTimeout(checkConnection, 2000);
                    }
                })
                .catch(() => {
                    setTimeout(checkConnection, 2000);
                });
        }
    </script>
</head>
<body>
    <!-- Updated spinner -->
    <div class="spinner-overlay">
	<div class="spinner-container">
        <div class="spinner"></div>
        <div class="spinner-text" data-lang="loading">Loading...</div>
        </div>
    </div>
    <div id="statusIndicator" class="status-indicator"></div>
    <div class="container">
        <div class="header">
            <a href="index.php" class="home-button" title="Home">
                <img src="assets/img/home.svg" class="settings-icon" alt="Home">
            </a>
            <h1 data-lang="i2s_title">I2S Settings</h1>
        </div>
        
        <form method="post" onsubmit="showOverlay()">
            <div class="group">
                <div class="group-header">
                    <h2 data-lang="mode_title">Mode</h2>
                    <div class="toggle-switch-compact">
                        <input type="radio" name="mode" value="pll" id="mode-pll" class="toggle-input-compact" 
                               <?php if ($current_mode === 'pll') echo 'checked'; ?>>
                        <input type="radio" name="mode" value="ext" id="mode-ext" class="toggle-input-compact" 
                               <?php if ($current_mode === 'ext') echo 'checked'; ?>>
                        <label class="toggle-label-compact">
                            <div class="toggle-slider-compact"></div>
                            <span class="toggle-option-compact left" data-lang="pll_mode" onclick="document.getElementById('mode-pll').click()">PLL</span>
                            <span class="toggle-option-compact right" data-lang="ext_mode" onclick="document.getElementById('mode-ext').click()">EXT</span>
                        </label>
                    </div>
                </div>
            </div>
            <div class="group">
                <div class="submode-rows">
                    <div class="submode-row">
                        <button type="submit" name="submode" value="std" id="std-btn"
                            class="btn-custom <?php if ($current_submode === 'std') echo 'active'; ?>" data-lang="std_mode">STD</button>
                        <button type="submit" name="submode" value="8ch" id="8ch-btn"
                            class="btn-custom <?php if ($current_submode === '8ch') echo 'active'; ?>" data-lang="8ch_mode">8CH</button>
                    </div>
                    <div class="submode-row">
                        <button type="submit" name="submode" value="lr" id="lr-btn"
                            class="btn-custom <?php if ($current_submode === 'lr') echo 'active'; ?>" data-lang="lr_mode">L/R</button>
                        <button type="submit" name="submode" value="plr" id="plr-btn"
                            class="btn-custom <?php if ($current_submode === 'plr') echo 'active'; ?>" data-lang="plr_mode">±L/±R</button>
                    </div>
                </div>
            </div>
            <div class="group">
                <div class="group-header">
                    <h2 data-lang="mclk_title">MCLK</h2>
                    <div class="toggle-switch-compact">
                        <input type="radio" name="mclk" value="512" id="mclk-512" class="toggle-input-compact" 
                               <?php if ($current_mclk === '512') echo 'checked'; ?>>
                        <input type="radio" name="mclk" value="1024" id="mclk-1024" class="toggle-input-compact" 
                               <?php if ($current_mclk === '1024') echo 'checked'; ?>>
                        <label class="toggle-label-compact">
                            <div class="toggle-slider-compact"></div>
                            <span class="toggle-option-compact left" onclick="document.getElementById('mclk-512').click()">512</span>
                            <span class="toggle-option-compact right" onclick="document.getElementById('mclk-1024').click()">1024</span>
                        </label>
                    </div>
                </div>
            </div>
            <div class="group">
                <div class="group-header">
                    <h2 data-lang="pcm_swap_title">PCM Swap</h2>
                    <div class="toggle-switch-compact">
                        <input type="radio" name="pcm_swap" value="0" id="pcm-normal" class="toggle-input-compact" 
                               <?php if ($current_pcm_swap === '0') echo 'checked'; ?>>
                        <input type="radio" name="pcm_swap" value="1" id="pcm-swap" class="toggle-input-compact" 
                               <?php if ($current_pcm_swap === '1') echo 'checked'; ?>>
                        <label class="toggle-label-compact">
                            <div class="toggle-slider-compact"></div>
                            <span class="toggle-option-compact left" onclick="document.getElementById('pcm-normal').click()">OFF</span>
                            <span class="toggle-option-compact right" onclick="document.getElementById('pcm-swap').click()">ON</span>
                        </label>
                    </div>
                </div>
            </div>
            <div class="group">
                <div class="group-header">
                    <h2 data-lang="dsd_swap_title">DSD Swap</h2>
                    <div class="toggle-switch-compact">
                        <input type="radio" name="dsd_swap" value="0" id="dsd-normal" class="toggle-input-compact" 
                               <?php if ($current_dsd_swap === '0') echo 'checked'; ?>>
                        <input type="radio" name="dsd_swap" value="1" id="dsd-swap" class="toggle-input-compact" 
                               <?php if ($current_dsd_swap === '1') echo 'checked'; ?>>
                        <label class="toggle-label-compact">
                            <div class="toggle-slider-compact"></div>
                            <span class="toggle-option-compact left" onclick="document.getElementById('dsd-normal').click()">OFF</span>
                            <span class="toggle-option-compact right" onclick="document.getElementById('dsd-swap').click()">ON</span>
                        </label>
                    </div>
                </div>
            </div>
            <div class="warning">
                <span class="pulse" data-lang="warning_attention">Warning!</span>
                <span data-lang="warning_text1">MCLK output has different settings in PLL and EXT modes (OUTPUT/INPUT).</span> <br>
                <span data-lang="warning_text2">System reboot is required after changing I2S settings to apply them.</span>
            </div>
        </form>
        <div class="reboot-btn">
            <a href="#" onclick="confirmReboot(event)" class="reboot-link" title="Reboot">
                <img src="assets/img/reboot.svg" class="settings-icon reboot-icon" alt="Reboot">
            </a>
        </div>
    </div>

    <!-- Custom confirm dialog -->
    <div id="custom-confirm" class="confirm-overlay">
        <div class="confirm-content">
            <div id="confirm-message" class="confirm-message"></div>
            <div class="confirm-buttons">
                <button id="confirm-yes" class="confirm-btn confirm-btn-yes" data-lang="yes_btn">Yes</button>
                <button id="confirm-no" class="confirm-btn confirm-btn-no" data-lang="cancel_btn">Cancel</button>
            </div>
        </div>
    </div>

    <script src="assets/js/jquery-3.7.1.min.js"></script>
    <script>
        // Only load translations, no button state management
        $(document).ready(function() {
            
            const translations = {
                'ru': {
                    'i2s_title': 'Настройки I2S',
                    'mode_title': 'Режим',
                    'pll_mode': 'PLL',
                    'ext_mode': 'EXT',
                    'submode_title': 'Вариант выхода',
                    'std_mode': 'STD',
                    'lr_mode': 'L/R',
                    'plr_mode': '±L/±R',
                    '8ch_mode': '8CH',
                    'mclk_title': 'MCLK',
                    'warning_attention': 'Внимание!',
                    'warning_text1': 'Выход MCLK в режимах PLL и EXT имеет разные настройки (OUTPUT/INPUT).',
                    'warning_text2': 'После изменения настроек I2S необходима перезагрузка системы для вступления в силу.',
                    'yes_btn': 'Да',
                    'cancel_btn': 'Отмена',
                    'loading': 'Загрузка...',
                    'pcm_swap_title': 'PCM Swap',
                    'dsd_swap_title': 'DSD Swap'
                },
                'en': {
                    'i2s_title': 'I2S Settings',
                    'mode_title': 'Mode',
                    'pll_mode': 'PLL',
                    'ext_mode': 'EXT',
                    'submode_title': 'Output Mode',
                    'std_mode': 'STD',
                    'lr_mode': 'L/R',
                    'plr_mode': '±L/±R',
                    '8ch_mode': '8CH',
                    'mclk_title': 'MCLK',
                    'warning_attention': 'Warning!',
                    'warning_text1': 'MCLK output has different settings in PLL and EXT modes (OUTPUT/INPUT).',
                    'warning_text2': 'System reboot is required after changing I2S settings to apply them.',
                    'yes_btn': 'Yes',
                    'cancel_btn': 'Cancel',
                    'loading': 'Loading...',
                    'pcm_swap_title': 'PCM Swap',
                    'dsd_swap_title': 'DSD Swap'
                },
                'de': {
                    'i2s_title': 'I2S Einstellungen',
                    'mode_title': 'Modus',
                    'pll_mode': 'PLL',
                    'ext_mode': 'EXT',
                    'submode_title': 'Ausgangsmodus',
                    'std_mode': 'STD',
                    'lr_mode': 'L/R',
                    'plr_mode': '±L/±R',
                    '8ch_mode': '8CH',
                    'mclk_title': 'MCLK',
                    'warning_attention': 'Achtung!',
                    'warning_text1': 'MCLK-Ausgang hat unterschiedliche Einstellungen in PLL- und EXT-Modi (OUTPUT/INPUT).',
                    'warning_text2': 'Systemneustart ist erforderlich, nachdem I2S-Einstellungen geändert wurden.',
                    'yes_btn': 'Ja',
                    'cancel_btn': 'Abbrechen',
                    'loading': 'Laden...',
                    'pcm_swap_title': 'PCM Swap',
                    'dsd_swap_title': 'DSD Swap'
                },
                'fr': {
                    'i2s_title': 'Paramètres I2S',
                    'mode_title': 'Mode',
                    'pll_mode': 'PLL',
                    'ext_mode': 'EXT',
                    'submode_title': 'Mode de sortie',
                    'std_mode': 'STD',
                    'lr_mode': 'L/R',
                    'plr_mode': '±L/±R',
                    '8ch_mode': '8CH',
                    'mclk_title': 'MCLK',
                    'warning_attention': 'Attention!',
                    'warning_text1': 'La sortie MCLK a des paramètres différents en modes PLL et EXT (OUTPUT/INPUT).',
                    'warning_text2': 'Un redémarrage du système est nécessaire après modification des paramètres I2S.',
                    'yes_btn': 'Oui',
                    'cancel_btn': 'Annuler',
                    'loading': 'Chargement...',
                    'pcm_swap_title': 'PCM Swap',
                    'dsd_swap_title': 'DSD Swap'
                },
                'zh': {
                    'i2s_title': 'I2S 设置',
                    'mode_title': '模式',
                    'pll_mode': 'PLL',
                    'ext_mode': 'EXT',
                    'submode_title': '输出模式',
                    'std_mode': 'STD',
                    'lr_mode': 'L/R',
                    'plr_mode': '±L/±R',
                    '8ch_mode': '8CH',
                    'mclk_title': 'MCLK',
                    'warning_attention': '注意！',
                    'warning_text1': 'MCLK输出在PLL和EXT模式下具有不同的设置（OUTPUT/INPUT）。',
                    'warning_text2': '更改I2S设置后需要重启系统才能生效。',
                    'yes_btn': '是',
                    'cancel_btn': '取消',
                    'loading': '加载中...',
                    'pcm_swap_title': 'PCM Swap',
                    'dsd_swap_title': 'DSD Swap'
                }
            };
            
            const currentLang = detectLanguage();
            
            // Apply translations without touching button states
            $('[data-lang]').each(function() {
                const key = $(this).data('lang');
                if (translations[currentLang] && translations[currentLang][key]) {
                    $(this).text(translations[currentLang][key]);
                }
            });
            
            // Handle toggle switches - submit form when radio button changes
            document.querySelectorAll('.toggle-input-compact').forEach(input => {
                input.addEventListener('change', function() {
                    if (this.checked) {
                        setTimeout(() => {
                            this.closest('form').submit();
                        }, 200);
                    }
                });
            });
        });
    </script>
</body>
</html>