<?php
header('Content-Type: application/json');

if ($_SERVER['REQUEST_METHOD'] !== 'POST') {
    http_response_code(405);
    echo json_encode(['error' => 'Method not allowed']);
    exit;
}

// Function to read control from cache
function getCachedControl() {
    $cache_file = '/tmp/mixer_control_cache';
    
    if (file_exists($cache_file)) {
        $cache_content = file_get_contents($cache_file);
        // Format: 'E70 Velvet',0 or DX3 Pro ,0 or cache:DX3 Pro ,0
        
        // Remove cache: prefix if present
        $cache_content = trim($cache_content);
        if (strpos($cache_content, 'cache:') === 0) {
            $cache_content = substr($cache_content, 6);
        }
        
        // Parse format: 'Name',0 or Name,0
        if (preg_match("/^'([^']+)',\d+$/", $cache_content, $matches)) {
            // Name in single quotes: 'E70 Velvet',0
            return $matches[1];
        } elseif (preg_match("/^([^,]+),\d+$/", $cache_content, $matches)) {
            // Name without quotes: DX3 Pro ,0
            return $matches[1];
        }
    }
    
    // Fallback to PCM if cache unavailable
    return 'PCM';
}

// Function to read all data from system_status.json
function getSystemStatus() {
    $status_file = '/tmp/system_status.json';
    
    if (file_exists($status_file)) {
        $json = file_get_contents($status_file);
        $status = json_decode($json, true);
        if ($status !== null) {
            return $status;
        }
    }
    
    // Fallback - assume controls are available
    return [
        'volume_control_available' => true,
        'mute_control_available' => true,
        'volume' => '100%',
        'muted' => false
    ];
}

$action = $_POST['action'] ?? '';
$control = getCachedControl();
$system_status = getSystemStatus();

switch ($action) {
    case 'volume_up':
        if (!$system_status['volume_control_available']) {
            echo json_encode(['error' => 'Volume control not available']);
            exit;
        }
        
        exec('/usr/bin/amixer -q sset "' . $control . '" 5%+ 2>/dev/null', $output, $return_code);
        shell_exec('/opt/dbus_notify VolumeChanged "volume_up" 2>/dev/null &');
        
        // Trigger immediate status update
        shell_exec('/usr/bin/killall -USR1 dbus_monitor 2>/dev/null &');
        
        if ($return_code === 0) {
            echo json_encode(['success' => true]);
        } else {
            http_response_code(500);
            echo json_encode(['error' => 'Command failed', 'control' => $control]);
        }
        exit;
        
    case 'volume_down':
        if (!$system_status['volume_control_available']) {
            echo json_encode(['error' => 'Volume control not available']);
            exit;
        }
        
        exec('/usr/bin/amixer -q sset "' . $control . '" 5%- 2>/dev/null', $output, $return_code);
        shell_exec('/opt/dbus_notify VolumeChanged "volume_down" 2>/dev/null &');
        
        // Trigger immediate status update
        shell_exec('/usr/bin/killall -USR1 dbus_monitor 2>/dev/null &');
        
        if ($return_code === 0) {
            echo json_encode(['success' => true]);
        } else {
            http_response_code(500);
            echo json_encode(['error' => 'Command failed', 'control' => $control]);
        }
        exit;
        
    case 'get_volume':
        // Always get data from system_status.json only
        echo json_encode([
            'volume' => $system_status['volume'] ?? '100%',
            'control' => $control,
            'muted' => $system_status['muted'] ?? false,
            'timestamp' => $system_status['timestamp'] ?? time()
        ]);
        exit;
        
    case 'set_volume':
        if (!$system_status['volume_control_available']) {
            echo json_encode(['error' => 'Volume control not available']);
            exit;
        }
        
        $volume = intval($_POST['volume'] ?? 0);
        if ($volume >= 0 && $volume <= 100) {
            exec("/usr/bin/amixer -q sset \"$control\" {$volume}% 2>/dev/null", $output, $return_code);
            shell_exec("/opt/dbus_notify VolumeChanged \"set_volume_{$volume}\" 2>/dev/null &");
            
            // Триггерим немедленное обновление статуса
            shell_exec('/usr/bin/killall -USR1 dbus_monitor 2>/dev/null &');
            
            if ($return_code === 0) {
                echo json_encode(['success' => true]);
            } else {
                http_response_code(500);
                echo json_encode(['error' => 'Command failed', 'control' => $control]);
            }
        } else {
            http_response_code(400);
            echo json_encode(['error' => 'Invalid volume level']);
        }
        exit;
        
    case 'toggle_mute':
        if (!$system_status['mute_control_available']) {
            echo json_encode(['error' => 'Mute control not available']);
            exit;
        }
        
        // Get current state from system_status.json
        $is_muted = $system_status['muted'] ?? false;
        
        if ($is_muted) {
            exec('/usr/bin/amixer -q sset "' . $control . '" unmute 2>/dev/null', $output, $return_code);
            shell_exec('/opt/dbus_notify VolumeChanged "unmute" 2>/dev/null &');
            $new_state = false;
        } else {
            exec('/usr/bin/amixer -q sset "' . $control . '" mute 2>/dev/null', $output, $return_code);
            shell_exec('/opt/dbus_notify VolumeChanged "mute" 2>/dev/null &');
            $new_state = true;
        }
        
        // Trigger immediate status update
        shell_exec('/usr/bin/killall -USR1 dbus_monitor 2>/dev/null &');
        
        if ($return_code === 0) {
            echo json_encode(['success' => true, 'muted' => $new_state]);
        } else {
            http_response_code(500);
            echo json_encode(['error' => 'Command failed', 'control' => $control]);
        }
        exit;
        
    default:
        http_response_code(400);
        echo json_encode(['error' => 'Invalid action']);
        exit;
}
?>
