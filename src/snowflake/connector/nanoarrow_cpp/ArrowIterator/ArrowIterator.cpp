#include "Python/Common.hpp"

#include <memory>
#include <vector>

#include "CArrowChunkIterator.hpp"
#include "CArrowIterator.hpp"
#include "CArrowTableIterator.hpp"

namespace sf {

// Forward declares.
extern PyModuleDef arrowIteratorModule_def;
struct ArrowIteratorModuleState;

// Helper functions.

// A shim for `Py_NewRef`, until Python 3.9 support is removed.
static PyObject *newRef(PyObject *o) {
  Py_IncRef(o);
  return o;
}

static PyObject *getPyarrow() {
    py::UniqueRef pyarrowModule(PyImport_ImportModule("pyarrow"));
    if (pyarrowModule.get() == nullptr) {
        // No pyarrow. Clear the exception.
        PyErr_Clear();
    }
    return pyarrowModule.get();
}

static int errorHandlerWrapper(PyObject *cursor, const char *errorClassName, const char *errorNumberName, PyObject *msg) {
    // TODO: Relative import.
    py::UniqueRef errorCodeModule(PyImport_ImportModule("snowflake.connector.errorcode"));
    if (errorCodeModule.get() == nullptr) {
        return -1;
    }
    py::UniqueRef errorNumber(PyObject_GetAttrString(errorCodeModule.get(), errorNumberName));
    if (errorNumber.get() == nullptr) {
        return -1;
    }

    // TODO: Relative import.
    py::UniqueRef errorModule(PyImport_ImportModule("snowflake.connector.errors"));
    if (errorModule.get() == nullptr) {
        return -1;
    }

    py::UniqueRef errorClass(PyObject_GetAttrString(errorModule.get(), "Error"));
    if (errorClass.get() == nullptr) {
        return -1;
    }

    py::UniqueRef errorType(PyObject_GetAttrString(errorModule.get(), errorClassName));
    if (errorType.get() == nullptr) {
        return -1;
    }

    py::UniqueRef connection;
    if (cursor != Py_None) {
        connection.reset(PyObject_GetAttrString(cursor, "connection"));
        if (connection.get() == nullptr) {
            return -1;
        }
    } else {
        connection.reset(newRef(Py_None));
    }

    py::UniqueRef errorValue(PyDict_New());
    if (errorValue.get() == nullptr) {
        return -1;
    }
    if (PyDict_SetItemString(errorValue.get(), "msg", msg) < 0) {
        return -1;
    }
    if (PyDict_SetItemString(errorValue.get(), "errno", errorNumber.get()) < 0) {
        return -1;
    }

    py::UniqueRef ret(PyObject_CallMethod(errorClass.get(), "errorhandler_wrapper", "OOOO", connection.get(), cursor, errorType.get(), errorValue.get()));
    if (ret.get() == nullptr) {
        return -1;
    }
    return 0;
}

static ArrowIteratorModuleState *getArrowIteratorModuleState(PyObject *module) {
    void *state = PyModule_GetState(module);
    assert(state != NULL);
    return (ArrowIteratorModuleState *)(state);
}

// Python class structures.

struct EmptyPyArrowIteratorObject {
    PyObject_HEAD
};

// A regular C++ structure, which holds the fields.
struct PyArrowIteratorFields {
    PyArrowIteratorFields() {
        context.reset(newRef(Py_None));
        cursor.reset(newRef(Py_None));
        arrowBytesObject.reset(newRef(Py_None));
        pyarrowTable.reset(newRef(Py_None));
    }

    py::UniqueRef context;
    py::UniqueRef cursor;

    // A reference to the object that arrowBytes points at.
    py::UniqueRef arrowBytesObject;
    char* arrowBytes = nullptr;
    int64_t arrowBytesSize = 0;

    std::unique_ptr<CArrowIterator> cIterator;

    std::vector<uintptr_t> nanoarrowTable;
    std::vector<uintptr_t> nanoarrowSchema;

    bool useDictResult = false;
    bool tableReturned = false;

    // This is the flag indicating whether fetch data as numpy datatypes or not. The flag
    // is passed from the constructor of SnowflakeConnection class. Note, only FIXED, REAL
    // and TIMESTAMP_NTZ will be converted into numpy data types, all other sql types will
    // still be converted into native python types.
    // https://docs.snowflake.com/en/user-guide/sqlalchemy.html#numpy-data-type-support
    bool useNumpy = false;
    bool numberToDecimal = false;
    py::UniqueRef pyarrowTable;
};

struct PyArrowIteratorObject {
    EmptyPyArrowIteratorObject base;
    PyArrowIteratorFields fields;
};

struct PyArrowRowIteratorObject {
    PyArrowIteratorObject base;
};

struct PyArrowTableIteratorObject {
    PyArrowIteratorObject base;
};

struct ArrowIteratorModuleState {
    // Types.
    PyTypeObject* typeEmptyPyArrowIterator;
    PyTypeObject* typePyArrowIterator;
    PyTypeObject* typePyArrowRowIterator;
    PyTypeObject* typePyArrowTableIterator;
};

// Member functions of classes.

static PyObject *EmptyPyArrowIterator_iter(PyObject *self) {
    Py_INCREF(self);
    return self;
}

static PyObject *EmptyPyArrowIterator_next(PyObject *self) {
    PyErr_SetNone(PyExc_StopIteration);
    return nullptr;
}

static PyType_Slot EmptyPyArrowIterator_slots[] = {
    // TODO: Do we need the `EmptyPyArrowIterator.init()` (unrelated to `__init__`)? The pyx one does nothing.
    {Py_tp_iter, (void *)EmptyPyArrowIterator_iter},
    {Py_tp_iternext, (void *)EmptyPyArrowIterator_next},
    {0, nullptr},
};


static PyObject *PyArrowIterator_new(PyTypeObject *type, PyObject *args, PyObject *kwds) {
    allocfunc alloc = (allocfunc)PyType_GetSlot(type, Py_tp_alloc);
    if (alloc == nullptr) {
        return nullptr;
    }
    py::UniqueRef ret(alloc(type, 0));
    if (ret.get() == nullptr) {
        return nullptr;
    }

    PyArrowIteratorObject *self = (PyArrowIteratorObject *)ret.get();
    // Placement `new` to initialize the structure.
    new(&self->fields) PyArrowIteratorFields();

    return ret.release();
}

static int PyArrowIterator_init(PyObject *selfObj, PyObject *args, PyObject *kwargs) {
    static const char *keywordList[] = {
        "cursor",
        "arrow_bytes",
        "arrow_context",
        "use_dict_result",
        "numpy",
        "number_to_decimal",
        nullptr,
    };
    PyObject *cursor = nullptr;
    PyObject *arrowBytesObject = nullptr;
    char *arrowBytes = nullptr;
    Py_ssize_t arrowBytesSize = 0;
    PyObject *arrowContext = nullptr;
    int useDictResult = 0;
    int numpy = 0;
    int numberToDecimal = 0;

    // `O` is a PyObject.
    // `p` is a boolean, 1==true, 0==false.
    // `s#` is a string (start, length) pair.
    const int ret = PyArg_ParseTupleAndKeywords(
        args, kwargs, "OOOppp", const_cast<char **>(keywordList),
        &cursor, &arrowBytesObject, &arrowContext,
        &useDictResult, &numpy, &numberToDecimal);
    if (!ret) {
        return -1;
    }

    py::UniqueRef arrowBytesRef(PyBytes_FromObject(arrowBytesObject));

    if (arrowBytesRef.get() == nullptr) {
        return -1;
    }
    if (PyBytes_AsStringAndSize(arrowBytesRef.get(), &arrowBytes, &arrowBytesSize) < 0) {
        return -1;
    }

    PyArrowIteratorObject *self = (PyArrowIteratorObject *)selfObj;

    self->fields.cursor.reset(newRef(cursor));
    self->fields.context.reset(newRef(arrowContext));
    self->fields.arrowBytesObject = std::move(arrowBytesRef);

    self->fields.arrowBytes = arrowBytes;
    self->fields.arrowBytesSize = arrowBytesSize;

    self->fields.useDictResult = useDictResult;
    self->fields.useNumpy = numpy;
    self->fields.numberToDecimal = numberToDecimal;
    return 0;
}

static void PyArrowIterator_dealloc(PyObject *selfObj) {
    freefunc freefn = (freefunc)PyType_GetSlot(Py_TYPE(selfObj), Py_tp_free);
    // Release all the fields by calling the destructor.
    PyArrowIteratorObject *self = (PyArrowIteratorObject *)selfObj;
    self->fields.~PyArrowIteratorFields();
    freefn(self);
}

//static int PyArrowIterator_traverse(CustomObject *self, visitproc visit, void *arg) {
//    // TODO: List all members that are PyObjects.
//    //Py_VISIT(self->field);
//    return 0;
//}

// TODO: Clear.

// TODO: Can't we inherit our parent's?
static PyObject *PyArrowIterator_iter(PyObject *self) {
    Py_INCREF(self);
    return self;
}

static PyType_Slot PyArrowIterator_slots[] = {
    {Py_tp_new, (void *)PyArrowIterator_new},
    {Py_tp_init, (void *)PyArrowIterator_init},
    {Py_tp_dealloc, (void *)PyArrowIterator_dealloc},
    // TODO: traverse.
    {Py_tp_iter, (void *)PyArrowIterator_iter},
    {0, nullptr},
};

static int PyArrowRowIterator_init(PyObject *selfObj, PyObject *args, PyObject *kwargs) {
    const int ret = PyArrowIterator_init(selfObj, args, kwargs);
    if (ret < 0) {
        return ret;
    }

    PyArrowIteratorObject *self = (PyArrowIteratorObject *)selfObj;
    if (self->fields.cIterator.get() != nullptr) {
        return 0;
    }

    if (self->fields.useDictResult) {
        self->fields.cIterator = std::make_unique<DictCArrowChunkIterator>(
            self->fields.context.get(),
            self->fields.arrowBytes,
            self->fields.arrowBytesSize,
            self->fields.useNumpy
        );
    } else {
        self->fields.cIterator = std::make_unique<CArrowChunkIterator>(
            self->fields.context.get(),
            self->fields.arrowBytes,
            self->fields.arrowBytesSize,
            self->fields.useNumpy
        );
    }
    // TODO: Ownership of things that `cret` points at is unclear.
    ReturnVal cret = self->fields.cIterator->checkInitializationStatus();
    if (cret.exception != nullptr) {
        py::UniqueRef msg(PyUnicode_FromFormat("Failed to open arrow stream: %S", cret.exception));
        if (msg.get() == nullptr) {
            return -1;
        }
        if (errorHandlerWrapper(self->fields.cursor.get(), "OperationalError", "ER_FAILED_TO_READ_ARROW_STREAM", msg.get()) < 0) {
            return -1;
        }
        return -1;
    }

    // TODO: Should we log the number of batches here?

    return 0;
}

static PyObject *PyArrowRowIterator_next(PyObject *selfObj) {
    PyArrowIteratorObject *self = (PyArrowIteratorObject *)selfObj;
    ReturnVal cret = self->fields.cIterator->next();
    if (cret.successObj == nullptr) {
        py::UniqueRef msg(PyUnicode_FromFormat("Failed to convert current row, cause: %S", cret.exception));
        if (msg.get() == nullptr) {
            return nullptr;
        }
        if (errorHandlerWrapper(self->fields.cursor.get(), "InterfaceError", "ER_FAILED_TO_CONVERT_ROW_TO_PYTHON_TYPE", msg.get()) < 0) {
            return nullptr;
        }
        return nullptr;
    }

    // The child class holds onto a reference to the row,
    // but we should use a different reference, in case anything
    // happens to the child.
    py::UniqueRef ret(newRef(cret.successObj));

    if (ret.get() == Py_None) {
        PyErr_SetNone(PyExc_StopIteration);
        return nullptr;
    }
    return ret.release();
}

static PyType_Slot PyArrowRowIterator_slots[] = {
    {Py_tp_init, (void *)PyArrowRowIterator_init},
    {Py_tp_iternext, (void *)PyArrowRowIterator_next},
    {0, nullptr},
};

static int PyArrowTableIterator_init(PyObject *selfObj, PyObject *args, PyObject *kwargs) {
    const int ret = PyArrowIterator_init(selfObj, args, kwargs);
    if (ret < 0) {
        return ret;
    }

    py::UniqueRef pyarrowModule(getPyarrow());

    PyArrowIteratorObject *self = (PyArrowIteratorObject *)selfObj;
    if (pyarrowModule.get() == nullptr) {
        py::UniqueRef msg(PyUnicode_FromString(
            "Optional dependency: 'pyarrow' is not installed, please see the following link for install "
            "instructions: https://docs.snowflake.com/en/user-guide/python-connector-pandas.html#installation"
        ));
        if (msg.get() == nullptr) {
            return -1;
        }
        // TODO: This previously used `raise Error.errorhandler_make_exception(...)`. Is it OK to use `errorhandler_wrapper` instead?
        if (errorHandlerWrapper(self->fields.cursor.get(), "ProgrammingError", "ER_NO_PYARROW", msg.get()) < 0) {
            return -1;
        }
        return -1;
    }

    if (self->fields.cIterator.get() != nullptr) {
        return 0;
    }

    self->fields.cIterator = std::make_unique<CArrowTableIterator>(
        self->fields.context.get(),
        self->fields.arrowBytes,
        self->fields.arrowBytesSize,
        self->fields.numberToDecimal
    );
    ReturnVal cret = self->fields.cIterator->checkInitializationStatus();
    if (cret.exception) {
        py::UniqueRef msg(PyUnicode_FromFormat("Failed to open arrow stream: %S", cret.exception));
        if (msg.get() == nullptr) {
            return -1;
        }
        if (errorHandlerWrapper(self->fields.cursor.get(), "OperationalError", "ER_FAILED_TO_READ_ARROW_STREAM", msg.get()) < 0) {
            return -1;
        }
        return -1;
    }

    ReturnVal cret2 = self->fields.cIterator->next();
    // TODO: Should we care about this result?
    (void)cret2;
    self->fields.nanoarrowTable = self->fields.cIterator->getArrowArrayPtrs();
    self->fields.nanoarrowSchema = self->fields.cIterator->getArrowSchemaPtrs();

    // Create the pyarrow table.
    const size_t batchesLen = self->fields.nanoarrowTable.size();
    self->fields.pyarrowTable.reset(PyList_New(batchesLen));
    PyObject *batches = self->fields.pyarrowTable.get();
    if (batches == nullptr) {
        return -1;
    }

    py::UniqueRef recordBatchClass(PyObject_GetAttrString(pyarrowModule.get(), "RecordBatch"));
    if (recordBatchClass.get() == nullptr) {
        return -1;
    }
    for (size_t i = 0; i < batchesLen; ++i) {
        // Get pyarrow.
        const uintptr_t table = self->fields.nanoarrowTable[i];
        const uintptr_t schema = self->fields.nanoarrowSchema[i];

        py::UniqueRef batch(PyObject_CallMethod(recordBatchClass.get(), "_import_from_c", "nn", table, schema));
        if (batch.get() == nullptr) {
            return -1;
        }
        const int ret = PyList_SetItem(batches, i, batch.release());
        (void)ret;
        assert(ret == 0);
    }

    py::UniqueRef tableClass(PyObject_GetAttrString(pyarrowModule.get(), "Table"));
    if (tableClass.get() == nullptr) {
        return -1;
    }

    self->fields.pyarrowTable.reset(PyObject_CallMethod(tableClass.get(), "from_batches", "O", batches));
    if (self->fields.pyarrowTable.get() == nullptr) {
        return -1;
    }
    // TODO: Should we log the number of batches here?
    return 0;
}

static PyObject *PyArrowTableIterator_next(PyArrowIteratorObject *self) {
    if (self->fields.tableReturned == false) {
        self->fields.tableReturned = true;
        return newRef(self->fields.pyarrowTable.get());
    }
    PyErr_SetNone(PyExc_StopIteration);
    return nullptr;
}

static PyType_Slot PyArrowTableIterator_slots[] = {
    {Py_tp_init, (void *)PyArrowTableIterator_init},
    {Py_tp_iternext, (void *)PyArrowTableIterator_next},
    {0, nullptr},
};


// Class specs.

static PyType_Spec EmptyPyArrowIterator_spec {
    /*name=*/"snowflake.connector.nanoarrow_arrow_iterator.EmptyPyArrowIterator",
    /*basicsize=*/sizeof(EmptyPyArrowIteratorObject),
    /*itemsize=*/0,
    // TODO: Py_TPFLAGS_HAVE_GC here and elsewhere.
    /*flags=*/Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
    /*slots=*/EmptyPyArrowIterator_slots,
};

static PyType_Spec PyArrowIterator_spec {
    /*name=*/"snowflake.connector.nanoarrow_arrow_iterator.PyArrowIterator",
    /*basicsize=*/sizeof(PyArrowIteratorObject),
    /*itemsize=*/0,
    /*flags=*/Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
    /*slots=*/PyArrowIterator_slots,
};

static PyType_Spec PyArrowRowIterator_spec {
    /*name=*/"snowflake.connector.nanoarrow_arrow_iterator.PyArrowRowIterator",
    /*basicsize=*/sizeof(PyArrowRowIteratorObject),
    /*itemsize=*/0,
    /*flags=*/Py_TPFLAGS_DEFAULT,
    /*slots=*/PyArrowRowIterator_slots,
};


static PyType_Spec PyArrowTableIterator_spec {
    /*name=*/"snowflake.connector.nanoarrow_arrow_iterator.PyArrowTableIterator",
    /*basicsize=*/sizeof(PyArrowTableIteratorObject),
    /*itemsize=*/0,
    /*flags=*/Py_TPFLAGS_DEFAULT,
    /*slots=*/PyArrowTableIterator_slots,
};

static PyMethodDef arrowIteratorModuleMethods[] = {
    {nullptr, nullptr, 0, nullptr},
};

static int arrow_iterator_module_exec(PyObject *m) {
    const auto createType = [m](const char *name, PyType_Spec &spec, PyObject *base) -> py::UniqueRef {
        // After removing support for Python 3.9, we should be
        // able to pass `&base` directly. Until then, we need
        // to create a 1-item tuple.
        py::UniqueRef basesTuple;
        if (base != nullptr) {
            basesTuple.reset(PyTuple_New(/*len=*/1));
            if (basesTuple.get() == nullptr) {
                return py::UniqueRef();
            }
            // Note that `PyTuple_SetItem` steals a reference to `base`.
            Py_INCREF(base);
            const int ret = PyTuple_SetItem(basesTuple.get(), 0, base);
            (void)ret;
            assert(ret == 0);
        }

        // After removing support for Python 3.9, we should use
        // `PyType_FromModuleAndSpec` here.
        py::UniqueRef newType(PyType_FromSpecWithBases(&spec, basesTuple.get()));
        if (newType.get() == nullptr) {
            return py::UniqueRef();
        }

        // Register the type with the module. Note that `PyModule_AddObject`
        // steals ownership on success, so we increment the refcount. After
        // removing support for Python 3.9, we can use `PyModule_AddObjectRef`
        // in the limited API, which lets us skip some of this refcount
        // handling.
        Py_INCREF(newType.get());
        if (PyModule_AddObject(m, name, newType.get()) < 0) {
            Py_DECREF(newType.get());
            return py::UniqueRef();
        }
        return newType;
    };

    py::UniqueRef typeEmptyPyArrowIterator = createType("EmptyPyArrowIterator", EmptyPyArrowIterator_spec, nullptr);
    if (typeEmptyPyArrowIterator.get() == nullptr) {
      return -1;
    }
    py::UniqueRef typePyArrowIterator = createType("PyArrowIterator", PyArrowIterator_spec, typeEmptyPyArrowIterator.get());
    if (typePyArrowIterator.get() == nullptr) {
      return -1;
    }
    py::UniqueRef typePyArrowRowIterator = createType("PyArrowRowIterator", PyArrowRowIterator_spec, typePyArrowIterator.get());
    if (typePyArrowRowIterator.get() == nullptr) {
      return -1;
    }
    py::UniqueRef typePyArrowTableIterator = createType("PyArrowTableIterator", PyArrowTableIterator_spec, typePyArrowIterator.get());
    if (typePyArrowTableIterator.get() == nullptr) {
      return -1;
    }

    // NOTE: After this point, we can no longer return an error,
    // since things might not be cleaned up.

    // TODO: Hook up.
    ArrowIteratorModuleState *state = getArrowIteratorModuleState(m);

    // Initialize types.
    state->typeEmptyPyArrowIterator = (PyTypeObject *)typeEmptyPyArrowIterator.release();
    state->typePyArrowIterator = (PyTypeObject *)typePyArrowIterator.release();
    state->typePyArrowRowIterator = (PyTypeObject *)typePyArrowRowIterator.release();
    state->typePyArrowTableIterator = (PyTypeObject *)typePyArrowTableIterator.release();

    return 0;
}

static PyModuleDef_Slot arrowIteratorModule_slots[] = {
    {Py_mod_exec, (void *)arrow_iterator_module_exec},
    {0, nullptr},
};

// TODO: Module traverse.
PyModuleDef arrowIteratorModule_def = {
    /*m_base=*/PyModuleDef_HEAD_INIT,
    /*m_name=*/"nanoarrow_arrow_iterator",
    /*m_doc=*/nullptr,
    /*m_size=*/sizeof(ArrowIteratorModuleState),
    /*m_methods=*/arrowIteratorModuleMethods,
    /*m_slots=*/arrowIteratorModule_slots,
};

}

PyMODINIT_FUNC
PyInit_nanoarrow_arrow_iterator(void)
{
    return PyModuleDef_Init(&sf::arrowIteratorModule_def);
}