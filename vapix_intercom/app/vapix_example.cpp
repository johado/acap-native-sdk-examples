#include <curl/curl.h>
#include <gio/gio.h>
#include <jansson.h>
#include <syslog.h>
#include <nlohmann/json.hpp>
using namespace std;

__attribute__((noreturn)) __attribute__((format(printf, 1, 2))) static void
panic(const char* format, ...) {
    va_list arg;
    va_start(arg, format);
    vsyslog(LOG_ERR, format, arg);
    va_end(arg);
    exit(1);
}

static char* parse_credentials(GVariant* result) {
    char* credentials_string = NULL;
    char* id                 = NULL;
    char* password           = NULL;

    g_variant_get(result, "(&s)", &credentials_string);
    if (sscanf(credentials_string, "%m[^:]:%ms", &id, &password) != 2)
        panic("Error parsing credential string '%s'", credentials_string);
    char* credentials = g_strdup_printf("%s:%s", id, password);

    free(id);
    free(password);
    return credentials;
}

static char* retrieve_vapix_credentials(const char* username) {
    GError* error               = NULL;
    GDBusConnection* connection = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &error);
    if (!connection)
        panic("Error connecting to D-Bus: %s", error->message);

    const char* bus_name       = "com.axis.HTTPConf1";
    const char* object_path    = "/com/axis/HTTPConf1/VAPIXServiceAccounts1";
    const char* interface_name = "com.axis.HTTPConf1.VAPIXServiceAccounts1";
    const char* method_name    = "GetCredentials";

    GVariant* result = g_dbus_connection_call_sync(connection,
                                                   bus_name,
                                                   object_path,
                                                   interface_name,
                                                   method_name,
                                                   g_variant_new("(s)", username),
                                                   NULL,
                                                   G_DBUS_CALL_FLAGS_NONE,
                                                   -1,
                                                   NULL,
                                                   &error);
    if (!result)
        panic("Error invoking D-Bus method: %s", error->message);

    char* credentials = parse_credentials(result);

    g_variant_unref(result);
    g_object_unref(connection);
    return credentials;
}

static size_t append_to_gstring_callback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    size_t processed_bytes = size * nmemb;
    g_string_append_len((GString*)userdata, ptr, processed_bytes);
    return processed_bytes;
}

static char*
vapix_post(CURL* handle, const char* credentials, const char* endpoint, const char* request) {
    GString* response = g_string_new(NULL);
    const char* path  = "/axis-cgi/";
    if (endpoint[0] == '/') {
        path = "";
    }
    char* url = g_strdup_printf("http://127.0.0.12%s%s", path, endpoint);

    curl_easy_setopt(handle, CURLOPT_URL, url);
    curl_easy_setopt(handle, CURLOPT_USERPWD, credentials);
    curl_easy_setopt(handle, CURLOPT_HTTPAUTH, CURLAUTH_BASIC);
    curl_easy_setopt(handle, CURLOPT_POSTFIELDS, request);
    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, append_to_gstring_callback);
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, response);

    CURLcode res = curl_easy_perform(handle);
    if (res != CURLE_OK)
        panic("curl_easy_perform error %d: '%s'", res, curl_easy_strerror(res));

    long response_code;
    curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &response_code);
    if (response_code != 200)
        panic("Got response code %ld from request to %s with response '%s'",
              response_code,
              request,
              response->str);

    free(url);
    return g_string_free(response, FALSE);
}

static json_t*
vapix_post_json(CURL* handle, const char* credentials, const char* endpoint, const char* request) {
    char* text_response = vapix_post(handle, credentials, endpoint, request);
    json_error_t parse_error;
    json_t* json_response = json_loads(text_response, 0, &parse_error);
    if (!json_response)
        panic("Invalid JSON response: %s", parse_error.text);

    const json_t* request_error = json_object_get(json_response, "error");
    if (request_error)
        panic("Failed to perform request: %s",
              json_string_value(json_object_get(request_error, "message")));

    free(text_response);
    return json_response;
}

static nlohmann::json
vapix_post_json2(CURL* handle, const char* credentials, const char* endpoint, const nlohmann::json &jsonreq) {
    std::string request = jsonreq.dump(2);
    char* text_response = vapix_post(handle, credentials, endpoint, request.c_str());
    nlohmann::json response = nlohmann::json::parse(text_response);
    free(text_response);
    return response;
}


static json_t* get_all_properties(CURL* handle, const char* credentials) {
    const char* endpoint = "basicdeviceinfo.cgi";
    const char* request =
        "{"
        "  \"apiVersion\": \"1.3\","
        "  \"method\": \"getAllProperties\""
        "}";
    return vapix_post_json(handle, credentials, endpoint, request);
}

static const char* read_property(const json_t* all_props, const char* prop_name) {
    const json_t* data       = json_object_get(all_props, "data");
    const json_t* prop_list  = json_object_get(data, "propertyList");
    const json_t* prop_value = json_object_get(prop_list, prop_name);
    return json_string_value(prop_value);
}

static void show_widget(CURL* handle,
                        const char* credentials,
                        const char* widgetId,
                        const char* widget, /* Must be escaped JSON */
                        long durationMillisec) {
    const char* endpoint = "/vapix/intercom/axdisplay:ShowWidget";
    char* request        = NULL;

    if (widgetId != NULL) {
        request = g_strdup_printf("{ \"widgetId\": \"%s\", \"durationMillisec\": %lu}",
                                  widgetId,
                                  durationMillisec);
    } else if (widget != NULL) {
        request = g_strdup_printf("{ \"widget\": %s, \"durationMillisec\": %lu}",
                                  widget,
                                  durationMillisec);
    }
    if (request) {
        json_t* response = vapix_post_json(handle, credentials, endpoint, request);

        json_decref(response);
        g_free(request);
    }
}

void show_widget2(CURL* handle,
                  const char* credentials,
                  const char* widgetId,
                  const nlohmann::json &widget,
                  long durationMillisec) {
    const char* endpoint = "/vapix/intercom/axdisplay:ShowWidget";
    nlohmann::json request;

    if (widgetId != NULL) {
        request["widgetId"] = widgetId;
    } else if (!widget.empty()) {
        request["widget"] = widget;
    }
    request["durationMillisec"] = durationMillisec;

    nlohmann::json response = vapix_post_json2(handle, credentials, endpoint, request);
}


static void set_widget(CURL* handle, const char* credentials, const char* widget) {
    const char* endpoint = "/vapix/intercom/axdisplay:SetWidgets";
    char* request        = NULL;

    if (widget != NULL) {
        request = g_strdup_printf("{ \"widgets\": [%s]}", widget);
    }
    if (request) {
        json_t* response = vapix_post_json(handle, credentials, endpoint, request);

        json_decref(response);
        g_free(request);
    }
}


void set_widget2(CURL* handle, const char* credentials, const nlohmann::json &widget) {
    const char* endpoint = "/vapix/intercom/axdisplay:SetWidgets";
    nlohmann::json request;

    //request["widgets"] = nlohmann::json::array()
    request["widgets"][0] = widget;
    nlohmann::json response = vapix_post_json2(handle, credentials, endpoint, request);
}

static void test_inline_widgets(CURL* handle, const char* credentials) {
    const char* widgetId           = NULL;
    char* widget                   = NULL;
    unsigned long durationMillisec = 3000;
    for (int i = 0; i < 10; i++) {
        widget = g_strdup_printf(
            "{ \"type\": \"Page\", \"children\": [\n"
            "{\"type\": \"Label\", \"label\": \"Testing %i\"}\n"
            "]}\n",
            i);

        syslog(LOG_INFO, "Widget: %s", widget);
        show_widget(handle, credentials, widgetId, widget, durationMillisec);
        sleep(5);
        g_free(widget);
    }
}

void test_inline_widgets2(CURL* handle, const char* credentials) {
    const char* widgetId           = NULL;
    unsigned long durationMillisec = 3000;
    nlohmann::json widget;
    for (int i = 0; i < 10; i++) {
        nlohmann::json child;
        char label[64];

        sprintf(label, "Testing %i", i);
        child["type"] = "Label";
        child["label"] = label;
        widget["type"] = "Page";
        widget["children"] = nlohmann::json::array();
        widget["children"][0] = child;
        //std::string widgetstr = widget.dump();
        //syslog(LOG_INFO, "Widget: %s", widget.dump().c_str());
        show_widget2(handle, credentials, widgetId, widget, durationMillisec);
        sleep(5);
    }
}


static void test_ref_widgets(CURL* handle, const char* credentials) {
    const char* widgetId           = NULL;
    char* widget                   = NULL;
    unsigned long durationMillisec = 3000;
    int i                          = 0;

    syslog(LOG_INFO, "test_ref_widgets");

    /* Create a dynamiclabel widget to be referenced */
    widget = g_strdup_printf(
        "{ \"id\": \"acap.dynamiclabel\", \"type\": \"Label\", \"label\": \"Testing %i\"}\n",
        i);
    set_widget(handle, credentials, widget);
    g_free(widget);

    /* Create the page that uses the dynamiclabel */
    widget = g_strdup_printf(
        "{ \"id\": \"acap.test1\", \"type\": \"Page\", \"children\": [\n"
        "{\"type\": \"Label\", \"label\": \"Testing dynamic\"},\n"
        "{\"type\": \"Reference\", \"widgetReference\": \"acap.dynamiclabel\"}\n"
        "]}\n");
    set_widget(handle, credentials, widget);
    g_free(widget);

    widget           = NULL;
    widgetId         = "acap.test1";
    durationMillisec = 10000;
    show_widget(handle, credentials, widgetId, widget, durationMillisec);

    for (int i = 0; i < 10; i++) {
        /* Update the dynamiclabel */
        widget = g_strdup_printf(
            "{ \"id\": \"acap.dynamiclabel\", \"type\": \"Label\", \"label\": \"Testing %i\"}\n",
            i);
        syslog(LOG_INFO, "set_widget %s", widget);
        set_widget(handle, credentials, widget);
        g_free(widget);
        widget = NULL;

        // show_widget(handle, credentials, widgetId, widget, durationMillisec);
        sleep(1);
    }
}


void test_ref_widgets2(CURL* handle, const char* credentials) {
    const char* widgetId           = NULL;
    unsigned long durationMillisec = 3000;
    nlohmann::json widget;

    syslog(LOG_INFO, "test_ref_widgets2");

    /* Create a dynamiclabel widget to be referenced */
    widget["id"] = "acap.dynamiclabel2";
    widget["type"] = "Label";
    widget["label"] = "Testing2 0";
    set_widget2(handle, credentials, widget);

    /* Create the page that uses the dynamiclabel */
    widget["id"] = "acap.test2";
    widget["type"] = "Page";
    widget["name"] = "ACAP testpage2";
    widget["children"] = nlohmann::json::array( {
        {
            {"type", "Label"},
            {"label", "Testing dynamic"}
        },
        {
            {"type", "Reference"},
            {"widgetReference", "acap.dynamiclabel2"}
        }
    });
    set_widget2(handle, credentials, widget);

    widget           = nlohmann::json::object();
    widgetId         = "acap.test2";
    durationMillisec = 10000;
    show_widget2(handle, credentials, widgetId, widget, durationMillisec);

    for (int i = 0; i < 10; i++) {
        /* Update the dynamiclabel */
        char label[64];

        sprintf(label, "Testing2 %i", i);

        widget["id"] = "acap.dynamiclabel2";
        widget["type"] = "Label";
        widget["label"] = label;

        syslog(LOG_INFO, "set_widget %s", widget.dump().c_str());
        set_widget2(handle, credentials, widget);

        // show_widget(handle, credentials, widgetId, widget, durationMillisec);
        sleep(1);
    }
}


static void test_widgets(CURL* handle, const char* credentials) {
    if (0) {
        test_inline_widgets(handle, credentials);
    }

    test_ref_widgets(handle, credentials);

    test_ref_widgets2(handle, credentials);
}

int main(void) {
    openlog(NULL, LOG_PID, LOG_USER);

    syslog(LOG_INFO, "Curl version %s", curl_version_info(CURLVERSION_NOW)->version);
    //syslog(LOG_INFO, "Jansson version %s", JANSSON_VERSION);

    curl_global_init(CURL_GLOBAL_DEFAULT);
    CURL* handle      = curl_easy_init();
    char* credentials = retrieve_vapix_credentials("example-vapix-user");

    json_t* all_props = get_all_properties(handle, credentials);

    syslog(LOG_INFO, "ProdShortName: %s", read_property(all_props, "ProdShortName"));
    syslog(LOG_INFO, "Soc: %s", read_property(all_props, "Soc"));
    syslog(LOG_INFO, "SocSerialNumber: %s", read_property(all_props, "SocSerialNumber"));

    json_decref(all_props);
    syslog(LOG_INFO, "test_widgets");

    test_widgets(handle, credentials);

    free(credentials);
    curl_easy_cleanup(handle);
    curl_global_cleanup();
}
