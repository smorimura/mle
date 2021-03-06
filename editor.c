#include <unistd.h>
#include <signal.h>
#include <sys/time.h>
#include <termbox.h>
#include "uthash.h"
#include "utlist.h"
#include "mle.h"
#include "mlbuf.h"

static kbinding_t* _editor_get_kbinding_node(kbinding_t* parent, kinput_t* input, loop_context_t* loop_ctx, int is_peek, int* ret_again);
static int _editor_close_bview_inner(editor_t* editor, bview_t* bview, int* optret_num_closed);
static int _editor_prompt_input_submit(cmd_context_t* ctx);
static int _editor_prompt_input_complete(cmd_context_t* ctx);
static int _editor_prompt_yn_yes(cmd_context_t* ctx);
static int _editor_prompt_yn_no(cmd_context_t* ctx);
static int _editor_prompt_yna_all(cmd_context_t* ctx);
static int _editor_prompt_cancel(cmd_context_t* ctx);
static int _editor_menu_submit(cmd_context_t* ctx);
static int _editor_menu_cancel(cmd_context_t* ctx);
static int _editor_prompt_menu_up(cmd_context_t* ctx);
static int _editor_prompt_menu_down(cmd_context_t* ctx);
static int _editor_prompt_menu_page_up(cmd_context_t* ctx);
static int _editor_prompt_menu_page_down(cmd_context_t* ctx);
static int _editor_prompt_isearch_next(cmd_context_t* ctx);
static int _editor_prompt_isearch_prev(cmd_context_t* ctx);
static void _editor_startup(editor_t* editor);
static void _editor_loop(editor_t* editor, loop_context_t* loop_ctx);
static int _editor_maybe_toggle_macro(editor_t* editor, kinput_t* input);
static void _editor_resize(editor_t* editor, int w, int h);
static void _editor_draw_cursors(editor_t* editor, bview_t* bview);
static void _editor_get_user_input(editor_t* editor, cmd_context_t* ctx);
static void _editor_record_macro_input(kmacro_t* macro, kinput_t* input);
static cmd_funcref_t* _editor_get_command(editor_t* editor, cmd_context_t* ctx, kinput_t* opt_peek_input);
static void _editor_ingest_paste(editor_t* editor, cmd_context_t* ctx);
static cmd_func_t _editor_resolve_funcref(editor_t* editor, cmd_funcref_t* ref);
static int _editor_key_to_input(char* key, kinput_t* ret_input);
static void _editor_init_signal_handlers(editor_t* editor);
static void _editor_graceful_exit(int signum);
static void _editor_init_kmaps(editor_t* editor);
static void _editor_init_kmap(editor_t* editor, kmap_t** ret_kmap, char* name, cmd_funcref_t default_funcref, int allow_fallthru, kbinding_def_t* defs);
static void _editor_init_kmap_add_binding(editor_t* editor, kmap_t* kmap, char* cmd_name, char* key, char* static_param);
static int _editor_init_kmap_by_str(editor_t* editor, kmap_t** ret_kmap, char* str);
static int _editor_init_kmap_add_binding_to_trie(kbinding_t** parent, cmd_funcref_t* funcref, char* key_patt, char* static_param);
static int _editor_init_kmap_add_binding_by_str(editor_t* editor, kmap_t* kmap, char* str);
static void _editor_destroy_kmap(kmap_t* kmap, kbinding_t* parent);
static int _editor_add_macro_by_str(editor_t* editor, char* str);
static void _editor_init_syntaxes(editor_t* editor);
static void _editor_init_syntax(editor_t* editor, syntax_t** optret_syntax, char* name, char* path_pattern, srule_def_t* defs);
static int _editor_init_syntax_by_str(editor_t* editor, syntax_t** ret_syntax, char* str);
static void _editor_init_syntax_add_rule(syntax_t* syntax, srule_def_t def);
static int _editor_init_syntax_add_rule_by_str(syntax_t* syntax, char* str);
static void _editor_destroy_syntax_map(syntax_t* map);
static int _editor_init_from_rc(editor_t* editor, FILE* rc);
static int _editor_init_from_args(editor_t* editor, int argc, char** argv);
static void _editor_init_status(editor_t* editor);
static void _editor_init_bviews(editor_t* editor, int argc, char** argv);
static int _editor_init_or_deinit_commands(editor_t* editor, int is_deinit);
static int _editor_drain_async_procs(editor_t* editor);

// Init editor from args
int editor_init(editor_t* editor, int argc, char** argv) {
    int rv;
    FILE* rc;
    char *home_rc;
    rv = MLE_OK;
    do {
        // Set editor defaults
        editor->is_in_init = 1;
        editor->tab_width = MLE_DEFAULT_TAB_WIDTH;
        editor->tab_to_space = MLE_DEFAULT_TAB_TO_SPACE;
        editor->trim_paste = MLE_DEFAULT_TRIM_PASTE;
        editor->viewport_scope_x = -4;
        editor->viewport_scope_y = -4;
        editor->startup_linenum = -1;
        editor->color_col = -1;
        editor->exit_code = EXIT_SUCCESS;
        editor_set_macro_toggle_key(editor, MLE_DEFAULT_MACRO_TOGGLE_KEY);

        // Init signal handlers
        _editor_init_signal_handlers(editor);

        // Init kmaps
        _editor_init_kmaps(editor);

        // Init syntaxes
        _editor_init_syntaxes(editor);

        // Parse rc files
        home_rc = NULL;
        if (getenv("HOME")) {
            asprintf(&home_rc, "%s/%s", getenv("HOME"), ".mlerc");
            if (util_is_file(home_rc, "rb", &rc)) {
                rv = _editor_init_from_rc(editor, rc);
                fclose(rc);
            }
            free(home_rc);
        }
        if (rv != MLE_OK) break;
        if (util_is_file("/etc/mlerc", "rb", &rc)) {
            rv = _editor_init_from_rc(editor, rc);
            fclose(rc);
        }
        if (rv != MLE_OK) break;

        // Parse cli args
        rv = _editor_init_from_args(editor, argc, argv);
        if (rv != MLE_OK) break;

        // Init status bar
        _editor_init_status(editor);

        // Init bviews
        _editor_init_bviews(editor, argc, argv);

        // Init commands
        _editor_init_or_deinit_commands(editor, 0);
    } while(0);

    editor->is_in_init = 0;
    return rv;
}

// Run editor
int editor_run(editor_t* editor) {
    loop_context_t loop_ctx;
    memset(&loop_ctx, 0, sizeof(loop_context_t));
    _editor_resize(editor, -1, -1);
    _editor_startup(editor);
    _editor_loop(editor, &loop_ctx);
    return MLE_OK;
}

// Deinit editor
int editor_deinit(editor_t* editor) {
    bview_t* bview;
    bview_t* bview_tmp1;
    bview_t* bview_tmp2;
    kmap_t* kmap;
    kmap_t* kmap_tmp;
    kmacro_t* macro;
    kmacro_t* macro_tmp;
    cmd_funcref_t* funcref;
    cmd_funcref_t* funcref_tmp;
    _editor_init_or_deinit_commands(editor, 1);
    if (editor->status) bview_destroy(editor->status);
    CDL_FOREACH_SAFE2(editor->all_bviews, bview, bview_tmp1, bview_tmp2, all_prev, all_next) {
        CDL_DELETE2(editor->all_bviews, bview, all_prev, all_next);
        bview_destroy(bview);
    }
    HASH_ITER(hh, editor->kmap_map, kmap, kmap_tmp) {
        HASH_DEL(editor->kmap_map, kmap);
        _editor_destroy_kmap(kmap, kmap->bindings->children);
        free(kmap->bindings);
        free(kmap->name);
        free(kmap);
    }
    HASH_ITER(hh, editor->macro_map, macro, macro_tmp) {
        HASH_DEL(editor->macro_map, macro);
        if (macro->inputs) free(macro->inputs);
        if (macro->name) free(macro->name);
        free(macro);
    }
    HASH_ITER(hh, editor->func_map, funcref, funcref_tmp) {
        HASH_DEL(editor->func_map, funcref);
        if (funcref->name) free(funcref->name);
        free(funcref);
    }
    if (editor->macro_record) {
        if (editor->macro_record->inputs) free(editor->macro_record->inputs);
        free(editor->macro_record);
    }
    _editor_destroy_syntax_map(editor->syntax_map);
    if (editor->kmap_init_name) free(editor->kmap_init_name);
    if (editor->insertbuf) free(editor->insertbuf);
    if (editor->tty) fclose(editor->tty);
    return MLE_OK;
}

// Prompt user for input
int editor_prompt(editor_t* editor, char* prompt, editor_prompt_params_t* params, char** optret_answer) {
    bview_t* bview_tmp;
    loop_context_t loop_ctx;
    memset(&loop_ctx, 0, sizeof(loop_context_t));

    // Disallow nested prompts
    if (editor->prompt) {
        if (optret_answer) *optret_answer = NULL;
        return MLE_ERR;
    }

    // Init loop_ctx
    loop_ctx.invoker = editor->active;
    loop_ctx.should_exit = 0;
    loop_ctx.prompt_answer = NULL;

    // Init prompt
    editor_open_bview(editor, NULL, MLE_BVIEW_TYPE_PROMPT, NULL, 0, 1, 0, &editor->rect_prompt, NULL, &editor->prompt);
    if (params && params->prompt_cb) bview_add_listener(editor->prompt, params->prompt_cb, params->prompt_cb_udata);
    editor->prompt->prompt_str = prompt;
    bview_push_kmap(editor->prompt, params && params->kmap ? params->kmap : editor->kmap_prompt_input);

    // Insert data if present
    if (params && params->data && params->data_len > 0) {
        buffer_insert(editor->prompt->buffer, 0, params->data, params->data_len, NULL);
        mark_move_eol(editor->prompt->active_cursor->mark);
    }

    // Loop inside prompt
    _editor_loop(editor, &loop_ctx);

    // Set answer
    if (optret_answer) {
        *optret_answer = loop_ctx.prompt_answer;
    } else if (loop_ctx.prompt_answer) {
        free(loop_ctx.prompt_answer);
        loop_ctx.prompt_answer = NULL;
    }

    // Restore previous focus
    bview_tmp = editor->prompt;
    editor->prompt = NULL;
    editor_close_bview(editor, bview_tmp, NULL);
    editor_set_active(editor, loop_ctx.invoker);

    return MLE_OK;
}

// Open dialog menu
int editor_menu(editor_t* editor, cmd_func_t callback, char* opt_buf_data, int opt_buf_data_len, async_proc_t* opt_aproc, bview_t** optret_menu) {
    bview_t* menu;
    editor_open_bview(editor, NULL, MLE_BVIEW_TYPE_EDIT, NULL, 0, 1, 0, &editor->rect_edit, NULL, &menu);
    menu->is_menu = 1;
    menu->menu_callback = callback;
    bview_push_kmap(menu, editor->kmap_menu);
    if (opt_buf_data) {
        mark_insert_before(menu->active_cursor->mark, opt_buf_data, opt_buf_data_len);
    }
    if (opt_aproc) {
        async_proc_set_invoker(opt_aproc, menu);
    }
    if (optret_menu) *optret_menu = menu;
    return MLE_OK;
}

// Open dialog menu with prompt
int editor_prompt_menu(editor_t* editor, char* prompt, char* opt_buf_data, int opt_buf_data_len, bview_listener_cb_t opt_prompt_cb, async_proc_t* opt_aproc, char** optret_line) {
    bview_t* menu;
    bview_t* orig;
    char* prompt_answer;
    orig = editor->active;
    editor_open_bview(editor, NULL, MLE_BVIEW_TYPE_EDIT, NULL, 0, 1, 0, &editor->rect_edit, NULL, &menu);
    menu->is_menu = 1;
    if (opt_aproc) {
        async_proc_set_invoker(opt_aproc, menu);
    }
    if (opt_buf_data) {
        mark_insert_before(menu->active_cursor->mark, opt_buf_data, opt_buf_data_len);
    }
    editor_prompt(editor, prompt, &(editor_prompt_params_t) { .kmap = editor->kmap_prompt_menu, .prompt_cb = opt_prompt_cb }, &prompt_answer);
    if (optret_line) {
        if (prompt_answer) {
            *optret_line = strndup(menu->active_cursor->mark->bline->data, menu->active_cursor->mark->bline->data_len);
            free(prompt_answer);
        } else {
            *optret_line = NULL;
        }
    }
    editor_close_bview(editor, menu, NULL);
    editor_set_active(editor, orig);
    return MLE_OK;
}

// Open a bview
int editor_open_bview(editor_t* editor, bview_t* parent, int type, char* opt_path, int opt_path_len, int make_active, bint_t linenum, bview_rect_t* opt_rect, buffer_t* opt_buffer, bview_t** optret_bview) {
    bview_t* bview;
    bview = bview_new(editor, opt_path, opt_path_len, opt_buffer);
    bview->type = type;
    CDL_PREPEND2(editor->all_bviews, bview, all_prev, all_next);
    if (!parent) {
        DL_APPEND2(editor->top_bviews, bview, top_prev, top_next);
    } else {
        parent->split_child = bview;
    }
    if (make_active) {
        editor_set_active(editor, bview);
    }
    if (opt_rect) {
        bview_resize(bview, opt_rect->x, opt_rect->y, opt_rect->w, opt_rect->h);
    }
    if (linenum > 0) {
        mark_move_to(bview->active_cursor->mark, linenum - 1, 0);
        bview_center_viewport_y(bview);
    }
    if (optret_bview) {
        *optret_bview = bview;
    }
    if (opt_path && util_is_dir(opt_path)) {
        // TODO This is hacky
        cmd_context_t ctx;
        memset(&ctx, 0, sizeof(cmd_context_t));
        ctx.editor = editor;
        ctx.static_param = strndup(opt_path, opt_path_len);
        ctx.bview = bview;
        cmd_browse(&ctx);
        editor_close_bview(editor, bview, NULL);
        free(ctx.static_param);
        ctx.static_param = NULL;
    }
    return MLE_OK;
}

// Close a bview
int editor_close_bview(editor_t* editor, bview_t* bview, int* optret_num_closed) {
    int rc;
    if (optret_num_closed) *optret_num_closed = 0;
    if ((rc = _editor_close_bview_inner(editor, bview, optret_num_closed)) == MLE_OK) {
        _editor_resize(editor, editor->w, editor->h);
    }
    return rc;
}

// Set the active bview
int editor_set_active(editor_t* editor, bview_t* bview) {
    if (!editor_bview_exists(editor, bview)) {
        MLE_RETURN_ERR(editor, "No bview %p in editor->all_bviews", bview);
    } else if (editor->prompt) {
        MLE_RETURN_ERR(editor, "Cannot abandon prompt for bview %p", bview);
    }
    editor->active = bview;
    if (MLE_BVIEW_IS_EDIT(bview)) {
        editor->active_edit = bview;
        editor->active_edit_root = bview_get_split_root(bview);
    }
    bview_rectify_viewport(bview);
    return MLE_OK;
}

// Set macro toggle key
int editor_set_macro_toggle_key(editor_t* editor, char* key) {
    return _editor_key_to_input(key, &editor->macro_toggle_key);
}

// Return 1 if bview exists in editor, else return 0
int editor_bview_exists(editor_t* editor, bview_t* bview) {
    bview_t* tmp;
    CDL_FOREACH2(editor->all_bviews, tmp, all_next) {
        if (tmp == bview) return 1;
    }
    return 0;
}

// Return number of EDIT bviews open
int editor_bview_edit_count(editor_t* editor) {
    int count;
    bview_t* bview;
    count = 0;
    CDL_FOREACH2(editor->all_bviews, bview, all_next) {
        if (MLE_BVIEW_IS_EDIT(bview)) count += 1;
    }
    return count;
}

// Return number of bviews displaying buffer
int editor_count_bviews_by_buffer(editor_t* editor, buffer_t* buffer) {
    int count;
    bview_t* bview;
    count = 0;
    CDL_FOREACH2(editor->all_bviews, bview, all_next) {
        if (bview->buffer == buffer) count += 1;
    }
    return count;
}

// Register a command
int editor_register_cmd(editor_t* editor, char* name, cmd_func_t opt_func, cmd_funcref_t** optret_funcref) {
    cmd_funcref_t* funcref;
    HASH_FIND_STR(editor->func_map, name, funcref);
    if (funcref) {
        if (opt_func) funcref->func = opt_func;
    } else {
        funcref = calloc(1, sizeof(cmd_funcref_t));
        funcref->name = strdup(name);
        funcref->func = opt_func;
        HASH_ADD_KEYPTR(hh, editor->func_map, funcref->name, strlen(funcref->name), funcref);
    }
    if (optret_funcref) *optret_funcref = funcref;
    return MLE_OK;
}

// Get input from either macro or user
int editor_get_input(editor_t* editor, cmd_context_t* ctx) {
    ctx->is_user_input = 0;
    if (editor->macro_apply
        && editor->macro_apply_input_index < editor->macro_apply->inputs_len
    ) {
        // Get input from macro
        ctx->input = editor->macro_apply->inputs[editor->macro_apply_input_index];
        editor->macro_apply_input_index += 1;
    } else {
        // Clear macro
        if (editor->macro_apply) {
            editor->macro_apply = NULL;
            editor->macro_apply_input_index = 0;
        }
        // Get user input
        _editor_get_user_input(editor, ctx);
        ctx->is_user_input = 1;
    }
    if (editor->is_recording_macro && editor->macro_record) {
        // Record macro input
        _editor_record_macro_input(editor->macro_record, &ctx->input);
    }
    return MLE_OK;
}

// Display the editor
int editor_display(editor_t* editor) {
    bview_t* bview;
    tb_clear();
    bview_draw(editor->active_edit_root);
    bview_draw(editor->status);
    if (editor->prompt) bview_draw(editor->prompt);
    DL_FOREACH2(editor->top_bviews, bview, top_next) {
        _editor_draw_cursors(editor, bview);
    }
    tb_present();
    return MLE_OK;
}

// Close a bview
static int _editor_close_bview_inner(editor_t* editor, bview_t* bview, int *optret_num_closed) {
    if (!editor_bview_exists(editor, bview)) {
        MLE_RETURN_ERR(editor, "No bview %p in editor->all_bviews", bview);
    }
    if (bview->split_child) {
        _editor_close_bview_inner(editor, bview->split_child, optret_num_closed);
    }
    if (bview->split_parent) {
        bview->split_parent->split_child = NULL;
        editor_set_active(editor, bview->split_parent);
    } else {
        if (bview->all_prev && bview->all_prev != bview && MLE_BVIEW_IS_EDIT(bview->all_prev)) {
            editor_set_active(editor, bview->all_prev);
        } else if (bview->all_next && bview->all_next != bview && MLE_BVIEW_IS_EDIT(bview->all_next)) {
            editor_set_active(editor, bview->all_next);
        } else {
            editor_open_bview(editor, NULL, MLE_BVIEW_TYPE_EDIT, NULL, 0, 1, 0, &editor->rect_edit, NULL, NULL);
        }
    }
    if (!bview->split_parent) {
        DL_DELETE2(editor->top_bviews, bview, top_prev, top_next);
    }
    CDL_DELETE2(editor->all_bviews, bview, all_prev, all_next);
    bview_destroy(bview);
    if (optret_num_closed) *optret_num_closed += 1;
    return MLE_OK;
}

// Invoked when user hits enter in a prompt_input
static int _editor_prompt_input_submit(cmd_context_t* ctx) {
    bint_t answer_len;
    char* answer;
    buffer_get(ctx->bview->buffer, &answer, &answer_len);
    ctx->loop_ctx->prompt_answer = strndup(answer, answer_len);
    ctx->loop_ctx->should_exit = 1;
    return MLE_OK;
}

// Invoke when user hits tab in a prompt_input
static int _editor_prompt_input_complete(cmd_context_t* ctx) {
    loop_context_t* loop_ctx;
    loop_ctx = ctx->loop_ctx;
    char* cmd;
    char* cmd_arg;
    char* terms;
    size_t terms_len;
    int num_terms;
    char* term;
    int term_index;

    // Update tab_complete_term and tab_complete_index
    if (loop_ctx->last_cmd && loop_ctx->last_cmd->func == _editor_prompt_input_complete) {
        loop_ctx->tab_complete_index += 1;
    } else if (ctx->bview->buffer->first_line->data_len < MLE_LOOP_CTX_MAX_COMPLETE_TERM_SIZE) {
        snprintf(
            loop_ctx->tab_complete_term,
            MLE_LOOP_CTX_MAX_COMPLETE_TERM_SIZE,
            "%.*s",
            (int)ctx->bview->buffer->first_line->data_len,
            ctx->bview->buffer->first_line->data
        );
        loop_ctx->tab_complete_index = 0;
    } else {
        return MLE_OK;
    }

    // Assemble compgen command
    cmd_arg = util_escape_shell_arg(
        loop_ctx->tab_complete_term,
        strlen(loop_ctx->tab_complete_term)
    );
    asprintf(&cmd, "compgen -f %s | sort", cmd_arg);

    // Run compgen command
    terms = NULL;
    terms_len = 0;
    util_shell_exec(ctx->editor, cmd, 1, NULL, 0, "bash", &terms, &terms_len);
    free(cmd);
    free(cmd_arg);

    // Get number of terms
    // TODO valgrind thinks there's an error here
    num_terms = 0;
    term = strchr(terms, '\n');
    while (term) {
        num_terms += 1;
        term = strchr(term + 1, '\n');
    }

    // Bail if no terms
    if (num_terms < 1) {
        free(terms);
        return MLE_OK;
    }

    // Determine term index
    term_index = loop_ctx->tab_complete_index % num_terms;

    // Set prompt input to term
    term = strtok(terms, "\n");
    while (term != NULL) {
        if (term_index == 0) {
            buffer_set(ctx->bview->buffer, term, strlen(term));
            mark_move_eol(ctx->cursor->mark);
            break;
        } else {
            term_index -= 1;
        }
        term = strtok(NULL, "\n");
    }

    free(terms);
    return MLE_OK;
}

// Invoked when user hits a in a prompt_yna
static int _editor_prompt_yna_all(cmd_context_t* ctx) {
    ctx->loop_ctx->prompt_answer = MLE_PROMPT_ALL;
    ctx->loop_ctx->should_exit = 1;
    return MLE_OK;
}

// Invoked when user hits y in a prompt_yn(a)
static int _editor_prompt_yn_yes(cmd_context_t* ctx) {
    ctx->loop_ctx->prompt_answer = MLE_PROMPT_YES;
    ctx->loop_ctx->should_exit = 1;
    return MLE_OK;
}

// Invoked when user hits n in a prompt_yn(a)
static int _editor_prompt_yn_no(cmd_context_t* ctx) {
    ctx->loop_ctx->prompt_answer = MLE_PROMPT_NO;
    ctx->loop_ctx->should_exit = 1;
    return MLE_OK;
}

// Invoked when user cancels (Ctrl-C) a prompt_(input|yn), or hits any key in a prompt_ok
static int _editor_prompt_cancel(cmd_context_t* ctx) {
    ctx->loop_ctx->prompt_answer = NULL;
    ctx->loop_ctx->should_exit = 1;
    return MLE_OK;
}

// Invoked when user hits enter in a menu
static int _editor_menu_submit(cmd_context_t* ctx) {
    if (ctx->bview->menu_callback) return ctx->bview->menu_callback(ctx);
    return MLE_OK;
}

// Invoked when user hits C-c in a menu
static int _editor_menu_cancel(cmd_context_t* ctx) {
    if (ctx->bview->async_proc) async_proc_destroy(ctx->bview->async_proc);
    return MLE_OK;
}

// Invoked when user hits up in a prompt_menu
static int _editor_prompt_menu_up(cmd_context_t* ctx) {
    mark_move_vert(ctx->editor->active_edit->active_cursor->mark, -1);
    bview_rectify_viewport(ctx->editor->active_edit);
    return MLE_OK;
}

// Invoked when user hits down in a prompt_menu
static int _editor_prompt_menu_down(cmd_context_t* ctx) {
    mark_move_vert(ctx->editor->active_edit->active_cursor->mark, 1);
    bview_rectify_viewport(ctx->editor->active_edit);
    return MLE_OK;
}

// Invoked when user hits page-up in a prompt_menu
static int _editor_prompt_menu_page_up(cmd_context_t* ctx) {
    mark_move_vert(ctx->editor->active_edit->active_cursor->mark, -1 * ctx->editor->active_edit->rect_buffer.h);
    bview_zero_viewport_y(ctx->editor->active_edit);
    return MLE_OK;
}

// Invoked when user hits page-down in a prompt_menu
static int _editor_prompt_menu_page_down(cmd_context_t* ctx) {
    mark_move_vert(ctx->editor->active_edit->active_cursor->mark, ctx->editor->active_edit->rect_buffer.h);
    bview_zero_viewport_y(ctx->editor->active_edit);
    return MLE_OK;
}

// Invoked when user hits down in a prompt_isearch
static int _editor_prompt_isearch_next(cmd_context_t* ctx) {
    if (ctx->editor->active_edit->isearch_rule) {
        mark_move_next_cre(ctx->editor->active_edit->active_cursor->mark, ctx->editor->active_edit->isearch_rule->cre);
        bview_center_viewport_y(ctx->editor->active_edit);
    }
    return MLE_OK;
}

// Invoked when user hits up in a prompt_isearch
static int _editor_prompt_isearch_prev(cmd_context_t* ctx) {
    if (ctx->editor->active_edit->isearch_rule) {
        mark_move_prev_cre(ctx->editor->active_edit->active_cursor->mark, ctx->editor->active_edit->isearch_rule->cre);
        bview_center_viewport_y(ctx->editor->active_edit);
    }
    return MLE_OK;
}

// Drops a cursor on each isearch match
static int _editor_prompt_isearch_drop_cursors(cmd_context_t* ctx) {
    bview_t* bview;
    mark_t* mark;
    pcre* cre;
    cursor_t* orig_cursor;
    cursor_t* last_cursor;
    bview = ctx->editor->active_edit;
    if (!bview->isearch_rule) return MLE_OK;
    orig_cursor = bview->active_cursor;
    mark = bview->active_cursor->mark;
    cre = bview->isearch_rule->cre;
    mark_move_beginning(mark);
    last_cursor = NULL;
    while (mark_move_next_cre(mark, cre) == MLBUF_OK) {
        bview_add_cursor(bview, mark->bline, mark->col, &last_cursor);
    }
    if (last_cursor) bview_remove_cursor(bview, last_cursor);
    bview->active_cursor = orig_cursor;
    bview_center_viewport_y(bview);
    ctx->loop_ctx->prompt_answer = NULL;
    ctx->loop_ctx->should_exit = 1;
    return MLE_OK;
}

// Run startup actions. This is before any user-input is processed.
static void _editor_startup(editor_t* editor) {
    // Jump to line in current bview if specified
    if (editor->startup_linenum >= 0) {
        mark_move_to(editor->active_edit->active_cursor->mark, editor->startup_linenum, 0);
        bview_center_viewport_y(editor->active_edit);
    }
}

// Run editor loop
static void _editor_loop(editor_t* editor, loop_context_t* loop_ctx) {
    cmd_funcref_t* cmd_ref;
    cmd_context_t cmd_ctx;
    cmd_func_t cmd_fn;

    // Increment loop_depth
    editor->loop_depth += 1;

    // Init cmd_context
    memset(&cmd_ctx, 0, sizeof(cmd_context_t));
    cmd_ctx.editor = editor;
    cmd_ctx.loop_ctx = loop_ctx;

    // Loop until editor should exit
    while (!loop_ctx->should_exit) {
        // Set loop_ctx
        editor->loop_ctx = loop_ctx;

        // Display editor
        if (!editor->is_display_disabled) {
            editor_display(editor);
        }

        // Check for async input
        if (editor->async_procs && _editor_drain_async_procs(editor)) {
            continue;
        }

        // Get input
        editor_get_input(editor, &cmd_ctx);

        // Toggle macro?
        if (_editor_maybe_toggle_macro(editor, &cmd_ctx.input)) {
            continue;
        }

        if ((cmd_ref = _editor_get_command(editor, &cmd_ctx, NULL)) != NULL) {
            // Found command in kmap trie, now resolve
            if ((cmd_fn = _editor_resolve_funcref(editor, cmd_ref)) != NULL) {
                // Resolved, now execute
                if (cmd_ctx.is_user_input && cmd_fn == cmd_insert_data) {
                    _editor_ingest_paste(editor, &cmd_ctx);
                }
                cmd_ctx.cursor = editor->active ? editor->active->active_cursor : NULL;
                cmd_ctx.bview = cmd_ctx.cursor ? cmd_ctx.cursor->bview : NULL;
                cmd_ctx.udata = &cmd_ref->udata;
                cmd_fn(&cmd_ctx);
                loop_ctx->binding_node = NULL;
                loop_ctx->wildcard_params_len = 0;
                loop_ctx->numeric_params_len = 0;
                loop_ctx->last_cmd = cmd_ref;
            }
        } else if (loop_ctx->need_more_input) {
            // Need more input to find
        } else {
            // Not found, bad command
            loop_ctx->binding_node = NULL;
        }
    }

    // Free pastebuf if present
    if (cmd_ctx.pastebuf) free(cmd_ctx.pastebuf);

    // Decrement loop_depth
    editor->loop_depth -= 1;
}

// If input == editor->macro_toggle_key, toggle macro mode and return 1. Else
// return 0.
static int _editor_maybe_toggle_macro(editor_t* editor, kinput_t* input) {
    char* name;
    if (memcmp(input, &editor->macro_toggle_key, sizeof(kinput_t)) != 0) {
        return 0;
    }
    if (editor->is_recording_macro) {
        // Stop recording macro and add to map
        if (editor->macro_record->inputs_len > 0) {
            // Remove toggle key from macro inputs
            editor->macro_record->inputs_len -= 1; // TODO This is hacky
        }
        HASH_ADD_STR(editor->macro_map, name, editor->macro_record);
        editor->macro_record = NULL;
        editor->is_recording_macro = 0;
    } else {
        // Get macro name and start recording
        editor_prompt(editor, "record_macro: Name?", NULL, &name);
        if (!name) return 1;
        editor->macro_record = calloc(1, sizeof(kmacro_t));
        editor->macro_record->name = name;
        editor->is_recording_macro = 1;
    }
    return 1;
}

// Resize the editor
static void _editor_resize(editor_t* editor, int w, int h) {
    bview_t* bview;
    bview_rect_t* bounds;

    editor->w = w >= 0 ? w : tb_width();
    editor->h = h >= 0 ? h : tb_height();

    editor->rect_edit.x = 0;
    editor->rect_edit.y = 0;
    editor->rect_edit.w = editor->w;
    editor->rect_edit.h = editor->h - 2;

    editor->rect_status.x = 0;
    editor->rect_status.y = editor->h - 2;
    editor->rect_status.w = editor->w;
    editor->rect_status.h = 1;

    editor->rect_prompt.x = 0;
    editor->rect_prompt.y = editor->h - 1;
    editor->rect_prompt.w = editor->w;
    editor->rect_prompt.h = 1;

    DL_FOREACH2(editor->top_bviews, bview, top_next) {
        if (MLE_BVIEW_IS_PROMPT(bview)) {
            bounds = &editor->rect_prompt;
        } else if (MLE_BVIEW_IS_STATUS(bview)) {
            bounds = &editor->rect_status;
        } else {
            if (bview->split_parent) continue;
            bounds = &editor->rect_edit;
        }
        bview_resize(bview, bounds->x, bounds->y, bounds->w, bounds->h);
    }
}

// Draw bviews cursors recursively
static void _editor_draw_cursors(editor_t* editor, bview_t* bview) {
    if (MLE_BVIEW_IS_EDIT(bview) && bview_get_split_root(bview) != editor->active_edit_root) {
        return;
    }
    bview_draw_cursor(bview, bview == editor->active ? 1 : 0);
    if (bview->split_child) {
        _editor_draw_cursors(editor, bview->split_child);
    }
}

// Get user input
static void _editor_get_user_input(editor_t* editor, cmd_context_t* ctx) {
    int rc;
    tb_event_t ev;

    // Reset pastebuf
    ctx->pastebuf_len = 0;

    // Use pastebuf_leftover is present
    if (ctx->has_pastebuf_leftover) {
        ctx->input = ctx->pastebuf_leftover;
        ctx->has_pastebuf_leftover = 0;
        return;
    }

    // Poll for event
    while (1) {
        rc = tb_poll_event(&ev);
        if (rc == -1) {
            continue; // Error
        } else if (rc == TB_EVENT_RESIZE) {
            // Resize
            _editor_resize(editor, ev.w, ev.h);
            editor_display(editor);
            continue;
        }
        ctx->input = (kinput_t){ ev.mod, ev.ch, ev.key };
        break;
    }
}

// Ingest available input until non-cmd_insert_data
static void _editor_ingest_paste(editor_t* editor, cmd_context_t* ctx) {
    int rc;
    tb_event_t ev;
    kinput_t input;
    cmd_funcref_t* funcref;
    memset(&input, 0, sizeof(kinput_t));

    // Reset pastebuf
    ctx->pastebuf_len = 0;

    // Peek events
    while (1) {
        // Expand pastebuf if needed
        if (ctx->pastebuf_len + 1 > ctx->pastebuf_size) {
            ctx->pastebuf_size += MLE_PASTEBUF_INCR;
            ctx->pastebuf = realloc(ctx->pastebuf, sizeof(kinput_t) * ctx->pastebuf_size);
        }

        // Peek event
        rc = tb_peek_event(&ev, 0);
        if (rc == -1) {
            break; // Error
        } else if (rc == 0) {
            break; // Timeout
        } else if (rc == TB_EVENT_RESIZE) {
            // Resize
            _editor_resize(editor, ev.w, ev.h);
            editor_display(editor);
            break;
        }
        input = (kinput_t){ ev.mod, ev.ch, ev.key };
        // TODO check for macro key
        funcref = _editor_get_command(editor, ctx, &input);
        if (funcref && funcref->func == cmd_insert_data) {
            // Insert data; keep ingesting
            ctx->pastebuf[ctx->pastebuf_len++] = input;
        } else {
            // Not insert data; set leftover and stop ingesting
            ctx->has_pastebuf_leftover = 1;
            ctx->pastebuf_leftover = input;
            break;
        }
    }
}

// Copy input into macro buffer
static void _editor_record_macro_input(kmacro_t* macro, kinput_t* input) {
    if (!macro->inputs) {
        macro->inputs = calloc(8, sizeof(kinput_t));
        macro->inputs_len = 0;
        macro->inputs_cap = 8;
    } else if (macro->inputs_len + 1 > macro->inputs_cap) {
        macro->inputs_cap = macro->inputs_len + 8;
        macro->inputs = realloc(macro->inputs, macro->inputs_cap * sizeof(kinput_t));
    }
    memcpy(macro->inputs + macro->inputs_len, input, sizeof(kinput_t));
    macro->inputs_len += 1;
}

// Return command for input
static cmd_funcref_t* _editor_get_command(editor_t* editor, cmd_context_t* ctx, kinput_t* opt_peek_input) {
    loop_context_t* loop_ctx;
    kinput_t* input;
    kbinding_t* node;
    kbinding_t* binding;
    kmap_node_t* kmap_node;
    int is_top;
    int is_peek;
    int again;

    // Init some vars
    loop_ctx = ctx->loop_ctx;
    is_peek = opt_peek_input ? 1 : 0;
    input = opt_peek_input ? opt_peek_input : &ctx->input;
    kmap_node = editor->active->kmap_tail;
    node = loop_ctx->binding_node;
    is_top = (node == NULL ? 1 : 0);
    loop_ctx->need_more_input = 0;
    loop_ctx->binding_node = NULL;

    // Look for key binding
    while (kmap_node) {
        if (is_top) node = kmap_node->kmap->bindings;
        again = 0;
        binding = _editor_get_kbinding_node(node, input, loop_ctx, is_peek, &again);
        if (binding) {
            if (again) {
                // Need more input on current node
                if (!is_peek) {
                    loop_ctx->need_more_input = 1;
                    loop_ctx->binding_node = binding;
                }
                return NULL;
            } else if (binding->funcref) {
                // Found leaf!
                if (!is_peek) {
                    ctx->static_param = binding->static_param;
                }
                return binding->funcref;
            } else if (binding->children) {
                // Need more input on next node
                if (!is_peek) {
                    loop_ctx->need_more_input = 1;
                    loop_ctx->binding_node = binding;
                }
                return NULL;
            } else {
                // This shouldn't happen... TODO err
                return NULL;
            }
        } else if (node == kmap_node->kmap->bindings) {
            // Binding not found at top level
            if (kmap_node->kmap->default_funcref) {
                // Fallback to default
                return kmap_node->kmap->default_funcref;
            }
            if (kmap_node->kmap->allow_fallthru) {
                // Fallback to previous kmap on stack
                kmap_node = kmap_node->prev;
                is_top = 1;
            } else {
                // Fallback not allowed
                return NULL;
            }
        } else {
            // Binding not found
            return NULL;
        }
    }

    // No more kmaps
    return NULL;
}

// Find binding by input in trie, taking into account numeric and wildcards patterns
static kbinding_t* _editor_get_kbinding_node(kbinding_t* node, kinput_t* input, loop_context_t* loop_ctx, int is_peek, int* ret_again) {
    kbinding_t* binding;
    kinput_t input_tmp;
    memset(&input_tmp, 0, sizeof(kinput_t));

    if (!is_peek) {
        // Look for numeric .. TODO can be more efficient about this
        if (input->ch >= '0' && input->ch <= '9') {
            if (!loop_ctx->numeric_node) {
                input_tmp = MLE_KINPUT_NUMERIC;
                HASH_FIND(hh, node->children, &input_tmp, sizeof(kinput_t), binding);
                loop_ctx->numeric_node = binding;
            }
            if (loop_ctx->numeric_node) {
                if (loop_ctx->numeric_len < MLE_LOOP_CTX_MAX_NUMERIC_LEN) {
                    loop_ctx->numeric[loop_ctx->numeric_len] = (char)input->ch;
                    loop_ctx->numeric_len += 1;
                    *ret_again = 1;
                    return node; // Need more input on this node
                }
                return NULL; // Ran out of `numeric` buffer .. TODO err
            }
        }

        // Parse/reset numeric buffer
        if (loop_ctx->numeric_len > 0) {
            if (loop_ctx->numeric_params_len < MLE_LOOP_CTX_MAX_NUMERIC_PARAMS) {
                loop_ctx->numeric[loop_ctx->numeric_len] = '\0';
                loop_ctx->numeric_params[loop_ctx->numeric_params_len] = strtoul(loop_ctx->numeric, NULL, 10);
                loop_ctx->numeric_params_len += 1;
                loop_ctx->numeric_len = 0;
                node = loop_ctx->numeric_node; // Resume on numeric's children
                loop_ctx->numeric_node = NULL;
            } else {
                loop_ctx->numeric_len = 0;
                loop_ctx->numeric_node = NULL;
                return NULL; // Ran out of `numeric_params` space .. TODO err
            }
        }
    }

    // Look for input
    HASH_FIND(hh, node->children, input, sizeof(kinput_t), binding);
    if (binding) {
        return binding;
    }

    if (!is_peek) {
        // Look for wildcard
        input_tmp = MLE_KINPUT_WILDCARD;
        HASH_FIND(hh, node->children, &input_tmp, sizeof(kinput_t), binding);
        if (binding) {
            if (loop_ctx->wildcard_params_len < MLE_LOOP_CTX_MAX_WILDCARD_PARAMS) {
                loop_ctx->wildcard_params[loop_ctx->wildcard_params_len] = input->ch;
                loop_ctx->wildcard_params_len += 1;
            } else {
                return NULL; // Ran out of `wildcard_params` space .. TODO err
            }
            return binding;
        }
    }

    return NULL;
}

// Resolve a funcref to a func
static cmd_func_t _editor_resolve_funcref(editor_t* editor, cmd_funcref_t* ref) {
    cmd_funcref_t* resolved;
    if (!ref->func) {
        HASH_FIND_STR(editor->func_map, ref->name, resolved);
        if (resolved) {
            ref->func = resolved->func;
        }
    }
    return ref->func;
}

// Return a kinput_t given a key name
static int _editor_key_to_input(char* key, kinput_t* ret_input) {
    int keylen;
    int mod;
    uint32_t ch;
    keylen = strlen(key);
    memset(ret_input, 0, sizeof(kinput_t));

    // Check for special key
#define MLE_KEY_DEF(pckey, pmod, pch, pkey) \
    } else if (keylen == strlen((pckey)) && !strncmp((pckey), key, keylen)) { \
        ret_input->mod = (pmod); \
        ret_input->ch = (pch); \
        ret_input->key = (pkey); \
        return MLE_OK;
    if (keylen < 1) {
        return MLE_ERR;
#include "keys.h"
    }
#undef MLE_KEY_DEF

    // Check for character, with potential ALT modifier
    mod = 0;
    ch = 0;
    if (keylen > 2 && !strncmp("M-", key, 2)) {
        mod = TB_MOD_ALT;
        key += 2;
    }
    utf8_char_to_unicode(&ch, key, NULL);
    if (ch < 1) {
        return MLE_ERR;
    }
    ret_input->mod = mod;
    ret_input->ch = ch;
    return MLE_OK;
}

// Init signal handlers
static void _editor_init_signal_handlers(editor_t* editor) {
    struct sigaction action;
    memset(&action, 0, sizeof(struct sigaction));
    action.sa_handler = _editor_graceful_exit;
    sigaction(SIGTERM, &action, NULL);
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGQUIT, &action, NULL);
    sigaction(SIGHUP, &action, NULL);
}

// Gracefully exit
static void _editor_graceful_exit(int signum) {
    bview_t* bview;
    char path[64];
    int bview_num;
    bview_num = 0;
    tb_shutdown();
    CDL_FOREACH2(_editor.all_bviews, bview, all_next) {
        if (bview->buffer->is_unsaved) {
            snprintf((char*)&path, 64, "mle.bak.%d.%d", getpid(), bview_num);
            buffer_save_as(bview->buffer, path, strlen(path));
            bview_num += 1;
        }
    }
    editor_deinit(&_editor);
    exit(1);
}

// Init built-in kmaps
static void _editor_init_kmaps(editor_t* editor) {
    _editor_init_kmap(editor, &editor->kmap_normal, "mle_normal", MLE_FUNCREF(cmd_insert_data), 0, (kbinding_def_t[]){
        MLE_KBINDING_DEF(cmd_delete_before, "backspace"),
        MLE_KBINDING_DEF(cmd_delete_before, "backspace2"),
        MLE_KBINDING_DEF(cmd_delete_after, "delete"),
        MLE_KBINDING_DEF(cmd_move_bol, "C-a"),
        MLE_KBINDING_DEF(cmd_move_bol, "home"),
        MLE_KBINDING_DEF(cmd_move_eol, "C-e"),
        MLE_KBINDING_DEF(cmd_move_eol, "end"),
        MLE_KBINDING_DEF(cmd_move_beginning, "M-\\"),
        MLE_KBINDING_DEF(cmd_move_end, "M-/"),
        MLE_KBINDING_DEF(cmd_move_left, "left"),
        MLE_KBINDING_DEF(cmd_move_right, "right"),
        MLE_KBINDING_DEF(cmd_move_up, "up"),
        MLE_KBINDING_DEF(cmd_move_down, "down"),
        MLE_KBINDING_DEF(cmd_move_page_up, "page-up"),
        MLE_KBINDING_DEF(cmd_move_page_down, "page-down"),
        MLE_KBINDING_DEF(cmd_move_to_line, "M-g"),
        MLE_KBINDING_DEF_EX(cmd_move_relative, "M-y ## u", "up", NULL),
        MLE_KBINDING_DEF_EX(cmd_move_relative, "M-y ## d", "down", NULL),
        MLE_KBINDING_DEF(cmd_move_until_forward, "M-' **"),
        MLE_KBINDING_DEF(cmd_move_until_back, "M-; **"),
        MLE_KBINDING_DEF(cmd_move_word_forward, "M-f"),
        MLE_KBINDING_DEF(cmd_move_word_back, "M-b"),
        MLE_KBINDING_DEF(cmd_search, "C-f"),
        MLE_KBINDING_DEF(cmd_search_next, "C-g"),
        MLE_KBINDING_DEF(cmd_search_next, "F3"),
        MLE_KBINDING_DEF(cmd_find_word, "C-v"),
        MLE_KBINDING_DEF(cmd_isearch, "C-r"),
        MLE_KBINDING_DEF(cmd_replace, "C-t"),
        MLE_KBINDING_DEF(cmd_cut, "C-k"),
        MLE_KBINDING_DEF(cmd_copy, "M-k"),
        MLE_KBINDING_DEF(cmd_uncut, "C-u"),
        MLE_KBINDING_DEF(cmd_redraw, "C-l"),
        MLE_KBINDING_DEF_EX(cmd_copy_by, "C-c d", "bracket", NULL),
        MLE_KBINDING_DEF_EX(cmd_copy_by, "C-c w", "word", NULL),
        MLE_KBINDING_DEF_EX(cmd_copy_by, "C-c s", "word_back", NULL),
        MLE_KBINDING_DEF_EX(cmd_copy_by, "C-c f", "word_forward", NULL),
        MLE_KBINDING_DEF_EX(cmd_copy_by, "C-c a", "bol", NULL),
        MLE_KBINDING_DEF_EX(cmd_copy_by, "C-c e", "eol", NULL),
        MLE_KBINDING_DEF_EX(cmd_cut_by, "C-d d", "bracket", NULL),
        MLE_KBINDING_DEF_EX(cmd_cut_by, "C-d w", "word", NULL),
        MLE_KBINDING_DEF_EX(cmd_cut_by, "C-d s", "word_back", NULL),
        MLE_KBINDING_DEF_EX(cmd_cut_by, "C-d f", "word_forward", NULL),
        MLE_KBINDING_DEF_EX(cmd_cut_by, "C-d a", "bol", NULL),
        MLE_KBINDING_DEF_EX(cmd_cut_by, "C-d e", "eol", NULL),
        MLE_KBINDING_DEF(cmd_delete_word_before, "M-w"),
        MLE_KBINDING_DEF(cmd_delete_word_after, "M-d"),
        MLE_KBINDING_DEF(cmd_toggle_sel_bound, "M-a"),
        MLE_KBINDING_DEF(cmd_drop_sleeping_cursor, "C-/ ."),
        MLE_KBINDING_DEF(cmd_wake_sleeping_cursors, "C-/ a"),
        MLE_KBINDING_DEF(cmd_remove_extra_cursors, "C-/ /"),
        MLE_KBINDING_DEF(cmd_drop_cursor_column, "C-/ '"),
        MLE_KBINDING_DEF(cmd_apply_macro, "M-j"),
        MLE_KBINDING_DEF(cmd_apply_macro_by, "M-m **"),
        MLE_KBINDING_DEF(cmd_next, "M-n"),
        MLE_KBINDING_DEF(cmd_prev, "M-p"),
        MLE_KBINDING_DEF(cmd_split_vertical, "M-v"),
        MLE_KBINDING_DEF(cmd_split_horizontal, "M-h"),
        MLE_KBINDING_DEF(cmd_split_vertical, "M-="),
        MLE_KBINDING_DEF(cmd_split_horizontal, "M--"),
        MLE_KBINDING_DEF(cmd_grep, "C-q"),
        MLE_KBINDING_DEF(cmd_fsearch, "C-p"),
        MLE_KBINDING_DEF(cmd_browse, "C-b"),
        MLE_KBINDING_DEF(cmd_undo, "C-z"),
        MLE_KBINDING_DEF(cmd_redo, "C-y"),
        MLE_KBINDING_DEF(cmd_save, "C-s"),
        MLE_KBINDING_DEF(cmd_save_as, "M-s"),
        MLE_KBINDING_DEF_EX(cmd_set_opt, "M-o a", "tab_to_space", NULL),
        MLE_KBINDING_DEF_EX(cmd_set_opt, "M-o t", "tab_width", NULL),
        MLE_KBINDING_DEF_EX(cmd_set_opt, "M-o y", "syntax", NULL),
        MLE_KBINDING_DEF(cmd_open_new, "C-n"),
        MLE_KBINDING_DEF(cmd_open_file, "C-o"),
        MLE_KBINDING_DEF(cmd_open_replace_new, "C-w n"),
        MLE_KBINDING_DEF(cmd_open_replace_file, "C-w o"),
        MLE_KBINDING_DEF_EX(cmd_fsearch, "C-w p", "replace", NULL),
        MLE_KBINDING_DEF(cmd_indent, "M-."),
        MLE_KBINDING_DEF(cmd_outdent, "M-,"),
        MLE_KBINDING_DEF(cmd_shell, "M-e"),
        MLE_KBINDING_DEF(cmd_close, "M-c"),
        MLE_KBINDING_DEF(cmd_quit, "C-x"),
        MLE_KBINDING_DEF(NULL, NULL)
    });
    _editor_init_kmap(editor, &editor->kmap_vim_normal, "vim_normal", MLE_FUNCREF_EX(cmd_vim_normal, cmdinit_vim_normal), 0, (kbinding_def_t[]){
        MLE_KBINDING_DEF(NULL, NULL)
    });
    _editor_init_kmap(editor, &editor->kmap_prompt_input, "mle_prompt_input", MLE_FUNCREF_NONE, 1, (kbinding_def_t[]){
        MLE_KBINDING_DEF(_editor_prompt_input_submit, "enter"),
        MLE_KBINDING_DEF(_editor_prompt_input_complete, "tab"),
        MLE_KBINDING_DEF(_editor_prompt_cancel, "C-c"),
        MLE_KBINDING_DEF(_editor_prompt_cancel, "C-x"),
        MLE_KBINDING_DEF(_editor_prompt_cancel, "M-c"),
        MLE_KBINDING_DEF(NULL, NULL)
    });
    _editor_init_kmap(editor, &editor->kmap_prompt_yn, "mle_prompt_yn", MLE_FUNCREF_NONE, 0, (kbinding_def_t[]){
        MLE_KBINDING_DEF(_editor_prompt_yn_yes, "y"),
        MLE_KBINDING_DEF(_editor_prompt_yn_no, "n"),
        MLE_KBINDING_DEF(_editor_prompt_cancel, "C-c"),
        MLE_KBINDING_DEF(_editor_prompt_cancel, "C-x"),
        MLE_KBINDING_DEF(_editor_prompt_cancel, "M-c"),
        MLE_KBINDING_DEF(NULL, NULL)
    });
    _editor_init_kmap(editor, &editor->kmap_prompt_yna, "mle_prompt_yna", MLE_FUNCREF_NONE, 0, (kbinding_def_t[]){
        MLE_KBINDING_DEF(_editor_prompt_yn_yes, "y"),
        MLE_KBINDING_DEF(_editor_prompt_yn_no, "n"),
        MLE_KBINDING_DEF(_editor_prompt_yna_all, "a"),
        MLE_KBINDING_DEF(_editor_prompt_cancel, "C-c"),
        MLE_KBINDING_DEF(_editor_prompt_cancel, "C-x"),
        MLE_KBINDING_DEF(_editor_prompt_cancel, "M-c"),
        MLE_KBINDING_DEF(NULL, NULL)
    });
    _editor_init_kmap(editor, &editor->kmap_prompt_ok, "mle_prompt_ok", MLE_FUNCREF(_editor_prompt_cancel), 0, (kbinding_def_t[]){
        MLE_KBINDING_DEF(NULL, NULL)
    });
    _editor_init_kmap(editor, &editor->kmap_menu, "mle_menu", MLE_FUNCREF_NONE, 1, (kbinding_def_t[]){
        MLE_KBINDING_DEF(_editor_menu_submit, "enter"),
        MLE_KBINDING_DEF(_editor_menu_cancel, "C-c"),
        MLE_KBINDING_DEF(NULL, NULL)
    });
    _editor_init_kmap(editor, &editor->kmap_prompt_menu, "mle_prompt_menu", MLE_FUNCREF_NONE, 1, (kbinding_def_t[]){
        MLE_KBINDING_DEF(_editor_prompt_input_submit, "enter"),
        MLE_KBINDING_DEF(_editor_prompt_menu_up, "up"),
        MLE_KBINDING_DEF(_editor_prompt_menu_down, "down"),
        MLE_KBINDING_DEF(_editor_prompt_menu_up, "left"),
        MLE_KBINDING_DEF(_editor_prompt_menu_down, "right"),
        MLE_KBINDING_DEF(_editor_prompt_menu_page_up, "page-up"),
        MLE_KBINDING_DEF(_editor_prompt_menu_page_down, "page-down"),
        MLE_KBINDING_DEF(_editor_prompt_cancel, "C-c"),
        MLE_KBINDING_DEF(_editor_prompt_cancel, "C-x"),
        MLE_KBINDING_DEF(_editor_prompt_cancel, "M-c"),
        MLE_KBINDING_DEF(NULL, NULL)
    });
    _editor_init_kmap(editor, &editor->kmap_prompt_isearch, "mle_prompt_isearch", MLE_FUNCREF_NONE, 1, (kbinding_def_t[]){
        MLE_KBINDING_DEF(_editor_prompt_isearch_prev, "up"),
        MLE_KBINDING_DEF(_editor_prompt_isearch_next, "down"),
        MLE_KBINDING_DEF(_editor_prompt_isearch_drop_cursors, "C-/"),
        MLE_KBINDING_DEF(_editor_prompt_cancel, "enter"),
        MLE_KBINDING_DEF(_editor_prompt_cancel, "C-c"),
        MLE_KBINDING_DEF(_editor_prompt_cancel, "C-x"),
        MLE_KBINDING_DEF(_editor_prompt_cancel, "M-c"),
        MLE_KBINDING_DEF(NULL, NULL)
    });
}

// Init a single kmap
static void _editor_init_kmap(editor_t* editor, kmap_t** ret_kmap, char* name, cmd_funcref_t default_funcref, int allow_fallthru, kbinding_def_t* defs) {
    kmap_t* kmap;
    cmd_funcref_t* funcref;

    kmap = calloc(1, sizeof(kmap_t));
    kmap->name = strdup(name);
    kmap->allow_fallthru = allow_fallthru;
    kmap->bindings = calloc(1, sizeof(kbinding_t));
    if (default_funcref.name) {
        editor_register_cmd(editor, default_funcref.name, default_funcref.func, &kmap->default_funcref);
    }

    while (defs && defs->key_patt) {
        editor_register_cmd(editor, defs->funcref.name, defs->funcref.func, &funcref);
        _editor_init_kmap_add_binding(editor, kmap, defs->funcref.name, defs->key_patt, defs->static_param);
        defs++;
    }

    HASH_ADD_KEYPTR(hh, editor->kmap_map, kmap->name, strlen(kmap->name), kmap);
    *ret_kmap = kmap;
}

// Add a binding to a kmap
static void _editor_init_kmap_add_binding(editor_t* editor, kmap_t* kmap, char* cmd_name, char* key_patt, char* static_param) {
    cmd_funcref_t* funcref;
    char* key_patt_dup;
    char* static_param_dup;
    key_patt_dup = strdup(key_patt);
    static_param_dup = static_param ? strdup(static_param) : NULL;
    editor_register_cmd(editor, cmd_name, NULL, &funcref);
    _editor_init_kmap_add_binding_to_trie(&kmap->bindings->children, funcref, key_patt_dup, static_param_dup);
    free(key_patt_dup);
}

// Add a binding to a kmap trie
static int _editor_init_kmap_add_binding_to_trie(kbinding_t** trie, cmd_funcref_t* funcref, char* key_patt, char* static_param) {
    char* next_key;
    kbinding_t* node;
    kinput_t input;

    // Find next_key and add nullchar to key_patt for this key
    next_key = strchr(key_patt, ' ');
    if (next_key != NULL) {
        *next_key = '\0';
        next_key += 1;
    }

    // Parse key_patt token as input
    memset(&input, 0, sizeof(kinput_t));
    if (strcmp("##", key_patt) == 0) {
        input = MLE_KINPUT_NUMERIC;
    } else if (strcmp("**", key_patt) == 0) {
        input = MLE_KINPUT_WILDCARD;
    } else if (_editor_key_to_input(key_patt, &input) == MLE_OK) {
        // Hi mom!
    } else {
        return MLE_ERR;
    }

    // Add node for input if it doesn't already exist
    node = NULL;
    HASH_FIND(hh, *trie, &input, sizeof(kinput_t), node);
    if (!node) {
        node = calloc(1, sizeof(kbinding_t));
        node->input = input;
        HASH_ADD(hh, *trie, input, sizeof(kinput_t), node);
    }

    if (next_key) {
        // Recurse for next key
        if (_editor_init_kmap_add_binding_to_trie(&node->children, funcref, next_key, static_param) != MLE_OK) {
            free(node);
            return MLE_ERR;
        }
    } else {
        // Leaf node, set funcref
        node->static_param = static_param;
        node->funcref = funcref;
    }

    return MLE_OK;
}

// Proxy for _editor_init_kmap with str in format '<name>,<default_cmd>,<allow_fallthru>'
static int _editor_init_kmap_by_str(editor_t* editor, kmap_t** ret_kmap, char* str) {
    char* args[3];
    args[0] = strtok(str,  ","); if (!args[0]) return MLE_ERR;
    args[1] = strtok(NULL, ","); if (!args[1]) return MLE_ERR;
    args[2] = strtok(NULL, ",");
    _editor_init_kmap(editor, ret_kmap, args[0], (cmd_funcref_t){ args[2] ? args[1] : NULL, NULL, NULL }, atoi(args[2] ? args[2] : args[1]), NULL);
    return MLE_OK;
}

// Proxy for _editor_init_kmap_add_binding with str in format '<cmd>,<key>,<param>'
static int _editor_init_kmap_add_binding_by_str(editor_t* editor, kmap_t* kmap, char* str) {
    char* args[3];
    args[0] = strtok(str,  ","); if (!args[0]) return MLE_ERR;
    args[1] = strtok(NULL, ","); if (!args[1]) return MLE_ERR;
    args[2] = strtok(NULL, ",");
    _editor_init_kmap_add_binding(editor, kmap, args[0], args[1], args[2]);
    return MLE_OK;
}

// Destroy a kmap
static void _editor_destroy_kmap(kmap_t* kmap, kbinding_t* trie) {
    kbinding_t* binding;
    kbinding_t* binding_tmp;
    int is_top;
    is_top = (trie == kmap->bindings ? 1 : 0);
    HASH_ITER(hh, trie, binding, binding_tmp) {
        if (binding->children) {
            _editor_destroy_kmap(kmap, binding->children);
        }
        HASH_DELETE(hh, trie, binding);
        if (binding->static_param) free(binding->static_param);
        free(binding);
    }
    if (is_top) {
        if (kmap->name) free(kmap->name);
        free(kmap);
    }
}

// Add a macro by str with format '<name> <key1> <key2> ... <keyN>'
static int _editor_add_macro_by_str(editor_t* editor, char* str) {
    int has_input;
    char* token;
    kmacro_t* macro;
    kinput_t input = { 0, 0, 0 };

    has_input = 0;
    macro = NULL;

    // Tokenize by space
    token = strtok(str, ",");
    while (token) {
        if (!macro) {
            // Make macro with <name> on first token
            macro = calloc(1, sizeof(kmacro_t));
            macro->name = strdup(token);
        } else {
            // Parse token as kinput_t
            if (_editor_key_to_input(token, &input) != MLE_OK) {
                free(macro->name);
                free(macro);
                return MLE_ERR;
            }
            // Add kinput_t to macro
            _editor_record_macro_input(macro, &input);
            has_input = 1;
        }
        // Get next token
        token = strtok(NULL, ",");
    }

    // Add macro to map if has_input
    if (has_input) {
        HASH_ADD_KEYPTR(hh, editor->macro_map, macro->name, strlen(macro->name), macro);
        return MLE_OK;
    }

    // Fail
    if (macro) {
        free(macro->name);
        free(macro);
    }
    return MLE_ERR;
}

// Init built-in syntax map
static void _editor_init_syntaxes(editor_t* editor) {
    _editor_init_syntax(editor, NULL, "syn_generic", "\\.(c|cpp|h|hpp|php|py|rb|erb|sh|pl|go|js|java|jsp|lua)$", (srule_def_t[]){
        { "(?<![\\w%@$])("
          "abstract|alias|alignas|alignof|and|and_eq|arguments|array|as|asm|"
          "assert|auto|base|begin|bitand|bitor|bool|boolean|break|byte|"
          "callable|case|catch|chan|char|checked|class|clone|cmp|compl|const|"
          "const_cast|constexpr|continue|debugger|decimal|declare|decltype|"
          "def|default|defer|defined|del|delegate|delete|die|do|done|double|"
          "dynamic_cast|echo|elif|else|elseif|elsif|empty|end|enddeclare|"
          "endfor|endforeach|endif|endswitch|endwhile|ensure|enum|eq|esac|"
          "eval|event|except|exec|exit|exp|explicit|export|extends|extern|"
          "fallthrough|false|fi|final|finally|fixed|float|for|foreach|friend|"
          "from|func|function|ge|global|go|goto|gt|if|implements|implicit|"
          "import|in|include|include_once|inline|instanceof|insteadof|int|"
          "interface|internal|is|isset|lambda|le|let|list|lock|long|lt|m|map|"
          "module|mutable|namespace|native|ne|new|next|nil|no|noexcept|not|"
          "not_eq|null|nullptr|object|operator|or|or_eq|out|override|package|"
          "params|pass|print|private|protected|public|q|qq|qr|qw|qx|raise|"
          "range|readonly|redo|ref|register|reinterpret_cast|require|"
          "require_once|rescue|retry|return|s|sbyte|sealed|select|self|short|"
          "signed|sizeof|stackalloc|static|static_assert|static_cast|"
          "strictfp|string|struct|sub|super|switch|synchronized|template|"
          "then|this|thread_local|throw|throws|time|tr|trait|transient|true|"
          "try|type|typedef|typeid|typename|typeof|uint|ulong|unchecked|"
          "undef|union|unless|unsafe|unset|unsigned|until|use|ushort|using|"
          "var|virtual|void|volatile|when|while|with|xor|xor_eq|y|yield"
          ")\\b", NULL, TB_GREEN, TB_DEFAULT },
        { "[(){}<>\\[\\].,;:?!+=/\\\\%^*-]", NULL, TB_RED | TB_BOLD, TB_DEFAULT },
        { "(?<!\\w)[\\%@$][a-zA-Z_$][a-zA-Z0-9_]*\\b", NULL, TB_GREEN, TB_DEFAULT },
        { "\\b[A-Z_][A-Z0-9_]*\\b", NULL, TB_RED | TB_BOLD, TB_DEFAULT },
        { "\\b(-?(0x)?[0-9]+|true|false|null)\\b", NULL, TB_BLUE | TB_BOLD, TB_DEFAULT },
        { "/([^/]|\\\\/)+/(?!/)", NULL, TB_YELLOW, TB_DEFAULT },
        { "'([^']|\\')*?'", NULL, TB_YELLOW | TB_BOLD, TB_DEFAULT },
        { "\"(\\\"|[^\"])*?\"", NULL, TB_YELLOW | TB_BOLD, TB_DEFAULT },
        { "/" "/.*$", NULL, TB_CYAN, TB_DEFAULT },
        { "^\\s*#( .*|)$", NULL, TB_CYAN, TB_DEFAULT },
        { "^#!/.*$", NULL, TB_CYAN, TB_DEFAULT },
        { "/\\" "*", "\\*" "/", TB_CYAN, TB_DEFAULT },
        { "<\\?(php)?|\\?" ">", NULL, TB_GREEN, TB_DEFAULT },
        { "\\?" ">", "<\\?(php)?", TB_WHITE, TB_DEFAULT },
        { "\"\"\"", "\"\"\"", TB_YELLOW | TB_BOLD, TB_DEFAULT },
        { "\\t+", NULL, TB_RED | TB_UNDERLINE, TB_DEFAULT },
        { "\\s+$", NULL, TB_DEFAULT, TB_GREEN },
        { NULL, NULL, 0, 0 }
    });
}

// Init a single syntax
static void _editor_init_syntax(editor_t* editor, syntax_t** optret_syntax, char* name, char* path_pattern, srule_def_t* defs) {
    syntax_t* syntax;

    syntax = calloc(1, sizeof(syntax_t));
    syntax->name = strdup(name);
    syntax->path_pattern = strdup(path_pattern);

    while (defs && defs->re) {
        _editor_init_syntax_add_rule(syntax, *defs);
        defs++;
    }
    HASH_ADD_KEYPTR(hh, editor->syntax_map, syntax->name, strlen(syntax->name), syntax);

    if (optret_syntax) *optret_syntax = syntax;
}

// Proxy for _editor_init_syntax with str in format '<name>,<path_pattern>'
static int _editor_init_syntax_by_str(editor_t* editor, syntax_t** ret_syntax, char* str) {
    char* args[2];
    args[0] = strtok(str,  ","); if (!args[0]) return MLE_ERR;
    args[1] = strtok(NULL, ","); if (!args[1]) return MLE_ERR;
    _editor_init_syntax(editor, ret_syntax, args[0], args[1], NULL);
    return MLE_OK;
}

// Add rule to syntax
static void _editor_init_syntax_add_rule(syntax_t* syntax, srule_def_t def) {
    srule_node_t* node;
    node = calloc(1, sizeof(srule_node_t));
    if (def.re_end) {
        node->srule = srule_new_multi(def.re, strlen(def.re), def.re_end, strlen(def.re_end), def.fg, def.bg);
    } else {
        node->srule = srule_new_single(def.re, strlen(def.re), 0, def.fg, def.bg);
    }
    if (node->srule) DL_APPEND(syntax->srules, node);
}

// Proxy for _editor_init_syntax_add_rule with str in format '<start>,<end>,<fg>,<bg>' or '<regex>,<fg>,<bg>'
static int _editor_init_syntax_add_rule_by_str(syntax_t* syntax, char* str) {
    char* args[4];
    int style_i;
    args[0] = strtok(str,  ","); if (!args[0]) return MLE_ERR;
    args[1] = strtok(NULL, ","); if (!args[1]) return MLE_ERR;
    args[2] = strtok(NULL, ","); if (!args[2]) return MLE_ERR;
    args[3] = strtok(NULL, ",");
    style_i = args[3] ? 2 : 1;
    _editor_init_syntax_add_rule(syntax, (srule_def_t){ args[0], style_i == 2 ? args[1] : NULL, atoi(args[style_i]), atoi(args[style_i + 1]) });
    return MLE_OK;
}

// Destroy a syntax
static void _editor_destroy_syntax_map(syntax_t* map) {
    syntax_t* syntax;
    syntax_t* syntax_tmp;
    srule_node_t* srule;
    srule_node_t* srule_tmp;
    HASH_ITER(hh, map, syntax, syntax_tmp) {
        HASH_DELETE(hh, map, syntax);
        DL_FOREACH_SAFE(syntax->srules, srule, srule_tmp) {
            DL_DELETE(syntax->srules, srule);
            srule_destroy(srule->srule);
            free(srule);
        }
        free(syntax->name);
        free(syntax->path_pattern);
        free(syntax);
    }
}

// Parse rc file
static int _editor_init_from_rc(editor_t* editor, FILE* rc) {
    int rv;
    long size;
    char *rc_data;
    char *rc_data_stop;
    char* eol;
    char* bol;
    int fargc;
    char** fargv;
    rv = MLE_OK;

    // Read all from rc
    fseek(rc, 0L, SEEK_END);
    size = ftell(rc);
    fseek(rc, 0L, SEEK_SET);
    rc_data = malloc(size + 1);
    fread(rc_data, size, 1, rc);
    rc_data[size] = '\0';
    rc_data_stop = rc_data + size;

    // Make fargc, fargv
    int i;
    fargv = NULL;
    for (i = 0; i < 2; i++) {
        bol = rc_data;
        fargc = 1;
        while (bol < rc_data_stop) {
            eol = strchr(bol, '\n');
            if (!eol) eol = rc_data_stop - 1;
            if (fargv) {
                *eol = '\0';
                fargv[fargc] = bol;
            }
            fargc += 1;
            bol = eol + 1;
        }
        if (!fargv) {
            if (fargc < 2) break;
            fargv = malloc((fargc + 1) * sizeof(char*));
            fargv[0] = "mle";
            fargv[fargc] = NULL;
        }
    }

    // Parse args
    if (fargv) {
        rv = _editor_init_from_args(editor, fargc, fargv);
        free(fargv);
    }

    free(rc_data);

    return rv;
}

// Parse cli args
static int _editor_init_from_args(editor_t* editor, int argc, char** argv) {
    int rv;
    kmap_t* cur_kmap;
    syntax_t* cur_syntax;
    int c;
    rv = MLE_OK;

    cur_kmap = NULL;
    cur_syntax = NULL;
    optind = 0;
    while (rv == MLE_OK && (c = getopt(argc, argv, "ha:bc:K:k:l:M:m:n:S:s:t:vx:y:z:")) != -1) {
        switch (c) {
            case 'h':
                printf("mle version %s\n\n", MLE_VERSION);
                printf("Usage: mle [options] [file:line]...\n\n");
                printf("    -h           Show this message\n");
                printf("    -a <1|0>     Enable/disable tab_to_space (default: %d)\n", MLE_DEFAULT_TAB_TO_SPACE);
                printf("    -b           Highlight bracket pairs\n");
                printf("    -c <column>  Color column\n");
                printf("    -K <kdef>    Set current kmap definition (use with -k)\n");
                printf("    -k <kbind>   Add key binding to current kmap definition (use with -K)\n");
                printf("    -l <ltype>   Set linenum type (default: 0)\n");
                printf("    -M <macro>   Add a macro\n");
                printf("    -m <key>     Set macro toggle key (default: %s)\n", MLE_DEFAULT_MACRO_TOGGLE_KEY);
                printf("    -n <kmap>    Set init kmap (default: mle_normal)\n");
                printf("    -S <syndef>  Set current syntax definition (use with -s)\n");
                printf("    -s <synrule> Add syntax rule to current syntax definition (use with -S)\n");
                printf("    -t <size>    Set tab size (default: %d)\n", MLE_DEFAULT_TAB_WIDTH);
                printf("    -v           Print version and exit\n");
                printf("    -x <script>  Execute user script\n");
                printf("    -y <syntax>  Set override syntax for files opened at start up\n");
                printf("    -z <1|0>     Enable/disable trim_paste (default: %d)\n\n", MLE_DEFAULT_TRIM_PASTE);
                printf("    file         At start up, open file\n");
                printf("    file:line    At start up, open file at line\n");
                printf("    kdef         '<name>,<default_cmd>,<allow_fallthru>'\n");
                printf("    kbind        '<cmd>,<key>'\n");
                printf("    ltype        0=absolute, 1=relative, 2=both\n");
                printf("    macro        '<name> <key1> <key2> ... <keyN>'\n");
                printf("    syndef       '<name>,<path_pattern>'\n");
                printf("    synrule      '<start>,<end>,<fg>,<bg>'\n");
                rv = MLE_ERR;
                break;
            case 'a':
                editor->tab_to_space = atoi(optarg) ? 1 : 0;
                break;
            case 'b':
                editor->highlight_bracket_pairs = 1;
                break;
            case 'c':
                editor->color_col = atoi(optarg);
                break;
            case 'K':
                if (_editor_init_kmap_by_str(editor, &cur_kmap, optarg) != MLE_OK) {
                    MLE_LOG_ERR("Could not init kmap by str: %s\n", optarg);
                    editor->exit_code = EXIT_FAILURE;
                    rv = MLE_ERR;
                }
                break;
            case 'k':
                if (!cur_kmap || _editor_init_kmap_add_binding_by_str(editor, cur_kmap, optarg) != MLE_OK) {
                    MLE_LOG_ERR("Could not add key binding to kmap %p by str: %s\n", cur_kmap, optarg);
                    editor->exit_code = EXIT_FAILURE;
                    rv = MLE_ERR;
                }
                break;
            case 'l':
                editor->linenum_type = atoi(optarg);
                if (editor->linenum_type < 0 || editor->linenum_type > 2) editor->linenum_type = 0;
                break;
            case 'M':
                if (_editor_add_macro_by_str(editor, optarg) != MLE_OK) {
                    MLE_LOG_ERR("Could not add macro by str: %s\n", optarg);
                    editor->exit_code = EXIT_FAILURE;
                    rv = MLE_ERR;
                }
                break;
            case 'm':
                if (editor_set_macro_toggle_key(editor, optarg) != MLE_OK) {
                    MLE_LOG_ERR("Could not set macro key to: %s\n", optarg);
                    editor->exit_code = EXIT_FAILURE;
                    rv = MLE_ERR;
                }
                break;
            case 'n':
                editor->kmap_init_name = strdup(optarg);
                break;
            case 'S':
                if (_editor_init_syntax_by_str(editor, &cur_syntax, optarg) != MLE_OK) {
                    MLE_LOG_ERR("Could not init syntax by str: %s\n", optarg);
                    editor->exit_code = EXIT_FAILURE;
                    rv = MLE_ERR;
                }
                break;
            case 's':
                if (!cur_syntax || _editor_init_syntax_add_rule_by_str(cur_syntax, optarg) != MLE_OK) {
                    MLE_LOG_ERR("Could not add style rule to syntax %p by str: %s\n", cur_syntax, optarg);
                    editor->exit_code = EXIT_FAILURE;
                    rv = MLE_ERR;
                }
                break;
            case 't':
                editor->tab_width = atoi(optarg);
                break;
            case 'v':
                printf("mle version %s\n", MLE_VERSION);
                rv = MLE_ERR;
                break;
            case 'y':
                editor->syntax_override = optarg;
                break;
            case 'z':
                editor->trim_paste = atoi(optarg) ? 1 : 0;
                break;
            default:
                rv = MLE_ERR;
                break;
        }
    }

    return rv;
}

// Init status bar
static void _editor_init_status(editor_t* editor) {
    editor->status = bview_new(editor, NULL, 0, NULL);
    editor->status->type = MLE_BVIEW_TYPE_STATUS;
    editor->rect_status.fg = TB_WHITE;
    editor->rect_status.bg = TB_BLACK | TB_BOLD;
}

// Init bviews
static void _editor_init_bviews(editor_t* editor, int argc, char** argv) {
    int i;
    char* colon;
    char *path;
    int path_len;

    // Open bviews
    if (optind >= argc) {
        // Open blank
        editor_open_bview(editor, NULL, MLE_BVIEW_TYPE_EDIT, NULL, 0, 1, 0, &editor->rect_edit, NULL, NULL);
    } else {
        // Open files
        for (i = optind; i < argc; i++) {
            path = argv[i];
            path_len = strlen(path);
            if (util_is_file(path, NULL, NULL) || util_is_dir(path)) {
                editor_open_bview(editor, NULL, MLE_BVIEW_TYPE_EDIT, path, path_len, 1, 0, &editor->rect_edit, NULL, NULL);
            } else if ((colon = strrchr(path, ':')) != NULL) {
                path_len = colon - path;
                editor->startup_linenum = strtoul(colon + 1, NULL, 10);
                if (editor->startup_linenum > 0) editor->startup_linenum -= 1;
                editor_open_bview(editor, NULL, MLE_BVIEW_TYPE_EDIT, path, path_len, 1, 0, &editor->rect_edit, NULL, NULL);
            } else {
                editor_open_bview(editor, NULL, MLE_BVIEW_TYPE_EDIT, path, path_len, 1, 0, &editor->rect_edit, NULL, NULL);
            }
        }
    }
}

// Manage async procs, giving priority to user input. Return 1 if drain should
// be called again, else return 0.
static int _editor_drain_async_procs(editor_t* editor) {
    int maxfd;
    fd_set readfds;
    struct timeval timeout;
    struct timeval now;
    async_proc_t* aproc;
    async_proc_t* aproc_tmp;
    char buf[1024 + 1];
    size_t nbytes;
    int rc;
    int is_done;

    // Open tty if not already open
    if (!editor->tty) {
        if (!(editor->tty = fopen("/dev/tty", "r"))) {
            // TODO error
            return 0;
        }
        editor->ttyfd = fileno(editor->tty);
    }

    // Set timeout to 1s
    timeout.tv_sec = 1;
    timeout.tv_usec = 0;

    // Add tty to readfds
    FD_ZERO(&readfds);
    FD_SET(editor->ttyfd, &readfds);

    // Add async procs to readfds
    maxfd = editor->ttyfd;
    DL_FOREACH(editor->async_procs, aproc) {
        FD_SET(aproc->pipefd, &readfds);
        if (aproc->pipefd > maxfd) maxfd = aproc->pipefd;
    }

    // Perform select
    rc = select(maxfd + 1, &readfds, NULL, NULL, &timeout);
    gettimeofday(&now, NULL);

    if (rc < 0) {
        return 0; // TODO Display errors
    } else if (rc == 0) {
        return 1; // Nothing to ready, call again
    }

    if (FD_ISSET(editor->ttyfd, &readfds)) {
        // Immediately give priority to user input
        return 0;
    } else {
        // Read async procs
        DL_FOREACH_SAFE(editor->async_procs, aproc, aproc_tmp) {
            // Read and invoke callback
            is_done = 0;
            if (FD_ISSET(aproc->pipefd, &readfds)) {
                nbytes = fread(&buf, sizeof(char), 1024, aproc->pipe);
                if (nbytes > 0) {
                    buf[nbytes] = '\0';
                    aproc->callback(aproc, buf, nbytes, ferror(aproc->pipe), feof(aproc->pipe), 0);
                }
                is_done = ferror(aproc->pipe) || feof(aproc->pipe);
            }

            // Close and free if eof, error, or timeout
            // TODO Alert user when timeout occurs
            if (is_done || aproc->is_done || util_timeval_is_gt(&aproc->timeout, &now)) {
                aproc->callback(aproc, NULL, 0, 0, 0, 1);
                async_proc_destroy(aproc); // Calls DL_DELETE
            }
        }
    }

    return 1;
}

// Init/deinit commands
static int _editor_init_or_deinit_commands(editor_t* editor, int is_deinit) {
    cmd_funcref_t* funcref;
    cmd_funcref_t* tmp;
    HASH_ITER(hh, editor->func_map, funcref, tmp) {
        if (funcref->func_init) {
            funcref->func_init(editor, funcref, is_deinit);
        }
    }
    return MLE_OK;
}
