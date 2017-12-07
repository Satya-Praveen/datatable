//------------------------------------------------------------------------------
//  Copyright 2017 H2O.ai
//
//  Licensed under the Apache License, Version 2.0 (the "License");
//  you may not use this file except in compliance with the License.
//  You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
//  Unless required by applicable law or agreed to in writing, software
//  distributed under the License is distributed on an "AS IS" BASIS,
//  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
//  See the License for the specific language governing permissions and
//  limitations under the License.
//------------------------------------------------------------------------------
#include "utils/pyobj.h"
#include "py_datatable.h"
#include "py_types.h"
#include "utils/exceptions.h"


PyObj::PyObj() {
  obj = nullptr;
  tmp = nullptr;
}

PyObj::PyObj(PyObject* o) {
  obj = o;
  tmp = nullptr;
  if (!obj) throw PyError();
  Py_INCREF(o);
}

PyObj::PyObj(PyObject* o, const char* attr) {
  // `PyObject_GetAttrString` returns a new reference
  obj = PyObject_GetAttrString(o, attr);
  tmp = nullptr;
  if (!obj) throw PyError();
}

PyObj::PyObj(const PyObj& other) {
  obj = other.obj;
  tmp = nullptr;
  Py_INCREF(obj);
}

PyObj::PyObj(PyObj&& other) {
  obj = other.obj;
  tmp = other.tmp;
  other.obj = nullptr;
  other.tmp = nullptr;
}

PyObj& PyObj::operator=(const PyObj& other) {
  if (obj || tmp) {
    throw RuntimeError()
      << "Cannot assign to PyObj: it already contains a PyObject " << obj
      << " [tmp=" << tmp << "]";
  }
  obj = other.obj;
  tmp = other.tmp;
  Py_XINCREF(obj);
  Py_XINCREF(tmp);
  return *this;
}

PyObj::~PyObj() {
  Py_XDECREF(obj);
  Py_XDECREF(tmp);
}

PyObj PyObj::fromPyObjectNewRef(PyObject* t) {
  PyObj res(t);
  Py_XDECREF(t);
  return res;
}


// Note from http://en.cppreference.com/w/cpp/language/copy_elision:
//
//     If a function returns a class type by value, and the return statement's
//     expression is the name of a non-volatile object with automatic storage
//     duration, which isn't the function parameter, or a catch clause
//     parameter, and which has the same type (ignoring top-level
//     cv-qualification) as the return type of the function, then copy/move
//     (since C++11) is omitted. When that local object is constructed, it is
//     constructed directly in the storage where the function's return value
//     would otherwise be moved or copied to. This variant of copy elision is
//     known as NRVO, "named return value optimization".
//
PyObj PyObj::attr(const char* attrname) const {
  return PyObj(obj, attrname);
}


PyObj PyObj::invoke(const char* fn, const char* format, ...) const {
  if (!obj) throw RuntimeError() << "Cannot invoke an empty PyObj";
  PyObject* callable = nullptr;
  PyObject* args = nullptr;
  PyObject* res = nullptr;
  do {
    callable = PyObject_GetAttrString(obj, fn);  // new ref
    if (!callable) break;
    va_list va;
    va_start(va, format);
    args = Py_VaBuildValue(format, va);  // new ref
    va_end(va);
    if (!args) break;
    res = PyObject_CallObject(callable, args);  // new ref
  } while (0);
  Py_XDECREF(callable);
  Py_XDECREF(args);
  if (!res) throw PyError();
  return PyObj::fromPyObjectNewRef(res);
}


int8_t PyObj::as_bool() const {
  if (obj == Py_True) return 1;
  if (obj == Py_False) return 0;
  if (obj == Py_None) return GETNA<int8_t>();
  throw ValueError() << "Value " << obj << " is not boolean";
}


int64_t PyObj::as_int64() const {
  if (PyLong_Check(obj)) return PyLong_AsInt64(obj);
  if (obj == Py_None) return GETNA<int64_t>();
  throw ValueError() << "Value " << obj << " is not integer";
}


int32_t PyObj::as_int32() const {
  return static_cast<int32_t>(as_int64());
}


double PyObj::as_double() const {
  if (PyFloat_Check(obj)) return PyFloat_AsDouble(obj);
  if (obj == Py_None) return GETNA<double>();
  if (PyLong_Check(obj)) {
    double res = PyLong_AsDouble(obj);
    if (res == -1 && PyErr_Occurred()) throw PyError();
    return res;
  }
  throw ValueError() << "Value " << obj << " is not a double";
}


const char* PyObj::as_cstring() const {
  if (!obj) throw ValueError() << "PyObj() was not initialized properly";
  if (PyUnicode_Check(obj)) {
    if (tmp) throw RuntimeError() << "Cannot convert to string more than once";
    tmp = PyUnicode_AsEncodedString(obj, "utf-8", "strict");
    return PyBytes_AsString(tmp);
  }
  if (PyBytes_Check(obj)) return PyBytes_AsString(obj);
  if (obj == Py_None) return nullptr;
  throw ValueError() << "Value " << obj << " is not a string";
}


std::string PyObj::as_string() const {
  const char* src = as_cstring();
  return std::string(src? src : "");
}


char* PyObj::as_ccstring() const {
  const char* src = as_cstring();
  if (!src) return nullptr;
  size_t len = strlen(src);
  char* newbuf = new char[len + 1];
  memcpy(newbuf, src, len + 1);
  return newbuf;
}


char PyObj::as_char() const {
  const char* src = as_cstring();
  return src? src[0] : '\0';
}


PyObject* PyObj::as_pyobject() const {
  Py_XINCREF(obj);
  return obj;
}


DataTable* PyObj::as_datatable() const {
  DataTable* dt = nullptr;
  int ret = dt_unwrap(obj, &dt);
  if (!ret) throw Error();
  return dt;
}


std::vector<std::string> PyObj::as_stringlist() const {
  std::vector<std::string> res;
  if (PyList_Check(obj) || PyTuple_Check(obj)) {
    PyObject** items = PyList_Check(obj)
        ? reinterpret_cast<PyListObject*>(obj)->ob_item
        : reinterpret_cast<PyTupleObject*>(obj)->ob_item;
    Py_ssize_t count = Py_SIZE(obj);
    res.reserve(static_cast<size_t>(count));
    for (Py_ssize_t i = 0; i < count; ++i) {
      PyObject* item = items[i];
      if (PyUnicode_Check(item)) {
        PyObject* y = PyUnicode_AsEncodedString(item, "utf-8", "strict");
        if (!y) throw PyError();
        res.push_back(PyBytes_AsString(y));
        Py_DECREF(y);
      } else
      if (PyBytes_Check(item)) {
        res.push_back(PyBytes_AsString(item));
      } else {
        throw TypeError() << "Item " << i << " in the list is not a string: "
                          << item << " (" << PyObject_Type(item) << ")";
      }
    }
  } else if (obj != Py_None) {
    throw TypeError() << "A list of strings is expected, got " << obj;
  }
  return res;
}


char** PyObj::as_cstringlist() const {
  if (obj == Py_None) {
    return nullptr;
  }
  if (PyList_Check(obj) || PyTuple_Check(obj)) {
    Py_ssize_t count = Py_SIZE(obj);
    PyObject** items = PyList_Check(obj)
        ? reinterpret_cast<PyListObject*>(obj)->ob_item
        : reinterpret_cast<PyTupleObject*>(obj)->ob_item;
    char** res = nullptr;
    try {
      res = new char*[count + 1];
      for (Py_ssize_t i = 0; i <= count; ++i) res[i] = nullptr;
      for (Py_ssize_t i = 0; i < count; ++i) {
        PyObject* item = items[i];
        if (PyUnicode_Check(item)) {
          PyObject* y = PyUnicode_AsEncodedString(item, "utf-8", "strict");
          if (!y) throw PyError();
          size_t len = static_cast<size_t>(PyBytes_Size(y));
          res[i] = new char[len + 1];
          memcpy(res[i], PyBytes_AsString(y), len + 1);
          Py_DECREF(y);
        } else
        if (PyBytes_Check(item)) {
          size_t len = static_cast<size_t>(PyBytes_Size(item));
          res[i] = new char[len + 1];
          memcpy(res[i], PyBytes_AsString(item), len + 1);
        } else {
          throw TypeError() << "Item " << i << " in the list is not a string: "
                            << item << " (" << PyObject_Type(item) << ")";
        }
      }
      return res;
    } catch (...) {
      // Clean-up `res` before re-throwing the exception
      for (Py_ssize_t i = 0; i < count; ++i) delete[] res[i];
      delete[] res;
      throw;
    }
  }
  throw TypeError() << "A list of strings is expected, got " << obj;
}


void PyObj::print() {
  PyObject* s = PyObject_Repr(obj);
  printf("%s\n", PyUnicode_AsUTF8(s));
  Py_XDECREF(s);
}