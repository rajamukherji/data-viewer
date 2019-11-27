#include "ml_gir.h"
#include "minilang.h"
#include "ml_macros.h"
#include <gc.h>
#include <girepository.h>
#include <gtk/gtk.h>
#include <stdio.h>

static ml_value_t *MLTrue = 0, *MLFalse = 0;

typedef struct typelib_t {
	const ml_type_t *Type;
	GITypelib *Handle;
	const char *Namespace;
} typelib_t;

static ml_type_t *TypelibT = 0;

typedef struct typelib_iter_t {
	const ml_type_t *Type;
	GITypelib *Handle;
	const char *Namespace;
	GIBaseInfo *Current;
	int Index, Total;
} typelib_iter_t;

static ml_value_t *baseinfo_to_value(GIBaseInfo *Info);

static ml_value_t *typelib_iter_current(typelib_iter_t *Iter) {
	return baseinfo_to_value(Iter->Current);
}

static ml_value_t *typelib_iter_next(typelib_iter_t *Iter) {
	if (++Iter->Index >= Iter->Total) return MLNil;
	Iter->Current = g_irepository_get_info(NULL, Iter->Namespace, Iter->Index);
	return (ml_value_t *)Iter;
}

static ml_value_t *typelib_iter_key(typelib_iter_t *Iter) {
	return ml_string(g_base_info_get_name(Iter->Current), -1);
}

ml_type_t TypelibIterT[1] = {{
	MLTypeT,
	MLIteratableT, "typelib-iter",
	ml_default_hash,
	ml_default_call,
	ml_default_deref,
	ml_default_assign,
	ml_default_iterate,
	(void *)typelib_iter_current,
	(void *)typelib_iter_next,
	(void *)typelib_iter_key,
}};

static ml_value_t *ml_gir_require(void *Data, int Count, ml_value_t **Args) {
	ML_CHECK_ARG_COUNT(1);
	ML_CHECK_ARG_TYPE(0, MLStringT);
	typelib_t *Typelib = new(typelib_t);
	Typelib->Type = TypelibT;
	GError *Error = 0;
	Typelib->Namespace = ml_string_value(Args[0]);
	Typelib->Handle = g_irepository_require(NULL, Typelib->Namespace, NULL, 0, &Error);
	if (!Typelib->Handle) return ml_error("GirError", Error->message);
	return (ml_value_t *)Typelib;
}

typedef struct object_t {
	ml_type_t Base;
	GIObjectInfo *Info;
} object_t;

typedef struct object_instance_t {
	const object_t *Type;
	void *Handle;
} object_instance_t;

static ml_type_t *ObjectT = 0;
static ml_type_t *ObjectInstanceT = 0;
static object_instance_t *ObjectInstanceNil;

static ml_type_t *object_info_lookup(GIObjectInfo *Info);
static ml_type_t *struct_info_lookup(GIStructInfo *Info);
static ml_type_t *enum_info_lookup(GIEnumInfo *Info);

static void instance_finalize(object_instance_t *Instance, void *Data) {
	g_object_unref(Instance->Handle);
}

static GQuark MLQuark;

static ml_value_t *object_instance_get(void *Handle) {
	if (Handle == 0) return (ml_value_t *)ObjectInstanceNil;
	ml_value_t *Instance = (ml_value_t *)g_object_get_qdata(Handle, MLQuark);
	if (Instance) return Instance;
	GType Type = G_OBJECT_TYPE(Handle);
	GIBaseInfo *Info = g_irepository_find_by_gtype(NULL, Type);
	switch (g_base_info_get_type(Info)) {
	case GI_INFO_TYPE_INVALID: {
		return ml_error("NotImplemented", "Not able to marshal %s yet at %d", g_base_info_get_name(Info), __LINE__);
	}
	case GI_INFO_TYPE_FUNCTION: {
		return ml_error("NotImplemented", "Not able to marshal %s yet at %d", g_base_info_get_name(Info), __LINE__);
	}
	case GI_INFO_TYPE_CALLBACK: {
		return ml_error("NotImplemented", "Not able to marshal %s yet at %d", g_base_info_get_name(Info), __LINE__);
	}
	case GI_INFO_TYPE_STRUCT: {
		return ml_error("NotImplemented", "Not able to marshal %s yet at %d", g_base_info_get_name(Info), __LINE__);
	}
	case GI_INFO_TYPE_BOXED: {
		return ml_error("NotImplemented", "Not able to marshal %s yet at %d", g_base_info_get_name(Info), __LINE__);
	}
	case GI_INFO_TYPE_ENUM: {
		return ml_error("NotImplemented", "Not able to marshal %s yet at %d", g_base_info_get_name(Info), __LINE__);
	}
	case GI_INFO_TYPE_FLAGS: {
		return ml_error("NotImplemented", "Not able to marshal %s yet at %d", g_base_info_get_name(Info), __LINE__);
	}
	case GI_INFO_TYPE_OBJECT: {
		object_instance_t *Instance = new(object_instance_t);
		Instance->Type = (object_t *)object_info_lookup((GIObjectInfo *)Info);
		Instance->Handle = Handle;
		g_object_set_qdata(Handle, MLQuark, Instance);
		g_object_ref_sink(Handle);
		GC_register_finalizer(Instance, (GC_finalization_proc)instance_finalize, 0, 0, 0);
		return (ml_value_t *)Instance;
		break;
	}
	case GI_INFO_TYPE_INTERFACE: {
		return ml_error("NotImplemented", "Not able to marshal %s yet at %d", g_base_info_get_name(Info), __LINE__);
	}
	case GI_INFO_TYPE_CONSTANT: {
		return ml_error("NotImplemented", "Not able to marshal %s yet at %d", g_base_info_get_name(Info), __LINE__);
	}
	case GI_INFO_TYPE_UNION: {
		return ml_error("NotImplemented", "Not able to marshal %s yet at %d", g_base_info_get_name(Info), __LINE__);
	}
	case GI_INFO_TYPE_VALUE: {
		return ml_error("NotImplemented", "Not able to marshal %s yet at %d", g_base_info_get_name(Info), __LINE__);
	}
	case GI_INFO_TYPE_SIGNAL: {
		return ml_error("NotImplemented", "Not able to marshal %s yet at %d", g_base_info_get_name(Info), __LINE__);
	}
	case GI_INFO_TYPE_VFUNC: {
		return ml_error("NotImplemented", "Not able to marshal %s yet at %d", g_base_info_get_name(Info), __LINE__);
	}
	case GI_INFO_TYPE_PROPERTY: {
		return ml_error("NotImplemented", "Not able to marshal %s yet at %d", g_base_info_get_name(Info), __LINE__);
	}
	case GI_INFO_TYPE_FIELD: {
		return ml_error("NotImplemented", "Not able to marshal %s yet at %d", g_base_info_get_name(Info), __LINE__);
	}
	case GI_INFO_TYPE_ARG: {
		return ml_error("NotImplemented", "Not able to marshal %s yet at %d", g_base_info_get_name(Info), __LINE__);
	}
	case GI_INFO_TYPE_TYPE: {
		return ml_error("NotImplemented", "Not able to marshal %s yet at %d", g_base_info_get_name(Info), __LINE__);
	}
	case GI_INFO_TYPE_UNRESOLVED: {
		return ml_error("NotImplemented", "Not able to marshal %s yet at %d", g_base_info_get_name(Info), __LINE__);
	}
	}
	return MLNil;
}

static ml_value_t *ml_object_instance_to_string(void *Data, int Count, ml_value_t **Args) {
	object_instance_t *Instance = (object_instance_t *)Args[0];
	return ml_string_format("<%s>", g_base_info_get_name((GIBaseInfo *)Instance->Type->Info));
}

typedef struct struct_t {
	ml_type_t Base;
	GIStructInfo *Info;
} struct_t;

typedef struct struct_instance_t {
	const struct_t *Type;
	void *Value;
} struct_instance_t;

static ml_type_t *StructT = 0;
static ml_type_t *StructInstanceT = 0;

static ml_value_t *struct_instance_new(struct_t *Struct, int Count, ml_value_t **Args) {
	struct_instance_t *Instance = new(struct_instance_t);
	Instance->Type = Struct;
	Instance->Value = GC_malloc(g_struct_info_get_size(Struct->Info));
	return (ml_value_t *)Instance;
}

static ml_value_t *ml_struct_instance_to_string(void *Data, int Count, ml_value_t **Args) {
	struct_instance_t *Instance = (struct_instance_t *)Args[0];
	return ml_string_format("<%s>", g_base_info_get_name((GIBaseInfo *)Instance->Type->Info));
}

typedef struct field_ref_t {
	const ml_type_t *Type;
	void *Address;
} field_ref_t;

#define FIELD_REF(UNAME, LNAME, GTYPE, GETTER, SETTER) \
static ml_value_t *field_ref_ ## LNAME ## _deref(field_ref_t *Ref) { \
	GTYPE Value = *(GTYPE *)Ref->Address; \
	return GETTER; \
} \
\
static ml_value_t *field_ref_ ## LNAME ## _assign(field_ref_t *Ref, ml_value_t *Value) { \
	GTYPE *Address = (GTYPE *)Ref->Address; \
	*Address = SETTER; \
	return Value; \
} \
\
static ml_type_t FieldRef ## UNAME ## T[1] = {{ \
	MLTypeT, \
	MLAnyT, "field-ref-" #LNAME, \
	ml_default_hash, \
	ml_default_call, \
	(void *)field_ref_ ## LNAME ## _deref, \
	(void *)field_ref_ ## LNAME ## _assign, \
	ml_default_iterate, \
	ml_default_current, \
	ml_default_next, \
	ml_default_key \
}}

FIELD_REF(Boolean, boolean, gboolean, Value ? MLTrue : MLFalse, Value == MLTrue);
FIELD_REF(Int8, int8, gint8, ml_integer(Value), ml_integer_value(Value));
FIELD_REF(UInt8, uint8, guint8, ml_integer(Value), ml_integer_value(Value));
FIELD_REF(Int16, int16, gint16, ml_integer(Value), ml_integer_value(Value));
FIELD_REF(UInt16, uint16, guint16, ml_integer(Value), ml_integer_value(Value));
FIELD_REF(Int32, int32, gint32, ml_integer(Value), ml_integer_value(Value));
FIELD_REF(UInt32, uint32, guint32, ml_integer(Value), ml_integer_value(Value));
FIELD_REF(Int64, int64, gint64, ml_integer(Value), ml_integer_value(Value));
FIELD_REF(UInt64, uint64, guint64, ml_integer(Value), ml_integer_value(Value));
FIELD_REF(Float, float, gfloat, ml_real(Value), ml_real_value(Value));
FIELD_REF(Double, double, gdouble, ml_real(Value), ml_real_value(Value));
FIELD_REF(Utf8, utf8, gchar *, ml_string(Value, -1), (char *)ml_string_value(Value));

static ml_value_t *struct_field_ref(GIFieldInfo *Info, int Count, ml_value_t **Args) {
	struct_instance_t *Instance = (struct_instance_t *)Args[0];
	field_ref_t *Ref = new(field_ref_t);
	Ref->Address = (char *)Instance->Value + g_field_info_get_offset(Info);
	GITypeInfo *TypeInfo = g_field_info_get_type(Info);
	switch (g_type_info_get_tag(TypeInfo)) {
	case GI_TYPE_TAG_VOID: return ml_error("TodoError", "Field ref not implemented yet");
	case GI_TYPE_TAG_BOOLEAN: Ref->Type = FieldRefBooleanT; break;
	case GI_TYPE_TAG_INT8: Ref->Type = FieldRefInt8T; break;
	case GI_TYPE_TAG_UINT8: Ref->Type = FieldRefUInt8T; break;
	case GI_TYPE_TAG_INT16: Ref->Type = FieldRefInt16T; break;
	case GI_TYPE_TAG_UINT16: Ref->Type = FieldRefUInt16T; break;
	case GI_TYPE_TAG_INT32: Ref->Type = FieldRefInt32T; break;
	case GI_TYPE_TAG_UINT32: Ref->Type = FieldRefUInt32T; break;
	case GI_TYPE_TAG_INT64: Ref->Type = FieldRefInt64T; break;
	case GI_TYPE_TAG_UINT64: Ref->Type = FieldRefUInt64T; break;
	case GI_TYPE_TAG_FLOAT: Ref->Type = FieldRefFloatT; break;
	case GI_TYPE_TAG_DOUBLE: Ref->Type = FieldRefDoubleT; break;
	case GI_TYPE_TAG_GTYPE: return ml_error("TodoError", "Field ref not implemented yet");
	case GI_TYPE_TAG_UTF8: Ref->Type = FieldRefUtf8T; break;
	case GI_TYPE_TAG_FILENAME: Ref->Type = FieldRefUtf8T; break;
	case GI_TYPE_TAG_ARRAY: return ml_error("TodoError", "Field ref not implemented yet");
	case GI_TYPE_TAG_INTERFACE: {
		GIBaseInfo *InterfaceInfo = g_type_info_get_interface(TypeInfo);
		switch (g_base_info_get_type(InterfaceInfo)) {
		case GI_INFO_TYPE_INVALID: return ml_error("TodoError", "Field ref not implemented yet");
		case GI_INFO_TYPE_FUNCTION: return ml_error("TodoError", "Field ref not implemented yet");
		case GI_INFO_TYPE_CALLBACK: return ml_error("TodoError", "Field ref not implemented yet");
		case GI_INFO_TYPE_STRUCT: return ml_error("TodoError", "Field ref not implemented yet");
		case GI_INFO_TYPE_BOXED: return ml_error("TodoError", "Field ref not implemented yet");
		case GI_INFO_TYPE_ENUM: return ml_error("TodoError", "Field ref not implemented yet");
		case GI_INFO_TYPE_FLAGS: return ml_error("TodoError", "Field ref not implemented yet");
		case GI_INFO_TYPE_OBJECT: return ml_error("TodoError", "Field ref not implemented yet");
		case GI_INFO_TYPE_INTERFACE: return ml_error("TodoError", "Field ref not implemented yet");
		case GI_INFO_TYPE_CONSTANT: return ml_error("TodoError", "Field ref not implemented yet");
		case GI_INFO_TYPE_UNION: return ml_error("TodoError", "Field ref not implemented yet");
		case GI_INFO_TYPE_VALUE: return ml_error("TodoError", "Field ref not implemented yet");
		case GI_INFO_TYPE_SIGNAL: return ml_error("TodoError", "Field ref not implemented yet");
		case GI_INFO_TYPE_VFUNC: return ml_error("TodoError", "Field ref not implemented yet");
		case GI_INFO_TYPE_PROPERTY: return ml_error("TodoError", "Field ref not implemented yet");
		case GI_INFO_TYPE_FIELD: return ml_error("TodoError", "Field ref not implemented yet");
		case GI_INFO_TYPE_ARG: return ml_error("TodoError", "Field ref not implemented yet");
		case GI_INFO_TYPE_TYPE: return ml_error("TodoError", "Field ref not implemented yet");
		case GI_INFO_TYPE_UNRESOLVED: return ml_error("TodoError", "Field ref not implemented yet");
		}
		break;
	}
	case GI_TYPE_TAG_GLIST: return ml_error("TodoError", "Field ref not implemented yet");
	case GI_TYPE_TAG_GSLIST: return ml_error("TodoError", "Field ref not implemented yet");
	case GI_TYPE_TAG_GHASH: return ml_error("TodoError", "Field ref not implemented yet");
	case GI_TYPE_TAG_ERROR: return ml_error("TodoError", "Field ref not implemented yet");
	case GI_TYPE_TAG_UNICHAR: return ml_error("TodoError", "Field ref not implemented yet");
	}
	return (ml_value_t *)Ref;
}

typedef struct enum_t {
	ml_type_t Base;
	GIEnumInfo *Info;
	stringmap_t ByName[1];
	ml_value_t *ByIndex[];
} enum_t;

typedef struct enum_value_t {
	const enum_t *Type;
	ml_value_t *Name;
	gint64 Value;
} enum_value_t;

static ml_type_t *EnumT = 0;
static ml_type_t *EnumValueT = 0;

static ml_value_t *ml_enum_value_to_string(void *Data, int Count, ml_value_t **Args) {
	enum_value_t *Value = (enum_value_t *)Args[0];
	return Value->Name;
}

static size_t array_element_size(GITypeInfo *Info) {
	switch (g_type_info_get_tag(Info)) {
	case GI_TYPE_TAG_VOID: return sizeof(char);
	case GI_TYPE_TAG_BOOLEAN: return sizeof(gboolean);
	case GI_TYPE_TAG_INT8: return sizeof(gint8);
	case GI_TYPE_TAG_UINT8: return sizeof(guint8);
	case GI_TYPE_TAG_INT16: return sizeof(gint16);
	case GI_TYPE_TAG_UINT16: return sizeof(guint16);
	case GI_TYPE_TAG_INT32: return sizeof(gint32);
	case GI_TYPE_TAG_UINT32: return sizeof(guint32);
	case GI_TYPE_TAG_INT64: return sizeof(gint64);
	case GI_TYPE_TAG_UINT64: return sizeof(guint64);
	case GI_TYPE_TAG_FLOAT: return sizeof(gfloat);
	case GI_TYPE_TAG_DOUBLE: return sizeof(gdouble);
	case GI_TYPE_TAG_GTYPE: return sizeof(GType);
	case GI_TYPE_TAG_UTF8: return sizeof(char *);
	case GI_TYPE_TAG_FILENAME: return sizeof(char *);
	case GI_TYPE_TAG_ARRAY: return sizeof(void *);
	case GI_TYPE_TAG_INTERFACE: return sizeof(void *);
	case GI_TYPE_TAG_GLIST: return sizeof(GList *);
	case GI_TYPE_TAG_GSLIST: return sizeof(GSList *);
	case GI_TYPE_TAG_GHASH: return sizeof(GHashTable *);
	case GI_TYPE_TAG_ERROR: return sizeof(GError *);
	case GI_TYPE_TAG_UNICHAR: return sizeof(gunichar);
	}
	return 0;
}

static ml_value_t *function_info_invoke(GIFunctionInfo *Info, int Count, ml_value_t **Args) {
	int NArgs = g_callable_info_get_n_args((GICallableInfo *)Info);
	int NArgsIn = 0, NArgsOut = 0;
	for (int I = 0; I < NArgs; ++I) {
		GIArgInfo *ArgInfo = g_callable_info_get_arg((GICallableInfo *)Info, I);
		switch (g_arg_info_get_direction(ArgInfo)) {
		case GI_DIRECTION_IN: ++NArgsIn; break;
		case GI_DIRECTION_OUT: ++NArgsOut; break;
		case GI_DIRECTION_INOUT: ++NArgsIn; ++NArgsOut; break;
		}
	}
	GIArgument ArgsIn[NArgsIn];
	GIArgument ArgsOut[NArgsOut];
	int IndexIn = 0, IndexOut = 0, N = 0;
	if (g_function_info_get_flags(Info) == GI_FUNCTION_IS_METHOD) {
		ArgsIn[0].v_pointer = ((object_instance_t *)Args[0])->Handle;
		N = 1;
		IndexIn = 1;
	}
	for (int I = 0; I < NArgs; ++I) {
		GIArgInfo *ArgInfo = g_callable_info_get_arg((GICallableInfo *)Info, I);
		GITypeInfo TypeInfo[1];
		g_arg_info_load_type(ArgInfo, TypeInfo);
		switch (g_arg_info_get_direction(ArgInfo)) {
		case GI_DIRECTION_IN: {
			if (N >= Count) return ml_error("InvokeError", "Not enough arguments");
			ml_value_t *Arg = Args[N++];
			switch (g_type_info_get_tag(TypeInfo)) {
			case GI_TYPE_TAG_VOID: {
				break;
			}
			case GI_TYPE_TAG_BOOLEAN: {
				ArgsIn[IndexIn].v_boolean = Arg == MLTrue ? TRUE : FALSE;
				break;
			}
			case GI_TYPE_TAG_INT8: {
				ArgsIn[IndexIn].v_int8 = ml_integer_value(Arg);
				break;
			}
			case GI_TYPE_TAG_UINT8: {
				ArgsIn[IndexIn].v_uint8 = ml_integer_value(Arg);
				break;
			}
			case GI_TYPE_TAG_INT16: {
				ArgsIn[IndexIn].v_int16 = ml_integer_value(Arg);
				break;
			}
			case GI_TYPE_TAG_UINT16: {
				ArgsIn[IndexIn].v_uint16 = ml_integer_value(Arg);
				break;
			}
			case GI_TYPE_TAG_INT32: {
				ArgsIn[IndexIn].v_int32 = ml_integer_value(Arg);
				break;
			}
			case GI_TYPE_TAG_UINT32: {
				ArgsIn[IndexIn].v_uint32 = ml_integer_value(Arg);
				break;
			}
			case GI_TYPE_TAG_INT64: {
				ArgsIn[IndexIn].v_int64 = ml_integer_value(Arg);
				break;
			}
			case GI_TYPE_TAG_UINT64: {
				ArgsIn[IndexIn].v_uint64 = ml_integer_value(Arg);
				break;
			}
			case GI_TYPE_TAG_FLOAT: {
				ArgsIn[IndexIn].v_float = ml_real_value(Arg);
				break;
			}
			case GI_TYPE_TAG_DOUBLE: {
				ArgsIn[IndexIn].v_double = ml_real_value(Arg);
				break;
			}
			case GI_TYPE_TAG_GTYPE: {
				break;
			}
			case GI_TYPE_TAG_UTF8: {
				ArgsIn[IndexIn].v_string = (char *)ml_string_value(Arg);
				break;
			}
			case GI_TYPE_TAG_FILENAME: {
				ArgsIn[IndexIn].v_string = (char *)ml_string_value(Arg);
				break;
			}
			case GI_TYPE_TAG_ARRAY: {
				if (!ml_is(Arg, MLListT)) {
					return ml_error("TypeError", "Expected list for parameter %d", I);
				}
				GITypeInfo *ElementInfo = g_type_info_get_param_type(TypeInfo, 0);
				size_t ElementSize = array_element_size(ElementInfo);
				char *Array = GC_malloc((ml_list_length(Arg) + 1) * ElementSize);
				// TODO: fill array
				ArgsIn[IndexIn].v_pointer = Array;
				break;
			}
			case GI_TYPE_TAG_INTERFACE: {
				GIBaseInfo *InterfaceInfo = g_type_info_get_interface(TypeInfo);
				switch (g_base_info_get_type(InterfaceInfo)) {
				case GI_INFO_TYPE_INVALID: {
					return ml_error("NotImplemented", "Not able to marshal %s yet at %d", g_base_info_get_name(InterfaceInfo), __LINE__);
				}
				case GI_INFO_TYPE_FUNCTION: {
					return ml_error("NotImplemented", "Not able to marshal %s yet at %d", g_base_info_get_name(InterfaceInfo), __LINE__);
				}
				case GI_INFO_TYPE_CALLBACK: {
					return ml_error("NotImplemented", "Not able to marshal %s yet at %d", g_base_info_get_name(InterfaceInfo), __LINE__);
				}
				case GI_INFO_TYPE_STRUCT: {
					if (ml_is(Arg, StructInstanceT)) {
						ArgsIn[IndexIn].v_pointer = ((struct_instance_t *)Arg)->Value;
					} else {
						return ml_error("TypeError", "Expected gir struct not %s for parameter %d", Args[I]->Type->Name, I);
					}
					break;
				}
				case GI_INFO_TYPE_BOXED: {
					return ml_error("NotImplemented", "Not able to marshal %s yet at %d", g_base_info_get_name(InterfaceInfo), __LINE__);
				}
				case GI_INFO_TYPE_ENUM: {
					if (ml_is(Arg, EnumValueT)) {
						ArgsIn[IndexIn].v_int64 = ((enum_value_t *)Arg)->Value;
					} else {
						return ml_error("TypeError", "Expected gir enum not %s for parameter %d", Args[I]->Type->Name, I);
					}
					break;
				}
				case GI_INFO_TYPE_FLAGS: {
					return ml_error("NotImplemented", "Not able to marshal %s yet at %d", g_base_info_get_name(InterfaceInfo), __LINE__);
				}
				case GI_INFO_TYPE_OBJECT: {
					if (ml_is(Arg, ObjectInstanceT)) {
						ArgsIn[IndexIn].v_pointer = ((object_instance_t *)Arg)->Handle;
					} else {
						return ml_error("TypeError", "Expected gir object not %s for parameter %d", Args[I]->Type->Name, I);
					}
					break;
				}
				case GI_INFO_TYPE_INTERFACE: {
					if (ml_is(Arg, ObjectInstanceT)) {
						ArgsIn[IndexIn].v_pointer = ((object_instance_t *)Arg)->Handle;
					} else {
						return ml_error("TypeError", "Expected gir object not %s for parameter %d", Args[I]->Type->Name, I);
					}
					break;
				}
				case GI_INFO_TYPE_CONSTANT: {
					return ml_error("NotImplemented", "Not able to marshal %s yet at %d", g_base_info_get_name(InterfaceInfo), __LINE__);
				}
				case GI_INFO_TYPE_UNION: {
					return ml_error("NotImplemented", "Not able to marshal %s yet at %d", g_base_info_get_name(InterfaceInfo), __LINE__);
				}
				case GI_INFO_TYPE_VALUE: {
					return ml_error("NotImplemented", "Not able to marshal %s yet at %d", g_base_info_get_name(InterfaceInfo), __LINE__);
				}
				case GI_INFO_TYPE_SIGNAL: {
					return ml_error("NotImplemented", "Not able to marshal %s yet at %d", g_base_info_get_name(InterfaceInfo), __LINE__);
				}
				case GI_INFO_TYPE_VFUNC: {
					return ml_error("NotImplemented", "Not able to marshal %s yet at %d", g_base_info_get_name(InterfaceInfo), __LINE__);
				}
				case GI_INFO_TYPE_PROPERTY: {
					return ml_error("NotImplemented", "Not able to marshal %s yet at %d", g_base_info_get_name(InterfaceInfo), __LINE__);
				}
				case GI_INFO_TYPE_FIELD: {
					return ml_error("NotImplemented", "Not able to marshal %s yet at %d", g_base_info_get_name(InterfaceInfo), __LINE__);
				}
				case GI_INFO_TYPE_ARG: {
					return ml_error("NotImplemented", "Not able to marshal %s yet at %d", g_base_info_get_name(InterfaceInfo), __LINE__);
				}
				case GI_INFO_TYPE_TYPE: {
					return ml_error("NotImplemented", "Not able to marshal %s yet at %d", g_base_info_get_name(InterfaceInfo), __LINE__);
				}
				case GI_INFO_TYPE_UNRESOLVED: {
					return ml_error("NotImplemented", "Not able to marshal %s yet at %d", g_base_info_get_name(InterfaceInfo), __LINE__);
				}
				}
				break;
			}
			case GI_TYPE_TAG_GLIST: {
				break;
			}
			case GI_TYPE_TAG_GSLIST: {
				break;
			}
			case GI_TYPE_TAG_GHASH: {
				break;
			}
			case GI_TYPE_TAG_ERROR: {
				break;
			}
			case GI_TYPE_TAG_UNICHAR: {
				break;
			}
			}
			++IndexIn;
			break;
		}
		case GI_DIRECTION_OUT: {
			switch (g_type_info_get_tag(TypeInfo)) {
			case GI_TYPE_TAG_VOID: {
				break;
			}
			case GI_TYPE_TAG_BOOLEAN: {
				break;
			}
			case GI_TYPE_TAG_INT8: {
				break;
			}
			case GI_TYPE_TAG_UINT8: {
				break;
			}
			case GI_TYPE_TAG_INT16: {
				break;
			}
			case GI_TYPE_TAG_UINT16: {
				break;
			}
			case GI_TYPE_TAG_INT32: {
				break;
			}
			case GI_TYPE_TAG_UINT32: {
				break;
			}
			case GI_TYPE_TAG_INT64: {
				break;
			}
			case GI_TYPE_TAG_UINT64: {
				break;
			}
			case GI_TYPE_TAG_FLOAT: {
				break;
			}
			case GI_TYPE_TAG_DOUBLE: {
				break;
			}
			case GI_TYPE_TAG_GTYPE: {
				break;
			}
			case GI_TYPE_TAG_UTF8: {
				break;
			}
			case GI_TYPE_TAG_FILENAME: {
				break;
			}
			case GI_TYPE_TAG_ARRAY: {
				break;
			}
			case GI_TYPE_TAG_INTERFACE: {
				GIBaseInfo *InterfaceInfo = g_type_info_get_interface(TypeInfo);
				switch (g_base_info_get_type(InterfaceInfo)) {
				case GI_INFO_TYPE_INVALID: {
					return ml_error("NotImplemented", "Not able to marshal %s yet at %d", g_base_info_get_name(InterfaceInfo), __LINE__);
				}
				case GI_INFO_TYPE_FUNCTION: {
					return ml_error("NotImplemented", "Not able to marshal %s yet at %d", g_base_info_get_name(InterfaceInfo), __LINE__);
				}
				case GI_INFO_TYPE_CALLBACK: {
					return ml_error("NotImplemented", "Not able to marshal %s yet at %d", g_base_info_get_name(InterfaceInfo), __LINE__);
				}
				case GI_INFO_TYPE_STRUCT: {
					if (N >= Count) return ml_error("InvokeError", "Not enough arguments");
					ml_value_t *Arg = Args[N++];
					if (ml_is(Arg, StructInstanceT)) {
						ArgsOut[IndexOut].v_pointer = ((struct_instance_t *)Arg)->Value;
					} else {
						return ml_error("TypeError", "Expected gir struct not %s for parameter %d", Args[I]->Type->Name, I);
					}
					break;
				}
				case GI_INFO_TYPE_BOXED: {
					return ml_error("NotImplemented", "Not able to marshal %s yet at %d", g_base_info_get_name(InterfaceInfo), __LINE__);
				}
				case GI_INFO_TYPE_ENUM: {
					break;
				}
				case GI_INFO_TYPE_FLAGS: {
					return ml_error("NotImplemented", "Not able to marshal %s yet at %d", g_base_info_get_name(InterfaceInfo), __LINE__);
				}
				case GI_INFO_TYPE_OBJECT: {
					break;
				}
				case GI_INFO_TYPE_INTERFACE: {
					break;
				}
				case GI_INFO_TYPE_CONSTANT: {
					return ml_error("NotImplemented", "Not able to marshal %s yet at %d", g_base_info_get_name(InterfaceInfo), __LINE__);
				}
				case GI_INFO_TYPE_UNION: {
					return ml_error("NotImplemented", "Not able to marshal %s yet at %d", g_base_info_get_name(InterfaceInfo), __LINE__);
				}
				case GI_INFO_TYPE_VALUE: {
					return ml_error("NotImplemented", "Not able to marshal %s yet at %d", g_base_info_get_name(InterfaceInfo), __LINE__);
				}
				case GI_INFO_TYPE_SIGNAL: {
					return ml_error("NotImplemented", "Not able to marshal %s yet at %d", g_base_info_get_name(InterfaceInfo), __LINE__);
				}
				case GI_INFO_TYPE_VFUNC: {
					return ml_error("NotImplemented", "Not able to marshal %s yet at %d", g_base_info_get_name(InterfaceInfo), __LINE__);
				}
				case GI_INFO_TYPE_PROPERTY: {
					return ml_error("NotImplemented", "Not able to marshal %s yet at %d", g_base_info_get_name(InterfaceInfo), __LINE__);
				}
				case GI_INFO_TYPE_FIELD: {
					return ml_error("NotImplemented", "Not able to marshal %s yet at %d", g_base_info_get_name(InterfaceInfo), __LINE__);
				}
				case GI_INFO_TYPE_ARG: {
					return ml_error("NotImplemented", "Not able to marshal %s yet at %d", g_base_info_get_name(InterfaceInfo), __LINE__);
				}
				case GI_INFO_TYPE_TYPE: {
					return ml_error("NotImplemented", "Not able to marshal %s yet at %d", g_base_info_get_name(InterfaceInfo), __LINE__);
				}
				case GI_INFO_TYPE_UNRESOLVED: {
					return ml_error("NotImplemented", "Not able to marshal %s yet at %d", g_base_info_get_name(InterfaceInfo), __LINE__);
				}
				}
				break;
			}
			case GI_TYPE_TAG_GLIST: {
				break;
			}
			case GI_TYPE_TAG_GSLIST: {
				break;
			}
			case GI_TYPE_TAG_GHASH: {
				break;
			}
			case GI_TYPE_TAG_ERROR: {
				break;
			}
			case GI_TYPE_TAG_UNICHAR: {
				break;
			}
			}
			++IndexOut;
			break;
		}
		case GI_DIRECTION_INOUT: {
			switch (g_type_info_get_tag(TypeInfo)) {
			case GI_TYPE_TAG_VOID: {
				break;
			}
			case GI_TYPE_TAG_BOOLEAN: {
				break;
			}
			case GI_TYPE_TAG_INT8: {
				break;
			}
			case GI_TYPE_TAG_UINT8: {
				break;
			}
			case GI_TYPE_TAG_INT16: {
				break;
			}
			case GI_TYPE_TAG_UINT16: {
				break;
			}
			case GI_TYPE_TAG_INT32: {
				break;
			}
			case GI_TYPE_TAG_UINT32: {
				break;
			}
			case GI_TYPE_TAG_INT64: {
				break;
			}
			case GI_TYPE_TAG_UINT64: {
				break;
			}
			case GI_TYPE_TAG_FLOAT: {
				break;
			}
			case GI_TYPE_TAG_DOUBLE: {
				break;
			}
			case GI_TYPE_TAG_GTYPE: {
				break;
			}
			case GI_TYPE_TAG_UTF8: {
				break;
			}
			case GI_TYPE_TAG_FILENAME: {
				break;
			}
			case GI_TYPE_TAG_ARRAY: {
				break;
			}
			case GI_TYPE_TAG_INTERFACE: {
				GIBaseInfo *InterfaceInfo = g_type_info_get_interface(TypeInfo);
				switch (g_base_info_get_type(InterfaceInfo)) {
				case GI_INFO_TYPE_INVALID: {
					return ml_error("NotImplemented", "Not able to marshal %s yet at %d", g_base_info_get_name(InterfaceInfo), __LINE__);
				}
				case GI_INFO_TYPE_FUNCTION: {
					return ml_error("NotImplemented", "Not able to marshal %s yet at %d", g_base_info_get_name(InterfaceInfo), __LINE__);
				}
				case GI_INFO_TYPE_CALLBACK: {
					return ml_error("NotImplemented", "Not able to marshal %s yet at %d", g_base_info_get_name(InterfaceInfo), __LINE__);
				}
				case GI_INFO_TYPE_STRUCT: {
					if (N >= Count) return ml_error("InvokeError", "Not enough arguments");
					ml_value_t *Arg = Args[N++];
					if (ml_is(Arg, StructInstanceT)) {
						ArgsIn[IndexIn].v_pointer = ArgsOut[IndexOut].v_pointer = ((struct_instance_t *)Arg)->Value;
					} else {
						return ml_error("TypeError", "Expected gir struct not %s for parameter %d", Args[I]->Type->Name, I);
					}
					break;
				}
				case GI_INFO_TYPE_BOXED: {
					return ml_error("NotImplemented", "Not able to marshal %s yet at %d", g_base_info_get_name(InterfaceInfo), __LINE__);
				}
				case GI_INFO_TYPE_ENUM: {
					break;
				}
				case GI_INFO_TYPE_FLAGS: {
					return ml_error("NotImplemented", "Not able to marshal %s yet at %d", g_base_info_get_name(InterfaceInfo), __LINE__);
				}
				case GI_INFO_TYPE_OBJECT: {
					break;
				}
				case GI_INFO_TYPE_INTERFACE: {
					break;
				}
				case GI_INFO_TYPE_CONSTANT: {
					return ml_error("NotImplemented", "Not able to marshal %s yet at %d", g_base_info_get_name(InterfaceInfo), __LINE__);
				}
				case GI_INFO_TYPE_UNION: {
					return ml_error("NotImplemented", "Not able to marshal %s yet at %d", g_base_info_get_name(InterfaceInfo), __LINE__);
				}
				case GI_INFO_TYPE_VALUE: {
					return ml_error("NotImplemented", "Not able to marshal %s yet at %d", g_base_info_get_name(InterfaceInfo), __LINE__);
				}
				case GI_INFO_TYPE_SIGNAL: {
					return ml_error("NotImplemented", "Not able to marshal %s yet at %d", g_base_info_get_name(InterfaceInfo), __LINE__);
				}
				case GI_INFO_TYPE_VFUNC: {
					return ml_error("NotImplemented", "Not able to marshal %s yet at %d", g_base_info_get_name(InterfaceInfo), __LINE__);
				}
				case GI_INFO_TYPE_PROPERTY: {
					return ml_error("NotImplemented", "Not able to marshal %s yet at %d", g_base_info_get_name(InterfaceInfo), __LINE__);
				}
				case GI_INFO_TYPE_FIELD: {
					return ml_error("NotImplemented", "Not able to marshal %s yet at %d", g_base_info_get_name(InterfaceInfo), __LINE__);
				}
				case GI_INFO_TYPE_ARG: {
					return ml_error("NotImplemented", "Not able to marshal %s yet at %d", g_base_info_get_name(InterfaceInfo), __LINE__);
				}
				case GI_INFO_TYPE_TYPE: {
					return ml_error("NotImplemented", "Not able to marshal %s yet at %d", g_base_info_get_name(InterfaceInfo), __LINE__);
				}
				case GI_INFO_TYPE_UNRESOLVED: {
					return ml_error("NotImplemented", "Not able to marshal %s yet at %d", g_base_info_get_name(InterfaceInfo), __LINE__);
				}
				}
				break;
			}
			case GI_TYPE_TAG_GLIST: {
				break;
			}
			case GI_TYPE_TAG_GSLIST: {
				break;
			}
			case GI_TYPE_TAG_GHASH: {
				break;
			}
			case GI_TYPE_TAG_ERROR: {
				break;
			}
			case GI_TYPE_TAG_UNICHAR: {
				break;
			}
			}
			++IndexIn;
			++IndexOut;
			break;
		}
		}
	}
	GError *Error = 0;
	GIArgument ReturnValue[1];
	gboolean Invoked = g_function_info_invoke(Info, ArgsIn, IndexIn, ArgsOut, IndexOut, ReturnValue, &Error);
	if (!Invoked) return ml_error("InvokeError", "Error: %s", Error->message);
	GITypeInfo *ReturnInfo = g_callable_info_get_return_type((GICallableInfo *)Info);
	switch (g_type_info_get_tag(ReturnInfo)) {
	case GI_TYPE_TAG_VOID: {
		return MLNil;
	}
	case GI_TYPE_TAG_BOOLEAN: {
		return ReturnValue->v_boolean ? MLTrue : MLFalse;
	}
	case GI_TYPE_TAG_INT8: {
		return ml_integer(ReturnValue->v_int8);
	}
	case GI_TYPE_TAG_UINT8: {
		return ml_integer(ReturnValue->v_uint8);
	}
	case GI_TYPE_TAG_INT16: {
		return ml_integer(ReturnValue->v_int16);
	}
	case GI_TYPE_TAG_UINT16: {
		return ml_integer(ReturnValue->v_uint16);
	}
	case GI_TYPE_TAG_INT32: {
		return ml_integer(ReturnValue->v_int32);
	}
	case GI_TYPE_TAG_UINT32: {
		return ml_integer(ReturnValue->v_uint32);
	}
	case GI_TYPE_TAG_INT64: {
		return ml_integer(ReturnValue->v_int64);
	}
	case GI_TYPE_TAG_UINT64: {
		return ml_integer(ReturnValue->v_uint64);
	}
	case GI_TYPE_TAG_FLOAT: {
		return ml_real(ReturnValue->v_float);
	}
	case GI_TYPE_TAG_DOUBLE: {
		return ml_real(ReturnValue->v_double);
	}
	case GI_TYPE_TAG_GTYPE: {
		break;
	}
	case GI_TYPE_TAG_UTF8: {
		return ml_string(ReturnValue->v_string, -1);
	}
	case GI_TYPE_TAG_FILENAME: {
		return ml_string(ReturnValue->v_string, -1);
	}
	case GI_TYPE_TAG_ARRAY: {
		break;
	}
	case GI_TYPE_TAG_INTERFACE: {
		GIBaseInfo *InterfaceInfo = g_type_info_get_interface(ReturnInfo);
		switch (g_base_info_get_type(InterfaceInfo)) {
		case GI_INFO_TYPE_STRUCT: {
			struct_instance_t *Instance = new(struct_instance_t);
			Instance->Type = (struct_t *)struct_info_lookup((GIStructInfo *)InterfaceInfo);
			Instance->Value = ReturnValue->v_pointer;
			break;
		}
		case GI_INFO_TYPE_BOXED: {
			break;
		}
		case GI_INFO_TYPE_ENUM: {
			enum_t *Enum = (enum_t *)enum_info_lookup((GIEnumInfo *)InterfaceInfo);
			return Enum->ByIndex[ReturnValue->v_int];
		}
		case GI_INFO_TYPE_FLAGS: {
			break;
		}
		case GI_INFO_TYPE_OBJECT: {
			return object_instance_get(ReturnValue->v_pointer);
		}
		case GI_INFO_TYPE_INTERFACE: {
			break;
		}
		case GI_INFO_TYPE_CONSTANT: {
			break;
		}
		case GI_INFO_TYPE_UNION: {
			break;
		}
		case GI_INFO_TYPE_VALUE: {
			break;
		}
		case GI_INFO_TYPE_SIGNAL: {
			break;
		}
		case GI_INFO_TYPE_VFUNC: {
			break;
		}
		case GI_INFO_TYPE_PROPERTY: {
			break;
		}
		case GI_INFO_TYPE_FIELD: {
			break;
		}
		case GI_INFO_TYPE_ARG: {
			break;
		}
		case GI_INFO_TYPE_TYPE: {
			break;
		}
		case GI_INFO_TYPE_UNRESOLVED: {
			break;
		}
		}
		break;
	}
	case GI_TYPE_TAG_GLIST: {
		return ml_list();
	}
	case GI_TYPE_TAG_GSLIST: {
		return ml_list();
	}
	case GI_TYPE_TAG_GHASH: {
		return ml_map();
	}
	case GI_TYPE_TAG_ERROR: {
		GError *Error = ReturnValue->v_pointer;
		return ml_error("GError", "%s", Error->message);
	}
	case GI_TYPE_TAG_UNICHAR: {
		return ml_integer(ReturnValue->v_uint32);
	}
	}
	return ml_error("InvokeError", "Unsupported situtation");
}

static ml_value_t *constructor_invoke(GIFunctionInfo *Info, int Count, ml_value_t **Args) {
	return function_info_invoke(Info, Count - 1, Args + 1);
}

static ml_value_t *method_invoke(GIFunctionInfo *Info, int Count, ml_value_t **Args) {
	return function_info_invoke(Info, Count, Args);
}

static void interface_add_methods(object_t *Object, GIInterfaceInfo *Info) {
	int NumMethods = g_interface_info_get_n_methods(Info);
	for (int I = 0; I < NumMethods; ++I) {
		GIFunctionInfo *MethodInfo = g_interface_info_get_method(Info, I);
		const char *MethodName = g_base_info_get_name((GIBaseInfo *)MethodInfo);
		switch (g_function_info_get_flags(MethodInfo)) {
		case GI_FUNCTION_IS_METHOD: {
			ml_method_by_name(MethodName, MethodInfo, (ml_callback_t)method_invoke, Object, NULL);
			break;
		}
		}
	}
}

static void object_add_methods(object_t *Object, ml_type_t *Type, GIObjectInfo *Info) {
	int NumMethods = g_object_info_get_n_methods(Info);
	for (int I = 0; I < NumMethods; ++I) {
		GIFunctionInfo *MethodInfo = g_object_info_get_method(Info, I);
		const char *MethodName = g_base_info_get_name((GIBaseInfo *)MethodInfo);
		switch (g_function_info_get_flags(MethodInfo)) {
		case GI_FUNCTION_IS_METHOD: {
			ml_method_by_name(MethodName, MethodInfo, (ml_callback_t)method_invoke, Object, NULL);
			break;
		}
		case GI_FUNCTION_IS_CONSTRUCTOR: {
			ml_method_by_name(MethodName, MethodInfo, (ml_callback_t)constructor_invoke, Type, NULL);
			break;
		}
		}
	}
	GIObjectInfo *Parent = g_object_info_get_parent(Info);
	if (Parent) object_add_methods(Object, Type, Parent);
	int NumInterfaces = g_object_info_get_n_interfaces(Info);
	for (int I = 0; I < NumInterfaces; ++I) {
		interface_add_methods(Object, g_object_info_get_interface(Info, I));
	}
}

static stringmap_t TypeMap[1] = {STRINGMAP_INIT};

static ml_type_t *object_info_lookup(GIObjectInfo *Info) {
	const char *TypeName = g_base_info_get_name((GIBaseInfo *)Info);
	ml_type_t **Slot = (ml_type_t **)stringmap_slot(TypeMap, TypeName);
	if (!Slot[0]) {
		ml_type_t *ParentType = ml_type(ObjectT, "gir-parent");
		object_t *Object = new(object_t);
		Object->Base = ObjectT[0];
		Object->Base.Type = ParentType;
		Object->Base.Name = TypeName;
		Object->Base.Parent = ObjectInstanceT;
		Object->Info = Info;
		object_add_methods(Object, ParentType, Info);
		Slot[0] = (ml_type_t *)Object;
	}
	return Slot[0];
}

static ml_type_t *struct_info_lookup(GIStructInfo *Info) {
	const char *TypeName = g_base_info_get_name((GIBaseInfo *)Info);
	ml_type_t **Slot = (ml_type_t **)stringmap_slot(TypeMap, TypeName);
	if (!Slot[0]) {
		ml_type_t *ParentType = ml_type(StructT, "gir-struct");
		struct_t *Struct = new(struct_t);
		Struct->Base = StructT[0];
		Struct->Base.Type = ParentType;
		Struct->Base.Name = TypeName;
		Struct->Base.Parent = StructInstanceT;
		Struct->Info = Info;
		ml_method_by_name("new", Struct, (void *)struct_instance_new, ParentType, NULL);
		Slot[0] = (ml_type_t *)Struct;
		int NumFields = g_struct_info_get_n_fields(Info);
		for (int I = 0; I < NumFields; ++I) {
			GIFieldInfo *FieldInfo = g_struct_info_get_field(Info, I);
			const char *FieldName = g_base_info_get_name((GIBaseInfo *)FieldInfo);
			ml_method_by_name(FieldName, FieldInfo, (ml_callback_t)struct_field_ref, Struct, NULL);
		}
	}
	return Slot[0];
}

static ml_value_t *enum_info_invoke(enum_t *Enum, int Count, ml_value_t **Args) {
	ML_CHECK_ARG_COUNT(1);
	ML_CHECK_ARG_TYPE(0, MLStringT);
	enum_value_t *Value = (enum_value_t *)stringmap_search(Enum->ByName, ml_string_value(Args[0]));
	if (!Value) return ml_error("NameError", "Invalid enum name %s", ml_string_value(Args[0]));
	return (ml_value_t *)Value;
}

static ml_type_t *enum_info_lookup(GIEnumInfo *Info) {
	const char *TypeName = g_registered_type_info_get_type_name((GIBaseInfo *)Info);
	ml_type_t **Slot = (ml_type_t **)stringmap_slot(TypeMap, TypeName);
	if (!Slot[0]) {
		int NumValues = g_enum_info_get_n_values(Info);
		enum_t *Enum = xnew(enum_t, NumValues, ml_value_t *);
		Enum->Base = EnumT[0];
		Enum->Base.Type = EnumT;
		Enum->Base.Name = TypeName;
		Enum->Base.Parent = EnumValueT;
		for (int I = 0; I < NumValues; ++I) {
			GIValueInfo *ValueInfo = g_enum_info_get_value(Info, I);
			const char *ValueName = GC_strdup(g_base_info_get_name((GIBaseInfo *)ValueInfo));
			printf("Enum: %s\n", ValueName);
			enum_value_t *Value = new(enum_value_t);
			Value->Type = Enum;
			Value->Name = ml_string(ValueName, -1);
			Value->Value = g_value_info_get_value(ValueInfo);
			stringmap_insert(Enum->ByName, ValueName, Value);
			Enum->ByIndex[I] = (ml_value_t *)Value;
		}
		Enum->Info = Info;
		Slot[0] = (ml_type_t *)Enum;
	}
	return Slot[0];
}

static ml_value_t *baseinfo_to_value(GIBaseInfo *Info) {
	switch (g_base_info_get_type(Info)) {
	case GI_INFO_TYPE_INVALID: {
		break;
	}
	case GI_INFO_TYPE_FUNCTION: {
		return ml_function(Info, (ml_callback_t)function_info_invoke);
	}
	case GI_INFO_TYPE_CALLBACK: {
		break;
	}
	case GI_INFO_TYPE_STRUCT: {
		return (ml_value_t *)struct_info_lookup((GIStructInfo *)Info);
	}
	case GI_INFO_TYPE_BOXED: {
		break;
	}
	case GI_INFO_TYPE_ENUM: {
		return (ml_value_t *)enum_info_lookup((GIEnumInfo *)Info);
	}
	case GI_INFO_TYPE_FLAGS: {
		break;
	}
	case GI_INFO_TYPE_OBJECT: {
		return (ml_value_t *)object_info_lookup((GIObjectInfo *)Info);
	}
	case GI_INFO_TYPE_INTERFACE: {
		break;
	}
	case GI_INFO_TYPE_CONSTANT: {
		break;
	}
	case GI_INFO_TYPE_UNION: {
		break;
	}
	case GI_INFO_TYPE_VALUE: {
		break;
	}
	case GI_INFO_TYPE_SIGNAL: {
		break;
	}
	case GI_INFO_TYPE_VFUNC: {
		break;
	}
	case GI_INFO_TYPE_PROPERTY: {
		break;
	}
	case GI_INFO_TYPE_FIELD: {
		break;
	}
	case GI_INFO_TYPE_ARG: {
		break;
	}
	case GI_INFO_TYPE_TYPE: {
		break;
	}
	case GI_INFO_TYPE_UNRESOLVED: {
		break;
	}
	}
	return MLNil;
}

static ml_value_t *typelib_invoke(typelib_t *Typelib, int Count, ml_value_t **Args) {
	ML_CHECK_ARG_COUNT(1);
	ML_CHECK_ARG_TYPE(0, MLStringT);
	const char *Name = ml_string_value(Args[0]);
	GIBaseInfo *Info = g_irepository_find_by_name(NULL, Typelib->Namespace, Name);
	if (!Info) return ml_error("NameError", "Symbol %s not found in %s", Name, Typelib->Namespace);
	return baseinfo_to_value(Info);
}

static ml_value_t *typelib_iterate(typelib_t *Typelib) {
	typelib_iter_t *Iter = new(typelib_iter_t);
	Iter->Type = TypelibIterT;
	Iter->Namespace = Typelib->Namespace;
	Iter->Handle = Typelib->Handle;
	Iter->Index = 0;
	Iter->Total = g_irepository_get_n_infos(NULL, Iter->Namespace);
	Iter->Current = g_irepository_get_info(NULL, Iter->Namespace, 0);
	return (ml_value_t *)Iter;
}

static ml_value_t *_value_to_ml(const GValue *Value) {
	switch (G_VALUE_TYPE(Value)) {
	case G_TYPE_NONE: return MLNil;
	case G_TYPE_CHAR: return ml_integer(g_value_get_schar(Value));
	case G_TYPE_UCHAR: return ml_integer(g_value_get_uchar(Value));
	case G_TYPE_BOOLEAN: return g_value_get_boolean(Value) ? MLTrue : MLFalse;
	case G_TYPE_INT: return ml_integer(g_value_get_int(Value));
	case G_TYPE_UINT: return ml_integer(g_value_get_uint(Value));
	case G_TYPE_LONG: return ml_integer(g_value_get_long(Value));
	case G_TYPE_ULONG: return ml_integer(g_value_get_ulong(Value));
	case G_TYPE_ENUM: {
		GType Type = G_VALUE_TYPE(Value);
		GIBaseInfo *Info = g_irepository_find_by_gtype(NULL, Type);
		enum_t *Enum = (enum_t *)enum_info_lookup((GIEnumInfo *)Info);
		return Enum->ByIndex[g_value_get_enum(Value)];
	}
	case G_TYPE_FLAGS: return ml_integer(g_value_get_flags(Value));
	case G_TYPE_FLOAT: return ml_real(g_value_get_float(Value));
	case G_TYPE_DOUBLE: return ml_real(g_value_get_double(Value));
	case G_TYPE_STRING: return ml_string(g_value_get_string(Value), -1);
	case G_TYPE_POINTER: return MLNil; //Std$Address$new(g_value_get_pointer(Value));
	default: {
		if (G_VALUE_HOLDS(Value, G_TYPE_OBJECT)) {
			return object_instance_get(g_value_get_object(Value));
		} else {
			printf("Warning: Unknown parameter type: %s\n", G_VALUE_TYPE_NAME(Value));
			return MLNil;
		}
	}
	}
}

static void _ml_to_value(ml_value_t * Source, GValue *Dest) {
	if (Source == MLNil) {
		g_value_init(Dest, G_TYPE_NONE);
	} else if (Source == MLTrue) {
		g_value_init(Dest, G_TYPE_BOOLEAN);
		g_value_set_boolean(Dest, TRUE);
	} else if (Source == MLFalse) {
		g_value_init(Dest, G_TYPE_BOOLEAN);
		g_value_set_boolean(Dest, FALSE);
	} else if (Source->Type == MLIntegerT) {
		g_value_init(Dest, G_TYPE_LONG);
		g_value_set_long(Dest, ml_integer_value(Source));
	} else if (Source->Type == MLRealT) {
		g_value_init(Dest, G_TYPE_DOUBLE);
		g_value_set_double(Dest, ml_real_value(Source));
	} else if (Source->Type == MLStringT) {
		g_value_init(Dest, G_TYPE_STRING);
		g_value_set_string(Dest, ml_string_value(Source));
	} else if (ml_is(Source, ObjectInstanceT)) {
		void *Object = ((object_instance_t *)Source)->Handle;
		g_value_init(Dest, G_OBJECT_TYPE(Object));
		g_value_set_object(Dest, Object);
	} else if (ml_is(Source, StructInstanceT)) {
		void *Value = ((struct_instance_t *)Source)->Value;
		g_value_init(Dest, G_TYPE_POINTER);
		g_value_set_object(Dest, Value);
	} else if (ml_is(Source, EnumValueT)) {
		enum_t *Enum = (enum_t *)((enum_value_t *)Source)->Type;
		GType Type = g_type_from_name(g_base_info_get_name((GIBaseInfo *)Enum->Info));
		g_value_init(Dest, Type);
		g_value_set_enum(Dest, ((enum_value_t *)Source)->Value);
	} else {
		g_value_init(Dest, G_TYPE_NONE);
	}
}

static void __marshal(GClosure *Closure, GValue *Result, guint NumArgs, const GValue *Args, gpointer Hint, ml_value_t *Function) {
	ml_value_t *MLArgs[NumArgs];
	for (guint I = 0; I < NumArgs; ++I) MLArgs[I] = _value_to_ml(Args + I);
	ml_value_t *MLResult = ml_call(Function, NumArgs, MLArgs);
	if (Result) _ml_to_value(MLResult, Result);
}

static ml_value_t *ml_gir_connect(void *Data, int Count, ml_value_t **Args) {
	object_instance_t *Instance = (object_instance_t *)Args[0];
	const char *Signal = ml_string_value(Args[1]);
	GClosure *Closure = g_closure_new_simple(sizeof(GClosure), NULL);
	g_closure_set_meta_marshal(Closure, Args[2], (GClosureMarshal)__marshal);
	g_signal_connect_closure(Instance->Handle, Signal, Closure, Count > 3 && Args[3] != MLNil);
	return Args[0];
}

void ml_gir_init(stringmap_t *Globals) {
	gtk_init(0, 0);
	TypelibT = ml_type(MLAnyT, "gir-typelib");
	TypelibT->call = (void *)typelib_invoke;
	TypelibT->iterate = (void *)typelib_iterate;
	ObjectT = ml_type(MLTypeT, "gir-object");
	ObjectInstanceT = ml_type(MLAnyT, "gir-object-instance");
	MLQuark = g_quark_from_static_string("<<minilang>>");
	ObjectInstanceNil = new(object_instance_t);
	ObjectInstanceNil->Type = (object_t *)ObjectInstanceT;
	StructT = ml_type(MLTypeT, "gir-struct");
	StructInstanceT = ml_type(MLAnyT, "gir-struct-instance");
	EnumT = ml_type(MLTypeT, "gir-enum");
	EnumT->call = (void *)enum_info_invoke;
	EnumValueT = ml_type(MLAnyT, "gir-value");
	MLTrue = ml_method("true");
	MLFalse = ml_method("false");
	stringmap_insert(Globals, "gir", ml_function(NULL, ml_gir_require));
	ml_method_by_name("connect", NULL, ml_gir_connect, ObjectInstanceT, MLStringT, MLFunctionT, NULL);
	ml_method_by_name("string", NULL, ml_object_instance_to_string, ObjectInstanceT, NULL);
	ml_method_by_name("string", NULL, ml_enum_value_to_string, EnumValueT, NULL);
	ml_method_by_name("string", NULL, ml_struct_instance_to_string, StructInstanceT, NULL);
}
