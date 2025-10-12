<?php
header('Content-Type: text/plain');

// Open process and get real stdout stream from script execution
$cmd = "/usr/bin/sudo /opt/update.sh 2>&1";
$descriptorspec = array(
    1 => array("pipe", "w"), // stdout
    2 => array("pipe", "w")  // stderr
);

$process = proc_open($cmd, $descriptorspec, $pipes);

if (is_resource($process)) {
    while ($line = fgets($pipes[1])) {
        echo $line;
        ob_flush();
        flush();
    }
    fclose($pipes[1]);
    fclose($pipes[2]);
    proc_close($process);
}
?>

