#include <ncurses.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>
#include <cjson/cJSON.h>
#include <sys/stat.h>
#include <sys/types.h>

struct MemoryBuffer
{
    char *data;
    size_t size;
};

typedef struct
{
    char title[128];
    char author[128];
    char id[128];
    char slug[128];
} SearchResult;

typedef struct
{
    char loader[32];
    char version[32];
    char download_url[512];
    char filename[128];
} VersionOption;

typedef enum
{
    VIEW_SEARCH,
    VIEW_DETAILS,
    VIEW_CONFIRM_DOWNLOAD
} AppView;

typedef enum
{
    FILTER_LOADER,
    FILTER_PROJECT_TYPE,
    FILTER_ENV
} FilterType;

typedef struct
{
    const char *label;
    const char *facet_val;
    FilterType type;
    bool selected;
} FilterOption;

static size_t WriteCallback(void *contents, size_t size, size_t nmemb, void *userp)
{
    size_t total_size = size * nmemb;
    struct MemoryBuffer *mem = (struct MemoryBuffer *)userp;

    char *ptr = realloc(mem->data, mem->size + total_size + 1);
    if (!ptr)
        return 0;

    mem->data = ptr;
    memcpy(&(mem->data[mem->size]), contents, total_size);
    mem->size += total_size;
    mem->data[mem->size] = 0;

    return total_size;
}

static size_t WriteFileCallback(void *ptr, size_t size, size_t nmemb, void *stream)
{
    return fwrite(ptr, size, nmemb, (FILE *)stream);
}

char *http_get(const char *url)
{
    CURL *curl_handle;
    CURLcode res;
    struct MemoryBuffer chunk;

    chunk.data = malloc(1);
    chunk.size = 0;

    if (!chunk.data)
        return NULL;

    curl_handle = curl_easy_init();
    if (!curl_handle)
    {
        free(chunk.data);
        return NULL;
    }

    curl_easy_setopt(curl_handle, CURLOPT_URL, url);
    curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, (void *)&chunk);
    curl_easy_setopt(curl_handle, CURLOPT_USERAGENT, "modrinth-cli");

    res = curl_easy_perform(curl_handle);
    curl_easy_cleanup(curl_handle);

    if (res != CURLE_OK)
    {
        free(chunk.data);
        return NULL;
    }

    return chunk.data;
}

bool download_file(const char *url, const char *out_filepath)
{
    CURL *curl_handle = curl_easy_init();
    if (!curl_handle)
        return false;

    FILE *fp = fopen(out_filepath, "wb");
    if (!fp)
    {
        curl_easy_cleanup(curl_handle);
        return false;
    }

    curl_easy_setopt(curl_handle, CURLOPT_URL, url);
    curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, WriteFileCallback);
    curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, fp);
    curl_easy_setopt(curl_handle, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl_handle, CURLOPT_USERAGENT, "modrinth-cli");

    CURLcode res = curl_easy_perform(curl_handle);
    curl_easy_cleanup(curl_handle);
    fclose(fp);

    return (res == CURLE_OK);
}

// Builds JSON Facets string
char *build_facets_json(FilterOption *options, int num_options, const char *version_filter)
{
    cJSON *facets_array = cJSON_CreateArray();

    // 1. Loaders & Server Software (OR group)
    cJSON *loaders_group = cJSON_CreateArray();
    for (int i = 0; i < num_options; i++)
    {
        if (options[i].selected && options[i].type == FILTER_LOADER)
        {
            char buf[128];
            snprintf(buf, sizeof(buf), "categories:%s", options[i].facet_val);
            cJSON_AddItemToArray(loaders_group, cJSON_CreateString(buf));
        }
    }
    if (cJSON_GetArraySize(loaders_group) > 0)
        cJSON_AddItemToArray(facets_array, loaders_group);
    else
        cJSON_Delete(loaders_group);

    // 2. Project Types (OR group)
    cJSON *types_group = cJSON_CreateArray();
    for (int i = 0; i < num_options; i++)
    {
        if (options[i].selected && options[i].type == FILTER_PROJECT_TYPE)
        {
            char buf[128];
            snprintf(buf, sizeof(buf), "project_type:%s", options[i].facet_val);
            cJSON_AddItemToArray(types_group, cJSON_CreateString(buf));
        }
    }
    if (cJSON_GetArraySize(types_group) > 0)
        cJSON_AddItemToArray(facets_array, types_group);
    else
        cJSON_Delete(types_group);

    // 3. Environment Filters
    cJSON *env_group = cJSON_CreateArray();
    for (int i = 0; i < num_options; i++)
    {
        if (options[i].selected && options[i].type == FILTER_ENV)
        {
            char buf[128];
            snprintf(buf, sizeof(buf), "client_side:%s", options[i].facet_val);
            cJSON_AddItemToArray(env_group, cJSON_CreateString(buf));
        }
    }
    if (cJSON_GetArraySize(env_group) > 0)
        cJSON_AddItemToArray(facets_array, env_group);
    else
        cJSON_Delete(env_group);

    // 4. Version Filter
    if (version_filter && strlen(version_filter) > 0)
    {
        cJSON *ver_group = cJSON_CreateArray();
        char buf[128];
        snprintf(buf, sizeof(buf), "versions:%s", version_filter);
        cJSON_AddItemToArray(ver_group, cJSON_CreateString(buf));
        cJSON_AddItemToArray(facets_array, ver_group);
    }

    char *json_out = cJSON_PrintUnformatted(facets_array);
    cJSON_Delete(facets_array);
    return json_out;
}

char *search_modrinth(char *query, FilterOption *options, int num_options, const char *version_filter, char *limit_raw)
{
    CURL *curl_handle = curl_easy_init();
    if (!curl_handle)
        return NULL;

    char *facets_raw = build_facets_json(options, num_options, version_filter);

    char *encoded_query = curl_easy_escape(curl_handle, query ? query : "", 0);
    char *encoded_facets = curl_easy_escape(curl_handle, facets_raw, 0);
    char *encoded_limit = curl_easy_escape(curl_handle, limit_raw, 0);

    char full_url[2048];
    snprintf(full_url, sizeof(full_url),
             "https://api.modrinth.com/v2/search?query=%s&facets=%s&limit=%s",
             encoded_query ? encoded_query : "",
             encoded_facets ? encoded_facets : "[]",
             encoded_limit ? encoded_limit : "10");

    free(facets_raw);
    curl_free(encoded_query);
    curl_free(encoded_facets);
    curl_free(encoded_limit);
    curl_easy_cleanup(curl_handle);

    return http_get(full_url);
}

int fetch_mod_versions(const char *project_id, VersionOption *out_versions, int max_count)
{
    char url[512];
    snprintf(url, sizeof(url), "https://api.modrinth.com/v2/project/%s/version", project_id);

    char *data = http_get(url);
    if (!data)
        return 0;

    int count = 0;
    cJSON *json = cJSON_Parse(data);
    if (json && cJSON_IsArray(json))
    {
        int total = cJSON_GetArraySize(json);
        for (int i = 0; i < total && count < max_count; i++)
        {
            cJSON *version_item = cJSON_GetArrayItem(json, i);
            cJSON *game_versions = cJSON_GetObjectItemCaseSensitive(version_item, "game_versions");
            cJSON *loaders = cJSON_GetObjectItemCaseSensitive(version_item, "loaders");
            cJSON *files = cJSON_GetObjectItemCaseSensitive(version_item, "files");

            const char *ver_str = "Unknown";
            if (cJSON_IsArray(game_versions) && cJSON_GetArraySize(game_versions) > 0)
            {
                cJSON *v = cJSON_GetArrayItem(game_versions, 0);
                if (v && v->valuestring)
                    ver_str = v->valuestring;
            }

            const char *file_url = "";
            const char *filename = "";

            if (cJSON_IsArray(files) && cJSON_GetArraySize(files) > 0)
            {
                cJSON *primary_file = NULL;
                for (int f = 0; f < cJSON_GetArraySize(files); f++)
                {
                    cJSON *f_item = cJSON_GetArrayItem(files, f);
                    cJSON *is_primary = cJSON_GetObjectItemCaseSensitive(f_item, "primary");
                    if (cJSON_IsTrue(is_primary))
                    {
                        primary_file = f_item;
                        break;
                    }
                }
                if (!primary_file)
                    primary_file = cJSON_GetArrayItem(files, 0);

                cJSON *url_obj = cJSON_GetObjectItemCaseSensitive(primary_file, "url");
                cJSON *fn_obj = cJSON_GetObjectItemCaseSensitive(primary_file, "filename");

                if (url_obj && url_obj->valuestring)
                    file_url = url_obj->valuestring;
                if (fn_obj && fn_obj->valuestring)
                    filename = fn_obj->valuestring;
            }

            if (cJSON_IsArray(loaders))
            {
                for (int l = 0; l < cJSON_GetArraySize(loaders) && count < max_count; l++)
                {
                    cJSON *loader_item = cJSON_GetArrayItem(loaders, l);
                    if (loader_item && loader_item->valuestring)
                    {
                        strncpy(out_versions[count].loader, loader_item->valuestring, sizeof(out_versions[count].loader) - 1);
                        strncpy(out_versions[count].version, ver_str, sizeof(out_versions[count].version) - 1);
                        strncpy(out_versions[count].download_url, file_url, sizeof(out_versions[count].download_url) - 1);
                        strncpy(out_versions[count].filename, filename, sizeof(out_versions[count].filename) - 1);

                        out_versions[count].loader[sizeof(out_versions[count].loader) - 1] = '\0';
                        out_versions[count].version[sizeof(out_versions[count].version) - 1] = '\0';
                        out_versions[count].download_url[sizeof(out_versions[count].download_url) - 1] = '\0';
                        out_versions[count].filename[sizeof(out_versions[count].filename) - 1] = '\0';
                        count++;
                    }
                }
            }
        }
    }

    if (json)
        cJSON_Delete(json);
    free(data);

    return count;
}

int main(void)
{
    struct stat stats;
    int stat_ec = stat("mods", &stats);
    
    if(S_ISREG(stats.st_mode)) {
      remove("mods");
      mkdir("mods",0755);
    } else if (!stat_ec == 0 && !S_ISDIR(stats.st_mode)) {
      mkdir("mods",0755);
    }

    initscr();
    cbreak();
    noecho();
    curs_set(0);
    keypad(stdscr, TRUE);

    start_color();
    init_pair(1, COLOR_GREEN, COLOR_BLACK);
    init_pair(2, COLOR_WHITE, COLOR_GREEN);

    FilterOption options[] = {
        // Mod Loaders
        {"[Loader] Fabric", "fabric", FILTER_LOADER, false},
        {"[Loader] Forge", "forge", FILTER_LOADER, false},
        {"[Loader] NeoForge", "neoforge", FILTER_LOADER, false},
        {"[Loader] Babric", "babric", FILTER_LOADER, false},
        {"[Loader] BTA (Babric)", "bta", FILTER_LOADER, false},
        {"[Loader] Java Agent", "java_agent", FILTER_LOADER, false},
        {"[Loader] Legacy Fabric", "legacyfabric", FILTER_LOADER, false},
        {"[Loader] LiteLoader", "liteloader", FILTER_LOADER, false},
        {"[Loader] ModLoader", "modloader", FILTER_LOADER, false},
        {"[Loader] NilLoader", "nilloader", FILTER_LOADER, false},
        {"[Loader] Ornithe", "ornithe", FILTER_LOADER, false},
        {"[Loader] Quilt", "quilt", FILTER_LOADER, false},
        {"[Loader] Rift", "rift", FILTER_LOADER, false},

        // Server Platforms
        {"[Server] Paper", "paper", FILTER_LOADER, false},
        {"[Server] Purpur", "purpur", FILTER_LOADER, false},
        {"[Server] Waterfall", "waterfall", FILTER_LOADER, false},
        {"[Server] Velocity", "velocity", FILTER_LOADER, false},
        {"[Server] Folia", "folia", FILTER_LOADER, false},

        // Project Types
        {"[Type] Mod", "mod", FILTER_PROJECT_TYPE, false},
        {"[Type] Modpack", "modpack", FILTER_PROJECT_TYPE, false},
        {"[Type] Resourcepack", "resourcepack", FILTER_PROJECT_TYPE, false},
        {"[Type] Shader", "shader", FILTER_PROJECT_TYPE, false},
        {"[Type] Plugin", "plugin", FILTER_PROJECT_TYPE, false},

        // Environment Options
        {"[Env] Client Required", "required", FILTER_ENV, false},
        {"[Env] Client Optional", "optional", FILTER_ENV, false},
        {"[Env] Client Unsupported", "unsupported", FILTER_ENV, false}
    };
    int num_options = sizeof(options) / sizeof(options[0]);

    char query_buffer[256] = "";
    int query_len = 0;

    char version_buffer[64] = "";
    int version_len = 0;

    SearchResult results[50];
    int results_count = 0;

    VersionOption versions[100];
    int versions_count = 0;

    int hover = 0;
    int hover_column = 0;

    AppView current_view = VIEW_SEARCH;
    SearchResult selected_mod;
    VersionOption selected_version;

    int confirm_choice = 0;
    char status_msg[256] = "";

    int scroll_offset_col1 = 0;
    int scroll_offset_col2 = 0;

    int key = 0;

    while (key != 'q' || (current_view == VIEW_SEARCH && hover_column == 2 && hover == 0))
    {
        int rows, cols;
        getmaxyx(stdscr, rows, cols);

        int col2_x = cols * 0.28;
        int col3_x = cols * (0.28 + 0.44);

        clear();

        int col1_start = 0;
        int col1_end = col2_x;

        int col2_start = col2_x + 1;
        int col2_end = col3_x;

        int col3_start = col3_x + 1;
        int col3_end = cols;

        mvprintw(0, col1_start + (col1_end - col1_start - 7) / 2, "Filters");
        mvprintw(0, col2_start + (col2_end - col2_start - 6) / 2, "Result");
        mvprintw(0, col3_start + (col3_end - col3_start - 5) / 2, "Query");

        for (int x = 0; x < cols; ++x)
            mvprintw(1, x, "-");

        for (int y = 0; y < rows; y++)
        {
            mvaddch(y, col2_x, '|');
            mvaddch(y, col3_x, '|');
        }

        refresh();

        int win_height = rows - 2;

        // COLUMN 1 (Filters Window)
        int col1_width = col2_x;
        if (win_height > 0 && col1_width > 0)
        {
            WINDOW *col1_win = newwin(win_height, col1_width, 2, 0);
            int max_visible_col1 = win_height - 2;
            if (max_visible_col1 < 1)
                max_visible_col1 = 1;

            if (current_view == VIEW_SEARCH && hover_column == 0)
            {
                if (hover < scroll_offset_col1)
                    scroll_offset_col1 = hover;
                else if (hover >= scroll_offset_col1 + max_visible_col1)
                    scroll_offset_col1 = hover - max_visible_col1 + 1;
            }

            mvwprintw(col1_win, 0, 1, "Facets");

            for (int i = 0; i < max_visible_col1 && (i + scroll_offset_col1) < num_options; i++)
            {
                int idx = i + scroll_offset_col1;
                bool is_selected = options[idx].selected;
                bool is_hovered = (idx == hover && hover_column == 0 && current_view == VIEW_SEARCH);

                if (is_selected && is_hovered)
                    wattrset(col1_win, COLOR_PAIR(2) | A_BOLD | A_REVERSE);
                else if (is_selected)
                    wattrset(col1_win, COLOR_PAIR(2) | A_BOLD);
                else if (is_hovered)
                    wattrset(col1_win, COLOR_PAIR(1) | A_BOLD);
                else
                    wattrset(col1_win, A_NORMAL);

                mvwprintw(col1_win, i + 2, 1, "%s", options[idx].label);
            }

            wrefresh(col1_win);
            delwin(col1_win);
        }

        // COLUMN 2 (Results & Details Window)
        int col2_width = col3_x - col2_x - 1;
        if (win_height > 0 && col2_width > 0)
        {
            WINDOW *col2_win = newwin(win_height, col2_width, 2, col2_x + 1);

            if (current_view == VIEW_DETAILS)
            {
                mvwprintw(col2_win, 0, 1, "Mod: %s", selected_mod.title);

                int max_visible_col2 = win_height - 2;
                if (max_visible_col2 < 1)
                    max_visible_col2 = 1;

                if (hover < scroll_offset_col2)
                    scroll_offset_col2 = hover;
                else if (hover >= scroll_offset_col2 + max_visible_col2)
                    scroll_offset_col2 = hover - max_visible_col2 + 1;

                for (int v = 0; v < max_visible_col2 && (v + scroll_offset_col2) < versions_count; v++)
                {
                    int idx = v + scroll_offset_col2;
                    bool is_hovered = (idx == hover);
                    if (is_hovered)
                        wattrset(col2_win, COLOR_PAIR(2) | A_BOLD);
                    else
                        wattrset(col2_win, A_NORMAL);

                    mvwprintw(col2_win, v + 2, 1, "[%s] v%s", versions[idx].loader, versions[idx].version);
                }
            }
            else
            {
                int max_visible_col2 = win_height - 1;
                if (max_visible_col2 < 1)
                    max_visible_col2 = 1;

                if (hover_column == 1)
                {
                    if (hover < scroll_offset_col2)
                        scroll_offset_col2 = hover;
                    else if (hover >= scroll_offset_col2 + max_visible_col2)
                        scroll_offset_col2 = hover - max_visible_col2 + 1;
                }

                if (results_count == 0)
                {
                    mvwprintw(col2_win, 1, 1, "No results.");
                }
                else
                {
                    for (int i = 0; i < max_visible_col2 && (i + scroll_offset_col2) < results_count; i++)
                    {
                        int idx = i + scroll_offset_col2;
                        bool is_hovered = (idx == hover && hover_column == 1);

                        if (is_hovered)
                            wattrset(col2_win, COLOR_PAIR(1) | A_BOLD);
                        else
                            wattrset(col2_win, A_NORMAL);

                        mvwprintw(col2_win, i, 1, "%s by %s", results[idx].title, results[idx].author);
                    }
                }
            }

            wrefresh(col2_win);
            delwin(col2_win);
        }

        // COLUMN 3 (Query, Search, Version Input & Active Filters)
        int col3_width = cols - col3_x - 1;
        if (win_height > 0 && col3_width > 0)
        {
            WINDOW *col3_win = newwin(win_height, col3_width, 2, col3_x + 1);

            int visible_width = col3_width - 4;
            if (visible_width < 1)
                visible_width = 1;

            int text_offset = 0;
            if (query_len > visible_width)
                text_offset = query_len - visible_width;

            // Search input field
            mvwprintw(col3_win, 0, 1, "Search Term:");
            if (current_view == VIEW_SEARCH && hover_column == 2 && hover == 0)
                wattrset(col3_win, COLOR_PAIR(1) | A_BOLD);
            else
                wattrset(col3_win, A_NORMAL);

            mvwprintw(col3_win, 1, 1, "[ %-*.*s ]", visible_width - 2, visible_width - 2, query_buffer + text_offset);

            // Version filter field
            wattrset(col3_win, A_NORMAL);
            mvwprintw(col3_win, 3, 1, "Game Version Filter:");
            if (current_view == VIEW_SEARCH && hover_column == 2 && hover == 1)
                wattrset(col3_win, COLOR_PAIR(1) | A_BOLD);
            else
                wattrset(col3_win, A_NORMAL);

            mvwprintw(col3_win, 4, 1, "[ %-*.*s ]", visible_width - 2, visible_width - 2, version_buffer);

            // Search button
            if (current_view == VIEW_SEARCH && hover_column == 2 && hover == 2)
                wattrset(col3_win, COLOR_PAIR(2) | A_BOLD);
            else
                wattrset(col3_win, COLOR_PAIR(1));

            mvwprintw(col3_win, 6, 1, "[ Search ]");

            // Selected filters output
            wattrset(col3_win, A_NORMAL);
            mvwprintw(col3_win, 8, 1, "Selected Filters:");

            int render_line = 9;
            for (int i = 0; i < num_options; i++)
            {
                if (options[i].selected)
                {
                    mvwprintw(col3_win, render_line, 2, "- %s", options[i].label);
                    render_line++;
                }
            }

            if (strlen(status_msg) > 0)
            {
                wattrset(col3_win, COLOR_PAIR(1) | A_BOLD);
                mvwprintw(col3_win, win_height - 2, 1, "%s", status_msg);
            }

            wrefresh(col3_win);
            delwin(col3_win);
        }

        // CONFIRM DOWNLOAD MODAL OVERLAY
        if (current_view == VIEW_CONFIRM_DOWNLOAD)
        {
            int dlg_w = 60;
            int dlg_h = 8;
            int dlg_y = (rows - dlg_h) / 2;
            int dlg_x = (cols - dlg_w) / 2;

            WINDOW *dlg = newwin(dlg_h, dlg_w, dlg_y, dlg_x);
            box(dlg, 0, 0);

            mvwprintw(dlg, 1, (dlg_w - 18) / 2, "DOWNLOAD CONFIRM");
            mvwprintw(dlg, 3, 2, "Are you sure you want to download?");
            mvwprintw(dlg, 4, 2, "%s [%s] v%s", selected_mod.title, selected_version.loader, selected_version.version);

            if (confirm_choice == 0)
                wattrset(dlg, COLOR_PAIR(2) | A_BOLD);
            else
                wattrset(dlg, COLOR_PAIR(1));
            mvwprintw(dlg, 6, (dlg_w / 2) - 10, "[ YES ]");

            if (confirm_choice == 1)
                wattrset(dlg, COLOR_PAIR(2) | A_BOLD);
            else
                wattrset(dlg, COLOR_PAIR(1));
            mvwprintw(dlg, 6, (dlg_w / 2) + 4, "[ NO ]");

            wrefresh(dlg);
            delwin(dlg);
        }

        int current_max = 0;
        if (current_view == VIEW_SEARCH)
        {
            if (hover_column == 0)
                current_max = num_options;
            else if (hover_column == 1)
                current_max = results_count;
            else if (hover_column == 2)
                current_max = 3; // 0=Query, 1=Version Filter, 2=Search Button
        }
        else if (current_view == VIEW_DETAILS)
        {
            current_max = versions_count;
        }

        key = getch();

        if (current_view == VIEW_CONFIRM_DOWNLOAD)
        {
            if (key == KEY_LEFT || key == KEY_RIGHT || key == '\t')
            {
                confirm_choice = 1 - confirm_choice;
            }
            else if (key == KEY_ENTER || key == '\n' || key == '\r')
            {
                if (confirm_choice == 0)
                {
                    if (strlen(selected_version.download_url) > 0)
                    {
                        char target_path[256];
                        snprintf(target_path, sizeof(target_path), "./%s",
                                 strlen(selected_version.filename) > 0 ? selected_version.filename : "downloaded_mod.jar");

                        bool ok = download_file(selected_version.download_url, target_path);
                        if (ok)
                            snprintf(status_msg, sizeof(status_msg), "Downloaded: %s", target_path);
                        else
                            snprintf(status_msg, sizeof(status_msg), "Download failed!");
                    }
                    else
                    {
                        snprintf(status_msg, sizeof(status_msg), "No download URL available!");
                    }
                }
                current_view = VIEW_DETAILS;
            }
            else if (key == 27 || key == 'b' || key == 'B')
            {
                current_view = VIEW_DETAILS;
            }
        }
        else if (current_view == VIEW_DETAILS)
        {
            if (key == KEY_UP && hover > 0)
            {
                hover--;
            }
            else if (key == KEY_DOWN && hover < versions_count - 1)
            {
                hover++;
            }
            else if (key == KEY_ENTER || key == '\n' || key == '\r')
            {
                if (versions_count > 0)
                {
                    selected_version = versions[hover];
                    confirm_choice = 0;
                    current_view = VIEW_CONFIRM_DOWNLOAD;
                }
            }
            else if (key == 27 || key == 'b' || key == 'B')
            {
                current_view = VIEW_SEARCH;
                hover_column = 1;
                hover = 0;
                scroll_offset_col2 = 0;
            }
        }
        else
        {
            if (key == KEY_UP && hover > 0)
            {
                hover--;
            }
            else if (key == KEY_DOWN && current_max > 0 && hover < current_max - 1)
            {
                hover++;
            }
            else if (key == KEY_LEFT)
            {
                hover_column = (hover_column - 1 + 3) % 3;
                hover = 0;
            }
            else if (key == KEY_RIGHT)
            {
                hover_column = (hover_column + 1) % 3;
                hover = 0;
            }
            else if (key == KEY_ENTER || key == '\n' || key == '\r')
            {
                if (hover_column == 0)
                {
                    options[hover].selected = !options[hover].selected;
                }
                else if (hover_column == 1 && results_count > 0)
                {
                    selected_mod = results[hover];
                    versions_count = fetch_mod_versions(selected_mod.id, versions, 100);
                    current_view = VIEW_DETAILS;
                    hover = 0;
                    scroll_offset_col2 = 0;
                }
                else if (hover_column == 2 && hover == 2)
                {
                    char *data = search_modrinth(query_buffer, options, num_options, version_buffer, "100");
                    results_count = 0;

                    if (data != NULL)
                    {
                        cJSON *json = cJSON_Parse(data);
                        if (json != NULL)
                        {
                            cJSON *hits = cJSON_GetObjectItemCaseSensitive(json, "hits");
                            if (cJSON_IsArray(hits))
                            {
                                int array_size = cJSON_GetArraySize(hits);
                                if (array_size > 50)
                                    array_size = 50;

                                for (int i = 0; i < array_size; i++)
                                {
                                    cJSON *item = cJSON_GetArrayItem(hits, i);
                                    cJSON *title_obj = cJSON_GetObjectItemCaseSensitive(item, "title");
                                    cJSON *author_obj = cJSON_GetObjectItemCaseSensitive(item, "author");
                                    cJSON *id_obj = cJSON_GetObjectItemCaseSensitive(item, "project_id");

                                    const char *title = (title_obj && title_obj->valuestring) ? title_obj->valuestring : "Unknown";
                                    const char *author = (author_obj && author_obj->valuestring) ? author_obj->valuestring : "Unknown";
                                    const char *mod_id = (id_obj && id_obj->valuestring) ? id_obj->valuestring : "N/A";

                                    strncpy(results[i].title, title, sizeof(results[i].title) - 1);
                                    results[i].title[sizeof(results[i].title) - 1] = '\0';

                                    strncpy(results[i].author, author, sizeof(results[i].author) - 1);
                                    results[i].author[sizeof(results[i].author) - 1] = '\0';

                                    strncpy(results[i].id, mod_id, sizeof(results[i].id) - 1);
                                    results[i].id[sizeof(results[i].id) - 1] = '\0';

                                    results_count++;
                                }
                            }
                            cJSON_Delete(json);
                        }
                        free(data);
                    }
                }
            }
            else if (hover_column == 2 && hover == 0)
            {
                if ((key == KEY_BACKSPACE || key == 127 || key == '\b') && query_len > 0)
                {
                    query_buffer[--query_len] = '\0';
                }
                else if (key >= 32 && key <= 126 && query_len < (int)sizeof(query_buffer) - 1)
                {
                    query_buffer[query_len++] = (char)key;
                    query_buffer[query_len] = '\0';
                }
            }
            else if (hover_column == 2 && hover == 1)
            {
                if ((key == KEY_BACKSPACE || key == 127 || key == '\b') && version_len > 0)
                {
                    version_buffer[--version_len] = '\0';
                }
                else if (key >= 32 && key <= 126 && version_len < (int)sizeof(version_buffer) - 1)
                {
                    version_buffer[version_len++] = (char)key;
                    version_buffer[version_len] = '\0';
                }
            }
        }
    }

    endwin();
    return 0;
}