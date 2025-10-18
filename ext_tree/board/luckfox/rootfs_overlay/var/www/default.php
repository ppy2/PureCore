<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Reset to Default</title>
    <script src="assets/js/jquery-3.7.1.min.js"></script>
</head>
<body>
    <script>
        // Clear hidden buttons from localStorage
        localStorage.removeItem('hiddenButtons');
        
        // Redirect to main page
        window.location.href = '/';
    </script>
</body>
</html>
