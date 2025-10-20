<?php
header('Content-Type: application/json; charset=utf-8');
error_reporting(E_ALL);
ini_set('display_errors', 1);

// Lock file to prevent concurrent service switches
$lockfile = '/tmp/service_switch.lock';
$lock_fp = null; // Lock file pointer

function logMessage($message) {
    error_log("[Player Manager] " . $message);
}

function executeCommand($command) {
    logMessage("Executing: $command");
    $output = shell_exec("/usr/bin/sudo $command 2>&1");
    logMessage("Output: " . trim((string)$output));
    return trim((string)$output);
}

function acquireLock($lockfile) {
    global $lock_fp;
    $lock_fp = fopen($lockfile, 'c');
    if (!$lock_fp) {
        throw new Exception("Cannot open lock file");
    }
    
    // Non-blocking exclusive lock
    if (!flock($lock_fp, LOCK_EX | LOCK_NB)) {
        fclose($lock_fp);
        $lock_fp = null;
        throw new Exception("Service switch already in progress, please wait");
    }
    
    logMessage("Lock acquired");
    return $lock_fp;
}

function releaseLock() {
    global $lock_fp;
    if ($lock_fp) {
        flock($lock_fp, LOCK_UN);
        fclose($lock_fp);
        $lock_fp = null;
        logMessage("Lock released");
    }
}

$players = [
    'naa' => ['process' => 'networkaudiod', 'script' => 'S95naa'],
    'raat' => ['process' => 'raat_app', 'script' => 'S95roonready'],
    'mpd' => ['process' => 'mpd', 'script' => 'S95mpd'],
    'aprenderer' => ['process' => 'ap2renderer', 'script' => 'S95aprenderer'],
    'squeeze2upn' => ['process' => 'squeeze2upn', 'script' => 'S95apsq'],
    'aplayer' => ['process' => 'aplayer', 'script' => 'S95aplayer'],
    'apscream' => ['process' => 'apscream', 'script' => 'S95apscream'],
    'shairport' => ['process' => 'shairport-sync', 'script' => 'S95shairport'],
    'lms' => ['process' => 'squeezelite', 'script' => 'S95squeezelite'],
//    'screen-audio' => ['process' => 'screen_audio', 'script' => 'S95screen-audio'],
    'spotify' => ['process' => 'librespot', 'script' => 'S95spotify'],
    'qobuz' => ['process' => 'qobuz-connect', 'script' => 'S95qobuz'],
    'tidalconnect' => ['process' => 'tidalconnect', 'script' => 'S95tidal'],
];

try {
    if ($_SERVER['REQUEST_METHOD'] !== 'POST') {
        throw new Exception("Invalid request method");
    }

    $player_to_start = $_POST['service'] ?? '';
    if (!isset($players[$player_to_start])) {
        throw new Exception("Invalid player: $player_to_start");
    }

    logMessage("Request to start player: $player_to_start");

    // Acquire lock to prevent concurrent switches
    acquireLock($lockfile);

    // Check script existence
    $script_path = "/etc/rc.pure/{$players[$player_to_start]['script']}";
    if (!file_exists($script_path)) {
        releaseLock();
        throw new Exception("Player script not found: $script_path");
    }

    // Stop all current players
    executeCommand("/etc/init.d/S95* stop");

    // Remove all S95* from /etc/init.d/
    executeCommand("/bin/rm -f /etc/init.d/S95*");

    // Create symlink
    $target_link = "/etc/init.d/{$players[$player_to_start]['script']}";
    executeCommand("/bin/ln -s '$script_path' '$target_link'");
    
    // Start player
    logMessage("Starting player: $player_to_start");
    $start_output = executeCommand("$target_link start");

    // Send D-Bus signal about service change for instant update
    executeCommand("/opt/dbus_notify ServiceChanged \"$player_to_start\"");

    // Wait for system_monitor to confirm player started (up to 10 seconds)
    $confirmed = false;
    for ($i = 0; $i < 20; $i++) {
        $status_file = '/tmp/system_status.json';
        if (file_exists($status_file)) {
            $content = @file_get_contents($status_file);
            if ($content) {
                $status = @json_decode($content, true);
                if ($status && isset($status['active_service']) && $status['active_service'] === $player_to_start) {
                    $confirmed = true;
                    logMessage("Player $player_to_start confirmed by system_monitor");
                    break;
                }
            }
        }
        usleep(500000); // 500ms
    }

    // Release lock after confirmation or timeout
    releaseLock();

    if (!$confirmed) {
        logMessage("Warning: Player $player_to_start not confirmed by system_monitor within 10s");
    }

    echo json_encode([
        'status' => 'success',
        'message' => "Successfully started $player_to_start",
        'confirmed' => $confirmed
    ]);

} catch (Exception $e) {
    releaseLock();
    logMessage("Error: " . $e->getMessage());
    echo json_encode([
        'status' => 'error',
        'message' => $e->getMessage()
    ]);
}
?>
