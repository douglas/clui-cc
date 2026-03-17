/*
 * webview-test.c — Minimal WebKitGTK rendering test for CLUI-CC debugging
 *
 * Usage:
 *   ./builddir/webview-test                          # Step 1: inline HTML, plain GTK4
 *   ./builddir/webview-test --layer-shell             # Step 2: + layer-shell overlay
 *   ./builddir/webview-test --url http://127.0.0.1:5173  # Step 3: load from URL
 *   ./builddir/webview-test --layer-shell --transparent  # Step 2b: transparent overlay
 *   ./builddir/webview-test --timeout 5               # Auto-quit after 5 seconds
 */

#include <gtk/gtk.h>
#include <gtk4-layer-shell.h>
#include <webkit/webkit.h>
#include <string.h>
#include <stdlib.h>

static const char *INLINE_HTML =
    "<html><body style='margin:0;background:red;display:flex;"
    "align-items:center;justify-content:center;height:100vh'>"
    "<h1 style='color:white;font-size:72px;font-family:sans-serif'>"
    "HELLO WEBKIT</h1></body></html>";

static const char *HELLO_HTML =
    "<html><body style='margin:0;background:#1a1a2e;display:flex;"
    "align-items:center;justify-content:center;height:100vh;"
    "flex-direction:column'>"
    "<h1 style='color:#e94560;font-size:72px;font-family:sans-serif;"
    "margin:0'>Hello World</h1>"
    "<p style='color:#aaa;font-size:24px;font-family:sans-serif'>"
    "Press ESC to close</p></body></html>";

static gboolean use_layer_shell = FALSE;
static gboolean use_transparent = FALSE;
static gboolean use_hello = FALSE;
static const char *load_url = NULL;
static int timeout_seconds = 0;
static GtkApplication *global_app = NULL;

static gboolean
on_timeout(gpointer data)
{
    (void)data;
    g_print("[test] Timeout reached (%d seconds) — quitting\n", timeout_seconds);
    g_application_quit(G_APPLICATION(global_app));
    return G_SOURCE_REMOVE;
}

static void
on_script_message(WebKitUserContentManager *manager,
                  JSCValue                 *js_result,
                  gpointer                  user_data)
{
    (void)manager;
    (void)user_data;

    char *str = jsc_value_to_string(js_result);
    if (!str) return;

    if (strcmp(str, "escape") == 0) {
        g_print("[test] ESC received from webview JS — quitting\n");
        g_application_quit(G_APPLICATION(global_app));
    }

    g_free(str);
}

/* Stub handler for the 'clui' UCM channel — bridge.js sends messages here.
   We auto-resolve invoke promises with sensible defaults so the React app
   can render without a real backend. */
static void
on_clui_message(WebKitUserContentManager *manager,
                JSCValue                 *js_result,
                gpointer                  user_data)
{
    (void)manager;
    (void)user_data;

    char *json_str = jsc_value_to_json(js_result, 0);
    if (!json_str) return;

    g_print("[clui-stub] %s\n", json_str);
    g_free(json_str);
}

static void
on_load_changed(WebKitWebView *wv, WebKitLoadEvent event, gpointer data)
{
    (void)data;
    const char *uri = webkit_web_view_get_uri(wv);
    const char *name = "?";
    switch (event) {
    case WEBKIT_LOAD_STARTED:   name = "STARTED";   break;
    case WEBKIT_LOAD_REDIRECTED: name = "REDIRECTED"; break;
    case WEBKIT_LOAD_COMMITTED: name = "COMMITTED";  break;
    case WEBKIT_LOAD_FINISHED:  name = "FINISHED";   break;
    default: break;
    }
    g_print("[webview] Load %s: %s\n", name, uri ? uri : "(null)");
    if (event == WEBKIT_LOAD_FINISHED) {
        g_print("[webview] Widget size: %dx%d\n",
                gtk_widget_get_width(GTK_WIDGET(wv)),
                gtk_widget_get_height(GTK_WIDGET(wv)));

        /* DOM diagnostic — delayed to let React render */
        webkit_web_view_evaluate_javascript(wv,
            "setTimeout(function() {"
            "  var root = document.getElementById('root');"
            "  var d = '[diag] title=' + document.title"
            "    + ' body.children=' + document.body.children.length"
            "    + ' #root.children=' + (root ? root.children.length : 'MISSING')"
            "    + ' #root.html=' + (root ? root.innerHTML.substring(0,300) : 'N/A')"
            "    + ' clui=' + (typeof window.clui)"
            "    + ' stubs=' + (typeof window.__cluiStubs);"
            "  console.log(d);"
            "}, 2000);",
            -1, NULL, NULL, NULL, NULL, NULL);
    }
}

static gboolean
on_load_failed(WebKitWebView *wv, WebKitLoadEvent event,
               const char *failing_uri, GError *error, gpointer data)
{
    (void)wv; (void)event; (void)data;
    g_printerr("[webview] Load FAILED: %s — %s\n",
               failing_uri, error ? error->message : "unknown");
    return FALSE;
}

static gboolean
on_key_pressed(GtkEventControllerKey *ctrl, guint keyval,
               guint keycode, GdkModifierType state, gpointer data)
{
    (void)ctrl; (void)keycode; (void)state;
    if (keyval == GDK_KEY_Escape) {
        g_print("[test] ESC from GTK key controller — quitting\n");
        g_application_quit(G_APPLICATION(data));
        return TRUE;
    }
    return FALSE;
}

static void
on_activate(GtkApplication *app, gpointer user_data)
{
    (void)user_data;

    GtkWindow *window = GTK_WINDOW(gtk_application_window_new(app));
    gtk_window_set_title(window, "WebView Test");

    if (!use_transparent)
        gtk_window_set_default_size(window, 800, 400);

    if (use_layer_shell) {
        g_print("[test] Using layer-shell overlay (keyboard=ON_DEMAND)\n");
        gtk_layer_init_for_window(window);
        gtk_layer_set_layer(window, GTK_LAYER_SHELL_LAYER_OVERLAY);
        gtk_layer_set_namespace(window, "webview-test");
        gtk_layer_set_keyboard_mode(window,
            GTK_LAYER_SHELL_KEYBOARD_MODE_ON_DEMAND);

        if (use_transparent) {
            /* Full-screen transparent overlay — anchor all edges */
            gtk_layer_set_exclusive_zone(window, -1);
            gtk_layer_set_anchor(window, GTK_LAYER_SHELL_EDGE_TOP, TRUE);
            gtk_layer_set_anchor(window, GTK_LAYER_SHELL_EDGE_BOTTOM, TRUE);
            gtk_layer_set_anchor(window, GTK_LAYER_SHELL_EDGE_LEFT, TRUE);
            gtk_layer_set_anchor(window, GTK_LAYER_SHELL_EDGE_RIGHT, TRUE);
            g_print("[test] Transparent: anchored all edges, exclusive_zone=-1\n");
        } else {
            gtk_layer_set_exclusive_zone(window, 0);
            gtk_layer_set_anchor(window, GTK_LAYER_SHELL_EDGE_BOTTOM, TRUE);
            gtk_layer_set_margin(window, GTK_LAYER_SHELL_EDGE_BOTTOM, 24);
        }
    }

    if (use_transparent) {
        GtkCssProvider *css = gtk_css_provider_new();
        gtk_css_provider_load_from_string(css,
            "window.background { background-color: transparent; }");
        gtk_style_context_add_provider_for_display(
            gdk_display_get_default(),
            GTK_STYLE_PROVIDER(css),
            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
        g_object_unref(css);
        g_print("[test] GTK CSS: transparent window background\n");
    }

    /* ESC to quit — kept as fallback for non-webview-focused cases */
    GtkEventController *key_ctrl = gtk_event_controller_key_new();
    g_signal_connect(key_ctrl, "key-pressed",
                     G_CALLBACK(on_key_pressed), app);
    gtk_widget_add_controller(GTK_WIDGET(window), key_ctrl);

    /* Set up UCM with script message handlers */
    WebKitUserContentManager *ucm = webkit_user_content_manager_new();

    /* ESC handler channel */
    g_signal_connect(ucm, "script-message-received::webviewTest",
                     G_CALLBACK(on_script_message), NULL);
    webkit_user_content_manager_register_script_message_handler(
        ucm, "webviewTest", NULL);

    /* Stub 'clui' channel — bridge.js sends invoke/send messages here */
    g_signal_connect(ucm, "script-message-received::clui",
                     G_CALLBACK(on_clui_message), NULL);
    webkit_user_content_manager_register_script_message_handler(
        ucm, "clui", NULL);

    /* Stub window.clui before bridge.js runs — bridge.js checks
       if (window.clui) and skips if it already exists. This lets
       the React app render without a real backend. */
    WebKitUserScript *stub_script = webkit_user_script_new(
        "(function() {"
        "  console.log('[clui-stub] Defining stub window.clui');"
        "  var noop = function() {};"
        "  var resolved = function(v) { return Promise.resolve(v); };"
        "  var listeners = {};"
        "  function addListener(ch, cb) {"
        "    if (!listeners[ch]) listeners[ch] = [];"
        "    listeners[ch].push(cb);"
        "    return function() {"
        "      listeners[ch] = listeners[ch].filter(function(f){return f!==cb;});"
        "    };"
        "  }"
        "  var api = {"
        "    start: function() { return resolved({ version:'0.1.0-test', auth:{email:'test@test.com',subscriptionType:'pro'}, projectPath:'/tmp', homePath:'/tmp' }); },"
        "    createTab: function() { return resolved({ tabId:'test-tab-1' }); },"
        "    getTheme: function() { return resolved({ isDark:true }); },"
        "    status: function() { return resolved({ running:false }); },"
        "    tabHealth: function() { return resolved({}); },"
        "    isVisible: function() { return resolved(true); },"
        "    getDiagnostics: function() { return resolved({}); },"
        "    listSessions: function() { return resolved([]); },"
        "    fetchMarketplace: function() { return resolved([]); },"
        "    listInstalledPlugins: function() { return resolved([]); },"
        "    prompt: function() { return resolved({}); },"
        "    cancel: function() { return resolved(); },"
        "    stopTab: function() { return resolved(); },"
        "    retry: function() { return resolved({}); },"
        "    closeTab: function() { return resolved(); },"
        "    selectDirectory: function() { return resolved(null); },"
        "    openExternal: function() { return resolved(); },"
        "    openInTerminal: function() { return resolved(); },"
        "    attachFiles: function() { return resolved([]); },"
        "    takeScreenshot: function() { return resolved(null); },"
        "    pasteImage: function() { return resolved(); },"
        "    transcribeAudio: function() { return resolved(null); },"
        "    respondPermission: function() { return resolved(); },"
        "    initSession: noop,"
        "    resetTabSession: noop,"
        "    installPlugin: function() { return resolved(); },"
        "    uninstallPlugin: function() { return resolved(); },"
        "    setPermissionMode: noop,"
        "    loadSession: function() { return resolved({}); },"
        "    resizeHeight: noop,"
        "    setWindowWidth: noop,"
        "    animateHeight: function() { return resolved(); },"
        "    hideWindow: noop,"
        "    setIgnoreMouseEvents: noop,"
        "    onEvent: function(cb) { return addListener('event',cb); },"
        "    onTabStatusChange: function(cb) { return addListener('tab-status',cb); },"
        "    onError: function(cb) { return addListener('error',cb); },"
        "    onSkillStatus: function(cb) { return addListener('skill',cb); },"
        "    onThemeChange: function(cb) { return addListener('theme',cb); },"
        "    onWindowShown: function(cb) { return addListener('shown',cb); }"
        "  };"
        "  Object.defineProperty(window, 'clui', {"
        "    value: Object.freeze(api),"
        "    writable: false,"
        "    configurable: false"
        "  });"
        "  window.__cluiSetInputRegion = noop;"
        "  console.log('[clui-stub] Stub ready');"
        "})();"
        "\n"
        "document.addEventListener('keydown', function(e) {"
        "  if (e.key === 'Escape') {"
        "    window.webkit.messageHandlers.webviewTest.postMessage('escape');"
        "  }"
        "});",
        WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
        WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START,
        NULL, NULL);
    webkit_user_content_manager_add_script(ucm, stub_script);
    webkit_user_script_unref(stub_script);

    WebKitWebView *wv = WEBKIT_WEB_VIEW(
        g_object_new(WEBKIT_TYPE_WEB_VIEW,
                     "user-content-manager", ucm,
                     NULL));

    g_signal_connect(wv, "load-changed",
                     G_CALLBACK(on_load_changed), NULL);
    g_signal_connect(wv, "load-failed",
                     G_CALLBACK(on_load_failed), NULL);
    /* Dump JS console.log to stdout for debugging */
    WebKitSettings *settings = webkit_web_view_get_settings(wv);
    webkit_settings_set_enable_developer_extras(settings, TRUE);
    webkit_settings_set_enable_write_console_messages_to_stdout(settings, TRUE);

    /* WebView background: transparent or opaque dark */
    if (use_transparent) {
        GdkRGBA transparent_bg = { 0.0, 0.0, 0.0, 0.0 };
        webkit_web_view_set_background_color(wv, &transparent_bg);
        g_print("[test] WebView background: transparent\n");
    } else {
        GdkRGBA dark_bg = { 0.141, 0.141, 0.133, 1.0 }; /* #242422 */
        webkit_web_view_set_background_color(wv, &dark_bg);
    }

    gtk_widget_set_hexpand(GTK_WIDGET(wv), TRUE);
    gtk_widget_set_vexpand(GTK_WIDGET(wv), TRUE);
    gtk_window_set_child(window, GTK_WIDGET(wv));

    g_print("[test] WebKitGTK %d.%d.%d\n",
            webkit_get_major_version(),
            webkit_get_minor_version(),
            webkit_get_micro_version());

    if (load_url) {
        g_print("[test] Loading URL: %s\n", load_url);
        webkit_web_view_load_uri(wv, load_url);
    } else if (use_hello) {
        g_print("[test] Loading hello world HTML\n");
        webkit_web_view_load_html(wv, HELLO_HTML, NULL);
    } else {
        g_print("[test] Loading inline HTML\n");
        webkit_web_view_load_html(wv, INLINE_HTML, NULL);
    }

    if (timeout_seconds > 0) {
        g_print("[test] Auto-quit in %d seconds\n", timeout_seconds);
        g_timeout_add_seconds(timeout_seconds, on_timeout, NULL);
    }

    gtk_widget_set_visible(GTK_WIDGET(window), TRUE);
    g_print("[test] Window visible — press ESC to quit\n");
}

int
main(int argc, char *argv[])
{
    /* Parse our flags before GTK gets argv */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--layer-shell") == 0) {
            use_layer_shell = TRUE;
        } else if (strcmp(argv[i], "--url") == 0 && i + 1 < argc) {
            load_url = argv[++i];
        } else if (strcmp(argv[i], "--hello") == 0) {
            use_hello = TRUE;
        } else if (strcmp(argv[i], "--transparent") == 0) {
            use_transparent = TRUE;
        } else if (strcmp(argv[i], "--timeout") == 0 && i + 1 < argc) {
            timeout_seconds = atoi(argv[++i]);
        }
    }

    global_app = gtk_application_new("com.clui.webview-test",
                                      G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(global_app, "activate", G_CALLBACK(on_activate), NULL);

    int status = g_application_run(G_APPLICATION(global_app), 1, argv);
    g_object_unref(global_app);
    return status;
}
