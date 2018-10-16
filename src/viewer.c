#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <gtk/gtk.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include "libcsv/csv.h"

#ifdef USE_GL
#include <epoxy/gl.h>
#endif

#define MAX_CACHED_IMAGES 1024
#define MAX_VISIBLE_IMAGES 64
#define POINT_COLOUR_CHROMA 0.5
#define POINT_COLOUR_SATURATION 0.7
#define POINT_COLOUR_VALUE 0.9

typedef struct node_t node_t;
typedef int node_callback_t(void *Data, node_t *Node);
typedef struct field_t field_t;
typedef struct filter_t filter_t;
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
	int Filtered;
};

struct field_t {
	const char *Name;
	GHashTable *EnumHash;
	GtkListStore *EnumStore;
	const char **EnumNames;
	double *EnumValues;
	GtkTreeViewColumn *PreviewColumn;
	range_t Range;
	int EnumSize;
	int PreviewIndex;
	int FilterGeneration;
	double Values[];
};

typedef void filter_fn_t(int Count, node_t *Nodes, double *Input, double Value);

struct filter_t {
	filter_t *Next;
	viewer_t *Viewer;
	field_t *Field;
	filter_fn_t *Operator;
	GtkWidget *Widget, *ValueWidget;
	double Value;
};

struct viewer_t {
	GtkWidget *MainWindow, *MainVPaned;
	GtkLabel *NumVisibleLabel;
    GtkWidget *FilterWindow, *FiltersBox;
	GtkWidget *DrawingArea;
	GtkWidget *PreviewWidget;
	GtkWidget *XComboBox, *YComboBox, *CComboBox, *EditFieldComboBox, *EditValueComboBox;
	GtkListStore *ImagesStore, *ValuesStore;
	GtkListStore *FieldsStore;
	GtkListStore *OperatorsStore;
	node_t *Root, *Nodes;
	cairo_t *Cairo;
#ifdef USE_GL
	float *GLVertices, *GLColours;
#else
	cairo_surface_t *CachedBackground;
#endif
	node_t *CacheHead, *CacheTail;
	field_t **Fields, *EditField;
	filter_t *Filters;
	int *Filtered;
	point_t Min, Max, Scale, DataMin, DataMax, Pointer;
	double PointSize, BoxSize, EditValue;
	int NumNodes, NumFields, NumCachedImages, NumVisibleImages;
	int XIndex, YIndex, CIndex;
	int FilterGeneration;
#ifdef USE_GL
	int GLCount, GLReady;
	GLuint GLArrays[2], GLBuffers[4];
	GLuint GLProgram, GLTransformLocation;
#endif
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

typedef struct {
	void *Data;
	node_callback_t *Callback;
	double X1, Y1, X2, Y2;
} foreach_t;

static void foreach_node2(node_t *Root, foreach_t *Foreach) {
	// Bounds  = [Bounds]
	if (!Root) {
	} else if (Foreach->X2 < Root->X) {
		if (Foreach->Y2 < Root->Y) {
			foreach_node2(Root->Children[0], Foreach);
		} else if (Foreach->Y1 > Root->Y) {
			foreach_node2(Root->Children[2], Foreach);
		} else {
			foreach_node2(Root->Children[0], Foreach);
			foreach_node2(Root->Children[2], Foreach);
		}
	} else if (Foreach->X1 > Root->X) {
		if (Foreach->Y2 < Root->Y) {
			foreach_node2(Root->Children[1], Foreach);
		} else if (Foreach->Y1 > Root->Y) {
			foreach_node2(Root->Children[3], Foreach);
		} else {
			foreach_node2(Root->Children[1], Foreach);
			foreach_node2(Root->Children[3], Foreach);
		}
	} else {
		if (Foreach->Y2 < Root->Y) {
			foreach_node2(Root->Children[0], Foreach);
			foreach_node2(Root->Children[1], Foreach);
		} else if (Foreach->Y1 > Root->Y) {
			foreach_node2(Root->Children[2], Foreach);
			foreach_node2(Root->Children[3], Foreach);
		} else {
			Foreach->Callback(Foreach->Data, Root);
			foreach_node2(Root->Children[0], Foreach);
			foreach_node2(Root->Children[1], Foreach);
			foreach_node2(Root->Children[2], Foreach);
			foreach_node2(Root->Children[3], Foreach);
		}
	}
}

static inline int foreach_node(node_t *Root, double X1, double Y1, double X2, double Y2, void *Data, node_callback_t *Callback) {
	foreach_t Foreach = {Data, Callback, X1, Y1, X2, Y2};
	foreach_node2(Root, &Foreach);
	return 0;
/*
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
*/
}

static void forall_nodes(node_t *Root, void *Data, node_callback_t *Callback) {
	if (Root) {
		Callback(Data, Root);
		forall_nodes(Root->Children[0], Data, Callback);
		forall_nodes(Root->Children[1], Data, Callback);
		forall_nodes(Root->Children[2], Data, Callback);
		forall_nodes(Root->Children[3], Data, Callback);
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

	node_t *Root = 0;
	Node = Viewer->Nodes;
	int I = NumNodes;
	while (--I >= 0) {
		if (!Node->Filtered) {
			Root = Node;
			break;
		}
		++Node;
	}
	++Node;
	Viewer->Root = Root;
	while (--I >= 0) {
		if (!Node->Filtered) add_node(Root, Node);
		++Node;
	}

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

static void filter_enum_field(viewer_t *Viewer, field_t *Field) {
	printf("filter_enum_field(%s)\n", Field->Name);
	node_t *Node = Viewer->Nodes;
	int NumNodes = Viewer->NumNodes;
	double *EnumValues = Field->EnumValues;
	memset(EnumValues, 0, Field->EnumSize * sizeof(double));
	double Max = 0.0;
	double *Value = Field->Values;
	for (int I = NumNodes; --I >= 0;) {
		if (!Node->Filtered) {
			int Index = (int)Value[0];
			if (Index && (EnumValues[Index] == 0.0)) {
				Max += 1.0;
				EnumValues[Index] = Max;
			}
		}
		++Node;
		++Value;
	}
	Field->Range.Max = Max;
	printf("Field->Range.Max = %f\n", Max);
	Field->FilterGeneration = Viewer->FilterGeneration;
}

static void set_viewer_colour_index(viewer_t *Viewer, int CIndex) {
	Viewer->CIndex = CIndex;
	int NumNodes = Viewer->NumNodes;
	field_t *CField = Viewer->Fields[CIndex];
	double Min = CField->Range.Min;
	double Range = CField->Range.Max - Min;
	node_t *Node = Viewer->Nodes;
	double *CValue = CField->Values;
	if (CField->EnumStore) {
		if (CField->FilterGeneration != Viewer->FilterGeneration) {
			filter_enum_field(Viewer, CField);
		}
		double *EnumValues = CField->EnumValues;
		Range += 1.0;
		for (int I = NumNodes; --I >= 0;) {
			double Value = EnumValues[(int)CValue[0]];
			if (Value > 0.0) {
				set_node_rgb(Node, 6.0 * (Value - Min) / Range);
			} else {
				Node->R = Node->G = Node->B = POINT_COLOUR_SATURATION;
			}
			++Node;
			++CValue;
		}
	} else {
		if (Range <= 1.0e-6) Range = 1.0;
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
				if (Size) {
					gpointer *Ref = g_hash_table_lookup(Field->EnumHash, Text);
					if (Ref) {
						Value = *(double *)Ref;
					} else {
						char *EnumName = malloc(Size + 1);
						memcpy(EnumName, Text, Size);
						EnumName[Size] = 0;
						Ref = (void *)&Field->Values[Loader->Row - 1];
						g_hash_table_insert(Field->EnumHash, EnumName, Ref);
						Value = g_hash_table_size(Field->EnumHash);
					}
				} else {
					Value = 0.0;
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

static void set_enum_name_fn(const char *Name, const double *Value, const char **Names) {
	Names[(int)Value[0]] = Name;
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
	csv_init(Parser, CSV_APPEND_NULL);
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
		Field->PreviewIndex = -1;
		Field->PreviewColumn = 0;
		Field->FilterGeneration = 0;
		Fields[I] = Field;
	}
#ifdef USE_GL
	Viewer->GLVertices = (float *)malloc(NumNodes * 3 * 3 * sizeof(float));
	Viewer->GLColours = (float *)malloc(NumNodes * 3 * 4 * sizeof(float));
#endif
	printf("Loading rows...\n");
	fopen(CsvFileName, "r");
	Loader->Nodes = Nodes;
	Loader->Fields = Fields;
	Loader->Row = Loader->Index = 0;
	csv_init(Parser, CSV_APPEND_NULL);
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
			int EnumSize = Field->EnumSize = g_hash_table_size(Field->EnumHash) + 1;
			const char **EnumNames = (const char **)malloc(EnumSize * sizeof(const char *));
			EnumNames[0] = "";
			g_hash_table_foreach(Field->EnumHash, (void *)set_enum_name_fn, EnumNames);
			Field->EnumNames = EnumNames;
			GtkListStore *EnumStore = Field->EnumStore = gtk_list_store_new(2, G_TYPE_STRING, G_TYPE_DOUBLE);
			for (int J = 0; J < EnumSize; ++J) {
				gtk_list_store_insert_with_values(Field->EnumStore, 0, -1, 0, Field->EnumNames[J], 1, (double)(J + 1), -1);
			}
			Field->EnumValues = (double *)malloc(EnumSize * sizeof(double));
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

static int draw_node_value(viewer_t *Viewer, node_t *Node) {
	++Viewer->NumVisibleImages;
	field_t **Fields = Viewer->Fields;
	int NumFields = Viewer->NumFields;
	int Index = Node - Viewer->Nodes;
	GtkTreeIter Iter[1];
	gtk_list_store_append(Viewer->ValuesStore, Iter);
	if (Viewer->NumVisibleImages <= MAX_VISIBLE_IMAGES) {
		for (int I = 0; I < NumFields; ++I) {
			field_t *Field = Fields[I];
			if (Field->PreviewColumn) {
				double Value = Field->Values[Index];
				GdkRGBA Colour[1];
				Colour->alpha = 0.5;
				if (Field->EnumStore && Value == 0.0) {
					Colour->red = Colour->green = Colour->blue = POINT_COLOUR_SATURATION;
				} else {
					double H = 6.0 * (Value - Field->Range.Min) / (Field->Range.Max - Field->Range.Min);
					if (H < 1.0) {
						Colour->red = POINT_COLOUR_VALUE;
						Colour->green = POINT_COLOUR_VALUE - POINT_COLOUR_CHROMA * fabs(H - 1.0);
						Colour->blue = POINT_COLOUR_VALUE - POINT_COLOUR_CHROMA;
					} else if (H < 2.0) {
						Colour->red = POINT_COLOUR_VALUE - POINT_COLOUR_CHROMA * fabs(H - 1.0);
						Colour->green = POINT_COLOUR_VALUE;
						Colour->blue = POINT_COLOUR_VALUE - POINT_COLOUR_CHROMA;
					} else if (H < 3.0) {
						Colour->red = POINT_COLOUR_VALUE - POINT_COLOUR_CHROMA;
						Colour->green = POINT_COLOUR_VALUE;
						Colour->blue = POINT_COLOUR_VALUE - POINT_COLOUR_CHROMA * fabs(H - 3.0);
					} else if (H < 4.0) {
						Colour->red = POINT_COLOUR_VALUE - POINT_COLOUR_CHROMA;
						Colour->green = POINT_COLOUR_VALUE - POINT_COLOUR_CHROMA * fabs(H - 3.0);
						Colour->blue = POINT_COLOUR_VALUE;
					} else if (H < 5.0) {
						Colour->red = POINT_COLOUR_VALUE - POINT_COLOUR_CHROMA * fabs(H - 5.0);
						Colour->green = POINT_COLOUR_VALUE - POINT_COLOUR_CHROMA;
						Colour->blue = POINT_COLOUR_VALUE;
					} else {
						Colour->red = POINT_COLOUR_VALUE;
						Colour->green = POINT_COLOUR_VALUE - POINT_COLOUR_CHROMA;
						Colour->blue = POINT_COLOUR_VALUE - POINT_COLOUR_CHROMA * fabs(H - 5.0);
					}
				}
				if (Field->EnumStore) {
					gtk_list_store_set(Viewer->ValuesStore, Iter, Field->PreviewIndex, Field->EnumNames[(int)Value], Field->PreviewIndex + 1, Colour, -1);
				} else {
					gtk_list_store_set(Viewer->ValuesStore, Iter, Field->PreviewIndex, Value, Field->PreviewIndex + 1, &Colour, -1);
				}
			}
		}
	}
	return 0;
}

static void update_preview(viewer_t *Viewer) {
	Viewer->NumVisibleImages = 0;
	double X1 = Viewer->Min.X + (Viewer->Pointer.X - Viewer->BoxSize / 2) / Viewer->Scale.X;
	double Y1 = Viewer->Min.Y + (Viewer->Pointer.Y - Viewer->BoxSize / 2) / Viewer->Scale.Y;
	double X2 = Viewer->Min.X + (Viewer->Pointer.X + Viewer->BoxSize / 2) / Viewer->Scale.X;
	double Y2 = Viewer->Min.Y + (Viewer->Pointer.Y + Viewer->BoxSize / 2) / Viewer->Scale.Y;
	if (Viewer->ImagesStore) {
		gtk_list_store_clear(Viewer->ImagesStore);
		foreach_node(Viewer->Root, X1, Y1, X2, Y2, Viewer, (node_callback_t *)draw_node_image);
	} else if (Viewer->ValuesStore) {
		gtk_list_store_clear(Viewer->ValuesStore);
		foreach_node(Viewer->Root, X1, Y1, X2, Y2, Viewer, (node_callback_t *)draw_node_value);
	}
	char NumVisibleText[64];
	sprintf(NumVisibleText, "%d points", Viewer->NumVisibleImages);
	gtk_label_set_text(Viewer->NumVisibleLabel, NumVisibleText);
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
#ifdef USE_GL
	//double X = (Node->X - Viewer->Min.X) / (Viewer->Max.X - Viewer->Min.X);
	//double Y = (Node->Y - Viewer->Min.Y) / (Viewer->Max.Y - Viewer->Min.Y);
	double X = Viewer->Scale.X * (Node->X - Viewer->Min.X);
	double Y = Viewer->Scale.Y * (Node->Y - Viewer->Min.Y);
	//printf("Point at (%f, %f)\n", X, Y);
	int Index = Viewer->GLCount;
	Viewer->GLVertices[3 * Index + 0] = X - Viewer->PointSize / 2;
	Viewer->GLVertices[3 * Index + 1] = Y + Viewer->PointSize * 0.33;
	Viewer->GLVertices[3 * Index + 2] = 0.0;
	Viewer->GLVertices[3 * Index + 3] = X;
	Viewer->GLVertices[3 * Index + 4] = Y - Viewer->PointSize * 0.66;
	Viewer->GLVertices[3 * Index + 5] = 0.0;
	Viewer->GLVertices[3 * Index + 6] = X + Viewer->PointSize / 2;
	Viewer->GLVertices[3 * Index + 7] = Y + Viewer->PointSize * 0.33;
	Viewer->GLVertices[3 * Index + 8] = 0.0;
	Viewer->GLColours[4 * Index + 0] = Node->R;
	Viewer->GLColours[4 * Index + 1] = Node->G;
	Viewer->GLColours[4 * Index + 2] = Node->B;
	Viewer->GLColours[4 * Index + 3] = 1.0;
	Viewer->GLColours[4 * Index + 4] = Node->R;
	Viewer->GLColours[4 * Index + 5] = Node->G;
	Viewer->GLColours[4 * Index + 6] = Node->B;
	Viewer->GLColours[4 * Index + 7] = 1.0;
	Viewer->GLColours[4 * Index + 8] = Node->R;
	Viewer->GLColours[4 * Index + 9] = Node->G;
	Viewer->GLColours[4 * Index + 10] = Node->B;
	Viewer->GLColours[4 * Index + 11] = 1.0;
	Viewer->GLCount = Index + 3;
#else
	double X = Viewer->Scale.X * (Node->X - Viewer->Min.X);
	double Y = Viewer->Scale.Y * (Node->Y - Viewer->Min.Y);
	cairo_t *Cairo = Viewer->Cairo;
	cairo_new_path(Cairo);
	cairo_rectangle(Cairo, X - Viewer->PointSize / 2, Y - Viewer->PointSize / 2, Viewer->PointSize, Viewer->PointSize);
	cairo_set_source_rgb(Cairo, Node->R, Node->G, Node->B);
	cairo_fill(Cairo);
#endif
	return 0;
}

#ifdef USE_GL
static gboolean render_viewer(GtkGLArea *Widget, GdkGLContext *Context, viewer_t *Viewer) {
	puts("render_viewer");
	guint Width = gtk_widget_get_allocated_width(Viewer->DrawingArea);
	guint Height = gtk_widget_get_allocated_height(Viewer->DrawingArea);
	//glViewport(0, 0, Width, Height);
	//glMatrixMode(GL_PROJECTION);
	//glLoadIdentity();
	//glOrtho(0.0, Width, 0.0, Height, -1.0, 1.0);
	//glMatrixMode(GL_MODELVIEW);
	//glLoadIdentity();
	//glClearColor(0.5, 0.5, 0.5, 1.0);

	GLfloat transform[] = {
		1.0f, 0.0f, 0.0f, 0.0f,
		0.0f, 1.0f, 0.0f, 0.0f,
		0.0f, 0.0f, 1.0f, 0.0f,
		0.0f, 0.0f, 0.0f, 1.0f
	};

	printf("Scale = %f, %f\n", Viewer->Scale.X, Viewer->Scale.Y);
	printf("Width, Height = %d, %d\n", Width, Height);
	printf("Range = (%f, %f) - (%f, %f)\n", Viewer->Min.X, Viewer->Min.Y, Viewer->Max.X, Viewer->Max.Y);

	transform[0] = 2.0 / Width;
	transform[12] = -1.0;
	transform[5] = -2.0 / Height;
	transform[13] = 1.0;

	glClearColor(1.0, 1.0, 1.0, 1.0);
	glClear(GL_COLOR_BUFFER_BIT);
	glUseProgram(Viewer->GLProgram);
	glUniformMatrix4fv(Viewer->GLTransformLocation, 1, GL_FALSE, transform);

	glBindVertexArray(Viewer->GLArrays[0]);
	glEnableVertexAttribArray(0);
	glBindBuffer(GL_ARRAY_BUFFER, Viewer->GLBuffers[0]);
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, (void *)0);
	glEnableVertexAttribArray(1);
	glBindBuffer(GL_ARRAY_BUFFER, Viewer->GLBuffers[1]);
	glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 0, (void *)0);
	glDrawArrays(GL_TRIANGLES, 0, Viewer->GLCount);
	glDisableVertexAttribArray(0);
	glBindBuffer(GL_ARRAY_BUFFER, 0);

	float BoxX1 = Viewer->Pointer.X - Viewer->BoxSize / 2;
	float BoxY1 = Viewer->Pointer.Y - Viewer->BoxSize / 2;
	float BoxX2 = Viewer->Pointer.X + Viewer->BoxSize / 2;
	float BoxY2 = Viewer->Pointer.Y + Viewer->BoxSize / 2;

	float BoxVertices[] = {
		BoxX1, BoxY1, 0.1,
		BoxX1, BoxY2, 0.1,
		BoxX2, BoxY1, 0.1,
		BoxX2, BoxY2, 0.1
	};

	float BoxColours[] = {
		0.5, 0.5, 1.0, 0.5,
		0.5, 0.5, 1.0, 0.5,
		0.5, 0.5, 1.0, 0.5,
		0.5, 0.5, 1.0, 0.5
	};

	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

	glBindVertexArray(Viewer->GLArrays[1]);
	glEnableVertexAttribArray(0);
	glBindBuffer(GL_ARRAY_BUFFER, Viewer->GLBuffers[2]);
	glBufferData(GL_ARRAY_BUFFER, 4 * 3 * sizeof(float), BoxVertices, GL_STATIC_DRAW);
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, (void *)0);
	glEnableVertexAttribArray(1);
	glBindBuffer(GL_ARRAY_BUFFER, Viewer->GLBuffers[3]);
	glBufferData(GL_ARRAY_BUFFER, 4 * 4 * sizeof(float), BoxColours, GL_STATIC_DRAW);
	glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 0, (void *)0);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	glDisableVertexAttribArray(0);
	glBindBuffer(GL_ARRAY_BUFFER, 0);

	glDisable(GL_BLEND);

	glUseProgram(0);
	glFlush();
	return TRUE;
}

static void load_gl_shaders(viewer_t *Viewer) {
	GLuint VertexShaderID = glCreateShader(GL_VERTEX_SHADER);
	GLuint FragmentShaderID = glCreateShader(GL_FRAGMENT_SHADER);

	GLint Result = GL_FALSE;
	int InfoLogLength;

	const char *VertexSource[] = {
		"#version 330\n",
		"layout(location = 0) in vec3 position;\n",
		"layout(location = 1) in vec4 color;\n",
		"out vec4 fragmentColor;\n",
		"uniform mat4 transform;\n",
		"void main() {\n",
		"\tgl_Position = transform * vec4(position, 1.0f);\n",
		"\tfragmentColor = color;\n",
		"}\n"
	};
	glShaderSource(VertexShaderID, sizeof(VertexSource) / sizeof(const char *), VertexSource, NULL);
	glCompileShader(VertexShaderID);

	glGetShaderiv(VertexShaderID, GL_COMPILE_STATUS, &Result);
	if (Result) puts("vertex shader compile success!");
	glGetShaderiv(VertexShaderID, GL_INFO_LOG_LENGTH, &InfoLogLength);
	if (InfoLogLength > 0) {
		char Message[InfoLogLength + 1];
		glGetShaderInfoLog(VertexShaderID, InfoLogLength, NULL, Message);
		puts(Message);
	}

	const char *FragmentSource[] = {
		"#version 330\n",
		"in vec4 fragmentColor;\n",
		"out vec4 color;\n"
		"void main() {\n",
		"\tcolor = fragmentColor;\n",
		"}\n"
	};

	glShaderSource(FragmentShaderID, sizeof(FragmentSource) / sizeof(const char *), FragmentSource, NULL);
	glCompileShader(FragmentShaderID);

	glGetShaderiv(FragmentShaderID, GL_COMPILE_STATUS, &Result);
	if (Result) puts("fragment shader compile success!");
	glGetShaderiv(FragmentShaderID, GL_INFO_LOG_LENGTH, &InfoLogLength);
	if (InfoLogLength > 0) {
		char Message[InfoLogLength + 1];
		glGetShaderInfoLog(FragmentShaderID, InfoLogLength, NULL, Message);
		puts(Message);
	}

	GLuint ProgramID = glCreateProgram();
	glAttachShader(ProgramID, VertexShaderID);
	glAttachShader(ProgramID, FragmentShaderID);
	glLinkProgram(ProgramID);

	glGetShaderiv(ProgramID, GL_LINK_STATUS, &Result);
	if (Result) puts("program link success!");
	glGetShaderiv(ProgramID, GL_INFO_LOG_LENGTH, &InfoLogLength);
	if (InfoLogLength > 0) {
		char Message[InfoLogLength + 1];
		glGetShaderInfoLog(ProgramID, InfoLogLength, NULL, Message);
		puts(Message);
	}

	glDetachShader(ProgramID, VertexShaderID);
	glDetachShader(ProgramID, FragmentShaderID);

	glDeleteShader(VertexShaderID);
	glDeleteShader(FragmentShaderID);

	Viewer->GLProgram = ProgramID;
	Viewer->GLTransformLocation = glGetUniformLocation(ProgramID, "transform");
}

static void realize_viewer(GtkGLArea *Widget, viewer_t *Viewer) {
	puts("realize_viewer");
	guint Width = gtk_widget_get_allocated_width(Viewer->DrawingArea);
	guint Height = gtk_widget_get_allocated_height(Viewer->DrawingArea);
	gtk_gl_area_make_current(Widget);
	GError *Error = gtk_gl_area_get_error(Widget);
	if (Error) {
		printf("GL Error %d: %s\n", Error->code, Error->message);
	}
	//glViewport(0, 0, Width, Height);
	//glEnableClientState(GL_COLOR_ARRAY);
	//glEnableClientState(GL_VERTEX_ARRAY);
	glGenVertexArrays(2, Viewer->GLArrays);
	glBindVertexArray(Viewer->GLArrays[0]);
	glGenBuffers(2, Viewer->GLBuffers);
	glBindVertexArray(Viewer->GLArrays[1]);
	glGenBuffers(2, Viewer->GLBuffers + 2);
	load_gl_shaders(Viewer);
	Viewer->GLReady = 1;
}
#else
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
#endif

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
#ifdef USE_GL
	Viewer->GLCount = 0;
	clock_t Start = clock();
	foreach_node(Viewer->Root, Viewer->Min.X, Viewer->Min.Y, Viewer->Max.X, Viewer->Max.Y, Viewer, (node_callback_t *)redraw_point);
	printf("foreach_node took %d\n", clock() - Start);
	printf("rendered %d points\n", Viewer->GLCount);
	if (Viewer->GLReady) {
		gtk_gl_area_make_current(GTK_GL_AREA(Viewer->DrawingArea));
		glBindBuffer(GL_ARRAY_BUFFER, Viewer->GLBuffers[0]);
		glBufferData(GL_ARRAY_BUFFER, Viewer->GLCount * 3 * sizeof(float), Viewer->GLVertices, GL_STATIC_DRAW);
		glBindBuffer(GL_ARRAY_BUFFER, Viewer->GLBuffers[1]);
		glBufferData(GL_ARRAY_BUFFER, Viewer->GLCount * 4 * sizeof(float), Viewer->GLColours, GL_STATIC_DRAW);
		glBindBuffer(GL_ARRAY_BUFFER, 0);
	}
#else
	guint Width = cairo_image_surface_get_width(Viewer->CachedBackground);
	guint Height = cairo_image_surface_get_height(Viewer->CachedBackground);
	cairo_t *Cairo = cairo_create(Viewer->CachedBackground);
	cairo_set_source_rgb(Cairo, 1.0, 1.0, 1.0);
	cairo_rectangle(Cairo, 0.0, 0.0, Width, Height);
	cairo_fill(Cairo);
	Viewer->Cairo = Cairo;
	clock_t Start = clock();
	foreach_node(Viewer->Root, Viewer->Min.X, Viewer->Min.Y, Viewer->Max.X, Viewer->Max.Y, Viewer, (node_callback_t *)redraw_point);
	printf("foreach_node took %d\n", clock() - Start);
	Viewer->Cairo = 0;
	cairo_destroy(Cairo);
#endif
}

static void resize_viewer(GtkWidget *Widget, GdkRectangle *Allocation, viewer_t *Viewer) {
	Viewer->Scale.X = Allocation->width / (Viewer->Max.X - Viewer->Min.X);
	Viewer->Scale.Y = Allocation->height / (Viewer->Max.Y - Viewer->Min.Y);
#ifdef USE_GL
#else
	if (Viewer->CachedBackground) {
		cairo_surface_destroy(Viewer->CachedBackground);
	}
	Viewer->CachedBackground = cairo_image_surface_create(CAIRO_FORMAT_RGB24, Allocation->width, Allocation->height);
#endif
	redraw_viewer_background(Viewer);
	//update_preview(Viewer);
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
	update_preview(Viewer);
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
	update_preview(Viewer);
	gtk_widget_queue_draw(Widget);
	return FALSE;
}

static gboolean key_press_viewer(GtkWidget *Widget, GdkEventKey *Event, viewer_t *Viewer) {
	switch (Event->keyval) {
	case GDK_KEY_x: {
		set_viewer_indices(Viewer, (Viewer->XIndex + 1) % Viewer->NumFields, Viewer->YIndex);
		redraw_viewer_background(Viewer);
		update_preview(Viewer);
		gtk_widget_queue_draw(Widget);
		break;
	}
	case GDK_KEY_X: {
		set_viewer_indices(Viewer, (Viewer->XIndex + Viewer->NumFields - 1) % Viewer->NumFields, Viewer->YIndex);
		redraw_viewer_background(Viewer);
		update_preview(Viewer);
		gtk_widget_queue_draw(Widget);
		break;
	}
	case GDK_KEY_y: {
		set_viewer_indices(Viewer, Viewer->XIndex, (Viewer->YIndex + 1) % Viewer->NumFields);
		redraw_viewer_background(Viewer);
		update_preview(Viewer);
		gtk_widget_queue_draw(Widget);
		break;
	}
	case GDK_KEY_Y: {
		set_viewer_indices(Viewer, Viewer->XIndex, (Viewer->YIndex + Viewer->NumFields - 1) % Viewer->NumFields);
		redraw_viewer_background(Viewer);
		update_preview(Viewer);
		gtk_widget_queue_draw(Widget);
		break;
	}
	case GDK_KEY_c: {
		set_viewer_colour_index(Viewer, (Viewer->CIndex + 1) % Viewer->NumFields);
		redraw_viewer_background(Viewer);
		update_preview(Viewer);
		gtk_widget_queue_draw(Widget);
		break;
	}
	case GDK_KEY_C: {
		set_viewer_colour_index(Viewer, (Viewer->CIndex + Viewer->NumFields - 1) % Viewer->NumFields);
		redraw_viewer_background(Viewer);
		update_preview(Viewer);
		gtk_widget_queue_draw(Widget);
		break;
	}
	case GDK_KEY_s: {
#ifdef USE_GL
#else
		cairo_surface_write_to_png(Viewer->CachedBackground, "screenshot.png");
#endif
		break;
	}
	}
	return FALSE;
}

static void x_field_changed(GtkComboBox *Widget, viewer_t *Viewer) {
	set_viewer_indices(Viewer, gtk_combo_box_get_active(Widget), Viewer->YIndex);
	redraw_viewer_background(Viewer);
	update_preview(Viewer);
	gtk_widget_queue_draw(Viewer->DrawingArea);
}

static void y_field_changed(GtkComboBox *Widget, viewer_t *Viewer) {
	set_viewer_indices(Viewer, Viewer->XIndex, gtk_combo_box_get_active(Widget));
	redraw_viewer_background(Viewer);
	update_preview(Viewer);
	gtk_widget_queue_draw(Viewer->DrawingArea);
}

static void c_field_changed(GtkComboBox *Widget, viewer_t *Viewer) {
	set_viewer_colour_index(Viewer, gtk_combo_box_get_active(Widget));
	redraw_viewer_background(Viewer);
	update_preview(Viewer);
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
	gtk_button_set_image(GTK_BUTTON(CancelButton), gtk_image_new_from_icon_name("window-close-symbolic", GTK_ICON_SIZE_BUTTON));
	gtk_box_pack_start(GTK_BOX(ButtonBox), CancelButton, FALSE, FALSE, 4);

	GtkWidget *AcceptButton = gtk_button_new_with_label("Accept");
	gtk_button_set_image(GTK_BUTTON(AcceptButton), gtk_image_new_from_icon_name("emblem-ok-symbolic", GTK_ICON_SIZE_BUTTON));
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
	Viewer->EditValue = gtk_combo_box_get_active(Widget);
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
	Field->PreviewIndex = -1;
	Field->PreviewColumn = 0;
	gtk_list_store_insert_with_values(Field->EnumStore, 0, -1, 0, "", 1, 0.0, -1);
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
	double Value = Viewer->EditValue = gtk_tree_model_iter_n_children(GTK_TREE_MODEL(Field->EnumStore), 0);
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
		"Cancel", GTK_RESPONSE_CANCEL,
		"Save", GTK_RESPONSE_ACCEPT,
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

static void filter_operator_equal(int Count, node_t *Node, double *Input, double Value) {
	while (--Count >= 0) {
		if (!Node->Filtered) if (Input[0] != Value) Node->Filtered = 1;
		++Node;
		++Input;
	}
}

static void filter_operator_not_equal(int Count, node_t *Node, double *Input, double Value) {
	while (--Count >= 0) {
		if (!Node->Filtered) if (Input[0] == Value) Node->Filtered = 1;
		++Node;
		++Input;
	}
}

static void filter_operator_less(int Count, node_t *Node, double *Input, double Value) {
	while (--Count >= 0) {
		if (!Node->Filtered) if (Input[0] >= Value) Node->Filtered = 1;
		++Node;
		++Input;
	}
}

static void filter_operator_greater(int Count, node_t *Node, double *Input, double Value) {
	while (--Count >= 0) {
		if (!Node->Filtered) if (Input[0] <= Value) Node->Filtered = 1;
		++Node;
		++Input;
	}
}

static void filter_operator_less_or_equal(int Count, node_t *Node, double *Input, double Value) {
	while (--Count >= 0) {
		if (!Node->Filtered) if (Input[0] > Value) Node->Filtered = 1;
		++Node;
		++Input;
	}
}

static void filter_operator_greater_or_equal(int Count, node_t *Node, double *Input, double Value) {
	while (--Count >= 0) {
		if (!Node->Filtered) if (Input[0] < Value) Node->Filtered = 1;
		++Node;
		++Input;
	}
}

static void viewer_filter_nodes(viewer_t *Viewer) {
	int NumNodes = Viewer->NumNodes;
	node_t *Node = Viewer->Nodes;
	for (int I = NumNodes; --I >= 0;) {
		Node->Filtered = 0;
		++Node;
	}
	for (filter_t *Filter = Viewer->Filters; Filter; Filter = Filter->Next) {
		if (Filter->Operator && Filter->Field) {
			Filter->Operator(Viewer->NumNodes, Viewer->Nodes, Filter->Field->Values, Filter->Value);
		}
	}
	++Viewer->FilterGeneration;
	set_viewer_colour_index(Viewer, Viewer->CIndex);
	set_viewer_indices(Viewer, Viewer->XIndex, Viewer->YIndex);
	redraw_viewer_background(Viewer);
	update_preview(Viewer);
	gtk_widget_queue_draw(Viewer->DrawingArea);
}

static void filter_enum_value_changed(GtkComboBox *Widget, filter_t *Filter) {
	Filter->Value = gtk_combo_box_get_active(Widget);
	viewer_filter_nodes(Filter->Viewer);
}

static void filter_enum_entry_changed(GtkEntry *Widget, filter_t *Filter) {
	gpointer *Ref = g_hash_table_lookup(Filter->Field->EnumHash, gtk_entry_get_text(GTK_ENTRY(Widget)));
	if (Ref) {
		Filter->Value = *(double *)Ref;
		viewer_filter_nodes(Filter->Viewer);
	}
}

static void filter_real_value_changed(GtkSpinButton *Widget, filter_t *Filter) {
	Filter->Value = gtk_spin_button_get_value(Widget);
	viewer_filter_nodes(Filter->Viewer);
}

static void filter_field_changed(GtkComboBox *Widget, filter_t *Filter) {
	viewer_t *Viewer = Filter->Viewer;
	field_t *Field = Filter->Field = Viewer->Fields[gtk_combo_box_get_active(GTK_COMBO_BOX(Widget))];
	if (Filter->ValueWidget) gtk_widget_destroy(Filter->ValueWidget);
	if (Field->EnumStore) {
		if (gtk_tree_model_iter_n_children(GTK_TREE_MODEL(Field->EnumStore), 0) < 100) {
			GtkWidget *ValueComboBox = Filter->ValueWidget = gtk_combo_box_new_with_model(GTK_TREE_MODEL(Field->EnumStore));
			GtkCellRenderer *FieldRenderer = gtk_cell_renderer_text_new();
			gtk_cell_layout_pack_end(GTK_CELL_LAYOUT(ValueComboBox), FieldRenderer, TRUE);
			gtk_cell_layout_add_attribute(GTK_CELL_LAYOUT(ValueComboBox), FieldRenderer, "text", 0);
			g_signal_connect(G_OBJECT(ValueComboBox), "changed", G_CALLBACK(filter_enum_value_changed), Filter);
		} else {
			GtkWidget *ValueEntry = Filter->ValueWidget = gtk_entry_new();
			GtkEntryCompletion *EntryCompletion = gtk_entry_completion_new();
			gtk_entry_completion_set_model(EntryCompletion, GTK_TREE_MODEL(Field->EnumStore));
			gtk_entry_completion_set_text_column(EntryCompletion, 0);
			gtk_entry_set_completion(GTK_ENTRY(ValueEntry), EntryCompletion);
			g_signal_connect(G_OBJECT(ValueEntry), "changed", G_CALLBACK(filter_enum_entry_changed), Filter);
		}
	} else {
		GtkWidget *ValueSpinButton = Filter->ValueWidget = gtk_spin_button_new_with_range(Field->Range.Min, Field->Range.Max, (Field->Range.Max - Field->Range.Min) / 20.0);
		g_signal_connect(G_OBJECT(ValueSpinButton), "value-changed", G_CALLBACK(filter_real_value_changed), Filter);
	}
	gtk_box_pack_start(GTK_BOX(Filter->Widget), Filter->ValueWidget, FALSE, FALSE, 4);
	gtk_widget_show_all(Filter->Widget);
}

static void filter_operator_changed(GtkComboBox *Widget, filter_t *Filter) {
	switch (gtk_combo_box_get_active(Widget)) {
	case 0: Filter->Operator = filter_operator_equal; break;
	case 1: Filter->Operator = filter_operator_not_equal; break;
	case 2: Filter->Operator = filter_operator_less; break;
	case 3: Filter->Operator = filter_operator_greater; break;
	case 4: Filter->Operator = filter_operator_less_or_equal; break;
	case 5: Filter->Operator = filter_operator_greater_or_equal; break;
	default: Filter->Operator = 0; break;
	}
	viewer_filter_nodes(Filter->Viewer);
}

static void filter_remove(GtkWidget *Button, filter_t *Filter) {
	viewer_t *Viewer = Filter->Viewer;
	filter_t **Slot = &Viewer->Filters;
	while (Slot[0] != Filter) Slot = &Slot[0]->Next;
	Slot[0] = Slot[0]->Next;
	gtk_widget_destroy(Filter->Widget);
	viewer_filter_nodes(Viewer);
}

static void filter_create(GtkButton *Widget, viewer_t *Viewer) {
	filter_t *Filter = (filter_t *)malloc(sizeof(filter_t));
	Filter->Viewer = Viewer;
	GtkWidget *FilterBox = Filter->Widget = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	Filter->ValueWidget = 0;
	Filter->Field = 0;
	Filter->Operator = 0;

	GtkWidget *RemoveButton = gtk_button_new_with_label("Remove");
	gtk_button_set_image(GTK_BUTTON(RemoveButton), gtk_image_new_from_icon_name("list-remove-symbolic", GTK_ICON_SIZE_BUTTON));
	gtk_box_pack_start(GTK_BOX(FilterBox), RemoveButton, FALSE, FALSE, 4);

	GtkCellRenderer *FieldRenderer;
	GtkWidget *FieldComboBox = gtk_combo_box_new_with_model(GTK_TREE_MODEL(Viewer->FieldsStore));
	FieldRenderer = gtk_cell_renderer_text_new();
	gtk_cell_layout_pack_end(GTK_CELL_LAYOUT(FieldComboBox), FieldRenderer, TRUE);
	gtk_cell_layout_add_attribute(GTK_CELL_LAYOUT(FieldComboBox), FieldRenderer, "text", 0);
	gtk_box_pack_start(GTK_BOX(FilterBox), FieldComboBox, FALSE, FALSE, 4);

	GtkWidget *OperatorComboBox = gtk_combo_box_new_with_model(GTK_TREE_MODEL(Viewer->OperatorsStore));
	FieldRenderer = gtk_cell_renderer_text_new();
	gtk_cell_layout_pack_end(GTK_CELL_LAYOUT(OperatorComboBox), FieldRenderer, TRUE);
	gtk_cell_layout_add_attribute(GTK_CELL_LAYOUT(OperatorComboBox), FieldRenderer, "text", 0);
	gtk_box_pack_start(GTK_BOX(FilterBox), OperatorComboBox, FALSE, FALSE, 4);

	g_signal_connect(G_OBJECT(RemoveButton), "clicked", G_CALLBACK(filter_remove), Filter);
	g_signal_connect(G_OBJECT(FieldComboBox), "changed", G_CALLBACK(filter_field_changed), Filter);
	g_signal_connect(G_OBJECT(OperatorComboBox), "changed", G_CALLBACK(filter_operator_changed), Filter);

	gtk_box_pack_start(GTK_BOX(Viewer->FiltersBox), FilterBox, FALSE, FALSE, 6);
	gtk_widget_show_all(FilterBox);

	Filter->Next = Viewer->Filters;
	Viewer->Filters = Filter;
}

static gboolean hide_filter_window(GtkWidget *Widget, GdkEvent *Event, viewer_t *Viewer) {
	gtk_widget_hide(Widget);
	return TRUE;
}

static void create_filter_window(viewer_t *Viewer) {
	GtkWidget *Window = Viewer->FilterWindow = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	gtk_window_set_transient_for(GTK_WINDOW(Window), GTK_WINDOW(Viewer->MainWindow));
	gtk_container_set_border_width(GTK_CONTAINER(Window), 6);
	GtkWidget *FiltersBox = Viewer->FiltersBox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
	gtk_container_add(GTK_CONTAINER(Window), FiltersBox);

	GtkListStore *OperatorsStore = Viewer->OperatorsStore = gtk_list_store_new(2, G_TYPE_STRING, G_TYPE_POINTER);
	gtk_list_store_insert_with_values(OperatorsStore, 0, -1, 0, "=", 1, filter_operator_equal, -1);
	gtk_list_store_insert_with_values(OperatorsStore, 0, -1, 0, "≠", 1, filter_operator_not_equal, -1);
	gtk_list_store_insert_with_values(OperatorsStore, 0, -1, 0, "<", 1, filter_operator_less, -1);
	gtk_list_store_insert_with_values(OperatorsStore, 0, -1, 0, ">", 1, filter_operator_greater, -1);
	gtk_list_store_insert_with_values(OperatorsStore, 0, -1, 0, "≤", 1, filter_operator_less_or_equal, -1);
	gtk_list_store_insert_with_values(OperatorsStore, 0, -1, 0, "≥", 1, filter_operator_greater_or_equal, -1);

	GtkWidget *CreateButton = gtk_button_new_with_label("Add");
	gtk_button_set_image(GTK_BUTTON(CreateButton), gtk_image_new_from_icon_name("list-add-symbolic", GTK_ICON_SIZE_BUTTON));
	gtk_box_pack_start(GTK_BOX(FiltersBox), CreateButton, FALSE, FALSE, 6);

	g_signal_connect(G_OBJECT(CreateButton), "clicked", G_CALLBACK(filter_create), Viewer);

	g_signal_connect(G_OBJECT(Window), "delete-event", G_CALLBACK(hide_filter_window), Viewer);
}

static void show_filter_window(GtkButton *Widget, viewer_t *Viewer) {
	gtk_widget_show_all(Viewer->FilterWindow);
}

static void view_images_clicked(GtkWidget *Button, viewer_t *Viewer) {
	if (Viewer->ValuesStore) {
		gtk_container_remove(GTK_CONTAINER(Viewer->MainVPaned), Viewer->PreviewWidget);
		g_object_unref(G_OBJECT(Viewer->ValuesStore));
		Viewer->ValuesStore = 0;
	}
	Viewer->ImagesStore = gtk_list_store_new(2, G_TYPE_STRING, GDK_TYPE_PIXBUF);
	GtkWidget *ImagesScrolledArea = Viewer->PreviewWidget = gtk_scrolled_window_new(0, 0);
	GtkWidget *ImagesView = gtk_icon_view_new_with_model(GTK_TREE_MODEL(Viewer->ImagesStore));
	gtk_icon_view_set_text_column(GTK_ICON_VIEW(ImagesView), 0);
	gtk_icon_view_set_pixbuf_column(GTK_ICON_VIEW(ImagesView), 1);
	gtk_icon_view_set_item_width(GTK_ICON_VIEW(ImagesView), 72);
	gtk_container_add(GTK_CONTAINER(ImagesScrolledArea), ImagesView);
	gtk_paned_pack2(GTK_PANED(Viewer->MainVPaned), ImagesScrolledArea, TRUE, TRUE);
	gtk_widget_show_all(ImagesScrolledArea);
}

static void data_column_remove_clicked(GtkWidget *Button, field_t *Field) {
	Field->PreviewIndex = -1;
	GtkTreeView *ValuesView = GTK_TREE_VIEW(gtk_tree_view_column_get_tree_view(Field->PreviewColumn));
	gtk_tree_view_remove_column(ValuesView, Field->PreviewColumn);
	Field->PreviewColumn = 0;
}

static void view_data_clicked(GtkWidget *Button, viewer_t *Viewer) {
	if (Viewer->ImagesStore) {
		gtk_container_remove(GTK_CONTAINER(Viewer->MainVPaned), Viewer->PreviewWidget);
		g_object_unref(G_OBJECT(Viewer->ImagesStore));
		Viewer->ImagesStore = 0;
	}

	int NumFields = Viewer->NumFields;
	GType Types[NumFields * 2 - 2];
	for (int I = 1; I < NumFields; ++I) {
		field_t *Field = Viewer->Fields[I];
		Types[2 * I - 2] = Field->EnumStore ? G_TYPE_STRING : G_TYPE_DOUBLE;
		Types[2 * I - 1] = GDK_TYPE_RGBA;
		Field->PreviewIndex = 2 * I - 2;
	}

	Viewer->ValuesStore = gtk_list_store_newv(2 * NumFields - 2, Types);
	GtkWidget *ValuesScrolledArea = Viewer->PreviewWidget = gtk_scrolled_window_new(0, 0);
	GtkWidget *ValuesView = gtk_tree_view_new_with_model(GTK_TREE_MODEL(Viewer->ValuesStore));

	for (int I = 1; I < NumFields; ++I) {
		field_t *Field = Viewer->Fields[I];
		GtkTreeViewColumn *Column = gtk_tree_view_column_new();
		GtkWidget *Header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
		GtkWidget *Title = gtk_label_new(Field->Name);
		gtk_box_pack_start(GTK_BOX(Header), Title, TRUE, TRUE, 2);
		GtkWidget *RemoveButton = gtk_event_box_new();
		gtk_container_add(GTK_CONTAINER(RemoveButton), gtk_image_new_from_icon_name("window-close", GTK_ICON_SIZE_SMALL_TOOLBAR));
		gtk_box_pack_end(GTK_BOX(Header), RemoveButton, FALSE, FALSE, 2);
		gtk_tree_view_column_set_widget(Column, Header);
		gtk_widget_show_all(Header);
		gtk_tree_view_column_set_clickable(Column, TRUE);
		gtk_tree_view_column_set_reorderable(Column, TRUE);
		GtkCellRenderer *Renderer = gtk_cell_renderer_text_new();
		gtk_tree_view_column_pack_start(Column, Renderer, TRUE);
		gtk_tree_view_column_add_attribute(Column, Renderer, "text", Field->PreviewIndex);
		gtk_tree_view_column_add_attribute(Column, Renderer, "background-rgba", Field->PreviewIndex + 1);
		gtk_tree_view_append_column(GTK_TREE_VIEW(ValuesView), Column);
		Field->PreviewColumn = Column;
		g_signal_connect(Column, "clicked", G_CALLBACK(data_column_remove_clicked), Field);
	}

	gtk_container_add(GTK_CONTAINER(ValuesScrolledArea), ValuesView);
	gtk_paned_pack2(GTK_PANED(Viewer->MainVPaned), ValuesScrolledArea, TRUE, TRUE);
	gtk_widget_show_all(ValuesScrolledArea);
}

static GtkWidget *create_viewer_action_bar(viewer_t *Viewer) {
	GtkActionBar *ActionBar = GTK_ACTION_BAR(gtk_action_bar_new());
	//GtkToolItem *OpenCsvButton = gtk_tool_button_new(gtk_image_new_from_icon_name("gtk-open", GTK_ICON_SIZE_BUTTON), "Open");
	GtkWidget *SaveCsvButton = gtk_button_new_with_label("Save");
	gtk_button_set_image(GTK_BUTTON(SaveCsvButton), gtk_image_new_from_icon_name("document-save-as", GTK_ICON_SIZE_BUTTON));
	gtk_action_bar_pack_start(ActionBar, SaveCsvButton);

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

	gtk_action_bar_pack_start(ActionBar, XComboBox);
	gtk_action_bar_pack_start(ActionBar, YComboBox);
	gtk_action_bar_pack_start(ActionBar, CComboBox);


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

	GtkWidget *AddFieldButton = gtk_button_new_with_label("Add Field");
	gtk_button_set_image(GTK_BUTTON(AddFieldButton), gtk_image_new_from_icon_name("list-add", GTK_ICON_SIZE_BUTTON));

	GtkWidget *AddValueButton = gtk_button_new_with_label("Add Value");
	gtk_button_set_image(GTK_BUTTON(AddValueButton), gtk_image_new_from_icon_name("list-add", GTK_ICON_SIZE_BUTTON));

	gtk_action_bar_pack_start(ActionBar, EditFieldComboBox);
	gtk_action_bar_pack_start(ActionBar, AddFieldButton);

	gtk_action_bar_pack_start(ActionBar, EditValueComboBox);
	gtk_action_bar_pack_start(ActionBar, AddValueButton);

	g_signal_connect(G_OBJECT(AddFieldButton), "clicked", G_CALLBACK(add_field_clicked), Viewer);
	g_signal_connect(G_OBJECT(AddValueButton), "clicked", G_CALLBACK(add_value_clicked), Viewer);

	create_filter_window(Viewer);

	GtkWidget *FilterButton = gtk_button_new_with_label("Filter");
	gtk_button_set_image(GTK_BUTTON(FilterButton), gtk_image_new_from_icon_name("edit-find-replace", GTK_ICON_SIZE_BUTTON));
	gtk_action_bar_pack_start(ActionBar, FilterButton);

	g_signal_connect(G_OBJECT(FilterButton), "clicked", G_CALLBACK(show_filter_window), Viewer);

	GtkWidget *ViewImagesButton = gtk_button_new_with_label("View Images");
	gtk_button_set_image(GTK_BUTTON(ViewImagesButton), gtk_image_new_from_icon_name("image-x-generic", GTK_ICON_SIZE_BUTTON));

	GtkWidget *ViewDataButton = gtk_button_new_with_label("View Data");
	gtk_button_set_image(GTK_BUTTON(AddValueButton), gtk_image_new_from_icon_name("view-list", GTK_ICON_SIZE_BUTTON));

	gtk_action_bar_pack_start(ActionBar, ViewImagesButton);
	gtk_action_bar_pack_start(ActionBar, ViewDataButton);

	g_signal_connect(G_OBJECT(ViewImagesButton), "clicked", G_CALLBACK(view_images_clicked), Viewer);
	g_signal_connect(G_OBJECT(ViewDataButton), "clicked", G_CALLBACK(view_data_clicked), Viewer);

	GtkWidget *NumVisibleLabel = gtk_label_new(0);
	gtk_action_bar_pack_end(ActionBar, NumVisibleLabel);
	Viewer->NumVisibleLabel = GTK_LABEL(NumVisibleLabel);

	return GTK_WIDGET(ActionBar);
}

static viewer_t *create_viewer(const char *CsvFileName) {
	viewer_t *Viewer = (viewer_t *)malloc(sizeof(viewer_t));
#ifdef USE_GL
	Viewer->PointSize = 6.0;
	Viewer->BoxSize = 40.0;
	Viewer->GLVertices = 0;
	Viewer->GLColours = 0;
	Viewer->GLReady = 0;
#else
	Viewer->PointSize = 4.0;
	Viewer->BoxSize = 40.0;
	Viewer->CachedBackground = 0;
#endif
	Viewer->CacheHead = Viewer->CacheTail = 0;
	Viewer->NumCachedImages = 0;
	Viewer->EditField = 0;
	Viewer->Filters = 0;
	Viewer->FilterGeneration = 1;
	Viewer->FieldsStore = gtk_list_store_new(2, G_TYPE_STRING, G_TYPE_INT);

	GtkWidget *MainWindow = Viewer->MainWindow = gtk_window_new(GTK_WINDOW_TOPLEVEL);
#ifdef USE_GL
	Viewer->DrawingArea = gtk_gl_area_new();
#else
	Viewer->DrawingArea = gtk_drawing_area_new();
#endif

	GtkWidget *MainVBox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_container_add(GTK_CONTAINER(MainWindow), MainVBox);

	GtkWidget *ActionBar = create_viewer_action_bar(Viewer);
	gtk_box_pack_start(GTK_BOX(MainVBox), ActionBar, FALSE, FALSE, 0);

	Viewer->MainVPaned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
	gtk_box_pack_start(GTK_BOX(MainVBox), Viewer->MainVPaned, TRUE, TRUE, 0);

	gtk_paned_pack1(GTK_PANED(Viewer->MainVPaned), Viewer->DrawingArea, TRUE, TRUE);

	Viewer->ImagesStore = gtk_list_store_new(2, G_TYPE_STRING, GDK_TYPE_PIXBUF);
	GtkWidget *ImagesScrolledArea = Viewer->PreviewWidget = gtk_scrolled_window_new(0, 0);
	GtkWidget *ImagesView = gtk_icon_view_new_with_model(GTK_TREE_MODEL(Viewer->ImagesStore));
	gtk_icon_view_set_text_column(GTK_ICON_VIEW(ImagesView), 0);
	gtk_icon_view_set_pixbuf_column(GTK_ICON_VIEW(ImagesView), 1);
	gtk_icon_view_set_item_width(GTK_ICON_VIEW(ImagesView), 72);
	gtk_container_add(GTK_CONTAINER(ImagesScrolledArea), ImagesView);
	gtk_paned_pack2(GTK_PANED(Viewer->MainVPaned), ImagesScrolledArea, TRUE, TRUE);

	gtk_widget_add_events(Viewer->DrawingArea, GDK_SCROLL_MASK);
	gtk_widget_add_events(Viewer->DrawingArea, GDK_POINTER_MOTION_MASK);
	gtk_widget_add_events(Viewer->DrawingArea, GDK_BUTTON_PRESS_MASK);
	gtk_widget_add_events(Viewer->DrawingArea, GDK_BUTTON_RELEASE_MASK);
	gtk_widget_add_events(Viewer->DrawingArea, GDK_KEY_PRESS);
#ifdef USE_GL
	g_signal_connect(G_OBJECT(Viewer->DrawingArea), "render", G_CALLBACK(render_viewer), Viewer);
	g_signal_connect(G_OBJECT(Viewer->DrawingArea), "realize", G_CALLBACK(realize_viewer), Viewer);
#else
	g_signal_connect(G_OBJECT(Viewer->DrawingArea), "draw", G_CALLBACK(redraw_viewer), Viewer);
#endif
	g_signal_connect(G_OBJECT(Viewer->DrawingArea), "size-allocate", G_CALLBACK(resize_viewer), Viewer);
	g_signal_connect(G_OBJECT(Viewer->DrawingArea), "scroll-event", G_CALLBACK(scroll_viewer), Viewer);
	g_signal_connect(G_OBJECT(Viewer->DrawingArea), "button-press-event", G_CALLBACK(button_press_viewer), Viewer);
	g_signal_connect(G_OBJECT(Viewer->DrawingArea), "motion-notify-event", G_CALLBACK(motion_notify_viewer), Viewer);
	g_signal_connect(G_OBJECT(Viewer->DrawingArea), "key-press-event", G_CALLBACK(key_press_viewer), Viewer);
	g_signal_connect(G_OBJECT(Viewer->MainWindow), "destroy", G_CALLBACK(gtk_main_quit), 0);


	viewer_open_file(Viewer, CsvFileName);

	gtk_window_resize(GTK_WINDOW(Viewer->MainWindow), 640, 480);
	gtk_paned_set_position(GTK_PANED(Viewer->MainVPaned), 320);
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
