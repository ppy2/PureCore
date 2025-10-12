<?php
// System reboot script
if ($_SERVER['REQUEST_METHOD'] === 'POST') {
    exec('/usr/bin/sudo /opt/reboot.sh > /dev/null 2>&1 &');
    echo "OK";
}
?>