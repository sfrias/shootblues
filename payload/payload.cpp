#include <windows.h>
#include "C:\python27\include\python.h"

enum RPCMessageType : unsigned int {
  RMT_Run = 0,
  RMT_AddModule = 1,
  RMT_RemoveModule = 2,
  RMT_ReloadModules = 3,
  RMT_CallFunction = 4
};

#pragma pack(push)
#pragma pack(1)
struct RPCMessage {
  RPCMessageType type;
  unsigned int messageId;
  const char * moduleName;
  const char * functionName;
  const char * text;
};
#pragma pack(pop)

typedef BOOL (WINAPI *pChangeWindowMessageFilter) (
  __in UINT message, __in DWORD dwFlag
);

static HWND g_rpcWindow;
static int g_rpcMessageId;

static PyObject * g_excException;
static PyObject * g_excValueError;
static PyObject * g_excImportError;
static PyObject * g_sysModule;
static PyObject * g_tracebackModule;
static PyObject * g_jsonModule;
static PyObject * g_module;
static PyObject * g_moduleDict;
static PyObject * g_unloadedModules;
static PyObject * g_typeNamedChannel;

PyObject * rpcSend (PyObject * self, PyObject * args, PyObject * kwargs) {
  static char * kwlist[] = {"messageBody", "id", NULL};
  char * messageBody = 0;
  int messageBodyLen = 0;
  unsigned int messageId = 0;

  if (!PyArg_ParseTupleAndKeywords(args, kwargs, "z#|I", kwlist, &messageBody, &messageBodyLen, &messageId))
    return NULL;

  // Allocate enough memory to hold our message body and store it there
  size_t regionSize = 4;
  if (messageBody)
    regionSize += messageBodyLen + 1;

  LPVOID region = VirtualAlloc(0, regionSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
  if (region) {
    *reinterpret_cast<unsigned int *>(region) = messageId;
    if (messageBody)
      strcpy((char *)region + sizeof(messageId), messageBody);

    HWND rpcWindow = g_rpcWindow;
    if (self) {
      PyObject * rpcWindowObj = PyObject_GetAttrString(self, "__hwnd__");
      if (rpcWindowObj) {
        rpcWindow = reinterpret_cast<HWND>(PyLong_AsVoidPtr(rpcWindowObj));
        Py_DECREF(rpcWindowObj);
      } else {
        PyErr_Clear();
      }
    }

    // Post a message to our parent process (it will free our memory region once it gets the message)
    if (PostMessage(
      rpcWindow, g_rpcMessageId, 
      reinterpret_cast<WPARAM>(region), 
      *reinterpret_cast<LPARAM *>(&regionSize)
    )) {
      return Py_BuildValue("");
    } else {
      VirtualFree(region, 0, MEM_RELEASE);
    }
  }

  PyErr_SetFromWindowsErr(GetLastError());
  return NULL;
}

void pyRpcSend (PyObject * messageBody, int messageId) {
  PyObject * args = PyTuple_Pack(1, messageBody);
  PyObject * kwargs = Py_BuildValue("{s,I}", "id", messageId);
  PyObject * result = rpcSend(0, args, kwargs);
  Py_XDECREF(result);
  Py_XDECREF(args);
  Py_XDECREF(kwargs);
}

bool errorHandler (int messageId) {
  PyObject * errType = 0, * errValue = 0, * traceback = 0;
  PyErr_Fetch(&errType, &errValue, &traceback);
  if (!errType)
    errType = Py_BuildValue("");
  if (!errValue)
    errValue = Py_BuildValue("");
  if (!traceback)
    traceback = Py_BuildValue("");

  PyObject * format_exception = PyObject_GetAttrString(g_tracebackModule, "format_exception");
  PyObject * args = PyTuple_Pack(3, errType, errValue, traceback);
  PyObject * exception = PyObject_CallObject(format_exception, args);
  Py_XDECREF(args);

  if (exception) {
    PyObject * separator = PyString_FromString("");
    PyObject * exceptionString = _PyString_Join(separator, exception);
    Py_XDECREF(separator);
    Py_XDECREF(exception);
    if (exceptionString) {
      pyRpcSend(exceptionString, messageId);
      Py_XDECREF(exceptionString);
    }
  } else {
    PyObject * exceptionString = PyString_FromString("errorHandler called with no error set! WTF.");
    pyRpcSend(exceptionString, messageId);
    Py_XDECREF(exceptionString);
  }

  Py_XDECREF(args);
  Py_XDECREF(format_exception);
  Py_XDECREF(errType);
  Py_XDECREF(errValue);
  Py_XDECREF(traceback);

  PyErr_Clear();
  return true;
}

void callFunction (const char * moduleName, const char * functionName, const char * argumentsJson, unsigned int messageId) {
  PyObject * fullname = PyString_FromFormat("shootblues.%s", moduleName);
  PyObject * module = PyImport_Import(fullname);
  Py_DECREF(fullname);
  if (!module) {
    errorHandler(messageId);
    return;
  }

  PyObject * function = PyObject_GetAttrString(module, functionName);
  if (!function) {
    Py_DECREF(module);
    errorHandler(messageId);
    return;
  }

  PyObject * args;
  if (!argumentsJson) {
    args = NULL;
  } else {
    PyObject * loads = PyObject_GetAttrString(g_jsonModule, "loads");
    PyObject * argsString = PyString_FromString(argumentsJson);
    args = PyTuple_Pack(1, argsString);
    PyObject * result = PyObject_CallObject(loads, args);
    Py_XDECREF(loads);
    Py_XDECREF(args);
    Py_XDECREF(argsString);
    if (!result) {
      Py_XDECREF(module);
      Py_XDECREF(function);
      errorHandler(messageId);
      return;
    }
    args = PySequence_Tuple(result);
    Py_XDECREF(result);
  }

  PyObject * result = PyObject_CallObject(function, args);

  if (result) {
    PyObject * dumps = PyObject_GetAttrString(g_jsonModule, "dumps");
    Py_XDECREF(args);
    args = PyTuple_Pack(1, result);
    PyObject * resultJson = PyObject_CallObject(dumps, args);
    Py_XDECREF(dumps);
    Py_XDECREF(args);
    pyRpcSend(resultJson, messageId);
    Py_DECREF(resultJson);
  } else
    errorHandler(messageId);

  Py_XDECREF(args);
  Py_XDECREF(result);
  Py_DECREF(function);
  Py_DECREF(module);
}

void runString (const char * script, unsigned int messageId) {
  PyCodeObject * codeObject = (PyCodeObject *)Py_CompileStringFlags(
    script, "shootblues", Py_file_input, 0
  );

  if (codeObject) {
    PyObject * module = PyImport_AddModule("__main__");
    if (module != NULL) {
      PyObject * globals = PyModule_GetDict(module);
      PyObject * result = PyEval_EvalCode(codeObject, globals, globals);
      Py_XDECREF(result);

      if (PyErr_Occurred())
        errorHandler(messageId);
    } 

    Py_DECREF(codeObject);
  }
}

PyObject * run (PyObject * self, PyObject * args, PyObject * kwargs) {
  static char * kwlist[] = {"script", "id", NULL};
  char * script = 0;
  unsigned int messageId = 0;
  if (!PyArg_ParseTupleAndKeywords(args, kwargs, "s|I", kwlist, &script, &messageId))
    return NULL;

  runString(script, messageId);

  return Py_BuildValue("");
}

int addModuleString (const char * moduleName, const char * script, int messageId) {
  PyObject * scriptString = PyString_FromString(script);
  int result = PyDict_SetItemString(g_moduleDict, moduleName, scriptString);
  Py_DECREF(scriptString);

  if (messageId) {
    if (result != 0) {
      errorHandler(messageId);
    } else {
      PyObject * rcode = PyString_FromFormat("%i", result);
      pyRpcSend(rcode, messageId);
      Py_DECREF(rcode);
    }
  }

  return result;
}

int removeModuleString (const char * moduleName, int messageId) {
  PyObject * nameString = PyString_FromString(moduleName);
  PyList_Append(g_unloadedModules, nameString);
  Py_DECREF(nameString);

  int result = PyDict_DelItemString(g_moduleDict, moduleName);

  if (messageId) {
    if (result != 0) {
      errorHandler(messageId);
    } else {
      PyObject * rcode = PyString_FromFormat("%i", result);
      pyRpcSend(rcode, messageId);
      Py_DECREF(rcode);
    }
  }

  return result;
}

void reloadModulesWithId (int messageId) {
  PyObject * moduleNames = PyMapping_Keys(g_moduleDict);
  PyObject * sysModules = PyObject_GetAttrString(g_sysModule, "modules");

  PyObject * sequences[] = {
    moduleNames, g_unloadedModules, 0
  };

  PyObject * iter = 0;
  bool failed = false;

  // Call all modules' __unload__ handler if present
  for (PyObject ** currentSequence = sequences; *currentSequence != 0; currentSequence++) {
    iter = PyObject_GetIter(*currentSequence);

    while (PyObject * name = PyIter_Next(iter)) {
      PyObject * fullname = PyString_FromFormat("shootblues.%s", PyString_AsString(name));

      PyObject * existingModule = 0;

      if (PyMapping_HasKey(sysModules, fullname))
        existingModule = PyObject_GetItem(sysModules, fullname);
      if (!existingModule && PyObject_HasAttr(g_module, name))
          existingModule = PyObject_GetAttr(g_module, name);

      // If the module was previously loaded, try and call its __unload__ handler
      if (existingModule) {
        if (PyObject_HasAttrString(existingModule, "__unload__")) {
          PyObject * unloadHandler = PyObject_GetAttrString(existingModule, "__unload__");
          PyObject * result = PyObject_CallObject(unloadHandler, NULL);
          if (!result)
            failed = errorHandler(0);
          else
            Py_DECREF(result);
          Py_DECREF(unloadHandler);
        }
        Py_DECREF(existingModule);
      }

      Py_DECREF(fullname);
      Py_DECREF(name);
    }
    Py_DECREF(iter);
  }

  // Remove modules now that we've called the unload handler
  for (PyObject ** currentSequence = sequences; *currentSequence != 0; currentSequence++) {
    iter = PyObject_GetIter(*currentSequence);

    while (PyObject * name = PyIter_Next(iter)) {
      PyObject * fullname = PyString_FromFormat("shootblues.%s", PyString_AsString(name));

      PyObject * existingModule = 0;

      if (PyMapping_HasKey(sysModules, fullname)) {
        existingModule = PyObject_GetItem(sysModules, fullname);
        PyMapping_DelItem(sysModules, fullname);
      }
      if (PyObject_HasAttr(g_module, name)) {
        if (!existingModule)
          existingModule = PyObject_GetAttr(g_module, name);
        PyObject_DelAttr(g_module, name);
      }

      if (PyErr_Occurred())
        failed = errorHandler(0);

      Py_DECREF(fullname);
      Py_DECREF(name);
    }
    Py_DECREF(iter);
  }

  Py_XDECREF(sysModules);
  PyObject * newUnloadedModules = PyList_New(0);
  PyObject_SetAttrString(g_module, "unloadedModules", newUnloadedModules);
  Py_DECREF(g_unloadedModules);
  g_unloadedModules = newUnloadedModules;

  PyObject * loadedModules = PyList_New(0);

  // Import all modules
  iter = PyObject_GetIter(moduleNames);
  while (PyObject * name = PyIter_Next(iter)) {
    PyObject * fullname = PyString_FromFormat("shootblues.%s", PyString_AsString(name));
    PyObject * module = PyImport_Import(fullname);

    if (module) {
      PyList_Append(loadedModules, name);
      Py_DECREF(module);
    } else
      failed = errorHandler(0);

    Py_DECREF(fullname);
    Py_DECREF(name);
  }
  Py_DECREF(iter);
  Py_DECREF(moduleNames);

  if (messageId) {
    PyObject * dumps = PyObject_GetAttrString(g_jsonModule, "dumps");
    PyObject * args = PyTuple_Pack(1, loadedModules);
    PyObject * modulesJson = PyObject_CallObject(dumps, args);
    Py_XDECREF(dumps);
    Py_DECREF(args);
    pyRpcSend(modulesJson, messageId);
    Py_XDECREF(modulesJson);
  }
  
  Py_DECREF(loadedModules);
}

PyObject * findModule (PyObject * self, PyObject * args, PyObject * kwargs) {
  static char * kwlist[] = {"fullname", "path", NULL};
  char * moduleName = 0;
  int moduleNameLen = 0;
  PyObject * path = 0;
  if (!PyArg_ParseTupleAndKeywords(args, kwargs, "s#|O", kwlist, &moduleName, &moduleNameLen, &path))
    return NULL;

  if ((moduleNameLen < 11) || memcmp(moduleName, "shootblues.", 11) != 0)
    return Py_BuildValue("");

  if (PyMapping_HasKeyString(g_moduleDict, moduleName + 11)) {
    Py_INCREF(g_module);
    return g_module;
  } else
    return Py_BuildValue("");
}

PyObject * loadModule (PyObject * self, PyObject * args) {
  // Inexplicably HasKeyString takes char* and not const char*
  char *moduleName;
  int moduleNameLen;
  if (!PyArg_ParseTuple(args, "s#", &moduleName, &moduleNameLen))
    return NULL;

  if ((moduleNameLen < 11) || memcmp(moduleName, "shootblues.", 11) != 0) {
    PyErr_SetString(g_excImportError, "load_module can only load child modules of shootblues");
    return NULL;
  }

  PyObject * scriptText = PyMapping_GetItemString(g_moduleDict, moduleName + 11);
  if (!scriptText) {
    PyErr_SetString(g_excImportError, "Module not found");
    return NULL;
  }

  PyObject * sysModules = PyObject_GetAttrString(g_sysModule, "modules");
  if (sysModules && PyMapping_HasKeyString(sysModules, moduleName)) {
    PyObject * result = PyMapping_GetItemString(sysModules, moduleName);
    return result;
  }
  Py_XDECREF(sysModules);

  PyObject * codeObject = Py_CompileStringFlags(
    PyString_AsString(scriptText), moduleName + 11, Py_file_input, 0
  );
  Py_XDECREF(scriptText);

  if (codeObject) {
    PyObject * module = PyImport_ExecCodeModuleEx(moduleName, codeObject, moduleName + 11);

    if (module != NULL) {
      PyObject_SetAttrString(module, "__loader__", g_module);
      PyObject_SetAttrString(module, "__path__", PyString_FromString(moduleName + 11));
      PyObject_SetAttrString(module, "__package__", PyString_FromString("shootblues"));
      Py_DECREF(codeObject);
    } else {
      Py_DECREF(codeObject);
      return NULL;
    }

    Py_INCREF(module);
    return module;
  } else {
    return NULL;
  }
}

PyObject * createRpcChannel (PyObject * self, PyObject * args) {
  unsigned long windowHandle;
  if (!PyArg_ParseTuple(args, "k", &windowHandle))
    return NULL;

  HWND hWnd = reinterpret_cast<HWND>(windowHandle);
  if (!IsWindow(hWnd)) {
    PyErr_SetString(g_excValueError, "Invalid window handle");
    return NULL;
  }

  static PyMethodDef sendMethod = {
    "send", (PyCFunction)rpcSend, METH_KEYWORDS, "Send an RPC message via the channel."
  };  
  
  PyObject * obj = PyObject_CallObject(g_typeNamedChannel, NULL);
  PyObject * val = PyLong_FromVoidPtr(hWnd);
  PyObject_SetAttrString(obj, "__hwnd__", val);
  Py_DECREF(val);
  val = PyCFunction_NewEx(&sendMethod, obj, NULL);
  PyObject_SetAttrString(obj, "send", val);
  Py_DECREF(val);

  return obj;
}

PyObject * noOp (PyObject * self, PyObject * args) {
  return Py_BuildValue("");
}

static PyMethodDef PythonMethods[] = {
  {"rpcSend", (PyCFunction)rpcSend, METH_KEYWORDS, "Send an RPC message to the parent process."},
  {"run", (PyCFunction)run, METH_KEYWORDS, "Compiles and runs a script block."},
  {"createChannel", createRpcChannel, METH_VARARGS, "Creates an RPC response channel."},
  {"find_module", (PyCFunction)findModule, METH_KEYWORDS, "Implements the Finder protocol (PEP 302)."},
  {"load_module", loadModule, METH_VARARGS, "Implements the Loader protocol (PEP 302)."},
  {"Dependency", noOp, METH_VARARGS, "Specifies that your script depends on another script to function, so that it will be loaded before your script."},
  {"OptionalDependency", noOp, METH_VARARGS, "Specifies that your script benefits from another script's presence, so that it will be loaded before your script."},
  {NULL, NULL, 0, NULL}
};

int __cdecl EnsureGILCreated (void * arg) {
  // This is supposed to acquire the GIL, but because AddPendingCall ensures that
  //  we already have the GIL, we don't need to release it here... confusing
  PyEval_InitThreads();
  return 0;
}

DWORD __stdcall payload (HWND rpcWindow) {
  g_rpcWindow = rpcWindow;
  g_rpcMessageId = RegisterWindowMessage(L"ShootBlues.RPCMessage");

  MSG msg;
  // Create our thread message queue
  PeekMessage(&msg, 0, g_rpcMessageId, g_rpcMessageId, 0);

  HMODULE hModule;

  // Allow our thread to receive RPC messages from processes of lower integrity level
  {
    hModule = LoadLibrary(L"user32.dll");

    pChangeWindowMessageFilter pFn = (pChangeWindowMessageFilter)GetProcAddress(hModule, "ChangeWindowMessageFilter");
    if (pFn)
      pFn(g_rpcMessageId, MSGFLT_ADD);

    FreeLibrary(hModule);
  }  

  // Post a null message to alert the parent process that we are alive
  PostMessage(rpcWindow, g_rpcMessageId, 0, 0);

  // Ensure that we do not load python before the host process has, because that can cause a crash
  while (GetModuleHandleEx(
    GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, L"python27.dll", &hModule
  ) == 0)
    Sleep(100);

  // Wait for an initialized python environment in our host process
  while (!Py_IsInitialized())
    Sleep(100);

  // If the threading lock has not been created, we ask python to create it at the earliest possibility
  if (!PyEval_ThreadsInitialized())
    Py_AddPendingCall(EnsureGILCreated, 0);

  // Wait for the threading lock to be created so that our attempts to acquire the GIL later
  //  will not fail or crash
  while (!PyEval_ThreadsInitialized())
    Sleep(100);

  // Initialize our python extensions
  PyGILState_STATE gil = PyGILState_Ensure();

  PyObject * exceptionsModule = PyImport_ImportModule("exceptions");
  g_excException = PyObject_GetAttrString(exceptionsModule, "Exception");
  g_excValueError = PyObject_GetAttrString(exceptionsModule, "ValueError");
  g_excImportError = PyObject_GetAttrString(exceptionsModule, "ImportError");

  g_module = Py_InitModule3("shootblues", PythonMethods, "Shoot Blues");
  // Setting these three values causes our find_module hook to be invoked for child modules :|
  PyObject_SetAttrString(g_module, "__package__", PyString_FromString("shootblues"));
  PyObject_SetAttrString(g_module, "__file__", PyString_FromString("shootblues"));
  PyObject_SetAttrString(g_module, "__path__", PyString_FromString("shootblues"));

  // Initialize our custom type
  g_typeNamedChannel = PyClass_New(NULL, PyDict_New(), PyString_FromString("NamedChannel"));
  PyObject_SetAttrString(g_typeNamedChannel, "__module__", PyString_FromString("shootblues"));
  PyModule_AddObject(g_module, "NamedChannel", g_typeNamedChannel);

  g_moduleDict = PyDict_New();
  PyObject_SetAttrString(g_module, "modules", g_moduleDict);
  g_unloadedModules = PyList_New(0);
  PyObject_SetAttrString(g_module, "unloadedModules", g_unloadedModules);

  // Install our import hook
  g_sysModule = PyImport_ImportModule("sys");
  PyObject * metaPath = PyObject_GetAttrString(g_sysModule, "meta_path");
  PyList_Append(metaPath, g_module);
  Py_DECREF(metaPath);

  g_jsonModule = PyImport_ImportModule("json");
  g_tracebackModule = PyImport_ImportModule("traceback");

  PyGILState_Release(gil);

  BOOL result;
  while ((result = GetMessage(&msg, 0, g_rpcMessageId, g_rpcMessageId)) != 0) {
    // We've been told to terminate our message queue
    if (result == -1)
      break;

    RPCMessage * rpc = reinterpret_cast<RPCMessage *>(msg.wParam);
    DWORD rpcSize = *reinterpret_cast<DWORD *>(&(msg.lParam));

    PyGILState_STATE gil = PyGILState_Ensure();

    switch (rpc->type) {
      case RMT_Run:
        runString(rpc->text, rpc->messageId);
        break;
      case RMT_AddModule:
        addModuleString(rpc->moduleName, rpc->text, rpc->messageId);
        break;
      case RMT_RemoveModule:
        removeModuleString(rpc->moduleName, rpc->messageId);
        break;
      case RMT_ReloadModules:
        reloadModulesWithId(rpc->messageId);
        break;
      case RMT_CallFunction:
        callFunction(rpc->moduleName, rpc->functionName, rpc->text, rpc->messageId);
        break;
    }

    if (PyErr_Occurred())
      errorHandler(rpc->messageId);

    PyGILState_Release(gil);

    // Free the memory block containing the message body since we've parsed it now
    VirtualFree(rpc, 0, MEM_RELEASE);
  }

  return 0;
}