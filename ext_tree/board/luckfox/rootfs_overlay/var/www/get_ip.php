<?php
header('Content-Type: text/plain');

// Determine device IP address
$ip = $_SERVER['SERVER_ADDR'] ?? gethostbyname(gethostname());

echo trim($ip);
?>

