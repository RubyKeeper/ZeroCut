#include <gtk/gtk.h>
#include <gdk/gdkx.h>
#include <mpv/client.h>
#include <locale.h>
#include <libgen.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    double start;
    double end;
    gboolean has_explicit_end;
} Segment;

typedef struct {
    GtkWidget *window;
    GtkWidget *video_area;
    GtkWidget *timeline_area;
    GtkWidget *lbl_cur;
    GtkWidget *lbl_dur;
    GtkWidget *lbl_range;
    GtkWidget *btn_play;
    GtkWidget *btn_mute;
    GtkWidget *vol_scale;
    GtkListStore *seg_store;
    GtkWidget *seg_view;

    mpv_handle *mpv;
    GArray *segments;
    int active_seg_idx;

    char *current_file;
    char cli_path[4096];

    double duration;
    double current_pos;
    double volume;
    gboolean is_muted;
    gboolean is_seeking;
    gboolean is_paused;
} AppState;

static void format_time(double sec, char *buf, size_t len) {
    if (sec < 0) sec = 0;
    int hrs = (int)(sec / 3600);
    int mins = (int)(((int)sec % 3600) / 60);
    double rem = sec - (hrs * 3600 + mins * 60);
    snprintf(buf, len, "%02d:%02d:%06.3f", hrs, mins, rem);
}

static void redraw_timeline(AppState *app) {
    if (app->timeline_area) {
        gtk_widget_queue_draw(app->timeline_area);
    }
}

static void update_range_label(AppState *app) {
    char t_in[32], t_out[32], text[128];
    if (app->active_seg_idx >= 0 && app->active_seg_idx < (int)app->segments->len) {
        Segment s = g_array_index(app->segments, Segment, app->active_seg_idx);
        format_time(s.start, t_in, sizeof(t_in));
        if (s.has_explicit_end) {
            format_time(s.end, t_out, sizeof(t_out));
            snprintf(text, sizeof(text), "Сегмент #%d: %s → %s", app->active_seg_idx + 1, t_in, t_out);
        } else {
            snprintf(text, sizeof(text), "Сегмент #%d: %s → Конец", app->active_seg_idx + 1, t_in);
        }
    } else {
        snprintf(text, sizeof(text), "Нет активного сегмента");
    }
    gtk_label_set_text(GTK_LABEL(app->lbl_range), text);
    redraw_timeline(app);
}

/* Синхронизация списка GtkListStore с массивом segments и выделение активного */
static void sync_segment_list(AppState *app) {
    gtk_list_store_clear(app->seg_store);
    for (guint i = 0; i < app->segments->len; i++) {
        Segment s = g_array_index(app->segments, Segment, i);
        char t1[32], t2[32], label[128];
        format_time(s.start, t1, sizeof(t1));
        if (s.has_explicit_end) {
            format_time(s.end, t2, sizeof(t2));
            snprintf(label, sizeof(label), "#%u: %s → %s", i + 1, t1, t2);
        } else {
            snprintf(label, sizeof(label), "#%u: %s → Конец", i + 1, t1);
        }

        GtkTreeIter iter;
        gtk_list_store_append(app->seg_store, &iter);
        gtk_list_store_set(app->seg_store, &iter, 0, label, -1);

        if ((int)i == app->active_seg_idx) {
            GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(app->seg_view));
            gtk_tree_selection_select_iter(sel, &iter);
        }
    }
    update_range_label(app);
}

static void show_message(GtkWindow *parent, GtkMessageType type, const char *title, const char *msg) {
    GtkWidget *dialog = gtk_message_dialog_new(parent, GTK_DIALOG_MODAL, type, GTK_BUTTONS_OK, "%s", title);
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog), "%s", msg);
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
}

static void seek_to_time(AppState *app, double target) {
    if (!app->current_file || !app->mpv || app->duration <= 0.0) return;
    if (target < 0.0) target = 0.0;
    if (target > app->duration) target = app->duration;

    app->current_pos = target;
    char buf[32];
    format_time(target, buf, sizeof(buf));
    gtk_label_set_text(GTK_LABEL(app->lbl_cur), buf);
    redraw_timeline(app);

    char pos_str[32];
    snprintf(pos_str, sizeof(pos_str), "%.3f", target);
    const char *cmd[] = {"seek", pos_str, "absolute", "exact", NULL};
    mpv_command(app->mpv, cmd);
}

/* Прыжок строго по ключевым кадрам через mpv */
static void seek_keyframe(AppState *app, gboolean forward) {
    if (!app->current_file || !app->mpv) return;

    if (!app->is_paused) {
        int pause_flag = 1;
        mpv_set_property(app->mpv, "pause", MPV_FORMAT_FLAG, &pause_flag);
        app->is_paused = TRUE;
        gtk_button_set_label(GTK_BUTTON(app->btn_play), "▶ Play");
    }

    const char *offset = forward ? "0.1" : "-0.1";
    const char *cmd[] = {"seek", offset, "relative+keyframes", NULL};
    mpv_command(app->mpv, cmd);
}

static void on_prev_kf_clicked(GtkButton *btn, AppState *app) {
    seek_keyframe(app, FALSE);
}

static void on_next_kf_clicked(GtkButton *btn, AppState *app) {
    seek_keyframe(app, TRUE);
}

/* Переход к началу или концу активного сегмента */
static void on_jump_seg_start(GtkButton *btn, AppState *app) {
    if (app->active_seg_idx >= 0 && app->active_seg_idx < (int)app->segments->len) {
        Segment s = g_array_index(app->segments, Segment, app->active_seg_idx);
        seek_to_time(app, s.start);
    }
}

static void on_jump_seg_end(GtkButton *btn, AppState *app) {
    if (app->active_seg_idx >= 0 && app->active_seg_idx < (int)app->segments->len) {
        Segment s = g_array_index(app->segments, Segment, app->active_seg_idx);
        seek_to_time(app, s.end);
    }
}

/* Управление громкостью */
static void apply_volume(AppState *app, double new_vol) {
    if (new_vol < 0.0) new_vol = 0.0;
    if (new_vol > 130.0) new_vol = 130.0;
    app->volume = new_vol;

    if (app->mpv) {
        mpv_set_property(app->mpv, "volume", MPV_FORMAT_DOUBLE, &app->volume);
    }
    if (app->vol_scale) {
        gtk_range_set_value(GTK_RANGE(app->vol_scale), app->volume);
    }
}

static void on_volume_changed(GtkRange *range, AppState *app) {
    double val = gtk_range_get_value(range);
    app->volume = val;
    if (app->mpv) {
        mpv_set_property(app->mpv, "volume", MPV_FORMAT_DOUBLE, &app->volume);
    }
}

static void toggle_mute(AppState *app) {
    app->is_muted = !app->is_muted;
    int flag = app->is_muted ? 1 : 0;
    if (app->mpv) {
        mpv_set_property(app->mpv, "mute", MPV_FORMAT_FLAG, &flag);
    }
    gtk_button_set_label(GTK_BUTTON(app->btn_mute), app->is_muted ? "🔇" : "🔊");
}

static void on_mute_clicked(GtkButton *btn, AppState *app) {
    toggle_mute(app);
}

static void toggle_play(AppState *app) {
    if (!app->current_file) return;
    app->is_paused = !app->is_paused;
    int flag = app->is_paused ? 1 : 0;
    mpv_set_property(app->mpv, "pause", MPV_FORMAT_FLAG, &flag);
    gtk_button_set_label(GTK_BUTTON(app->btn_play), app->is_paused ? "▶ Play" : "⏸ Pause");
}

static void on_play_clicked(GtkButton *btn, AppState *app) {
    toggle_play(app);
}

/* Клик левой кнопкой по экрану видео -> Play / Pause */
static gboolean on_video_press(GtkWidget *w, GdkEventButton *ev, AppState *app) {
    if (ev->button == 1) {
        toggle_play(app);
        return TRUE;
    }
    return FALSE;
}

static gboolean on_video_scroll(GtkWidget *w, GdkEventScroll *ev, AppState *app) {
    if (ev->direction == GDK_SCROLL_UP) {
        apply_volume(app, app->volume + 5.0);
        return TRUE;
    } else if (ev->direction == GDK_SCROLL_DOWN) {
        apply_volume(app, app->volume - 5.0);
        return TRUE;
    }
    return FALSE;
}

/* Отрисовка кастомного таймлайна */
static gboolean on_timeline_draw(GtkWidget *widget, cairo_t *cr, AppState *app) {
    GtkAllocation alloc;
    gtk_widget_get_allocation(widget, &alloc);
    double w = alloc.width;
    double h = alloc.height;

    cairo_set_source_rgb(cr, 0.14, 0.14, 0.16);
    cairo_rectangle(cr, 0, 0, w, h);
    cairo_fill(cr);

    cairo_set_source_rgb(cr, 0.28, 0.28, 0.32);
    cairo_set_line_width(cr, 1.0);
    cairo_rectangle(cr, 0.5, 0.5, w - 1.0, h - 1.0);
    cairo_stroke(cr);

    if (app->duration <= 0.0) {
        return FALSE;
    }

    for (guint i = 0; i < app->segments->len; i++) {
        Segment s = g_array_index(app->segments, Segment, i);
        double x1 = (s.start / app->duration) * w;
        double x2 = (s.end / app->duration) * w;
        if (x2 < x1) { double tmp = x1; x1 = x2; x2 = tmp; }
        double seg_w = (x2 - x1 < 2.0) ? 2.0 : (x2 - x1);

        gboolean is_active = ((int)i == app->active_seg_idx);
        if (is_active) {
            cairo_set_source_rgba(cr, 0.22, 0.85, 0.42, 0.55);
        } else {
            cairo_set_source_rgba(cr, 0.16, 0.55, 0.28, 0.35);
        }
        cairo_rectangle(cr, x1, 2, seg_w, h - 4);
        cairo_fill(cr);

        if (is_active) {
            cairo_set_source_rgb(cr, 0.35, 1.0, 0.50);
            cairo_set_line_width(cr, 2.0);
        } else {
            cairo_set_source_rgb(cr, 0.25, 0.70, 0.38);
            cairo_set_line_width(cr, 1.0);
        }
        cairo_rectangle(cr, x1, 2, seg_w, h - 4);
        cairo_stroke(cr);

        /* Маркеры начала (зелёный) и конца (оранжевый) для активного сегмента */
        if (is_active) {
            cairo_set_source_rgb(cr, 0.20, 0.95, 0.40);
            cairo_set_line_width(cr, 2.0);
            cairo_move_to(cr, x1, 0);
            cairo_line_to(cr, x1, h);
            cairo_stroke(cr);

            cairo_set_source_rgb(cr, 0.95, 0.45, 0.25);
            cairo_move_to(cr, x2, 0);
            cairo_line_to(cr, x2, h);
            cairo_stroke(cr);
        }
    }

    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.15);
    cairo_set_line_width(cr, 1.0);
    int ticks = 40;
    for (int i = 1; i < ticks; i++) {
        double tx = (w / ticks) * i;
        double tick_h = (i % 5 == 0) ? 8.0 : 4.0;
        cairo_move_to(cr, tx, 0);
        cairo_line_to(cr, tx, tick_h);
        cairo_stroke(cr);
    }

    double cur_x = (app->current_pos / app->duration) * w;
    cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
    cairo_set_line_width(cr, 2.0);
    cairo_move_to(cr, cur_x, 0);
    cairo_line_to(cr, cur_x, h);
    cairo_stroke(cr);

    return FALSE;
}

static void seek_to_mouse_x(AppState *app, double x) {
    if (app->duration <= 0.0) return;
    GtkAllocation alloc;
    gtk_widget_get_allocation(app->timeline_area, &alloc);
    if (alloc.width <= 0) return;

    double ratio = x / (double)alloc.width;
    if (ratio < 0.0) ratio = 0.0;
    if (ratio > 1.0) ratio = 1.0;

    seek_to_time(app, ratio * app->duration);
}

static gboolean on_timeline_press(GtkWidget *w, GdkEventButton *ev, AppState *app) {
    if (ev->button == 1 && app->current_file) {
        app->is_seeking = TRUE;
        seek_to_mouse_x(app, ev->x);
        return TRUE;
    }
    return FALSE;
}

static gboolean on_timeline_motion(GtkWidget *w, GdkEventMotion *ev, AppState *app) {
    if (app->is_seeking && app->current_file) {
        seek_to_mouse_x(app, ev->x);
        return TRUE;
    }
    return FALSE;
}

static gboolean on_timeline_release(GtkWidget *w, GdkEventButton *ev, AppState *app) {
    if (ev->button == 1) {
        app->is_seeking = FALSE;
        return TRUE;
    }
    return FALSE;
}

static void load_video(AppState *app, const char *filepath) {
    g_free(app->current_file);
    app->current_file = g_strdup(filepath);

    g_array_set_size(app->segments, 0);
    app->active_seg_idx = -1;
    sync_segment_list(app);

    app->duration = 0.0;
    app->current_pos = 0.0;
    redraw_timeline(app);

    char *basename = g_path_get_basename(filepath);
    char title[512];
    snprintf(title, sizeof(title), "ZeroCut — %s", basename);
    gtk_window_set_title(GTK_WINDOW(app->window), title);
    g_free(basename);

    const char *cmd[] = {"loadfile", filepath, NULL};
    mpv_command(app->mpv, cmd);

    int pause = 1;
    mpv_set_property(app->mpv, "pause", MPV_FORMAT_FLAG, &pause);
    app->is_paused = TRUE;
    gtk_button_set_label(GTK_BUTTON(app->btn_play), "▶ Play");
}

static void on_open_clicked(GtkButton *btn, AppState *app) {
    GtkWidget *dialog = gtk_file_chooser_dialog_new(
        "Выберите видео/аудио файл", GTK_WINDOW(app->window),
        GTK_FILE_CHOOSER_ACTION_OPEN,
        "_Отмена", GTK_RESPONSE_CANCEL,
        "_Открыть", GTK_RESPONSE_ACCEPT, NULL
    );

    GtkFileFilter *filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, "Медиафайлы (*.wmv, *.asf, *.wma, *.mp4, *.mkv, *.avi)");
    gtk_file_filter_add_pattern(filter, "*.wmv");
    gtk_file_filter_add_pattern(filter, "*.WMV");
    gtk_file_filter_add_pattern(filter, "*.wma");
    gtk_file_filter_add_pattern(filter, "*.WMA");
    gtk_file_filter_add_pattern(filter, "*.asf");
    gtk_file_filter_add_pattern(filter, "*.ASF");
    gtk_file_filter_add_pattern(filter, "*.mp4");
    gtk_file_filter_add_pattern(filter, "*.MP4");
    gtk_file_filter_add_pattern(filter, "*.mkv");
    gtk_file_filter_add_pattern(filter, "*.MKV");
    gtk_file_filter_add_pattern(filter, "*.avi");
    gtk_file_filter_add_pattern(filter, "*.AVI");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), filter);

    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        char *filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
        load_video(app, filename);
        g_free(filename);
    }
    gtk_widget_destroy(dialog);
}

/* Поиск сегмента, в который попадает текущая позиция прокрутки */
static int find_segment_at_pos(AppState *app, double pos) {
    for (guint i = 0; i < app->segments->len; i++) {
        Segment s = g_array_index(app->segments, Segment, i);
        if (pos >= s.start - 0.001 && pos <= s.end + 0.001) {
            return (int)i;
        }
    }
    return -1;
}

/* Умная кнопка Начало [I] */
static void on_set_in(GtkButton *btn, AppState *app) {
    if (!app->current_file || app->duration <= 0.0) return;
    double pos = app->current_pos;
    const double eps = 0.05;

    /* 1. Если сегментов ещё нет -> создаём первый [pos .. duration] */
    if (app->segments->len == 0) {
        Segment s = {pos, app->duration, FALSE};
        g_array_append_val(app->segments, s);
        app->active_seg_idx = 0;
        sync_segment_list(app);
        return;
    }

    /* 2. Если курсор на границе или после конца закрытого активного сегмента -> открываем новый сегмент */
    if (app->active_seg_idx >= 0 && app->active_seg_idx < (int)app->segments->len) {
        Segment *act = &g_array_index(app->segments, Segment, app->active_seg_idx);
        if (act->has_explicit_end && pos >= act->end - eps) {
            Segment s = {pos, app->duration, FALSE};
            g_array_append_val(app->segments, s);
            app->active_seg_idx = (int)app->segments->len - 1;
            sync_segment_list(app);
            return;
        }
    }

    /* 3. Если курсор внутри существующего сегмента -> редактируем его начало */
    int hover_idx = find_segment_at_pos(app, pos);
    if (hover_idx >= 0) {
        Segment *seg = &g_array_index(app->segments, Segment, hover_idx);
        if (pos < seg->end) {
            seg->start = pos;
            app->active_seg_idx = hover_idx;
            sync_segment_list(app);
            return;
        }
    }

    /* 4. Если в списке выбран сегмент и курсор левее его конца -> меняем начало выбранного */
    if (app->active_seg_idx >= 0 && app->active_seg_idx < (int)app->segments->len) {
        Segment *act = &g_array_index(app->segments, Segment, app->active_seg_idx);
        if (pos < act->end) {
            act->start = pos;
            sync_segment_list(app);
            return;
        }
    }

    /* 5. В остальных случаях открываем новый сегмент */
    Segment s = {pos, app->duration, FALSE};
    g_array_append_val(app->segments, s);
    app->active_seg_idx = (int)app->segments->len - 1;
    sync_segment_list(app);
}

/* Умная кнопка Конец [O] */
static void on_set_out(GtkButton *btn, AppState *app) {
    if (!app->current_file || app->duration <= 0.0) return;
    double pos = app->current_pos;

    /* 1. Если сегментов ещё нет -> создаём [0.0 .. pos] */
    if (app->segments->len == 0) {
        if (pos <= 0.0) return;
        Segment s = {0.0, pos, TRUE};
        g_array_append_val(app->segments, s);
        app->active_seg_idx = 0;
        sync_segment_list(app);
        return;
    }

    /* 2. Если курсор внутри какого-то сегмента и правее его начала -> закрываем/меняем этот сегмент */
    int hover_idx = find_segment_at_pos(app, pos);
    if (hover_idx >= 0) {
        Segment *seg = &g_array_index(app->segments, Segment, hover_idx);
        if (pos > seg->start) {
            seg->end = pos;
            seg->has_explicit_end = TRUE;
            app->active_seg_idx = hover_idx;
            sync_segment_list(app);
            return;
        }
    }

    /* 3. Редактируем текущий активный сегмент, если pos > start */
    if (app->active_seg_idx >= 0 && app->active_seg_idx < (int)app->segments->len) {
        Segment *act = &g_array_index(app->segments, Segment, app->active_seg_idx);
        if (pos > act->start) {
            act->end = pos;
            act->has_explicit_end = TRUE;
            sync_segment_list(app);
            return;
        }
    }
}

/* Кнопка принудительного создания нового сегмента */
static void on_new_segment(GtkButton *btn, AppState *app) {
    if (!app->current_file || app->duration <= 0.0) return;
    Segment s = {app->current_pos, app->duration, FALSE};
    g_array_append_val(app->segments, s);
    app->active_seg_idx = (int)app->segments->len - 1;
    sync_segment_list(app);
}

static void on_del_segment(GtkButton *btn, AppState *app) {
    if (app->active_seg_idx >= 0 && app->active_seg_idx < (int)app->segments->len) {
        g_array_remove_index(app->segments, app->active_seg_idx);
        if (app->segments->len == 0) {
            app->active_seg_idx = -1;
        } else if (app->active_seg_idx >= (int)app->segments->len) {
            app->active_seg_idx = (int)app->segments->len - 1;
        }
        sync_segment_list(app);
    }
}

/* Выбор сегмента кликом в списке */
static void on_segment_selection_changed(GtkTreeSelection *sel, AppState *app) {
    GtkTreeIter iter;
    GtkTreeModel *model;
    if (gtk_tree_selection_get_selected(sel, &model, &iter)) {
        GtkTreePath *path = gtk_tree_model_get_path(model, &iter);
        app->active_seg_idx = gtk_tree_path_get_indices(path)[0];
        gtk_tree_path_free(path);
        update_range_label(app);
    }
}

/* Двойной клик по сегменту в списке -> перемотка на его начало */
static void on_segment_row_activated(GtkTreeView *tree_view, GtkTreePath *path,
                                     GtkTreeViewColumn *column, AppState *app) {
    int idx = gtk_tree_path_get_indices(path)[0];
    if (idx >= 0 && idx < (int)app->segments->len) {
        app->active_seg_idx = idx;
        Segment s = g_array_index(app->segments, Segment, idx);
        seek_to_time(app, s.start);
        update_range_label(app);
    }
}

static void on_export_clicked(GtkButton *btn, AppState *app) {
    if (!app->current_file) return;

    if (access(app->cli_path, X_OK) != 0) {
        char err[4200];
        snprintf(err, sizeof(err), "Исполняемый файл не найден по пути:\n%s", app->cli_path);
        show_message(GTK_WINDOW(app->window), GTK_MESSAGE_ERROR, "CLI-модуль не найден", err);
        return;
    }

    GArray *export_segs = g_array_new(FALSE, FALSE, sizeof(Segment));
    if (app->segments->len > 0) {
        g_array_append_vals(export_segs, app->segments->data, app->segments->len);
    } else {
        Segment s = {0.0, app->duration, TRUE};
        g_array_append_val(export_segs, s);
    }

    GtkWidget *dialog = gtk_file_chooser_dialog_new(
        "Сохранить обрезанный файл", GTK_WINDOW(app->window),
        GTK_FILE_CHOOSER_ACTION_SAVE,
        "_Отмена", GTK_RESPONSE_CANCEL,
        "_Сохранить", GTK_RESPONSE_ACCEPT, NULL
    );
    gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(dialog), TRUE);

    char *dir = g_path_get_dirname(app->current_file);
    char *base = g_path_get_basename(app->current_file);
    char default_name[512];
    snprintf(default_name, sizeof(default_name), "cut_%s", base);
    gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(dialog), dir);
    gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dialog), default_name);
    g_free(dir);
    g_free(base);

    if (gtk_dialog_run(GTK_DIALOG(dialog)) != GTK_RESPONSE_ACCEPT) {
        gtk_widget_destroy(dialog);
        g_array_free(export_segs, TRUE);
        return;
    }
    char *out_file = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
    gtk_widget_destroy(dialog);

    GPtrArray *argv = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(argv, g_strdup(app->cli_path));
    g_ptr_array_add(argv, g_strdup("-i"));
    g_ptr_array_add(argv, g_strdup(app->current_file));
    g_ptr_array_add(argv, g_strdup("-o"));
    g_ptr_array_add(argv, g_strdup(out_file));

    for (guint i = 0; i < export_segs->len; i++) {
        Segment s = g_array_index(export_segs, Segment, i);
        double dur = s.end - s.start;
        g_ptr_array_add(argv, g_strdup("-start"));
        g_ptr_array_add(argv, g_strdup_printf("%.3f", s.start));
        g_ptr_array_add(argv, g_strdup("-dur"));
        g_ptr_array_add(argv, g_strdup_printf("%.3f", dur));
    }
    g_ptr_array_add(argv, NULL);

    gchar *std_out = NULL, *std_err = NULL;
    gint exit_status = 0;
    GError *error = NULL;

    gboolean ok = g_spawn_sync(
        NULL, (gchar **)argv->pdata, NULL, G_SPAWN_DEFAULT,
        NULL, NULL, &std_out, &std_err, &exit_status, &error
    );

    if (!ok) {
        show_message(GTK_WINDOW(app->window), GTK_MESSAGE_ERROR, "Ошибка запуска CLI", error->message);
        g_error_free(error);
    } else if (exit_status == 0 && g_file_test(out_file, G_FILE_TEST_EXISTS)) {
        show_message(GTK_WINDOW(app->window), GTK_MESSAGE_INFO, "Готово", out_file);
    } else {
        show_message(GTK_WINDOW(app->window), GTK_MESSAGE_ERROR, "Ошибка при экспорте",
                     (std_err && *std_err) ? std_err : (std_out ? std_out : "Неизвестная ошибка"));
    }

    g_free(std_out);
    g_free(std_err);
    g_free(out_file);
    g_ptr_array_free(argv, TRUE);
    g_array_free(export_segs, TRUE);
}

static gboolean on_key_press(GtkWidget *w, GdkEventKey *ev, AppState *app) {
    guint mods = ev->state & gtk_accelerator_get_default_mod_mask();
    gboolean is_kf_mod = (mods & GDK_SHIFT_MASK) || (mods & GDK_MOD1_MASK);

    switch (ev->keyval) {
        case GDK_KEY_space:
            toggle_play(app);
            return TRUE;
        case GDK_KEY_i:
        case GDK_KEY_I:
            on_set_in(NULL, app);
            return TRUE;
        case GDK_KEY_o:
        case GDK_KEY_O:
            on_set_out(NULL, app);
            return TRUE;
        case GDK_KEY_n:
        case GDK_KEY_N:
            on_new_segment(NULL, app);
            return TRUE;
        case GDK_KEY_bracketleft:
            on_jump_seg_start(NULL, app);
            return TRUE;
        case GDK_KEY_bracketright:
            on_jump_seg_end(NULL, app);
            return TRUE;
        case GDK_KEY_m:
        case GDK_KEY_M:
            toggle_mute(app);
            return TRUE;
        case GDK_KEY_Up:
            apply_volume(app, app->volume + 5.0);
            return TRUE;
        case GDK_KEY_Down:
            apply_volume(app, app->volume - 5.0);
            return TRUE;
        case GDK_KEY_Right:
            if (is_kf_mod) {
                seek_keyframe(app, TRUE);
            } else {
                const char *cmd[] = {"frame-step", NULL};
                mpv_command(app->mpv, cmd);
            }
            return TRUE;
        case GDK_KEY_Left:
            if (is_kf_mod) {
                seek_keyframe(app, FALSE);
            } else {
                const char *cmd[] = {"frame-back-step", NULL};
                mpv_command(app->mpv, cmd);
            }
            return TRUE;
    }
    return FALSE;
}

static gboolean poll_mpv_events(gpointer data) {
    AppState *app = (AppState *)data;
    while (app->mpv) {
        mpv_event *ev = mpv_wait_event(app->mpv, 0);
        if (ev->event_id == MPV_EVENT_NONE) break;
        if (ev->event_id == MPV_EVENT_PROPERTY_CHANGE) {
            mpv_event_property *prop = (mpv_event_property *)ev->data;
            if (strcmp(prop->name, "time-pos") == 0 && prop->format == MPV_FORMAT_DOUBLE) {
                if (!app->is_seeking) {
                    app->current_pos = *(double *)prop->data;
                    char buf[32];
                    format_time(app->current_pos, buf, sizeof(buf));
                    gtk_label_set_text(GTK_LABEL(app->lbl_cur), buf);
                    redraw_timeline(app);
                }
            } else if (strcmp(prop->name, "duration") == 0 && prop->format == MPV_FORMAT_DOUBLE) {
                app->duration = *(double *)prop->data;
                char buf[32];
                format_time(app->duration, buf, sizeof(buf));
                gtk_label_set_text(GTK_LABEL(app->lbl_dur), buf);
                gboolean updated = FALSE;
                for (guint i = 0; i < app->segments->len; i++) {
                    Segment *s = &g_array_index(app->segments, Segment, i);
                    if (!s->has_explicit_end) {
                        s->end = app->duration;
                        updated = TRUE;
                    }
                }
                if (updated) sync_segment_list(app);
            }
        }
    }
    return G_SOURCE_CONTINUE;
}

static void on_video_realize(GtkWidget *widget, AppState *app) {
    GdkWindow *gdk_win = gtk_widget_get_window(widget);
    int64_t wid = (int64_t)gdk_x11_window_get_xid(gdk_win);
    mpv_set_option(app->mpv, "wid", MPV_FORMAT_INT64, &wid);
    mpv_initialize(app->mpv);
    mpv_set_property(app->mpv, "volume", MPV_FORMAT_DOUBLE, &app->volume);
    mpv_observe_property(app->mpv, 0, "time-pos", MPV_FORMAT_DOUBLE);
    mpv_observe_property(app->mpv, 0, "duration", MPV_FORMAT_DOUBLE);
}

static void on_drag_data_received(GtkWidget *w, GdkDragContext *ctx, gint x, gint y,
                                  GtkSelectionData *data, guint info, guint time, AppState *app) {
    gchar **uris = gtk_selection_data_get_uris(data);
    if (uris && uris[0]) {
        char *path = g_filename_from_uri(uris[0], NULL, NULL);
        if (path) {
            load_video(app, path);
            g_free(path);
        }
        g_strfreev(uris);
    }
    gtk_drag_finish(ctx, TRUE, FALSE, time);
}

int main(int argc, char *argv[]) {
    gtk_init(&argc, &argv);
    g_object_set(gtk_settings_get_default(), "gtk-application-prefer-dark-theme", TRUE, NULL);
    setlocale(LC_NUMERIC, "C");

    AppState app = {0};
    app.segments = g_array_new(FALSE, FALSE, sizeof(Segment));
    app.active_seg_idx = -1;
    app.is_paused = TRUE;
    app.volume = 100.0;

    char exe_path[4096] = {0};
    ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
    if (len != -1) {
        exe_path[len] = '\0';
        char *dir = dirname(exe_path);
        snprintf(app.cli_path, sizeof(app.cli_path), "%s/bin/zerocut-cli", dir);
    }

    app.mpv = mpv_create();
    mpv_set_option_string(app.mpv, "keep-open", "yes");
    mpv_set_option_string(app.mpv, "input-default-bindings", "no");
    mpv_set_option_string(app.mpv, "input-vo-keyboard", "no");
    mpv_set_option_string(app.mpv, "input-cursor", "no");

    app.window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(app.window), "ZeroCut — Lossless Video/Audio Cutter");
    gtk_window_set_default_size(GTK_WINDOW(app.window), 1200, 720);
    g_signal_connect(app.window, "destroy", G_CALLBACK(gtk_main_quit), NULL);
    g_signal_connect(app.window, "key-press-event", G_CALLBACK(on_key_press), &app);

    gtk_drag_dest_set(app.window, GTK_DEST_DEFAULT_ALL, NULL, 0, GDK_ACTION_COPY);
    gtk_drag_dest_add_uri_targets(app.window);
    g_signal_connect(app.window, "drag-data-received", G_CALLBACK(on_drag_data_received), &app);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 8);
    gtk_container_add(GTK_CONTAINER(app.window), vbox);

    /* Верхняя панель */
    GtkWidget *top_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *btn_open = gtk_button_new_with_label("Открыть файл");
    gtk_widget_set_tooltip_text(btn_open, "Open media file");
    g_signal_connect(btn_open, "clicked", G_CALLBACK(on_open_clicked), &app);
    gtk_box_pack_start(GTK_BOX(top_bar), btn_open, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(top_bar), gtk_label_new("Клик по видео / Space: Play | ←→: кадр | Shift+←→: KF | I / O: границы | [ / ]: прыжок по сегменту"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), top_bar, FALSE, FALSE, 0);

    /* Центр: Видео + Список сегментов */
    GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    app.video_area = gtk_drawing_area_new();
    gtk_widget_set_size_request(app.video_area, 820, 480);
    gtk_widget_add_events(app.video_area, GDK_SCROLL_MASK | GDK_BUTTON_PRESS_MASK);
    g_signal_connect(app.video_area, "scroll-event", G_CALLBACK(on_video_scroll), &app);
    g_signal_connect(app.video_area, "button-press-event", G_CALLBACK(on_video_press), &app);

    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_data(css, "drawingarea#video { background-color: #101010; }", -1, NULL);
    gtk_widget_set_name(app.video_area, "video");
    gtk_style_context_add_provider(gtk_widget_get_style_context(app.video_area),
                                   GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);

    g_signal_connect(app.video_area, "realize", G_CALLBACK(on_video_realize), &app);
    gtk_paned_pack1(GTK_PANED(paned), app.video_area, TRUE, FALSE);

    GtkWidget *right_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_box_pack_start(GTK_BOX(right_box), gtk_label_new("Сегменты для экспорта:"), FALSE, FALSE, 0);

    app.seg_store = gtk_list_store_new(1, G_TYPE_STRING);
    app.seg_view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(app.seg_store));
    GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
    GtkTreeViewColumn *col = gtk_tree_view_column_new_with_attributes("Отрезки (2x клик — переход)", renderer, "text", 0, NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(app.seg_view), col);

    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(app.seg_view));
    g_signal_connect(sel, "changed", G_CALLBACK(on_segment_selection_changed), &app);
    g_signal_connect(app.seg_view, "row-activated", G_CALLBACK(on_segment_row_activated), &app);

    gtk_box_pack_start(GTK_BOX(right_box), app.seg_view, TRUE, TRUE, 0);

    GtkWidget *seg_btn_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    GtkWidget *btn_new_seg = gtk_button_new_with_label("+ Новый");
    gtk_widget_set_tooltip_text(btn_new_seg, "Create new segment at current position (N)");
    g_signal_connect(btn_new_seg, "clicked", G_CALLBACK(on_new_segment), &app);

    GtkWidget *btn_del = gtk_button_new_with_label("Удалить");
    gtk_widget_set_tooltip_text(btn_del, "Delete selected segment");
    g_signal_connect(btn_del, "clicked", G_CALLBACK(on_del_segment), &app);

    gtk_box_pack_start(GTK_BOX(seg_btn_box), btn_new_seg, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(seg_btn_box), btn_del, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(right_box), seg_btn_box, FALSE, FALSE, 0);

    gtk_paned_pack2(GTK_PANED(paned), right_box, FALSE, FALSE);
    gtk_box_pack_start(GTK_BOX(vbox), paned, TRUE, TRUE, 0);

    /* Кастомный визуальный таймлайн */
    GtkWidget *time_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    app.lbl_cur = gtk_label_new("00:00:00.000");

    app.timeline_area = gtk_drawing_area_new();
    gtk_widget_set_size_request(app.timeline_area, -1, 34);
    gtk_widget_add_events(app.timeline_area,
                          GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_POINTER_MOTION_MASK);
    g_signal_connect(app.timeline_area, "draw", G_CALLBACK(on_timeline_draw), &app);
    g_signal_connect(app.timeline_area, "button-press-event", G_CALLBACK(on_timeline_press), &app);
    g_signal_connect(app.timeline_area, "motion-notify-event", G_CALLBACK(on_timeline_motion), &app);
    g_signal_connect(app.timeline_area, "button-release-event", G_CALLBACK(on_timeline_release), &app);

    app.lbl_dur = gtk_label_new("00:00:00.000");

    gtk_box_pack_start(GTK_BOX(time_bar), app.lbl_cur, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(time_bar), app.timeline_area, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(time_bar), app.lbl_dur, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), time_bar, FALSE, FALSE, 0);

    /* Нижняя панель управления */
    GtkWidget *ctrl_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);

    GtkWidget *btn_prev_kf = gtk_button_new_with_label("⏮ KF");
    gtk_widget_set_tooltip_text(btn_prev_kf, "Previous keyframe (Shift + Left)");
    g_signal_connect(btn_prev_kf, "clicked", G_CALLBACK(on_prev_kf_clicked), &app);

    app.btn_play = gtk_button_new_with_label("▶ Play");
    gtk_widget_set_tooltip_text(app.btn_play, "Play / Pause (Space or Click video)");
    g_signal_connect(app.btn_play, "clicked", G_CALLBACK(on_play_clicked), &app);

    GtkWidget *btn_next_kf = gtk_button_new_with_label("KF ⏭");
    gtk_widget_set_tooltip_text(btn_next_kf, "Next keyframe (Shift + Right)");
    g_signal_connect(btn_next_kf, "clicked", G_CALLBACK(on_next_kf_clicked), &app);

    GtkWidget *btn_jump_in = gtk_button_new_with_label("[|◀");
    gtk_widget_set_tooltip_text(btn_jump_in, "Jump to active segment start ([)");
    g_signal_connect(btn_jump_in, "clicked", G_CALLBACK(on_jump_seg_start), &app);

    GtkWidget *btn_in = gtk_button_new_with_label("Начало [ I ]");
    gtk_widget_set_tooltip_text(btn_in, "Set segment start / Start new segment (I)");
    g_signal_connect(btn_in, "clicked", G_CALLBACK(on_set_in), &app);

    GtkWidget *btn_out = gtk_button_new_with_label("Конец [ O ]");
    gtk_widget_set_tooltip_text(btn_out, "Set segment end (O)");
    g_signal_connect(btn_out, "clicked", G_CALLBACK(on_set_out), &app);

    GtkWidget *btn_jump_out = gtk_button_new_with_label("▶|]");
    gtk_widget_set_tooltip_text(btn_jump_out, "Jump to active segment end (])");
    g_signal_connect(btn_jump_out, "clicked", G_CALLBACK(on_jump_seg_end), &app);

    app.lbl_range = gtk_label_new("Нет активного сегмента");

    /* Блок громкости */
    app.btn_mute = gtk_button_new_with_label("🔊");
    gtk_widget_set_tooltip_text(app.btn_mute, "Mute / Unmute (M)");
    g_signal_connect(app.btn_mute, "clicked", G_CALLBACK(on_mute_clicked), &app);

    app.vol_scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 130, 5);
    gtk_widget_set_size_request(app.vol_scale, 90, -1);
    gtk_scale_set_draw_value(GTK_SCALE(app.vol_scale), FALSE);
    gtk_range_set_value(GTK_RANGE(app.vol_scale), 100.0);
    gtk_widget_set_tooltip_text(app.vol_scale, "Volume (Up / Down or Mouse Wheel)");
    g_signal_connect(app.vol_scale, "value-changed", G_CALLBACK(on_volume_changed), &app);

    GtkWidget *btn_exp = gtk_button_new_with_label("Экспорт (Lossless)");
    gtk_widget_set_tooltip_text(btn_exp, "Export segments without re-encoding");
    g_signal_connect(btn_exp, "clicked", G_CALLBACK(on_export_clicked), &app);

    gtk_box_pack_start(GTK_BOX(ctrl_bar), btn_prev_kf, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(ctrl_bar), app.btn_play, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(ctrl_bar), btn_next_kf, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(ctrl_bar), btn_jump_in, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(ctrl_bar), btn_in, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(ctrl_bar), btn_out, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(ctrl_bar), btn_jump_out, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(ctrl_bar), app.lbl_range, FALSE, FALSE, 6);

    gtk_box_pack_end(GTK_BOX(ctrl_bar), btn_exp, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(ctrl_bar), app.vol_scale, FALSE, FALSE, 2);
    gtk_box_pack_end(GTK_BOX(ctrl_bar), app.btn_mute, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(vbox), ctrl_bar, FALSE, FALSE, 0);

    g_timeout_add(40, poll_mpv_events, &app);
    gtk_widget_show_all(app.window);

    if (argc > 1 && g_file_test(argv[1], G_FILE_TEST_EXISTS)) {
        load_video(&app, argv[1]);
    }

    gtk_main();
    mpv_terminate_destroy(app.mpv);
    g_array_free(app.segments, TRUE);
    return 0;
}
