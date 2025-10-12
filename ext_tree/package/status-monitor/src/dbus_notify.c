#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dbus/dbus.h>

#define DBUS_SERVICE_NAME "org.purefox.statusmonitor"
#define DBUS_OBJECT_PATH "/org/purefox/statusmonitor"
#define DBUS_INTERFACE_NAME "org.purefox.StatusMonitor"

// Send D-Bus signal
int send_dbus_signal(const char* signal_name, const char* message) {
    DBusConnection *conn;
    DBusMessage *msg;
    DBusError error;
    
    dbus_error_init(&error);
    
    // Connect to system bus
    conn = dbus_bus_get(DBUS_BUS_SYSTEM, &error);
    if (dbus_error_is_set(&error)) {
        fprintf(stderr, "D-Bus connection error: %s\n", error.message);
        dbus_error_free(&error);
        return -1;
    }
    
    // Create signal message
    msg = dbus_message_new_signal(DBUS_OBJECT_PATH, DBUS_INTERFACE_NAME, signal_name);
    if (!msg) {
        fprintf(stderr, "Failed to create D-Bus message\n");
        return -1;
    }
    
    // Add arguments
    if (message) {
        dbus_message_append_args(msg,
                                DBUS_TYPE_STRING, &message,
                                DBUS_TYPE_INVALID);
    }
    
    // Send signal
    if (!dbus_connection_send(conn, msg, NULL)) {
        fprintf(stderr, "Failed to send D-Bus signal\n");
        dbus_message_unref(msg);
        return -1;
    }
    
    // Force send
    dbus_connection_flush(conn);
    
    // Cleanup
    dbus_message_unref(msg);
    
    printf("D-Bus signal '%s' sent successfully\n", signal_name);
    return 0;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: %s <signal_name> [message]\n", argv[0]);
        printf("Examples:\n");
        printf("  %s ServiceChanged \"mpd started\"\n", argv[0]);
        printf("  %s VolumeChanged \"volume changed\"\n", argv[0]);
        return 1;
    }
    
    const char *signal_name = argv[1];
    const char *message = (argc > 2) ? argv[2] : NULL;
    
    return send_dbus_signal(signal_name, message);
}