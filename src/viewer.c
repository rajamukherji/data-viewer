#define _GNU_SOURCE
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <gtk/gtk.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include "libcsv/csv.h"

typedef struct {
	double X, Y;
} point_t;

typedef struct {
	double Min, Max;
} range_t;

typedef struct node_t node_t;
typedef int node_callback_t(void *Data, node_t *Node);

struct node_t {
	node_t *Children[4];
	const char *FileName;
	GdkPixbuf *Pixbuf;
	node_t *CacheNext, *CachePrev;
	double X, Y, R, G, B;
	double Values[];
};

typedef struct viewer_t viewer_t;

#define MAX_CACHED_IMAGES 1024
#define MAX_VISIBLE_IMAGES 64
#define POINT_COLOUR_CHROMA 0.5
#define POINT_COLOUR_SATURATION 0.7
#define POINT_COLOUR_VALUE 0.9

struct viewer_t {
	GtkWidget *PointsWindow;
	GtkWidget *DrawingArea;
	GtkWidget *ResultsWindow;
	GtkWidget *ImagesView;
	GtkWidget *ImagesScrolledArea;
	GtkListStore *ImagesStore;
	node_t *Root;
	cairo_t *Cairo;
	cairo_surface_t *CachedBackground;
	node_t *CacheHead, *CacheTail;
	point_t Min, Max, Scale, DataMin, DataMax, Pointer;
	double PointSize, BoxSize;
	int NumValues, NumCachedImages, NumVisibleImages;
	int XIndex, YIndex, CIndex;
	range_t *Ranges;
};

static void add_node(node_t *Root, node_t *Node) {
	int Index = (Node->Y >= Root->Y) * 2 + (Node->X >= Root->X);
	node_t *Child = Root->Children[Index];
	if (!Child) {
		Root->Children[Index] = Node;
	} else {
		add_node(Child, Node);
	}
}

static int foreach_node(node_t *Root, double X1, double Y1, double X2, double Y2, void *Data, node_callback_t *Callback) {
	if (!Root) return 0;
	if (X2 < Root->X) {
		if (Y2 < Root->Y) {
			return foreach_node(Root->Children[0], X1, Y1, X2, Y2, Data, Callback);
		} else if (Y1 > Root->Y) {
			return foreach_node(Root->Children[2], X1, Y1, X2, Y2, Data, Callback);
		} else {
			return foreach_node(Root->Children[0], X1, Y1, X2, Y2, Data, Callback)
				|| foreach_node(Root->Children[2], X1, Y1, X2, Y2, Data, Callback);
		}
	} else if (X1 > Root->X) {
		if (Y2 < Root->Y) {
			return foreach_node(Root->Children[1], X1, Y1, X2, Y2, Data, Callback);
		} else if (Y1 > Root->Y) {
			return foreach_node(Root->Children[3], X1, Y1, X2, Y2, Data, Callback);
		} else {
			return foreach_node(Root->Children[1], X1, Y1, X2, Y2, Data, Callback)
				|| foreach_node(Root->Children[3], X1, Y1, X2, Y2, Data, Callback);
		}
	} else {
		if (Y2 < Root->Y) {
			return foreach_node(Root->Children[0], X1, Y1, X2, Y2, Data, Callback)
				|| foreach_node(Root->Children[1], X1, Y1, X2, Y2, Data, Callback);
		} else if (Y1 > Root->Y) {
			return foreach_node(Root->Children[2], X1, Y1, X2, Y2, Data, Callback)
				|| foreach_node(Root->Children[3], X1, Y1, X2, Y2, Data, Callback);
		} else {
			return Callback(Data, Root)
				|| foreach_node(Root->Children[0], X1, Y1, X2, Y2, Data, Callback)
				|| foreach_node(Root->Children[1], X1, Y1, X2, Y2, Data, Callback)
				|| foreach_node(Root->Children[2], X1, Y1, X2, Y2, Data, Callback)
				|| foreach_node(Root->Children[3], X1, Y1, X2, Y2, Data, Callback);
		}
	}
}

typedef struct {
	viewer_t *Viewer;
	node_t *Node;
	int Index, Row;
} csv_node_loader_t;

static void load_nodes_field_callback(void *Field, size_t Size, csv_node_loader_t *Loader) {
	if (Loader->Row) {
		if (!Loader->Index) {
			node_t *Node = (node_t *)malloc(sizeof(node_t) + Loader->Viewer->NumValues * sizeof(double));
			memset(Node, 0, sizeof(node_t));
			Node->Children[3] = Loader->Node;
			Loader->Node = Node;
			char *FileName = malloc(Size + 1);
			memcpy(FileName, Field, Size);
			FileName[Size] = 0;
			Node->FileName = FileName;
		} else {
			int Index = Loader->Index - 1;
			double Value = Loader->Node->Values[Index] = atof(Field);
			if (Loader->Viewer->Ranges[Index].Min > Value) Loader->Viewer->Ranges[Index].Min = Value;
			if (Loader->Viewer->Ranges[Index].Max < Value) Loader->Viewer->Ranges[Index].Max = Value;
		}
	}
	++Loader->Index;
}

static void load_nodes_row_callback(int Char, csv_node_loader_t *Loader) {
	if (!Loader->Row) {
		int NumValues = Loader->Viewer->NumValues = Loader->Index - 1;
		range_t *Ranges = Loader->Viewer->Ranges = (range_t *)malloc(NumValues * sizeof(range_t));
		for (int I = 0; I < NumValues; ++I) {
			Ranges[I].Min = INFINITY;
			Ranges[I].Max = -INFINITY;
		}
	}
	Loader->Index = 0;
	++Loader->Row;
	if (Loader->Row % 1000 == 0) printf("Loaded row %d\n", Loader->Row);
}

static void readd_node(node_t *Root, node_t *Node, int XIndex, int YIndex) {
	node_t *Children[4] = {
		Node->Children[0],
		Node->Children[1],
		Node->Children[2],
		Node->Children[3]
	};
	Node->Children[0] = 0;
	Node->Children[1] = 0;
	Node->Children[2] = 0;
	Node->Children[3] = 0;
	Node->X = Node->Values[XIndex];
	Node->Y = Node->Values[YIndex];
	add_node(Root, Node);
	if (Children[0]) readd_node(Root, Children[0], XIndex, YIndex);
	if (Children[1]) readd_node(Root, Children[1], XIndex, YIndex);
	if (Children[2]) readd_node(Root, Children[2], XIndex, YIndex);
	if (Children[3]) readd_node(Root, Children[3], XIndex, YIndex);
}

static void set_viewer_indices(viewer_t *Viewer, int XIndex, int YIndex) {
	Viewer->XIndex = XIndex;
	Viewer->YIndex = YIndex;
	node_t *Root = Viewer->Root;
	node_t *Children[4] = {
		Root->Children[0],
		Root->Children[1],
		Root->Children[2],
		Root->Children[3]
	};
	Root->Children[0] = 0;
	Root->Children[1] = 0;
	Root->Children[2] = 0;
	Root->Children[3] = 0;
	Root->X = Root->Values[XIndex];
	Root->Y = Root->Values[YIndex];
	if (Children[0]) readd_node(Root, Children[0], XIndex, YIndex);
	if (Children[1]) readd_node(Root, Children[1], XIndex, YIndex);
	if (Children[2]) readd_node(Root, Children[2], XIndex, YIndex);
	if (Children[3]) readd_node(Root, Children[3], XIndex, YIndex);
	double RangeX = Viewer->Ranges[XIndex].Max - Viewer->Ranges[XIndex].Min;
	double RangeY = Viewer->Ranges[YIndex].Max - Viewer->Ranges[YIndex].Min;
	Viewer->DataMin.X = Viewer->Ranges[XIndex].Min - RangeX * 0.01;
	Viewer->DataMin.Y = Viewer->Ranges[YIndex].Min - RangeY * 0.01;
	Viewer->DataMax.X = Viewer->Ranges[XIndex].Max + RangeX * 0.01;
	Viewer->DataMax.Y = Viewer->Ranges[YIndex].Max + RangeY * 0.01;
	Viewer->Min = Viewer->DataMin;
	Viewer->Max = Viewer->DataMax;
	int Width = gtk_widget_get_allocated_width(Viewer->DrawingArea);
	int Height = gtk_widget_get_allocated_height(Viewer->DrawingArea);
	Viewer->Scale.X = Width / (Viewer->Max.X - Viewer->Min.X);
	Viewer->Scale.Y = Height / (Viewer->Max.Y - Viewer->Min.Y);
	char Title[64];
	sprintf(Title, "X = %d, Y = %d, Colour = %d", Viewer->XIndex, Viewer->YIndex, Viewer->CIndex);
	gtk_window_set_title(GTK_WINDOW(Viewer->PointsWindow), Title);
}

typedef struct {
	int Index;
	double Min, Range;
} colour_range_t;

static int set_node_colour(colour_range_t *ColourRange, node_t *Node) {
	double Value = Node->Values[ColourRange->Index];
	double H = 6.0 * (Value - ColourRange->Min) / ColourRange->Range;
	/*double F = H - floor(H);
	double P = POINT_COLOUR_VALUE * (1.0 - POINT_COLOUR_SATURATION);
	double Q = POINT_COLOUR_VALUE * (1.0 - POINT_COLOUR_SATURATION * F);
	double T = POINT_COLOUR_VALUE * (1.0 - POINT_COLOUR_SATURATION * (1.0 - F));*/
	if (H < 1.0) {
		Node->R = POINT_COLOUR_VALUE;
		Node->G = POINT_COLOUR_VALUE - POINT_COLOUR_CHROMA * fabs(H - 1.0);
		Node->B = POINT_COLOUR_VALUE - POINT_COLOUR_CHROMA;
	} else if (H < 2.0) {
		Node->R = POINT_COLOUR_VALUE - POINT_COLOUR_CHROMA * fabs(H - 1.0);
		Node->G = POINT_COLOUR_VALUE;
		Node->B = POINT_COLOUR_VALUE - POINT_COLOUR_CHROMA;
	} else if (H < 3.0) {
		Node->R = POINT_COLOUR_VALUE - POINT_COLOUR_CHROMA;
		Node->G = POINT_COLOUR_VALUE;
		Node->B = POINT_COLOUR_VALUE - POINT_COLOUR_CHROMA * fabs(H - 3.0);
	} else if (H < 4.0) {
		Node->R = POINT_COLOUR_VALUE - POINT_COLOUR_CHROMA;
		Node->G = POINT_COLOUR_VALUE - POINT_COLOUR_CHROMA * fabs(H - 3.0);
		Node->B = POINT_COLOUR_VALUE;
	} else if (H < 5.0) {
		Node->R = POINT_COLOUR_VALUE - POINT_COLOUR_CHROMA * fabs(H - 5.0);
		Node->G = POINT_COLOUR_VALUE - POINT_COLOUR_CHROMA;
		Node->B = POINT_COLOUR_VALUE;
	} else {
		Node->R = POINT_COLOUR_VALUE;
		Node->G = POINT_COLOUR_VALUE - POINT_COLOUR_CHROMA;
		Node->B = POINT_COLOUR_VALUE - POINT_COLOUR_CHROMA * fabs(H - 5.0);
	}
	return 0;
}

static void set_viewer_colour_index(viewer_t *Viewer, int Index) {
	Viewer->CIndex = Index;
	colour_range_t ColourRange[1] = {{
		Index,
		Viewer->Ranges[Index].Min,
		Viewer->Ranges[Index].Max - Viewer->Ranges[Index].Min
	}};
	foreach_node(Viewer->Root, -INFINITY, -INFINITY, INFINITY, INFINITY, ColourRange, (node_callback_t *)set_node_colour);
	char Title[64];
	sprintf(Title, "X = %d, Y = %d, Colour = %d", Viewer->XIndex, Viewer->YIndex, Viewer->CIndex);
	gtk_window_set_title(GTK_WINDOW(Viewer->PointsWindow), Title);
}

static void load_nodes(viewer_t *Viewer, const char *CsvFileName) {
	FILE *File = fopen(CsvFileName, "r");
	if (!File) {
		fprintf(stderr, "Error reading from %s\n", CsvFileName);
		exit(1);
	}
	char Buffer[4096];
	struct csv_parser Parser[1];
	csv_init(Parser, 0);
	size_t Count = fread(Buffer, 1, 4096, File);
	csv_node_loader_t Loader[1] = {{Viewer, 0, 0, 0}};
	while (Count > 0) {
		csv_parse(Parser, Buffer, Count, (void *)load_nodes_field_callback, (void *)load_nodes_row_callback, Loader);
		Count = fread(Buffer, 1, 4096, File);
	}
	fclose(File);
	Viewer->Root = Loader->Node;
	set_viewer_indices(Viewer, 0, 1);
	set_viewer_colour_index(Viewer, Viewer->NumValues - 1);
}

static GdkPixbuf *get_node_pixbuf(viewer_t *Viewer, node_t *Node) {
	if (Node->Pixbuf) {
		if (Node->CachePrev) {
			Node->CachePrev->CacheNext = Node->CacheNext;
		} else {
			Viewer->CacheHead = Node->CacheNext;
		}
		if (Node->CacheNext) {
			Node->CacheNext->CachePrev = Node->CachePrev;
		} else {
			Viewer->CacheTail = Node->CachePrev;
		}
		Node->CacheNext = Node->CachePrev = 0;
	} else {
		if (Viewer->NumCachedImages == MAX_CACHED_IMAGES) {
			node_t *LRU = Viewer->CacheHead;
			Viewer->CacheHead = LRU->CacheNext;
			Viewer->CacheHead->CachePrev = 0;
			g_object_unref(LRU->Pixbuf);
			LRU->Pixbuf = 0;
		} else {
			++Viewer->NumCachedImages;
		}
		GError *Error = 0;
		Node->Pixbuf = gdk_pixbuf_new_from_file_at_size(Node->FileName, 128, -1, &Error);
		if (!Node->Pixbuf) {
			guchar *Pixels = malloc(128 * 192 * 4);
			cairo_surface_t *Surface = cairo_image_surface_create_for_data(Pixels, CAIRO_FORMAT_ARGB32, 128, 192, 128 * 4);
			cairo_t *Cairo = cairo_create(Surface);
			cairo_rectangle(Cairo, 0.0, 0.0, 128.0, 192.0);
			cairo_set_source_rgb(Cairo,
				(Node->X - Viewer->DataMin.X) / (Viewer->DataMax.X - Viewer->DataMin.X),
				1.0,
				(Node->Y - Viewer->DataMin.Y) / (Viewer->DataMax.Y - Viewer->DataMin.Y)
			);
			cairo_fill(Cairo);
			cairo_destroy(Cairo);
			cairo_surface_destroy(Surface);
			Node->Pixbuf = gdk_pixbuf_new_from_data(Pixels, GDK_COLORSPACE_RGB, TRUE, 8, 128, 192, 128 * 4, (void *)free, 0);
		}
	}
	if (Viewer->CacheTail) {
		Node->CachePrev = Viewer->CacheTail;
		Viewer->CacheTail->CacheNext = Node;
		Viewer->CacheTail = Node;
	} else {
		Viewer->CacheHead = Viewer->CacheTail = Node;
	}
	return Node->Pixbuf;
}

static int draw_node_image(viewer_t *Viewer, node_t *Node) {
	GdkPixbuf *Pixbuf = get_node_pixbuf(Viewer, Node);
	GtkTreeIter Iter;
	gtk_list_store_insert_with_values(Viewer->ImagesStore, &Iter, -1, 0, Node->FileName, 1, Pixbuf, -1);
	return ++Viewer->NumVisibleImages == MAX_VISIBLE_IMAGES;
}

static void draw_node_images(viewer_t *Viewer) {
	gtk_list_store_clear(Viewer->ImagesStore);
	Viewer->NumVisibleImages = 0;
	double X1 = Viewer->Min.X + (Viewer->Pointer.X - Viewer->BoxSize / 2) / Viewer->Scale.X;
	double Y1 = Viewer->Min.Y + (Viewer->Pointer.Y - Viewer->BoxSize / 2) / Viewer->Scale.Y;
	double X2 = Viewer->Min.X + (Viewer->Pointer.X + Viewer->BoxSize / 2) / Viewer->Scale.X;
	double Y2 = Viewer->Min.Y + (Viewer->Pointer.Y + Viewer->BoxSize / 2) / Viewer->Scale.Y;
	foreach_node(Viewer->Root, X1, Y1, X2, Y2, Viewer, (node_callback_t *)draw_node_image);
}

static int redraw_point(viewer_t *Viewer, node_t *Node) {
	double X = Viewer->Scale.X * (Node->X - Viewer->Min.X);
	double Y = Viewer->Scale.Y * (Node->Y - Viewer->Min.Y);
	cairo_new_path(Viewer->Cairo);
	cairo_rectangle(Viewer->Cairo, X - Viewer->PointSize / 2, Y - Viewer->PointSize / 2, Viewer->PointSize, Viewer->PointSize);
	cairo_set_source_rgb(Viewer->Cairo, Node->R, Node->G, Node->B);
	cairo_fill(Viewer->Cairo);
	return 0;
}

static void redraw_viewer(GtkWidget *Widget, cairo_t *Cairo, viewer_t *Viewer) {
	cairo_set_source_surface(Cairo, Viewer->CachedBackground, 0.0, 0.0);
	cairo_paint(Cairo);
	cairo_new_path(Cairo);
	cairo_rectangle(Cairo,
		Viewer->Pointer.X - Viewer->BoxSize / 2,
		Viewer->Pointer.Y - Viewer->BoxSize / 2,
		Viewer->BoxSize,
		Viewer->BoxSize
	);
	cairo_set_source_rgb(Cairo, 0.5, 0.5, 1.0);
	cairo_stroke_preserve(Cairo);
	cairo_set_source_rgba(Cairo, 0.5, 0.5, 1.0, 0.5);
	cairo_fill(Cairo);
}

static void zoom_viewer(viewer_t *Viewer, double X, double Y, double Zoom) {
	double XMin = X - Zoom * (X - Viewer->Min.X);
	double XMax = X + Zoom * (Viewer->Max.X - X);
	double YMin = Y - Zoom * (Y - Viewer->Min.Y);
	double YMax = Y + Zoom * (Viewer->Max.Y - Y);
	if (XMin < Viewer->DataMin.X) XMin = Viewer->DataMin.X;
	if (XMax > Viewer->DataMax.X) XMax = Viewer->DataMax.X;
	if (YMin < Viewer->DataMin.Y) YMin = Viewer->DataMin.Y;
	if (YMax > Viewer->DataMax.Y) YMax = Viewer->DataMax.Y;
	Viewer->Min.X = XMin;
	Viewer->Max.X = XMax;
	Viewer->Min.Y = YMin;
	Viewer->Max.Y = YMax;
	guint Width = gtk_widget_get_allocated_width(Viewer->DrawingArea);
	guint Height = gtk_widget_get_allocated_height(Viewer->DrawingArea);
	Viewer->Scale.X = Width / (Viewer->Max.X - Viewer->Min.X);
	Viewer->Scale.Y = Height / (Viewer->Max.Y - Viewer->Min.Y);
}

static void pan_viewer(viewer_t *Viewer, double DeltaX, double DeltaY) {
	if (DeltaX < 0) {
		double XMin = Viewer->Min.X + DeltaX;
		if (XMin < Viewer->DataMin.X) XMin = Viewer->DataMin.X;
		double XMax = XMin + (Viewer->Max.X - Viewer->Min.X);
		Viewer->Min.X = XMin;
		Viewer->Max.X = XMax;
	} else {
		double XMax = Viewer->Max.X + DeltaX;
		if (XMax > Viewer->DataMax.X) XMax = Viewer->DataMax.X;
		double XMin = XMax - (Viewer->Max.X - Viewer->Min.X);
		Viewer->Min.X = XMin;
		Viewer->Max.X = XMax;
	}
	if (DeltaY < 0) {
		double YMin = Viewer->Min.Y + DeltaY;
		if (YMin < Viewer->DataMin.Y) YMin = Viewer->DataMin.Y;
		double YMax = YMin + (Viewer->Max.Y - Viewer->Min.Y);
		Viewer->Min.Y = YMin;
		Viewer->Max.Y = YMax;
	} else {
		double YMax = Viewer->Max.Y + DeltaY;
		if (YMax > Viewer->DataMax.Y) YMax = Viewer->DataMax.Y;
		double YMin = YMax - (Viewer->Max.Y - Viewer->Min.Y);
		Viewer->Min.Y = YMin;
		Viewer->Max.Y = YMax;
	}
}

static void redraw_viewer_background(viewer_t *Viewer) {
	guint Width = cairo_image_surface_get_width(Viewer->CachedBackground);
	guint Height = cairo_image_surface_get_height(Viewer->CachedBackground);
	cairo_t *Cairo = cairo_create(Viewer->CachedBackground);
	cairo_set_source_rgb(Cairo, 1.0, 1.0, 1.0);
	cairo_rectangle(Cairo, 0.0, 0.0, Width, Height);
	cairo_fill(Cairo);
	Viewer->Cairo = Cairo;
	foreach_node(Viewer->Root, Viewer->Min.X, Viewer->Min.Y, Viewer->Max.X, Viewer->Max.Y, Viewer, (node_callback_t *)redraw_point);
	Viewer->Cairo = 0;
	cairo_destroy(Cairo);
}

static void resize_viewer(GtkWidget *Widget, GdkRectangle *Allocation, viewer_t *Viewer) {
	Viewer->Scale.X = Allocation->width / (Viewer->Max.X - Viewer->Min.X);
	Viewer->Scale.Y = Allocation->height / (Viewer->Max.Y - Viewer->Min.Y);
	if (Viewer->CachedBackground) {
		cairo_surface_destroy(Viewer->CachedBackground);
	}
	/*double CacheWidth = (Viewer->DataMax.X - Viewer->DataMin.X) * Viewer->Scale.X;
	double CacheHeight = (Viewer->DataMax.Y - Viewer->DataMin.Y) * Viewer->Scale.Y;
	if (CacheWidth > 2 * Allocation->width) CacheWidth = 2 * Allocation->width;
	if (CacheHeight > 2 * Allocation->height) CacheHeight = 2 * Allocation->height;
	Viewer->CachedBackground = cairo_image_surface_create(CAIRO_FORMAT_RGB24, CacheWidth, CacheHeight);*/
	Viewer->CachedBackground = cairo_image_surface_create(CAIRO_FORMAT_RGB24, Allocation->width, Allocation->height);
	redraw_viewer_background(Viewer);
}

static gboolean scroll_viewer(GtkWidget *Widget, GdkEventScroll *Event, viewer_t *Viewer) {
	double X = Viewer->Min.X + (Event->x / Viewer->Scale.X);
	double Y = Viewer->Min.Y + (Event->y / Viewer->Scale.Y);
	if (Event->direction == GDK_SCROLL_DOWN) {
		zoom_viewer(Viewer, X, Y, 1.1);
	} else if (Event->direction == GDK_SCROLL_UP) {
		zoom_viewer(Viewer, X, Y, 1.0 / 1.1);
	}
	redraw_viewer_background(Viewer);
	gtk_widget_queue_draw(Widget);
	draw_node_images(Viewer);
	return FALSE;
}

static gboolean button_press_viewer(GtkWidget *Widget, GdkEventButton *Event, viewer_t *Viewer) {
	if (Event->button == 1) {
		Viewer->Pointer.X = Event->x;
		Viewer->Pointer.Y = Event->y;
	}
	return FALSE;
}

static gboolean motion_notify_viewer(GtkWidget *Widget, GdkEventMotion *Event, viewer_t *Viewer) {
	if (Event->state & GDK_BUTTON1_MASK) {
		double DeltaX = (Viewer->Pointer.X - Event->x) / Viewer->Scale.X;
		double DeltaY = (Viewer->Pointer.Y - Event->y) / Viewer->Scale.Y;
		pan_viewer(Viewer, DeltaX, DeltaY);
		redraw_viewer_background(Viewer);
	}
	Viewer->Pointer.X = Event->x;
	Viewer->Pointer.Y = Event->y;
	gtk_widget_queue_draw(Widget);
	draw_node_images(Viewer);
	return FALSE;
}

static gboolean key_press_viewer(GtkWidget *Widget, GdkEventKey *Event, viewer_t *Viewer) {
	switch (Event->keyval) {
	case GDK_KEY_x: {
		set_viewer_indices(Viewer, (Viewer->XIndex + 1) % Viewer->NumValues, Viewer->YIndex);
		redraw_viewer_background(Viewer);
		gtk_widget_queue_draw(Widget);
		draw_node_images(Viewer);
		break;
	}
	case GDK_KEY_y: {
		set_viewer_indices(Viewer, Viewer->XIndex, (Viewer->YIndex + 1) % Viewer->NumValues);
		redraw_viewer_background(Viewer);
		gtk_widget_queue_draw(Widget);
		draw_node_images(Viewer);
		break;
	}
	case GDK_KEY_c: {
		set_viewer_colour_index(Viewer, (Viewer->CIndex + 1) % Viewer->NumValues);
		redraw_viewer_background(Viewer);
		gtk_widget_queue_draw(Widget);
		draw_node_images(Viewer);
		break;
	}
	}
	return FALSE;
}

static viewer_t *create_viewer(const char *CsvFileName) {
	viewer_t *Viewer = (viewer_t *)malloc(sizeof(viewer_t));
	Viewer->PointSize = 5.0;
	Viewer->BoxSize = 40.0;
	Viewer->PointsWindow = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	Viewer->DrawingArea = gtk_drawing_area_new();
	Viewer->CachedBackground = 0;
	Viewer->CacheHead = Viewer->CacheTail = 0;
	Viewer->NumCachedImages = 0;
	Viewer->ImagesScrolledArea = gtk_scrolled_window_new(0, 0);
	Viewer->ImagesStore = gtk_list_store_new(2, G_TYPE_STRING, GDK_TYPE_PIXBUF);
	Viewer->ImagesView = gtk_icon_view_new_with_model(GTK_TREE_MODEL(Viewer->ImagesStore));
	Viewer->ResultsWindow = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	gtk_icon_view_set_text_column(GTK_ICON_VIEW(Viewer->ImagesView), 0);
	gtk_icon_view_set_pixbuf_column(GTK_ICON_VIEW(Viewer->ImagesView), 1);
	gtk_icon_view_set_item_width(GTK_ICON_VIEW(Viewer->ImagesView), 72);
	gtk_widget_add_events(Viewer->DrawingArea, GDK_SCROLL_MASK);
	gtk_widget_add_events(Viewer->DrawingArea, GDK_POINTER_MOTION_MASK);
	gtk_widget_add_events(Viewer->DrawingArea, GDK_BUTTON_PRESS_MASK);
	gtk_widget_add_events(Viewer->DrawingArea, GDK_BUTTON_RELEASE_MASK);
	gtk_widget_add_events(Viewer->DrawingArea, GDK_KEY_PRESS);
	gtk_widget_set_can_focus(Viewer->DrawingArea, TRUE);
	gtk_container_add(GTK_CONTAINER(Viewer->PointsWindow), Viewer->DrawingArea);
	gtk_container_add(GTK_CONTAINER(Viewer->ImagesScrolledArea), Viewer->ImagesView);
	gtk_container_add(GTK_CONTAINER(Viewer->ResultsWindow), Viewer->ImagesScrolledArea);
	g_signal_connect(G_OBJECT(Viewer->DrawingArea), "draw", G_CALLBACK(redraw_viewer), Viewer);
	g_signal_connect(G_OBJECT(Viewer->DrawingArea), "size-allocate", G_CALLBACK(resize_viewer), Viewer);
	g_signal_connect(G_OBJECT(Viewer->DrawingArea), "scroll-event", G_CALLBACK(scroll_viewer), Viewer);
	g_signal_connect(G_OBJECT(Viewer->DrawingArea), "button-press-event", G_CALLBACK(button_press_viewer), Viewer);
	g_signal_connect(G_OBJECT(Viewer->DrawingArea), "motion-notify-event", G_CALLBACK(motion_notify_viewer), Viewer);
	g_signal_connect(G_OBJECT(Viewer->DrawingArea), "key-press-event", G_CALLBACK(key_press_viewer), Viewer);
	g_signal_connect(G_OBJECT(Viewer->ResultsWindow), "destroy", G_CALLBACK(gtk_main_quit), 0);
	g_signal_connect(G_OBJECT(Viewer->PointsWindow), "destroy", G_CALLBACK(gtk_main_quit), 0);
	load_nodes(Viewer, CsvFileName);
	gtk_window_resize(GTK_WINDOW(Viewer->PointsWindow), 640, 480);
	gtk_widget_show_all(Viewer->PointsWindow);
	gtk_window_resize(GTK_WINDOW(Viewer->ResultsWindow), 640, 480);
	gtk_widget_show_all(Viewer->ResultsWindow);
	return Viewer;
}

int main(int Argc, char *Argv[]) {
	gtk_init(&Argc, &Argv);
	viewer_t *Viewer = create_viewer(Argv[1]);
	gtk_main();
	return 0;
}
