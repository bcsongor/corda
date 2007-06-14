#include "common.h"
#include "system.h"
#include "heap.h"
#include "class_finder.h"

#define PROTECT(thread, name)                                   \
  Thread::Protector MAKE_NAME(protector_) (thread, &name);

#define ACQUIRE_MONITOR(x) MonitorResource MAKE_NAME(monitorResource_) (x)

namespace {

typedef void* object;
typedef unsigned Type;

#include "constants.h"

enum ObjectType {
  NullType,
  CollectedType,

#include "type-enums.h"

  OtherType
};

class Thread;

Type typeOf(object);
object& objectClass(object);
void assert(Thread* t, bool v);

#include "type-header.h"

class Machine {
 public:
  System* sys;
  Heap* heap;
  ClassFinder* classFinder;
  Thread* rootThread;
  Thread* exclusive;
  unsigned activeCount;
  unsigned liveCount;
  System::Monitor* stateLock;
  System::Monitor* heapLock;
  System::Monitor* classLock;
  object classMap;
};

class Thread {
 public:
  enum State {
    NoState,
    ActiveState,
    IdleState,
    ZombieState,
    ExclusiveState,
    ExitState
  };

  class Protector {
   public:
    Protector(Thread* t, object* p): t(t), p(p), next(t->protector) {
      t->protector = this;
    }

    ~Protector() {
      t->protector = next;
    }

    Thread* t;
    object* p;
    Protector* next;
  };

  static const unsigned HeapSize = 64 * 1024;
  static const unsigned StackSize = 64 * 1024;

  Machine* vm;
  Thread* next;
  Thread* child;
  State state;
  object thread;
  object frame;
  object code;
  object exception;
  unsigned ip;
  unsigned sp;
  unsigned heapIndex;
  object stack[StackSize];
  object heap[HeapSize];
  Protector* protector;
};

class MonitorResource {
 public:
  MonitorResource(System::Monitor* m): m(m) { m->acquire(); }
  ~MonitorResource() { m->release(); }

 private:
  System::Monitor* m;
};

inline void NO_RETURN
abort(Thread* t)
{
  t->vm->sys->abort();
}

inline void
assert(Thread* t, bool v)
{
  if (UNLIKELY(not v)) abort(t);
}

void
init(Machine* m, System* sys, Heap* heap, ClassFinder* classFinder)
{
  memset(m, 0, sizeof(Machine));
  m->sys = sys;
  m->heap = heap;
  m->classFinder = classFinder;

  if (not sys->success(sys->make(&(m->stateLock))) or
      not sys->success(sys->make(&(m->heapLock))) or
      not sys->success(sys->make(&(m->classLock))))
  {
    sys->abort();
  }
}

void
dispose(Machine* m)
{
  m->stateLock->dispose();
  m->heapLock->dispose();
}

void
init(Thread* t, Machine* m)
{
  memset(m, 0, sizeof(Thread));
  t->vm = m;
  m->rootThread = t;
  t->state = Thread::NoState;
}

void
iterate(Thread* t, Heap::Visitor* v)
{
  t->heapIndex = 0;

  v->visit(&(t->thread));
  v->visit(&(t->frame));
  v->visit(&(t->code));
  v->visit(&(t->exception));

  for (unsigned i = 0; i < t->sp; ++i) {
    v->visit(t->stack + t->sp);
  }

  for (Thread::Protector* p = t->protector; p; p = p->next) {
    v->visit(p->p);
  }

  for (Thread* t = t->child; t; t = t->next) {
    iterate(t, v);
  }
}

void
collect(Machine* m, Heap::CollectionType type)
{
  class Iterator: public Heap::Iterator {
   public:
    Iterator(Machine* m): m(m) { }

    void iterate(Heap::Visitor* v) {
      v->visit(&(m->classMap));

      for (Thread* t = m->rootThread; t; t = t->next) {
        ::iterate(t, v);
      }
    }
    
   private:
    Machine* m;
  } it(m);

  m->heap->collect(type, &it);
}

void
enter(Thread* t, Thread::State s)
{
  if (s == t->state) return;

  ACQUIRE_MONITOR(t->vm->stateLock);

  switch (s) {
  case Thread::ExclusiveState: {
    assert(t, t->state == Thread::ActiveState);

    while (t->vm->exclusive) {
      // another thread got here first.
      enter(t, Thread::IdleState);
      enter(t, Thread::ActiveState);
    }

    t->state = Thread::ExclusiveState;
    t->vm->exclusive = t;
      
    while (t->vm->activeCount > 1) {
      t->vm->stateLock->wait();
    }
  } break;

  case Thread::IdleState:
  case Thread::ZombieState: {
    switch (t->state) {
    case Thread::ExclusiveState: {
      assert(t, t->vm->exclusive == t);
      t->vm->exclusive = 0;
    } break;

    case Thread::ActiveState: break;

    default: abort(t);
    }

    -- t->vm->activeCount;
    if (s == Thread::ZombieState) {
      -- t->vm->liveCount;
    }
    t->state = s;

    t->vm->stateLock->notifyAll();
  } break;

  case Thread::ActiveState: {
    switch (t->state) {
    case Thread::ExclusiveState: {
      assert(t, t->vm->exclusive == t);

      t->state = s;
      t->vm->exclusive = 0;

      t->vm->stateLock->notifyAll();
    } break;

    case Thread::NoState:
    case Thread::IdleState: {
      while (t->vm->exclusive) {
        t->vm->stateLock->wait();
      }

      ++ t->vm->activeCount;
      if (t->state == Thread::NoState) {
        ++ t->vm->liveCount;
      }
      t->state = s;
    } break;

    default: abort(t);
    }
  } break;

  case Thread::ExitState: {
    switch (t->state) {
    case Thread::ExclusiveState: {
      assert(t, t->vm->exclusive == t);
      t->vm->exclusive = 0;
    } break;

    case Thread::ActiveState: break;

    default: abort(t);
    }
      
    -- t->vm->activeCount;
    t->state = s;

    while (t->vm->liveCount > 1) {
      t->vm->stateLock->wait();
    }
  } break;

  default: abort(t);
  }
}

void
maybeYieldAndMaybeCollect(Thread* t, unsigned size)
{
  if (size > Thread::HeapSize) {
    // large object support not yet implemented.
    abort(t);
  }

  ACQUIRE_MONITOR(t->vm->stateLock);

  while (t->vm->exclusive) {
    // another thread wants to enter the exclusive state, either for a
    // collection or some other reason.  We give it a chance here.
    enter(t, Thread::IdleState);
    enter(t, Thread::ActiveState);
  }

  if (t->heapIndex + size >= Thread::HeapSize) {
    enter(t, Thread::ExclusiveState);
    collect(t->vm, Heap::MinorCollection);
    enter(t, Thread::ActiveState);
  }
}

inline object
allocate(Thread* t, unsigned size)
{
  if (UNLIKELY(t->heapIndex + size >= Thread::HeapSize
               or t->vm->exclusive))
  {
    maybeYieldAndMaybeCollect(t, size);
  }

  object o = t->heap + t->heapIndex;
  t->heapIndex += size;
  return o;
}

inline void
set(Thread* t, object& target, object value)
{
  target = value;
  t->vm->heap->check(&target, t->vm->heapLock);
}

inline void
push(Thread* t, object o)
{
  t->stack[(t->sp)++] = o;
}

inline object
pop(Thread* t)
{
  return t->stack[--(t->sp)];
}

inline object&
top(Thread* t)
{
  return t->stack[t->sp - 1];
}

inline object
make(Thread* t, object class_)
{
  PROTECT(t, class_);
  unsigned size = classFixedSize(t, class_);
  object instance = allocate(t, size);
  *static_cast<object*>(instance) = class_;
  memset(static_cast<object*>(instance) + sizeof(object), 0,
         size - sizeof(object));
  return instance;
}

template <class T>
inline T&
cast(object p, unsigned offset)
{
  return *reinterpret_cast<T*>(static_cast<uint8_t*>(p) + offset);
}

object
makeString(Thread* t, const char* format, ...)
{
  static const unsigned Size = 256;
  char buffer[Size];
  
  va_list a;
  va_start(a, format);
  vsnprintf(buffer, Size - 1, format, a);
  va_end(a);

  object s = makeByteArray(t, strlen(buffer) + 1);
  memcpy(byteArrayBody(t, s), buffer, byteArrayLength(t, s));

  return makeString(t, s, 0, byteArrayLength(t, s), 0);
}

object
makeTrace(Thread* t)
{
  object trace = 0;
  PROTECT(t, trace);
  frameIp(t, t->frame) = t->ip;
  for (; t->frame; t->frame = frameNext(t, t->frame)) {
    trace = makeTrace
      (t, frameMethod(t, t->frame), frameIp(t, t->frame), trace);
  }
  return trace;
}

object
makeArrayIndexOutOfBoundsException(Thread* t, object message)
{
  PROTECT(t, message);
  object trace = makeTrace(t);
  return makeArrayIndexOutOfBoundsException(t, message, trace);
}

object
makeNegativeArrayStoreException(Thread* t, object message)
{
  PROTECT(t, message);
  object trace = makeTrace(t);
  return makeNegativeArrayStoreException(t, message, trace);
}

object
makeClassCastException(Thread* t, object message)
{
  PROTECT(t, message);
  object trace = makeTrace(t);
  return makeClassCastException(t, message, trace);
}

object
makeClassNotFoundException(Thread* t, object message)
{
  PROTECT(t, message);
  object trace = makeTrace(t);
  return makeClassNotFoundException(t, message, trace);
}

object
makeNullPointerException(Thread* t)
{
  return makeNullPointerException(t, 0, makeTrace(t));
}

object
makeStackOverflowError(Thread* t)
{
  return makeStackOverflowError(t, 0, makeTrace(t));
}

object
makeNoSuchFieldError(Thread* t, object message)
{
  PROTECT(t, message);
  object trace = makeTrace(t);
  return makeNoSuchFieldError(t, message, trace);
}

object
makeNoSuchMethodError(Thread* t, object message)
{
  PROTECT(t, message);
  object trace = makeTrace(t);
  return makeNoSuchMethodError(t, message, trace);
}

inline bool
isLongOrDouble(object o)
{
  return typeOf(o) == LongType or typeOf(o) == DoubleType;
}

inline object
getField(Thread* t, object instance, object field)
{
  return cast<object>(instance, fieldOffset(t, field) * sizeof(object));
}

inline void
setField(Thread* t, object o, object field, object value)
{
  set(t, cast<object>(o, fieldOffset(t, field) * sizeof(object)), value);
}

inline object
getStatic(Thread* t, object field)
{
  return rawArrayBody(t, classStaticTable(t, fieldClass(t, field)))
    [fieldOffset(t, field)];
}

inline void
setStatic(Thread* t, object field, object value)
{
  set(t, rawArrayBody(t, classStaticTable(t, fieldClass(t, field)))
      [fieldOffset(t, field)], value);
}

bool
instanceOf(Thread* t, object class_, object o)
{
  if (o == 0) {
    return false;
  }

  if (typeOf(class_) == InterfaceType) {
    Type id = interfaceId(t, class_);
    for (object oc = objectClass(o); oc; oc = classSuper(t, oc)) {
      object itable = classInterfaceTable(t, oc);
      for (unsigned i = 0; i < rawArrayLength(t, itable); i += 2) {
        if (interfaceId(t, rawArrayBody(t, itable)[i]) == id) {
          return true;
        }
      }
    }
  } else {
    Type id = classId(t, class_);
    for (object oc = objectClass(o); oc; oc = classSuper(t, oc)) {
      if (classId(t, oc) == id) {
        return true;
      }
    }
  }

  return false;
}

object
findInterfaceMethod(Thread* t, object method, object o)
{
  Type id = interfaceId(t, methodClass(t, method));
  object itable = classInterfaceTable(t, objectClass(o));
  for (unsigned i = 0; i < rawArrayLength(t, itable); i += 2) {
    if (interfaceId(t, rawArrayBody(t, itable)[i]) == id) {
      return rawArrayBody(t, rawArrayBody(t, itable)[i + 1])
        [methodOffset(t, method)];
    }
  }
  abort(t);
}

inline object
findMethod(Thread* t, object method, object class_)
{
  return rawArrayBody(t, classMethodTable(t, class_))
    [methodOffset(t, method)];
}

inline object
findVirtualMethod(Thread* t, object method, object o)
{
  return findMethod(t, method, objectClass(o));
}

bool
isSuperclass(Thread* t, object class_, object base)
{
  Type id = classId(t, class_);
  for (object oc = classSuper(t, base); oc; oc = classSuper(t, oc)) {
    if (classId(t, oc) == id) {
      return true;
    }
  }
  return false;
}

inline int
strcmp(const int8_t* a, const int8_t* b)
{
  return ::strcmp(reinterpret_cast<const char*>(a),
                  reinterpret_cast<const char*>(b));
}

inline bool
isSpecialMethod(Thread* t, object method, object class_)
{
  return (classFlags(t, class_) & ACC_SUPER)
    and strcmp(reinterpret_cast<const int8_t*>("<init>"), 
               byteArrayBody(t, methodName(t, method))) != 0
    and isSuperclass(t, methodClass(t, method), class_);
}

object
find(Thread* t, object class_, object table, object reference,
     object& (*name)(Thread*, object),
     object& (*spec)(Thread*, object),
     object (*makeError)(Thread*, object))
{
  object n = referenceName(t, reference);
  object s = referenceSpec(t, reference);
  for (unsigned i = 0; i < rawArrayLength(t, table); ++i) {
    object field = rawArrayBody(t, table)[i];
    if (strcmp(byteArrayBody(t, name(t, field)),
               byteArrayBody(t, n)) == 0 and
        strcmp(byteArrayBody(t, spec(t, field)),
               byteArrayBody(t, s)) == 0)
    {
      return field;
    }               
  }

  object message = makeString
    (t, "%s (%s) not found in %s",
     byteArrayBody(t, n),
     byteArrayBody(t, s),
     byteArrayBody(t, className(t, class_)));
  t->exception = makeError(t, message);
  return 0;
}

inline object
findFieldInClass(Thread* t, object class_, object reference)
{
  return find(t, class_, classFieldTable(t, class_), reference, fieldName,
              fieldSpec, makeNoSuchFieldError);
}

inline object
findMethodInClass(Thread* t, object class_, object reference)
{
  return find(t, class_, classMethodTable(t, class_), reference, methodName,
              methodSpec, makeNoSuchMethodError);
}

uint32_t
hash(const int8_t* s, unsigned length)
{
  uint32_t h = 0;
  for (unsigned i = 0; i < length; ++i) h = (h * 31) + s[i];
  return h;  
}

bool
byteArrayEqual(Thread* t, object a, object b)
{
  if (a == b) return true;

  if (byteArrayLength(t, a) == byteArrayLength(t, b)) {
    return strcmp(byteArrayBody(t, a), byteArrayBody(t, b)) == 0;
  } else {
    return false;
  }
}

object
hashMapFind(Thread* t, object map, uint32_t hash, object key,
            bool (*equal)(Thread*, object, object))
{
  unsigned index = hash & (rawArrayLength(t, map) - 1);
  object n = rawArrayBody(t, map)[index];
  while (n) {
    if (equal(t, tripleFirst(t, n), key)) {
      return tripleSecond(t, n);
    }
    
    n = tripleThird(t, n);
  }
  return 0;
}

object
hashMapInsert(Thread* t, object map, uint32_t hash, object key, object value)
{
  unsigned index = hash & (rawArrayLength(t, map) - 1);
  object n = rawArrayBody(t, map)[index];

  PROTECT(t, map);

  n = makeTriple(t, key, value, n);

  set(t, rawArrayBody(t, map)[index], n);
}

object
resolveClass(Thread* t, object spec)
{
  PROTECT(t, spec);
  ACQUIRE(t, t->vm->classLock);

  uint32_t h = hash(byteArrayBody(t, spec), byteArrayLength(t, spec) - 1);
  object class_ = hashMapFind(t, t->vm->classMap, h, spec, byteArrayEqual);
  if (class_ == 0) {
    unsigned size;
    const uint8_t* data = t->vm->classFinder->find
      (reinterpret_cast<const char*>(byteArrayBody(t, spec)), &size);

    if (data) {
      class_ = parseClass(t, data, size);

      PROTECT(t, class_);
      hashMapInsert(t, t->vm->classMap, h, spec, class_);
    } else {
      object message = makeString(t, "%s", byteArrayBody(t, spec));
      t->exception = makeClassNotFoundException(t, message);
    }
  }
  return class_;
}

inline object
resolveClass(Thread* t, object pool, unsigned index)
{
  object o = rawArrayBody(t, pool)[index];
  if (typeOf(o) == ByteArrayType) {
    PROTECT(t, pool);

    o = resolveClass(t, o);
    if (UNLIKELY(t->exception)) return 0;
    
    set(t, rawArrayBody(t, pool)[index], o);
  }
  return o; 
}

inline object
resolveClass(Thread* t, object container, object& (*class_)(Thread*, object))
{
  object o = class_(t, container);
  if (typeOf(o) == ByteArrayType) {
    PROTECT(t, container);

    o = resolveClass(t, o);
    if (UNLIKELY(t->exception)) return 0;
    
    set(t, class_(t, container), o);
  }
  return o; 
}

inline object
resolve(Thread* t, object pool, unsigned index,
        object (*find)(Thread*, object, object))
{
  object o = rawArrayBody(t, pool)[index];
  if (typeOf(o) == ReferenceType) {
    PROTECT(t, pool);

    object class_ = resolveClass(t, o, referenceClass);
    if (UNLIKELY(t->exception)) return 0;

    o = find(t, class_, rawArrayBody(t, pool)[index]);
    if (UNLIKELY(t->exception)) return 0;
    
    set(t, rawArrayBody(t, pool)[index], o);
  }
  return o;
}

inline object
resolveField(Thread* t, object pool, unsigned index)
{
  return resolve(t, pool, index, findFieldInClass);
}

inline object
resolveMethod(Thread* t, object pool, unsigned index)
{
  return resolve(t, pool, index, findMethodInClass);
}

object
run(Thread* t)
{
  unsigned& ip = t->ip;
  unsigned& sp = t->sp;
  object& code = t->code;
  object& frame = t->frame;
  object& exception = t->exception;
  object* stack = t->stack;
  unsigned parameterCount = 0;

 loop:
  switch (codeBody(t, code)[ip++]) {
  case aaload: {
    object index = pop(t);
    object array = pop(t);

    if (LIKELY(array)) {
      int32_t i = intValue(t, index);
      if (LIKELY(i >= 0 and
                 static_cast<uint32_t>(i) < objectArrayLength(t, array)))
      {
        push(t, objectArrayBody(t, array)[i]);
      } else {
        object message = makeString(t, "%d not in [0,%d]", i,
                                    objectArrayLength(t, array));
        exception = makeArrayIndexOutOfBoundsException(t, message);
        goto throw_;
      }
    } else {
      exception = makeNullPointerException(t);
      goto throw_;
    }
  } goto loop;

  case aastore: {
    object value = pop(t);
    object index = pop(t);
    object array = pop(t);
    int32_t i = intValue(t, index);

    if (LIKELY(array)) {
      if (LIKELY(i >= 0 and
                 static_cast<uint32_t>(i) < objectArrayLength(t, array)))
      {
        set(t, objectArrayBody(t, array)[i], value);
      } else {
        object message = makeString(t, "%d not in [0,%d]", i,
                                    objectArrayLength(t, array));
        exception = makeArrayIndexOutOfBoundsException(t, message);
        goto throw_;
      }
    } else {
      exception = makeNullPointerException(t);
      goto throw_;
    }
  } goto loop;

  case aconst_null: {
    push(t, 0);
  } goto loop;

  case aload:
  case iload:
  case lload: {
    push(t, frameLocals(t, frame)[codeBody(t, code)[ip++]]);
  } goto loop;

  case aload_0:
  case iload_0:
  case lload_0: {
    push(t, frameLocals(t, frame)[0]);
  } goto loop;

  case aload_1:
  case iload_1:
  case lload_1: {
    push(t, frameLocals(t, frame)[1]);
  } goto loop;

  case aload_2:
  case iload_2:
  case lload_2: {
    push(t, frameLocals(t, frame)[2]);
  } goto loop;

  case aload_3:
  case iload_3:
  case lload_3: {
    push(t, frameLocals(t, frame)[3]);
  } goto loop;

  case anewarray: {
    object count = pop(t);
    int32_t c = intValue(t, count);

    if (LIKELY(c >= 0)) {
      uint8_t index1 = codeBody(t, code)[ip++];
      uint8_t index2 = codeBody(t, code)[ip++];
      uint16_t index = (index1 << 8) | index2;
      
      object class_ = resolveClass(t, codePool(t, code), index);
      if (UNLIKELY(exception)) goto throw_;
      
      object array = makeObjectArray(t, class_, c);
      memset(objectArrayBody(t, array), 0, c * 4);
      
      push(t, array);
    } else {
      object message = makeString(t, "%d", c);
      exception = makeNegativeArrayStoreException(t, message);
      goto throw_;
    }
  } goto loop;

  case areturn:
  case ireturn:
  case lreturn: {
    frame = frameNext(t, frame);
    if (frame) {
      code = methodCode(t, frameMethod(t, frame));
      ip = frameIp(t, frame);
      goto loop;
    } else {
      object value = pop(t);
      code = 0;
      return value;
    }
  } goto loop;

  case arraylength: {
    object array = pop(t);
    if (LIKELY(array)) {
      if (typeOf(array) == ObjectArrayType) {
        push(t, makeInt(t, objectArrayLength(t, array)));
      } else {
        // for all other array types, the length follow the class pointer.
        push(t, makeInt(t, cast<uint32_t>(array, sizeof(void*))));
      }
    } else {
      exception = makeNullPointerException(t);
      goto throw_;
    }
  } abort(t);

  case astore:
  case istore:
  case lstore: {
    object value = pop(t);
    set(t, frameLocals(t, frame)[codeBody(t, code)[ip++]], value);
  } goto loop;

  case astore_0:
  case istore_0:
  case lstore_0: {
    object value = pop(t);
    set(t, frameLocals(t, frame)[0], value);
  } goto loop;

  case astore_1:
  case istore_1:
  case lstore_1: {
    object value = pop(t);
    set(t, frameLocals(t, frame)[1], value);
  } goto loop;

  case astore_2:
  case istore_2:
  case lstore_2: {
    object value = pop(t);
    set(t, frameLocals(t, frame)[2], value);
  } goto loop;

  case astore_3:
  case istore_3:
  case lstore_3: {
    object value = pop(t);
    set(t, frameLocals(t, frame)[3], value);
  } goto loop;

  case athrow: {
    exception = pop(t);
    if (UNLIKELY(exception == 0)) {
      exception = makeNullPointerException(t);      
    }
    goto throw_;
  } abort(t);

  case baload: {
    object index = pop(t);
    object array = pop(t);

    if (LIKELY(array)) {
      int32_t i = intValue(t, index);
      if (LIKELY(i >= 0 and
                 static_cast<uint32_t>(i) < byteArrayLength(t, array)))
      {
        push(t, makeByte(t, byteArrayBody(t, array)[i]));
      } else {
        object message = makeString(t, "%d not in [0,%d]", i,
                                    byteArrayLength(t, array));
        exception = makeArrayIndexOutOfBoundsException(t, message);
        goto throw_;
      }
    } else {
      exception = makeNullPointerException(t);
      goto throw_;
    }
  } goto loop;

  case bastore: {
    object value = pop(t);
    object index = pop(t);
    object array = pop(t);
    int32_t i = intValue(t, index);

    if (LIKELY(array)) {
      if (LIKELY(i >= 0 and
                 static_cast<uint32_t>(i) < byteArrayLength(t, array)))
      {
        byteArrayBody(t, array)[i] = intValue(t, value);
      } else {
        object message = makeString(t, "%d not in [0,%d]", i,
                                    byteArrayLength(t, array));
        exception = makeArrayIndexOutOfBoundsException(t, message);
        goto throw_;
      }
    } else {
      exception = makeNullPointerException(t);
      goto throw_;
    }
  } goto loop;

  case bipush: {
    push(t, makeInt(t, codeBody(t, code)[ip++]));
  } goto loop;

  case caload: {
    object index = pop(t);
    object array = pop(t);

    if (LIKELY(array)) {
      int32_t i = intValue(t, index);
      if (LIKELY(i >= 0 and
                 static_cast<uint32_t>(i) < charArrayLength(t, array)))
      {
        push(t, makeInt(t, charArrayBody(t, array)[i]));
      } else {
        object message = makeString(t, "%d not in [0,%d]", i,
                                    charArrayLength(t, array));
        exception = makeArrayIndexOutOfBoundsException(t, message);
        goto throw_;
      }
    } else {
      exception = makeNullPointerException(t);
      goto throw_;
    }
  } goto loop;

  case castore: {
    object value = pop(t);
    object index = pop(t);
    object array = pop(t);
    int32_t i = intValue(t, index);

    if (LIKELY(array)) {
      if (LIKELY(i >= 0 and
                 static_cast<uint32_t>(i) < charArrayLength(t, array)))
      {
        charArrayBody(t, array)[i] = intValue(t, value);
      } else {
        object message = makeString(t, "%d not in [0,%d]", i,
                                    charArrayLength(t, array));
        exception = makeArrayIndexOutOfBoundsException(t, message);
        goto throw_;
      }
    } else {
      exception = makeNullPointerException(t);
      goto throw_;
    }
  } goto loop;

  case checkcast: {
    uint8_t index1 = codeBody(t, code)[ip++];
    uint8_t index2 = codeBody(t, code)[ip++];

    if (stack[sp - 1]) {
      uint16_t index = (index1 << 8) | index2;
      
      object class_ = resolveClass(t, codePool(t, code), index);
      if (UNLIKELY(exception)) goto throw_;

      if (not instanceOf(t, class_, stack[sp - 1])) {
        object message = makeString
          (t, "%s as %s",
           byteArrayBody(t, className(t, objectClass(stack[sp - 1]))),
           byteArrayBody(t, className(t, class_)));
        exception = makeClassCastException(t, message);
        goto throw_;
      }
    }
  } goto loop;

  case dup: {
    object value = stack[sp - 1];
    push(t, value);
  } goto loop;

  case dup_x1: {
    object first = pop(t);
    object second = pop(t);
    
    push(t, first);
    push(t, second);
    push(t, first);
  } goto loop;

  case dup_x2: {
    object first = pop(t);
    object second = pop(t);
    object third = pop(t);
    
    push(t, first);
    push(t, third);
    push(t, second);
    push(t, first);
  } goto loop;

  case dup2: {
    object first = stack[sp - 1];
    if (isLongOrDouble(first)) {
      push(t, first);
    } else {
      object second = stack[sp - 2];
      push(t, second);
      push(t, first);
    }
  } goto loop;

  case dup2_x1: {
    object first = pop(t);
    object second = pop(t);
    
    if (isLongOrDouble(first)) {
      push(t, first);
      push(t, second);
      push(t, first);
    } else {
      object third = pop(t);
      push(t, second);
      push(t, first);
      push(t, third);
      push(t, second);
      push(t, first);
    }
  } goto loop;

  case dup2_x2: {
    object first = pop(t);
    object second = pop(t);
    
    if (isLongOrDouble(first)) {
      if (isLongOrDouble(second)) {
        push(t, first);
        push(t, second);
        push(t, first);
      } else {
        object third = pop(t);
        push(t, first);
        push(t, third);
        push(t, second);
        push(t, first);
      }
    } else {
      object third = pop(t);
      if (isLongOrDouble(third)) {
        push(t, second);
        push(t, first);
        push(t, third);
        push(t, second);
        push(t, first);
      } else {
        object fourth = pop(t);
        push(t, second);
        push(t, first);
        push(t, fourth);
        push(t, third);
        push(t, second);
        push(t, first);
      }
    }
  } goto loop;

  case getfield: {
    object instance = pop(t);
    if (LIKELY(instance)) {
      uint8_t index1 = codeBody(t, code)[ip++];
      uint8_t index2 = codeBody(t, code)[ip++];
      uint16_t index = (index1 << 8) | index2;
    
      object field = resolveField(t, codePool(t, code), index);
      if (UNLIKELY(exception)) goto throw_;
      
      push(t, getField(t, instance, field));
    } else {
      exception = makeNullPointerException(t);
      goto throw_;
    }
  } goto loop;

  case getstatic: {
    uint8_t index1 = codeBody(t, code)[ip++];
    uint8_t index2 = codeBody(t, code)[ip++];
    uint16_t index = (index1 << 8) | index2;

    object field = resolveField(t, codePool(t, code), index);
    if (UNLIKELY(exception)) goto throw_;

    object p = classInitializers(t, fieldClass(t, field));
    if (p) {
      set(t, classInitializers(t, fieldClass(t, field)), pairSecond(t, p));
      code = pairFirst(t, p);
      ip -= 3;
      parameterCount = 0;
      goto invoke;
    }
      
    push(t, getStatic(t, field));
  } goto loop;

  case goto_: {
    uint8_t offset1 = codeBody(t, code)[ip++];
    uint8_t offset2 = codeBody(t, code)[ip++];

    ip = (ip - 1) + ((offset1 << 8) | offset2);
  } goto loop;
    
  case goto_w: {
    uint8_t offset1 = codeBody(t, code)[ip++];
    uint8_t offset2 = codeBody(t, code)[ip++];
    uint8_t offset3 = codeBody(t, code)[ip++];
    uint8_t offset4 = codeBody(t, code)[ip++];

    ip = (ip - 1)
      + ((offset1 << 24) | (offset2 << 16) | (offset3 << 8) | offset4);
  } goto loop;

  case i2b: {
    object v = pop(t);
    
    push(t, makeInt(t, static_cast<int8_t>(intValue(t, v))));
  } goto loop;

  case i2c: {
    object v = pop(t);
    
    push(t, makeInt(t, static_cast<uint16_t>(intValue(t, v))));
  } goto loop;

  case i2l: {
    object v = pop(t);
    
    push(t, makeLong(t, intValue(t, v)));
  } goto loop;

  case i2s: {
    object v = pop(t);

    push(t, makeInt(t, static_cast<int16_t>(intValue(t, v))));
  } goto loop;

  case iadd: {
    object b = pop(t);
    object a = pop(t);
    
    push(t, makeInt(t, intValue(t, a) + intValue(t, b)));
  } goto loop;

  case iaload: {
    object index = pop(t);
    object array = pop(t);

    if (LIKELY(array)) {
      int32_t i = intValue(t, index);
      if (LIKELY(i >= 0 and
                 static_cast<uint32_t>(i) < intArrayLength(t, array)))
      {
        push(t, makeInt(t, intArrayBody(t, array)[i]));
      } else {
        object message = makeString(t, "%d not in [0,%d]", i,
                                    intArrayLength(t, array));
        exception = makeArrayIndexOutOfBoundsException(t, message);
        goto throw_;
      }
    } else {
      exception = makeNullPointerException(t);
      goto throw_;
    }
  } goto loop;

  case iand: {
    object b = pop(t);
    object a = pop(t);
    
    push(t, makeInt(t, intValue(t, a) & intValue(t, b)));
  } goto loop;

  case iastore: {
    object value = pop(t);
    object index = pop(t);
    object array = pop(t);
    int32_t i = intValue(t, index);

    if (LIKELY(array)) {
      if (LIKELY(i >= 0 and
                 static_cast<uint32_t>(i) < intArrayLength(t, array)))
      {
        intArrayBody(t, array)[i] = intValue(t, value);
      } else {
        object message = makeString(t, "%d not in [0,%d]", i,
                                    intArrayLength(t, array));
        exception = makeArrayIndexOutOfBoundsException(t, message);
        goto throw_;
      }
    } else {
      exception = makeNullPointerException(t);
      goto throw_;
    }
  } goto loop;

  case iconst_0: {
    push(t, makeInt(t, 0));
  } goto loop;

  case iconst_1: {
    push(t, makeInt(t, 1));
  } goto loop;

  case iconst_2: {
    push(t, makeInt(t, 2));
  } goto loop;

  case iconst_3: {
    push(t, makeInt(t, 3));
  } goto loop;

  case iconst_4: {
    push(t, makeInt(t, 4));
  } goto loop;

  case iconst_5: {
    push(t, makeInt(t, 5));
  } goto loop;

  case idiv: {
    object b = pop(t);
    object a = pop(t);
    
    push(t, makeInt(t, intValue(t, a) / intValue(t, b)));
  } goto loop;

  case if_acmpeq: {
    uint8_t offset1 = codeBody(t, code)[ip++];
    uint8_t offset2 = codeBody(t, code)[ip++];

    object b = pop(t);
    object a = pop(t);
    
    if (a == b) {
      ip = (ip - 1) + ((offset1 << 8) | offset2);
    }
  } goto loop;

  case if_acmpne: {
    uint8_t offset1 = codeBody(t, code)[ip++];
    uint8_t offset2 = codeBody(t, code)[ip++];

    object b = pop(t);
    object a = pop(t);
    
    if (a != b) {
      ip = (ip - 1) + ((offset1 << 8) | offset2);
    }
  } goto loop;

  case if_icmpeq: {
    uint8_t offset1 = codeBody(t, code)[ip++];
    uint8_t offset2 = codeBody(t, code)[ip++];

    object b = pop(t);
    object a = pop(t);
    
    if (intValue(t, a) == intValue(t, b)) {
      ip = (ip - 1) + ((offset1 << 8) | offset2);
    }
  } goto loop;

  case if_icmpne: {
    uint8_t offset1 = codeBody(t, code)[ip++];
    uint8_t offset2 = codeBody(t, code)[ip++];

    object b = pop(t);
    object a = pop(t);
    
    if (intValue(t, a) != intValue(t, b)) {
      ip = (ip - 1) + ((offset1 << 8) | offset2);
    }
  } goto loop;

  case if_icmpgt: {
    uint8_t offset1 = codeBody(t, code)[ip++];
    uint8_t offset2 = codeBody(t, code)[ip++];

    object b = pop(t);
    object a = pop(t);
    
    if (intValue(t, a) > intValue(t, b)) {
      ip = (ip - 1) + ((offset1 << 8) | offset2);
    }
  } goto loop;

  case if_icmpge: {
    uint8_t offset1 = codeBody(t, code)[ip++];
    uint8_t offset2 = codeBody(t, code)[ip++];

    object b = pop(t);
    object a = pop(t);
    
    if (intValue(t, a) >= intValue(t, b)) {
      ip = (ip - 1) + ((offset1 << 8) | offset2);
    }
  } goto loop;

  case if_icmplt: {
    uint8_t offset1 = codeBody(t, code)[ip++];
    uint8_t offset2 = codeBody(t, code)[ip++];

    object b = pop(t);
    object a = pop(t);
    
    if (intValue(t, a) < intValue(t, b)) {
      ip = (ip - 1) + ((offset1 << 8) | offset2);
    }
  } goto loop;

  case if_icmple: {
    uint8_t offset1 = codeBody(t, code)[ip++];
    uint8_t offset2 = codeBody(t, code)[ip++];

    object b = pop(t);
    object a = pop(t);
    
    if (intValue(t, a) < intValue(t, b)) {
      ip = (ip - 1) + ((offset1 << 8) | offset2);
    }
  } goto loop;

  case ifeq: {
    uint8_t offset1 = codeBody(t, code)[ip++];
    uint8_t offset2 = codeBody(t, code)[ip++];

    object v = pop(t);
    
    if (intValue(t, v) == 0) {
      ip = (ip - 1) + ((offset1 << 8) | offset2);
    }
  } goto loop;

  case ifne: {
    uint8_t offset1 = codeBody(t, code)[ip++];
    uint8_t offset2 = codeBody(t, code)[ip++];

    object v = pop(t);
    
    if (intValue(t, v)) {
      ip = (ip - 1) + ((offset1 << 8) | offset2);
    }
  } goto loop;

  case ifgt: {
    uint8_t offset1 = codeBody(t, code)[ip++];
    uint8_t offset2 = codeBody(t, code)[ip++];

    object v = pop(t);
    
    if (intValue(t, v) > 0) {
      ip = (ip - 1) + ((offset1 << 8) | offset2);
    }
  } goto loop;

  case ifge: {
    uint8_t offset1 = codeBody(t, code)[ip++];
    uint8_t offset2 = codeBody(t, code)[ip++];

    object v = pop(t);
    
    if (intValue(t, v) >= 0) {
      ip = (ip - 1) + ((offset1 << 8) | offset2);
    }
  } goto loop;

  case iflt: {
    uint8_t offset1 = codeBody(t, code)[ip++];
    uint8_t offset2 = codeBody(t, code)[ip++];

    object v = pop(t);
    
    if (intValue(t, v) < 0) {
      ip = (ip - 1) + ((offset1 << 8) | offset2);
    }
  } goto loop;

  case ifle: {
    uint8_t offset1 = codeBody(t, code)[ip++];
    uint8_t offset2 = codeBody(t, code)[ip++];

    object v = pop(t);
    
    if (intValue(t, v) <= 0) {
      ip = (ip - 1) + ((offset1 << 8) | offset2);
    }
  } goto loop;

  case ifnonnull: {
    uint8_t offset1 = codeBody(t, code)[ip++];
    uint8_t offset2 = codeBody(t, code)[ip++];

    object v = pop(t);
    
    if (v) {
      ip = (ip - 1) + ((offset1 << 8) | offset2);
    }
  } goto loop;

  case ifnull: {
    uint8_t offset1 = codeBody(t, code)[ip++];
    uint8_t offset2 = codeBody(t, code)[ip++];

    object v = pop(t);
    
    if (v == 0) {
      ip = (ip - 1) + ((offset1 << 8) | offset2);
    }
  } goto loop;

  case iinc: {
    uint8_t index = codeBody(t, code)[ip++];
    int8_t c = codeBody(t, code)[ip++];
    
    int32_t v = intValue(t, frameLocals(t, frame)[index]);
    frameLocals(t, frame)[index] = makeInt(t, v + c);
  } goto loop;

  case imul: {
    object b = pop(t);
    object a = pop(t);
    
    push(t, makeInt(t, intValue(t, a) * intValue(t, b)));
  } goto loop;

  case ineg: {
    object v = pop(t);
    
    push(t, makeInt(t, - intValue(t, v)));
  } goto loop;

  case instanceof: {
    uint8_t index1 = codeBody(t, code)[ip++];
    uint8_t index2 = codeBody(t, code)[ip++];

    if (stack[sp - 1]) {
      uint16_t index = (index1 << 8) | index2;
      
      object class_ = resolveClass(t, codePool(t, code), index);
      if (UNLIKELY(exception)) goto throw_;

      if (instanceOf(t, class_, stack[sp - 1])) {
        push(t, makeInt(t, 1));
      } else {
        push(t, makeInt(t, 0));
      }
    } else {
      push(t, makeInt(t, 0));
    }
  } goto loop;

  case invokeinterface: {
    uint8_t index1 = codeBody(t, code)[ip++];
    uint8_t index2 = codeBody(t, code)[ip++];
    uint16_t index = (index1 << 8) | index2;
      
    ip += 2;

    object method = resolveMethod(t, codePool(t, code), index);
    if (UNLIKELY(exception)) goto throw_;
    
    parameterCount = methodParameterCount(t, method);
    if (LIKELY(stack[sp - parameterCount])) {
      code = findInterfaceMethod(t, method, stack[sp - parameterCount]);
      if (UNLIKELY(exception)) goto throw_;

      goto invoke;
    } else {
      exception = makeNullPointerException(t);
      goto throw_;
    }
  } goto loop;

  case invokespecial: {
    uint8_t index1 = codeBody(t, code)[ip++];
    uint8_t index2 = codeBody(t, code)[ip++];
    uint16_t index = (index1 << 8) | index2;

    object method = resolveMethod(t, codePool(t, code), index);
    if (UNLIKELY(exception)) goto throw_;
    
    parameterCount = methodParameterCount(t, method);
    if (LIKELY(stack[sp - parameterCount])) {
      object class_ = methodClass(t, frameMethod(t, t->frame));
      if (isSpecialMethod(t, method, class_)) {
        code = findMethod(t, method, classSuper(t, class_));
        if (UNLIKELY(exception)) goto throw_;
      } else {
        code = method;
      }
      
      goto invoke;
    } else {
      exception = makeNullPointerException(t);
      goto throw_;
    }
  } goto loop;

  case invokestatic: {
    uint8_t index1 = codeBody(t, code)[ip++];
    uint8_t index2 = codeBody(t, code)[ip++];
    uint16_t index = (index1 << 8) | index2;

    object method = resolveMethod(t, codePool(t, code), index);
    if (UNLIKELY(exception)) goto throw_;
    
    object p = classInitializers(t, methodClass(t, method));
    if (p) {
      set(t, classInitializers(t, methodClass(t, method)), pairSecond(t, p));
      code = pairFirst(t, p);
      ip -= 3;
      parameterCount = 0;
      goto invoke;
    }

    parameterCount = methodParameterCount(t, method);
    code = method;
  } goto invoke;

  case invokevirtual: {
    uint8_t index1 = codeBody(t, code)[ip++];
    uint8_t index2 = codeBody(t, code)[ip++];
    uint16_t index = (index1 << 8) | index2;

    object method = resolveMethod(t, codePool(t, code), index);
    if (UNLIKELY(exception)) goto throw_;
    
    parameterCount = methodParameterCount(t, method);
    if (LIKELY(stack[sp - parameterCount])) {
      code = findVirtualMethod(t, method, stack[sp - parameterCount]);
      if (UNLIKELY(exception)) goto throw_;
      
      goto invoke;
    } else {
      exception = makeNullPointerException(t);
      goto throw_;
    }
  } goto loop;

  case ior: {
    object b = pop(t);
    object a = pop(t);
    
    push(t, makeInt(t, intValue(t, a) | intValue(t, b)));
  } goto loop;

  case irem: {
    object b = pop(t);
    object a = pop(t);
    
    push(t, makeInt(t, intValue(t, a) % intValue(t, b)));
  } goto loop;

  case ishl: {
    object b = pop(t);
    object a = pop(t);
    
    push(t, makeInt(t, intValue(t, a) << intValue(t, b)));
  } goto loop;

  case ishr: {
    object b = pop(t);
    object a = pop(t);
    
    push(t, makeInt(t, intValue(t, a) >> intValue(t, b)));
  } goto loop;

  case isub: {
    object b = pop(t);
    object a = pop(t);
    
    push(t, makeInt(t, intValue(t, a) - intValue(t, b)));
  } goto loop;

  case iushr: {
    object b = pop(t);
    object a = pop(t);
    
    push(t, makeInt(t, static_cast<uint32_t>(intValue(t, a)) >> intValue(t, b)));
  } goto loop;

  case ixor: {
    object b = pop(t);
    object a = pop(t);
    
    push(t, makeInt(t, intValue(t, a) ^ intValue(t, b)));
  } goto loop;

  case jsr: {
    uint8_t offset1 = codeBody(t, code)[ip++];
    uint8_t offset2 = codeBody(t, code)[ip++];

    push(t, makeInt(t, ip));
    ip = (ip - 1) + ((offset1 << 8) | offset2);
  } goto loop;

  case jsr_w: {
    uint8_t offset1 = codeBody(t, code)[ip++];
    uint8_t offset2 = codeBody(t, code)[ip++];
    uint8_t offset3 = codeBody(t, code)[ip++];
    uint8_t offset4 = codeBody(t, code)[ip++];

    push(t, makeInt(t, ip));
    ip = (ip - 1)
      + ((offset1 << 24) | (offset2 << 16) | (offset3 << 8) | offset4);
  } goto loop;

  case l2i: {
    object v = pop(t);
    
    push(t, makeInt(t, static_cast<int32_t>(longValue(t, v))));
  } goto loop;

  case ladd: {
    object b = pop(t);
    object a = pop(t);
    
    push(t, makeLong(t, longValue(t, a) + longValue(t, b)));
  } goto loop;

  case laload: {
    object index = pop(t);
    object array = pop(t);

    if (LIKELY(array)) {
      int32_t i = intValue(t, index);
      if (LIKELY(i >= 0 and
                 static_cast<uint32_t>(i) < longArrayLength(t, array)))
      {
        push(t, makeLong(t, longArrayBody(t, array)[i]));
      } else {
        object message = makeString(t, "%d not in [0,%d]", i,
                                    longArrayLength(t, array));
        exception = makeArrayIndexOutOfBoundsException(t, message);
        goto throw_;
      }
    } else {
      exception = makeNullPointerException(t);
      goto throw_;
    }
  } goto loop;

  case land: {
    object b = pop(t);
    object a = pop(t);
    
    push(t, makeLong(t, longValue(t, a) & longValue(t, b)));
  } goto loop;

  case lastore: {
    object value = pop(t);
    object index = pop(t);
    object array = pop(t);
    int32_t i = intValue(t, index);

    if (LIKELY(array)) {
      if (LIKELY(i >= 0 and
                 static_cast<uint32_t>(i) < longArrayLength(t, array)))
      {
        longArrayBody(t, array)[i] = longValue(t, value);
      } else {
        object message = makeString(t, "%d not in [0,%d]", i,
                                    longArrayLength(t, array));
        exception = makeArrayIndexOutOfBoundsException(t, message);
        goto throw_;
      }
    } else {
      exception = makeNullPointerException(t);
      goto throw_;
    }
  } goto loop;

  case lcmp: {
    object b = pop(t);
    object a = pop(t);
    
    push(t, makeInt(t, longValue(t, a) > longValue(t, b) ? 1
                    : longValue(t, a) == longValue(t, b) ? 0 : -1));
  } goto loop;

  case lconst_0: {
    push(t, makeLong(t, 0));
  } goto loop;

  case lconst_1: {
    push(t, makeLong(t, 1));
  } goto loop;

  case ldc: {
    push(t, rawArrayBody(t, codePool(t, code))[codeBody(t, code)[ip++]]);
  } goto loop;

  case ldc_w:
  case ldc2_w: {
    uint8_t index1 = codeBody(t, code)[ip++];
    uint8_t index2 = codeBody(t, code)[ip++];

    push(t, rawArrayBody(t, codePool(t, code))[(index1 << 8) | index2]);
  } goto loop;

  case ldiv: {
    object b = pop(t);
    object a = pop(t);
    
    push(t, makeLong(t, longValue(t, a) / longValue(t, b)));
  } goto loop;

  case lmul: {
    object b = pop(t);
    object a = pop(t);
    
    push(t, makeLong(t, longValue(t, a) * longValue(t, b)));
  } goto loop;

  case lneg: {
    object v = pop(t);
    
    push(t, makeLong(t, - longValue(t, v)));
  } goto loop;

  case lor: {
    object b = pop(t);
    object a = pop(t);
    
    push(t, makeLong(t, longValue(t, a) | longValue(t, b)));
  } goto loop;

  case lrem: {
    object b = pop(t);
    object a = pop(t);
    
    push(t, makeLong(t, longValue(t, a) % longValue(t, b)));
  } goto loop;

  case lshl: {
    object b = pop(t);
    object a = pop(t);
    
    push(t, makeLong(t, longValue(t, a) << longValue(t, b)));
  } goto loop;

  case lshr: {
    object b = pop(t);
    object a = pop(t);
    
    push(t, makeLong(t, longValue(t, a) >> longValue(t, b)));
  } goto loop;

  case lsub: {
    object b = pop(t);
    object a = pop(t);
    
    push(t, makeLong(t, longValue(t, a) - longValue(t, b)));
  } goto loop;

  case lushr: {
    object b = pop(t);
    object a = pop(t);
    
    push(t, makeLong(t, static_cast<uint64_t>(longValue(t, a))
                     << longValue(t, b)));
  } goto loop;

  case lxor: {
    object b = pop(t);
    object a = pop(t);
    
    push(t, makeLong(t, longValue(t, a) ^ longValue(t, b)));
  } goto loop;

  case new_: {
    uint8_t index1 = codeBody(t, code)[ip++];
    uint8_t index2 = codeBody(t, code)[ip++];
    uint16_t index = (index1 << 8) | index2;
    
    object class_ = resolveClass(t, codePool(t, code), index);
    if (UNLIKELY(exception)) goto throw_;

    object p = classInitializers(t, class_);
    if (p) {
      set(t, classInitializers(t, class_), pairSecond(t, p));
      code = pairFirst(t, p);
      ip -= 3;
      parameterCount = 0;
      goto invoke;
    }

    push(t, make(t, class_));
  } goto loop;

  case newarray: {
    object count = pop(t);
    int32_t c = intValue(t, count);

    if (LIKELY(c >= 0)) {
      uint8_t type = codeBody(t, code)[ip++];

      object array;
      unsigned factor;

      switch (type) {
      case T_BOOLEAN:
        array = makeBooleanArray(t, c);
        factor = 1;
        break;

      case T_CHAR:
        array = makeCharArray(t, c);
        factor = 2;
        break;

      case T_FLOAT:
        array = makeFloatArray(t, c);
        factor = 4;
        break;

      case T_DOUBLE:
        array = makeDoubleArray(t, c);
        factor = 8;
        break;

      case T_BYTE:
        array = makeByteArray(t, c);
        factor = 1;
        break;

      case T_SHORT:
        array = makeShortArray(t, c);
        factor = 2;
        break;

      case T_INT:
        array = makeIntArray(t, c);
        factor = 4;
        break;

      case T_LONG:
        array = makeLongArray(t, c);
        factor = 8;
        break;

      default: abort(t);
      }
      
      memset(static_cast<uint8_t*>(array) + sizeof(object) + 4, 0,
             c * factor);
      
      push(t, array);
    } else {
      object message = makeString(t, "%d", c);
      exception = makeNegativeArrayStoreException(t, message);
      goto throw_;
    }
  } goto loop;

  case nop: goto loop;

  case pop_: {
    -- sp;
  } goto loop;

  case pop2: {
    object top = stack[sp - 1];
    if (isLongOrDouble(top)) {
      -- sp;
    } else {
      sp -= 2;
    }
  } goto loop;

  case putfield: {
    object instance = pop(t);
    if (LIKELY(instance)) {
      uint8_t index1 = codeBody(t, code)[ip++];
      uint8_t index2 = codeBody(t, code)[ip++];
      uint16_t index = (index1 << 8) | index2;
    
      object field = resolveField(t, codePool(t, code), index);
      if (UNLIKELY(exception)) goto throw_;
      
      object value = pop(t);
      setField(t, instance, field, value);
    } else {
      exception = makeNullPointerException(t);
      goto throw_;
    }
  } goto loop;

  case putstatic: {
    uint8_t index1 = codeBody(t, code)[ip++];
    uint8_t index2 = codeBody(t, code)[ip++];
    uint16_t index = (index1 << 8) | index2;

    object field = resolveField(t, codePool(t, code), index);
    if (UNLIKELY(exception)) goto throw_;

    object p = classInitializers(t, fieldClass(t, field));
    if (p) {
      set(t, classInitializers(t, fieldClass(t, field)), pairSecond(t, p));
      code = pairFirst(t, p);
      ip -= 3;
      parameterCount = 0;
      goto invoke;
    }
      
    object value = pop(t);
    setStatic(t, field, value);
  } goto loop;

  case ret: {
    ip = intValue(t, frameLocals(t, frame)[codeBody(t, code)[ip]]);
  } goto loop;

  case return_: {
    frame = frameNext(t, frame);
    if (frame) {
      code = methodCode(t, frameMethod(t, frame));
      ip = frameIp(t, frame);
      goto loop;
    } else {
      code = 0;
      return 0;
    }
  } goto loop;

  case saload: {
    object index = pop(t);
    object array = pop(t);

    if (LIKELY(array)) {
      int32_t i = intValue(t, index);
      if (LIKELY(i >= 0 and
                 static_cast<uint32_t>(i) < shortArrayLength(t, array)))
      {
        push(t, makeShort(t, shortArrayBody(t, array)[i]));
      } else {
        object message = makeString(t, "%d not in [0,%d]", i,
                                    shortArrayLength(t, array));
        exception = makeArrayIndexOutOfBoundsException(t, message);
        goto throw_;
      }
    } else {
      exception = makeNullPointerException(t);
      goto throw_;
    }
  } goto loop;

  case sastore: {
    object value = pop(t);
    object index = pop(t);
    object array = pop(t);
    int32_t i = intValue(t, index);

    if (LIKELY(array)) {
      if (LIKELY(i >= 0 and
                 static_cast<uint32_t>(i) < shortArrayLength(t, array)))
      {
        shortArrayBody(t, array)[i] = intValue(t, value);
      } else {
        object message = makeString(t, "%d not in [0,%d]", i,
                                    shortArrayLength(t, array));
        exception = makeArrayIndexOutOfBoundsException(t, message);
        goto throw_;
      }
    } else {
      exception = makeNullPointerException(t);
      goto throw_;
    }
  } goto loop;

  case sipush: {
    uint8_t byte1 = codeBody(t, code)[ip++];
    uint8_t byte2 = codeBody(t, code)[ip++];

    push(t, makeInt(t, (byte1 << 8) | byte2));
  } goto loop;

  case swap: {
    object tmp = stack[sp - 1];
    stack[sp - 1] = stack[sp - 2];
    stack[sp - 2] = tmp;
  } goto loop;

  case wide: goto wide;

  default: abort(t);
  }

 wide:
  switch (codeBody(t, code)[ip++]) {
  case aload:
  case iload:
  case lload: {
    uint8_t index1 = codeBody(t, code)[ip++];
    uint8_t index2 = codeBody(t, code)[ip++];

    push(t, frameLocals(t, frame)[(index1 << 8) | index2]);
  } goto loop;

  case astore:
  case istore:
  case lstore: {
    uint8_t index1 = codeBody(t, code)[ip++];
    uint8_t index2 = codeBody(t, code)[ip++];

    object value = pop(t);
    set(t, frameLocals(t, frame)[(index1 << 8) | index2], value);
  } goto loop;

  case iinc: {
    uint8_t index1 = codeBody(t, code)[ip++];
    uint8_t index2 = codeBody(t, code)[ip++];
    uint16_t index = (index1 << 8) | index2;

    uint8_t count1 = codeBody(t, code)[ip++];
    uint8_t count2 = codeBody(t, code)[ip++];
    uint16_t count = (count1 << 8) | count2;
    
    int32_t v = intValue(t, frameLocals(t, frame)[index]);
    frameLocals(t, frame)[index] = makeInt(t, v + count);
  } goto loop;

  case ret: {
    uint8_t index1 = codeBody(t, code)[ip++];
    uint8_t index2 = codeBody(t, code)[ip++];

    ip = intValue(t, frameLocals(t, frame)[(index1 << 8) | index2]);
  } goto loop;

  default: abort(t);
  }

 invoke:
  if (UNLIKELY(codeMaxStack(t, methodCode(t, code)) + sp - parameterCount
               > Thread::StackSize))
  {
    exception = makeStackOverflowError(t);
    goto throw_;      
  }
  
  frameIp(t, frame) = ip;
  
  sp -= parameterCount;
  frame = makeFrame(t, code, frame, 0, sp,
                    codeMaxLocals(t, methodCode(t, code)));
  memcpy(frameLocals(t, frame), stack + sp, parameterCount);
  ip = 0;
  goto loop;

 throw_:
  for (; frame; frame = frameNext(t, frame)) {
    code = methodCode(t, frameMethod(t, frame));
    object eht = codeExceptionHandlerTable(t, code);
    if (eht) {
      for (unsigned i = 0; i < exceptionHandlerTableLength(t, eht); ++i) {
        ExceptionHandler* eh = exceptionHandlerTableBody(t, eht, i);
        uint16_t catchType = exceptionHandlerCatchType(eh);
        if (catchType == 0 or
            instanceOf(t,
                       rawArrayBody(t, codePool(t, code))[catchType],
                       exception))
        {
          sp = frameStackBase(t, frame);
          ip = exceptionHandlerIp(eh);
          push(t, exception);
          exception = 0;
          goto loop;
        }
      }
    }
  }

  object method = threadExceptionHandler(t, t->thread);
  code = methodCode(t, method);
  frame = makeFrame(t, method, 0, 0, 0, codeMaxLocals(t, code));
  sp = 0;
  ip = 0;
  push(t, exception);
  exception = 0;
  goto loop;
}

} // namespace
