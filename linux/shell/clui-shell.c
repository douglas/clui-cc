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
#define CONTENT_URL_DEFAULT "http://127.0.0.1:5173"

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
static void setup_webview(GtkWindow *window, const char *content_url);
static void setup_ipc_socket(const char *socket_path);
static void handle_ipc_message(const char *msg);
static void update_input_region(int x, int y, int width, int height);
static void toggle_visibility(void);
static void on_script_message(WebKitUserContentManager *manager,
                              JSCValue *js_result,
                              gpointer user_data);
static void on_load_changed(WebKitWebView *wv, WebKitLoadEvent event,
                            gpointer data);
static gboolean on_load_failed(WebKitWebView *wv, WebKitLoadEvent event,
                               const char *failing_uri, GError *error,
                               gpointer data);
static gboolean on_key_pressed(GtkEventControllerKey *ctrl, guint keyval,
                               guint keycode, GdkModifierType state,
                               gpointer data);

/* ─── Layer Shell Setup ─── */

static void
setup_layer_shell(GtkWindow *window)
{
    fprintf(stderr, "[layer-shell] Initializing layer shell\n");
    gtk_layer_init_for_window(window);
    gtk_layer_set_layer(window, GTK_LAYER_SHELL_LAYER_OVERLAY);
    gtk_layer_set_namespace(window, "clui-cc");

    /* Full-screen transparent overlay — anchor all edges, overlay everything */
    gtk_layer_set_exclusive_zone(window, -1);

    gtk_layer_set_keyboard_mode(window,
        GTK_LAYER_SHELL_KEYBOARD_MODE_ON_DEMAND);
    fprintf(stderr, "[layer-shell] Keyboard mode=ON_DEMAND\n");

    gtk_layer_set_anchor(window, GTK_LAYER_SHELL_EDGE_TOP, TRUE);
    gtk_layer_set_anchor(window, GTK_LAYER_SHELL_EDGE_BOTTOM, TRUE);
    gtk_layer_set_anchor(window, GTK_LAYER_SHELL_EDGE_LEFT, TRUE);
    gtk_layer_set_anchor(window, GTK_LAYER_SHELL_EDGE_RIGHT, TRUE);
    fprintf(stderr, "[layer-shell] Anchored: all 4 edges, exclusive_zone=-1\n");

    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_string(css,
        "window.background { background-color: transparent; }");
    gtk_style_context_add_provider_for_display(
        gdk_display_get_default(),
        GTK_STYLE_PROVIDER(css),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);
    fprintf(stderr, "[layer-shell] CSS background set to transparent\n");
}

/* ─── Input Region (click-through) ─── */

static void
update_input_region(int x, int y, int width, int height)
{
    if (!main_window) return;

    GtkNative *native = GTK_NATIVE(main_window);
    GdkSurface *surface = gtk_native_get_surface(native);
    if (!surface) {
        g_warning("[input-region] No GdkSurface — surface not yet realized?");
        return;
    }

    cairo_region_t *region;

    if (width <= 0 || height <= 0) {
        region = cairo_region_create();
        g_message("[input-region] Set to EMPTY (fully click-through)");
    } else {
        cairo_rectangle_int_t rect = { x, y, width, height };
        region = cairo_region_create_rectangle(&rect);
        g_message("[input-region] Set to {x=%d, y=%d, w=%d, h=%d}",
                  x, y, width, height);
    }

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

    g_message("[script-msg] Received: %.200s%s", json_str,
              strlen(json_str) > 200 ? "..." : "");

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
        if (g_strcmp0(channel, "clui:quit") == 0) {
            fprintf(stderr, "[script-msg] ESC received from JS — quitting\n");
            g_application_quit(G_APPLICATION(
                gtk_window_get_application(main_window)));
        } else if (g_strcmp0(channel, "clui:hide-window") == 0) {
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

/* ─── WebView Load Handlers ─── */

static void
on_load_changed(WebKitWebView *wv, WebKitLoadEvent event, gpointer data)
{
    (void)data;
    const char *uri = webkit_web_view_get_uri(wv);
    switch (event) {
    case WEBKIT_LOAD_STARTED:
        fprintf(stderr, "[webview] Load STARTED: %s\n", uri ? uri : "(null)");
        break;
    case WEBKIT_LOAD_REDIRECTED:
        fprintf(stderr, "[webview] Load REDIRECTED: %s\n", uri ? uri : "(null)");
        break;
    case WEBKIT_LOAD_COMMITTED:
        fprintf(stderr, "[webview] Load COMMITTED: %s\n", uri ? uri : "(null)");
        break;
    case WEBKIT_LOAD_FINISHED:
        fprintf(stderr, "[webview] Load FINISHED: %s\n", uri ? uri : "(null)");
        fprintf(stderr, "[webview] Widget size: %dx%d\n",
                gtk_widget_get_width(GTK_WIDGET(wv)),
                gtk_widget_get_height(GTK_WIDGET(wv)));
        /* DOM diagnostic (ESC handler is now a UCM user script) */
        webkit_web_view_evaluate_javascript(wv,
            "var diag = '[clui-diag] title=' + document.title"
            " + ' body.children=' + document.body.children.length"
            " + ' #root=' + (document.getElementById('root')"
            "   ? document.getElementById('root').children.length : 'MISSING')"
            " + ' body.bg=' + getComputedStyle(document.body).backgroundColor;"
            "console.log(diag); diag;",
            -1, NULL, NULL, NULL, NULL, NULL);
        break;
    default:
        fprintf(stderr, "[webview] Load event %d: %s\n", event,
                uri ? uri : "(null)");
        break;
    }
}

static gboolean
on_load_failed(WebKitWebView *wv, WebKitLoadEvent event,
               const char *failing_uri, GError *error, gpointer data)
{
    (void)event;
    (void)data;
    fprintf(stderr, "[webview] Load FAILED (event=%d): %s — %s\n", event,
            failing_uri, error ? error->message : "unknown error");

    /* Show an inline error page so the user sees what went wrong */
    char *html = g_strdup_printf(
        "<html><body style='background:#242422;color:#fff;"
        "font:20px monospace;padding:40px'>"
        "<h2>CLUI Shell: Load Failed</h2>"
        "<p>URI: %s</p>"
        "<p>Error: %s</p>"
        "<p style='color:#888;margin-top:2em'>Is Vite running? "
        "Try: npm run linux:dev-renderer</p>"
        "</body></html>",
        failing_uri, error ? error->message : "unknown");
    webkit_web_view_load_html(wv, html, NULL);
    g_free(html);
    return TRUE;
}

/* ─── WebView Setup ─── */

static void
setup_webview(GtkWindow *window, const char *content_url)
{
    fprintf(stderr, "[webview] Setting up WebKitGTK webview\n");

    WebKitUserContentManager *ucm = webkit_user_content_manager_new();

    /* bridge.js is injected via Vite (inline <script> in HTML) rather than
       UCM user scripts, which are unreliable in WebKitGTK 6.0. The UCM is
       still needed for the script message handler below. */

    g_signal_connect(ucm, "script-message-received::clui",
                     G_CALLBACK(on_script_message), NULL);
    webkit_user_content_manager_register_script_message_handler(ucm, "clui", NULL);
    fprintf(stderr, "[webview] Registered 'clui' script message handler\n");

    /* ESC key handler via UCM user script — more reliable than
       injecting via on_load_changed, fires even during page navigation */
    WebKitUserScript *esc_script = webkit_user_script_new(
        "document.addEventListener('keydown', function(e) {"
        "  if (e.key === 'Escape') {"
        "    window.webkit.messageHandlers.clui.postMessage("
        "      {channel: 'clui:quit', args: []});"
        "  }"
        "});",
        WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
        WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_END,
        NULL, NULL);
    webkit_user_content_manager_add_script(ucm, esc_script);
    webkit_user_script_unref(esc_script);
    fprintf(stderr, "[webview] ESC user script injected\n");

    webview = WEBKIT_WEB_VIEW(
        g_object_new(WEBKIT_TYPE_WEB_VIEW,
                     "user-content-manager", ucm,
                     NULL));

    g_signal_connect(webview, "load-changed",
                     G_CALLBACK(on_load_changed), NULL);
    g_signal_connect(webview, "load-failed",
                     G_CALLBACK(on_load_failed), NULL);

    /* Webview fills the full-screen window */
    gtk_widget_set_hexpand(GTK_WIDGET(webview), TRUE);
    gtk_widget_set_vexpand(GTK_WIDGET(webview), TRUE);

    gtk_window_set_child(window, GTK_WIDGET(webview));

    /* Transparent webview background — desktop shows through, React UI floats */
    GdkRGBA transparent_bg = { 0.0, 0.0, 0.0, 0.0 };
    webkit_web_view_set_background_color(webview, &transparent_bg);
    fprintf(stderr, "[webview] Background set to transparent\n");

    /* Always enable JS; dev extras only in debug mode */
    WebKitSettings *settings = webkit_web_view_get_settings(webview);
    webkit_settings_set_enable_javascript(settings, TRUE);

    gboolean dev_extras = g_getenv("CLUI_DEBUG") != NULL;
    webkit_settings_set_enable_developer_extras(settings, dev_extras);
    fprintf(stderr, "[webview] JS=enabled, dev_extras=%s\n",
            dev_extras ? "yes" : "no");

    fprintf(stderr, "[webview] WebKitGTK %d.%d.%d\n",
            webkit_get_major_version(),
            webkit_get_minor_version(),
            webkit_get_micro_version());

    fprintf(stderr, "[webview] Loading URI: %s\n", content_url);
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
        g_message("[ipc] Closing previous backend connection (fd=%d)",
                  backend_conn_fd);
        close(backend_conn_fd);
    }
    backend_conn_fd = client_fd;
    ipc_read_len = 0;
    g_message("[ipc] Backend connected (fd=%d)", client_fd);

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

    /* Messages without "cmd" may have "type" (backend protocol) */
    if (!cmd) {
        const char *type = json_object_get_string_member(obj, "type");
        if (type && (g_strcmp0(type, "resolve") == 0 ||
                     g_strcmp0(type, "reject") == 0 ||
                     g_strcmp0(type, "broadcast") == 0)) {
            /* Forward the entire JSON message to the webview for bridge.js
               to dispatch via __cluiDispatch / __cluiResolve / __cluiReject */
            char *escaped = g_strescape(msg, "");
            char *js = g_strdup_printf(
                "window.__cluiHandleBackendMessage && "
                "window.__cluiHandleBackendMessage(\"%s\")", escaped);
            if (webview) {
                webkit_web_view_evaluate_javascript(webview,
                    js, -1, NULL, NULL, NULL, NULL, NULL);
            }
            g_free(js);
            g_free(escaped);
        }
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

/* ─── Keyboard Handler ─── */

static gboolean
on_key_pressed(GtkEventControllerKey *ctrl, guint keyval,
               guint keycode, GdkModifierType state, gpointer data)
{
    (void)ctrl; (void)keycode; (void)state; (void)data;
    fprintf(stderr, "[key] Pressed: keyval=0x%x keycode=%u state=0x%x\n",
            keyval, keycode, state);
    if (keyval == GDK_KEY_Escape) {
        fprintf(stderr, "[key] ESC pressed — quitting\n");
        g_application_quit(G_APPLICATION(
            gtk_window_get_application(main_window)));
        return TRUE;
    }
    return FALSE;
}

/* ─── Application Activate ─── */

static void
on_activate(GtkApplication *app, gpointer user_data)
{
    (void)user_data;

    fprintf(stderr, "[activate] Creating application window\n");

    GdkDisplay *display = gdk_display_get_default();
    fprintf(stderr, "[activate] Display: %s\n", gdk_display_get_name(display));

    GListModel *monitors = gdk_display_get_monitors(display);
    guint n_monitors = g_list_model_get_n_items(monitors);
    for (guint i = 0; i < n_monitors; i++) {
        GdkMonitor *mon = GDK_MONITOR(g_list_model_get_item(monitors, i));
        GdkRectangle geom;
        gdk_monitor_get_geometry(mon, &geom);
        fprintf(stderr, "[activate] Monitor %u: %dx%d+%d+%d scale=%.1f\n",
                i, geom.width, geom.height, geom.x, geom.y,
                gdk_monitor_get_scale_factor(mon) * 1.0);
        g_object_unref(mon);
    }

    const char *gsk = g_getenv("GSK_RENDERER");
    const char *wdcm = g_getenv("WEBKIT_DISABLE_COMPOSITING_MODE");
    fprintf(stderr, "[activate] Env: GSK_RENDERER=%s, WEBKIT_DISABLE_COMPOSITING_MODE=%s\n",
            gsk ? gsk : "(unset)", wdcm ? wdcm : "(unset)");

    main_window = GTK_WINDOW(gtk_application_window_new(app));
    gtk_window_set_title(main_window, "CLUI CC");

    setup_layer_shell(main_window);

    const char *content_url = g_getenv(CONTENT_URL_ENV);
    if (!content_url) content_url = CONTENT_URL_DEFAULT;

    setup_webview(main_window, content_url);

    /* ESC key closes the overlay */
    GtkEventController *key_ctrl = gtk_event_controller_key_new();
    g_signal_connect(key_ctrl, "key-pressed",
                     G_CALLBACK(on_key_pressed), NULL);
    gtk_widget_add_controller(GTK_WIDGET(main_window), key_ctrl);
    fprintf(stderr, "[activate] ESC key handler attached\n");

    gtk_widget_set_visible(GTK_WIDGET(main_window), TRUE);
    fprintf(stderr, "[activate] Window set visible=TRUE, size=%dx%d\n",
            gtk_widget_get_width(GTK_WIDGET(main_window)),
            gtk_widget_get_height(GTK_WIDGET(main_window)));

    const char *socket_path = g_getenv(SOCKET_ENV);
    if (!socket_path) socket_path = SOCKET_DEFAULT;
    setup_ipc_socket(socket_path);

    fprintf(stderr, "[activate] Shell ready — waiting for page load\n");
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
