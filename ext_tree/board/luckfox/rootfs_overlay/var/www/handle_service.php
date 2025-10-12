<?php
header('Content-Type: application/json; charset=utf-8');
error_reporting(E_ALL);
ini_set('display_errors', 1);



function logMessage($message) {
    error_log("[Player Manager] " . $message);
}

function executeCommand($command) {
    logMessage("Executing: $command");
    $output = shell_exec("/usr/bin/sudo $command 2>&1");
    logMessage("Output: " . trim((string)$output));
    return trim((string)$output);
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

    // Check script existence
    $script_path = "/etc/rc.pure/{$players[$player_to_start]['script']}";
    if (!file_exists($script_path)) {
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

    // Check startup
//    $check = executeCommand("/usr/bin/pgrep -x {$players[$player_to_start]['process']}");
//    if (empty($check)) {
//        throw new Exception("Failed to start player: $player_to_start. Output: $start_output");
//    }

    echo json_encode([
        'status' => 'success',
        'message' => "Successfully started $player_to_start"
    ]);

} catch (Exception $e) {
    logMessage("Error: " . $e->getMessage());
    echo json_encode([
        'status' => 'error',
        'message' => $e->getMessage()
    ]);
}
?>
