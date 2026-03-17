#define _GNU_SOURCE

/*
 * clui-shell.c — GTK4 layer-shell overlay for CLUI-CC on Hyprland/wlroots
 *
 * Provides:
 *  - Layer-shell OVERLAY surface (always on top, no taskbar entry)
 *  - WebKitGTK webview loading the React frontend
 *  - Dynamic input regions (click-through for transparent areas)
 *  - Unix socket for IPC with Node.js backend + toggle script
 *  - Multi-display: follows cursor to correct output
 */

#include <gtk/gtk.h>
#include <gtk4-layer-shell.h>
#include <webkit/webkit.h>
#include <json-glib/json-glib.h>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

/* ─── Constants ─── */

#define SOCKET_ENV       "CLUI_SOCKET"
#define SOCKET_DEFAULT   "/tmp/clui-shell.sock"
#define CONTENT_URL_ENV  "CLUI_CONTENT_URL"
#define CONTENT_URL_DEFAULT "http://localhost:5173"

#define WINDOW_WIDTH     1040
#define WINDOW_HEIGHT    720
#define PILL_BOTTOM_MARGIN 24

/* ─── Globals ─── */

static GtkWindow    *main_window = NULL;
static WebKitWebView *webview    = NULL;
static gboolean      visible     = TRUE;
static int           ipc_server_fd  = -1;
static int           backend_conn_fd = -1;

/* ─── Forward declarations ─── */

static void setup_layer_shell(GtkWindow *window);
static void setup_webview(GtkWindow *window, const char *content_url, const char *bridge_js);
static void setup_ipc_socket(const char *socket_path);
static void handle_ipc_message(const char *msg);
static void update_input_region(int x, int y, int width, int height);
static void toggle_visibility(void);
static char *load_bridge_js(void);
static void on_script_message(WebKitUserContentManager *manager,
                              JSCValue *js_result,
                              gpointer user_data);

/* ─── Layer Shell Setup ─── */

static void
setup_layer_shell(GtkWindow *window)
{
    gtk_layer_init_for_window(window);
    gtk_layer_set_layer(window, GTK_LAYER_SHELL_LAYER_OVERLAY);
    gtk_layer_set_namespace(window, "clui-cc");

    /* Don't reserve screen space — overlay floats freely */
    gtk_layer_set_exclusive_zone(window, -1);

    /* Keyboard on demand: gets focus when clicked, transparent otherwise */
    gtk_layer_set_keyboard_mode(window,
        GTK_LAYER_SHELL_KEYBOARD_MODE_ON_DEMAND);

    /* Anchor to bottom-center */
    gtk_layer_set_anchor(window, GTK_LAYER_SHELL_EDGE_BOTTOM, TRUE);
    gtk_layer_set_margin(window, GTK_LAYER_SHELL_EDGE_BOTTOM, PILL_BOTTOM_MARGIN);

    /* Default size */
    gtk_window_set_default_size(window, WINDOW_WIDTH, WINDOW_HEIGHT);
}

/* ─── Input Region (click-through) ─── */

static void
update_input_region(int x, int y, int width, int height)
{
    if (!main_window) return;

    GtkNative *native = GTK_NATIVE(main_window);
    GdkSurface *surface = gtk_native_get_surface(native);
    if (!surface) return;

    cairo_region_t *region;

    if (width <= 0 || height <= 0) {
        /* Empty region = fully click-through */
        region = cairo_region_create();
    } else {
        cairo_rectangle_int_t rect = { x, y, width, height };
        region = cairo_region_create_rectangle(&rect);
    }

    /* This calls wl_surface_set_input_region under the hood */
    gdk_surface_set_input_region(surface, region);
    cairo_region_destroy(region);
}

/* ─── Visibility Toggle ─── */

static void
toggle_visibility(void)
{
    if (!main_window) return;

    if (visible) {
        gtk_widget_set_visible(GTK_WIDGET(main_window), FALSE);
        visible = FALSE;
    } else {
        gtk_widget_set_visible(GTK_WIDGET(main_window), TRUE);
        visible = TRUE;

        /* Notify renderer that window is shown */
        if (webview) {
            webkit_web_view_evaluate_javascript(webview,
                "window.__cluiOnWindowShown && window.__cluiOnWindowShown()",
                -1, NULL, NULL, NULL, NULL, NULL);
        }
    }
}

/* ─── WebKitGTK Script Message Handler ─── */

/*
 * Messages from the renderer arrive as JSON:
 *   { "channel": "clui:some-channel", "args": [...] }
 *
 * We forward them to the Node.js backend over the Unix socket,
 * except for window-management messages we handle locally.
 */
static void
on_script_message(WebKitUserContentManager *manager,
                  JSCValue                 *js_result,
                  gpointer                  user_data)
{
    (void)manager;
    (void)user_data;

    /* Use the global backend_conn_fd rather than signal user_data,
       so we don't need to reconnect the signal on each new connection. */
    char *json_str = jsc_value_to_json(js_result, 0);
    if (!json_str) return;

    /* Parse to check for local handling */
    JsonParser *parser = json_parser_new();
    if (!json_parser_load_from_data(parser, json_str, -1, NULL)) {
        g_free(json_str);
        g_object_unref(parser);
        return;
    }

    JsonNode *root = json_parser_get_root(parser);
    JsonObject *obj = json_node_get_object(root);
    const char *channel = json_object_get_string_member(obj, "channel");

    if (channel) {
        /* Handle window management locally */
        if (g_strcmp0(channel, "clui:hide-window") == 0) {
            gtk_widget_set_visible(GTK_WIDGET(main_window), FALSE);
            visible = FALSE;
        } else if (g_strcmp0(channel, "clui:set-input-region") == 0) {
            JsonArray *args = json_object_get_array_member(obj, "args");
            if (args && json_array_get_length(args) >= 4) {
                int rx = (int)json_array_get_int_element(args, 0);
                int ry = (int)json_array_get_int_element(args, 1);
                int rw = (int)json_array_get_int_element(args, 2);
                int rh = (int)json_array_get_int_element(args, 3);
                update_input_region(rx, ry, rw, rh);
            }
        } else if (backend_conn_fd >= 0) {
            /* Forward to Node.js backend */
            size_t len = strlen(json_str);
            char *msg = g_strdup_printf("%s\n", json_str);
            if (write(backend_conn_fd, msg, len + 1) < 0) {
                g_warning("Failed to write to backend socket: %s", g_strerror(errno));
            }
            g_free(msg);
        }
    }

    g_free(json_str);
    g_object_unref(parser);
}

/* ─── WebView Setup ─── */

static void
setup_webview(GtkWindow *window, const char *content_url, const char *bridge_js)
{
    WebKitUserContentManager *ucm = webkit_user_content_manager_new();

    /* Inject the bridge script that creates window.clui */
    if (bridge_js) {
        WebKitUserScript *script = webkit_user_script_new(
            bridge_js,
            WEBKIT_USER_CONTENT_INJECT_TOP_FRAME,
            WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START,
            NULL, NULL);
        webkit_user_content_manager_add_script(ucm, script);
        webkit_user_script_unref(script);
    }

    /* Register message handler: renderer calls
       window.webkit.messageHandlers.clui.postMessage({...}) */
    g_signal_connect(ucm, "script-message-received::clui",
                     G_CALLBACK(on_script_message), NULL);
    webkit_user_content_manager_register_script_message_handler(ucm, "clui", NULL);

    webview = WEBKIT_WEB_VIEW(
        g_object_new(WEBKIT_TYPE_WEB_VIEW,
                     "user-content-manager", ucm,
                     NULL));

    /* Transparent background for the overlay */
    GdkRGBA transparent = { 0.0, 0.0, 0.0, 0.0 };
    webkit_web_view_set_background_color(webview, &transparent);

    /* Enable dev tools in debug mode */
    if (g_getenv("CLUI_DEBUG")) {
        WebKitSettings *settings = webkit_web_view_get_settings(webview);
        webkit_settings_set_enable_developer_extras(settings, TRUE);
    }

    gtk_window_set_child(window, GTK_WIDGET(webview));
    webkit_web_view_load_uri(webview, content_url);
}

/* ─── IPC Socket (for toggle script + Node.js backend commands) ─── */

/* Buffer for reading partial lines from backend */
static char ipc_read_buf[65536];
static size_t ipc_read_len = 0;

static gboolean
on_ipc_client_data(GIOChannel *source, GIOCondition condition, gpointer data)
{
    (void)data;

    if (condition & (G_IO_HUP | G_IO_ERR)) {
        backend_conn_fd = -1;
        ipc_read_len = 0;
        return FALSE;
    }

    int fd = g_io_channel_unix_get_fd(source);
    ssize_t n = read(fd, ipc_read_buf + ipc_read_len,
                     sizeof(ipc_read_buf) - ipc_read_len - 1);
    if (n <= 0) {
        backend_conn_fd = -1;
        ipc_read_len = 0;
        return FALSE;
    }

    ipc_read_len += n;
    ipc_read_buf[ipc_read_len] = '\0';

    /* Process complete newline-delimited messages */
    char *start = ipc_read_buf;
    char *nl;
    while ((nl = strchr(start, '\n')) != NULL) {
        *nl = '\0';
        if (nl > start) {
            handle_ipc_message(start);
        }
        start = nl + 1;
    }

    /* Shift remaining partial data to front */
    size_t remaining = ipc_read_len - (start - ipc_read_buf);
    if (remaining > 0 && start != ipc_read_buf) {
        memmove(ipc_read_buf, start, remaining);
    }
    ipc_read_len = remaining;

    return TRUE;
}

static gboolean
on_ipc_connection(GIOChannel *source, GIOCondition condition, gpointer data)
{
    (void)condition;
    (void)data;

    int server_fd = g_io_channel_unix_get_fd(source);
    int client_fd = accept(server_fd, NULL, NULL);
    if (client_fd < 0) return TRUE;

    /* Only keep one backend connection at a time */
    if (backend_conn_fd >= 0) {
        close(backend_conn_fd);
    }
    backend_conn_fd = client_fd;
    ipc_read_len = 0;

    /* on_script_message reads backend_conn_fd directly, no signal reconnect needed */

    GIOChannel *ch = g_io_channel_unix_new(client_fd);
    g_io_channel_set_encoding(ch, NULL, NULL);
    g_io_add_watch(ch, G_IO_IN | G_IO_HUP | G_IO_ERR,
                   on_ipc_client_data, NULL);
    g_io_channel_unref(ch);

    return TRUE;
}

static void
setup_ipc_socket(const char *socket_path)
{
    /* Remove stale socket */
    unlink(socket_path);

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        g_error("Failed to create socket: %s", g_strerror(errno));
        return;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        g_error("Failed to bind socket %s: %s", socket_path, g_strerror(errno));
        close(fd);
        return;
    }

    if (listen(fd, 4) < 0) {
        g_error("Failed to listen: %s", g_strerror(errno));
        close(fd);
        return;
    }

    ipc_server_fd = fd;

    GIOChannel *ch = g_io_channel_unix_new(fd);
    g_io_channel_set_encoding(ch, NULL, NULL);
    g_io_add_watch(ch, G_IO_IN, on_ipc_connection, NULL);
    g_io_channel_unref(ch);

    g_message("IPC socket listening on %s", socket_path);
}

/* ─── IPC Message Dispatch ─── */

static void
handle_ipc_message(const char *msg)
{
    JsonParser *parser = json_parser_new();
    if (!json_parser_load_from_data(parser, msg, -1, NULL)) {
        g_warning("Invalid IPC JSON: %s", msg);
        g_object_unref(parser);
        return;
    }

    JsonNode *root = json_parser_get_root(parser);
    JsonObject *obj = json_node_get_object(root);
    const char *cmd = json_object_get_string_member(obj, "cmd");

    if (!cmd) {
        g_object_unref(parser);
        return;
    }

    if (g_strcmp0(cmd, "toggle") == 0) {
        toggle_visibility();
    } else if (g_strcmp0(cmd, "show") == 0) {
        if (!visible) toggle_visibility();
    } else if (g_strcmp0(cmd, "hide") == 0) {
        if (visible) toggle_visibility();
    } else if (g_strcmp0(cmd, "input-region") == 0) {
        int x = (int)json_object_get_int_member(obj, "x");
        int y = (int)json_object_get_int_member(obj, "y");
        int w = (int)json_object_get_int_member(obj, "width");
        int h = (int)json_object_get_int_member(obj, "height");
        update_input_region(x, y, w, h);
    } else if (g_strcmp0(cmd, "eval") == 0) {
        /* Evaluate JS in the webview (for backend → renderer messages) */
        const char *js = json_object_get_string_member(obj, "js");
        if (js && webview) {
            webkit_web_view_evaluate_javascript(webview,
                js, -1, NULL, NULL, NULL, NULL, NULL);
        }
    } else if (g_strcmp0(cmd, "quit") == 0) {
        g_application_quit(G_APPLICATION(
            gtk_window_get_application(main_window)));
    }

    g_object_unref(parser);
}

/* ─── Bridge JS Loader ─── */

static char *
load_bridge_js(void)
{
    /* Look for bridge.js next to the binary, or in known locations */
    const char *candidates[] = {
        NULL, /* filled below */
        "/usr/share/clui-cc/bridge.js",
        "/usr/local/share/clui-cc/bridge.js",
    };

    /* Build path relative to executable */
    char exe_dir[4096];
    ssize_t len = readlink("/proc/self/exe", exe_dir, sizeof(exe_dir) - 1);
    if (len > 0) {
        exe_dir[len] = '\0';
        char *slash = strrchr(exe_dir, '/');
        if (slash) {
            *(slash + 1) = '\0';
            strcat(exe_dir, "bridge.js");
            candidates[0] = exe_dir;
        }
    }

    /* Also check CLUI_BRIDGE_JS env var */
    const char *env_path = g_getenv("CLUI_BRIDGE_JS");
    if (env_path) {
        char *contents = NULL;
        gsize length = 0;
        if (g_file_get_contents(env_path, &contents, &length, NULL)) {
            return contents;
        }
    }

    for (int i = 0; i < 3; i++) {
        if (!candidates[i]) continue;
        char *contents = NULL;
        gsize length = 0;
        if (g_file_get_contents(candidates[i], &contents, &length, NULL)) {
            g_message("Loaded bridge.js from %s", candidates[i]);
            return contents;
        }
    }

    g_warning("bridge.js not found — window.clui will be unavailable");
    return NULL;
}

/* ─── Cleanup ─── */

static void
cleanup(void)
{
    if (ipc_server_fd >= 0) {
        close(ipc_server_fd);
        ipc_server_fd = -1;
    }
    if (backend_conn_fd >= 0) {
        close(backend_conn_fd);
        backend_conn_fd = -1;
    }

    const char *socket_path = g_getenv(SOCKET_ENV);
    if (!socket_path) socket_path = SOCKET_DEFAULT;
    unlink(socket_path);
}

static void
deferred_input_region_clear(gpointer data)
{
    (void)data;
    update_input_region(0, 0, 0, 0);
}

/* ─── Application Activate ─── */

static void
on_activate(GtkApplication *app, gpointer user_data)
{
    (void)user_data;

    main_window = GTK_WINDOW(gtk_application_window_new(app));
    gtk_window_set_title(main_window, "CLUI CC");

    /* Layer shell: overlay, non-activating, click-through */
    setup_layer_shell(main_window);

    /* Load bridge.js for window.clui API injection */
    char *bridge_js = load_bridge_js();

    /* Content URL */
    const char *content_url = g_getenv(CONTENT_URL_ENV);
    if (!content_url) content_url = CONTENT_URL_DEFAULT;

    /* WebKitGTK webview with transparent background */
    setup_webview(main_window, content_url, bridge_js);
    g_free(bridge_js);

    /* Start with empty input region (fully click-through) until renderer
       reports its interactive bounds */
    gtk_widget_set_visible(GTK_WIDGET(main_window), TRUE);

    /* Defer input region setup until after the surface is realized */
    g_idle_add_once((GSourceOnceFunc)deferred_input_region_clear, NULL);

    /* IPC socket for toggle script + backend */
    const char *socket_path = g_getenv(SOCKET_ENV);
    if (!socket_path) socket_path = SOCKET_DEFAULT;
    setup_ipc_socket(socket_path);
}

/* ─── Main ─── */

int
main(int argc, char *argv[])
{
    atexit(cleanup);

    GtkApplication *app = gtk_application_new("com.clui.shell",
                                               G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);

    int status = g_application_run(G_APPLICATION(app), argc, argv);

    g_object_unref(app);
    return status;
}
