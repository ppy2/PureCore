<?php
// System shutdown script
if ($_SERVER['REQUEST_METHOD'] === 'POST') {
    exec('/usr/bin/sudo /opt/shutdown.sh > /dev/null 2>&1 &');
    echo "OK";
}
?>