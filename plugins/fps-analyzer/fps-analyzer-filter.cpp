#include <obs-module.h>
#include <util/platform.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#define strcasecmp _stricmp
#endif

// Declare overlay info for registration in overlay file
extern struct obs_source_info fps_overlay_source_info;

#define FPS_CSV_HISTORY_LIMIT 300
#define ROLLING_MAX 120 // max 2 sekundy przy 60 FPS
#define FRAMETIME_HISTORY 60

// Dodaj enum do wyboru metody analizy
typedef enum {
    ANALYZE_LAST_LINE = 0,
    ANALYZE_DIFF = 1
} analyze_method_t;

// Prototypes
static void keep_last_n_lines(const char *csv_path, int n);
static void build_txt_path(const char *output_path, char *txt_path, size_t txt_path_size);
static void build_csv_path(const char *output_path, char *csv_path, size_t csv_path_size);

struct fps_analyzer_filter {
    obs_source_t *context;
    char output_path[512];
    double update_interval;
    uint64_t last_unique_frame_time;
    bool clear_csv_on_start;
    uint64_t rolling_times[ROLLING_MAX];
    int rolling_count;
    int rolling_start;
    uint64_t last_write_time;
    double frametime_history[FRAMETIME_HISTORY];
    int frametime_pos;
    int frametime_count;
    analyze_method_t analyze_method;
    double sensitivity;
    uint8_t *prev_frame;
    size_t prev_frame_size;
    int tearing_detected;
    bool enable_tearing_detection;
    double tearing_sensitivity;
    uint8_t *prev_lines[3];
    size_t prev_lines_size;
    int tearing_history[5];
    int tearing_history_pos;
};

// Funkcja do liczenia różniących się bajtów
static size_t count_diff_bytes(const uint8_t *a, const uint8_t *b, size_t size) {
    size_t diff = 0;
    for (size_t i = 0; i < size; ++i) {
        if (a[i] != b[i]) ++diff;
    }
    return diff;
}

// Funkcja do inicjalizacji buforów dla poprzednich linii
static void init_prev_lines_buffers(struct fps_analyzer_filter *filter, int roi_width) {
    for (int i = 0; i < 3; ++i) {
        if (filter->prev_lines[i]) bfree(filter->prev_lines[i]);
        filter->prev_lines[i] = (uint8_t*)bzalloc(roi_width);
    }
    filter->prev_lines_size = roi_width;
}

// Funkcja do inicjalizacji bufora poprzedniej klatki
static void init_prev_frame_buffer(struct fps_analyzer_filter *filter, size_t roi_size, const uint8_t *roi_ptr) {
    if (filter->prev_frame) bfree(filter->prev_frame);
    filter->prev_frame = (uint8_t*)bzalloc(roi_size);
    filter->prev_frame_size = roi_size;
    memcpy(filter->prev_frame, roi_ptr, roi_size);
}

// Funkcja do wykrywania tearingu na podstawie 3 linii
static bool detect_tearing(struct fps_analyzer_filter *filter, struct obs_source_frame *frame) {
    if (!filter->enable_tearing_detection) return false;
    
    const int roi_width = frame->width;
    uint8_t lines_luma[3*4096];
    int lines[3] = {0, (int)frame->height/2, (int)frame->height-1};
    
    // Pobierz 3 linie (góra, środek, dół)
    if (frame->format == VIDEO_FORMAT_NV12) {
        for (int i = 0; i < 3; ++i) {
            int y = lines[i];
            memcpy(lines_luma + i*roi_width, frame->data[0] + y*frame->linesize[0], roi_width);
        }
    } else if (frame->format == VIDEO_FORMAT_YUY2) {
        for (int i = 0; i < 3; ++i) {
            int y = lines[i];
            uint8_t *src = frame->data[0] + y*frame->linesize[0];
            for (int x = 0; x < (int)roi_width; ++x) {
                lines_luma[i*roi_width + x] = src[x*2];
            }
        }
    } else {
        return false;
    }
    
    // Sprawdź czy mamy poprzednie linie
    if (!filter->prev_lines[0] || filter->prev_lines_size != roi_width) {
        init_prev_lines_buffers(filter, roi_width);
        memcpy(filter->prev_lines[0], lines_luma, roi_width);
        memcpy(filter->prev_lines[1], lines_luma + roi_width, roi_width);
        memcpy(filter->prev_lines[2], lines_luma + 2*roi_width, roi_width);
        return false;
    }
    
    // Porównaj 3 linie osobno z progiem czułości
    double change_percent[3] = {0.0, 0.0, 0.0};
    for (int i = 0; i < 3; ++i) {
        size_t diff = count_diff_bytes(lines_luma + i*roi_width, filter->prev_lines[i], roi_width);
        change_percent[i] = (roi_width > 0) ? (100.0 * diff / roi_width) : 0.0;
    }
    
    // Zapisz aktualne linie
    memcpy(filter->prev_lines[0], lines_luma, roi_width);
    memcpy(filter->prev_lines[1], lines_luma + roi_width, roi_width);
    memcpy(filter->prev_lines[2], lines_luma + 2*roi_width, roi_width);
    
    // Ulepszona logika wykrywania tearingu
    bool significant_change[3] = {false, false, false};
    for (int i = 0; i < 3; ++i) {
        significant_change[i] = (change_percent[i] >= filter->tearing_sensitivity);
    }
    
    // Wykryj tearing: jeśli nie wszystkie linie się zmieniły jednocześnie
    bool all_changed = significant_change[0] && significant_change[1] && significant_change[2];
    bool none_changed = !significant_change[0] && !significant_change[1] && !significant_change[2];
    
    bool tearing_detected = !(all_changed || none_changed);
    
    // Dodaj do historii tearingu
    filter->tearing_history[filter->tearing_history_pos] = tearing_detected ? 1 : 0;
    filter->tearing_history_pos = (filter->tearing_history_pos + 1) % 5;
    
    // Sprawdź czy w ostatnich 5 klatkach było więcej niż 2 wykrycia tearingu
    int recent_tears = 0;
    for (int i = 0; i < 5; ++i) {
        recent_tears += filter->tearing_history[i];
    }
    
    return (recent_tears >= 2); // Zwróć true tylko jeśli w ostatnich 5 klatkach było co najmniej 2 wykrycia
}

static struct obs_source_frame *fps_analyzer_filter_video(void *data, struct obs_source_frame *frame)
{
    struct fps_analyzer_filter *filter = (struct fps_analyzer_filter *)data;
    if (frame && frame->data[0]) {
        int roi_line, roi_lines;
        const int roi_width = frame->width;
        uint8_t *roi_ptr = NULL;
        size_t roi_size = 0;
        int is_unique = 0;
        if (filter->analyze_method == ANALYZE_DIFF) {
            // Pełna analiza: cała klatka
            roi_line = 0;
            roi_lines = frame->height;
        } else {
            // Last line: tylko ostatnia linia
            roi_line = frame->height - 1;
            roi_lines = 1;
        }
        if (frame->format == VIDEO_FORMAT_NV12) {
            if (filter->analyze_method == ANALYZE_DIFF) {
                // Cała klatka: kopiujemy całość luminancji do bufora
                static uint8_t nv12_luma[4096*2160]; // max 4K, jeśli więcej - przyciąć
                size_t max_size = sizeof(nv12_luma);
                size_t full_size = roi_width * frame->height;
                size_t copy_size = (full_size < max_size) ? full_size : max_size;
                for (int y = 0; y < (int)frame->height && y*roi_width < (int)copy_size; ++y) {
                    memcpy(nv12_luma + y*roi_width, frame->data[0] + y*frame->linesize[0], roi_width);
                }
                roi_ptr = nv12_luma;
                roi_size = copy_size;
            } else {
                roi_ptr = frame->data[0] + roi_line * frame->linesize[0];
                roi_size = roi_width * roi_lines;
            }
        } else if (frame->format == VIDEO_FORMAT_YUY2) {
            if (filter->analyze_method == ANALYZE_DIFF) {
                // Cała klatka: kopiujemy tylko bajty Y z każdej linii
                static uint8_t yuy2_luma[4096*2160]; // max 4K, jeśli więcej - przyciąć
                size_t max_size = sizeof(yuy2_luma);
                size_t full_size = roi_width * frame->height;
                size_t copy_size = (full_size < max_size) ? full_size : max_size;
                for (int y = 0; y < (int)frame->height && y*roi_width < (int)copy_size; ++y) {
                    uint8_t *src = frame->data[0] + y*frame->linesize[0];
                    for (int x = 0; x < (int)roi_width && y*roi_width + x < (int)copy_size; ++x) {
                        yuy2_luma[y*roi_width + x] = src[x*2];
                    }
                }
                roi_ptr = yuy2_luma;
                roi_size = copy_size;
            } else {
                roi_ptr = frame->data[0] + roi_line * frame->linesize[0];
                static uint8_t yuy2_luma[4096];
                size_t max_width = sizeof(yuy2_luma);
                size_t copy_width = roi_width < max_width ? roi_width : max_width;
                for (size_t i = 0, j = 0; i < copy_width; ++i, j += 2) {
                    yuy2_luma[i] = roi_ptr[j];
                }
                roi_ptr = yuy2_luma;
                roi_size = copy_width * roi_lines;
            }
        } else {
            return frame;
        }
        // Wspólna logika analizy dla obu metod
        if (!filter->prev_frame || filter->prev_frame_size != roi_size) {
            init_prev_frame_buffer(filter, roi_size, roi_ptr);
            is_unique = 1;
        } else {
            size_t diff = count_diff_bytes(roi_ptr, filter->prev_frame, roi_size);
            double percent = (roi_size > 0) ? (100.0 * diff / roi_size) : 0.0;
            if (percent >= filter->sensitivity) {
                is_unique = 1;
            }
            memcpy(filter->prev_frame, roi_ptr, roi_size);
        }
        
        // Wykrywanie tearingu (niezależne od metody analizy)
        filter->tearing_detected = detect_tearing(filter, frame);
        // --- rolling window i reszta logiki bez zmian ---
        if (is_unique) {
            uint64_t now = os_gettime_ns();
            int idx = (filter->rolling_start + filter->rolling_count) % ROLLING_MAX;
            filter->rolling_times[idx] = now;
            if (filter->rolling_count < ROLLING_MAX) {
                filter->rolling_count++;
            } else {
                filter->rolling_start = (filter->rolling_start + 1) % ROLLING_MAX;
            }
            while (filter->rolling_count > 0 &&
                   now - filter->rolling_times[filter->rolling_start] > 1000000000ULL) {
                filter->rolling_start = (filter->rolling_start + 1) % ROLLING_MAX;
                filter->rolling_count--;
            }
            if (filter->last_unique_frame_time != 0) {
                double ft = (now - filter->last_unique_frame_time) / 1000000.0;
                filter->frametime_history[filter->frametime_pos] = ft;
                filter->frametime_pos = (filter->frametime_pos + 1) % FRAMETIME_HISTORY;
                if (filter->frametime_count < FRAMETIME_HISTORY)
                    filter->frametime_count++;
            }
            filter->last_unique_frame_time = now;
        }
    }
    return frame;
}

static void fps_analyzer_video_tick(void *data, float seconds)
{
    struct fps_analyzer_filter *filter = (struct fps_analyzer_filter *)data;
    uint64_t now = os_gettime_ns();
    double elapsed = (now - filter->last_write_time) / 1000000000.0;
    if (elapsed < filter->update_interval)
        return;
    filter->last_write_time = now;
    // --- FPS jako odwrotność średniego frametime z ostatnich 60 klatek ---
    double avg_frametime = 0.0;
    for (int i = 0; i < filter->frametime_count; ++i)
        avg_frametime += filter->frametime_history[i];
    if (filter->frametime_count > 0)
        avg_frametime /= filter->frametime_count;
    double fps = (avg_frametime > 0.0) ? (1000.0 / avg_frametime) : 0.0;
    // zaokrąglenie do liczby całkowitej po to żeby nie mieć FPS z przecinkami
    int fps_smooth = (int)round(fps);
    double frametime_ms = avg_frametime;
    
    // Zapisz do pliku TXT
    char path[512];
    build_txt_path(filter->output_path, path, sizeof(path));
    FILE *f = fopen(path, "w");
    if (f) {
        if (filter->tearing_detected) {
            fprintf(f, "FPS: %d\nFrametime: %.2f ms\nWarning: Tearing detected", fps_smooth, frametime_ms);
        }
        else{
            fprintf(f, "FPS: %d\nFrametime: %.2f ms\n", fps_smooth, frametime_ms);
        }
        fclose(f);
    }
    
    // Zapisz do pliku CSV
    char csv_path[512];
    build_csv_path(filter->output_path, csv_path, sizeof(csv_path));
    FILE *csv = fopen(csv_path, "a");
    if (csv) {
        time_t t = time(NULL);
        fprintf(csv, "%lld,%d,%.2f\n", (long long)t, fps_smooth, frametime_ms);
        fclose(csv);
    }
    keep_last_n_lines(csv_path, FPS_CSV_HISTORY_LIMIT);
}

static void fps_analyzer_destroy(void *data)
{
    struct fps_analyzer_filter *filter = (struct fps_analyzer_filter *)data;
    if (filter) {
        if (filter->prev_frame) bfree(filter->prev_frame);
        for (int i = 0; i < 3; ++i) {
            if (filter->prev_lines[i]) bfree(filter->prev_lines[i]);
        }
    }
    bfree(data);
}

static void *fps_analyzer_create(obs_data_t *settings, obs_source_t *context)
{
    struct fps_analyzer_filter *filter = (struct fps_analyzer_filter *)bzalloc(sizeof(struct fps_analyzer_filter));
    filter->context = context;
    filter->output_path[0] = '\0';
    filter->update_interval = 1.0;
    filter->last_unique_frame_time = 0;
    filter->clear_csv_on_start = obs_data_get_bool(settings, "clear_csv_on_start");
    const char *path = obs_data_get_string(settings, "output_path");
    if (path) {
        strncpy(filter->output_path, path, sizeof(filter->output_path));
        filter->output_path[sizeof(filter->output_path)-1] = '\0';
    }
    filter->update_interval = obs_data_get_double(settings, "update_interval");
    if (filter->update_interval <= 0.0)
        filter->update_interval = 1.0;
    filter->rolling_count = 0;
    filter->rolling_start = 0;
    filter->last_write_time = 0;
    filter->frametime_pos = 0;
    filter->frametime_count = 0;
    filter->analyze_method = (analyze_method_t)obs_data_get_int(settings, "analyze_method");
    filter->sensitivity = obs_data_get_double(settings, "sensitivity");
    filter->tearing_detected = 0;
    filter->enable_tearing_detection = obs_data_get_bool(settings, "enable_tearing_detection");
    filter->tearing_sensitivity = obs_data_get_double(settings, "tearing_sensitivity");
    filter->prev_lines[0] = NULL;
    filter->prev_lines[1] = NULL;
    filter->prev_lines[2] = NULL;
    filter->prev_lines_size = 0;
    filter->tearing_history_pos = 0;
    for (int i = 0; i < 5; ++i) {
        filter->tearing_history[i] = 0;
    }
    return filter;
}

static const char *fps_analyzer_get_name(void *unused)
{
    UNUSED_PARAMETER(unused);
    return "FPS Analyzer 0.2";
}

// Callback do dynamicznego włączania/wyłączania slidera sensitivity
static bool analyze_method_modified(obs_properties_t *props, obs_property_t *p, obs_data_t *settings)
{
    int method = (int)obs_data_get_int(settings, "analyze_method");
    obs_property_t *slider = obs_properties_get(props, "sensitivity");
    obs_property_set_visible(slider, method == ANALYZE_DIFF || method == ANALYZE_LAST_LINE);
    return true;
}

static obs_properties_t *fps_analyzer_properties(void *data)
{
    obs_properties_t *props = obs_properties_create();
    obs_properties_add_path(props, "output_path", "FPS Output file",
                            OBS_PATH_FILE_SAVE, "Text File (*.txt)", NULL);
    obs_properties_add_bool(props, "clear_csv_on_start", "Clear CSV file on start (default: yes)");
    obs_properties_add_bool(props, "enable_tearing_detection", "Tearing detection (default: yes)");
    obs_properties_add_float_slider(props, "tearing_sensitivity", "Tearing sensitivity threshold (%)", 0.1, 10.0, 0.1);
    obs_properties_add_text(
        props,
        "sensitivity_info",
        "",
        OBS_TEXT_INFO
    );
    obs_property_t *interval = obs_properties_add_list(
        props, "update_interval", "Update interval (seconds)",
        OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_FLOAT);
    obs_property_list_add_float(interval, "0.5", 0.5);
    obs_property_list_add_float(interval, "1", 1.0);
    obs_property_list_add_float(interval, "2", 2.0);
    obs_property_set_long_description(interval, "How often FPS/frametime is written to file.");
    // Dropdown do wyboru metody analizy
    obs_property_t *method = obs_properties_add_list(props, "analyze_method", "Analysis method",
        OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(method, "Last line diff (pixel analysis)", ANALYZE_LAST_LINE);
    obs_property_list_add_int(method, "Full frame diff (all lines)", ANALYZE_DIFF);
    // Slider do progu czułości
    obs_property_t *slider = obs_properties_add_float_slider(props, "sensitivity", "Sensitivity threshold (%)", 0.0, 5.0, 0.1);
    // Ustaw widoczność na starcie
    int method_val = data ? ((struct fps_analyzer_filter*)data)->analyze_method : ANALYZE_LAST_LINE;
    obs_property_set_visible(slider, method_val == ANALYZE_DIFF || method_val == ANALYZE_LAST_LINE);
    // Callback na dropdownie
    obs_property_set_modified_callback(method, analyze_method_modified);
    return props;
}

static void fps_analyzer_update(void *data, obs_data_t *settings)
{
    struct fps_analyzer_filter *filter = (struct fps_analyzer_filter *)data;
    const char *path = obs_data_get_string(settings, "output_path");
    if (path) {
        strncpy(filter->output_path, path, sizeof(filter->output_path));
        filter->output_path[sizeof(filter->output_path)-1] = '\0';
    }
    filter->update_interval = obs_data_get_double(settings, "update_interval");
    if (filter->update_interval <= 0.0)
        filter->update_interval = 1.0;
    filter->clear_csv_on_start = obs_data_get_bool(settings, "clear_csv_on_start");
    filter->enable_tearing_detection = obs_data_get_bool(settings, "enable_tearing_detection");
    filter->tearing_sensitivity = obs_data_get_double(settings, "tearing_sensitivity");
    filter->analyze_method = (analyze_method_t)obs_data_get_int(settings, "analyze_method");
    filter->sensitivity = obs_data_get_double(settings, "sensitivity");
}



// Funkcja do budowania ścieżki pliku TXT
static void build_txt_path(const char *output_path, char *txt_path, size_t txt_path_size) {
    if (output_path[0]) {
        strncpy(txt_path, output_path, txt_path_size);
        txt_path[txt_path_size-1] = '\0';
        if (strlen(txt_path) < 4 || strcasecmp(txt_path + strlen(txt_path) - 4, ".txt") != 0) {
            if (strlen(txt_path) + 4 < txt_path_size) {
                strcat(txt_path, ".txt");
            }
        }
    } else {
        strcpy(txt_path, "fps.txt");
    }
}

// Funkcja do budowania ścieżki pliku CSV
static void build_csv_path(const char *output_path, char *csv_path, size_t csv_path_size) {
    if (output_path[0]) {
        strncpy(csv_path, output_path, csv_path_size);
        csv_path[csv_path_size-1] = '\0';
        char *dot = strrchr(csv_path, '.');
        if (dot) {
            strcpy(dot, ".csv");
        } else {
            strcat(csv_path, ".csv");
        }
    } else {
        strcpy(csv_path, "fps.csv");
    }
}

static void keep_last_n_lines(const char *csv_path, int n) {
    FILE *f = fopen(csv_path, "r");
    if (!f) return;
    char *lines[FPS_CSV_HISTORY_LIMIT+1];
    int count = 0;
    char buf[256];
    while (fgets(buf, sizeof(buf), f)) {
        lines[count] = strdup(buf);
        count++;
        if (count > n) {
            free(lines[0]);
            memmove(lines, lines+1, sizeof(char*)*n);
            count = n;
        }
    }
    fclose(f);
    f = fopen(csv_path, "w");
    if (f) {
        for (int i = 0; i < count; ++i) {
            fputs(lines[i], f);
            free(lines[i]);
        }
        fclose(f);
    } else {
        for (int i = 0; i < count; ++i) free(lines[i]);
    }
}

static void fps_analyzer_get_defaults(obs_data_t *settings)
{
    obs_data_set_default_bool(settings, "clear_csv_on_start", true);
    obs_data_set_default_bool(settings, "enable_tearing_detection", true);
    obs_data_set_default_double(settings, "tearing_sensitivity", 1.0);
    obs_data_set_default_int(settings, "analyze_method", ANALYZE_LAST_LINE);
    obs_data_set_default_double(settings, "sensitivity", 0.1);
}

struct obs_source_info fps_analyzer_filter_info = {
    .id = "fps_analyzer_filter",
    .type = OBS_SOURCE_TYPE_FILTER,
    .output_flags = OBS_SOURCE_VIDEO,
    .get_name = fps_analyzer_get_name,
    .create = fps_analyzer_create,
    .destroy = fps_analyzer_destroy,
    .get_defaults = fps_analyzer_get_defaults,
    .get_properties = fps_analyzer_properties,
    .update = fps_analyzer_update,
    .video_tick = fps_analyzer_video_tick,
    .filter_video = fps_analyzer_filter_video,
}; 