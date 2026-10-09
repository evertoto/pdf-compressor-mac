#include <gtk/gtk.h>
#include <spawn.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glob.h>

typedef struct {
    char *input_path;
    char *setting;
    int request_id;
    long long est_size;
} EstimationRequest;

typedef struct {
    int request_id;
    long long est_size;
} EstimateResult;

typedef struct {
    char *input_path;
    char *output_path;
    char *setting;
    char *result_msg;
} CompressionRequest;

static GtkWidget *file_name_label;
static GtkWidget *file_size_label;
static GtkWidget *estimate_value_label;
static GtkWidget *reduction_value_label;
static GtkWidget *compress_button;
static GtkWidget *progress_bar;
static GtkWidget *progress_label;
static GtkWidget *level_buttons[3];

static char *selected_file = NULL;
static long long selected_size = 0;
static int selected_level = 1;
static int estimation_id = 0;
static gboolean compressing = FALSE;
static gboolean shutting_down = FALSE;

static const char *pdf_settings[3] = {"/screen", "/ebook", "/prepress"};
static const char *level_names[3] = {"Low", "Medium", "High"};

static char *output_dir = NULL;

extern char **environ;

typedef void (*ProgressCallback)(int current_page, int total_pages);

static int progress_current_page = 0;
static int progress_total_pages = -1;
static gboolean progress_has_real_data = FALSE;
static gint progress_value = 0;
static guint progress_timeout_id = 0;

static char *cached_estim_path = NULL;
static char *cached_estim_input = NULL;
static long long cached_estim_mtime = 0;
static long long cached_estim_size = 0;
static int cached_estim_level = -1;

static gboolean estimation_running = FALSE;
static char *current_estim_input = NULL;
static int current_estim_level = -1;
static gboolean pending_compression = FALSE;

static char *find_gs(void)
{
    gchar *found = g_find_program_in_path("gs");
    if (found) {
        if (access(found, X_OK) == 0) {
            char *dup = g_strdup(found);
            g_free(found);
            return dup;
        }
        g_free(found);
    }

    const char *candidates[] = {
        "/opt/homebrew/bin/gs",
        "/usr/local/bin/gs",
    };
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); ++i) {
        if (access(candidates[i], X_OK) == 0) {
            return g_strdup(candidates[i]);
        }
    }

    glob_t gl;
    if (glob("/opt/homebrew/Cellar/ghostscript/*/bin/gs", 0, NULL, &gl) == 0 && gl.gl_pathc > 0) {
        const char *path = gl.gl_pathv[0];
        if (access(path, X_OK) == 0) {
            char *dup = g_strdup(path);
            globfree(&gl);
            return dup;
        }
    }
    globfree(&gl);

    return NULL;
}

static void parse_progress_line(const char *line, size_t line_len, ProgressCallback progress_cb)
{
    gboolean has_page = FALSE;
    for (size_t i = 0; i < line_len - 3; i++) {
        if ((line[i] == 'p' || line[i] == 'P') &&
            (line[i+1] == 'a' || line[i+1] == 'A') &&
            (line[i+2] == 'g' || line[i+2] == 'G') &&
            (line[i+3] == 'e' || line[i+3] == 'E')) {
            has_page = TRUE;
            break;
        }
    }
    if (!has_page) return;

    int num1 = -1, num2 = -1;
    size_t len = line_len;
    size_t i = 0;

    while (i < len && !(line[i] >= '0' && line[i] <= '9')) i++;
    if (i < len) {
        num1 = 0;
        while (i < len && line[i] >= '0' && line[i] <= '9') {
            num1 = num1 * 10 + (line[i] - '0');
            i++;
        }
    }

    while (i < len && !(line[i] >= '0' && line[i] <= '9')) i++;
    if (i < len) {
        num2 = 0;
        while (i < len && line[i] >= '0' && line[i] <= '9') {
            num2 = num2 * 10 + (line[i] - '0');
            i++;
        }
    }

    if (num1 > 0 && num2 > 0 && progress_cb) {
        progress_cb(num1, num2);
    }
}

static int run_gs(const char *gs_path,
                  const char *input,
                  const char *output,
                  const char *setting,
                  char *errbuf,
                  size_t errbuf_size,
                  ProgressCallback progress_cb)
{
    char setting_arg[256];
    snprintf(setting_arg, sizeof(setting_arg), "-dPDFSETTINGS=%s", setting);
    char output_arg[4096];
    snprintf(output_arg, sizeof(output_arg), "-sOutputFile=%s", output);

    gint argc = 8;
    char **argv = (char **)malloc((argc + 1) * sizeof(char *));
    argv[0] = (char *)gs_path;
    argv[1] = (char *)"-sDEVICE=pdfwrite";
    argv[2] = (char *)"-dCompatibilityLevel=1.4";
    argv[3] = (char *)setting_arg;
    argv[4] = (char *)"-dNOPAUSE";
    argv[5] = (char *)"-dBATCH";
    argv[6] = (char *)output_arg;
    argv[7] = (char *)input;
    argv[8] = NULL;

    int combined_pipe[2];
    if (pipe(combined_pipe) != 0) {
        snprintf(errbuf, errbuf_size, "pipe failed: %s", strerror(errno));
        free(argv);
        return -1;
    }

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, combined_pipe[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, combined_pipe[1], STDERR_FILENO);
    posix_spawn_file_actions_addclose(&actions, combined_pipe[0]);
    posix_spawn_file_actions_addclose(&actions, combined_pipe[1]);

    pid_t pid;
    int status;
    int err = posix_spawnp(&pid, gs_path, &actions, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&actions);
    free(argv);
    close(combined_pipe[1]);

    if (err != 0) {
        snprintf(errbuf, errbuf_size, "Failed to start gs: %s", strerror(err));
        close(combined_pipe[0]);
        return -1;
    }

    char *line_buf = NULL;
    size_t line_len = 0;
    size_t line_cap = 0;
    char *all_output = NULL;
    size_t output_len = 0;
    size_t output_cap = 0;

    while (TRUE) {
        char *chunk = malloc(4096);
        ssize_t n = read(combined_pipe[0], chunk, 4096);
        free(chunk);
        if (n <= 0) break;

        for (ssize_t i = 0; i < n; i++) {
            if (line_cap <= line_len) {
                line_cap = line_len * 2 + 1024;
                line_buf = (char *)realloc(line_buf, line_cap);
            }
            line_buf[line_len++] = chunk[i];

            if (output_cap <= output_len) {
                output_cap = output_len * 2 + 4096;
                all_output = (char *)realloc(all_output, output_cap);
            }
            all_output[output_len++] = chunk[i];

            if (chunk[i] == '\n') {
                line_buf[line_len] = '\0';
                parse_progress_line(line_buf, line_len - 1, progress_cb);
                line_len = 0;
            }
        }
    }

    if (line_len > 0) {
        line_buf[line_len] = '\0';
        parse_progress_line(line_buf, line_len, progress_cb);
    }

    if (all_output && output_len > 0) {
        if (output_len > errbuf_size - 1) {
            memmove(all_output, all_output + output_len - (errbuf_size - 1), errbuf_size - 1);
            output_len = errbuf_size - 1;
        }
        all_output[output_len] = '\0';
        snprintf(errbuf, errbuf_size, "%s", all_output);
    } else {
        errbuf[0] = '\0';
    }

    free(line_buf);
    free(all_output);
    close(combined_pipe[0]);

    if (waitpid(pid, &status, 0) == -1) {
        snprintf(errbuf, errbuf_size, "waitpid failed: %s", strerror(errno));
        return -1;
    }

    if (!WIFEXITED(status)) {
        snprintf(errbuf, errbuf_size, "gs terminated by signal");
        return -1;
    }

    int exit_code = WEXITSTATUS(status);
    if (exit_code != 0) {
        if (errbuf[0] == '\0') {
            snprintf(errbuf, errbuf_size, "gs exited with status %d", exit_code);
        }
        return -1;
    }

    return 0;
}

static int run_ghostscript(const char *input, const char *output, const char *setting, char **err_msg_out, ProgressCallback progress_cb)
{
    char *gs_path = find_gs();
    if (!gs_path) {
        *err_msg_out = g_strdup("Ghostscript (gs) not found. Install with: brew install ghostscript");
        return -1;
    }
    char errbuf[1024] = {0};
    int rc = run_gs(gs_path, input, output, setting, errbuf, sizeof(errbuf), progress_cb);
    g_free(gs_path);
    if (rc != 0) {
        *err_msg_out = g_strdup(errbuf);
        return -1;
    }
    *err_msg_out = NULL;
    return 0;
}

static gboolean update_estimate_ui(gpointer data)
{
    if (shutting_down)
        return G_SOURCE_REMOVE;

    EstimateResult *res = (EstimateResult *)data;

    if (res->request_id != estimation_id)
        return G_SOURCE_REMOVE;

    long long est_size = res->est_size;

    double est_mb = est_size / (1024.0 * 1024.0);
    char buf[256];
    snprintf(buf, sizeof(buf), "%.2f MB", est_mb);
    gtk_label_set_text(GTK_LABEL(estimate_value_label), buf);

    if (selected_size > 0) {
        double ratio = (double)est_size / (double)selected_size;
        double perc = (1.0 - ratio) * 100.0;
        snprintf(buf, sizeof(buf), "%.0f%%", perc);
        gtk_label_set_text(GTK_LABEL(reduction_value_label), buf);
    } else {
        gtk_label_set_text(GTK_LABEL(reduction_value_label), "N/A");
    }

    return G_SOURCE_REMOVE;
}

static gboolean set_label_error(gpointer msg) {
    if (shutting_down)
        return G_SOURCE_REMOVE;
    gtk_label_set_text(GTK_LABEL(estimate_value_label), (char *)msg);
    return G_SOURCE_REMOVE;
}

static void on_progress(int current_page, int total_pages)
{
    progress_current_page = current_page;
    progress_total_pages = total_pages;
    progress_has_real_data = TRUE;
}

static gboolean update_compress_ui_from_cache(gpointer data)
{
    if (shutting_down) {
        g_free(data);
        return G_SOURCE_REMOVE;
    }

    char *output_path = (char *)data;

    compressing = FALSE;
    if (progress_timeout_id) {
        g_source_remove(progress_timeout_id);
        progress_timeout_id = 0;
    }
    progress_value = 100;
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(progress_bar), 1.0);
    gtk_label_set_text(GTK_LABEL(progress_label), "100%");
    gtk_widget_set_sensitive(compress_button, TRUE);
    gtk_button_set_label(GTK_BUTTON(compress_button), "Compress");

    GtkWidget *dialog = gtk_message_dialog_new(NULL, GTK_DIALOG_MODAL,
                                               GTK_MESSAGE_INFO, GTK_BUTTONS_OK,
                                               "Compression complete (from cache):\n%s", output_path);
    g_signal_connect(dialog, "response",
        G_CALLBACK(gtk_widget_destroy), dialog);
    gtk_widget_show_all(dialog);
    g_free(output_path);

    return G_SOURCE_REMOVE;
}

static gboolean start_compression_from_cache_idle(gpointer user_data)
{
    (void)user_data;
    if (!cached_estim_path) return G_SOURCE_REMOVE;

    char *basename = g_path_get_basename(selected_file);
    char *name = NULL;
    if (g_str_has_suffix(basename, ".pdf")) {
        size_t len = strlen(basename) - 4;
        name = g_strndup(basename, len);
    } else {
        name = g_strdup(basename);
    }
    g_free(basename);

    char *out_path = NULL;
    int i = 0;
    GString *suffix = g_string_new("");
    do {
        g_string_truncate(suffix, 0);
        g_string_append(suffix, output_dir);
        g_string_append_c(suffix, '/');
        g_string_append(suffix, name);
        g_string_append(suffix, "_compressed");
        if (i > 0) {
            g_string_append_printf(suffix, "_%d", i);
        }
        g_string_append(suffix, ".pdf");
        out_path = g_strdup(suffix->str);
        i++;
    } while (g_file_test(out_path, G_FILE_TEST_EXISTS));
    g_string_free(suffix, TRUE);
    g_free(name);

    GFile *src = g_file_new_for_path(cached_estim_path);
    GFile *dst = g_file_new_for_path(out_path);
    GError *err = NULL;
    if (g_file_copy(src, dst, G_FILE_COPY_NONE, 0, NULL, NULL, &err)) {
        unlink(cached_estim_path);
        g_free(cached_estim_path);
        cached_estim_path = NULL;
        g_free(cached_estim_input);
        cached_estim_input = NULL;
        cached_estim_mtime = 0;
        cached_estim_size = 0;
        cached_estim_level = -1;
        g_object_unref(src);
        g_object_unref(dst);
        g_idle_add_full(G_PRIORITY_DEFAULT, update_compress_ui_from_cache, g_strdup(out_path), g_free);
        g_free(out_path);
        return G_SOURCE_REMOVE;
    } else {
        g_error_free(err);
        g_object_unref(src);
        g_object_unref(dst);
        g_free(out_path);
    }

    compressing = FALSE;
    if (progress_timeout_id) {
        g_source_remove(progress_timeout_id);
        progress_timeout_id = 0;
    }
    progress_value = 0;
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(progress_bar), 0.0);
    gtk_label_set_text(GTK_LABEL(progress_label), "0%");
    gtk_widget_set_sensitive(compress_button, TRUE);
    gtk_button_set_label(GTK_BUTTON(compress_button), "Compress");

    GtkWidget *dialog = gtk_message_dialog_new(NULL, GTK_DIALOG_MODAL,
                                               GTK_MESSAGE_ERROR, GTK_BUTTONS_OK,
                                               "Failed to copy cached estimation result.");
    g_signal_connect(dialog, "response",
        G_CALLBACK(gtk_widget_destroy), dialog);
    gtk_widget_show_all(dialog);

    return G_SOURCE_REMOVE;
}

static gpointer estimation_thread_func(gpointer user_data)
{
    EstimationRequest *req = (EstimationRequest *)user_data;

    char *tmpl = g_strdup("/tmp/pdfcompressorXXXXXX");
    int fd = mkstemp(tmpl);
    if (fd < 0) {
        char *msg = g_strdup("Falha ao create file tempor&acirc;ary.");
        g_idle_add_full(G_PRIORITY_DEFAULT, set_label_error, msg, g_free);
        g_free(tmpl);
        g_free(req->input_path);
        g_free(req->setting);
        g_free(req);
        return NULL;
    }
    close(fd);
    unlink(tmpl);

    char *err_msg = NULL;
    if (run_ghostscript(req->input_path, tmpl, req->setting, &err_msg, NULL) != 0) {
        char *msg = g_strdup_printf("Erro na similation: %s", err_msg ? err_msg : "unknown");
        g_idle_add_full(G_PRIORITY_DEFAULT, set_label_error, msg, g_free);
        g_free(err_msg);
        unlink(tmpl);
        g_free(tmpl);
        g_free(req->input_path);
        g_free(req->setting);
        g_free(req);
        return NULL;
    }

    struct stat st;
    if (stat(tmpl, &st) != 0) {
        char *msg = g_strdup("Falha ao obtain size of file tempor&acirc;ary.");
        g_idle_add_full(G_PRIORITY_DEFAULT, set_label_error, msg, g_free);
        unlink(tmpl);
        g_free(tmpl);
        g_free(req->input_path);
        g_free(req->setting);
        g_free(req);
        return NULL;
    }

    req->est_size = st.st_size;

    cached_estim_path = tmpl;
    cached_estim_input = g_strdup(req->input_path);
    cached_estim_mtime = st.st_mtime;
    cached_estim_size = st.st_size;
    cached_estim_level = selected_level;

    EstimateResult *result = g_new0(EstimateResult, 1);
    result->request_id = req->request_id;
    result->est_size = req->est_size;

    g_free(req->input_path);
    g_free(req->setting);
    g_free(req);

    estimation_running = FALSE;
    current_estim_input = NULL;
    current_estim_level = -1;

    if (pending_compression) {
        pending_compression = FALSE;
        g_idle_add_full(G_PRIORITY_DEFAULT, start_compression_from_cache_idle, NULL, NULL);
    }

    g_idle_add_full(G_PRIORITY_DEFAULT, update_estimate_ui, result, g_free);
    return NULL;
}

static void start_estimation(void)
{
    if (!selected_file)
        return;

    estimation_id++;
    estimation_running = TRUE;
    current_estim_input = selected_file;
    current_estim_level = selected_level;
    pending_compression = FALSE;

    EstimationRequest *req = (EstimationRequest *)malloc(sizeof(EstimationRequest));
    req->input_path = g_strdup(selected_file);
    req->setting = g_strdup(pdf_settings[selected_level]);
    req->request_id = estimation_id;
    req->est_size = 0;

    gtk_label_set_text(GTK_LABEL(estimate_value_label), "Calculating...");
    gtk_label_set_text(GTK_LABEL(reduction_value_label), "N/A");

    GThread *thr = g_thread_new("estimate", estimation_thread_func, req);
    g_thread_unref(thr);
}

static gboolean update_progress(gpointer user_data)
{
    if (shutting_down || !compressing) {
        progress_timeout_id = 0;
        return G_SOURCE_REMOVE;
    }

    if (progress_has_real_data && progress_total_pages > 0) {
        double frac = (double)progress_current_page / (double)progress_total_pages;
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(progress_bar), frac);
        char buf[64];
        snprintf(buf, sizeof(buf), "%d%%", (int)(frac * 100));
        gtk_label_set_text(GTK_LABEL(progress_label), buf);
    } else {
        gtk_progress_bar_pulse(GTK_PROGRESS_BAR(progress_bar));
        gtk_label_set_text(GTK_LABEL(progress_label), "Compressing...");
    }

    return G_SOURCE_CONTINUE;
}

static gboolean update_compress_ui(gpointer data)
{
    if (shutting_down) {
        CompressionRequest *tmp = (CompressionRequest *)data;
        g_free(tmp->result_msg);
        g_free(tmp);
        return G_SOURCE_REMOVE;
    }

    CompressionRequest *req = (CompressionRequest *)data;

    char *result_msg = req->result_msg;
    g_free(req->output_path);
    g_free(req->setting);
    g_free(req->input_path);
    g_free(req);

    compressing = FALSE;
    if (progress_timeout_id) {
        g_source_remove(progress_timeout_id);
        progress_timeout_id = 0;
    }
    progress_value = 0;
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(progress_bar), 0.0);
    gtk_label_set_text(GTK_LABEL(progress_label), "0%");
    gtk_widget_set_sensitive(compress_button, TRUE);
    gtk_button_set_label(GTK_BUTTON(compress_button), "Compress");

    if (result_msg) {
        GtkWidget *dialog = gtk_message_dialog_new(NULL, GTK_DIALOG_MODAL,
                                                   GTK_MESSAGE_INFO, GTK_BUTTONS_OK,
                                                   "%s", result_msg);
        g_signal_connect(dialog, "response",
            G_CALLBACK(gtk_widget_destroy), dialog);
        gtk_widget_show_all(dialog);
        g_free(result_msg);
    }

    return G_SOURCE_REMOVE;
}

static void on_window_destroy(GtkWidget *widget, gpointer user_data)
{
    (void)widget;
    (void)user_data;

    shutting_down = TRUE;
    estimation_id++;
    if (cached_estim_path) {
        unlink(cached_estim_path);
        g_free(cached_estim_path);
        cached_estim_path = NULL;
    }
    gtk_main_quit();
}

static gpointer compression_thread_func(gpointer user_data)
{
    CompressionRequest *req = (CompressionRequest *)user_data;

    if (cached_estim_path &&
        cached_estim_input == req->input_path &&
        cached_estim_level == selected_level) {
        struct stat st;
        if (stat(req->input_path, &st) == 0 &&
            st.st_mtime == cached_estim_mtime &&
            st.st_size == cached_estim_size) {
            GFile *src = g_file_new_for_path(cached_estim_path);
            GFile *dst = g_file_new_for_path(req->output_path);
            GError *err = NULL;
            if (g_file_copy(src, dst, G_FILE_COPY_NONE, 0, NULL, NULL, &err)) {
                unlink(cached_estim_path);
                g_free(cached_estim_path);
                cached_estim_path = NULL;
                g_free(cached_estim_input);
                cached_estim_input = NULL;
                cached_estim_mtime = 0;
                cached_estim_size = 0;
                cached_estim_level = -1;

                g_object_unref(src);
                g_object_unref(dst);

                g_idle_add_full(G_PRIORITY_DEFAULT, update_compress_ui_from_cache, g_strdup(req->output_path), g_free);
                g_free(req->input_path);
                g_free(req->output_path);
                g_free(req->setting);
                g_free(req);
                return NULL;
            } else {
                g_error_free(err);
                g_object_unref(src);
                g_object_unref(dst);
            }
        }
    }

    char *err_msg = NULL;
    int rc = run_ghostscript(req->input_path, req->output_path, req->setting, &err_msg, on_progress);

    CompressionRequest *result = (CompressionRequest *)malloc(sizeof(CompressionRequest));
    result->input_path = NULL;
    result->setting = NULL;
    result->output_path = g_strdup(req->output_path);
    if (rc == 0) {
        result->result_msg = g_strdup_printf("Compression complete:\n%s", req->output_path);
    } else {
        result->result_msg = g_strdup_printf("Compression failed: %s", err_msg ? err_msg : "unknown");
        g_free(err_msg);
    }
    g_free(req->input_path);
    g_free(req->output_path);
    g_free(req->setting);
    g_free(req);

    g_idle_add(update_compress_ui, result);
    return NULL;
}

static void on_compress_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    (void)user_data;
    if (!selected_file) {
        GtkWidget *dialog = gtk_message_dialog_new(NULL, GTK_DIALOG_MODAL,
                                                   GTK_MESSAGE_ERROR, GTK_BUTTONS_OK,
                                                   "No PDF selected.");
        g_signal_connect(dialog, "response",
            G_CALLBACK(gtk_widget_destroy), dialog);
        gtk_widget_show_all(dialog);
        return;
    }

    if (compressing)
        return;

    if (estimation_running && current_estim_input == selected_file && current_estim_level == selected_level) {
        pending_compression = TRUE;
        return;
    }

    char *basename = g_path_get_basename(selected_file);
    char *name = NULL;
    if (g_str_has_suffix(basename, ".pdf")) {
        size_t len = strlen(basename) - 4;
        name = g_strndup(basename, len);
    } else {
        name = g_strdup(basename);
    }
    g_free(basename);

    char *out_path = NULL;
    int i = 0;
    GString *suffix = g_string_new("");
    do {
        g_string_truncate(suffix, 0);
        g_string_append(suffix, output_dir);
        g_string_append_c(suffix, '/');
        g_string_append(suffix, name);
        g_string_append(suffix, "_compressed");
        if (i > 0) {
            g_string_append_printf(suffix, "_%d", i);
        }
        g_string_append(suffix, ".pdf");
        out_path = g_strdup(suffix->str);
        i++;
    } while (g_file_test(out_path, G_FILE_TEST_EXISTS));
    g_string_free(suffix, TRUE);
    g_free(name);

    compressing = TRUE;
    progress_value = 0;
    progress_current_page = 0;
    progress_total_pages = -1;
    progress_has_real_data = FALSE;
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(progress_bar), 0.0);
    gtk_label_set_text(GTK_LABEL(progress_label), "0%");
    if (progress_timeout_id) g_source_remove(progress_timeout_id);
    progress_timeout_id = g_timeout_add(100, update_progress, NULL);
    gtk_widget_set_sensitive(compress_button, FALSE);
    gtk_button_set_label(GTK_BUTTON(compress_button), "Compressing...");

    CompressionRequest *req = (CompressionRequest *)malloc(sizeof(CompressionRequest));
    req->input_path = g_strdup(selected_file);
    req->output_path = g_strdup(out_path);
    req->setting = g_strdup(pdf_settings[selected_level]);
    req->result_msg = NULL;
    g_free(out_path);

    GThread *thr = g_thread_new("compress", compression_thread_func, req);
    g_thread_unref(thr);
}

static void on_level_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    int level = (int)(long)user_data;
    selected_level = level;
    for (int i = 0; i < 3; i++) {
        if (i == level) {
            GdkColor c;
            gdk_color_parse("#6490C3", &c);
            gtk_widget_modify_bg(level_buttons[i], GTK_STATE_NORMAL, &c);
            gtk_button_set_label(GTK_BUTTON(level_buttons[i]), level_names[i]);
        } else {
            GdkColor c;
            gdk_color_parse("#5980A5", &c);
            gtk_widget_modify_bg(level_buttons[i], GTK_STATE_NORMAL, &c);
            gtk_button_set_label(GTK_BUTTON(level_buttons[i]), level_names[i]);
        }
    }

    if (cached_estim_path) {
        unlink(cached_estim_path);
        g_free(cached_estim_path);
        cached_estim_path = NULL;
    }
    cached_estim_input = NULL;
    cached_estim_mtime = 0;
    cached_estim_size = 0;
    cached_estim_level = -1;
    pending_compression = FALSE;

    start_estimation();
}

static void on_select_file_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    (void)user_data;
    GtkFileChooserNative *chooser = gtk_file_chooser_native_new(
        "Select PDF", NULL, GTK_FILE_CHOOSER_ACTION_OPEN, "Open", "Cancel");

    GtkFileFilter *filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, "PDF files");
    gtk_file_filter_add_mime_type(filter, "application/pdf");
    gtk_file_filter_add_pattern(filter, "*.pdf");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(chooser), filter);

    gint resp = gtk_native_dialog_run(GTK_NATIVE_DIALOG(chooser));
    if (resp == GTK_RESPONSE_ACCEPT) {
        char *path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(chooser));

        if (selected_file)
            g_free(selected_file);
        selected_file = g_strdup(path);

        struct stat st;
        if (stat(selected_file, &st) == 0) {
            selected_size = st.st_size;
            double mb = selected_size / (1024.0 * 1024.0);
            char buf[256];
            snprintf(buf, sizeof(buf), "%.2f MB", mb);
            gtk_label_set_text(GTK_LABEL(file_size_label), buf);

            char *basename = g_path_get_basename(selected_file);
            gtk_label_set_text(GTK_LABEL(file_name_label), basename);
            g_free(basename);

            if (cached_estim_path) {
                unlink(cached_estim_path);
                g_free(cached_estim_path);
                cached_estim_path = NULL;
            }
            cached_estim_input = NULL;
            cached_estim_mtime = 0;
            cached_estim_size = 0;
            cached_estim_level = -1;
            pending_compression = FALSE;

            start_estimation();
        } else {
            gtk_label_set_text(GTK_LABEL(file_size_label), "Failed to read file.");
        }
        g_free(path);
    }
    gtk_native_dialog_destroy((GtkNativeDialog *)chooser);
}

static void build_ui(GtkWidget *window)
{
    gtk_window_set_title(GTK_WINDOW(window), "PDF Compressor");
    gtk_window_set_default_size(GTK_WINDOW(window), 400, 380);
    gtk_window_set_resizable(GTK_WINDOW(window), FALSE);
    g_signal_connect(window, "destroy", G_CALLBACK(on_window_destroy), NULL);

    GdkColor bg_color;
    gdk_color_parse("#FFFFFF", &bg_color);
    gtk_widget_modify_bg(window, GTK_STATE_NORMAL, &bg_color);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(window), vbox);

    GtkWidget *file_area = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_box_pack_start(GTK_BOX(vbox), file_area, TRUE, TRUE, 0);

    file_name_label = gtk_label_new("No PDF selected");
    gtk_label_set_xalign(GTK_LABEL(file_name_label), 0.5);
    gtk_box_pack_start(GTK_BOX(file_area), file_name_label, TRUE, TRUE, 0);
    gtk_label_set_markup(GTK_LABEL(file_name_label), "<span foreground='white' size='140'>No PDF selected</span>");

    file_size_label = gtk_label_new("Size: N/A");
    gtk_label_set_xalign(GTK_LABEL(file_size_label), 0.5);
    gtk_box_pack_start(GTK_BOX(file_area), file_size_label, TRUE, TRUE, 0);
    gtk_label_set_markup(GTK_LABEL(file_size_label), "<span foreground='#B4C2CC' size='110'>Size: N/A</span>");

    GtkWidget *select_btn = gtk_button_new_with_label("Select PDF");
    g_signal_connect(select_btn, "clicked", G_CALLBACK(on_select_file_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(vbox), select_btn, TRUE, TRUE, 0);
    GdkColor btn_color;
    gdk_color_parse("#E0E0E0", &btn_color);
    gtk_widget_modify_bg(select_btn, GTK_STATE_NORMAL, &btn_color);

    GtkWidget *level_area = gtk_box_new(GTK_ORIENTATION_VERTICAL, 20);
    gtk_box_pack_start(GTK_BOX(vbox), level_area, TRUE, TRUE, 0);

    GtkWidget *level_title = gtk_label_new("LEVEL OF COMPRESSION");
    gtk_label_set_xalign(GTK_LABEL(level_title), 0.5);
    gtk_box_pack_start(GTK_BOX(level_area), level_title, TRUE, TRUE, 0);
    gtk_label_set_markup(GTK_LABEL(level_title), "<span foreground='#DCCDF0' size='130' weight='bold'>LEVEL OF COMPRESSION</span>");

    GtkWidget *level_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
    gtk_box_pack_start(GTK_BOX(level_area), level_hbox, TRUE, TRUE, 0);

    for (int i = 0; i < 3; i++) {
        level_buttons[i] = gtk_button_new_with_label(level_names[i]);
        g_signal_connect(level_buttons[i], "clicked", G_CALLBACK(on_level_clicked), GINT_TO_POINTER(i));
        gtk_box_pack_start(GTK_BOX(level_hbox), level_buttons[i], TRUE, TRUE, 0);
        if (i == selected_level) {
            gdk_color_parse("#4687B5", &bg_color);
            gtk_widget_modify_bg(level_buttons[i], GTK_STATE_NORMAL, &bg_color);
        } else {
            gdk_color_parse("#E8E8E8", &bg_color);
            gtk_widget_modify_bg(level_buttons[i], GTK_STATE_NORMAL, &bg_color);
        }
    }

    GtkWidget *results_area = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16);
    gtk_box_pack_start(GTK_BOX(vbox), results_area, TRUE, TRUE, 0);

    GtkWidget *estimate_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_box_pack_start(GTK_BOX(results_area), estimate_box, TRUE, TRUE, 0);

    GtkWidget *estimate_lbl = gtk_label_new("Estimate");
    gtk_label_set_xalign(GTK_LABEL(estimate_lbl), 0.5);
    gtk_box_pack_start(GTK_BOX(estimate_box), estimate_lbl, TRUE, TRUE, 0);
    gtk_label_set_markup(GTK_LABEL(estimate_lbl), "<span foreground='#AACDBE' size='100'>Estimate</span>");

    estimate_value_label = gtk_label_new("N/A");
    gtk_label_set_xalign(GTK_LABEL(estimate_value_label), 0.5);
    gtk_box_pack_start(GTK_BOX(estimate_box), estimate_value_label, TRUE, TRUE, 0);
    gtk_label_set_markup(GTK_LABEL(estimate_value_label), "<span foreground='white' size='160' weight='bold'>N/A</span>");

    GtkWidget *reduction_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_box_pack_start(GTK_BOX(results_area), reduction_box, TRUE, TRUE, 0);

    GtkWidget *reduction_lbl = gtk_label_new("Reduction");
    gtk_label_set_xalign(GTK_LABEL(reduction_lbl), 0.5);
    gtk_box_pack_start(GTK_BOX(reduction_box), reduction_lbl, TRUE, TRUE, 0);
    gtk_label_set_markup(GTK_LABEL(reduction_lbl), "<span foreground='#AACDBE' size='100'>Reduction</span>");

    reduction_value_label = gtk_label_new("N/A");
    gtk_label_set_xalign(GTK_LABEL(reduction_value_label), 0.5);
    gtk_box_pack_start(GTK_BOX(reduction_box), reduction_value_label, TRUE, TRUE, 0);
    gtk_label_set_markup(GTK_LABEL(reduction_value_label), "<span foreground='white' size='160' weight='bold'>N/A</span>");

    GtkWidget *sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start(GTK_BOX(vbox), sep, TRUE, TRUE, 0);

    GtkWidget *button_area = gtk_box_new(GTK_ORIENTATION_VERTICAL, 24);
    gtk_box_pack_start(GTK_BOX(vbox), button_area, TRUE, TRUE, 0);

    GtkWidget *progress_area = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_box_pack_start(GTK_BOX(vbox), progress_area, TRUE, TRUE, 0);

    progress_label = gtk_label_new("0%");
    gtk_label_set_xalign(GTK_LABEL(progress_label), 0.5);
    gtk_box_pack_start(GTK_BOX(progress_area), progress_label, TRUE, TRUE, 0);
    gtk_label_set_markup(GTK_LABEL(progress_label), "<span foreground='#B4C2CC' size='100'>0%</span>");

    progress_bar = gtk_progress_bar_new();
    gtk_box_pack_start(GTK_BOX(progress_area), progress_bar, TRUE, TRUE, 0);
    gtk_widget_set_sensitive(progress_bar, FALSE);

    compress_button = gtk_button_new_with_label("Compress");
    g_signal_connect(compress_button, "clicked", G_CALLBACK(on_compress_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(button_area), compress_button, TRUE, TRUE, 0);
    gdk_color_parse("#4687B5", &bg_color);
    gtk_widget_modify_bg(compress_button, GTK_STATE_NORMAL, &bg_color);

    gtk_widget_show_all(window);
}

int main(int argc, char *argv[])
{
    const char *home = g_get_home_dir();
    output_dir = g_build_filename(home, "pdfcompressor", "compressed", NULL);
    g_mkdir_with_parents(output_dir, 0755);

    if (argc > 1) {
        const char *input_path = argv[1];
        int level = 1;
        if (argc > 2 && strcmp(argv[2], "--level") == 0 && argc > 3) {
            const char *lvl = argv[3];
            if (strcmp(lvl, "low") == 0) level = 0;
            else if (strcmp(lvl, "medium") == 0) level = 1;
            else if (strcmp(lvl, "high") == 0) level = 2;
            else {
                fprintf(stderr, "Unknown level: %s\n", lvl);
                return 1;
            }
        }

        char *basename = g_path_get_basename(input_path);
        char *name = NULL;
        if (g_str_has_suffix(basename, ".pdf")) {
            size_t len = strlen(basename) - 4;
            name = g_strndup(basename, len);
        } else {
            name = g_strdup(basename);
        }
        g_free(basename);

        char *out_path = NULL;
        int i = 0;
        GString *suffix = g_string_new("");
        do {
            g_string_truncate(suffix, 0);
            g_string_append(suffix, output_dir);
            g_string_append_c(suffix, '/');
            g_string_append(suffix, name);
            g_string_append(suffix, "_compressed");
            if (i > 0) {
                g_string_append_printf(suffix, "_%d", i);
            }
            g_string_append(suffix, ".pdf");
            out_path = g_strdup(suffix->str);
            i++;
        } while (g_file_test(out_path, G_FILE_TEST_EXISTS));
        g_string_free(suffix, TRUE);
        g_free(name);

        char *err_msg = NULL;
        int rc = run_ghostscript(input_path, out_path, pdf_settings[level], &err_msg, NULL);
        if (rc == 0) {
            printf("Compression complete: %s\n", out_path);
        } else {
            fprintf(stderr, "Compression failed: %s\n", err_msg ? err_msg : "unknown");
            g_free(err_msg);
            g_free(out_path);
            return 1;
        }
        g_free(out_path);
        return 0;
    }

    gtk_init(&argc, &argv);

    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    build_ui(window);
    gtk_main();
    return 0;
}
