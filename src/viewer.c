#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "libcsv/csv.h"
#include <gc/gc.h>
#include <minilang.h>
#include <ml_macros.h>
#include <ml_file.h>
#include <ml_object.h>
#include <ml_iterfns.h>
#include <ml_gir.h>
#include <gtk_console.h>
#include "ml_csv.h"
#include "ml_gir.h"
#include <czmq.h>
#include "viewer.h"

#ifdef USE_GL
#include <epoxy/gl.h>
#endif

#define MAX_CACHED_IMAGES 1024
#define MAX_VISIBLE_IMAGES 64
#define POINT_COLOUR_CHROMA 0.5
#define POINT_COLOUR_SATURATION 0.7
#define POINT_COLOUR_VALUE 0.9
#ifdef USE_GL
#define POINT_SIZE 6.0
#define BOX_SIZE 40.0
#else
#define POINT_SIZE 4.0
#define BOX_SIZE 40.0
#endif

#define FIELD_COLUMN_NAME 0
#define FIELD_COLUMN_FIELD 1
#define FIELD_COLUMN_VISIBLE 2
#define FIELD_COLUMN_CONNECTED 3
#define FIELD_COLUMN_REMOTE 4

typedef void filter_fn_t(int Count, node_t *Nodes, double *Input, double Value);

struct filter_t {
	filter_t *Next;
	viewer_t *Viewer;
	field_t *Field;
	filter_fn_t *Operator;
	GtkWidget *Widget, *ValueWidget;
	double Value;
};

#ifdef MINGW
static char *stpcpy(char *Dest, const char *Source) {
	while (*Source) *Dest++ = *Source++;
	return Dest;
}

#define lstat stat
#endif

struct queued_callback_t {
	queued_callback_t *Next;
	void (*Callback)(viewer_t *Viewer, json_t *Result, void *Data);
	void *Data;
	int Index;
};

static stringmap_t EventHandlers[1] = {STRINGMAP_INIT};
typedef void (*event_handler_t)(viewer_t *Viewer, const char *Event, json_t *Details);

static gboolean remote_msg_fn(viewer_t *Viewer) {
	zmsg_t *Msg = zmsg_recv_nowait(Viewer->RemoteSocket);
	if (!Msg) return TRUE;
	zmsg_print(Msg);
	zframe_t *Frame = zmsg_pop(Msg);
	json_error_t Error;
	json_t *Response = json_loadb(zframe_data(Frame), zframe_size(Frame), 0, &Error);
	if (!Response) {
		fprintf(stderr, "Error parsing json\n");
		return TRUE;
	}
	int Index;
	json_t *Result;
	if (json_unpack(Response, "[io]", &Index, &Result)) {
		fprintf(stderr, "Error invalid json\n");
		return TRUE;
	}
	if (Index == 0) {
		console_printf(Viewer->Console, "Alert!\n");
		const char *Event = json_string_value(Result);
		json_t *Details = json_array_get(Response, 2);
		event_handler_t EventHandler = (event_handler_t )stringmap_search(EventHandlers, Event);
		if (EventHandler) EventHandler(Viewer, Event, Details);
		return TRUE;
	}
	if (json_is_object(Result)) {
		json_t *Error = json_object_get(Result, "error");
		if (Error) {
			fprintf(stderr, "Error: %s", json_string_value(Error));
		}
	}
	queued_callback_t **Slot = &Viewer->QueuedCallbacks;
	while (Slot[0]) {
		queued_callback_t *Queued = Slot[0];
		if (Queued->Index == Index) {
			Slot[0] = Queued->Next;
			Queued->Callback(Viewer, Result, Queued->Data);
			break;
		}
		Slot = &Queued->Next;
	}
	return TRUE;
}

static void remote_request(viewer_t *Viewer, const char *Method, json_t *Request, void (*Callback)(viewer_t *, json_t *, void *), void *Data) {
	queued_callback_t *Queued = new(queued_callback_t);
	Queued->Callback = Callback;
	Queued->Data = Data;
	Queued->Index = ++Viewer->LastCallbackIndex;
	Queued->Next = Viewer->QueuedCallbacks;
	Viewer->QueuedCallbacks = Queued;
	zmsg_t *Msg = zmsg_new();
	zmsg_addstr(Msg, json_dumps(json_pack("[iso]", Queued->Index, Method, Request), JSON_COMPACT));
	zmsg_send(&Msg, Viewer->RemoteSocket);
}

typedef struct node_foreach_t {
	void *Data;
	node_callback_t *Callback;
	double X1, Y1, X2, Y2;
} node_foreach_t;

static void foreach_node_tree_x(node_foreach_t *Foreach, node_t *Node);

static void foreach_node_tree_y(node_foreach_t *Foreach, node_t *Node) {
	if (Foreach->Y2 < Node->Y) {
		if (Node->Children[0]) foreach_node_tree_x(Foreach, Node->Children[0]);
	} else if (Foreach->Y1 > Node->Y) {
		if (Node->Children[1]) foreach_node_tree_x(Foreach, Node->Children[1]);
	} else {
		if (Foreach->X1 <= Node->X && Foreach->X2 >= Node->X) {
			Foreach->Callback(Foreach->Data, Node);
		}
		if (Node->Children[0]) foreach_node_tree_x(Foreach, Node->Children[0]);
		if (Node->Children[1]) foreach_node_tree_x(Foreach, Node->Children[1]);
	}
}

static void foreach_node_tree_x(node_foreach_t *Foreach, node_t *Node) {
	if (Foreach->X2 < Node->X) {
		if (Node->Children[0]) foreach_node_tree_y(Foreach, Node->Children[0]);
	} else if (Foreach->X1 > Node->X) {
		if (Node->Children[1]) foreach_node_tree_y(Foreach, Node->Children[1]);
	} else {
		if (Foreach->Y1 <= Node->Y && Foreach->Y2 >= Node->Y) {
			Foreach->Callback(Foreach->Data, Node);
		}
		if (Node->Children[0]) foreach_node_tree_y(Foreach, Node->Children[0]);
		if (Node->Children[1]) foreach_node_tree_y(Foreach, Node->Children[1]);
	}
}

static inline void foreach_node(viewer_t *Viewer, double X1, double Y1, double X2, double Y2, void *Data, node_callback_t *Callback) {
	node_foreach_t Foreach = {Data, Callback, X1, Y1, X2, Y2};
	//clock_t Start = clock();
	if (Viewer->Root) foreach_node_tree_x(&Foreach, Viewer->Root);
	//printf("foreach_node:%d @ %lu\n", __LINE__, clock() - Start);
}

static void merge_sort_x(node_t **Start, node_t **End, node_t **Buffer) {
	node_t **Mid = Start + (End - Start) / 2;
	if (Mid - Start > 1) merge_sort_x(Start, Mid, Buffer);
	if (End - Mid > 1) merge_sort_x(Mid, End, Buffer);
	node_t **A = Start;
	node_t **B = Mid;
	node_t **C = Buffer;
	double XA = A[0]->X;
	double XB = B[0]->X;
	for (;;) {
		if (XA <= XB) {
			*C++ = *A++;
			if (A < Mid) {
				XA = A[0]->X;
			} else {
				memcpy(C, B, (End - B) * sizeof(node_t *));
				break;
			}
		} else {
			*C++ = *B++;
			if (B < End) {
				XB = B[0]->X;
			} else {
				memcpy(C, A, (Mid - A) * sizeof(node_t *));
				break;
			}
		}
	}
	memcpy(Start, Buffer, (End - Start) * sizeof(node_t *));
}

static void merge_sort_y(node_t **Start, node_t **End, node_t **Buffer) {
	node_t **Mid = Start + (End - Start) / 2;
	if (Mid - Start > 1) merge_sort_y(Start, Mid, Buffer);
	if (End - Mid > 1) merge_sort_y(Mid, End, Buffer);
	node_t **A = Start;
	node_t **B = Mid;
	node_t **C = Buffer;
	double YA = A[0]->Y;
	double YB = B[0]->Y;
	for (;;) {
		if (YA <= YB) {
			*C++ = *A++;
			if (A < Mid) {
				YA = A[0]->Y;
			} else {
				memcpy(C, B, (End - B) * sizeof(node_t *));
				break;
			}
		} else {
			*C++ = *B++;
			if (B < End) {
				YB = B[0]->Y;
			} else {
				memcpy(C, A, (Mid - A) * sizeof(node_t *));
				break;
			}
		}
	}
	memcpy(Start, Buffer, (End - Start) * sizeof(node_t *));
}

static void split_node_list_y(node_t *Root, node_t *HeadX, node_t *HeadY1, int Count1, int Count2);

static void split_node_list_x(node_t *Root, node_t *HeadX1, node_t *HeadY, int Count1, int Count2) {
	int RootIndex = Root->XIndex;
	node_t *HeadY1 = 0, *HeadY2 = 0;
	node_t **SlotY1 = &HeadY1, **SlotY2 = &HeadY2;
	node_t *Root1 = 0, *Root2 = 0;
	int Split1 = (1 + Count1) >> 1, Split2 = (1 + Count2) >> 1;
	//printf("split_node_list_x(%d/%d, X:%d)\n\t", Count1, Count2, RootIndex);
	//int Actual = 0, Left = 0, Right = 0;
	for (node_t *Node = HeadY; Node; Node = Node->Children[1]) {
		//++Actual;
		//printf(" %d,%d", Node->XIndex, Node->YIndex);
		if (Node->XIndex < RootIndex) {
			//++Left;
			if (--Split1 == 0) {
				SlotY1[0] = 0;
				Root1 = Node;
				//printf("[1]");
			} else {
				SlotY1[0] = Node;
			}
			SlotY1 = &Node->Children[1];
		} else if (Node->XIndex > RootIndex) {
			//++Right;
			if (--Split2 == 0) {
				SlotY2[0] = 0;
				Root2 = Node;
				//printf("[2]");
			} else {
				SlotY2[0] = Node;
			}
			SlotY2 = &Node->Children[1];
		}
	}
	//printf(" -> %d/%d/%d\n", Actual, Left, Right);
	SlotY1[0] = SlotY2[0] = 0;
	node_t *HeadX2 = Root->Children[0];
	Root->Children[0] = Root1;
	Root->Children[1] = Root2;
	if (Root1) split_node_list_y(Root1, HeadX1, HeadY1, Count1 - (Count1 >> 1) - 1, Count1 >> 1);
	if (Root2) split_node_list_y(Root2, HeadX2, HeadY2, Count2 - (Count2 >> 1) - 1, Count2 >> 1);
}

static void split_node_list_y(node_t *Root, node_t *HeadX, node_t *HeadY1, int Count1, int Count2) {
	int RootIndex = Root->YIndex;
	node_t *HeadX1 = 0, *HeadX2 = 0;
	node_t **SlotX1 = &HeadX1, **SlotX2 = &HeadX2;
	node_t *Root1 = 0, *Root2 = 0;
	int Split1 = (1 + Count1) >> 1, Split2 = (1 + Count2) >> 1;
	//printf("split_node_list_y(%d/%d, X:%d)\n\t", Count1, Count2, RootIndex);
	//int Actual = 0, Left = 0, Right = 0;
	for (node_t *Node = HeadX; Node; Node = Node->Children[0]) {
		//++Actual;
		//printf(" %d,%d", Node->XIndex, Node->YIndex);
		if (Node->YIndex < RootIndex) {
			//++Left;
			if (--Split1 == 0) {
				SlotX1[0] = 0;
				Root1 = Node;
				//printf("[1]");
			} else {
				SlotX1[0] = Node;
			}
			SlotX1 = &Node->Children[0];
		} else if (Node->YIndex > RootIndex) {
			//++Right;
			if (--Split2 == 0) {
				SlotX2[0] = 0;
				Root2 = Node;
				//printf("[2]");
			} else {
				SlotX2[0] = Node;
			}
			SlotX2 = &Node->Children[0];
		}
	}
	//printf(" -> %d/%d/%d\n", Actual, Left, Right);
	SlotX1[0] = SlotX2[0] = 0;
	node_t *HeadY2 = Root->Children[1];
	Root->Children[0] = Root1;
	Root->Children[1] = Root2;
	if (Root1) split_node_list_x(Root1, HeadX1, HeadY1, Count1 - (Count1 >> 1) - 1, Count1 >> 1);
	if (Root2) split_node_list_x(Root2, HeadX2, HeadY2, Count2 - (Count2 >> 1) - 1, Count2 >> 1);
}

static void update_node_tree(viewer_t *Viewer) {
	//clock_t Start = clock();
	if (Viewer->NumFiltered == 0) {
		Viewer->Root = 0;
	} else if (Viewer->NumFiltered == 1) {
		node_t *Node = Viewer->Nodes;
		while (!Node->Filtered) ++Node;
		Node->Children[0] = 0;
		Node->Children[1] = 0;
		Viewer->Root = Node;
	} else {
		int Count1 = Viewer->NumFiltered >> 1;
		int MidIndex = Count1 + 1;
		node_t *HeadX = 0, *HeadY = 0;
		node_t **SlotX = &HeadX, **SlotY = &HeadY;
		node_t **NodeX = Viewer->SortedX, **NodeY = Viewer->SortedY;
		node_t *Root = 0;
		for (int I = Viewer->NumNodes; --I >= 0; ++NodeX, ++NodeY) {
			node_t *Node = *NodeX;
			if (Node->Filtered) {
				if (--MidIndex == 0) {
					Root = Node;
					SlotX[0] = 0;
				} else {
					SlotX[0] = Node;
				}
				SlotX = &Node->Children[0];
			}
			if (NodeY[0]->Filtered) {
				SlotY[0] = NodeY[0];
				SlotY = &NodeY[0]->Children[1];
			}
		}
		SlotX[0] = SlotY[0] = 0;
		split_node_list_x(Root, HeadX, HeadY, Count1, Viewer->NumFiltered - Count1 - 1);
		Viewer->Root = Root;
	}
	//printf("update_node_tree took %lu\n", clock() - Start);
}

static ml_value_t *viewer_global_get(viewer_t *Viewer, const char *Name) {
	return stringmap_search(Viewer->Globals, Name) ?: MLNil;
}

typedef struct node_ref_t {
	const ml_type_t *Type;
	field_t *Field;
	node_t *Node;
} node_ref_t;

static ml_value_t *node_ref_deref(node_ref_t *Ref) {
	field_t *Field = Ref->Field;
	double Value = Field->Values[Ref->Node - Ref->Node->Viewer->Nodes];
	if (Field->EnumNames) {
		if (Value) {
			return ml_string(Field->EnumNames[(int)Value], -1);
		} else {
			return MLNil;
		}
	} else {
		return ml_real(Value);
	}
}

static void column_values_set(viewer_t *Viewer, json_t *Result, field_t *Field) {

}

static int set_enum_name_fn(const char *Name, const double *Value, const char **Names) {
	Names[(int)Value[0]] = Name;
	return 0;
}

static ml_value_t *node_ref_assign(node_ref_t *Ref, ml_value_t *Value) {
	Value = Value->Type->deref(Value);
	if (Value->Type == MLErrorT) return Value;
	field_t *Field = Ref->Field;
	if (Field->EnumMap) {
		int Index;
		if (Value->Type == MLIntegerT) {
			Index = ml_integer_value(Value);
			if (Index < 0 || Index >= Field->EnumSize) return ml_error("RangeError", "enum index out of range");
		} else if (Value->Type == MLRealT) {
			Index = ml_real_value(Value);
			if (Index < 0 || Index >= Field->EnumSize) return ml_error("RangeError", "enum index out of range");
		} else if (Value->Type == MLStringT) {
			const char *Text = ml_string_value(Value);
			double **Slot = stringmap_slot(Field->EnumMap, Text);
			if (!Slot[0]) {
				double *Ref2 = Slot[0] = new(double);
				Ref2[0] = Field->EnumMap->Size;
				int EnumSize = Field->EnumSize = Field->EnumMap->Size + 1;
				const char **EnumNames = (const char **)GC_malloc(EnumSize * sizeof(const char *));
				EnumNames[0] = "";
				stringmap_foreach(Field->EnumMap, EnumNames, (void *)set_enum_name_fn);
				Field->EnumNames = EnumNames;
				gtk_list_store_clear(Field->EnumStore);
				for (int J = 0; J < EnumSize; ++J) {
					gtk_list_store_insert_with_values(Field->EnumStore, 0, -1, 0, Field->EnumNames[J], 1, (double)(J + 1), -1);
				}
				Field->EnumValues = (int *)GC_malloc_atomic(EnumSize * sizeof(int));
				Field->Range.Min = 0.0;
				Field->Range.Max = EnumSize;
			}
			Index = Slot[0][0];
		} else {
			return ml_error("TypeError", "invalid value for assignment");
		}
		Field->Values[Ref->Node - Ref->Node->Viewer->Nodes] = Index;
		if (Field->RemoteId) {
			json_t *Request = json_pack("{sss[i]s[s]}",
				"column", Field->RemoteId,
				"indices", Ref->Node - Ref->Node->Viewer->Nodes,
				"values", Field->EnumNames[Index]
			);
			remote_request(Ref->Node->Viewer, "column/values/set", Request, (void *)column_values_set, Field);
		}
	} else {
		double Value2;
		if (Value->Type == MLIntegerT) {
			Value2 = ml_integer_value(Value);
		} else if (Value->Type == MLRealT) {
			Value2 = ml_real_value(Value);
		} else {
			return ml_error("TypeError", "invalid value for assignment");
		}
		Field->Values[Ref->Node - Ref->Node->Viewer->Nodes] = Value2;
		if (Value2 < Field->Range.Min) Field->Range.Min = Value2;
		if (Value2 > Field->Range.Max) Field->Range.Max = Value2;

		if (Field->RemoteId) {
			json_t *Request = json_pack("{sss[i]s[f]}",
				"column", Field->RemoteId,
				"indices", Ref->Node - Ref->Node->Viewer->Nodes,
				"values", Value2
			);
			remote_request(Ref->Node->Viewer, "column/values/set", Request, (void *)column_values_set, Field);
		}
	}
	return Value;
}

static ml_type_t NodeRefT[1] = {{
	MLTypeT,
	MLAnyT, "node-ref",
	ml_default_hash,
	ml_default_call,
	(void *)node_ref_deref,
	(void *)node_ref_assign,
	NULL, 0, 0
}};

static ml_value_t *node_field_string_fn(void *Data, int Count, ml_value_t **Args) {
	node_t *Node = (node_t *)Args[0];
	const char *Name = ml_string_value(Args[1]);
	viewer_t *Viewer = Node->Viewer;
	field_t *Field = stringmap_search(Viewer->FieldsByName, Name);
	if (!Field) return ml_error("FieldError", "no such field %s", Name);
	node_ref_t *Ref = new(node_ref_t);
	Ref->Type = NodeRefT;
	Ref->Node = Node;
	Ref->Field = Field;
	return (ml_value_t *)Ref;
}

static ml_value_t *node_field_field_fn(void *Data, int Count, ml_value_t **Args) {
	node_t *Node = (node_t *)Args[0];
	field_t *Field = (field_t *)Args[1];
	node_ref_t *Ref = new(node_ref_t);
	Ref->Type = NodeRefT;
	Ref->Node = Node;
	Ref->Field = Field;
	return (ml_value_t *)Ref;
}

typedef struct node_image_t {
	const ml_type_t *Type;
	node_t *Node;
} node_image_t;

static ml_value_t *node_image_deref(node_image_t *Ref) {
	return ml_string(Ref->Node->FileName, -1);
}

static ml_value_t *node_image_assign(node_image_t *Ref, ml_value_t *Value) {
	Value = Value->Type->deref(Value);
	if (Value->Type == MLErrorT) return Value;
	if (Value->Type != MLStringT) return ml_error("TypeError", "Node image must be a string");
	node_t *Node = Ref->Node;
	Node->FileName = ml_string_value(Value);
	g_object_unref(Node->File);
	Node->File = g_file_new_for_path(Node->FileName);
	return Value;
}

static ml_type_t NodeImageT[1] = {{
	MLTypeT,
	MLAnyT, "node-image",
	ml_default_hash,
	ml_default_call,
	(void *)node_image_deref,
	(void *)node_image_assign,
	NULL, 0, 0
}};

static ml_value_t *node_image_fn(void *Data, int Count, ml_value_t **Args) {
	node_t *Node = (node_t *)Args[0];
	node_image_t *Ref = new(node_image_t);
	Ref->Type = NodeImageT;
	Ref->Node = Node;
	return (ml_value_t *)Ref;
}

typedef struct nodes_iter_t {
	const ml_type_t *Type;
	node_t *Nodes;
	int NumNodes;
} nodes_iter_t;

static void nodes_iter_current(ml_state_t *Caller, ml_value_t *Ref) {
	nodes_iter_t *Iter = (nodes_iter_t *)Ref;
	ML_CONTINUE(Caller, Iter->Nodes);
}

static void nodes_iter_next(ml_state_t *Caller, ml_value_t *Ref) {
	nodes_iter_t *Iter = (nodes_iter_t *)Ref;
	if (Iter->NumNodes) {
		--Iter->NumNodes;
		++Iter->Nodes;
		ML_CONTINUE(Caller, Ref);
	} else {
		ML_CONTINUE(Caller, MLNil);
	}
}

static ml_type_t NodesIterT[1] = {{
	MLTypeT,
	MLAnyT, "nodes-iter",
	ml_default_hash,
	ml_default_call,
	ml_default_deref,
	ml_default_assign,
	NULL, 0, 0
}};

static void nodes_iterate(ml_state_t *Caller, ml_value_t *Value) {
	nodes_iter_t *Nodes = (nodes_iter_t *)Value;
	if (Nodes->NumNodes) {
		nodes_iter_t *Iter = new(nodes_iter_t);
		Iter->Type = NodesIterT;
		Iter->Nodes = Nodes->Nodes;
		Iter->NumNodes = Nodes->NumNodes - 1;
		ML_CONTINUE(Caller, Iter);
	} else {
		ML_CONTINUE(Caller, MLNil);
	}
}

static ml_type_t NodesT[1] = {{
	MLTypeT,
	MLIteratableT, "nodes",
	ml_default_hash,
	ml_default_call,
	ml_default_deref,
	ml_default_assign,
	NULL, 0, 0
}};

typedef struct fields_t {
	const ml_type_t *Type;
	viewer_t *Viewer;
} fields_t;

static ml_value_t *fields_get_by_name(void *Data, int Count, ml_value_t **Args) {
	viewer_t *Viewer = ((fields_t *)Args[0])->Viewer;
	const char *Name = ml_string_value(Args[1]);
	return (ml_value_t *)stringmap_search(Viewer->FieldsByName, Name) ?: MLNil;
}

static ml_value_t *fields_get_by_index(void *Data, int Count, ml_value_t **Args) {
	viewer_t *Viewer = ((fields_t *)Args[0])->Viewer;
	int Index = ml_integer_value(Args[1]) - 1;
	if (Index < 0 || Index >= Viewer->NumFields) return ml_error("IndexError", "Invalid field index");
	return (ml_value_t *)Viewer->Fields[Index];
}

static ml_value_t *fields_new_field(void *Data, int Count, ml_value_t **Args) {
	viewer_t *Viewer = ((fields_t *)Args[0])->Viewer;
	const char *Name = ml_string_value(Args[1]);
	const char *Type = ml_string_value(Args[2]);
	int NumFields = Viewer->NumFields + 1;
	field_t **Fields = (field_t **)GC_malloc(NumFields * sizeof(field_t *));
	field_t *Field = (field_t *)GC_malloc(sizeof(field_t) + Viewer->NumNodes * sizeof(double));
	Field->Type = FieldT;
	if (!strcmp(Type, "string")) {
		Field->EnumStore = gtk_list_store_new(2, G_TYPE_STRING, G_TYPE_DOUBLE);
		Field->EnumNames = (const char **)GC_malloc(sizeof(const char *));
		Field->EnumValues = (int *)GC_malloc_atomic(sizeof(int));
		Field->EnumNames[0] = "";
		Field->EnumSize = 1;
		Field->EnumMap = new(stringmap_t);
		gtk_list_store_insert_with_values(Field->EnumStore, 0, -1, 0, "", 1, 0.0, -1);
	}
	Field->Name = GC_strdup(Name);
	Field->PreviewColumn = 0;
	Field->PreviewVisible = 1;
	Field->FilterGeneration = 0;
	Field->Sum = Field->Sum2 = 0.0;
	Field->RemoteGenerations = (json_int_t *)GC_malloc_atomic(Viewer->NumNodes * sizeof(json_int_t));
	memset(Field->RemoteGenerations, 0, Viewer->NumNodes * sizeof(json_int_t));
	memset(Field->Values, 0, Viewer->NumNodes * sizeof(double));
	for (int I = 0; I < Viewer->NumFields; ++I) Fields[I] = Viewer->Fields[I];
	Fields[Viewer->NumFields] = Field;
	stringmap_insert(Viewer->FieldsByName, Field->Name, Field);
	GC_free(Viewer->Fields);
	Viewer->Fields = Fields;
	Viewer->NumFields = NumFields;
	gtk_list_store_insert_with_values(Viewer->FieldsStore, 0, -1,
		FIELD_COLUMN_NAME, Name,
		FIELD_COLUMN_FIELD, Field,
		FIELD_COLUMN_VISIBLE, TRUE,
		FIELD_COLUMN_CONNECTED, TRUE,
		FIELD_COLUMN_REMOTE, Name,
		-1
	);
	return (ml_value_t *)Field;
}

static ml_type_t FieldsT[1] = {{
	MLTypeT,
	MLAnyT, "fields",
	ml_default_hash,
	ml_default_call,
	ml_default_deref,
	ml_default_assign,
	NULL, 0, 0
}};

static ml_value_t *field_name_fn(void *Data, int Count, ml_value_t **Args) {
	field_t *Field = (field_t *)Args[0];
	return ml_string(Field->Name, -1);
}

static ml_value_t *field_enum_value_fn(void *Data, int Count, ml_value_t **Args) {
	field_t *Field = (field_t *)Args[0];
	if (!Field->EnumMap) return ml_error("TypeError", "field is not an enum");
	const char *Name = ml_string_value(Args[1]);
	double *Ref = stringmap_search(Field->EnumMap, Name);
	if (Ref) {
		return ml_real(*Ref);
	} else {
		return ml_error("ValueError", "enum name not found");
	}
}

static ml_value_t *field_enum_name_fn(void *Data, int Count, ml_value_t **Args) {
	field_t *Field = (field_t *)Args[0];
	if (!Field->EnumMap) return ml_error("TypeError", "field is not an enumeration");
	int Value = ml_integer_value(Args[1]);
	if (Value < 0 || Value >= Field->EnumSize) return ml_error("RangeError", "enum value out of range");
	return ml_string(Field->EnumNames[Value], -1);
}

static ml_value_t *field_enum_size_fn(void *Data, int Count, ml_value_t **Args) {
	field_t *Field = (field_t *)Args[0];
	if (!Field->EnumMap) return ml_error("TypeError", "field is not an enumeration");
	return ml_integer(Field->EnumSize);
}

static ml_value_t *field_range_min_fn(void *Data, int Count, ml_value_t **Args) {
	field_t *Field = (field_t *)Args[0];
	return ml_real(Field->Range.Min);
}

static ml_value_t *field_range_max_fn(void *Data, int Count, ml_value_t **Args) {
	field_t *Field = (field_t *)Args[0];
	return ml_real(Field->Range.Max);
}

static ml_value_t *clipboard_fn(viewer_t *Viewer, int Count, ml_value_t **Args) {
	ml_value_t *AppendMethod = ml_method("append");
	ML_CHECK_ARG_COUNT(1);
	ml_stringbuffer_t Buffer[1] = {ML_STRINGBUFFER_INIT};
	for (int I = 0; I < Count; ++I) {
		ml_value_t *Result = ml_inline(AppendMethod, 2, Buffer, Args[I]);
		if (Result->Type == MLErrorT) return Result;
		if (Result != MLNil) ml_stringbuffer_add(Buffer, " ", 1);
	}
	int Length = Buffer->Length;
	const char *Text = ml_stringbuffer_get(Buffer);
	gtk_clipboard_set_text(Viewer->Clipboard, Text, Length);
	return MLNil;
}

static void set_viewer_indices(viewer_t *Viewer, int XIndex, int YIndex) {
	Viewer->XIndex = XIndex;
	Viewer->YIndex = YIndex;
	if (XIndex == -1 || YIndex == -1) return;
	int NumNodes = Viewer->NumNodes;
	field_t *XField = Viewer->Fields[XIndex];
	field_t *YField = Viewer->Fields[YIndex];

	node_t *Node = Viewer->Nodes;
	double *XValue = XField->Values;
	double *YValue = YField->Values;
	for (int I = NumNodes; --I >= 0;) {
		Node->X = *XValue;
		Node->Y = *YValue;
		++Node;
		++XValue;
		++YValue;
	}
	merge_sort_x(Viewer->SortedX, Viewer->SortedX + NumNodes, Viewer->SortBuffer);
	merge_sort_y(Viewer->SortedY, Viewer->SortedY + NumNodes, Viewer->SortBuffer);
	for (int I = 0; I < NumNodes; ++I) {
		Viewer->SortedX[I]->XIndex = I;
		Viewer->SortedY[I]->YIndex = I;
	}
	update_node_tree(Viewer);
	double RangeX = XField->Range.Max - XField->Range.Min;
	double RangeY = YField->Range.Max - YField->Range.Min;
	if (RangeX < 1e-9) RangeX = 1e-9;
	if (RangeY < 1e-9) RangeY = 1e-9;
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

static void clear_viewer_indices(viewer_t *Viewer) {
	Viewer->XIndex = -1;
	Viewer->YIndex = -1;
	int NumNodes = Viewer->NumNodes;

	node_t *Node = Viewer->Nodes;
	for (int I = NumNodes; --I >= 0;) {
		Node->X = (double)rand() / RAND_MAX;
		Node->Y = (double)rand() / RAND_MAX;
		Node->Colour = 0xFF000000;
		++Node;
	}
	merge_sort_x(Viewer->SortedX, Viewer->SortedX + NumNodes, Viewer->SortBuffer);
	merge_sort_y(Viewer->SortedY, Viewer->SortedY + NumNodes, Viewer->SortBuffer);
	for (int I = 0; I < NumNodes; ++I) {
		Viewer->SortedX[I]->XIndex = I;
		Viewer->SortedY[I]->YIndex = I;
	}
	update_node_tree(Viewer);
	double RangeX = 1.0;
	double RangeY = 1.0;
	Viewer->DataMin.X = 0.0 - RangeX * 0.01;
	Viewer->DataMin.Y = 0.0 - RangeY * 0.01;
	Viewer->DataMax.X = 1.0 + RangeX * 0.01;
	Viewer->DataMax.Y = 1.0 + RangeY * 0.01;
	Viewer->Min = Viewer->DataMin;
	Viewer->Max = Viewer->DataMax;
	int Width = gtk_widget_get_allocated_width(Viewer->DrawingArea);
	int Height = gtk_widget_get_allocated_height(Viewer->DrawingArea);
	Viewer->Scale.X = Width / (Viewer->Max.X - Viewer->Min.X);
	Viewer->Scale.Y = Height / (Viewer->Max.Y - Viewer->Min.Y);
}

static inline void set_node_rgb(node_t *Node, double H) {
	double R, G, B;
	if (H < 1.0) {
		R = POINT_COLOUR_VALUE;
		G = POINT_COLOUR_VALUE - POINT_COLOUR_CHROMA * fabs(H - 1.0);
		B = POINT_COLOUR_VALUE - POINT_COLOUR_CHROMA;
	} else if (H < 2.0) {
		R = POINT_COLOUR_VALUE - POINT_COLOUR_CHROMA * fabs(H - 1.0);
		G = POINT_COLOUR_VALUE;
		B = POINT_COLOUR_VALUE - POINT_COLOUR_CHROMA;
	} else if (H < 3.0) {
		R = POINT_COLOUR_VALUE - POINT_COLOUR_CHROMA;
		G = POINT_COLOUR_VALUE;
		B = POINT_COLOUR_VALUE - POINT_COLOUR_CHROMA * fabs(H - 3.0);
	} else if (H < 4.0) {
		R = POINT_COLOUR_VALUE - POINT_COLOUR_CHROMA;
		G = POINT_COLOUR_VALUE - POINT_COLOUR_CHROMA * fabs(H - 3.0);
		B = POINT_COLOUR_VALUE;
	} else if (H < 5.0) {
		R = POINT_COLOUR_VALUE - POINT_COLOUR_CHROMA * fabs(H - 5.0);
		G = POINT_COLOUR_VALUE - POINT_COLOUR_CHROMA;
		B = POINT_COLOUR_VALUE;
	} else {
		R = POINT_COLOUR_VALUE;
		G = POINT_COLOUR_VALUE - POINT_COLOUR_CHROMA;
		B = POINT_COLOUR_VALUE - POINT_COLOUR_CHROMA * fabs(H - 5.0);
	}
#ifdef USE_GL
	Node->R = R;
	Node->G = G;
	Node->B = B;
#else
	Node->Colour =
		(((unsigned int)255) << 24) +
		(((unsigned int)((R * 255) + 0.5)) << 16) +
		(((unsigned int)((G * 255) + 0.5)) << 8) +
		(((unsigned int)((B * 255) + 0.5)));
#endif
}

static void filter_enum_field(viewer_t *Viewer, field_t *Field) {
	printf("filter_enum_field(%s)\n", Field->Name);
	node_t *Node = Viewer->Nodes;
	int NumNodes = Viewer->NumNodes;
	int *EnumValues = Field->EnumValues;
	memset(EnumValues, 0, Field->EnumSize * sizeof(int));
	int Max = 0;
	double *Value = Field->Values;
	for (int I = NumNodes; --I >= 0;) {
		if (Node->Filtered) {
			int Index = (int)Value[0];
			if (Index && !EnumValues[Index]) EnumValues[Index] = ++Max;
		}
		++Node;
		++Value;
	}
	Field->Range.Max = Max;
	printf("Field->Range.Max = %d\n", Max);
	Field->FilterGeneration = Viewer->FilterGeneration;
}

static void set_viewer_colour_index(viewer_t *Viewer, int CIndex) {
	Viewer->CIndex = CIndex;
	int NumNodes = Viewer->NumNodes;
	field_t *CField = Viewer->Fields[CIndex];
	node_t *Node = Viewer->Nodes;
	double *CValue = CField->Values;
	if (CField->EnumStore) {
		if (CField->FilterGeneration != Viewer->FilterGeneration) {
			filter_enum_field(Viewer, CField);
		}
		int *EnumValues = CField->EnumValues;
		double Range = CField->Range.Max + 1;
		for (int I = NumNodes; --I >= 0;) {
			int Value = EnumValues[(int)CValue[0]];
			if (Value > 0.0) {
				set_node_rgb(Node, 6.0 * Value / Range);
			} else {
#ifdef USE_GL
				Node->R = Node->G = Node->B = POINT_COLOUR_SATURATION;
#else
				Node->Colour = 0xFF808080;
#endif
			}
			++Node;
			++CValue;
		}
	} else {
		double Min = CField->Range.Min;
		double Range = CField->Range.Max - Min;
		if (Range <= 1.0e-6) Range = 1.0;
		Range += CField->SD;
		for (int I = NumNodes; --I >= 0;) {
			set_node_rgb(Node, 6.0 * (*CValue - Min) / Range);
			++Node;
			++CValue;
		}
	}
}

static void draw_node_image_loaded(GObject *Source, GAsyncResult *Result, node_t *Node) {
	viewer_t *Viewer = Node->Viewer;
	g_input_stream_close(Node->LoadStream, 0, 0);
	g_object_unref(G_OBJECT(Node->LoadStream));
	Node->LoadStream = 0;
	gboolean Cancelled = g_cancellable_is_cancelled(Node->LoadCancel);
	g_object_unref(G_OBJECT(Node->LoadCancel));
	Node->LoadCancel = 0;
	if (Cancelled) return;
	Node->Pixbuf = gdk_pixbuf_new_from_stream_finish(Result, 0);
	if (!Node->Pixbuf) {
		printf("Generating image %s (image read error)\n", Node->FileName);
		guchar *Pixels = GC_malloc_atomic(128 * 192 * 4);
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
	if (Node->LoadGeneration == Viewer->LoadGeneration) {
		gtk_list_store_insert_with_values(Viewer->ImagesStore, 0, -1,
			0, Node->FileName,
			1, Node->Pixbuf,
			2, Node,
		-1);
	}
}

static void draw_node_file_opened(GObject *Source, GAsyncResult *Result, node_t *Node) {
	gboolean Cancelled = g_cancellable_is_cancelled(Node->LoadCancel);
	if (Cancelled) {
		g_object_unref(G_OBJECT(Node->LoadCancel));
		Node->LoadCancel = 0;
		return;
	}
	GFileInputStream *InputStream = g_file_read_finish(Node->File, Result, 0);
	if (InputStream) {
		Node->LoadStream = G_INPUT_STREAM(InputStream);
		gdk_pixbuf_new_from_stream_at_scale_async(
			Node->LoadStream,
			128, 192, TRUE,
			Node->LoadCancel,
			(void *)draw_node_image_loaded,
			Node
		);
	} else {
		g_object_unref(G_OBJECT(Node->LoadCancel));
		Node->LoadCancel = 0;
		viewer_t *Viewer = Node->Viewer;
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
		gtk_list_store_insert_with_values(Viewer->ImagesStore, 0, -1,
			0, Node->FileName,
			1, Node->Pixbuf,
			2, Node,
		-1);
	}
}

static int draw_node_image(viewer_t *Viewer, node_t *Node) {
	Node->Next = Viewer->Selected;
	Viewer->Selected = Node;
	++Viewer->NumVisible;
	if (Viewer->NumVisible <= MAX_VISIBLE_IMAGES) {
		Node->LoadGeneration = Viewer->LoadGeneration;
		if (Node->Pixbuf) {
			gtk_list_store_insert_with_values(Viewer->ImagesStore, 0, -1,
				0, Node->FileName,
				1, Node->Pixbuf,
				2, Node,
			-1);
		} else if (!Node->LoadCancel) {
			int Index = Viewer->LoadCacheIndex;
			node_t **Cache = Viewer->LoadCache;
			while (Cache[Index] && (Cache[Index]->LoadGeneration == Viewer->LoadGeneration)) {
				Index = (Index + 1) % MAX_CACHED_IMAGES;
			}
			if (Cache[Index]) {
				node_t *OldNode = Cache[Index];
				if (OldNode->Pixbuf) {
					g_object_unref(G_OBJECT(OldNode->Pixbuf));
					OldNode->Pixbuf = 0;
				} else if (OldNode->LoadCancel) {
					g_cancellable_cancel(OldNode->LoadCancel);
				}
			}
			Cache[Index] = Node;
			Viewer->LoadCacheIndex = (Index + 1) % MAX_CACHED_IMAGES;
			Node->LoadCancel = g_cancellable_new();
			g_file_read_async(Node->File, G_PRIORITY_DEFAULT, Node->LoadCancel, (void *)draw_node_file_opened, Node);
		}
	}
	return 0;
}

static int draw_node_value(viewer_t *Viewer, node_t *Node) {
	Node->Next = Viewer->Selected;
	Viewer->Selected = Node;
	++Viewer->NumVisible;
	field_t **Fields = Viewer->Fields;
	int NumFields = Viewer->NumFields;
	int Index = Node - Viewer->Nodes;
	GtkTreeIter Iter[1];
	gtk_list_store_append(Viewer->ValuesStore, Iter);
	gtk_list_store_set(Viewer->ValuesStore, Iter, 0, Node->FileName, -1);
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
				gtk_list_store_set(Viewer->ValuesStore, Iter, 2 * I + 1, Field->EnumNames[(int)Value], 2 * I + 2, Colour, -1);
			} else {
				gtk_list_store_set(Viewer->ValuesStore, Iter, 2 * I + 1, Value, 2 * I + 2, &Colour, -1);
			}
		}
	}
	return 0;
}

static void update_preview(viewer_t *Viewer) {
	Viewer->NumVisible = 0;
	double X1 = Viewer->Min.X + (Viewer->Pointer.X - BOX_SIZE / 2) / Viewer->Scale.X;
	double Y1 = Viewer->Min.Y + (Viewer->Pointer.Y - BOX_SIZE / 2) / Viewer->Scale.Y;
	double X2 = Viewer->Min.X + (Viewer->Pointer.X + BOX_SIZE / 2) / Viewer->Scale.X;
	double Y2 = Viewer->Min.Y + (Viewer->Pointer.Y + BOX_SIZE / 2) / Viewer->Scale.Y;
	Viewer->Selected = 0;
	if (Viewer->ImagesStore) {
		++Viewer->LoadGeneration;
		gtk_list_store_clear(Viewer->ImagesStore);
		//printf("\n\n%s:%d\n", __FUNCTION__, __LINE__);
		foreach_node(Viewer, X1, Y1, X2, Y2, Viewer, (node_callback_t *)draw_node_image);
	} else if (Viewer->ValuesStore) {
		gtk_list_store_clear(Viewer->ValuesStore);
		//printf("\n\n%s:%d\n", __FUNCTION__, __LINE__);
		foreach_node(Viewer, X1, Y1, X2, Y2, Viewer, (node_callback_t *)draw_node_value);
	}
	char NumVisibleText[64];
	sprintf(NumVisibleText, "%d points", Viewer->NumVisible);
	gtk_label_set_text(Viewer->NumVisibleLabel, NumVisibleText);
}

static int edit_node_value(viewer_t *Viewer, node_t *Node) {
	field_t *Field = Viewer->EditField;
	++Viewer->NumUpdated;
	double Value = Field->Values[Node - Viewer->Nodes] = Viewer->EditValue;
	if (Field == Viewer->Fields[Viewer->CIndex]) {
		set_node_rgb(Node, 6.0 * (Value - Field->Range.Min) / (Field->Range.Max - Field->Range.Min));
	}
	return 0;
}

typedef struct edit_node_remote_t {
	viewer_t *Viewer;
	field_t *Field;
	json_t *Indices;
	json_t *Values;
} edit_node_remote_t;

static int edit_node_value_remote(edit_node_remote_t *Info, node_t *Node) {
	viewer_t *Viewer = Info->Viewer;
	field_t *Field = Info->Field;
	++Viewer->NumUpdated;
	size_t Index = Node - Viewer->Nodes;
	double Value = Field->Values[Index] = Viewer->EditValue;
	if (Field == Viewer->Fields[Viewer->CIndex]) {
		set_node_rgb(Node, 6.0 * (Value - Field->Range.Min) / (Field->Range.Max - Field->Range.Min));
	}
	json_array_append(Info->Indices, json_integer(Index));
	json_array_append(Info->Values, json_string(Field->EnumNames[(int)Value]));
	return 0;
}

static void viewer_filter_nodes(viewer_t *Viewer);

static void column_values_set_event(viewer_t *Viewer, const char *Event, json_t *Details) {
	const char *RemoteId;
	json_int_t Generation;
	json_t *Indices, *Values;
	if (json_unpack(Details, "[sioo]", &RemoteId, &Generation, &Indices, &Values)) return;
	field_t *Field = stringmap_search(Viewer->RemoteFields, RemoteId);
	if (!Field) return;
	int Length = json_array_size(Indices);
	if (Field->EnumMap) {
		int EnumUpdated = 0;
		for (int I = 0; I < Length; ++I) {
			size_t Index = json_integer_value(json_array_get(Indices, I));
			const char *Text = json_string_value(json_array_get(Values, I));
			double Value = 0.0;
			if (Text && Text[0]) {
				double *Ref = stringmap_search(Field->EnumMap, Text);
				if (Ref) {
					Value = *(double *)Ref;
				} else {
					Ref = new(double);
					stringmap_insert(Field->EnumMap, GC_strdup(Text), Ref);
					*(double *)Ref = Value = Field->EnumMap->Size;
					EnumUpdated = 1;
				}
			}
			Field->Values[Index] = Value;
		}
		if (EnumUpdated) {
			int EnumSize = Field->EnumSize = Field->EnumMap->Size + 1;
			const char **EnumNames = (const char **)GC_malloc(EnumSize * sizeof(const char *));
			EnumNames[0] = "";
			stringmap_foreach(Field->EnumMap, EnumNames, (void *)set_enum_name_fn);
			Field->EnumNames = EnumNames;
			gtk_list_store_clear(Field->EnumStore);
			for (int J = 0; J < EnumSize; ++J) {
				gtk_list_store_insert_with_values(Field->EnumStore, 0, -1, 0, Field->EnumNames[J], 1, (double)(J + 1), -1);
			}
			Field->EnumValues = (int *)GC_malloc_atomic(EnumSize * sizeof(int));
			Field->Range.Min = 0.0;
			Field->Range.Max = EnumSize;
		}
	} else {
		double Min = Field->Range.Min;
		double Max = Field->Range.Max;
		double Sum = Field->Sum;
		double Sum2 = Field->Sum2;
		for (int I = 0; I < Length; ++I) {
			size_t Index = json_integer_value(json_array_get(Indices, I));
			double Value = Field->Values[Index] = json_number_value(json_array_get(Values, I));
			if (Value < Min) Min = Value;
			if (Value > Max) Max = Value;
		}
		Field->Range.Min = Min;
		Field->Range.Max = Max;
	}
	viewer_filter_nodes(Viewer);
	int Redraw = 0;
	for (int Index = 0; Index < Viewer->NumFields; ++Index) {
		if (Viewer->Fields[Index] == Field) {
			if (Index == Viewer->CIndex) {
				Redraw = 1;
				++Viewer->FilterGeneration;
				set_viewer_colour_index(Viewer, Viewer->CIndex);
			}
			if (Index == Viewer->XIndex || Index == Viewer->YIndex) {
				Redraw = 1;
				set_viewer_indices(Viewer, Viewer->XIndex, Viewer->YIndex);
			}
			break;
		}
	}
	if (Redraw) {
		Viewer->RedrawBackground = 1;
		update_preview(Viewer);
		gtk_widget_queue_draw(Viewer->DrawingArea);
	}
}

static void edit_node_values(viewer_t *Viewer) {
	if (!Viewer->EditField) return;
	Viewer->NumUpdated = 0;
	double X1 = Viewer->Min.X + (Viewer->Pointer.X - BOX_SIZE / 2) / Viewer->Scale.X;
	double Y1 = Viewer->Min.Y + (Viewer->Pointer.Y - BOX_SIZE / 2) / Viewer->Scale.Y;
	double X2 = Viewer->Min.X + (Viewer->Pointer.X + BOX_SIZE / 2) / Viewer->Scale.X;
	double Y2 = Viewer->Min.Y + (Viewer->Pointer.Y + BOX_SIZE / 2) / Viewer->Scale.Y;
	printf("\n\n%s:%d\n", __FUNCTION__, __LINE__);
	field_t *Field = Viewer->EditField;
	if (Field->RemoteId) {
		edit_node_remote_t Info[1] = {{Viewer, Field, json_array(), json_array()}};
		foreach_node(Viewer, X1, Y1, X2, Y2, Info, (node_callback_t *)edit_node_value_remote);
		json_t *Request = json_pack("{sssoso}",
			"column", Field->RemoteId,
			"indices", Info->Indices,
			"values", Info->Values
		);
		remote_request(Viewer, "column/values/set", Request, (void *)column_values_set, Field);
	} else {
		foreach_node(Viewer, X1, Y1, X2, Y2, Viewer, (node_callback_t *)edit_node_value);
	}
	if (Field == Viewer->Fields[Viewer->CIndex]) {
		++Viewer->FilterGeneration;
		set_viewer_colour_index(Viewer, Viewer->CIndex);
	}
	char NumVisibleText[64];
	sprintf(NumVisibleText, "%d updates", Viewer->NumUpdated);
	gtk_label_set_text(Viewer->NumVisibleLabel, NumVisibleText);
	viewer_filter_nodes(Viewer);
}

static inline int redraw_point(viewer_t *Viewer, node_t *Node) {
#ifdef USE_GL
	//double X = (Node->X - Viewer->Min.X) / (Viewer->Max.X - Viewer->Min.X);
	//double Y = (Node->Y - Viewer->Min.Y) / (Viewer->Max.Y - Viewer->Min.Y);
	double X = Viewer->Scale.X * (Node->X - Viewer->Min.X);
	double Y = Viewer->Scale.Y * (Node->Y - Viewer->Min.Y);
	//printf("Point at (%f, %f)\n", X, Y);
	int Index = Viewer->GLCount;
	Viewer->GLVertices[3 * Index + 0] = X - POINT_SIZE / 2;
	Viewer->GLVertices[3 * Index + 1] = Y + POINT_SIZE * 0.33;
	Viewer->GLVertices[3 * Index + 2] = 0.0;
	Viewer->GLVertices[3 * Index + 3] = X;
	Viewer->GLVertices[3 * Index + 4] = Y - POINT_SIZE * 0.66;
	Viewer->GLVertices[3 * Index + 5] = 0.0;
	Viewer->GLVertices[3 * Index + 6] = X + POINT_SIZE / 2;
	Viewer->GLVertices[3 * Index + 7] = Y + POINT_SIZE * 0.33;
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
	int X0 = (X - POINT_SIZE / 2) + 0.5;
	int Y0 = (Y - POINT_SIZE / 2) + 0.5;
	int Stride = Viewer->CachedStride;
	unsigned int *Pixels = Viewer->CachedPixels + X0;
	Pixels = (unsigned int *)((char *)Pixels + Y0 * Stride);
	unsigned int Colour = Node->Colour;
	int PointSize = POINT_SIZE;
	for (int J = PointSize; --J >= 0;) {
		for (int I = 0; I < PointSize; ++I) Pixels[I] = Colour;
		Pixels = (unsigned int *)((char *)Pixels + Stride);
	}
	/*cairo_t *Cairo = Viewer->Cairo;
	cairo_new_path(Cairo);
	cairo_rectangle(Cairo, X - POINT_SIZE / 2, Y - POINT_SIZE / 2, POINT_SIZE, POINT_SIZE);
	cairo_set_source_rgb(Cairo, Node->R, Node->G, Node->B);
	cairo_fill(Cairo);*/
#endif
	return 0;
}

static void redraw_viewer_background(viewer_t *Viewer) {
#ifdef USE_GL
	Viewer->GLCount = 0;
	//clock_t Start = clock();
	printf("\n\n%s:%d\n", __FUNCTION__, __LINE__);
	foreach_node(Viewer, Viewer->Min.X, Viewer->Min.Y, Viewer->Max.X, Viewer->Max.Y, Viewer, (node_callback_t *)redraw_point);
	//printf("foreach_node took %d\n", clock() - Start);
	//printf("rendered %d points\n", Viewer->GLCount);
	if (Viewer->GLReady) {
		gtk_gl_area_make_current(GTK_GL_AREA(Viewer->DrawingArea));
		glBindBuffer(GL_ARRAY_BUFFER, Viewer->GLBuffers[0]);
		glBufferData(GL_ARRAY_BUFFER, Viewer->GLCount * 3 * sizeof(float), Viewer->GLVertices, GL_STATIC_DRAW);
		glBindBuffer(GL_ARRAY_BUFFER, Viewer->GLBuffers[1]);
		glBufferData(GL_ARRAY_BUFFER, Viewer->GLCount * 4 * sizeof(float), Viewer->GLColours, GL_STATIC_DRAW);
		glBindBuffer(GL_ARRAY_BUFFER, 0);
	}
#else
	Viewer->RedrawBackground = 1;
	/*guint Width = cairo_image_surface_get_width(Viewer->CachedBackground);
	guint Height = cairo_image_surface_get_height(Viewer->CachedBackground);
	cairo_t *Cairo = cairo_create(Viewer->CachedBackground);
	cairo_set_source_rgb(Cairo, 1.0, 1.0, 1.0);
	cairo_rectangle(Cairo, 0.0, 0.0, Width, Height);
	cairo_fill(Cairo);
	Viewer->Cairo = Cairo;
	clock_t Start = clock();
	printf("\n\n%s:%d\n", __FUNCTION__, __LINE__);
	foreach_node(Viewer, Viewer->Min.X, Viewer->Min.Y, Viewer->Max.X, Viewer->Max.Y, Viewer, (node_callback_t *)redraw_point);
	printf("foreach_node took %lu\n", clock() - Start);
	Viewer->Cairo = 0;
	cairo_destroy(Cairo);*/
#endif
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

	float BoxX1 = Viewer->Pointer.X - BOX_SIZE / 2;
	float BoxY1 = Viewer->Pointer.Y - BOX_SIZE / 2;
	float BoxX2 = Viewer->Pointer.X + BOX_SIZE / 2;
	float BoxY2 = Viewer->Pointer.Y + BOX_SIZE / 2;

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

static void realize_viewer_gl(GtkGLArea *Widget, viewer_t *Viewer) {
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
static void realize_viewer(GtkWidget *Widget, viewer_t *Viewer) {
	gdk_window_set_cursor(gtk_widget_get_window(Viewer->DrawingArea), Viewer->Cursor);
}

static void redraw_viewer(GtkWidget *Widget, cairo_t *Cairo, viewer_t *Viewer) {
	if (Viewer->RedrawBackground) {
		Viewer->RedrawBackground = 1;
		guint Width = cairo_image_surface_get_width(Viewer->CachedBackground);
		guint Height = cairo_image_surface_get_height(Viewer->CachedBackground);
		cairo_t *Cairo = cairo_create(Viewer->CachedBackground);
		cairo_set_source_rgb(Cairo, 1.0, 1.0, 1.0);
		cairo_rectangle(Cairo, 0.0, 0.0, Width, Height);
		cairo_fill(Cairo);
		Viewer->Cairo = Cairo;
		//clock_t Start = clock();
		//printf("\n\n%s:%d\n", __FUNCTION__, __LINE__);
		foreach_node(Viewer, Viewer->Min.X, Viewer->Min.Y, Viewer->Max.X, Viewer->Max.Y, Viewer, (node_callback_t *)redraw_point);
		//printf("foreach_node took %lu\n", clock() - Start);
		Viewer->Cairo = 0;
		cairo_destroy(Cairo);
	}
	cairo_set_source_surface(Cairo, Viewer->CachedBackground, 0.0, 0.0);
	cairo_paint(Cairo);
	if (Viewer->ShowBox) {
		cairo_new_path(Cairo);
		cairo_rectangle(Cairo,
			Viewer->Pointer.X - BOX_SIZE / 2,
			Viewer->Pointer.Y - BOX_SIZE / 2,
			BOX_SIZE,
			BOX_SIZE
		);
		cairo_set_source_rgb(Cairo, 1.0, 1.0, 0.5);
		cairo_stroke_preserve(Cairo);
		cairo_set_source_rgba(Cairo, 1.0, 1.0, 0.5, 0.5);
		cairo_fill(Cairo);
	}
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

static void resize_viewer(GtkWidget *Widget, GdkRectangle *Allocation, viewer_t *Viewer) {
	Viewer->Scale.X = Allocation->width / (Viewer->Max.X - Viewer->Min.X);
	Viewer->Scale.Y = Allocation->height / (Viewer->Max.Y - Viewer->Min.Y);
#ifdef USE_GL
#else
	if (Viewer->CachedBackground) {
		cairo_surface_destroy(Viewer->CachedBackground);
	}
	int PointSize = POINT_SIZE;
	int BufferSize = PointSize + 2;
	unsigned char *Pixels = GC_malloc_atomic((Allocation->width + 2 * BufferSize) * (Allocation->height + 2 * BufferSize) * sizeof(int));
	int Stride = (Allocation->width + 2 * BufferSize) * sizeof(unsigned int);
	Viewer->CachedBackground = cairo_image_surface_create_for_data(
		Pixels + BufferSize * Stride + BufferSize * sizeof(int),
		CAIRO_FORMAT_ARGB32,
		Allocation->width,
		Allocation->height,
		Stride
	);
	//cairo_image_surface_create(CAIRO_FORMAT_RGB24, Allocation->width, Allocation->height);
	Viewer->CachedPixels = (unsigned int *)cairo_image_surface_get_data(Viewer->CachedBackground);
	Viewer->CachedStride = cairo_image_surface_get_stride(Viewer->CachedBackground);
#endif
	redraw_viewer_background(Viewer);
	//update_preview(Viewer);
	gtk_widget_queue_draw(Widget);
}

static gboolean scroll_viewer(GtkWidget *Widget, GdkEventScroll *Event, viewer_t *Viewer) {
	printf("scroll_viewer()\n");
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
		if (Event->state & GDK_CONTROL_MASK) {
			edit_node_values(Viewer);
			redraw_viewer_background(Viewer);
		} else {
			update_preview(Viewer);
			Viewer->ShowBox = 0;
		}
		gtk_widget_queue_draw(Widget);
	} else if (Event->button == 2) {
		Viewer->Pointer.X = Event->x;
		Viewer->Pointer.Y = Event->y;
	} else if (Event->button == 3) {
		Viewer->Pointer.X = Event->x;
		Viewer->Pointer.Y = Event->y;
		edit_node_values(Viewer);
		redraw_viewer_background(Viewer);
		gtk_widget_queue_draw(Widget);
	}
	return FALSE;
}

static gboolean button_release_viewer(GtkWidget *Widget, GdkEventButton *Event, viewer_t *Viewer) {
	if (Event->button == 1) {
		Viewer->ShowBox = 1;
		gtk_widget_queue_draw(Widget);
	}
	return FALSE;
}

static gboolean motion_notify_viewer(GtkWidget *Widget, GdkEventMotion *Event, viewer_t *Viewer) {
	if (Event->state & GDK_BUTTON2_MASK) {
		double DeltaX = (Viewer->Pointer.X - Event->x) / Viewer->Scale.X;
		double DeltaY = (Viewer->Pointer.Y - Event->y) / Viewer->Scale.Y;
		pan_viewer(Viewer, DeltaX, DeltaY);
		redraw_viewer_background(Viewer);
		Viewer->Pointer.X = Event->x;
		Viewer->Pointer.Y = Event->y;
		gtk_widget_queue_draw(Widget);
	} else if (Event->state & GDK_BUTTON1_MASK) {
		Viewer->Pointer.X = Event->x;
		Viewer->Pointer.Y = Event->y;
		update_preview(Viewer);
	} else if (Event->state & GDK_SHIFT_MASK) {
		double DeltaX = (Viewer->Pointer.X - Event->x) / Viewer->Scale.X;
		double DeltaY = (Viewer->Pointer.Y - Event->y) / Viewer->Scale.Y;
		pan_viewer(Viewer, DeltaX, DeltaY);
		redraw_viewer_background(Viewer);
		Viewer->Pointer.X = Event->x;
		Viewer->Pointer.Y = Event->y;
		gtk_widget_queue_draw(Widget);
	}
	return FALSE;
}

static void images_selected_foreach(GtkIconView *ImagesView, GtkTreePath *Path, viewer_t *Viewer) {
	GtkTreeIter Iter[1];
	gtk_tree_model_get_iter(GTK_TREE_MODEL(Viewer->ImagesStore), Iter, Path);
	const char *Value = 0;
	gtk_tree_model_get(GTK_TREE_MODEL(Viewer->ImagesStore), Iter, 0, &Value, -1);
	gtk_clipboard_set_text(Viewer->Clipboard, Value, -1);
	g_free((void *)Value);
}

static gboolean key_press_viewer(GtkWidget *Widget, GdkEventKey *Event, viewer_t *Viewer) {
	printf("key_press_viewer()\n");
	if (!(Event->state & GDK_CONTROL_MASK)) return FALSE;
	switch (Event->keyval) {
	case GDK_KEY_s: {
#ifdef USE_GL
#else
		cairo_surface_write_to_png(Viewer->CachedBackground, "screenshot.png");
#endif
		return TRUE;
	}
	case GDK_KEY_0: case GDK_KEY_1: case GDK_KEY_2: case GDK_KEY_3:
	case GDK_KEY_4: case GDK_KEY_5: case GDK_KEY_6: case GDK_KEY_7:
	case GDK_KEY_8: case GDK_KEY_9: {
		ml_value_t *HotkeyFn = Viewer->HotkeyFns[Event->keyval - GDK_KEY_0];
		for (node_t *Node = Viewer->Selected; Node; Node = Node->Next) {
			ml_value_t *Result = ml_inline(HotkeyFn, 1, Node);
			if (Result->Type == MLErrorT) {
				console_log(Viewer->Console, Result);
				break;
			}
		}
		++Viewer->FilterGeneration;
		set_viewer_colour_index(Viewer, Viewer->CIndex);
		viewer_filter_nodes(Viewer);
		return TRUE;
	}
	}
	return FALSE;
}

typedef struct connect_info_t {
	viewer_t *Viewer;
	const char *Server;
	GtkWidget *Dialog;
	GtkListStore *DatasetsStore;
	GtkComboBox *DatasetCombo;
	const char *DatasetId;
	const char *ImagePrefix;
} connect_info_t;

static void connect_server_changed(GtkComboBoxText *ComboBox, connect_info_t *Info) {
	Info->Server = gtk_combo_box_text_get_active_text(ComboBox);
}

static void connect_dataset_changed(GtkComboBox *ComboBox, connect_info_t *Info) {
	GtkTreeIter Iter[1];
	if (gtk_combo_box_get_active_iter(ComboBox, Iter)) {
		gtk_tree_model_get(GTK_TREE_MODEL(Info->DatasetsStore), Iter, 0, &Info->DatasetId, -1);
	}
}

/*static gboolean remote_msg_fn(GIOChannel *Source, GIOCondition Condition, viewer_t *Viewer) {
	for (;;) {
		printf("remote_msg_fn(%x)\n", Source);
		int Status = zsock_events(Viewer->RemoteSocket);
		printf("Status = %d\n", Status);
		if (Status & ZMQ_POLLIN == 0) break;
		printf("Reading message\n");
		zmsg_t *Msg = zmsg_recv_nowait(Viewer->RemoteSocket);
		if (!Msg) break;
		zmsg_print(Msg);
		zframe_t *Frame = zmsg_pop(Msg);
		json_error_t Error;
		json_t *Response = json_loadb(zframe_data(Frame), zframe_size(Frame), 0, &Error);
		if (!Response) {
			fprintf(stderr, "Error parsing json\n");
			continue;
		}
		int Index;
		json_t *Result;
		if (json_unpack(Response, "[io]", &Index, &Result)) {
			fprintf(stderr, "Error invalid json\n");
			continue;
		}
		if (Index == 0) {
			// TODO: Handle alerts
			continue;
		}
		if (json_is_object(Result)) {
			json_t *Error = json_object_get(Result, "error");
			if (Error) {
				fprintf(stderr, "Error: %s", json_string_value(Error));
				continue;
			}
		}
		queued_callback_t **Slot = &Viewer->QueuedCallbacks;
		while (Slot[0]) {
			queued_callback_t *Queued = Slot[0];
			if (Queued->Index == Index) {
				Slot[0] = Queued->Next;
				Queued->Callback(Viewer, Result, Queued->Data);
				break;
			}
		}
	}
	return TRUE;
}*/

static void connect_dataset_list(viewer_t *Viewer, json_t *Result, connect_info_t *Info) {
	//printf("connect_dataset_list(%s)\n", json_dumps(Result, 0));
	gtk_list_store_clear(Info->DatasetsStore);
	for (int I = 0; I < json_array_size(Result); ++I) {
		const char *Id, *Name;
		int Length;
		if (!json_unpack(json_array_get(Result, I), "{sss{sssi}}", "id", &Id, "info", "name", &Name, "length", &Length)) {
			gtk_list_store_insert_with_values(Info->DatasetsStore, 0, -1, 0, Id, 1, Name, 2, Length, -1);
		}
	}
}

static void connect_connect_clicked(GtkWidget *Button, connect_info_t *Info) {
	viewer_t *Viewer = Info->Viewer;
	if (Info->Server) {
		if (Viewer->RemoteSocket) zsock_destroy(&Viewer->RemoteSocket);
		zsock_t *Socket = Viewer->RemoteSocket = zsock_new_dealer(Info->Server);
		/*if (!zsock_use_fd(Socket)) {
			Viewer->RemoteSocket = NULL;
			fprintf(stderr, "Error retrieving ZeroMQ file descriptor\n");
			return;
		}*/
		zmsg_t *Msg = zmsg_new();
		json_t *Request = json_pack("[isn]", ++Viewer->LastCallbackIndex, "dataset/list");
		zmsg_addstr(Msg, json_dumps(Request, JSON_COMPACT));
		zmsg_send(&Msg, Socket);
		Msg = zmsg_recv(Socket);
		zframe_t *Frame = zmsg_pop(Msg);
		json_error_t Error;
		json_t *Response = json_loadb(zframe_data(Frame), zframe_size(Frame), 0, &Error);
		if (!Response) {
			fprintf(stderr, "Error parsing json\n");
			return;
		}
		int Index;
		json_t *Result;
		if (json_unpack(Response, "[io]", &Index, &Result)) {
			fprintf(stderr, "Error invalid json\n");
			return;
		}
		connect_dataset_list(Viewer, Result, Info);
		gtk_combo_box_set_active(Info->DatasetCombo, 0);
		//GIOChannel *Channel = g_io_channel_unix_new(zsock_fd(Socket));
		//g_io_add_watch(Channel, G_IO_IN | G_IO_OUT | G_IO_ERR | G_IO_HUP, (void *)remote_msg_fn, Viewer);
		g_timeout_add(100, G_SOURCE_FUNC(remote_msg_fn), Viewer);
	}
}

typedef void text_dialog_callback_t(const char *Result, viewer_t *Viewer, void *Data);

typedef struct {
	GtkWidget *Dialog, *Entry;
	text_dialog_callback_t *Callback;
	viewer_t *Viewer;
	void *Data;
} text_dialog_info_t;

static void text_input_dialog_destroy(GtkWidget *Dialog, text_dialog_info_t *Info) {
	GC_free(Info);
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

static void text_input_dialog(const char *Title, const char *Initial, viewer_t *Viewer, text_dialog_callback_t *Callback, void *Data) {
	GtkWindow *Dialog = GTK_WINDOW(gtk_window_new(GTK_WINDOW_TOPLEVEL));
	gtk_window_set_title(Dialog, Title);
	gtk_window_set_transient_for(Dialog, GTK_WINDOW(Viewer->MainWindow));
	gtk_window_set_modal(Dialog, TRUE);
	gtk_container_set_border_width(GTK_CONTAINER(Dialog), 6);
	GtkWidget *VBox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
	gtk_container_add(GTK_CONTAINER(Dialog), VBox);

	GtkWidget *Entry = gtk_entry_new();
	if (Initial) gtk_entry_set_text(GTK_ENTRY(Entry), Initial);
	gtk_box_pack_start(GTK_BOX(VBox), Entry, TRUE, FALSE, 6);

	GtkWidget *ButtonBox = gtk_button_box_new(GTK_ORIENTATION_HORIZONTAL);
	gtk_box_pack_start(GTK_BOX(VBox), ButtonBox, FALSE, FALSE, 4);

	GtkWidget *CancelButton = gtk_button_new_with_label("Cancel");
	gtk_button_set_image(GTK_BUTTON(CancelButton), gtk_image_new_from_icon_name("window-close-symbolic", GTK_ICON_SIZE_BUTTON));
	gtk_box_pack_start(GTK_BOX(ButtonBox), CancelButton, FALSE, FALSE, 4);

	GtkWidget *AcceptButton = gtk_button_new_with_label("Accept");
	gtk_button_set_image(GTK_BUTTON(AcceptButton), gtk_image_new_from_icon_name("emblem-ok-symbolic", GTK_ICON_SIZE_BUTTON));
	gtk_box_pack_start(GTK_BOX(ButtonBox), AcceptButton, FALSE, FALSE, 4);

	text_dialog_info_t *Info = (text_dialog_info_t *)GC_malloc(sizeof(text_dialog_info_t));
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

static void dataset_create(viewer_t *Viewer, json_t *Result, connect_info_t *Info) {
	const char *ImagesId = json_string_value(json_object_get(Result, "image"));
	json_t *Values = json_array();
	node_t *Node = Viewer->Nodes;
	int ImagePrefixLength = strlen(Viewer->ImagePrefix);
	for (int I = 0; I < Viewer->NumNodes; ++I) {
		json_array_append(Values, json_string(Node[I].FileName));
	}
	remote_request(Viewer, "column/values/set", json_pack("{ssso}", "column", ImagesId, "values", Values), (void *)column_values_set, NULL);
	gtk_dialog_response(GTK_DIALOG(Info->Dialog), GTK_RESPONSE_CANCEL);
}

static void connect_create_dataset(const char *Result, viewer_t *Viewer, connect_info_t *Info) {
	remote_request(Viewer, "dataset/create", json_pack("{sssi}", "name", Result, "length", Viewer->NumNodes), (void *)dataset_create, Info);
}

static void connect_create_clicked(GtkWidget *Button, connect_info_t *Info) {
	viewer_t *Viewer = Info->Viewer;
	if (Viewer->RemoteSocket && Viewer->NumNodes) {
		text_input_dialog("Remote Dataset Name", NULL, Viewer, (void *)connect_create_dataset, Info);
	}
}

static void connect_images_get(viewer_t *Viewer, json_t *Result, connect_info_t *Info) {
	//printf("connect_images_get(%s)\n", json_dumps(Result, 0));
	if (!json_is_array(Result)) return;
	if (json_array_size(Result) != Viewer->NumNodes) return;
	Viewer->ImagePrefix = Info->ImagePrefix;
	printf("Viewer->ImagePrefix = %s\n", Viewer->ImagePrefix);
	node_t *Nodes = Viewer->Nodes;
	int ImagePrefixLength = strlen(Info->ImagePrefix);
	for (int I = 0; I < Viewer->NumNodes; ++I) {
		json_t *Json = json_array_get(Result, I);
		int Size = json_string_length(Json);
		const char *Text = json_string_value(Json);
		char *FileName, *FilePath;
		FileName = GC_malloc(Size + 1);
		memcpy(FileName, Text, Size);
		FileName[Size] = 0;
		if (ImagePrefixLength) {
			int Length = ImagePrefixLength + Size;
			FilePath = GC_malloc(Length + 1);
			memcpy(stpcpy(FilePath, Info->ImagePrefix), Text, Size);
			FilePath[Length] = 0;
		} else {
			FilePath = FileName;
		}
		Nodes[I].FileName = FileName;
		Nodes[I].File = g_file_new_for_path(FilePath);
		if (FilePath != FileName) GC_free(FilePath);
	}
}

static void connect_dataset_open(viewer_t *Viewer, json_t *Result, connect_info_t *Info) {
	//printf("connect_dataset_open(%s)\n", json_dumps(Result, 0));
	const char *Name, *ImageId;
	int NumNodes;
	json_unpack(Result, "{sssiss}", "name", &Name, "length", &NumNodes, "image", &ImageId);
	Viewer->NumNodes = NumNodes;
	int NumFields = Viewer->NumFields = 0;
	node_t *Nodes = Viewer->Nodes = (node_t *)GC_malloc(NumNodes * sizeof(node_t));
	Viewer->SortedX = (node_t **)GC_malloc(NumNodes * sizeof(node_t *));
	Viewer->SortedY = (node_t **)GC_malloc(NumNodes * sizeof(node_t *));
	Viewer->SortBuffer = (node_t **)GC_malloc(NumNodes * sizeof(node_t *));
	Viewer->NumFiltered = NumNodes;
	memset(Nodes, 0, NumNodes * sizeof(node_t));
	for (int I = 0; I < NumNodes; ++I) {
		Nodes[I].Type = NodeT;
		Nodes[I].Viewer = Viewer;
		Nodes[I].Filtered = 1;
		Viewer->SortedX[I] = &Nodes[I];
		Viewer->SortedY[I] = &Nodes[I];
	}
	field_t **Fields = Viewer->Fields = (field_t **)GC_malloc(NumFields * sizeof(field_t *));
	Viewer->RemoteFields[0] = (stringmap_t)STRINGMAP_INIT;
#ifdef USE_GL
	Viewer->GLVertices = (float *)GC_malloc_atomic(NumNodes * 3 * 3 * sizeof(float));
	Viewer->GLColours = (float *)GC_malloc_atomic(NumNodes * 3 * 4 * sizeof(float));
#endif
	console_printf(Viewer->Console, "Loading rows...\n");

	remote_request(Viewer, "column/values/get", json_pack("{ss}", "column", ImageId), (void *)connect_images_get, Info);

	gtk_list_store_clear(Viewer->FieldsStore);

	clear_viewer_indices(Viewer);

	nodes_iter_t *NodesIter = new(nodes_iter_t);
	NodesIter->Type = NodesT;
	NodesIter->Nodes = Viewer->Nodes;
	NodesIter->NumNodes = Viewer->NumNodes;
	stringmap_insert(Viewer->Globals, "Nodes", (ml_value_t *)NodesIter);

	fields_t *FieldsValue = new(fields_t);
	FieldsValue->Type = FieldsT;
	FieldsValue->Viewer = Viewer;
	stringmap_insert(Viewer->Globals, "Fields", (ml_value_t *)FieldsValue);

	char Title[strlen(Name) + strlen(" - DataViewer") + 1];
	sprintf(Title, "%s - DataViewer", Name);
	gtk_window_set_title(GTK_WINDOW(Viewer->MainWindow), Title);
}

static void connect_clicked(GtkWidget *Button, viewer_t *Viewer) {
	// Connects to a data server and connects a data set or creates one
	connect_info_t *Info = new(connect_info_t);
	GtkWidget *Dialog = Info->Dialog = gtk_dialog_new_with_buttons(
		"Connect to Data Server",
		GTK_WINDOW(Viewer->MainWindow),
		GTK_DIALOG_MODAL,
		"Cancel", GTK_RESPONSE_CANCEL,
		"Open", GTK_RESPONSE_ACCEPT,
		NULL
	);
	GtkListStore *DatasetsStore = gtk_list_store_new(3, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_INT);
	Info->Viewer = Viewer;
	Info->DatasetsStore = DatasetsStore;
	GtkBox *ContentArea = GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(Dialog)));
	gtk_container_set_border_width(GTK_CONTAINER(ContentArea), 6);

	GtkWidget *ServerCombo = gtk_combo_box_text_new_with_entry();
	g_signal_connect(G_OBJECT(ServerCombo), "changed", G_CALLBACK(connect_server_changed), Info);
	GtkWidget *ConnectButton = gtk_button_new_with_label("Connect");
	gtk_button_set_image(GTK_BUTTON(ConnectButton), gtk_image_new_from_icon_name("network-server-symbolic", GTK_ICON_SIZE_BUTTON));
	g_signal_connect(G_OBJECT(ConnectButton), "clicked", G_CALLBACK(connect_connect_clicked), Info);
	GtkWidget *ServerBox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
	gtk_box_pack_start(GTK_BOX(ServerBox), ServerCombo, TRUE, TRUE, 2);
	gtk_box_pack_start(GTK_BOX(ServerBox), ConnectButton, FALSE, FALSE, 2);
	gtk_box_pack_start(ContentArea, ServerBox, FALSE, FALSE, 2);

	GtkWidget *DatasetCombo = gtk_combo_box_new_with_model(GTK_TREE_MODEL(DatasetsStore));
	Info->DatasetCombo = GTK_COMBO_BOX(DatasetCombo);
	g_signal_connect(G_OBJECT(DatasetCombo), "changed", G_CALLBACK(connect_dataset_changed), Info);
	GtkCellRenderer *NameRenderer = gtk_cell_renderer_text_new();
	gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(DatasetCombo), NameRenderer, TRUE);
	gtk_cell_layout_set_attributes(GTK_CELL_LAYOUT(DatasetCombo), NameRenderer, "text", 1, NULL);
	GtkCellRenderer *LengthRenderer = gtk_cell_renderer_text_new();
	gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(DatasetCombo), LengthRenderer, FALSE);
	gtk_cell_layout_set_attributes(GTK_CELL_LAYOUT(DatasetCombo), LengthRenderer, "text", 2, NULL);

	GtkWidget *DatasetBox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
	gtk_box_pack_start(GTK_BOX(DatasetBox), DatasetCombo, TRUE, TRUE, 2);
	gtk_box_pack_start(ContentArea, DatasetBox, FALSE, FALSE, 2);

	GtkWidget *PrefixEntry = gtk_entry_new();
	GtkWidget *PrefixBox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
	gtk_box_pack_start(GTK_BOX(PrefixBox), gtk_label_new("Image Prefix"), FALSE, FALSE, 2);
	gtk_box_pack_start(GTK_BOX(PrefixBox), PrefixEntry, TRUE, TRUE, 2);
	gtk_box_pack_start(ContentArea, PrefixBox, FALSE, FALSE, 2);

	GtkWidget *CreateButton = gtk_button_new_with_label("Create");
	gtk_button_set_image(GTK_BUTTON(CreateButton), gtk_image_new_from_icon_name("document-new-symbolic", GTK_ICON_SIZE_BUTTON));
	g_signal_connect(G_OBJECT(CreateButton), "clicked", G_CALLBACK(connect_create_clicked), Info);
	GtkWidget *CreateBox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
	gtk_box_pack_start(GTK_BOX(CreateBox), CreateButton, TRUE, TRUE, 2);
	gtk_box_pack_start(ContentArea, CreateBox, FALSE, FALSE, 2);

	gtk_widget_show_all(Dialog);
	if (gtk_dialog_run(GTK_DIALOG(Dialog)) == GTK_RESPONSE_ACCEPT) {
		Info->ImagePrefix = GC_strdup(gtk_entry_get_text(GTK_ENTRY(PrefixEntry)));
		printf("ImagePrefix = %s\n", Info->ImagePrefix);
		if (Info->DatasetId) {
			printf("Connecting to dataset %s\n", Info->DatasetId);
			remote_request(Viewer, "dataset/open", json_pack("{ss}", "id", Info->DatasetId), (void *)connect_dataset_open, Info);
		}
	}
	gtk_widget_destroy(Dialog);
}

static void x_field_changed(GtkComboBox *Widget, viewer_t *Viewer) {
	int XIndex = gtk_combo_box_get_active(Widget);
	if (XIndex >= 0) {
		set_viewer_indices(Viewer, XIndex, Viewer->YIndex);
		redraw_viewer_background(Viewer);
		update_preview(Viewer);
		gtk_widget_queue_draw(Viewer->DrawingArea);
	}
}

static void y_field_changed(GtkComboBox *Widget, viewer_t *Viewer) {
	int YIndex = gtk_combo_box_get_active(Widget);
	if (YIndex >= 0) {
		set_viewer_indices(Viewer, Viewer->XIndex, YIndex);
		redraw_viewer_background(Viewer);
		update_preview(Viewer);
		gtk_widget_queue_draw(Viewer->DrawingArea);
	}
}

static void c_field_changed(GtkComboBox *Widget, viewer_t *Viewer) {
	int CIndex = gtk_combo_box_get_active(Widget);
	if (CIndex >= 0) {
		++Viewer->FilterGeneration;
		set_viewer_colour_index(Viewer, CIndex);
		redraw_viewer_background(Viewer);
		update_preview(Viewer);
		gtk_widget_queue_draw(Viewer->DrawingArea);
	}
}

static void edit_field_changed(GtkComboBox *Widget, viewer_t *Viewer) {
	int EditIndex = gtk_combo_box_get_active(GTK_COMBO_BOX(Widget));
	if (EditIndex >= 0) {
		field_t *Field = Viewer->EditField = Viewer->Fields[EditIndex];
		gtk_combo_box_set_model(GTK_COMBO_BOX(Viewer->EditValueComboBox), GTK_TREE_MODEL(Field->EnumStore));
		Viewer->EditValue = 0.0;
		gtk_combo_box_set_active(GTK_COMBO_BOX(Viewer->EditValueComboBox), -1);
	}
}

static void edit_value_changed(GtkComboBox *Widget, viewer_t *Viewer) {
	Viewer->EditValue = gtk_combo_box_get_active(Widget);
}

static void add_field_callback(const char *Name, viewer_t *Viewer, void *Data) {
	Name = GC_strdup(Name);
	int NumFields = Viewer->NumFields + 1;
	field_t **Fields = (field_t **)GC_malloc(NumFields * sizeof(field_t *));
	field_t *Field = (field_t *)GC_malloc(sizeof(field_t) + Viewer->NumNodes * sizeof(double));
	Field->Type = FieldT;
	Field->EnumStore = gtk_list_store_new(2, G_TYPE_STRING, G_TYPE_DOUBLE);

	Field->EnumNames = (const char **)GC_malloc(sizeof(const char *));
	Field->EnumValues = (int *)GC_malloc_atomic(sizeof(int));
	Field->EnumNames[0] = "";
	Field->EnumSize = 1;
	Field->EnumMap = new(stringmap_t);
	Field->Name = Name;
	Field->PreviewColumn = 0;
	Field->PreviewVisible = 1;
	Field->FilterGeneration = 0;
	Field->Sum = Field->Sum2 = 0.0;
	gtk_list_store_insert_with_values(Field->EnumStore, 0, -1, 0, "", 1, 0.0, -1);
	memset(Field->Values, 0, Viewer->NumNodes * sizeof(double));
	for (int I = 0; I < Viewer->NumFields; ++I) Fields[I] = Viewer->Fields[I];
	Fields[Viewer->NumFields] = Field;
	stringmap_insert(Viewer->FieldsByName, Field->Name, Field);
	GC_free(Viewer->Fields);
	Viewer->Fields = Fields;
	Viewer->NumFields = NumFields;
	gtk_list_store_insert_with_values(Viewer->FieldsStore, 0, -1, FIELD_COLUMN_NAME, Name, FIELD_COLUMN_FIELD, Field, FIELD_COLUMN_VISIBLE, TRUE, -1);
	gtk_combo_box_set_active(GTK_COMBO_BOX(Viewer->EditFieldComboBox), NumFields - 1);
}

static void add_field_clicked(GtkWidget *Button, viewer_t *Viewer) {
	text_input_dialog("Add Field", NULL, Viewer, (text_dialog_callback_t *)add_field_callback, 0);
}

static void add_value_callback(const char *Name, viewer_t *Viewer, void *Data) {
	field_t *Field = Viewer->EditField;
	if (!Field || !Field->EnumStore) return;
	Name = GC_strdup(Name);
	double Value = Viewer->EditValue = gtk_tree_model_iter_n_children(GTK_TREE_MODEL(Field->EnumStore), 0);
	gtk_list_store_insert_with_values(Field->EnumStore, 0, -1, 0, Name, 1, Value, -1);
	const char **EnumNames = (const char **)GC_malloc((Value + 1) * sizeof(const char *));
	memcpy(EnumNames, Field->EnumNames, Value * sizeof(const char *));
	EnumNames[(int)Value] = Name;
	GC_free(Field->EnumNames);
	Field->EnumNames = EnumNames;
	GC_free(Field->EnumValues);
	Field->EnumSize = Value + 1;
	Field->EnumValues = (int *)GC_malloc_atomic(Field->EnumSize * sizeof(int));
	double *Ref = new(double);
	*Ref = Value;
	stringmap_insert(Field->EnumMap, Name, Ref);
	gtk_combo_box_set_active(GTK_COMBO_BOX(Viewer->EditValueComboBox), Value);
	if (Field == Viewer->Fields[Viewer->CIndex]) {
		++Viewer->FilterGeneration;
		set_viewer_colour_index(Viewer, Viewer->CIndex);
		redraw_viewer_background(Viewer);
		gtk_widget_queue_draw(Viewer->DrawingArea);
	}
}

static void add_value_clicked(GtkWidget *Button, viewer_t *Viewer) {
	text_input_dialog("Add Value", NULL, Viewer, (text_dialog_callback_t *)add_value_callback, 0);
}

static void filter_operator_equal(int Count, node_t *Node, double *Input, double Value) {
	while (--Count >= 0) {
		if (Input[0] != Value) Node->Filtered = 0;
		++Node;
		++Input;
	}
}

static void filter_operator_not_equal(int Count, node_t *Node, double *Input, double Value) {
	while (--Count >= 0) {
		if (Input[0] == Value) Node->Filtered = 0;
		++Node;
		++Input;
	}
}

static void filter_operator_less(int Count, node_t *Node, double *Input, double Value) {
	while (--Count >= 0) {
		if (Input[0] >= Value) Node->Filtered = 0;
		++Node;
		++Input;
	}
}

static void filter_operator_greater(int Count, node_t *Node, double *Input, double Value) {
	while (--Count >= 0) {
		if (Input[0] <= Value) Node->Filtered = 0;
		++Node;
		++Input;
	}
}

static void filter_operator_less_or_equal(int Count, node_t *Node, double *Input, double Value) {
	while (--Count >= 0) {
		if (Input[0] > Value) Node->Filtered = 0;
		++Node;
		++Input;
	}
}

static void filter_operator_greater_or_equal(int Count, node_t *Node, double *Input, double Value) {
	while (--Count >= 0) {
		if (Input[0] < Value) Node->Filtered = 0;
		++Node;
		++Input;
	}
}

static void viewer_filter_nodes(viewer_t *Viewer) {
	int NumNodes = Viewer->NumNodes;
	node_t *Node = Viewer->Nodes;
	for (int I = NumNodes; --I >= 0;) {
		Node->Filtered = 1;
		++Node;
	}
	for (filter_t *Filter = Viewer->Filters; Filter; Filter = Filter->Next) {
		if (Filter->Operator && Filter->Field) {
			Filter->Operator(Viewer->NumNodes, Viewer->Nodes, Filter->Field->Values, Filter->Value);
		}
	}
	++Viewer->FilterGeneration;
	Node = Viewer->Nodes;
	int NumFiltered = 0;
	for (int I = NumNodes; --I >= 0; ++Node) if (Node->Filtered) ++NumFiltered;
	Viewer->NumFiltered = NumFiltered;
	set_viewer_colour_index(Viewer, Viewer->CIndex);
	update_node_tree(Viewer);
	redraw_viewer_background(Viewer);
	update_preview(Viewer);
	gtk_widget_queue_draw(Viewer->DrawingArea);
}

static void filter_enum_value_changed_ui(GtkComboBox *Widget, filter_t *Filter) {
	Filter->Value = gtk_combo_box_get_active(Widget);
	viewer_filter_nodes(Filter->Viewer);
}

static void filter_enum_entry_changed_ui(GtkEntry *Widget, filter_t *Filter) {
	double *Ref = stringmap_search(Filter->Field->EnumMap, gtk_entry_get_text(GTK_ENTRY(Widget)));
	if (Ref) {
		Filter->Value = *(double *)Ref;
		viewer_filter_nodes(Filter->Viewer);
	}
}

static void filter_real_value_changed_ui(GtkSpinButton *Widget, filter_t *Filter) {
	Filter->Value = gtk_spin_button_get_value(Widget);
	viewer_filter_nodes(Filter->Viewer);
}

static void filter_field_change(filter_t *Filter, field_t *Field) {
	if (Filter->Field) --(Filter->Field->FilterCount);
	++Field->FilterCount;
	if (Filter->ValueWidget) gtk_widget_destroy(Filter->ValueWidget);
	if (Field->EnumStore) {
		if (Field->EnumSize < 100) {
			GtkWidget *ValueComboBox = Filter->ValueWidget = gtk_combo_box_new_with_model(GTK_TREE_MODEL(Field->EnumStore));
			GtkCellRenderer *FieldRenderer = gtk_cell_renderer_text_new();
			gtk_cell_layout_pack_end(GTK_CELL_LAYOUT(ValueComboBox), FieldRenderer, TRUE);
			gtk_cell_layout_add_attribute(GTK_CELL_LAYOUT(ValueComboBox), FieldRenderer, "text", 0);
			g_signal_connect(G_OBJECT(ValueComboBox), "changed", G_CALLBACK(filter_enum_value_changed_ui), Filter);
		} else {
			GtkWidget *ValueEntry = Filter->ValueWidget = gtk_entry_new();
			GtkEntryCompletion *EntryCompletion = gtk_entry_completion_new();
			gtk_entry_completion_set_model(EntryCompletion, GTK_TREE_MODEL(Field->EnumStore));
			gtk_entry_completion_set_text_column(EntryCompletion, 0);
			gtk_entry_set_completion(GTK_ENTRY(ValueEntry), EntryCompletion);
			g_signal_connect(G_OBJECT(ValueEntry), "changed", G_CALLBACK(filter_enum_entry_changed_ui), Filter);
		}
	} else {
		GtkWidget *ValueSpinButton = Filter->ValueWidget = gtk_spin_button_new_with_range(Field->Range.Min, Field->Range.Max, (Field->Range.Max - Field->Range.Min) / 20.0);
		g_signal_connect(G_OBJECT(ValueSpinButton), "value-changed", G_CALLBACK(filter_real_value_changed_ui), Filter);
	}
	gtk_box_pack_start(GTK_BOX(Filter->Widget), Filter->ValueWidget, FALSE, FALSE, 4);
	gtk_widget_show_all(Filter->Widget);
}

static void filter_field_changed_ui(GtkComboBox *Widget, filter_t *Filter) {
	viewer_t *Viewer = Filter->Viewer;
	field_t *Field = Filter->Field = Viewer->Fields[gtk_combo_box_get_active(GTK_COMBO_BOX(Widget))];
	filter_field_change(Filter, Field);
}

static void filter_operator_changed_ui(GtkComboBox *Widget, filter_t *Filter) {
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

static void filter_remove_ui(GtkWidget *Button, filter_t *Filter) {
	if (Filter->Field) --(Filter->Field->FilterCount);
	viewer_t *Viewer = Filter->Viewer;
	filter_t **Slot = &Viewer->Filters;
	while (Slot[0] != Filter) Slot = &Slot[0]->Next;
	Slot[0] = Slot[0]->Next;
	gtk_widget_destroy(Filter->Widget);
	viewer_filter_nodes(Viewer);
}

static filter_t *filter_create(viewer_t *Viewer, field_t *Field, int Operator) {
	filter_t *Filter = new(filter_t);
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

	g_signal_connect(G_OBJECT(RemoveButton), "clicked", G_CALLBACK(filter_remove_ui), Filter);
	g_signal_connect(G_OBJECT(FieldComboBox), "changed", G_CALLBACK(filter_field_changed_ui), Filter);
	g_signal_connect(G_OBJECT(OperatorComboBox), "changed", G_CALLBACK(filter_operator_changed_ui), Filter);

	gtk_box_pack_start(GTK_BOX(Viewer->FiltersBox), FilterBox, FALSE, FALSE, 6);
	gtk_widget_show_all(FilterBox);

	Filter->Next = Viewer->Filters;
	Viewer->Filters = Filter;

	if (Field) {
		++Field->FilterCount;
		for (int Index = 0; Index < Viewer->NumFields; ++Index) {
			if (Viewer->Fields[Index] == Field) {
				gtk_combo_box_set_active(GTK_COMBO_BOX(FieldComboBox), Index);
				break;
			}
		}
	}

	if (Operator >= 0) gtk_combo_box_set_active(GTK_COMBO_BOX(OperatorComboBox), Operator);
	return Filter;
}

static void filter_create_ui(GtkButton *Widget, viewer_t *Viewer) {
	filter_create(Viewer, 0, -1);
}

static ml_value_t *EqualMethod = 0;
static ml_value_t *NotEqualMethod = 0;
static ml_value_t *LessMethod = 0;
static ml_value_t *GreaterMethod = 0;
static ml_value_t *LessOrEqualMethod = 0;
static ml_value_t *GreaterOrEqualMethod = 0;

static ml_value_t *filter_fn(viewer_t *Viewer, int Count, ml_value_t **Args) {
	ML_CHECK_ARG_COUNT(3);
	ML_CHECK_ARG_TYPE(0, FieldT);
	ML_CHECK_ARG_TYPE(1, MLMethodT);
	field_t *Field = (field_t *)Args[0];
	int Operator = -1;
	if (Args[1] == EqualMethod) {
		Operator = 0;
	} else if (Args[1] == NotEqualMethod) {
		Operator = 1;
	} else if (Args[1] == LessMethod) {
		Operator = 2;
	} else if (Args[1] == GreaterMethod) {
		Operator = 3;
	} else if (Args[1] == LessOrEqualMethod) {
		Operator = 4;
	} else if (Args[1] == GreaterOrEqualMethod) {
		Operator = 5;
	} else {
		return ml_error("ValueError", "Unknown operator %s", ml_method_name(Args[1]));
	}
	filter_t *Filter = filter_create(Viewer, Field, Operator);
	ml_value_t *Value = Args[2];
	if (Field->EnumMap) {
		int Index;
		if (Value->Type == MLIntegerT) {
			Index = ml_integer_value(Value);
			if (Index < 0 || Index >= Field->EnumSize) return ml_error("RangeError", "enum index out of range");
		} else if (Value->Type == MLRealT) {
			Index = ml_real_value(Value);
			if (Index < 0 || Index >= Field->EnumSize) return ml_error("RangeError", "enum index out of range");
		} else if (Value->Type == MLStringT) {
			double *Ref2 = stringmap_search(Field->EnumMap, ml_string_value(Value));
			if (Ref2) {
				Index = *(double *)Ref2;
			} else {
				return ml_error("ValueError", "enum name not found");
			}
		} else {
			return ml_error("TypeError", "invalid value for filter");
		}
		//Filter->Value = Index;
		if (GTK_IS_ENTRY(Filter->ValueWidget)) {
			gtk_entry_set_text(GTK_ENTRY(Filter->ValueWidget), Field->EnumNames[Index]);
		} else if (GTK_IS_COMBO_BOX(Filter->ValueWidget)) {
			gtk_combo_box_set_active(GTK_COMBO_BOX(Filter->ValueWidget), Index);
		}
	} else {
		double Value2;
		if (Value->Type == MLIntegerT) {
			Value2 = ml_integer_value(Value);
		} else if (Value->Type == MLRealT) {
			Value2 = ml_real_value(Value);
		} else {
			return ml_error("TypeError", "invalid value for filter");
		}
		//Filter->Value = Value2;
		if (GTK_IS_SPIN_BUTTON(Filter->ValueWidget)) {
			gtk_spin_button_set_value(GTK_SPIN_BUTTON(Filter->ValueWidget), Value2);
		}
	}
	return MLNil;
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

	g_signal_connect(G_OBJECT(CreateButton), "clicked", G_CALLBACK(filter_create_ui), Viewer);
	g_signal_connect(G_OBJECT(Window), "delete-event", G_CALLBACK(gtk_widget_hide_on_delete), Viewer);
}

static void show_filter_window(GtkButton *Widget, viewer_t *Viewer) {
	gtk_widget_show_all(Viewer->FilterWindow);
}

static void image_node_activated(GtkIconView *View, GtkTreePath *Path, viewer_t *Viewer) {
	printf("image_node_activated()\n");
	GtkTreeIter Iter[1];
	gtk_tree_model_get_iter(GTK_TREE_MODEL(Viewer->ImagesStore), Iter, Path);
	gtk_tree_model_get(GTK_TREE_MODEL(Viewer->ImagesStore), Iter, 2, &Viewer->ActiveNode, -1);
	ml_value_t *Result = ml_inline(Viewer->ActivationFn, 1, Viewer->ActiveNode);
	console_log(Viewer->Console, Result);
}

static gboolean image_node_button_press(GtkIconView *Widget, GdkEventButton *Event, viewer_t *Viewer) {
	if (Event->button != 3) return FALSE;
	GtkTreePath *Path;
	if (gtk_icon_view_get_item_at_pos(Widget, Event->x, Event->y, &Path, NULL)) {
		gtk_icon_view_select_path(Widget, Path);
		GtkTreeIter Iter[1];
		gtk_tree_model_get_iter(GTK_TREE_MODEL(Viewer->ImagesStore), Iter, Path);
		gtk_tree_model_get(GTK_TREE_MODEL(Viewer->ImagesStore), Iter, 2, &Viewer->ActiveNode, -1);
		gtk_menu_popup_at_pointer(Viewer->NodeMenu, (GdkEvent *)Event);
	}
	return TRUE;
}

typedef struct node_menu_item_t {
	viewer_t *Viewer;
	ml_value_t *Callback;
} node_menu_item_t;

static void node_menu_activate(GtkMenuItem *MenuItem, node_menu_item_t *NodeMenuItem) {
	viewer_t *Viewer = NodeMenuItem->Viewer;
	ml_value_t *Result = ml_inline(NodeMenuItem->Callback, 1, Viewer->ActiveNode);
	console_log(Viewer->Console, Result);
}

static ml_value_t *node_menu_fn(viewer_t *Viewer, int Count, ml_value_t **Args) {
	ML_CHECK_ARG_COUNT(2);
	ML_CHECK_ARG_TYPE(0, MLStringT);
	ML_CHECK_ARG_TYPE(1, MLFunctionT);
	GtkWidget *MenuItem = gtk_menu_item_new_with_label(ml_string_value(Args[0]));
	node_menu_item_t *NodeMenuItem = new(node_menu_item_t);
	NodeMenuItem->Viewer = Viewer;
	NodeMenuItem->Callback = Args[1];
	g_signal_connect(G_OBJECT(MenuItem), "activate", G_CALLBACK(node_menu_activate), NodeMenuItem);
	gtk_container_add(GTK_CONTAINER(Viewer->NodeMenu), MenuItem);
	gtk_widget_show_all(MenuItem);
	return MLNil;
}

static void view_images_clicked(GtkWidget *Button, viewer_t *Viewer) {
	if (Viewer->ValuesStore) {
		g_object_unref(G_OBJECT(Viewer->ValuesStore));
		Viewer->ValuesStore = 0;
	}
	if (Viewer->ImagesStore) {
		g_object_unref(G_OBJECT(Viewer->ImagesStore));
		Viewer->ImagesStore = 0;
	}
	if (Viewer->PreviewWidget) gtk_container_remove(GTK_CONTAINER(Viewer->MainVPaned), Viewer->PreviewWidget);
	Viewer->ImagesStore = gtk_list_store_new(3, G_TYPE_STRING, GDK_TYPE_PIXBUF, G_TYPE_POINTER);
	GtkWidget *ImagesScrolledArea = Viewer->PreviewWidget = gtk_scrolled_window_new(0, 0);
	GtkWidget *ImagesView = gtk_icon_view_new_with_model(GTK_TREE_MODEL(Viewer->ImagesStore));
	gtk_icon_view_set_selection_mode(GTK_ICON_VIEW(ImagesView), GTK_SELECTION_BROWSE);
	gtk_icon_view_set_text_column(GTK_ICON_VIEW(ImagesView), 0);
	gtk_icon_view_set_pixbuf_column(GTK_ICON_VIEW(ImagesView), 1);
	gtk_icon_view_set_item_width(GTK_ICON_VIEW(ImagesView), 72);
	gtk_container_add(GTK_CONTAINER(ImagesScrolledArea), ImagesView);
	gtk_paned_pack2(GTK_PANED(Viewer->MainVPaned), ImagesScrolledArea, TRUE, TRUE);
	gtk_widget_show_all(ImagesScrolledArea);
	g_signal_connect(G_OBJECT(ImagesView), "item-activated", G_CALLBACK(image_node_activated), Viewer);
	g_signal_connect(G_OBJECT(ImagesView), "button-press-event", G_CALLBACK(image_node_button_press), Viewer);
	update_preview(Viewer);
}

static void view_data_clicked(GtkWidget *Button, viewer_t *Viewer) {
	if (Viewer->ValuesStore) {
		g_object_unref(G_OBJECT(Viewer->ValuesStore));
		Viewer->ValuesStore = 0;
	}
	if (Viewer->ImagesStore) {
		g_object_unref(G_OBJECT(Viewer->ImagesStore));
		Viewer->ImagesStore = 0;
	}
	if (Viewer->PreviewWidget) gtk_container_remove(GTK_CONTAINER(Viewer->MainVPaned), Viewer->PreviewWidget);
	int NumFields = Viewer->NumFields;
	int NumTypes = 1 + NumFields * 2;
	GType Types[NumTypes];
	Types[0] = G_TYPE_STRING;
	for (int I = 0; I < NumFields; ++I) {
		field_t *Field = Viewer->Fields[I];
		Types[2 * I + 1] = Field->EnumStore ? G_TYPE_STRING : G_TYPE_DOUBLE;
		Types[2 * I + 2] = GDK_TYPE_RGBA;
	}

	Viewer->ValuesStore = gtk_list_store_newv(NumTypes, Types);
	GtkWidget *ValuesScrolledArea = Viewer->PreviewWidget = gtk_scrolled_window_new(0, 0);
	GtkWidget *ValuesView = gtk_tree_view_new_with_model(GTK_TREE_MODEL(Viewer->ValuesStore));

	GtkTreeViewColumn *Column = gtk_tree_view_column_new();
	gtk_tree_view_column_set_title(Column, "Image");
	gtk_tree_view_column_set_reorderable(Column, TRUE);
	GtkCellRenderer *Renderer = gtk_cell_renderer_text_new();
	gtk_tree_view_column_pack_start(Column, Renderer, TRUE);
	gtk_tree_view_column_add_attribute(Column, Renderer, "text", 0);
	gtk_tree_view_append_column(GTK_TREE_VIEW(ValuesView), Column);

	for (int I = 0; I < NumFields; ++I) {
		field_t *Field = Viewer->Fields[I];
		GtkTreeViewColumn *Column = gtk_tree_view_column_new();
		gtk_tree_view_column_set_title(Column, Field->Name);
		gtk_tree_view_column_set_reorderable(Column, TRUE);
		GtkCellRenderer *Renderer = gtk_cell_renderer_text_new();
		gtk_tree_view_column_pack_start(Column, Renderer, TRUE);
		gtk_tree_view_column_add_attribute(Column, Renderer, "text", 2 * I + 1);
		gtk_tree_view_column_add_attribute(Column, Renderer, "background-rgba", 2 * I + 2);
		gtk_tree_view_append_column(GTK_TREE_VIEW(ValuesView), Column);
		gtk_tree_view_column_set_visible(Column, Field->PreviewVisible);
		Field->PreviewColumn = Column;
	}

	gtk_container_add(GTK_CONTAINER(ValuesScrolledArea), ValuesView);
	gtk_paned_pack2(GTK_PANED(Viewer->MainVPaned), ValuesScrolledArea, TRUE, TRUE);
	gtk_widget_show_all(ValuesScrolledArea);

	update_preview(Viewer);
}

static void view_console_clicked(GtkWidget *Button, viewer_t *Viewer) {
	console_show(Viewer->Console, GTK_WINDOW(Viewer->MainWindow));
}

static void preview_column_visible_toggled(GtkCellRendererToggle *Renderer, char *Path, viewer_t *Viewer) {
	GtkTreeIter Iter[1];
	gtk_tree_model_get_iter_from_string(GTK_TREE_MODEL(Viewer->FieldsStore), Iter, Path);
	field_t *Field;
	gtk_tree_model_get(GTK_TREE_MODEL(Viewer->FieldsStore), Iter, FIELD_COLUMN_FIELD, &Field, -1);
	Field->PreviewVisible = !Field->PreviewVisible;
	if (Viewer->ValuesStore) gtk_tree_view_column_set_visible(Field->PreviewColumn, Field->PreviewVisible);
	gtk_list_store_set(Viewer->FieldsStore, Iter, FIELD_COLUMN_VISIBLE, Field->PreviewVisible, -1);
}

static void columns_remote_list(viewer_t *Viewer, json_t *Result, GtkListStore *FieldsModel) {
	if (!json_is_object(Result)) return;
	json_t *ColumnsInfo = json_object_get(Result, "columns");
	if (!ColumnsInfo) return;
	const char *Id;
	json_t *Info;
	json_object_foreach(ColumnsInfo, Id, Info) {
		if (!stringmap_search(Viewer->RemoteFields, Id)) {
			const char *Name, *Type;
			json_unpack(Info, "{ssss}", "name", &Name, "type", &Type);
			gtk_list_store_insert_with_values(FieldsModel, 0, -1, 0, Id, 1, Name, 2, Type, -1);
		}
	}
}

static void column_values_get(viewer_t *Viewer, json_t *Result, field_t *Field) {
	double *Values = Field->Values;
	if (Field->EnumMap) {
		for (int Index = 0; Index < Viewer->NumNodes; ++Index) {
			double Value = 0.0;
			const char *Text = json_string_value(json_array_get(Result, Index));
			if (Text && Text[0]) {
				double *Ref = stringmap_search(Field->EnumMap, Text);
				if (Ref) {
					Value = *(double *)Ref;
				} else {
					Ref = new(double);
					stringmap_insert(Field->EnumMap, GC_strdup(Text), Ref);
					*(double *)Ref = Value = Field->EnumMap->Size;
				}
			}
			Values[Index] = Value;
		}
		int EnumSize = Field->EnumSize = Field->EnumMap->Size + 1;
		const char **EnumNames = (const char **)GC_malloc(EnumSize * sizeof(const char *));
		EnumNames[0] = "";
		stringmap_foreach(Field->EnumMap, EnumNames, (void *)set_enum_name_fn);
		Field->EnumNames = EnumNames;
		GtkListStore *EnumStore = Field->EnumStore = gtk_list_store_new(2, G_TYPE_STRING, G_TYPE_DOUBLE);
		for (int J = 0; J < EnumSize; ++J) {
			gtk_list_store_insert_with_values(Field->EnumStore, 0, -1, 0, Field->EnumNames[J], 1, (double)(J + 1), -1);
		}
		Field->EnumValues = (int *)GC_malloc_atomic(EnumSize * sizeof(int));
		Field->Range.Min = 0.0;
		Field->Range.Max = EnumSize;
	} else {
		double Min = INFINITY, Max = -INFINITY;
		double Sum = 0.0, Sum2 = 0.0;
		for (int Index = 0; Index < Viewer->NumNodes; ++Index) {
			double Value = Values[Index] = json_number_value(json_array_get(Result, Index));
			if (Value < Min) Min = Value;
			if (Value > Max) Max = Value;
			Sum += Value;
			Sum2 += Value * Value;
		}
		Field->Range.Min = Min;
		Field->Range.Max = Max;
		Field->Sum = Sum;
		Field->Sum = Sum2;
		double Mean = Sum / Viewer->NumNodes;
		Field->SD = sqrt((Sum2 / Viewer->NumNodes) - Mean * Mean);
	}
}

typedef struct columns_list_t {
	viewer_t *Viewer;
	GtkComboBox *FieldsCombo;
	GtkListStore *FieldsModel;
} columns_list_t;

static void column_open(viewer_t *Viewer, json_t *Result, field_t *Field) {
	remote_request(Viewer, "column/values/get", json_pack("{ss}", "column", Field->RemoteId), (void *)column_values_get, Field);
}

static void column_open_clicked(GtkWidget *Button, columns_list_t *Info) {
	GtkTreeIter Iter[1];
	if (gtk_combo_box_get_active_iter(Info->FieldsCombo, Iter)) {
		viewer_t *Viewer = Info->Viewer;
		const char *RemoteId, *Name, *Type;
		gtk_tree_model_get(GTK_TREE_MODEL(Info->FieldsModel), Iter, 0, &RemoteId, 1, &Name, 2, &Type, -1);
		gtk_list_store_remove(Info->FieldsModel, Iter);
		int NumFields = Viewer->NumFields + 1;
		field_t **Fields = (field_t **)GC_malloc(NumFields * sizeof(field_t *));
		field_t *Field = (field_t *)GC_malloc(sizeof(field_t) + Viewer->NumNodes * sizeof(double));
		Field->Type = FieldT;
		if (!strcmp(Type, "string")) {
			Field->EnumStore = gtk_list_store_new(2, G_TYPE_STRING, G_TYPE_DOUBLE);
			Field->EnumNames = (const char **)GC_malloc(sizeof(const char *));
			Field->EnumValues = (int *)GC_malloc_atomic(sizeof(int));
			Field->EnumNames[0] = "";
			Field->EnumSize = 1;
			Field->EnumMap = new(stringmap_t);
			gtk_list_store_insert_with_values(Field->EnumStore, 0, -1, 0, "", 1, 0.0, -1);
		}
		Field->Name = GC_strdup(Name);
		Field->PreviewColumn = 0;
		Field->PreviewVisible = 1;
		Field->FilterGeneration = 0;
		Field->Sum = Field->Sum2 = 0.0;
		Field->RemoteId = RemoteId;
		Field->RemoteGenerations = (json_int_t *)GC_malloc_atomic(Viewer->NumNodes * sizeof(json_int_t));
		memset(Field->RemoteGenerations, 0, Viewer->NumNodes * sizeof(json_int_t));
		memset(Field->Values, 0, Viewer->NumNodes * sizeof(double));
		for (int I = 0; I < Viewer->NumFields; ++I) Fields[I] = Viewer->Fields[I];
		Fields[Viewer->NumFields] = Field;
		stringmap_insert(Viewer->FieldsByName, Field->Name, Field);
		GC_free(Viewer->Fields);
		Viewer->Fields = Fields;
		Viewer->NumFields = NumFields;
		gtk_list_store_insert_with_values(Viewer->FieldsStore, 0, -1,
			FIELD_COLUMN_NAME, Name,
			FIELD_COLUMN_FIELD, Field,
			FIELD_COLUMN_VISIBLE, TRUE,
			FIELD_COLUMN_CONNECTED, TRUE,
			FIELD_COLUMN_REMOTE, Name,
			-1
		);
		stringmap_insert(Viewer->RemoteFields, RemoteId, Field);
		remote_request(Viewer, "column/open", json_pack("{ss}", "column", RemoteId), (void *)column_open, Field);
	}
}

typedef struct column_create_remote_t {
	const char *Path;
	const char *Name;
	field_t *Field;
} column_create_remote_t;

static void column_create(viewer_t *Viewer, json_t *Result, column_create_remote_t *Info) {
	field_t *Field = Info->Field;
	Field->RemoteId = json_string_value(Result);
	json_t *Values = json_array();
	if (Field->EnumNames) {
		for (int I = 0; I < Viewer->NumNodes; ++I) {
			json_array_append(Values, json_string(Field->EnumNames[(int)Field->Values[I]]));
		}
	} else {
		for (int I = 0; I < Viewer->NumNodes; ++I) {
			json_array_append(Values, json_real(Field->Values[I]));
		}
	}
	json_t *Request = json_pack("{ssso}", "column", Field->RemoteId, "values", Values);
	remote_request(Viewer, "column/values/set", Request, (void *)column_values_set, Field);
	GtkTreeIter Iter[1];
	gtk_tree_model_get_iter_from_string(GTK_TREE_MODEL(Viewer->FieldsStore), Iter, Info->Path);
	gtk_list_store_set(Viewer->FieldsStore, Iter, FIELD_COLUMN_CONNECTED, TRUE, FIELD_COLUMN_REMOTE, Info->Name, -1);
}

static void column_create_remote(const char *Result, viewer_t *Viewer, column_create_remote_t *Info) {
	json_t *Request = json_pack("{ssss}",
		"name", Result,
		"type", Info->Field->EnumMap ? "string" : "real"
	);
	Info->Name = GC_strdup(Result);
	remote_request(Viewer, "column/create", Request, (void *)column_create, Info);
}

static void column_connected_toggled(GtkCellRendererToggle *Renderer, char *Path, viewer_t *Viewer) {
	GtkTreeIter Iter[1];
	gtk_tree_model_get_iter_from_string(GTK_TREE_MODEL(Viewer->FieldsStore), Iter, Path);
	const char *Name;
	field_t *Field;
	gtk_tree_model_get(GTK_TREE_MODEL(Viewer->FieldsStore), Iter, FIELD_COLUMN_FIELD, &Field, FIELD_COLUMN_NAME, &Name, -1);
	if (Field->RemoteId) {
		Field->RemoteId = 0;
		gtk_list_store_set(Viewer->FieldsStore, Iter, FIELD_COLUMN_CONNECTED, FALSE, FIELD_COLUMN_REMOTE, "", -1);
	} else {
		column_create_remote_t *Info = new(column_create_remote_t);
		Info->Path = GC_strdup(Path);
		Info->Field = Field;
		text_input_dialog("Remote Column Name", Name, Viewer, (text_dialog_callback_t *)column_create_remote, Info);
	}
}

typedef struct columns_load_t {
	viewer_t *Viewer;
	GtkListStore *ColumnsStore;
	field_t **Fields;
	int NumFields, FieldIndex, RowIndex;
} columns_load_t;

static void columns_header_field_fn(void *Data, size_t Length, columns_load_t *Info) {
	char *Name = GC_malloc_atomic(Length + 1);
	memcpy(Name, Data, Length);
	Name[Length] = 0;
	gtk_list_store_insert_with_values(Info->ColumnsStore, 0, -1, 0, Name, 1, FALSE, -1);
	++Info->NumFields;
}

static void columns_header_record_fn(int Delim, columns_load_t *Info) {

}

static void columns_data_field_fn(void *Data, size_t Length, columns_load_t *Info) {

}

static void columns_data_record_fn(int Delim, columns_load_t *Info) {
	Info->FieldIndex = 0;
	++Info->RowIndex;
}

static void column_selected_toggled(GtkCellRendererToggle *Renderer, char *Path, columns_load_t *Info) {
	GtkTreeIter Iter[1];
	gtk_tree_model_get_iter_from_string(GTK_TREE_MODEL(Info->ColumnsStore), Iter, Path);
	gboolean Selected;
	gtk_tree_model_get(GTK_TREE_MODEL(Info->ColumnsStore), Iter, 1, &Selected, -1);
	gtk_list_store_set(Info->ColumnsStore, Iter, 1, !Selected, -1);
}

static void columns_load_clicked(GtkWidget *Button, viewer_t *Viewer) {
	GtkWidget *FileChooser = gtk_file_chooser_dialog_new(
		"Select CSV file",
		GTK_WINDOW(Viewer->MainWindow),
		GTK_FILE_CHOOSER_ACTION_OPEN,
		"Cancel", GTK_RESPONSE_CANCEL,
		"Select", GTK_RESPONSE_ACCEPT,
		NULL
	);
	if (gtk_dialog_run(GTK_DIALOG(FileChooser)) != GTK_RESPONSE_ACCEPT) {
		gtk_widget_destroy(FileChooser);
		return;
	}
	char *FileName = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(FileChooser));
	gtk_widget_destroy(FileChooser);
	FILE *File = fopen(FileName, "r");
	if (!File) return;
	columns_load_t Info[1];
	Info->Viewer = Viewer;
	Info->ColumnsStore = gtk_list_store_new(2, G_TYPE_STRING, G_TYPE_BOOLEAN);
	Info->NumFields = 0;
	struct csv_parser Parser[1];
	csv_init(Parser, 0);
	char Buffer[129];
	if (!fgets(Buffer, 129, File)) return;
	int Length = strlen(Buffer);
	while (Length == 128) {
		csv_parse(Parser, Buffer, Length, (void *)columns_header_field_fn, (void *)columns_header_record_fn, Info);
		if (!fgets(Buffer, 129, File)) return;
		int Length = strlen(Buffer);
	}
	csv_parse(Parser, Buffer, Length, (void *)columns_header_field_fn, (void *)columns_header_record_fn, Info);
	GtkWidget *Dialog = gtk_dialog_new_with_buttons(
		"Select Columns",
		GTK_WINDOW(Viewer->MainWindow),
		GTK_DIALOG_MODAL,
		"Open", GTK_RESPONSE_ACCEPT,
		NULL
	);
	GtkWidget *ContentArea = gtk_dialog_get_content_area(GTK_DIALOG(Dialog));
	gtk_container_set_border_width(GTK_CONTAINER(ContentArea), 6);
	GtkWidget *FieldsScrolled = gtk_scrolled_window_new(0, 0);
	GtkWidget *FieldsView = gtk_tree_view_new_with_model(GTK_TREE_MODEL(Info->ColumnsStore));
	GtkCellRenderer *SelectRenderer = gtk_cell_renderer_toggle_new();
	g_signal_connect(G_OBJECT(SelectRenderer), "toggled", G_CALLBACK(column_selected_toggled), Viewer);
	gtk_tree_view_append_column(GTK_TREE_VIEW(FieldsView), gtk_tree_view_column_new_with_attributes("Name", gtk_cell_renderer_text_new(), "text", 0, NULL));
	gtk_tree_view_append_column(GTK_TREE_VIEW(FieldsView), gtk_tree_view_column_new_with_attributes("Visible", SelectRenderer, "active", 1, NULL));
	gtk_container_add(GTK_CONTAINER(FieldsScrolled), FieldsView);
	gtk_box_pack_start(GTK_BOX(ContentArea), FieldsScrolled, TRUE, TRUE, 2);
	gtk_widget_show_all(Dialog);
	gtk_dialog_run(GTK_DIALOG(Dialog));
	gtk_widget_destroy(Dialog);
	field_t **Fields = anew(field_t *, Info->NumFields);
	GtkTreeIter Iter[1];
	int I = 0;
	if (gtk_tree_model_get_iter_first(GTK_TREE_MODEL(Info->ColumnsStore), Iter)) do {
		gboolean Selected;
		gtk_tree_model_get(GTK_TREE_MODEL(Info->ColumnsStore), Iter, 1, &Selected, -1);
		if (Selected) {
			field_t *Field = Fields[I] = (field_t *)GC_malloc(sizeof(field_t) + Viewer->NumNodes * sizeof(double));
			Field->Type = FieldT;
			Field->EnumMap = 0;
			Field->Range.Min = INFINITY;
			Field->Range.Max = -INFINITY;
			Field->PreviewColumn = 0;
			Field->PreviewVisible = 1;
			Field->FilterGeneration = 0;
			Field->Sum = Field->Sum2 = 0.0;
			Fields[I] = Field;
		}
	} while (gtk_tree_model_iter_next(GTK_TREE_MODEL(Info->ColumnsStore), Iter));

}

static void show_columns_clicked(GtkWidget *Button, viewer_t *Viewer) {
	GtkWidget *Window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	GtkWidget *VBox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
	gtk_window_set_transient_for(GTK_WINDOW(Window), GTK_WINDOW(Viewer->MainWindow));
	gtk_container_set_border_width(GTK_CONTAINER(VBox), 6);
	GtkWidget *FieldsScrolled = gtk_scrolled_window_new(0, 0);
	GtkWidget *FieldsView = gtk_tree_view_new_with_model(GTK_TREE_MODEL(Viewer->FieldsStore));
	GtkCellRenderer *VisibleRenderer = gtk_cell_renderer_toggle_new();
	gtk_tree_view_append_column(GTK_TREE_VIEW(FieldsView), gtk_tree_view_column_new_with_attributes("Name", gtk_cell_renderer_text_new(), "text", FIELD_COLUMN_NAME, NULL));
	gtk_tree_view_append_column(GTK_TREE_VIEW(FieldsView), gtk_tree_view_column_new_with_attributes("Visible", VisibleRenderer, "active", FIELD_COLUMN_VISIBLE, NULL));
	gtk_container_add(GTK_CONTAINER(FieldsScrolled), FieldsView);
	gtk_box_pack_start(GTK_BOX(VBox), FieldsScrolled, TRUE, TRUE, 2);

	GtkWidget *LoadColumnsButton = gtk_button_new_with_label("Load Columns");
	gtk_button_set_image(GTK_BUTTON(LoadColumnsButton), gtk_image_new_from_icon_name("document-open-symbolic", GTK_ICON_SIZE_BUTTON));
	g_object_set(LoadColumnsButton, "always-show-image", TRUE, NULL);
	g_signal_connect(G_OBJECT(LoadColumnsButton), "clicked", G_CALLBACK(columns_load_clicked), Viewer);
	GtkWidget *LoadColumnsBox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
	gtk_box_pack_start(GTK_BOX(LoadColumnsBox), LoadColumnsButton, TRUE, TRUE, 2);
	gtk_box_pack_start(GTK_BOX(VBox), LoadColumnsBox, FALSE, FALSE, 2);

	if (Viewer->RemoteSocket) {
		columns_list_t *Info = new(columns_list_t);
		Info->Viewer = Viewer;
		GtkWidget *RemoteBox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
		GtkListStore *FieldsModel = Info->FieldsModel = gtk_list_store_new(3, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
		GtkWidget *FieldsCombo = gtk_combo_box_new_with_model(GTK_TREE_MODEL(FieldsModel));
		Info->FieldsCombo = GTK_COMBO_BOX(FieldsCombo);
		GtkCellRenderer *NameRenderer = gtk_cell_renderer_text_new();
		gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(FieldsCombo), NameRenderer, TRUE);
		gtk_cell_layout_set_attributes(GTK_CELL_LAYOUT(FieldsCombo), NameRenderer, "text", 1, NULL);
		GtkCellRenderer *TypeRenderer = gtk_cell_renderer_text_new();
		gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(FieldsCombo), TypeRenderer, TRUE);
		gtk_cell_layout_set_attributes(GTK_CELL_LAYOUT(FieldsCombo), TypeRenderer, "text", 2, NULL);
		GtkWidget *OpenButton = gtk_button_new_with_label("Open");
		gtk_button_set_image(GTK_BUTTON(OpenButton), gtk_image_new_from_icon_name("network-server-symbolic", GTK_ICON_SIZE_BUTTON));
		g_object_set(OpenButton, "always-show-image", TRUE, NULL);
		g_signal_connect(G_OBJECT(OpenButton), "clicked", G_CALLBACK(column_open_clicked), Info);
		gtk_box_pack_start(GTK_BOX(RemoteBox), FieldsCombo, TRUE, TRUE, 2);
		gtk_box_pack_start(GTK_BOX(RemoteBox), OpenButton, FALSE, FALSE, 2);
		gtk_box_pack_start(GTK_BOX(VBox), RemoteBox, FALSE, FALSE, 2);
		remote_request(Viewer, "dataset/info", json_null(), (void *)columns_remote_list, FieldsModel);

		GtkCellRenderer *ConnectedRenderer = gtk_cell_renderer_toggle_new();
		gtk_tree_view_append_column(GTK_TREE_VIEW(FieldsView), gtk_tree_view_column_new_with_attributes("Connected", ConnectedRenderer, "active", FIELD_COLUMN_CONNECTED, NULL));
		gtk_tree_view_append_column(GTK_TREE_VIEW(FieldsView), gtk_tree_view_column_new_with_attributes("Remote", gtk_cell_renderer_text_new(), "text", FIELD_COLUMN_REMOTE, NULL));
		g_signal_connect(G_OBJECT(ConnectedRenderer), "toggled", G_CALLBACK(column_connected_toggled), Viewer);
	}

	gtk_container_add(GTK_CONTAINER(Window), VBox);
	g_signal_connect(G_OBJECT(VisibleRenderer), "toggled", G_CALLBACK(preview_column_visible_toggled), Viewer);
	gtk_widget_show_all(Window);
}

static void viewer_save_file(GtkWidget *Button, viewer_t *Viewer) {
	GtkWidget *FileChooser = gtk_file_chooser_dialog_new(
		"Save as CSV file",
		GTK_WINDOW(Viewer->MainWindow),
		GTK_FILE_CHOOSER_ACTION_SAVE,
		"Cancel", GTK_RESPONSE_CANCEL,
		"Save", GTK_RESPONSE_ACCEPT,
		NULL
	);
	if (gtk_dialog_run(GTK_DIALOG(FileChooser)) != GTK_RESPONSE_ACCEPT) {
		gtk_widget_destroy(FileChooser);
		return;
	}
	char *FileName = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(FileChooser));
	gtk_widget_destroy(FileChooser);

	GtkProgressBar *ProgressBar = GTK_PROGRESS_BAR(gtk_progress_bar_new());
	gtk_progress_bar_set_show_text(ProgressBar, TRUE);
	GtkWidget *InfoContainerArea = gtk_info_bar_get_content_area(GTK_INFO_BAR(Viewer->InfoBar));
	gtk_container_add(GTK_CONTAINER(InfoContainerArea), GTK_WIDGET(ProgressBar));
	gtk_info_bar_set_message_type(GTK_INFO_BAR(Viewer->InfoBar), GTK_MESSAGE_INFO);
	gtk_widget_show(GTK_WIDGET(ProgressBar));
	gtk_widget_show(Viewer->InfoBar);
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
	char ProgressText[32];
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
		if (J % 10000 == 0) {
			sprintf(ProgressText, "%d / %d rows", J, NumNodes);
			gtk_progress_bar_set_text(ProgressBar, ProgressText);
			gtk_progress_bar_set_fraction(ProgressBar, (double)J / NumNodes);
			while (gtk_events_pending()) gtk_main_iteration();
		}
	}
	fclose(File);
	sprintf(ProgressText, "%d / %d rows", NumNodes, NumNodes);
	gtk_progress_bar_set_text(ProgressBar, ProgressText);
	gtk_progress_bar_set_fraction(ProgressBar, 1.0);
	while (gtk_events_pending()) gtk_main_iteration();
	gtk_widget_destroy(GTK_WIDGET(ProgressBar));
	gtk_widget_hide(Viewer->InfoBar);
}

typedef struct {
	viewer_t *Viewer;
	GtkProgressBar *ProgressBar;
	field_t **Fields;
	node_t *Nodes;
	const char *ImagePrefix;
	int NumNodes, Index, Row, ImagePrefixLength;
} csv_node_loader_t;

static void load_nodes_field_callback(void *Text, size_t Size, csv_node_loader_t *Loader) {
	if (!Loader->Row) {
		if (Loader->Index) {
			char *FieldName = GC_malloc(Size + 1);
			memcpy(FieldName, Text, Size);
			FieldName[Size] = 0;
			Loader->Fields[Loader->Index - 1]->Name = FieldName;
		}
	} else {
		if (!Loader->Index) {
			char *FileName, *FilePath;
			FileName = GC_malloc(Size + 1);
			memcpy(FileName, Text, Size);
			FileName[Size] = 0;
			if (Loader->ImagePrefixLength) {
				int Length = Loader->ImagePrefixLength + Size;
				FilePath = GC_malloc(Length + 1);
				memcpy(stpcpy(FilePath, Loader->ImagePrefix), Text, Size);
				FilePath[Length] = 0;
			} else {
				FilePath = FileName;
			}
			Loader->Nodes[Loader->Row - 1].FileName = FileName;
			Loader->Nodes[Loader->Row - 1].File = g_file_new_for_path(FilePath);
			if (FilePath != FileName) GC_free(FilePath);
		} else {
			int Index = Loader->Index - 1;
			field_t *Field = Loader->Fields[Index];
			double Value;
			insert:
			if (Field->EnumMap) {
				if (Size) {
					double *Ref = stringmap_search(Field->EnumMap, Text);
					if (Ref) {
						Value = *(double *)Ref;
					} else {
						Ref = new(double);
						stringmap_insert(Field->EnumMap, GC_strdup(Text), Ref);
						*(double *)Ref = Value = Field->EnumMap->Size;
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
					Field->EnumMap = new(stringmap_t);
					goto insert;
				}
			}
			Field->Values[Loader->Row - 1] = Value;
			if (Field->Range.Min > Value) Field->Range.Min = Value;
			if (Field->Range.Max < Value) Field->Range.Max = Value;
			Field->Sum += Value;
			Field->Sum2 += Value * Value;
		}
	}
	++Loader->Index;
}

static void load_nodes_row_callback(int Char, csv_node_loader_t *Loader) {
	Loader->Index = 0;
	++Loader->Row;
	if (Loader->Row % 10000 == 0) {
		char ProgressText[32];
		sprintf(ProgressText, "%d / %d rows", Loader->Row, Loader->NumNodes);
		gtk_progress_bar_set_text(Loader->ProgressBar, ProgressText);
		gtk_progress_bar_set_fraction(Loader->ProgressBar, (double)Loader->Row / (double)Loader->NumNodes);
		while (gtk_events_pending()) gtk_main_iteration();
	}
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

static void viewer_load_file(viewer_t *Viewer, const char *CsvFileName, const char *ImagePrefix) {
	char *Path = g_path_get_dirname(CsvFileName);
	chdir(Path);
	g_free(Path);

	Viewer->ImagePrefix = ImagePrefix;
	int ImagePrefixLength = ImagePrefix ? strlen(ImagePrefix) : 0;
	csv_node_loader_t Loader[1] = {{Viewer, 0, 0, 0, ImagePrefix, 0, 0, 0, ImagePrefixLength}};
	char Buffer[4096];
	struct csv_parser Parser[1];

	console_printf(Viewer->Console, "Counting rows...\n");
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
	node_t *Nodes = Viewer->Nodes = (node_t *)GC_malloc(NumNodes * sizeof(node_t));
	Viewer->SortedX = (node_t **)GC_malloc(NumNodes * sizeof(node_t *));
	Viewer->SortedY = (node_t **)GC_malloc(NumNodes * sizeof(node_t *));
	Viewer->SortBuffer = (node_t **)GC_malloc(NumNodes * sizeof(node_t *));
	Viewer->NumFiltered = NumNodes;
	memset(Nodes, 0, NumNodes * sizeof(node_t));
	for (int I = 0; I < NumNodes; ++I) {
		Nodes[I].Type = NodeT;
		Nodes[I].Viewer = Viewer;
		Nodes[I].Filtered = 1;
		Viewer->SortedX[I] = &Nodes[I];
		Viewer->SortedY[I] = &Nodes[I];
	}
	field_t **Fields = Viewer->Fields = (field_t **)GC_malloc(NumFields * sizeof(field_t *));
	for (int I = 0; I < NumFields; ++I) {
		field_t *Field = Fields[I] = (field_t *)GC_malloc(sizeof(field_t) + NumNodes * sizeof(double));
		Field->Type = FieldT;
		Field->EnumMap = 0;
		Field->Range.Min = INFINITY;
		Field->Range.Max = -INFINITY;
		Field->PreviewColumn = 0;
		Field->PreviewVisible = 1;
		Field->FilterGeneration = 0;
		Field->Sum = Field->Sum2 = 0.0;
		Fields[I] = Field;
	}
	Viewer->RemoteFields[0] = (stringmap_t)STRINGMAP_INIT;
#ifdef USE_GL
	Viewer->GLVertices = (float *)GC_malloc_atomic(NumNodes * 3 * 3 * sizeof(float));
	Viewer->GLColours = (float *)GC_malloc_atomic(NumNodes * 3 * 4 * sizeof(float));
#endif
	console_printf(Viewer->Console, "Loading rows...\n");
	File = fopen(CsvFileName, "r");
	Loader->Nodes = Nodes;
	Loader->Fields = Fields;
	Loader->Row = Loader->Index = 0;
	Loader->NumNodes = NumNodes;
	Loader->ProgressBar = GTK_PROGRESS_BAR(gtk_progress_bar_new());
	gtk_progress_bar_set_show_text(Loader->ProgressBar, TRUE);
	GtkWidget *InfoContainerArea = gtk_info_bar_get_content_area(GTK_INFO_BAR(Viewer->InfoBar));
	gtk_container_add(GTK_CONTAINER(InfoContainerArea), GTK_WIDGET(Loader->ProgressBar));
	gtk_info_bar_set_message_type(GTK_INFO_BAR(Viewer->InfoBar), GTK_MESSAGE_INFO);
	gtk_widget_show(GTK_WIDGET(Loader->ProgressBar));
	gtk_widget_show(Viewer->InfoBar);
	csv_init(Parser, CSV_APPEND_NULL);
	Count = fread(Buffer, 1, 4096, File);
	while (Count > 0) {
		csv_parse(Parser, Buffer, Count, (void *)load_nodes_field_callback, (void *)load_nodes_row_callback, Loader);
		Count = fread(Buffer, 1, 4096, File);
	}
	fclose(File);
	csv_fini(Parser, (void *)load_nodes_field_callback, (void *)load_nodes_row_callback, Loader);
	csv_free(Parser);
	char ProgressText[32];
	sprintf(ProgressText, "%d / %d rows", NumNodes, NumNodes);
	gtk_progress_bar_set_text(Loader->ProgressBar, ProgressText);
	gtk_progress_bar_set_fraction(Loader->ProgressBar, 1.0);
	while (gtk_events_pending()) gtk_main_iteration();

	gtk_list_store_clear(Viewer->FieldsStore);
	for (int I = 0; I < NumFields; ++I) {
		field_t *Field = Fields[I];
		gtk_list_store_insert_with_values(Viewer->FieldsStore, 0, -1, FIELD_COLUMN_NAME, Field->Name, FIELD_COLUMN_FIELD, Field, FIELD_COLUMN_VISIBLE, TRUE, -1);
		stringmap_insert(Viewer->FieldsByName, Field->Name, Field);
		if (Field->EnumMap) {
			int EnumSize = Field->EnumSize = Field->EnumMap->Size + 1;
			const char **EnumNames = (const char **)GC_malloc(EnumSize * sizeof(const char *));
			EnumNames[0] = "";
			stringmap_foreach(Field->EnumMap, EnumNames, (void *)set_enum_name_fn);
			Field->EnumNames = EnumNames;
			GtkListStore *EnumStore = Field->EnumStore = gtk_list_store_new(2, G_TYPE_STRING, G_TYPE_DOUBLE);
			for (int J = 0; J < EnumSize; ++J) {
				gtk_list_store_insert_with_values(Field->EnumStore, 0, -1, 0, Field->EnumNames[J], 1, (double)(J + 1), -1);
			}
			Field->EnumValues = (int *)GC_malloc_atomic(EnumSize * sizeof(int));
			Field->Range.Min = 0.0;
			Field->Range.Max = EnumSize;
		} else {
			double Mean = Field->Sum / Viewer->NumNodes;
			Field->SD = sqrt((Field->Sum2 / Viewer->NumNodes) - Mean * Mean);
		}
	}

	if (NumFields >= 2) {
		gtk_combo_box_set_active(GTK_COMBO_BOX(Viewer->XComboBox), 0);
		gtk_combo_box_set_active(GTK_COMBO_BOX(Viewer->YComboBox), 1);
		gtk_combo_box_set_active(GTK_COMBO_BOX(Viewer->CComboBox), Viewer->NumFields - 1);
	} else {
		clear_viewer_indices(Viewer);
	}

	nodes_iter_t *NodesIter = new(nodes_iter_t);
	NodesIter->Type = NodesT;
	NodesIter->Nodes = Viewer->Nodes;
	NodesIter->NumNodes = Viewer->NumNodes;
	stringmap_insert(Viewer->Globals, "Nodes", (ml_value_t *)NodesIter);

	fields_t *FieldsValue = new(fields_t);
	FieldsValue->Type = FieldsT;
	FieldsValue->Viewer = Viewer;
	stringmap_insert(Viewer->Globals, "Fields", (ml_value_t *)FieldsValue);

	gtk_widget_destroy(GTK_WIDGET(Loader->ProgressBar));
	gtk_widget_hide(Viewer->InfoBar);
	redraw_viewer_background(Viewer);
	gtk_widget_queue_draw(Viewer->DrawingArea);
	const char *Basename = g_path_get_basename(CsvFileName);
	char Title[strlen(Basename) + strlen(" - DataViewer") + 1];
	sprintf(Title, "%s - DataViewer", Basename);
	gtk_window_set_title(GTK_WINDOW(Viewer->MainWindow), Title);
}

static void prefix_directory_set(GtkFileChooser *Widget, GtkEntry *Entry) {
	gtk_entry_set_text(Entry, gtk_file_chooser_get_filename(Widget));
}

static void viewer_open_file(GtkWidget *Button, viewer_t *Viewer) {
	GtkWidget *FileChooser = gtk_file_chooser_dialog_new(
		"Open a CSV file",
		GTK_WINDOW(Viewer->MainWindow),
		GTK_FILE_CHOOSER_ACTION_OPEN,
		"Cancel", GTK_RESPONSE_CANCEL,
		"Open", GTK_RESPONSE_ACCEPT,
		NULL
	);
	if (gtk_dialog_run(GTK_DIALOG(FileChooser)) != GTK_RESPONSE_ACCEPT) {
		gtk_widget_destroy(FileChooser);
		return;
	}
	const char *FileName = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(FileChooser));
	const char *CurrentFolder = gtk_file_chooser_get_current_folder(GTK_FILE_CHOOSER(FileChooser));
	gtk_widget_destroy(FileChooser);
	const char *ImagePrefix = 0;

	GtkWidget *Dialog = gtk_dialog_new_with_buttons(
		"Set load options",
		GTK_WINDOW(Viewer->MainWindow),
		GTK_DIALOG_MODAL,
		"Cancel", GTK_RESPONSE_CANCEL,
		"Open", GTK_RESPONSE_ACCEPT,
		NULL
	);
	GtkWidget *LoadOptions = gtk_grid_new();
	gtk_grid_set_row_spacing(GTK_GRID(LoadOptions), 6);
	gtk_grid_set_column_spacing(GTK_GRID(LoadOptions), 6);
	GtkWidget *PrefixEntry = gtk_entry_new();
	gtk_grid_attach(GTK_GRID(LoadOptions), gtk_label_new("Image Prefix"), 0, 0, 1, 1);
	gtk_grid_attach(GTK_GRID(LoadOptions), PrefixEntry, 1, 0, 1, 1);

	GtkWidget *PrefixChooser = gtk_file_chooser_button_new("Image prefix directory", GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER);
	gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(PrefixChooser), CurrentFolder);
	g_signal_connect(G_OBJECT(PrefixChooser), "file-set", G_CALLBACK(prefix_directory_set), PrefixEntry);
	gtk_grid_attach(GTK_GRID(LoadOptions), PrefixChooser, 2, 0, 1, 1);

	GtkContainer *ContentArea = GTK_CONTAINER(gtk_dialog_get_content_area(GTK_DIALOG(Dialog)));
	gtk_container_set_border_width(ContentArea, 6);
	gtk_box_pack_start(GTK_BOX(ContentArea), LoadOptions, TRUE, TRUE, 6);
	gtk_widget_show_all(LoadOptions);
	if (gtk_dialog_run(GTK_DIALOG(Dialog)) == GTK_RESPONSE_ACCEPT) {
		ImagePrefix = GC_strdup(gtk_entry_get_text(GTK_ENTRY(PrefixEntry)));
	}
	gtk_widget_destroy(Dialog);
	while (gtk_events_pending()) gtk_main_iteration();
	viewer_load_file(Viewer, FileName, ImagePrefix);
}

static GtkWidget *create_viewer_action_bar(viewer_t *Viewer) {
	GtkActionBar *ActionBar = GTK_ACTION_BAR(gtk_action_bar_new());

	GtkWidget *ConnectButton = gtk_button_new_with_label("Connect");
	gtk_button_set_image(GTK_BUTTON(ConnectButton), gtk_image_new_from_icon_name("network-server-symbolic", GTK_ICON_SIZE_BUTTON));
	g_object_set(ConnectButton, "always-show-image", TRUE, NULL);
	gtk_action_bar_pack_start(ActionBar, ConnectButton);
	g_signal_connect(G_OBJECT(ConnectButton), "clicked", G_CALLBACK(connect_clicked), Viewer);

	GtkWidget *OpenCsvButton = gtk_button_new_with_label("Open");
	gtk_button_set_image(GTK_BUTTON(OpenCsvButton), gtk_image_new_from_icon_name("document-open-symbolic", GTK_ICON_SIZE_BUTTON));
	g_object_set(OpenCsvButton, "always-show-image", TRUE, NULL);
	gtk_action_bar_pack_start(ActionBar, OpenCsvButton);
	g_signal_connect(G_OBJECT(OpenCsvButton), "clicked", G_CALLBACK(viewer_open_file), Viewer);

	GtkWidget *SaveCsvButton = gtk_button_new_with_label("Save");
	gtk_button_set_image(GTK_BUTTON(SaveCsvButton), gtk_image_new_from_icon_name("document-save-as-symbolic", GTK_ICON_SIZE_BUTTON));
	g_object_set(SaveCsvButton, "always-show-image", TRUE, NULL);
	gtk_action_bar_pack_start(ActionBar, SaveCsvButton);
	g_signal_connect(G_OBJECT(SaveCsvButton), "clicked", G_CALLBACK(viewer_save_file), Viewer);

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

	gtk_action_bar_pack_start(ActionBar, gtk_label_new("X"));
	gtk_action_bar_pack_start(ActionBar, XComboBox);
	gtk_action_bar_pack_start(ActionBar, gtk_label_new("Y"));
	gtk_action_bar_pack_start(ActionBar, YComboBox);
	gtk_action_bar_pack_start(ActionBar, gtk_label_new("Colour"));
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
	gtk_button_set_image(GTK_BUTTON(AddFieldButton), gtk_image_new_from_icon_name("list-add-symbolic", GTK_ICON_SIZE_BUTTON));
	g_object_set(AddFieldButton, "always-show-image", TRUE, NULL);

	GtkWidget *AddValueButton = gtk_button_new_with_label("Add Value");
	gtk_button_set_image(GTK_BUTTON(AddValueButton), gtk_image_new_from_icon_name("list-add-symbolic", GTK_ICON_SIZE_BUTTON));
	g_object_set(AddValueButton, "always-show-image", TRUE, NULL);

	gtk_action_bar_pack_start(ActionBar, gtk_label_new("Edit"));
	gtk_action_bar_pack_start(ActionBar, EditFieldComboBox);
	gtk_action_bar_pack_start(ActionBar, AddFieldButton);

	gtk_action_bar_pack_start(ActionBar, gtk_label_new("Value"));
	gtk_action_bar_pack_start(ActionBar, EditValueComboBox);
	gtk_action_bar_pack_start(ActionBar, AddValueButton);

	g_signal_connect(G_OBJECT(AddFieldButton), "clicked", G_CALLBACK(add_field_clicked), Viewer);
	g_signal_connect(G_OBJECT(AddValueButton), "clicked", G_CALLBACK(add_value_clicked), Viewer);

	create_filter_window(Viewer);

	GtkWidget *FilterButton = gtk_button_new_with_label("Filter");
	gtk_button_set_image(GTK_BUTTON(FilterButton), gtk_image_new_from_icon_name("edit-find-replace-symbolic", GTK_ICON_SIZE_BUTTON));
	g_object_set(FilterButton, "always-show-image", TRUE, NULL);
	gtk_action_bar_pack_start(ActionBar, FilterButton);

	g_signal_connect(G_OBJECT(FilterButton), "clicked", G_CALLBACK(show_filter_window), Viewer);

	GtkWidget *ViewImagesButton = gtk_button_new_with_label("Images");
	gtk_button_set_image(GTK_BUTTON(ViewImagesButton), gtk_image_new_from_icon_name("view-grid-symbolic", GTK_ICON_SIZE_BUTTON));
	g_object_set(ViewImagesButton, "always-show-image", TRUE, NULL);

	GtkWidget *ViewDataButton = gtk_button_new_with_label("Data");
	gtk_button_set_image(GTK_BUTTON(ViewDataButton), gtk_image_new_from_icon_name("view-list-symbolic", GTK_ICON_SIZE_BUTTON));
	g_object_set(ViewDataButton, "always-show-image", TRUE, NULL);

	GtkWidget *ViewConsoleButton = gtk_button_new_with_label("Console");
	gtk_button_set_image(GTK_BUTTON(ViewConsoleButton), gtk_image_new_from_icon_name("utilities-terminal-symbolic", GTK_ICON_SIZE_BUTTON));
	g_object_set(ViewConsoleButton, "always-show-image", TRUE, NULL);

	GtkWidget *ShowColumnsButton = gtk_button_new_with_label("Columns");
	gtk_button_set_image(GTK_BUTTON(ShowColumnsButton), gtk_image_new_from_icon_name("object-select-symbolic", GTK_ICON_SIZE_BUTTON));
	g_object_set(ShowColumnsButton, "always-show-image", TRUE, NULL);

	gtk_action_bar_pack_start(ActionBar, ViewImagesButton);
	gtk_action_bar_pack_start(ActionBar, ViewDataButton);
	gtk_action_bar_pack_start(ActionBar, ShowColumnsButton);
	gtk_action_bar_pack_start(ActionBar, ViewConsoleButton);

	g_signal_connect(G_OBJECT(ViewImagesButton), "clicked", G_CALLBACK(view_images_clicked), Viewer);
	g_signal_connect(G_OBJECT(ViewDataButton), "clicked", G_CALLBACK(view_data_clicked), Viewer);
	g_signal_connect(G_OBJECT(ShowColumnsButton), "clicked", G_CALLBACK(show_columns_clicked), Viewer);
	g_signal_connect(G_OBJECT(ViewConsoleButton), "clicked", G_CALLBACK(view_console_clicked), Viewer);

	GtkWidget *NumVisibleLabel = gtk_label_new(0);
	gtk_action_bar_pack_end(ActionBar, NumVisibleLabel);
	Viewer->NumVisibleLabel = GTK_LABEL(NumVisibleLabel);

	return GTK_WIDGET(ActionBar);
}

#ifdef __MINGW32__
#define WIFEXITED(Status) (((Status) & 0x7f) == 0)
#define WEXITSTATUS(Status) (((Status) & 0xff00) >> 8)
#endif

static ml_value_t *shell_fn(void *Data, int Count, ml_value_t **Args) {
	ml_value_t *AppendMethod = ml_method("append");
	ML_CHECK_ARG_COUNT(1);
	ml_stringbuffer_t Buffer[1] = {ML_STRINGBUFFER_INIT};
	for (int I = 0; I < Count; ++I) {
		ml_value_t *Result = ml_inline(AppendMethod, 2, Buffer, Args[I]);
		if (Result->Type == MLErrorT) return Result;
		if (Result != MLNil) ml_stringbuffer_add(Buffer, " ", 1);
	}
	const char *Command = ml_stringbuffer_get(Buffer);
	//clock_t Start = clock();
	FILE *File = popen(Command, "r");
	char Chars[ML_STRINGBUFFER_NODE_SIZE];
	while (!feof(File)) {
		ssize_t Size = fread(Chars, 1, ML_STRINGBUFFER_NODE_SIZE, File);
		if (Size == -1) break;
		if (Size > 0) ml_stringbuffer_add(Buffer, Chars, Size);
	}
	int Result = pclose(File);
	//clock_t End = clock();
	if (WIFEXITED(Result)) {
		if (WEXITSTATUS(Result) != 0) {
			return ml_error("ExecuteError", "process returned non-zero exit code");
		} else {
			return ml_stringbuffer_get_string(Buffer);
		}
	} else {
		return ml_error("ExecuteError", "process exited abnormally");
	}
}

static ml_value_t *execute_fn(viewer_t *Viewer, int Count, ml_value_t **Args) {
	ml_value_t *AppendMethod = ml_method("append");
	ML_CHECK_ARG_COUNT(1);
	ml_stringbuffer_t Buffer[1] = {ML_STRINGBUFFER_INIT};
	for (int I = 0; I < Count; ++I) {
		ml_value_t *Result = ml_inline(AppendMethod, 2, Buffer, Args[I]);
		if (Result->Type == MLErrorT) return Result;
		if (Result != MLNil) ml_stringbuffer_add(Buffer, " ", 1);
	}
	const char *Command = ml_stringbuffer_get(Buffer);
	//clock_t Start = clock();
	FILE *File = popen(Command, "r");
	char Chars[ML_STRINGBUFFER_NODE_SIZE];
	while (!feof(File)) {
		ssize_t Size = fread(Chars, 1, ML_STRINGBUFFER_NODE_SIZE, File);
		if (Size == -1) break;
		console_append(Viewer->Console, Chars, Size);
	}
	int Result = pclose(File);
	//clock_t End = clock();
	if (WIFEXITED(Result)) {
		if (WEXITSTATUS(Result) != 0) {
			return ml_error("ExecuteError", "process returned non-zero exit code");
		} else {
			return MLNil;
		}
	} else {
		return ml_error("ExecuteError", "process exited abnormally");
	}
}

static ml_value_t *getenv_fn(void *Data, int Count, ml_value_t **Args) {
	ML_CHECK_ARG_COUNT(1);
	ML_CHECK_ARG_TYPE(0, MLStringT);
	const char *Key = ml_string_value(Args[0]);
	const char *Value = getenv(Key);
	if (Value) {
		return ml_string(Value, -1);
	} else {
		return MLNil;
	}
}

static ml_value_t *setenv_fn(viewer_t *Viewer, int Count, ml_value_t **Args) {
	ML_CHECK_ARG_COUNT(2);
	ML_CHECK_ARG_TYPE(0, MLStringT);
	ML_CHECK_ARG_TYPE(1, MLStringT);
	const char *Key = ml_string_value(Args[0]);
	const char *Value = ml_string_value(Args[1]);
	setenv(Key, Value, 1);
	return MLNil;
}

static json_t *ml_to_json(ml_value_t *Value);

static int ml_map_to_json(ml_value_t *Key, ml_value_t *Value, json_t *Object) {
	if (Key->Type == MLStringT) {
		json_object_set(Object, ml_string_value(Key), ml_to_json(Value));
	}
	return 0;
}

static json_t *ml_to_json(ml_value_t *Value) {
	if (Value == MLNil) return json_null();
	if (Value->Type == MLMethodT) {
		if (!strcmp(ml_method_name(Value), "true")) return json_true();
		if (!strcmp(ml_method_name(Value), "false")) return json_false();
		return json_null();
	}
	if (Value->Type == MLIntegerT) return json_integer(ml_integer_value(Value));
	if (Value->Type == MLRealT) return json_real(ml_real_value(Value));
	if (Value->Type == MLStringT) return json_string(ml_string_value(Value));
	if (Value->Type == MLListT) {
		json_t *Array = json_array();
		ML_LIST_FOREACH(Value, Iter) {
			json_array_append(Array, ml_to_json(Iter->Value));
		}
		return Array;
	}
	if (Value->Type == MLMapT) {
		json_t *Object = json_object();
		ml_map_foreach(Value, Object, (void *)ml_map_to_json);
		return Object;
	}
	return json_null();
}

static ml_value_t *json_to_ml(json_t *Json) {
	switch (json_typeof(Json)) {
	case JSON_NULL: return MLNil;
	case JSON_TRUE: return ml_method("true");
	case JSON_FALSE: return ml_method("false");
	case JSON_INTEGER: return ml_integer(json_integer_value(Json));
	case JSON_REAL: return ml_real(json_real_value(Json));
	case JSON_STRING: return ml_string(json_string_value(Json), json_string_length(Json));
	case JSON_ARRAY: {
		ml_value_t *List = ml_list();
		json_t *Value;
		int I;
		json_array_foreach(Json, I, Value) ml_list_append(List, json_to_ml(Value));
		return List;
	}
	case JSON_OBJECT: {
		ml_value_t *Map = ml_map();
		const char *Key;
		json_t *Value;
		json_object_foreach(Json, Key, Value) ml_map_insert(Map, ml_string(Key, -1), json_to_ml(Value));
		return Map;
	}
	default: return MLNil;
	}
}

static void remote_ml_callback(viewer_t *Viewer, json_t *Result, ml_value_t *Callback) {
	ml_inline(Callback, 1, json_to_ml(Result));
}

static ml_value_t *connect_fn(viewer_t *Viewer, int Count, ml_value_t **Args) {
	ML_CHECK_ARG_COUNT(1);
	ML_CHECK_ARG_TYPE(0, MLStringT);
	if (Viewer->RemoteSocket) zsock_destroy(&Viewer->RemoteSocket);
	zsock_t *Socket = Viewer->RemoteSocket = zsock_new_dealer(ml_string_value(Args[0]));
	zmsg_t *Msg = zmsg_new();
	json_t *Request = json_pack("[isn]", ++Viewer->LastCallbackIndex, "dataset/list");
	zmsg_addstr(Msg, json_dumps(Request, JSON_COMPACT));
	zmsg_send(&Msg, Socket);
	Msg = zmsg_recv(Socket);
	zframe_t *Frame = zmsg_pop(Msg);
	json_error_t Error;
	json_t *Response = json_loadb(zframe_data(Frame), zframe_size(Frame), 0, &Error);
	if (!Response) {
		fprintf(stderr, "Error parsing json\n");
		return MLNil;
	}
	int Index;
	json_t *Result;
	if (json_unpack(Response, "[io]", &Index, &Result)) {
		fprintf(stderr, "Error invalid json\n");
		return MLNil;
	}
	g_timeout_add(100, G_SOURCE_FUNC(remote_msg_fn), Viewer);
	return json_to_ml(Result);
}

static ml_value_t *remote_fn(viewer_t *Viewer, int Count, ml_value_t **Args) {
	ML_CHECK_ARG_COUNT(3);
	ML_CHECK_ARG_TYPE(0, MLStringT);
	if (!Viewer->RemoteSocket) return ml_error("RemoteError", "no remote connection");
	json_t *Request = ml_to_json(Args[1]);
	remote_request(Viewer, ml_string_value(Args[0]), Request, (void *)remote_ml_callback, Args[2]);
	return MLNil;
}

static ml_value_t *random_fn(viewer_t *Viewer, int Count, ml_value_t **Args) {
	return ml_real((double)rand() / RAND_MAX);
}

static void dataset_close(viewer_t *Viewer, json_t *Result, void *Data) {
	zsock_destroy(&Viewer->RemoteSocket);
	gtk_main_quit();
}

static void destroy_viewer(GtkWidget *Widget, viewer_t *Viewer) {
	if (Viewer->RemoteSocket) {
		remote_request(Viewer, "dataset/close", json_null(), dataset_close, NULL);
	} else {
		gtk_main_quit();
	}
}

static viewer_t *create_viewer(int Argc, char *Argv[]) {
	viewer_t *Viewer = new(viewer_t);
#ifdef USE_GL
	Viewer->GLVertices = 0;
	Viewer->GLColours = 0;
	Viewer->GLReady = 0;
#else
	Viewer->CachedBackground = 0;
#endif
	Viewer->EditField = 0;
	Viewer->Filters = 0;
	Viewer->FilterGeneration = 1;
	Viewer->LoadGeneration = 0;
	Viewer->LoadCache = (node_t **)GC_malloc(MAX_CACHED_IMAGES * sizeof(node_t *));
	Viewer->LoadCacheIndex = 0;
	Viewer->ShowBox = 0;
	Viewer->RedrawBackground = 0;
	Viewer->FieldsStore = gtk_list_store_new(5, G_TYPE_STRING, G_TYPE_POINTER, G_TYPE_BOOLEAN, G_TYPE_BOOLEAN, G_TYPE_STRING);
	Viewer->ActivationFn = ml_function(Viewer->Console, (void *)console_print);
	for (int I = 0; I < 10; ++I) Viewer->HotkeyFns[I] = Viewer->ActivationFn;
	Viewer->PreviewWidget = 0;

	ml_types_init(Viewer->Globals);
	ml_object_init(Viewer->Globals);
	ml_iterfns_init(Viewer->Globals);
	ml_file_init(Viewer->Globals);
	ml_csv_init(Viewer->Globals);
	ml_gir_init(Viewer->Globals);

	Viewer->Console = console_new((ml_getter_t)viewer_global_get, Viewer);

	stringmap_insert(Viewer->Globals, "activate", ml_reference(&Viewer->ActivationFn));
	stringmap_insert(Viewer->Globals, "hotkey0", ml_reference(&Viewer->HotkeyFns[0]));
	stringmap_insert(Viewer->Globals, "hotkey1", ml_reference(&Viewer->HotkeyFns[1]));
	stringmap_insert(Viewer->Globals, "hotkey2", ml_reference(&Viewer->HotkeyFns[2]));
	stringmap_insert(Viewer->Globals, "hotkey3", ml_reference(&Viewer->HotkeyFns[3]));
	stringmap_insert(Viewer->Globals, "hotkey4", ml_reference(&Viewer->HotkeyFns[4]));
	stringmap_insert(Viewer->Globals, "hotkey5", ml_reference(&Viewer->HotkeyFns[5]));
	stringmap_insert(Viewer->Globals, "hotkey6", ml_reference(&Viewer->HotkeyFns[6]));
	stringmap_insert(Viewer->Globals, "hotkey7", ml_reference(&Viewer->HotkeyFns[7]));
	stringmap_insert(Viewer->Globals, "hotkey8", ml_reference(&Viewer->HotkeyFns[8]));
	stringmap_insert(Viewer->Globals, "hotkey9", ml_reference(&Viewer->HotkeyFns[9]));
	stringmap_insert(Viewer->Globals, "menu", ml_function(Viewer, (void *)node_menu_fn));
	stringmap_insert(Viewer->Globals, "print", ml_function(Viewer->Console, (void *)console_print));
	stringmap_insert(Viewer->Globals, "clipboard", ml_function(Viewer, (void *)clipboard_fn));
	stringmap_insert(Viewer->Globals, "execute", ml_function(Viewer, (void *)execute_fn));
	stringmap_insert(Viewer->Globals, "shell", ml_function(Viewer, (void *)shell_fn));
	stringmap_insert(Viewer->Globals, "getenv", ml_function(Viewer, (void *)getenv_fn));
	stringmap_insert(Viewer->Globals, "setenv", ml_function(Viewer, (void *)setenv_fn));
	stringmap_insert(Viewer->Globals, "open", ml_function(Viewer, ml_file_open));
	stringmap_insert(Viewer->Globals, "filter", ml_function(Viewer, (void *)filter_fn));
	stringmap_insert(Viewer->Globals, "connect", ml_function(Viewer, (void *)connect_fn));
	stringmap_insert(Viewer->Globals, "remote", ml_function(Viewer, (void *)remote_fn));
	stringmap_insert(Viewer->Globals, "random", ml_function(Viewer, (void *)random_fn));

	GtkWidget *MainWindow = Viewer->MainWindow = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	gtk_window_set_title(GTK_WINDOW(Viewer->MainWindow), "DataViewer");
#ifdef USE_GL
	Viewer->DrawingArea = gtk_gl_area_new();
#else
	Viewer->DrawingArea = gtk_drawing_area_new();
#endif

	Viewer->Clipboard = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
	GtkWidget *MainVBox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_container_add(GTK_CONTAINER(MainWindow), MainVBox);

	GtkWidget *ActionBar = create_viewer_action_bar(Viewer);
	gtk_box_pack_start(GTK_BOX(MainVBox), ActionBar, FALSE, FALSE, 0);

	Viewer->InfoBar = gtk_info_bar_new();
	gtk_widget_set_no_show_all(Viewer->InfoBar, TRUE);
	gtk_box_pack_start(GTK_BOX(MainVBox), Viewer->InfoBar, FALSE, FALSE, 0);

	Viewer->MainVPaned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
	gtk_box_pack_start(GTK_BOX(MainVBox), Viewer->MainVPaned, TRUE, TRUE, 0);

	gtk_paned_pack1(GTK_PANED(Viewer->MainVPaned), Viewer->DrawingArea, TRUE, TRUE);

	cairo_surface_t *CursorSurface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, BOX_SIZE, BOX_SIZE);
	cairo_t *CursorCairo = cairo_create(CursorSurface);
	cairo_new_path(CursorCairo);
	cairo_rectangle(CursorCairo, 0.0, 0.0, BOX_SIZE, BOX_SIZE);
	cairo_set_source_rgb(CursorCairo, 0.5, 0.5, 1.0);
	cairo_stroke_preserve(CursorCairo);
	cairo_set_source_rgba(CursorCairo, 0.5, 0.5, 1.0, 0.5);
	cairo_fill(CursorCairo);
	Viewer->Cursor = gdk_cursor_new_from_surface(gdk_display_get_default(), CursorSurface, BOX_SIZE / 2.0, BOX_SIZE / 2.0);

	Viewer->ImagesStore = 0;
	Viewer->ValuesStore = 0;
	view_images_clicked(NULL, Viewer);

	gtk_widget_add_events(Viewer->DrawingArea, GDK_SCROLL_MASK);
	gtk_widget_add_events(Viewer->DrawingArea, GDK_POINTER_MOTION_MASK);
	gtk_widget_add_events(Viewer->DrawingArea, GDK_BUTTON_PRESS_MASK);
	gtk_widget_add_events(Viewer->DrawingArea, GDK_BUTTON_RELEASE_MASK);
	//gtk_widget_add_events(Viewer->DrawingArea, GDK_KEY_PRESS);
#ifdef USE_GL
	g_signal_connect(G_OBJECT(Viewer->DrawingArea), "render", G_CALLBACK(render_viewer), Viewer);
	g_signal_connect(G_OBJECT(Viewer->DrawingArea), "realize", G_CALLBACK(realize_viewer_gl), Viewer);
#else
	g_signal_connect(G_OBJECT(Viewer->DrawingArea), "draw", G_CALLBACK(redraw_viewer), Viewer);
	g_signal_connect(G_OBJECT(Viewer->DrawingArea), "realize", G_CALLBACK(realize_viewer), Viewer);
#endif
	g_signal_connect(G_OBJECT(Viewer->DrawingArea), "size-allocate", G_CALLBACK(resize_viewer), Viewer);
	g_signal_connect(G_OBJECT(Viewer->DrawingArea), "scroll-event", G_CALLBACK(scroll_viewer), Viewer);
	g_signal_connect(G_OBJECT(Viewer->DrawingArea), "button-press-event", G_CALLBACK(button_press_viewer), Viewer);
	g_signal_connect(G_OBJECT(Viewer->DrawingArea), "button-release-event", G_CALLBACK(button_release_viewer), Viewer);
	g_signal_connect(G_OBJECT(Viewer->DrawingArea), "motion-notify-event", G_CALLBACK(motion_notify_viewer), Viewer);
	g_signal_connect(G_OBJECT(Viewer->MainWindow), "key-press-event", G_CALLBACK(key_press_viewer), Viewer);
	g_signal_connect(G_OBJECT(Viewer->MainWindow), "destroy", G_CALLBACK(destroy_viewer), Viewer);

	Viewer->NodeMenu = GTK_MENU(gtk_menu_new());

	const char *CsvFileName = 0;
	const char *ImagePrefix = 0;
	for (int I = 1; I < Argc; ++I) {
		if (Argv[I][0] == '-') {
			if (Argv[I][1] == 'p') {
				if (++I >= Argc) {
					puts("Missing image path");
					exit(1);
				}
				ImagePrefix = Argv[I];
			}
		} else {
			CsvFileName = Argv[I];
		}
	}

	gtk_window_resize(GTK_WINDOW(Viewer->MainWindow), 640, 480);
	gtk_paned_set_position(GTK_PANED(Viewer->MainVPaned), 320);
	gtk_widget_show_all(Viewer->MainWindow);

	if (CsvFileName) {
		while (gtk_events_pending()) gtk_main_iteration();
		viewer_load_file(Viewer, CsvFileName, ImagePrefix);
	}

	return Viewer;
}

int main(int Argc, char *Argv[]) {
	GC_INIT();
	gtk_init(&Argc, &Argv);
	ml_init();
	EqualMethod = ml_method("=");
	NotEqualMethod = ml_method("!=");
	LessMethod = ml_method("<");
	GreaterMethod = ml_method(">");
	LessOrEqualMethod = ml_method("<=");
	GreaterOrEqualMethod = ml_method(">=");
	NodeT = ml_type(MLAnyT, "node");
	ml_method_by_name("[]", 0, node_field_string_fn, NodeT, MLStringT, NULL);
	ml_method_by_name("[]", 0, node_field_field_fn, NodeT, FieldT, NULL);
	ml_method_by_name("image", 0, node_image_fn, NodeT, NULL);
	ml_method_by_name("string", 0, node_image_fn, NodeT, NULL);
	FieldT = ml_type(MLAnyT, "field");
	ml_method_by_name("string", 0, field_name_fn, FieldT, NULL);
	ml_method_by_name("[]", 0, field_enum_value_fn, FieldT, MLStringT, NULL);
	ml_method_by_name("[]", 0, field_enum_name_fn, FieldT, MLIntegerT, NULL);
	ml_method_by_name("size", 0, field_enum_size_fn, FieldT, NULL);
	ml_method_by_name("min", 0, field_range_min_fn, FieldT, NULL);
	ml_method_by_name("max", 0, field_range_max_fn, FieldT, NULL);
	ml_method_by_name("[]", 0, fields_get_by_name, FieldsT, MLStringT, NULL);
	ml_method_by_name("[]", 0, fields_get_by_index, FieldsT, MLIntegerT, NULL);
	ml_method_by_name("new", 0, fields_new_field, FieldsT, MLStringT, MLStringT, NULL);

	ml_typed_fn_set(NodesT, ml_iterate, nodes_iterate);
	ml_typed_fn_set(NodesIterT, ml_iter_next, nodes_iter_next);
	ml_typed_fn_set(NodesIterT, ml_iter_value, nodes_iter_current);

	stringmap_insert(EventHandlers, "column/values/set", column_values_set_event);
	viewer_t *Viewer = create_viewer(Argc, Argv);
	gtk_main();
	return 0;
}
