#define _GNU_SOURCE
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <gtk/gtk.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include "libcsv/csv.h"

#define MAX_CACHED_IMAGES 1024
#define MAX_VISIBLE_IMAGES 64
#define POINT_COLOUR_CHROMA 0.5
#define POINT_COLOUR_SATURATION 0.7
#define POINT_COLOUR_VALUE 0.9

typedef struct node_t node_t;
typedef int node_callback_t(void *Data, node_t *Node);
typedef struct field_t field_t;
typedef struct viewer_t viewer_t;

typedef struct {
	double X, Y;
} point_t;

typedef struct {
	double Min, Max;
} range_t;

struct node_t {
	node_t *Children[4];
	const char *FileName;
	GdkPixbuf *Pixbuf;
	node_t *CacheNext, *CachePrev;
	double X, Y, R, G, B;
};

struct field_t {
	const char *Name;
	union {
		GHashTable *EnumHash;
		GtkListStore *EnumStore;
	};
	const char **EnumNames;
	range_t Range;
	double Values[];
};

struct viewer_t {
	GtkWidget *MainWindow;
	GtkWidget *DrawingArea;
	GtkWidget *ImagesView;
	GtkWidget *ImagesScrolledArea;
	GtkWidget *XComboBox, *YComboBox, *CComboBox, *EditFieldComboBox, *EditValueComboBox;
	GtkListStore *ImagesStore;
	GtkListStore *FieldsStore;
	node_t *Root, *Nodes;
	cairo_t *Cairo;
	cairo_surface_t *CachedBackground;
	node_t *CacheHead, *CacheTail;
	field_t **Fields, *EditField;
	point_t Min, Max, Scale, DataMin, DataMax, Pointer;
	double PointSize, BoxSize, EditValue;
	int NumNodes, NumFields, NumCachedImages, NumVisibleImages;
	int XIndex, YIndex, CIndex;
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

static void set_viewer_indices(viewer_t *Viewer, int XIndex, int YIndex) {
	Viewer->XIndex = XIndex;
	Viewer->YIndex = YIndex;
	int NumNodes = Viewer->NumNodes;
	field_t *XField = Viewer->Fields[XIndex];
	field_t *YField = Viewer->Fields[YIndex];

	node_t *Node = Viewer->Nodes;
	double *XValue = XField->Values;
	double *YValue = YField->Values;
	for (int I = NumNodes; --I >= 0;) {
		Node->X = *XValue;
		Node->Y = *YValue;
		Node->Children[0] = Node->Children[1] = Node->Children[2] = Node->Children[3] = 0;
		++Node;
		++XValue;
		++YValue;
	}

	node_t *Root = Viewer->Root = Node = Viewer->Nodes;
	for (int I = NumNodes - 1; --I >= 0;) add_node(Root, ++Node);

	double RangeX = XField->Range.Max - XField->Range.Min;
	double RangeY = YField->Range.Max - YField->Range.Min;
	Viewer->DataMin.X = XField->Range.Min - RangeX * 0.01;
	Viewer->DataMin.Y = YField->Range.Min - RangeY * 0.01;
	Viewer->DataMax.X = XField->Range.Max + RangeX * 0.01;
	Viewer->DataMax.Y = YField->Range.Max + RangeY * 0.01;
	Viewer->Min = Viewer->DataMin;
	Viewer->Max = Viewer->DataMax;
	int Width = gtk_widget_get_allocated_width(Viewer->DrawingArea);
	int Height = gtk_widget_get_allocated_height(Viewer->DrawingArea);
	Viewer->Scale.X = Width / (Viewer->Max.X - Viewer->Min.X);
	Viewer->Scale.Y = Height / (Viewer->Max.Y - Viewer->Min.Y);
}

static inline void set_node_rgb(node_t *Node, double H) {
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
}

static void set_viewer_colour_index(viewer_t *Viewer, int CIndex) {
	Viewer->CIndex = CIndex;
	int NumNodes = Viewer->NumNodes;
	field_t *CField = Viewer->Fields[CIndex];
	double Min = CField->Range.Min;
	double Range = CField->Range.Max - Min;
	if (Range <= 1.0e-6) Range = 1.0;
	node_t *Node = Viewer->Nodes;
	double *CValue = CField->Values;
	if (CField->EnumStore) {
		for (int I = NumNodes; --I >= 0;) {
			if (*CValue > 0.0) {
				set_node_rgb(Node, 6.0 * (*CValue - Min) / Range);
			} else {
				Node->R = Node->G = Node->B = 0.0;
			}
			++Node;
			++CValue;
		}
	} else {
		for (int I = NumNodes; --I >= 0;) {
			set_node_rgb(Node, 6.0 * (*CValue - Min) / Range);
			++Node;
			++CValue;
		}
	}
}

typedef struct {
	viewer_t *Viewer;
	field_t **Fields;
	node_t *Nodes;
	int Index, Row;
} csv_node_loader_t;

static void load_nodes_field_callback(void *Text, size_t Size, csv_node_loader_t *Loader) {
	if (!Loader->Row) {
		if (Loader->Index) {
			char *FieldName = malloc(Size + 1);
			memcpy(FieldName, Text, Size);
			FieldName[Size] = 0;
			Loader->Fields[Loader->Index - 1]->Name = FieldName;
		}
	} else {
		if (!Loader->Index) {
			char *FileName = malloc(Size + 1);
			memcpy(FileName, Text, Size);
			FileName[Size] = 0;
			Loader->Nodes[Loader->Row - 1].FileName = FileName;
		} else {
			int Index = Loader->Index - 1;
			field_t *Field = Loader->Fields[Index];
			double Value;
			insert:
			if (Field->EnumHash) {
				char *EnumName = malloc(Size + 1);
				memcpy(EnumName, Text, Size);
				EnumName[Size] = 0;
				gpointer *Ref = g_hash_table_lookup(Field->EnumHash, EnumName);
				if (Ref) {
					Value = *(double *)Ref;
					free(EnumName);
				} else {
					Ref = (void *)&Field->Values[Loader->Row - 1];
					g_hash_table_insert(Field->EnumHash, EnumName, Ref);
					Value = g_hash_table_size(Field->EnumHash);
					const char **EnumNames = (const char **)malloc((Value + 1) * sizeof(const char *));
					memcpy(EnumNames, Field->EnumNames, Value * sizeof(const char *));
					EnumNames[(int)Value] = EnumName;
					free(Field->EnumNames);
					Field->EnumNames = EnumNames;
				}
			} else {
				char *End;
				Value = strtod(Text, &End);
				if (End == Text) {
					if (Loader->Row != 1) {
						// TODO: Convert all previously loaded values to enums
						fprintf(stderr, "Convert previous values into enums has not been done yet!");
						exit(1);
					}
					Field->Range.Min = 0.0;
					Field->EnumHash = g_hash_table_new(g_str_hash, g_str_equal);
					Field->EnumNames = (const char **)malloc(sizeof(const char *));
					Field->EnumNames[0] = "";
					goto insert;
				}
			}
			Field->Values[Loader->Row - 1] = Value;
			if (Field->Range.Min > Value) Field->Range.Min = Value;
			if (Field->Range.Max < Value) Field->Range.Max = Value;
		}
	}
	++Loader->Index;
}

static void load_nodes_row_callback(int Char, csv_node_loader_t *Loader) {
	Loader->Index = 0;
	++Loader->Row;
	if (Loader->Row % 10000 == 0) printf("Loaded row %d\n", Loader->Row);
}

static void count_nodes_field_callback(void *Text, size_t Size, csv_node_loader_t *Loader) {
	++Loader->Index;
}

static void count_nodes_row_callback(int Char, csv_node_loader_t *Loader) {
	if (!Loader->Row) {
		int NumValues = Loader->Viewer->NumFields = Loader->Index - 1;
	}
	Loader->Index = 0;
	++Loader->Row;
	if (Loader->Row % 10000 == 0) printf("Counted row %d\n", Loader->Row);
}

static void create_field_enum_store(const char *Name, double *Value, GtkListStore *Store) {
	gtk_list_store_insert_with_values(Store, 0, -1, 0, Name, 1, Value[0], -1);
}

static void viewer_open_file(viewer_t *Viewer, const char *CsvFileName) {
	csv_node_loader_t Loader[1] = {{Viewer, 0, 0, 0, 0}};
	char Buffer[4096];
	struct csv_parser Parser[1];

	printf("Counting rows...\n");
	FILE *File = fopen(CsvFileName, "r");
	if (!File) {
		fprintf(stderr, "Error reading from %s\n", CsvFileName);
		exit(1);
	}
	csv_init(Parser, 0);
	size_t Count = fread(Buffer, 1, 4096, File);
	while (Count > 0) {
		csv_parse(Parser, Buffer, Count, (void *)count_nodes_field_callback, (void *)count_nodes_row_callback, Loader);
		Count = fread(Buffer, 1, 4096, File);
	}
	fclose(File);
	csv_fini(Parser, (void *)count_nodes_field_callback, (void *)count_nodes_row_callback, Loader);
	csv_free(Parser);

	int NumNodes = Viewer->NumNodes = Loader->Row - 1;
	int NumFields = Viewer->NumFields;
	node_t *Nodes = Viewer->Nodes = (node_t *)malloc(NumNodes * sizeof(node_t));
	memset(Nodes, 0, NumNodes * sizeof(node_t));
	field_t **Fields = Viewer->Fields = (field_t **)malloc(NumFields * sizeof(field_t *));
	for (int I = 0; I < NumFields; ++I) {
		field_t *Field = Fields[I] = (field_t *)malloc(sizeof(field_t) + NumNodes * sizeof(double));
		Field->EnumHash = 0;
		Field->Range.Min = INFINITY;
		Field->Range.Max = -INFINITY;
		Fields[I] = Field;
	}

	printf("Loading rows...\n");
	fopen(CsvFileName, "r");
	Loader->Nodes = Nodes;
	Loader->Fields = Fields;
	Loader->Row = Loader->Index = 0;
	csv_init(Parser, 0);
	Count = fread(Buffer, 1, 4096, File);
	while (Count > 0) {
		csv_parse(Parser, Buffer, Count, (void *)load_nodes_field_callback, (void *)load_nodes_row_callback, Loader);
		Count = fread(Buffer, 1, 4096, File);
	}
	fclose(File);
	csv_fini(Parser, (void *)load_nodes_field_callback, (void *)load_nodes_row_callback, Loader);
	csv_free(Parser);

	for (int I = 0; I < NumFields; ++I) {
		field_t *Field = Fields[I];
		gtk_list_store_insert_with_values(Viewer->FieldsStore, 0, -1, 0, Field->Name, 1, I, -1);
		if (Field->EnumHash) {
			GHashTable *EnumHash = Field->EnumHash;
			GtkListStore *EnumStore = Field->EnumStore = gtk_list_store_new(2, G_TYPE_STRING, G_TYPE_DOUBLE);
			g_hash_table_foreach(EnumHash, (GHFunc)create_field_enum_store, EnumStore);
		}
	}

	set_viewer_indices(Viewer, 0, 1);
	set_viewer_colour_index(Viewer, Viewer->NumFields - 1);
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
	++Viewer->NumVisibleImages;
	if (Viewer->NumVisibleImages <= MAX_VISIBLE_IMAGES) {
		GdkPixbuf *Pixbuf = get_node_pixbuf(Viewer, Node);
		gtk_list_store_insert_with_values(Viewer->ImagesStore, 0, -1, 0, Node->FileName, 1, Pixbuf, -1);
	}
	return 0;
}

static void draw_node_images(viewer_t *Viewer) {
	gtk_list_store_clear(Viewer->ImagesStore);
	Viewer->NumVisibleImages = 0;
	double X1 = Viewer->Min.X + (Viewer->Pointer.X - Viewer->BoxSize / 2) / Viewer->Scale.X;
	double Y1 = Viewer->Min.Y + (Viewer->Pointer.Y - Viewer->BoxSize / 2) / Viewer->Scale.Y;
	double X2 = Viewer->Min.X + (Viewer->Pointer.X + Viewer->BoxSize / 2) / Viewer->Scale.X;
	double Y2 = Viewer->Min.Y + (Viewer->Pointer.Y + Viewer->BoxSize / 2) / Viewer->Scale.Y;
	foreach_node(Viewer->Root, X1, Y1, X2, Y2, Viewer, (node_callback_t *)draw_node_image);
	char Title[64];
	sprintf(Title, "%d images under cursor", Viewer->NumVisibleImages);
	//gtk_window_set_title(GTK_WINDOW(Viewer->ResultsWindow), Title);
}

static int edit_node_value(viewer_t *Viewer, node_t *Node) {
	field_t *Field = Viewer->EditField;
	double Value = Field->Values[Node - Viewer->Nodes] = Viewer->EditValue;
	if (Field == Viewer->Fields[Viewer->CIndex]) {
		set_node_rgb(Node, 6.0 * (Value - Field->Range.Min) / (Field->Range.Max - Field->Range.Min));
	}
	return 0;
}

static void edit_node_values(viewer_t *Viewer) {
	if (!Viewer->EditField) return;
	double X1 = Viewer->Min.X + (Viewer->Pointer.X - Viewer->BoxSize / 2) / Viewer->Scale.X;
	double Y1 = Viewer->Min.Y + (Viewer->Pointer.Y - Viewer->BoxSize / 2) / Viewer->Scale.Y;
	double X2 = Viewer->Min.X + (Viewer->Pointer.X + Viewer->BoxSize / 2) / Viewer->Scale.X;
	double Y2 = Viewer->Min.Y + (Viewer->Pointer.Y + Viewer->BoxSize / 2) / Viewer->Scale.Y;
	foreach_node(Viewer->Root, X1, Y1, X2, Y2, Viewer, (node_callback_t *)edit_node_value);
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
	//draw_node_images(Viewer);
	gtk_widget_queue_draw(Widget);
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
	draw_node_images(Viewer);
	gtk_widget_queue_draw(Widget);
	return FALSE;
}

static gboolean button_press_viewer(GtkWidget *Widget, GdkEventButton *Event, viewer_t *Viewer) {
	if (Event->button == 1) {
		Viewer->Pointer.X = Event->x;
		Viewer->Pointer.Y = Event->y;
		return FALSE;
	} else if (Event->button == 3) {
		edit_node_values(Viewer);
		redraw_viewer_background(Viewer);
		gtk_widget_queue_draw(Widget);
		return FALSE;
	}
	return TRUE;
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
	draw_node_images(Viewer);
	gtk_widget_queue_draw(Widget);
	return FALSE;
}

static gboolean key_press_viewer(GtkWidget *Widget, GdkEventKey *Event, viewer_t *Viewer) {
	switch (Event->keyval) {
	case GDK_KEY_x: {
		set_viewer_indices(Viewer, (Viewer->XIndex + 1) % Viewer->NumFields, Viewer->YIndex);
		redraw_viewer_background(Viewer);
		draw_node_images(Viewer);
		gtk_widget_queue_draw(Widget);
		break;
	}
	case GDK_KEY_X: {
		set_viewer_indices(Viewer, (Viewer->XIndex + Viewer->NumFields - 1) % Viewer->NumFields, Viewer->YIndex);
		redraw_viewer_background(Viewer);
		draw_node_images(Viewer);
		gtk_widget_queue_draw(Widget);
		break;
	}
	case GDK_KEY_y: {
		set_viewer_indices(Viewer, Viewer->XIndex, (Viewer->YIndex + 1) % Viewer->NumFields);
		redraw_viewer_background(Viewer);
		draw_node_images(Viewer);
		gtk_widget_queue_draw(Widget);
		break;
	}
	case GDK_KEY_Y: {
		set_viewer_indices(Viewer, Viewer->XIndex, (Viewer->YIndex + Viewer->NumFields - 1) % Viewer->NumFields);
		redraw_viewer_background(Viewer);
		draw_node_images(Viewer);
		gtk_widget_queue_draw(Widget);
		break;
	}
	case GDK_KEY_c: {
		set_viewer_colour_index(Viewer, (Viewer->CIndex + 1) % Viewer->NumFields);
		redraw_viewer_background(Viewer);
		draw_node_images(Viewer);
		gtk_widget_queue_draw(Widget);
		break;
	}
	case GDK_KEY_C: {
		set_viewer_colour_index(Viewer, (Viewer->CIndex + Viewer->NumFields - 1) % Viewer->NumFields);
		redraw_viewer_background(Viewer);
		draw_node_images(Viewer);
		gtk_widget_queue_draw(Widget);
		break;
	}
	case GDK_KEY_s: {
		cairo_surface_write_to_png(Viewer->CachedBackground, "screenshot.png");
		break;
	}
	}
	return FALSE;
}

static void x_field_changed(GtkComboBox *Widget, viewer_t *Viewer) {
	set_viewer_indices(Viewer, gtk_combo_box_get_active(Widget), Viewer->YIndex);
	redraw_viewer_background(Viewer);
	draw_node_images(Viewer);
	gtk_widget_queue_draw(Viewer->DrawingArea);
}

static void y_field_changed(GtkComboBox *Widget, viewer_t *Viewer) {
	set_viewer_indices(Viewer, Viewer->XIndex, gtk_combo_box_get_active(Widget));
	redraw_viewer_background(Viewer);
	draw_node_images(Viewer);
	gtk_widget_queue_draw(Viewer->DrawingArea);
}

static void c_field_changed(GtkComboBox *Widget, viewer_t *Viewer) {
	set_viewer_colour_index(Viewer, gtk_combo_box_get_active(Widget));
	redraw_viewer_background(Viewer);
	draw_node_images(Viewer);
	gtk_widget_queue_draw(Viewer->DrawingArea);
}

static void edit_field_changed(GtkComboBox *Widget, viewer_t *Viewer) {
	field_t *Field = Viewer->EditField = Viewer->Fields[gtk_combo_box_get_active(GTK_COMBO_BOX(Widget))];
	gtk_combo_box_set_model(GTK_COMBO_BOX(Viewer->EditValueComboBox), GTK_TREE_MODEL(Field->EnumStore));
	Viewer->EditValue = 0.0;
	gtk_combo_box_set_active(GTK_COMBO_BOX(Viewer->EditValueComboBox), -1);
}

typedef void text_dialog_callback_t(const char *Result, viewer_t *Viewer, void *Data);

typedef struct {
	GtkWidget *Dialog, *Entry;
	text_dialog_callback_t *Callback;
	viewer_t *Viewer;
	void *Data;
} text_dialog_info_t;

static void text_input_dialog_destroy(GtkWidget *Dialog, text_dialog_info_t *Info) {
	free(Info);
}

static void text_input_dialog_cancel(GtkWidget *Button, text_dialog_info_t *Info) {
	gtk_widget_destroy(Info->Dialog);
}

static void text_input_dialog_accept(GtkWidget *Button, text_dialog_info_t *Info) {
	const char *Result = gtk_entry_get_text(GTK_ENTRY(Info->Entry));
	if (!strlen(Result)) return;
	Info->Callback(Result, Info->Viewer, Info->Data);
	gtk_widget_destroy(Info->Dialog);
}

static void text_input_dialog(const char *Title, viewer_t *Viewer, text_dialog_callback_t *Callback, void *Data) {
	GtkWindow *Dialog = GTK_WINDOW(gtk_window_new(GTK_WINDOW_TOPLEVEL));
	gtk_window_set_title(Dialog, Title);
	gtk_window_set_transient_for(Dialog, GTK_WINDOW(Viewer->MainWindow));
	gtk_window_set_modal(Dialog, TRUE);
	gtk_container_set_border_width(GTK_CONTAINER(Dialog), 6);
	GtkWidget *VBox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
	gtk_container_add(GTK_CONTAINER(Dialog), VBox);

	GtkWidget *Entry = gtk_entry_new();
	gtk_box_pack_start(GTK_BOX(VBox), Entry, TRUE, FALSE, 10);

	GtkWidget *ButtonBox = gtk_button_box_new(GTK_ORIENTATION_HORIZONTAL);
	gtk_box_pack_start(GTK_BOX(VBox), ButtonBox, FALSE, FALSE, 4);

	GtkWidget *CancelButton = gtk_button_new_with_label("Cancel");
	gtk_button_set_image(GTK_BUTTON(CancelButton), gtk_image_new_from_icon_name("gtk-cancel", GTK_ICON_SIZE_BUTTON));
	gtk_box_pack_start(GTK_BOX(ButtonBox), CancelButton, FALSE, FALSE, 4);

	GtkWidget *AcceptButton = gtk_button_new_with_label("Accept");
	gtk_button_set_image(GTK_BUTTON(AcceptButton), gtk_image_new_from_icon_name("gtk-accept", GTK_ICON_SIZE_BUTTON));
	gtk_box_pack_start(GTK_BOX(ButtonBox), AcceptButton, FALSE, FALSE, 4);

	text_dialog_info_t *Info = (text_dialog_info_t *)malloc(sizeof(text_dialog_info_t));
	Info->Dialog = GTK_WIDGET(Dialog);
	Info->Entry = Entry;
	Info->Callback = Callback;
	Info->Viewer = Viewer;
	Info->Data = Data;

	g_signal_connect(G_OBJECT(CancelButton), "clicked", G_CALLBACK(text_input_dialog_cancel), Info);
	g_signal_connect(G_OBJECT(AcceptButton), "clicked", G_CALLBACK(text_input_dialog_accept), Info);
	g_signal_connect(G_OBJECT(Dialog), "destroy", G_CALLBACK(text_input_dialog_destroy), Info);


	gtk_widget_show_all(GTK_WIDGET(Dialog));
}

static void edit_value_changed(GtkComboBox *Widget, viewer_t *Viewer) {
	Viewer->EditValue = gtk_combo_box_get_active(GTK_COMBO_BOX(Widget)) + 1;
}

static void add_field_callback(const char *Name, viewer_t *Viewer, void *Data) {
	Name = strdup(Name);
	gtk_list_store_insert_with_values(Viewer->FieldsStore, 0, -1, 0, Name, 1, Viewer->NumFields, -1);
	int NumFields = Viewer->NumFields + 1;
	field_t **Fields = (field_t **)malloc(NumFields * sizeof(field_t *));
	field_t *Field = (field_t *)malloc(sizeof(field_t) + Viewer->NumNodes * sizeof(double));
	Field->EnumStore = gtk_list_store_new(2, G_TYPE_STRING, G_TYPE_DOUBLE);
	Field->EnumNames = (const char **)malloc(sizeof(const char *));
	Field->EnumNames[0] = "";
	Field->Name = Name;
	Field->Range.Min = Field->Range.Max = 0;
	memset(Field->Values, 0, Viewer->NumNodes * sizeof(double));
	for (int I = 0; I < Viewer->NumFields; ++I) Fields[I] = Viewer->Fields[I];
	Fields[Viewer->NumFields] = Field;
	free(Viewer->Fields);
	Viewer->Fields = Fields;
	Viewer->NumFields = NumFields;
	gtk_combo_box_set_active(GTK_COMBO_BOX(Viewer->EditFieldComboBox), NumFields - 1);
}

static void add_field_clicked(GtkWidget *Button, viewer_t *Viewer) {
	text_input_dialog("Add Field", Viewer, (text_dialog_callback_t *)add_field_callback, 0);
}

static void add_value_callback(const char *Name, viewer_t *Viewer, void *Data) {
	field_t *Field = Viewer->EditField;
	if (!Field || !Field->EnumStore) return;
	Name = strdup(Name);
	double Value = Viewer->EditValue = gtk_tree_model_iter_n_children(GTK_TREE_MODEL(Field->EnumStore), 0) + 1;
	gtk_list_store_insert_with_values(Field->EnumStore, 0, -1, 0, Name, 1, Value, -1);
	const char **EnumNames = (const char **)malloc((Value + 1) * sizeof(const char *));
	memcpy(EnumNames, Field->EnumNames, Value * sizeof(const char *));
	EnumNames[(int)Value] = Name;
	free(Field->EnumNames);
	Field->EnumNames = EnumNames;
	Field->Range.Max = Value;
	gtk_combo_box_set_active(GTK_COMBO_BOX(Viewer->EditValueComboBox), Value - 1);
	if (Field == Viewer->Fields[Viewer->CIndex]) {
		set_viewer_colour_index(Viewer, Viewer->CIndex);
		redraw_viewer_background(Viewer);
		gtk_widget_queue_draw(Viewer->DrawingArea);
	}
}

static void add_value_clicked(GtkWidget *Button, viewer_t *Viewer) {
	text_input_dialog("Add Value", Viewer, (text_dialog_callback_t *)add_value_callback, 0);
}

static void save_csv(GtkWidget *Button, viewer_t *Viewer) {
	GtkWidget *FileChooser = gtk_file_chooser_dialog_new(
		"Save as CSV file",
		GTK_WINDOW(Viewer->MainWindow),
		GTK_FILE_CHOOSER_ACTION_SAVE,
		"gtk-cancel", GTK_RESPONSE_CANCEL,
		"gtk-save", GTK_RESPONSE_ACCEPT,
		0
	);
	if (gtk_dialog_run(GTK_DIALOG(FileChooser)) == GTK_RESPONSE_ACCEPT) {
		char *FileName = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(FileChooser));
		FILE *File = fopen(FileName, "wb");
		g_free(FileName);
		field_t **Fields = Viewer->Fields;
		node_t *Nodes = Viewer->Nodes;
		int NumFields = Viewer->NumFields;
		int NumNodes = Viewer->NumNodes;
		csv_fwrite(File, "filename", strlen("filename"));
		for (int I = 0; I < NumFields; ++I) {
			fputc(',', File);
			csv_fwrite(File, Fields[I]->Name, strlen(Fields[I]->Name));
		}
		fputc('\n', File);
		for (int J = 0; J < NumNodes; ++J) {
			csv_fwrite(File, Nodes[J].FileName, strlen(Nodes[J].FileName));
			for (int I = 0; I < NumFields; ++I) {
				fputc(',', File);
				field_t *Field = Fields[I];
				if (Field->EnumNames) {
					const char *Value = Field->EnumNames[(int)Field->Values[J]];
					csv_fwrite(File, Value, strlen(Value));
				} else {
					fprintf(File, "%f", Field->Values[J]);
				}
			}
			fputc('\n', File);
		}
		fclose(File);
	}
	gtk_widget_destroy(FileChooser);
}

static void create_viewer_toolbar(viewer_t *Viewer, GtkToolbar *Toolbar) {
	//GtkToolItem *OpenCsvButton = gtk_tool_button_new(gtk_image_new_from_icon_name("gtk-open", GTK_ICON_SIZE_SMALL_TOOLBAR), "Open");
	GtkToolItem *SaveCsvButton = gtk_tool_button_new(gtk_image_new_from_icon_name("gtk-save", GTK_ICON_SIZE_SMALL_TOOLBAR), "Save");
	//gtk_toolbar_insert(Toolbar, OpenCsvButton, -1);
	gtk_toolbar_insert(Toolbar, SaveCsvButton, -1);

	g_signal_connect(G_OBJECT(SaveCsvButton), "clicked", G_CALLBACK(save_csv), Viewer);

	GtkCellRenderer *FieldRenderer;
	GtkWidget *XComboBox = Viewer->XComboBox = gtk_combo_box_new_with_model(GTK_TREE_MODEL(Viewer->FieldsStore));
	FieldRenderer = gtk_cell_renderer_text_new();
	gtk_cell_layout_pack_end(GTK_CELL_LAYOUT(XComboBox), FieldRenderer, TRUE);
	gtk_cell_layout_add_attribute(GTK_CELL_LAYOUT(XComboBox), FieldRenderer, "text", 0);
	GtkWidget *YComboBox = Viewer->YComboBox = gtk_combo_box_new_with_model(GTK_TREE_MODEL(Viewer->FieldsStore));
	FieldRenderer = gtk_cell_renderer_text_new();
	gtk_cell_layout_pack_end(GTK_CELL_LAYOUT(YComboBox), FieldRenderer, TRUE);
	gtk_cell_layout_add_attribute(GTK_CELL_LAYOUT(YComboBox), FieldRenderer, "text", 0);
	GtkWidget *CComboBox = Viewer->CComboBox = gtk_combo_box_new_with_model(GTK_TREE_MODEL(Viewer->FieldsStore));
	FieldRenderer = gtk_cell_renderer_text_new();
	gtk_cell_layout_pack_end(GTK_CELL_LAYOUT(CComboBox), FieldRenderer, TRUE);
	gtk_cell_layout_add_attribute(GTK_CELL_LAYOUT(CComboBox), FieldRenderer, "text", 0);

	g_signal_connect(G_OBJECT(XComboBox), "changed", G_CALLBACK(x_field_changed), Viewer);
	g_signal_connect(G_OBJECT(YComboBox), "changed", G_CALLBACK(y_field_changed), Viewer);
	g_signal_connect(G_OBJECT(CComboBox), "changed", G_CALLBACK(c_field_changed), Viewer);

	GtkToolItem *ComboItem;

	gtk_toolbar_insert(Toolbar, gtk_separator_tool_item_new(), -1);
	ComboItem = gtk_tool_item_new();
	gtk_container_add(GTK_CONTAINER(ComboItem), XComboBox);
	gtk_toolbar_insert(Toolbar, ComboItem, -1);
	ComboItem = gtk_tool_item_new();
	gtk_container_add(GTK_CONTAINER(ComboItem), YComboBox);
	gtk_toolbar_insert(Toolbar, ComboItem, -1);
	ComboItem = gtk_tool_item_new();
	gtk_container_add(GTK_CONTAINER(ComboItem), CComboBox);
	gtk_toolbar_insert(Toolbar, ComboItem, -1);


	GtkWidget *EditFieldComboBox = Viewer->EditFieldComboBox = gtk_combo_box_new_with_model(GTK_TREE_MODEL(Viewer->FieldsStore));
	FieldRenderer = gtk_cell_renderer_text_new();
	gtk_cell_layout_pack_end(GTK_CELL_LAYOUT(EditFieldComboBox), FieldRenderer, TRUE);
	gtk_cell_layout_add_attribute(GTK_CELL_LAYOUT(EditFieldComboBox), FieldRenderer, "text", 0);
	GtkWidget *EditValueComboBox = Viewer->EditValueComboBox = gtk_combo_box_new();
	FieldRenderer = gtk_cell_renderer_text_new();
	gtk_cell_layout_pack_end(GTK_CELL_LAYOUT(EditValueComboBox), FieldRenderer, TRUE);
	gtk_cell_layout_add_attribute(GTK_CELL_LAYOUT(EditValueComboBox), FieldRenderer, "text", 0);

	g_signal_connect(G_OBJECT(EditFieldComboBox), "changed", G_CALLBACK(edit_field_changed), Viewer);
	g_signal_connect(G_OBJECT(EditValueComboBox), "changed", G_CALLBACK(edit_value_changed), Viewer);

	GtkToolItem *AddFieldButton = gtk_tool_button_new(gtk_image_new_from_icon_name("gtk-add", GTK_ICON_SIZE_SMALL_TOOLBAR), "Add Field");
	GtkToolItem *AddValueButton = gtk_tool_button_new(gtk_image_new_from_icon_name("gtk-add", GTK_ICON_SIZE_SMALL_TOOLBAR), "Add Value");

	gtk_toolbar_insert(Toolbar, gtk_separator_tool_item_new(), -1);
	ComboItem = gtk_tool_item_new();
	gtk_container_add(GTK_CONTAINER(ComboItem), EditFieldComboBox);
	gtk_toolbar_insert(Toolbar, ComboItem, -1);
	gtk_toolbar_insert(Toolbar, AddFieldButton, -1);

	gtk_toolbar_insert(Toolbar, gtk_separator_tool_item_new(), -1);
	ComboItem = gtk_tool_item_new();
	gtk_container_add(GTK_CONTAINER(ComboItem), EditValueComboBox);
	gtk_toolbar_insert(Toolbar, ComboItem, -1);
	gtk_toolbar_insert(Toolbar, AddValueButton, -1);

	g_signal_connect(G_OBJECT(AddFieldButton), "clicked", G_CALLBACK(add_field_clicked), Viewer);
	g_signal_connect(G_OBJECT(AddValueButton), "clicked", G_CALLBACK(add_value_clicked), Viewer);
}

static viewer_t *create_viewer(const char *CsvFileName) {
	viewer_t *Viewer = (viewer_t *)malloc(sizeof(viewer_t));
	Viewer->PointSize = 5.0;
	Viewer->BoxSize = 40.0;
	Viewer->CachedBackground = 0;
	Viewer->CacheHead = Viewer->CacheTail = 0;
	Viewer->NumCachedImages = 0;
	Viewer->EditField = 0;

	Viewer->FieldsStore = gtk_list_store_new(2, G_TYPE_STRING, G_TYPE_INT);

	GtkWidget *MainWindow = Viewer->MainWindow = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	Viewer->DrawingArea = gtk_drawing_area_new();

	GtkWidget *MainVBox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_container_add(GTK_CONTAINER(MainWindow), MainVBox);

	GtkWidget *Toolbar = gtk_toolbar_new();
	gtk_box_pack_start(GTK_BOX(MainVBox), Toolbar, FALSE, FALSE, 0);
	create_viewer_toolbar(Viewer, GTK_TOOLBAR(Toolbar));

	GtkWidget *MainVPaned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
	gtk_box_pack_start(GTK_BOX(MainVBox), MainVPaned, TRUE, TRUE, 0);

	Viewer->ImagesScrolledArea = gtk_scrolled_window_new(0, 0);
	Viewer->ImagesStore = gtk_list_store_new(2, G_TYPE_STRING, GDK_TYPE_PIXBUF);
	Viewer->ImagesView = gtk_icon_view_new_with_model(GTK_TREE_MODEL(Viewer->ImagesStore));

	gtk_paned_pack1(GTK_PANED(MainVPaned), Viewer->DrawingArea, TRUE, TRUE);
	gtk_paned_pack2(GTK_PANED(MainVPaned), Viewer->ImagesScrolledArea, TRUE, TRUE);

	gtk_icon_view_set_text_column(GTK_ICON_VIEW(Viewer->ImagesView), 0);
	gtk_icon_view_set_pixbuf_column(GTK_ICON_VIEW(Viewer->ImagesView), 1);
	gtk_icon_view_set_item_width(GTK_ICON_VIEW(Viewer->ImagesView), 72);
	gtk_widget_add_events(Viewer->DrawingArea, GDK_SCROLL_MASK);
	gtk_widget_add_events(Viewer->DrawingArea, GDK_POINTER_MOTION_MASK);
	gtk_widget_add_events(Viewer->DrawingArea, GDK_BUTTON_PRESS_MASK);
	gtk_widget_add_events(Viewer->DrawingArea, GDK_BUTTON_RELEASE_MASK);
	gtk_widget_add_events(Viewer->DrawingArea, GDK_KEY_PRESS);
	gtk_container_add(GTK_CONTAINER(Viewer->ImagesScrolledArea), Viewer->ImagesView);
	g_signal_connect(G_OBJECT(Viewer->DrawingArea), "draw", G_CALLBACK(redraw_viewer), Viewer);
	g_signal_connect(G_OBJECT(Viewer->DrawingArea), "size-allocate", G_CALLBACK(resize_viewer), Viewer);
	g_signal_connect(G_OBJECT(Viewer->DrawingArea), "scroll-event", G_CALLBACK(scroll_viewer), Viewer);
	g_signal_connect(G_OBJECT(Viewer->DrawingArea), "button-press-event", G_CALLBACK(button_press_viewer), Viewer);
	g_signal_connect(G_OBJECT(Viewer->DrawingArea), "motion-notify-event", G_CALLBACK(motion_notify_viewer), Viewer);
	g_signal_connect(G_OBJECT(Viewer->DrawingArea), "key-press-event", G_CALLBACK(key_press_viewer), Viewer);
	g_signal_connect(G_OBJECT(Viewer->MainWindow), "destroy", G_CALLBACK(gtk_main_quit), 0);


	viewer_open_file(Viewer, CsvFileName);

	gtk_window_resize(GTK_WINDOW(Viewer->MainWindow), 640, 480);
	gtk_paned_set_position(GTK_PANED(MainVPaned), 320);
	gtk_widget_show_all(Viewer->MainWindow);

	gtk_combo_box_set_active(GTK_COMBO_BOX(Viewer->XComboBox), 0);
	gtk_combo_box_set_active(GTK_COMBO_BOX(Viewer->YComboBox), 1);
	gtk_combo_box_set_active(GTK_COMBO_BOX(Viewer->CComboBox), Viewer->NumFields - 1);
	return Viewer;
}

int main(int Argc, char *Argv[]) {
	gtk_init(&Argc, &Argv);
	viewer_t *Viewer = create_viewer(Argv[1]);
	gtk_main();
	return 0;
}
