#ifndef VIEWER_H
#define VIEWER_H

#include <gtk/gtk.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <stringmap.h>
#include <jansson.h>

typedef struct node_t node_t;
typedef int node_callback_t(void *Data, node_t *Node);
typedef struct field_t field_t;
typedef struct filter_t filter_t;
typedef struct viewer_t viewer_t;
typedef struct queued_callback_t queued_callback_t;

typedef struct {
	double X, Y;
} point_t;

typedef struct {
	double Min, Max;
} range_t;

static ml_type_t *NodeT;
static ml_type_t *FieldT;

struct node_t {
	const ml_type_t *Type;
	node_t *Children[2];
	node_t *Next;
	viewer_t *Viewer;
	const char *FileName;
	GdkPixbuf *Pixbuf;
	GCancellable *LoadCancel;
	GInputStream *LoadStream;
	GFile *File;
	double X, Y;
	int XIndex, YIndex;
#ifdef USE_GL
	double R, G, B;
#else
	unsigned int Colour;
#endif
	int Filtered;
	int LoadGeneration;
};

struct field_t {
	const ml_type_t *Type;
	const char *Name;
	stringmap_t *EnumMap;
	GtkListStore *EnumStore;
	const char **EnumNames;
	int *EnumValues;
	GtkTreeViewColumn *PreviewColumn;
	const char *RemoteId;
	json_int_t *RemoteGenerations;
	range_t Range;
	int EnumSize;
	int PreviewVisible;
	int FilterCount;
	int FilterGeneration;
	double Sum, Sum2, SD;
	double Values[];
};

struct viewer_t {
	GtkWidget *MainWindow, *MainVPaned;
	GtkLabel *NumVisibleLabel;
    GtkWidget *FilterWindow, *FiltersBox;
	GtkWidget *DrawingArea, *ImagesView;
	GtkWidget *PreviewWidget;
	GtkWidget *XComboBox, *YComboBox, *CComboBox, *EditFieldComboBox, *EditValueComboBox;
	GtkWidget *InfoBar;
	GdkCursor *Cursor;
	GtkListStore *ImagesStore, *ValuesStore;
	GtkListStore *FieldsStore;
	GtkListStore *OperatorsStore;
	GtkClipboard *Clipboard;
	GtkMenu *NodeMenu;
	node_t *Nodes, *Root, *Selected;
	node_t **SortBuffer;
	node_t **SortedX, **SortedY;
	cairo_t *Cairo;
	node_t **LoadCache;
	node_t *ActiveNode;
	ml_value_t *ActivationFn;
	ml_value_t *HotkeyFns[10];
	console_t *Console;
	zsock_t *RemoteSocket;
	queued_callback_t *QueuedCallbacks;
	const char *ImagePrefix;
	stringmap_t Globals[1];
	stringmap_t FieldsByName[1];
	stringmap_t RemoteFields[1];
#ifdef USE_GL
	float *GLVertices, *GLColours;
#else
	cairo_surface_t *CachedBackground;
	unsigned int *CachedPixels;
	int CachedStride;
#endif
	field_t **Fields, *EditField;
	filter_t *Filters;
	point_t Min, Max, Scale, DataMin, DataMax, Pointer;
	double EditValue;
	int NumNodes, NumFields, NumFiltered, NumVisible, NumUpdated;
	int XIndex, YIndex, CIndex;
	int FilterGeneration, LoadGeneration;
	int LoadCacheIndex;
	int ShowBox, RedrawBackground;
	int LastCallbackIndex;
#ifdef USE_GL
	int GLCount, GLReady;
	GLuint GLArrays[2], GLBuffers[4];
	GLuint GLProgram, GLTransformLocation;
#endif
};

#endif
