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

static GtkWidget *file_label;
static GtkWidget *size_label;
static GtkWidget *estimate_label;
static GtkWidget *reduction_label;
static GtkWidget *level_combo;
static GtkWidget *compress_button;

static char *selected_file = NULL;
static long long selected_size = 0;
static int selected_level = 1;
static int estimation_id = 0;
static gboolean compressing = FALSE;
static gboolean shutting_down = FALSE;

static const char *pdf_settings[3] = {"/screen", "/ebook", "/prepress"};

static char *output_dir = NULL;

extern char **environ;

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

static int run_gs(const char *gs_path,
                  const char *input,
                  const char *output,
                  const char *setting,
                  char *errbuf,
                  size_t errbuf_size)
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

    int pipefd[2];
    if (pipe(pipefd) != 0) {
        snprintf(errbuf, errbuf_size, "pipe failed: %s", strerror(errno));
        free(argv);
        return -1;
    }

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDERR_FILENO);
    posix_spawn_file_actions_addclose(&actions, pipefd[0]);
    posix_spawn_file_actions_addclose(&actions, pipefd[1]);

    pid_t pid;
    int status;
    int err = posix_spawnp(&pid, gs_path, &actions, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&actions);
    free(argv);
    close(pipefd[1]);

    if (err != 0) {
        snprintf(errbuf, errbuf_size, "Failed to start gs: %s", strerror(err));
        close(pipefd[0]);
        return -1;
    }

    ssize_t total = 0;
    while (total < (ssize_t)errbuf_size - 1) {
        ssize_t r = read(pipefd[0], errbuf + total, errbuf_size - 1 - total);
        if (r <= 0) break;
        total += r;
    }
    errbuf[total] = '\0';
    close(pipefd[0]);

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

static int run_ghostscript(const char *input, const char *output, const char *setting, char **err_msg_out)
{
    char *gs_path = find_gs();
    if (!gs_path) {
        *err_msg_out = g_strdup("Ghostscript (gs) not found. Install with: brew install ghostscript");
        return -1;
    }
    char errbuf[1024] = {0};
    int rc = run_gs(gs_path, input, output, setting, errbuf, sizeof(errbuf));
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
    snprintf(buf, sizeof(buf), "Estimativa: %.2f MB", est_mb);
    gtk_label_set_text(GTK_LABEL(estimate_label), buf);

    if (selected_size > 0) {
        double ratio = (double)est_size / (double)selected_size;
        double perc = (1.0 - ratio) * 100.0;
        snprintf(buf, sizeof(buf), "Redução estimada: %.0f%%", perc);
    } else {
        snprintf(buf, sizeof(buf), "Redução estimada: N/A");
    }
    gtk_label_set_text(GTK_LABEL(reduction_label), buf);

    return G_SOURCE_REMOVE;
}

static gboolean set_label_error(gpointer msg) {
    if (shutting_down)
        return G_SOURCE_REMOVE;
    gtk_label_set_text(GTK_LABEL(estimate_label), (char *)msg);
    return G_SOURCE_REMOVE;
}

static gpointer estimation_thread_func(gpointer user_data)
{
    EstimationRequest *req = (EstimationRequest *)user_data;

    char *tmpl = g_strdup("/tmp/pdfcompressorXXXXXX");
    int fd = mkstemp(tmpl);
    if (fd < 0) {
        char *msg = g_strdup("Falha ao criar arquivo temporário.");
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
    if (run_ghostscript(req->input_path, tmpl, req->setting, &err_msg) != 0) {
        char *msg = g_strdup_printf("Erro na simulação: %s", err_msg ? err_msg : "unknown");
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
        char *msg = g_strdup("Falha ao obter tamanho do arquivo temporário.");
        g_idle_add_full(G_PRIORITY_DEFAULT, set_label_error, msg, g_free);
        unlink(tmpl);
        g_free(tmpl);
        g_free(req->input_path);
        g_free(req->setting);
        g_free(req);
        return NULL;
    }

    req->est_size = st.st_size;
    unlink(tmpl);
    g_free(tmpl);

    EstimateResult *result = g_new0(EstimateResult, 1);
    result->request_id = req->request_id;
    result->est_size = req->est_size;

    g_free(req->input_path);
    g_free(req->setting);
    g_free(req);

    g_idle_add_full(G_PRIORITY_DEFAULT, update_estimate_ui, result, g_free);
    return NULL;
}

static void start_estimation(void)
{
    if (!selected_file)
        return;

    estimation_id++;
    EstimationRequest *req = (EstimationRequest *)malloc(sizeof(EstimationRequest));
    req->input_path = g_strdup(selected_file);
    req->setting = g_strdup(pdf_settings[selected_level]);
    req->request_id = estimation_id;
    req->est_size = 0;

    gtk_label_set_text(GTK_LABEL(estimate_label), "Calculando estimativa...");
    gtk_label_set_text(GTK_LABEL(reduction_label), "Redução estimada: N/A");

    GThread *thr = g_thread_new("estimate", estimation_thread_func, req);
    g_thread_unref(thr);
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
    gtk_widget_set_sensitive(compress_button, TRUE);
    gtk_button_set_label(GTK_BUTTON(compress_button), "Comprimir PDF");

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
    gtk_main_quit();
}

static gpointer compression_thread_func(gpointer user_data)
{
    CompressionRequest *req = (CompressionRequest *)user_data;

    char *err_msg = NULL;
    int rc = run_ghostscript(req->input_path, req->output_path, req->setting, &err_msg);

    CompressionRequest *result = (CompressionRequest *)malloc(sizeof(CompressionRequest));
    result->input_path = NULL;
    result->setting = NULL;
    result->output_path = g_strdup(req->output_path);
    if (rc == 0) {
        result->result_msg = g_strdup_printf("Compressão concluída:\n%s", req->output_path);
    } else {
        result->result_msg = g_strdup_printf("Falha ao comprimir: %s", err_msg ? err_msg : "unknown");
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
                                                   "Nenhum PDF selecionado.");
        g_signal_connect(dialog, "response",
            G_CALLBACK(gtk_widget_destroy), dialog);
        gtk_widget_show_all(dialog);
        return;
    }

    if (compressing)
        return;

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
    gtk_widget_set_sensitive(compress_button, FALSE);
    gtk_button_set_label(GTK_BUTTON(compress_button), "Comprimindo...");

    CompressionRequest *req = (CompressionRequest *)malloc(sizeof(CompressionRequest));
    req->input_path = g_strdup(selected_file);
    req->output_path = g_strdup(out_path);
    req->setting = g_strdup(pdf_settings[selected_level]);
    req->result_msg = NULL;
    g_free(out_path);

    GThread *thr = g_thread_new("compress", compression_thread_func, req);
    g_thread_unref(thr);
}

static void on_level_changed(GtkComboBoxText *combo, gpointer user_data)
{
    (void)user_data;
    selected_level = gtk_combo_box_get_active(GTK_COMBO_BOX(combo));
    start_estimation();
}

static void on_select_file_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    (void)user_data;
    GtkFileChooserNative *chooser = gtk_file_chooser_native_new(
        "Selecionar PDF", NULL, GTK_FILE_CHOOSER_ACTION_OPEN, "Abrir", "Cancelar");

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
            snprintf(buf, sizeof(buf), "Tamanho atual: %.2f MB", mb);
            gtk_label_set_text(GTK_LABEL(size_label), buf);

            char *basename = g_path_get_basename(selected_file);
            snprintf(buf, sizeof(buf), "Arquivo selecionado: %s", basename);
            gtk_label_set_text(GTK_LABEL(file_label), buf);
            g_free(basename);

            start_estimation();
        } else {
            gtk_label_set_text(GTK_LABEL(size_label), "Falha ao ler arquivo.");
        }
        g_free(path);
    }
    gtk_native_dialog_destroy((GtkNativeDialog *)chooser);
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
                fprintf(stderr, "Nível desconhecido: %s\n", lvl);
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
        int rc = run_ghostscript(input_path, out_path, pdf_settings[level], &err_msg);
        if (rc == 0) {
            printf("Compressão concluída: %s\n", out_path);
        } else {
            fprintf(stderr, "Falha ao comprimir: %s\n", err_msg ? err_msg : "unknown");
            g_free(err_msg);
            g_free(out_path);
            return 1;
        }
        g_free(out_path);
        return 0;
    }

    gtk_init(&argc, &argv);

    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), "PDF Compressor");
    gtk_container_set_border_width(GTK_CONTAINER(window), 10);
    g_signal_connect(window, "destroy", G_CALLBACK(on_window_destroy), NULL);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 5);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 5);
    gtk_container_add(GTK_CONTAINER(window), grid);

    GtkWidget *select_btn = gtk_button_new_with_label("Selecionar PDF");
    g_signal_connect(select_btn, "clicked", G_CALLBACK(on_select_file_clicked), NULL);
    gtk_grid_attach(GTK_GRID(grid), select_btn, 0, 0, 1, 1);

    file_label = gtk_label_new("Nenhum arquivo selecionado.");
    gtk_grid_attach(GTK_GRID(grid), file_label, 1, 0, 1, 1);

    size_label = gtk_label_new("Tamanho atual: N/A");
    gtk_grid_attach(GTK_GRID(grid), size_label, 1, 1, 1, 1);

    level_combo = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(level_combo), "Baixa");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(level_combo), "Média");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(level_combo), "Alta");
    gtk_combo_box_set_active(GTK_COMBO_BOX(level_combo), 1);
    g_signal_connect(level_combo, "changed", G_CALLBACK(on_level_changed), NULL);
    gtk_grid_attach(GTK_GRID(grid), level_combo, 0, 2, 1, 1);

    estimate_label = gtk_label_new("Estimativa: N/A");
    gtk_grid_attach(GTK_GRID(grid), estimate_label, 1, 2, 1, 1);

    reduction_label = gtk_label_new("Redução estimada: N/A");
    gtk_grid_attach(GTK_GRID(grid), reduction_label, 1, 3, 1, 1);

    compress_button = gtk_button_new_with_label("Comprimir PDF");
    g_signal_connect(compress_button, "clicked", G_CALLBACK(on_compress_clicked), NULL);
    gtk_grid_attach(GTK_GRID(grid), compress_button, 0, 4, 1, 1);

    gtk_widget_show_all(window);
    gtk_main();
    return 0;
}
