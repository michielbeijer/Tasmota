/*********************************************************************************************\
 * Class wrappers for native objects
 * 
 * These class are simple wrappers (containers) for a pointer of an external object.
 * The pointer is stored interanlly by the class.
 * 
 * The constructor of this class must accept the first argument to be `comptr`,
 * in such case, the constructor must store the pointer.
 * The class is not supposed to free the object at `deinit` time.
\*********************************************************************************************/

#include "be_mapping.h"
#include "be_exec.h"
#include <string.h>

/*********************************************************************************************\
 * Create an object of `class_name` given an external poinrt `ptr`.
 * 
 * Instanciates the class and calls `init()` with `ptr` wrapped in `comptr` as single arg.
 * Both arguments but nost bu NULL.
 * 
 * On return, the created instance is top of stack.
\*********************************************************************************************/
void be_create_class_wrapper(bvm *vm, const char * class_name, void * ptr) {
  if (ptr == NULL) {
      be_throw(vm, BE_MALLOC_FAIL);
  }

  be_getglobal(vm, class_name);   // stack = class
  be_call(vm, 0);                 // instanciate, stack = instance
  be_getmember(vm, -1, "init");   // stack = instance, init_func
  be_pushvalue(vm, -2);           // stack = instance, init_func, instance
  be_pushcomptr(vm, ptr);         // stack = instance, init_func, instance, ptr
  be_call(vm, 2);                 // stack = instance, ret, instance, ptr
  be_pop(vm, 3);                  // stack = instance
}


/*********************************************************************************************\
 * Find an object by global or composite name.
 * 
 * I.e. `lv.lv_object` will check for a global called `lv` and a member `lv_object`
 * 
 * Only supports one level of depth, meaning a class within a module.
 * Does not check the type of the object found.
 * 
 * Arguments:
 *   `name`: can be NULL, in such case considers the member as not found
 * 
 * Case 1: (no dot in name) `lv_wifi_bars` will look for a global variable `lv_wifi_bars`
 * Case 2: (dot in name) `lvgl.lv_obj` will get global `lvgl` and look for `lv_obj` within this module
 * 
 * Returns the number of elements pushed on the stack: 1 for module, 2 for instance method, 0 if not found
\*********************************************************************************************/
int be_find_global_or_module_member(bvm *vm, const char * name) {
  char *saveptr;

  if (name == NULL) {
    be_pushnil(vm);
    return 0;
  }
  char name_buf[strlen(name)+1];
  strcpy(name_buf, name);

  char * prefix = strtok_r(name_buf, ".", &saveptr);
  char * suffix = strtok_r(NULL, ".", &saveptr);
  if (suffix) {
    if (be_getglobal(vm, prefix)) {
      if (be_getmember(vm, -1, suffix)) {
        if (be_isinstance(vm, -2)) {  // instance, so we need to push method + instance
          be_pushvalue(vm, -2);
          be_remove(vm, -3);
          return 2;
        } else {  // not instane, so keep only the top object
          be_remove(vm, -2);
          return 1;
        }
      } else {
        be_pop(vm, 2);
        return 0;
      }
    }
    be_pop(vm, 1);  // remove nil
    return 0;
  } else {  // no suffix, get the global object
    if (be_getglobal(vm, prefix)) {
      return 1;
    }
    be_pop(vm, 1);
    return 0;
  }
}


/*********************************************************************************************\
 * Automatically parse Berry stack and call the C function accordingly
 * 
 * This function takes the n incoming arguments and pushes them as arguments
 * on the stack for the C function:
 * - be_int -> int32_t
 * - be_bool -> int32_t with value 0/1
 * - be_string -> const char *
 * - be_instance -> gets the member "_p" and pushes as void*
 * 
 * This works because C silently ignores any unwanted arguments.
 * There is a strong requirements that all ints and pointers are 32 bits.
 * Float is not supported but could be added. Double cannot be supported because they are 64 bits
 * 
 * Optional argument:
 * - return_type: the C function return value is int32_t and is converted to the
 *   relevant Berry object depending on this char:
 *   '' (default): nil, no value
 *   'i' be_int
 *   'b' be_bool
 *   's' be_str
 * 
 * - arg_type: optionally check the types of input arguments, or throw an error
 *   string of argument types, '+' marks optional arguments
 *   '.' don't care
 *   'i' be_int
 *   'b' be_bool
 *   's' be_string
 *   'c' C callback
 *   '-' ignore and don't send to C function
 *   'lv_obj' be_instance of type or subtype
 *   '^lv_event_cb' callback of a named class - will call `_lvgl.gen_cb(arg_type, closure, self)` and expects a callback address in return
 * 
 * Ex: "oii+s" takes 3 mandatory arguments (obj_instance, int, int) and an optional fourth one [,string]
\*********************************************************************************************/
// general form of lv_obj_t* function, up to 4 parameters
// We can only send 32 bits arguments (no 64 bits nor double) and we expect pointers to be 32 bits

// read a single value at stack position idx, convert to int.
// if object instance, get `_p` member and convert it recursively
intptr_t be_convert_single_elt(bvm *vm, int idx, const char * arg_type, const char * gen_cb) {
  // berry_log_C("be_convert_single_elt(idx=%i, argtype='%s', gen_cb=%p", idx, arg_type, gen_cb);
  int ret = 0;
  char provided_type = 0;
  idx = be_absindex(vm, idx);   // make sure we have an absolute index
  
  // berry_log_C(">> 0 idx=%i arg_type=%s", idx, arg_type ? arg_type : "NULL");
  if (arg_type == NULL) { arg_type = "."; }    // if no type provided, replace with wildchar
  size_t arg_type_len = strlen(arg_type);

  // handle callbacks first, since a wrong parameter will always yield to a crash
  if (arg_type_len > 1 && arg_type[0] == '^') {     // it is a callback
    arg_type++;   // skip first character
    if (be_isclosure(vm, idx)) {
      ret = be_find_global_or_module_member(vm, gen_cb);
      if (ret) {
        be_remove(vm, -3);  // stack contains method + instance
        be_pushvalue(vm, idx);
        be_pushvalue(vm, 1);
        be_pushstring(vm, arg_type);
        be_call(vm, 2 + ret);
        const void * func = be_tocomptr(vm, -(3 + ret));
        be_pop(vm, 3 + ret);

        // berry_log_C("func=%p", func);
        return (int32_t) func;
      } else {
        be_raisef(vm, "type_error", "Can't find callback generator: %s", gen_cb);
      }
    } else {
      be_raise(vm, "type_error", "Closure expected for callback type");
    }
  }

  // first convert the value to int32
  if      (be_isint(vm, idx))     { ret = be_toint(vm, idx); provided_type = 'i'; }
  else if (be_isbool(vm, idx))    { ret = be_tobool(vm, idx); provided_type = 'b'; }
  else if (be_isstring(vm, idx))  { ret = (intptr_t) be_tostring(vm, idx); provided_type = 's'; }
  else if (be_iscomptr(vm, idx))  { ret = (intptr_t) be_tocomptr(vm, idx); provided_type = 'c'; }
  else if (be_isnil(vm, idx))     { ret = 0; provided_type = 'c'; }

  // check if simple type was a match
  if (provided_type) {
    bbool type_ok = bfalse;
    type_ok = (arg_type[0] == '.');                           // any type is accepted
    type_ok = type_ok || (arg_type[0] == provided_type);      // or type is a match
    type_ok = type_ok || (ret == 0 && arg_type_len != 1);    // or NULL is accepted for an instance
    
    if (!type_ok) {
      be_raisef(vm, "type_error", "Unexpected argument type '%c', expected '%s'", provided_type, arg_type);
    }
    // berry_log_C("be_convert_single_elt provided type=%i", ret);
    return ret;
  }

  // berry_log_C("be_convert_single_elt non simple type");
  // non-simple type
  if (be_isinstance(vm, idx))  {
    // check if the instance is a subclass of `bytes()``
    be_getbuiltin(vm, "bytes");
    if (be_isderived(vm, idx)) {
      be_pop(vm, 1);
      be_getmember(vm, idx, "_buffer");
      be_pushvalue(vm, idx);
      be_call(vm, 1);
      int32_t ret = (int32_t) be_tocomptr(vm, -2);
      be_pop(vm, 2);
      return ret;
    } else {
      be_pop(vm, 1);
      // we accept either `_p` or `.p` attribute to retrieve a pointer
      if (!be_getmember(vm, idx, "_p")) {
        be_pop(vm, 1);    // remove `nil`
        be_getmember(vm, idx, ".p");
      }
      int32_t ret = be_convert_single_elt(vm, -1, NULL, NULL);   // recurse
      be_pop(vm, 1);

      if (arg_type_len > 1) {
        // Check type
        be_classof(vm, idx);
        int class_found = be_find_global_or_module_member(vm, arg_type);
        // Stack: class_of_idx, class_of_target (or nil)
        if (class_found) {
          if (!be_isderived(vm, -2)) {
            be_raisef(vm, "type_error", "Unexpected class type '%s', expected '%s'", be_classname(vm, idx), arg_type);
          }
        } else {
          be_raisef(vm, "value_error", "Unable to find class '%s' (%d)", arg_type, arg_type_len);
        }
        be_pop(vm, 2);
      } else if (arg_type[0] != '.') {
        be_raisef(vm, "value_error", "Unexpected instance type '%s', expected '%s'", be_classname(vm, idx), arg_type);
      }

      return ret;
    }
  } else {
    be_raisef(vm, "value_error", "Unexpected '%s'", be_typename(vm, idx));
  }

  return ret;
}

/*********************************************************************************************\
 * Calling any LVGL function with auto-mapping
 * 
\*********************************************************************************************/

// check input parameters, and create callbacks if needed
// change values in place
//
// Format:
// - either a lowercase character encoding for a simple type
//   - 'b': bool
//   - 'i': int (int32_t)
//   - 's': string (const char *)
//
// - a class name surroungded by parenthesis
//   - '(lv_button)' -> lv_button class or derived
//   - '[lv_event_cb]' -> callback type, still prefixed with '^' to mark that it is cb
//
void be_check_arg_type(bvm *vm, int arg_start, int argc, const char * arg_type, intptr_t p[8]) {
  bbool arg_type_check = (arg_type != NULL);      // is type checking activated
  int32_t arg_idx = 0;    // position in arg_type string
  char type_short_name[32];

  uint32_t p_idx = 0; // index in p[], is incremented with each parameter except '-'
  for (uint32_t i = 0; i < argc; i++) {
    type_short_name[0] = 0;   // clear string
    // extract individual type
    if (NULL != arg_type) {
      switch (arg_type[arg_idx]) {
        case '-':
          arg_idx++;
          continue;   // ignore current parameter and advance
        case '.':
        case 'a'...'z':
          type_short_name[0] = arg_type[arg_idx];
          type_short_name[1] = 0;
          arg_idx++;
          break;
        case '(':
        case '^':
          {
            uint32_t prefix = 0;
            if (arg_type[arg_idx] == '^') {
              type_short_name[0] = '^';
              type_short_name[1] = 0;
              prefix = 1;
            }
            uint32_t offset = 0;
            arg_idx++;
            while (arg_type[arg_idx + offset] != ')' && arg_type[arg_idx + offset] != '^' && arg_type[arg_idx + offset] != 0 && offset+prefix+1 < sizeof(type_short_name)) {
              type_short_name[offset+prefix] = arg_type[arg_idx + offset];
              type_short_name[offset+prefix+1] = 0;
              offset++;
            }
            if (arg_type[arg_idx + offset] == 0) {
              arg_type = NULL;   // no more parameters, stop iterations
            }
            arg_idx += offset + 1;
          }
          break;
        case 0:
          arg_type = NULL;   // stop iterations
          break;
      }
    }
    // AddLog(LOG_LEVEL_INFO, ">> be_call_c_func arg %i, type %s", i, arg_type_check ? type_short_name : "<null>");
    p[p_idx++] = be_convert_single_elt(vm, i + arg_start, arg_type_check ? type_short_name : NULL, "_lvgl.gen_cb");
  }

  // check if we are missing arguments
  if (arg_type != NULL && arg_type[arg_idx] != 0) {
    be_raisef(vm, "value_error", "Missing arguments, remaining type '%s'", &arg_type[arg_idx]);
  }
}

//
// Internal function
//
// Called for constructors, i.e. C function mapped to Berry `init()`
//
// Pre-conditions:
//   The instance must be at stack position `1` (default when calling `init()`)
//
// Arguments:
//   vm: point to Berry vm (as usual)
//   ptr: the C pointer for internal data (can be NULL), will be stored in an instance variable
//   name: name of instance variable to store the pointer as `comptr`.
//         If NULL, this function does nothing
//         the name can be prefixed with `+`, if so first char is ignored.
//         Ex: `+_p` stores in instance variable `_p`
static void be_set_ctor_ptr(bvm *vm, void * ptr, const char *name) {
  if (name == NULL) return;    // do nothing if no name of attribute
  if (name[0] == '+') { name++; }   // skip prefix '^' if any
  if (strlen(name) == 0) return;  // do nothing if name is empty

  be_pushcomptr(vm, ptr);
  if (be_setmember(vm, 1, name)) {
    be_pop(vm, 1);
  } else {
    be_raisef(vm, "attribute_error", "Missing member '%s' in ctor", name);
  }
}

/*********************************************************************************************\
 * Call a C function with auto-mapping
 * 
 * Arguments:
 *   vm: pointer to Berry vm (as ususal)
 *   func: pointer to C function
 *   return_type: how to convert the result into a Berry type
 *   arg_type: string describing the optional and mandatory parameters
 * 
 * Note: the C function mapping supports max 8 arguments and does not directly support
 *       pointers to values (although it is possible to mimick with classes)
\*********************************************************************************************/
int be_call_c_func(bvm *vm, void * func, const char * return_type, const char * arg_type) {
  intptr_t p[8] = {0,0,0,0,0,0,0,0};
  int argc = be_top(vm); // Get the number of arguments

  // the following describe the active payload for the C function (start and count)
  // this is because the `init()` constructor first arg is not passed to the C function
  int arg_start = 1;      // start with standard values
  int arg_count = argc;

  // check if we call a constructor, in this case we store the return type into the new object
  // check if we call a constructor with a comptr as first arg
  if (return_type && return_type[0] == '+') {
    if (argc > 1 && be_iscomptr(vm, 2)) {
      void * obj = be_tocomptr(vm, 2);
      be_set_ctor_ptr(vm, obj, return_type);
      be_return_nil(vm);
    } else {
      // we need to discard the first arg
      arg_start++;
      arg_count--;
    }
  }

  fn_any_callable f = (fn_any_callable) func;
  be_check_arg_type(vm, arg_start, arg_count, arg_type, p);
  intptr_t ret = (*f)(p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
  // berry_log_C("be_call_c_func '%s' -> '%s': (%i,%i,%i,%i,%i,%i) -> %i", return_type, arg_type, p[0], p[1], p[2], p[3], p[4], p[5], ret);

  if ((return_type == NULL) || (strlen(return_type) == 0))       { be_return_nil(vm); }  // does not return
  else if (return_type[0] == '+') {
    void * obj = (void*) ret;
    be_set_ctor_ptr(vm, obj, return_type);
    be_return_nil(vm);
  }
  else if (strlen(return_type) == 1) {
    switch (return_type[0]) {
      case '.':   // fallback next
      case 'i':   be_pushint(vm, ret); break;
      case 'b':   be_pushbool(vm, ret);  break;
      case 's':   be_pushstring(vm, (const char*) ret);  break;
      case 'c':   be_pushint(vm, ret); break; // TODO missing 'c' general callback type
      default:    be_raise(vm, "internal_error", "Unsupported return type"); break;
    }
    be_return(vm);
  } else { // class name
    be_find_global_or_module_member(vm, return_type);
    be_pushcomptr(vm, (void*) ret);         // stack = class, ptr
    be_pushcomptr(vm, (void*) -1);         // stack = class, ptr, -1
    be_call(vm, 2);                 // instanciate with 2 arguments, stack = instance, -1, ptr
    be_pop(vm, 2);                  // stack = instance
    be_return(vm);
  }
}
