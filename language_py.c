#include <Python.h>
#include <stdbool.h>
#include <string.h>
#include <signal.h>
#include "util.h"
#include "language.h"

#include <frameobject.h>

typedef struct _py_internal {
    PyObject*globals;
    PyObject*module;
    language_t*li;
    char*buffer;
} py_internal_t;

static PyTypeObject FunctionProxyClass;

typedef struct {
    PyObject_HEAD
    char*name;
    py_internal_t*py_internal;
    function_t*function;
} FunctionProxyObject;

static value_t* pyobject_to_value(language_t*li, PyObject*o)
{
    if(o == Py_None) {
        return value_new_void();
    } else if(PyUnicode_Check(o)) {
        return value_new_string(PyUnicode_AS_DATA(o));
    } else if(PyString_Check(o)) {
        return value_new_string(PyString_AsString(o));
    } else if(PyLong_Check(o)) {
        return value_new_int32(PyLong_AsLongLong(o));
    } else if(PyInt_Check(o)) {
        return value_new_int32(PyInt_AsLong(o));
    } else if(PyFloat_Check(o)) {
        return value_new_float32(PyFloat_AsDouble(o));
#if PY_MAJOR_VERSION >= 3
    } else if(PyDouble_Check(o)) {
        return value_new_float32(PyDouble_AsDouble(o));
#endif
    } else if(PyBool_Check(o)) {
        return value_new_boolean(o == Py_True);
    } else if(PyList_Check(o)) {
        int i;
        int l = PyList_GET_SIZE(o);

        value_t*array = array_new();
        for(i=0;i<l;i++) {
            PyObject*e = PyList_GetItem(o, i);
            if(e == NULL)
                return NULL;
            array_append(array, pyobject_to_value(li, e));
        }
        return array;
    } else if(PyTuple_Check(o)) {
        int i;
        int l = PyTuple_GET_SIZE(o);
        value_t*array = array_new();
        for(i=0;i<l;i++) {
            PyObject*e = PyTuple_GetItem(o, i);
            if(e == NULL)
                return NULL;
            array_append(array, pyobject_to_value(li, e));
        }
        return array;
    } else {
        language_error(li, "Can't convert type %s", o->ob_type->tp_name);
        return NULL;
    }
}

static PyObject* value_to_pyobject(language_t*li, value_t*value, bool arrays_as_tuples)
{
    switch(value->type) {
        case TYPE_VOID:
            return Py_BuildValue("s", NULL);
        break;
        case TYPE_FLOAT32:
            return PyFloat_FromDouble(value->f32);
        break;
        case TYPE_INT32:
            return PyInt_FromLong(value->i32);
        break;
        case TYPE_BOOLEAN:
            return PyBool_FromLong(value->b);
        break;
        case TYPE_STRING: {
            return PyUnicode_FromString(value->str);
        }
        break;
        case TYPE_ARRAY: {
            if(arrays_as_tuples) {
                PyObject *array = PyTuple_New(value->length);
                int i;
                for(i=0;i<value->length;i++) {
                    PyObject*entry = value_to_pyobject(li, value->data[i], false);
                    if(!entry)
                        return NULL;
                    PyTuple_SetItem(array, i, entry);
                }
                return array;
            } else {
                PyObject *array = PyList_New(value->length);
                int i;
                for(i=0;i<value->length;i++) {
                    PyObject*entry = value_to_pyobject(li, value->data[i], false);
                    if(!entry)
                        return NULL;
                    PyList_SetItem(array, i, entry);
                }
                return array;
            }
        }
        break;
        default: {
            return NULL;
        }
    }
}

static PyObject* python_method_proxy(PyObject* _self, PyObject* _args)
{
    FunctionProxyObject* self = (FunctionProxyObject*)_self;

#ifdef DEBUG
    printf("[python] call external %s", self->name);
    _args->ob_type->tp_print(_args, stdout, 0);
    printf("\n");
#endif

    language_t*li = self->py_internal->li;
    value_t*args = pyobject_to_value(li, _args);
    value_t*ret = self->function->call(self->function, args);
    value_destroy(args);

    PyObject*pret = value_to_pyobject(li, ret, false);
    value_destroy(ret);

    return pret;
}

static void handle_exception(language_t*li)
{
    PyObject *exception, *v, *_tb;
    PyErr_Fetch(&exception, &v, &_tb);
    PyErr_NormalizeException(&exception, &v, &_tb);

    language_error(li, "Traceback (most recent call last):");

    PyTracebackObject *tb = (PyTracebackObject*)_tb;
    int err = 0;
    while(tb != NULL && err == 0) {
        language_error(li, "  File \"%.500s\", line %d, in %.500s\n",
                    PyString_AsString(tb->tb_frame->f_code->co_filename),
                    tb->tb_lineno,
                    PyString_AsString(tb->tb_frame->f_code->co_name));
        tb = tb->tb_next;
    }

    PyObject *message = PyObject_Str(v);
    char*msg = PyString_AsString(message);

    if (PyExceptionClass_Check(exception)) {
        PyObject* moduleName;
        char* className = PyExceptionClass_Name(exception);
        if (className != NULL) {
            char *dot = strrchr(className, '.');
            if (dot != NULL)
                className = dot+1;
        }
        language_error(li, "Exception %s: %s", className, msg);
    } else if(exception && exception->ob_type) {
        language_error(li, "Exception %s: %s", exception->ob_type->tp_name, msg);
    } else {
        language_error(li, "Exception: %s", msg);
    }

    Py_DECREF(message);

    Py_DECREF(exception);
    Py_DECREF(v);
    Py_DECREF(_tb);
}

static bool compile_script_py(language_t*li, const char*script)
{
    py_internal_t*py = (py_internal_t*)li->internal;
    log_dbg("[python] compiling script");

    // test memory allocation
    PyObject* tmp = PyString_FromString("test");
    Py_DECREF(tmp);

    PyObject* ret = PyRun_String(script, Py_file_input, py->globals, NULL);
    if(ret == NULL) {
        handle_exception(li);
        PyErr_Print();
        PyErr_Clear();
    }
#ifdef DEBUG
    if(ret!=NULL) {
        log_dbg("[python] compile successful");
    } else {
        log_dbg("[python] compile error");
    }
#endif
    return ret!=NULL;
}

static bool is_function_py(language_t*li, const char*name)
{
    py_internal_t*py = (py_internal_t*)li->internal;

    PyObject*function = PyDict_GetItemString(py->globals, name);

    return function != NULL;
}

static value_t* call_function_py(language_t*li, const char*name, value_t*_args)
{
    py_internal_t*py = (py_internal_t*)li->internal;
    log_dbg("[python] calling function %s", name);

    PyObject*function = PyDict_GetItemString(py->globals, name);
    if(function == NULL) {
        language_error(li, "Couldn't find function %s", name);
        return NULL;
    }

    if(!PyCallable_Check(function)) {
        language_error(li, "Object %s is not callable", name);
        return NULL;
    }

    ternaryfunc call =  function->ob_type->tp_call;
    if(call == NULL) {
        language_error(li, "Object %s is not callable", name);
        return NULL;
    }

    PyObject*args = value_to_pyobject(li, _args, true);
    if(!args)
        return NULL;
    PyObject*kwargs = PyDict_New();
    //PyObject*ret = PyObject_Call(function, args, kwargs);
    PyObject*ret = PyObject_CallObject(function, args);
    Py_DECREF(kwargs);
    Py_DECREF(args);

    if(ret == NULL) {
        handle_exception(li);
        PyErr_Print();
        PyErr_Clear();
        return NULL;
    } else {
        return pyobject_to_value(li, ret);
    }
}

static void define_constant_py(language_t*li, const char*name, value_t*value)
{
    py_internal_t*py = (py_internal_t*)li->internal;
    log_dbg("[python] defining constant %s", name);
    PyDict_SetItem(py->globals, PyString_FromString(name), value_to_pyobject(li, value, false));
}

#if PY_MAJOR_VERSION < 3
#define PYTHON_HEAD_INIT \
    PyObject_HEAD_INIT(NULL) \
    0,
#else
#define PYTHON_HEAD_INIT \
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
#endif

static void functionproxy_dealloc(PyObject* _self) {
    FunctionProxyObject* self = (FunctionProxyObject*)_self;
    if(self->name)
        free(self->name);
    PyObject_Del(self);
}
static PyTypeObject FunctionProxyClass =
{
    PYTHON_HEAD_INIT
    tp_name: "FunctionProxy",
    tp_basicsize: sizeof(FunctionProxyObject),
    tp_itemsize: 0,
    tp_dealloc: functionproxy_dealloc,
};

static void define_function_py(language_t*li, const char*name, function_t*f)
{
    py_internal_t*py_internal = (py_internal_t*)li->internal;

    PyObject*dict = py_internal->globals;

    PyMethodDef*m = calloc(sizeof(PyMethodDef), 1);
    m->ml_name = name;
    m->ml_meth = python_method_proxy;
    m->ml_flags = METH_VARARGS;

    FunctionProxyObject*self = PyObject_New(FunctionProxyObject, &FunctionProxyClass);
    self->name = strdup(name);
    self->function = f;
    self->py_internal = py_internal;

    PyObject*cfunction = PyCFunction_New(m, (PyObject*)self);
    PyDict_SetItemString(dict, name, cfunction);
}

static int py_reference_count = 0;

static bool initialize_py(language_t*li, size_t mem_size)
{
    if(li->internal)
        return true; //already initialized

    log_dbg("[python] initializing interpreter");

    li->internal = calloc(1, sizeof(py_internal_t));
    py_internal_t*py = (py_internal_t*)li->internal;
    py->li = li;

    if(py_reference_count==0) {
        void*old = signal(2, SIG_IGN);
        Py_Initialize();
#if PY_MAJOR_VERSION < 3
        FunctionProxyClass.ob_type = &PyType_Type;
#endif
        signal(2, old);
    }
    py_reference_count++;

    py->globals = PyDict_New();
    py->buffer = malloc(65536);

    py->module = PyImport_AddModule("__main__");
    PyObject* globals = PyModule_GetDict(py->module);

    PyDict_Update(py->globals, globals);

    //globals->ob_type->tp_print(globals, stdout, 0);
    
    PyDict_SetItem(py->globals, PyString_FromString("math"), PyImport_ImportModule("math"));

    /* compile an empty script so Python has a chance to load all the things
       it needs for compiling (encodingsmodule etc.) */
    PyRun_String("None", Py_file_input, py->globals, NULL);

    return true;
}

static void destroy_py(language_t* li)
{
    if(li->internal) {
        py_internal_t*py = (py_internal_t*)li->internal;
        free(py->buffer);
        free(py);
        if(--py_reference_count==0) {
            Py_Finalize();
        }
    }
    free(li);
}

language_t* python_interpreter_new()
{
    language_t * li = calloc(1, sizeof(language_t));
    li->name = "py";
    li->initialize = initialize_py;
    li->compile_script = compile_script_py;
    li->is_function = is_function_py;
    li->call_function = call_function_py;
    li->define_constant = define_constant_py;
    li->define_function = define_function_py;
    li->destroy = destroy_py;
    return li;
}
