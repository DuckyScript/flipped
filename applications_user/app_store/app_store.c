#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/submenu.h>
#include <gui/modules/dialog_ex.h>
#include <gui/modules/popup.h>
#include <input/input.h>
#include <storage/storage.h>
#include <notification/notification.h>
#include <notification/notification_messages.h>

#define TAG "AppStore"

#define APP_INSTALL_PATH EXT_PATH("app_install")
#define APP_INSTALL_MANIFEST APP_INSTALL_PATH "/manifest.txt"
#define APPS_INSTALL_DIR EXT_PATH("apps")
#define MAX_APPS 32
#define MAX_NAME_LEN 64
#define MAX_FILE_LEN 128
#define MAX_CAT_LEN 32
#define MAX_DESC_LEN 128
#define MANIFEST_LINE_MAX 384

typedef enum {
    AppStoreViewMenu,
    AppStoreViewInfo,
    AppStoreViewResult,
} AppStoreView;

typedef struct {
    char name[MAX_NAME_LEN];
    char filename[MAX_FILE_LEN];
    char category[MAX_CAT_LEN];
    char description[MAX_DESC_LEN];
} AppEntry;

typedef struct {
    Gui* gui;
    Storage* storage;
    NotificationApp* notifications;
    ViewDispatcher* view_dispatcher;
    Submenu* submenu;
    DialogEx* dialog;
    Popup* popup;

    AppEntry apps[MAX_APPS];
    uint32_t app_count;
    uint32_t selected_index;
} AppStoreApp;

static void app_store_trim(char* str) {
    // Trim trailing whitespace and newlines
    size_t len = strlen(str);
    while(len > 0 && (str[len - 1] == '\n' || str[len - 1] == '\r' || str[len - 1] == ' ')) {
        str[--len] = '\0';
    }
}

static bool app_store_parse_manifest(AppStoreApp* app) {
    app->app_count = 0;

    File* file = storage_file_alloc(app->storage);
    if(!storage_file_open(file, APP_INSTALL_MANIFEST, FSAM_READ, FSOM_OPEN_EXISTING)) {
        FURI_LOG_E(TAG, "Failed to open manifest: %s", APP_INSTALL_MANIFEST);
        storage_file_free(file);
        return false;
    }

    char line[MANIFEST_LINE_MAX];
    size_t line_pos = 0;
    char ch;

    while(app->app_count < MAX_APPS) {
        size_t bytes_read = storage_file_read(file, &ch, 1);
        if(bytes_read == 0) {
            // EOF - process remaining line
            if(line_pos > 0) {
                line[line_pos] = '\0';
            } else {
                break;
            }
        } else if(ch == '\n') {
            line[line_pos] = '\0';
            line_pos = 0;
        } else {
            if(line_pos < MANIFEST_LINE_MAX - 1) {
                line[line_pos++] = ch;
            }
            if(bytes_read > 0) continue;
        }

        // Skip empty lines and comments
        if(line[0] == '#' || line[0] == '\0') {
            if(bytes_read == 0) break;
            continue;
        }

        // Parse: name|filename|category|description
        AppEntry* entry = &app->apps[app->app_count];
        char* ptr = line;
        char* field_start = ptr;
        int field = 0;

        while(*ptr && field < 4) {
            if(*ptr == '|' || *(ptr + 1) == '\0') {
                if(*(ptr + 1) == '\0' && *ptr != '|') ptr++;
                char saved = *ptr;
                *ptr = '\0';

                switch(field) {
                case 0:
                    strlcpy(entry->name, field_start, MAX_NAME_LEN);
                    app_store_trim(entry->name);
                    break;
                case 1:
                    strlcpy(entry->filename, field_start, MAX_FILE_LEN);
                    app_store_trim(entry->filename);
                    break;
                case 2:
                    strlcpy(entry->category, field_start, MAX_CAT_LEN);
                    app_store_trim(entry->category);
                    break;
                case 3:
                    strlcpy(entry->description, field_start, MAX_DESC_LEN);
                    app_store_trim(entry->description);
                    break;
                }

                *ptr = saved;
                field_start = ptr + 1;
                field++;
            }
            ptr++;
        }

        if(field >= 3 && strlen(entry->name) > 0 && strlen(entry->filename) > 0) {
            FURI_LOG_I(TAG, "Found app: %s (%s)", entry->name, entry->filename);
            app->app_count++;
        }

        if(bytes_read == 0) break;
    }

    storage_file_close(file);
    storage_file_free(file);

    FURI_LOG_I(TAG, "Loaded %lu apps from manifest", app->app_count);
    return true;
}

static bool app_store_install_app(AppStoreApp* app, uint32_t index) {
    if(index >= app->app_count) return false;

    AppEntry* entry = &app->apps[index];

    // Build source path
    char src_path[256];
    snprintf(src_path, sizeof(src_path), APP_INSTALL_PATH "/%s", entry->filename);

    // Check source file exists
    FileInfo file_info;
    if(storage_common_stat(app->storage, src_path, &file_info) != FSE_OK) {
        FURI_LOG_E(TAG, "Source file not found: %s", src_path);
        return false;
    }

    // Build destination directory: /ext/apps/<category>/
    char dest_dir[256];
    snprintf(dest_dir, sizeof(dest_dir), APPS_INSTALL_DIR "/%s", entry->category);

    // Create category directory if needed
    storage_simply_mkdir(app->storage, APPS_INSTALL_DIR);
    storage_simply_mkdir(app->storage, dest_dir);

    // Build destination path
    char dest_path[256];
    snprintf(dest_path, sizeof(dest_path), "%s/%s", dest_dir, entry->filename);

    // Remove existing file if present
    storage_simply_remove(app->storage, dest_path);

    // Copy file
    FS_Error result = storage_common_copy(app->storage, src_path, dest_path);
    if(result != FSE_OK) {
        FURI_LOG_E(TAG, "Failed to copy %s -> %s (err %d)", src_path, dest_path, result);
        return false;
    }

    FURI_LOG_I(TAG, "Installed %s to %s", entry->name, dest_path);
    return true;
}

static bool app_store_is_installed(AppStoreApp* app, uint32_t index) {
    if(index >= app->app_count) return false;

    AppEntry* entry = &app->apps[index];
    char dest_path[256];
    snprintf(
        dest_path,
        sizeof(dest_path),
        APPS_INSTALL_DIR "/%s/%s",
        entry->category,
        entry->filename);

    FileInfo file_info;
    return storage_common_stat(app->storage, dest_path, &file_info) == FSE_OK;
}

// --- UI Callbacks ---

static void app_store_menu_callback(void* context, uint32_t index) {
    AppStoreApp* app = context;
    app->selected_index = index;

    AppEntry* entry = &app->apps[index];
    bool installed = app_store_is_installed(app, index);

    dialog_ex_set_header(app->dialog, entry->name, 64, 2, AlignCenter, AlignTop);
    dialog_ex_set_text(app->dialog, entry->description, 64, 18, AlignCenter, AlignTop);

    if(installed) {
        dialog_ex_set_left_button_text(app->dialog, "Back");
        dialog_ex_set_right_button_text(app->dialog, "Reinstall");
        dialog_ex_set_center_button_text(app->dialog, "Remove");
    } else {
        dialog_ex_set_left_button_text(app->dialog, "Back");
        dialog_ex_set_right_button_text(app->dialog, "Install");
        dialog_ex_set_center_button_text(app->dialog, NULL);
    }

    view_dispatcher_switch_to_view(app->view_dispatcher, AppStoreViewInfo);
}

static void app_store_show_result(AppStoreApp* app, const char* message) {
    popup_set_header(app->popup, message, 64, 20, AlignCenter, AlignCenter);
    popup_set_timeout(app->popup, 1500);
    popup_enable_timeout(app->popup);
    view_dispatcher_switch_to_view(app->view_dispatcher, AppStoreViewResult);
}

static void app_store_refresh_menu(AppStoreApp* app) {
    submenu_reset(app->submenu);

    if(app->app_count == 0) {
        submenu_add_item(app->submenu, "[No apps in manifest]", 0, NULL, NULL);
    } else {
        for(uint32_t i = 0; i < app->app_count; i++) {
            // Build label with installed indicator
            static char labels[MAX_APPS][MAX_NAME_LEN + 16];
            bool installed = app_store_is_installed(app, i);
            snprintf(
                labels[i],
                sizeof(labels[i]),
                "%s%s",
                installed ? "[*] " : "    ",
                app->apps[i].name);
            submenu_add_item(app->submenu, labels[i], i, app_store_menu_callback, app);
        }
    }
}

static void app_store_dialog_callback(DialogExResult result, void* context) {
    AppStoreApp* app = context;
    uint32_t index = app->selected_index;

    if(result == DialogExResultLeft) {
        // Back
        view_dispatcher_switch_to_view(app->view_dispatcher, AppStoreViewMenu);
    } else if(result == DialogExResultRight) {
        // Install / Reinstall
        if(app_store_install_app(app, index)) {
            notification_message(app->notifications, &sequence_success);
            app_store_show_result(app, "Installed!");
        } else {
            notification_message(app->notifications, &sequence_error);
            app_store_show_result(app, "Install failed!\n.fap not found");
        }
        app_store_refresh_menu(app);
    } else if(result == DialogExResultCenter) {
        // Remove
        AppEntry* entry = &app->apps[index];
        char dest_path[256];
        snprintf(
            dest_path,
            sizeof(dest_path),
            APPS_INSTALL_DIR "/%s/%s",
            entry->category,
            entry->filename);
        if(storage_simply_remove(app->storage, dest_path)) {
            notification_message(app->notifications, &sequence_success);
            app_store_show_result(app, "Removed!");
        } else {
            notification_message(app->notifications, &sequence_error);
            app_store_show_result(app, "Remove failed!");
        }
        app_store_refresh_menu(app);
    }
}

static void app_store_popup_callback(void* context) {
    AppStoreApp* app = context;
    view_dispatcher_switch_to_view(app->view_dispatcher, AppStoreViewInfo);
}

static uint32_t app_store_exit_callback(void* context) {
    UNUSED(context);
    return VIEW_NONE;
}

static uint32_t app_store_menu_back_callback(void* context) {
    UNUSED(context);
    return AppStoreViewMenu;
}

// --- App Lifecycle ---

static AppStoreApp* app_store_alloc(void) {
    AppStoreApp* app = malloc(sizeof(AppStoreApp));
    memset(app, 0, sizeof(AppStoreApp));

    app->gui = furi_record_open(RECORD_GUI);
    app->storage = furi_record_open(RECORD_STORAGE);
    app->notifications = furi_record_open(RECORD_NOTIFICATION);

    app->view_dispatcher = view_dispatcher_alloc();
    view_dispatcher_enable_queue(app->view_dispatcher);
    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);

    // Submenu (main app list)
    app->submenu = submenu_alloc();
    View* submenu_view = submenu_get_view(app->submenu);
    view_set_previous_callback(submenu_view, app_store_exit_callback);
    view_dispatcher_add_view(app->view_dispatcher, AppStoreViewMenu, submenu_view);

    // Dialog (app info / install confirm)
    app->dialog = dialog_ex_alloc();
    dialog_ex_set_result_callback(app->dialog, app_store_dialog_callback);
    dialog_ex_set_context(app->dialog, app);
    View* dialog_view = dialog_ex_get_view(app->dialog);
    view_set_previous_callback(dialog_view, app_store_menu_back_callback);
    view_dispatcher_add_view(app->view_dispatcher, AppStoreViewInfo, dialog_view);

    // Popup (result notification)
    app->popup = popup_alloc();
    popup_set_callback(app->popup, app_store_popup_callback);
    popup_set_context(app->popup, app);
    View* popup_view = popup_get_view(app->popup);
    view_set_previous_callback(popup_view, app_store_menu_back_callback);
    view_dispatcher_add_view(app->view_dispatcher, AppStoreViewResult, popup_view);

    return app;
}

static void app_store_free(AppStoreApp* app) {
    view_dispatcher_remove_view(app->view_dispatcher, AppStoreViewMenu);
    view_dispatcher_remove_view(app->view_dispatcher, AppStoreViewInfo);
    view_dispatcher_remove_view(app->view_dispatcher, AppStoreViewResult);

    submenu_free(app->submenu);
    dialog_ex_free(app->dialog);
    popup_free(app->popup);
    view_dispatcher_free(app->view_dispatcher);

    furi_record_close(RECORD_GUI);
    furi_record_close(RECORD_STORAGE);
    furi_record_close(RECORD_NOTIFICATION);

    free(app);
}

int32_t app_store_app(void* p) {
    UNUSED(p);

    AppStoreApp* app = app_store_alloc();

    // Parse manifest and populate menu
    if(!app_store_parse_manifest(app)) {
        FURI_LOG_W(TAG, "No manifest found, showing empty store");
    }
    app_store_refresh_menu(app);

    // Set header for empty state
    if(app->app_count == 0) {
        submenu_set_header(app->submenu, "App Store (empty)");
    } else {
        submenu_set_header(app->submenu, "App Store");
    }

    view_dispatcher_switch_to_view(app->view_dispatcher, AppStoreViewMenu);
    view_dispatcher_run(app->view_dispatcher);

    app_store_free(app);
    return 0;
}
