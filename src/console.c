#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <gtksourceview/gtksource.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include "libcsv/csv.h"
#include <gc/gc.h>
#include <minilang.h>
#include <stringmap.h>
#include <ml_compiler.h>

#include "console.h"

static ml_value_t *StringMethod;

struct console_t {
	GtkWidget *Container, *LogScrolled, *LogView, *InputView;
	ml_getter_t ParentGetter;
	void *ParentGlobals;
	mlc_scanner_t *Scanner;
	char *Input;
	stringmap_t Globals[1];
};

static ml_value_t *console_global_get(console_t *Console, const char *Name) {
return stringmap_search(Console->Globals, Name) ?: (Console->ParentGetter)(Console->ParentGlobals, Name);
}

static char *console_read(console_t *Console) {
	if (!Console->Input) return 0;
	char *Line = Console->Input;
	for (char *End = Console->Input; *End; ++End) {
		if (*End == '\n') {
			*End = 0;
			Console->Input = End + 1;
			printf("Line = <%s>\n", Line);
			return Line;
		}
	}
	Console->Input = 0;
	printf("Line = <%s>\n", Line);
	return Line;
}

void console_log(console_t *Console, ml_value_t *Value) {
	GtkTextIter End[1];
	GtkTextBuffer *LogBuffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(Console->LogView));
	gtk_text_buffer_get_end_iter(LogBuffer, End);
	if (Value->Type == MLErrorT) {
		char *Buffer;
		int Length = asprintf(&Buffer, "Error: %s\n", ml_error_message(Value));
		gtk_text_buffer_insert(LogBuffer, End, Buffer, Length);
		const char *Source;
		int Line;
		for (int I = 0; ml_error_trace(Value, I, &Source, &Line); ++I) {
			Length = asprintf(&Buffer, "\t%s:%d\n", Source, Line);
			gtk_text_buffer_insert(LogBuffer, End, Buffer, Length);
		}
	} else {
		ml_value_t *String = ml_call(StringMethod, 1, &Value);
		char *Buffer;
		int Length;
		if (String->Type == MLStringT) {
			Length = asprintf(&Buffer, "%s\n", ml_string_value(String));
		} else {
			Length = asprintf(&Buffer, "<%s>\n", Value->Type->Name);
		}
		gtk_text_buffer_insert(LogBuffer, End, Buffer, Length);
	}
	gtk_text_buffer_insert(LogBuffer, End, "\n", -1);
}

static void console_submit(GtkWidget *Button, console_t *Console) {
	GtkTextBuffer *InputBuffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(Console->InputView));
	GtkTextIter Start[1], End[1];
	gtk_text_buffer_get_bounds(InputBuffer, Start, End);
	Console->Input = gtk_text_buffer_get_text(InputBuffer, Start, End, FALSE);
	gtk_text_buffer_set_text(InputBuffer, "", 0);

	GtkTextBuffer *LogBuffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(Console->LogView));
	gtk_text_buffer_get_end_iter(LogBuffer, End);
	gtk_text_buffer_insert(LogBuffer, End, Console->Input, -1);
	gtk_text_buffer_insert(LogBuffer, End, "\n", -1);

	mlc_scanner_t *Scanner = Console->Scanner;
	mlc_function_t Function[1] = {{(void *)console_global_get, Console, NULL,}};
	SHA256_CTX HashContext[1];
	sha256_init(HashContext);
	if (setjmp(Scanner->OnError)) {
		char *Buffer;
		int Length = asprintf(&Buffer, "Error: %s\n", ml_error_message(Scanner->Error));
		gtk_text_buffer_insert(LogBuffer, End, Buffer, Length);
		const char *Source;
		int Line;
		for (int I = 0; ml_error_trace(Scanner->Error, I, &Source, &Line); ++I) printf("\t%s:%d\n", Source, Line);
		Scanner->Token = MLT_NONE;
		Scanner->Next = "";
	} else {
		for (;;) {
			mlc_expr_t *Expr = ml_accept_command(Scanner, Console->Globals);
			if (Expr == (mlc_expr_t *)-1) break;
			mlc_compiled_t Compiled = ml_compile(Function, Expr, HashContext);
			mlc_connect(Compiled.Exits, NULL);
			ml_closure_t *Closure = new(ml_closure_t);
			ml_closure_info_t *Info = Closure->Info = new(ml_closure_info_t);
			Closure->Type = MLClosureT;
			Info->Entry = Compiled.Start;
			Info->FrameSize = Function->Size;
			ml_value_t *Result = ml_closure_call((ml_value_t *)Closure, 0, NULL);
			console_log(Console, Result);
		}
		GtkAdjustment *Adjustment = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(Console->LogScrolled));
		gtk_adjustment_set_value(Adjustment, gtk_adjustment_get_upper(Adjustment));
	}
}

GtkWidget *console_get_widget(console_t *Console) {
	return Console->Container;
}

void console_append(console_t *Console, const char *Buffer, int Length) {
	GtkTextIter End[1];
	GtkTextBuffer *LogBuffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(Console->LogView));
	gtk_text_buffer_get_end_iter(LogBuffer, End);
	gtk_text_buffer_insert(LogBuffer, End, Buffer, Length);
}

ml_value_t *console_print(console_t *Console, int Count, ml_value_t **Args) {
	ml_value_t *StringMethod = ml_method("string");
	GtkTextIter End[1];
	GtkTextBuffer *LogBuffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(Console->LogView));
	gtk_text_buffer_get_end_iter(LogBuffer, End);
	for (int I = 0; I < Count; ++I) {
		ml_value_t *Result = Args[I];
		if (Result->Type != MLStringT) {
			Result = ml_call(StringMethod, 1, &Result);
			if (Result->Type == MLErrorT) return Result;
			if (Result->Type != MLStringT) return ml_error("ResultError", "string method did not return string");
		}
		gtk_text_buffer_insert(LogBuffer, End, ml_string_value(Result), ml_string_length(Result));
	}
	return MLNil;
}

console_t *console_new(ml_getter_t GlobalGet, void *Globals) {
	StringMethod = ml_method("string");
	console_t *Console = new(console_t);
	Console->ParentGetter = GlobalGet;
	Console->ParentGlobals = Globals;
	Console->Scanner = ml_scanner("Console", Console, (void *)console_read);
	Console->Container = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
	Console->LogView = gtk_text_view_new();
	gtk_text_view_set_editable(GTK_TEXT_VIEW(Console->LogView), FALSE);
	Console->InputView = gtk_source_view_new();
	Console->Input = 0;
	GtkWidget *InputPanel = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	GtkWidget *SubmitButton = gtk_button_new();
	gtk_button_set_image(GTK_BUTTON(SubmitButton), gtk_image_new_from_icon_name("go-jump-symbolic", GTK_ICON_SIZE_BUTTON));
	Console->LogScrolled = gtk_scrolled_window_new(NULL, NULL);
	gtk_container_add(GTK_CONTAINER(Console->LogScrolled), Console->LogView);
	gtk_box_pack_start(GTK_BOX(InputPanel), Console->InputView, TRUE, TRUE, 2);
	gtk_box_pack_start(GTK_BOX(InputPanel), SubmitButton, FALSE, FALSE, 2);
	gtk_box_pack_start(GTK_BOX(Console->Container), Console->LogScrolled, TRUE, TRUE, 2);
	GtkWidget *InputFrame = gtk_frame_new(NULL);
	gtk_container_add(GTK_CONTAINER(InputFrame), InputPanel);
	gtk_box_pack_start(GTK_BOX(Console->Container), InputFrame, FALSE, TRUE, 2);
	g_signal_connect(G_OBJECT(SubmitButton), "clicked", G_CALLBACK(console_submit), Console);
	return Console;
}
