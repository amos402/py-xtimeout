#include "Python.h"
#include "pythread.h"
#include "frameobject.h"

#include <cassert>
#include <chrono>
#include <functional>
#include <memory>
#include <queue>
#include <unordered_map>
#include <vector>

#include <atomic>
#include <condition_variable>
#include <thread>
#include <mutex>

#include <climits>
#include <time.h>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <mmsystem.h>
#pragma comment(lib, "Winmm.lib")
#endif // _WIN32

typedef std::chrono::steady_clock ClockType;
static const auto INTERVAL_RESOLUTION = std::chrono::milliseconds(5);

static int g_PyTLSKey = -1;
static long g_MainThreadId;


static void AccurateSleep(unsigned long nMilliseconds)
{
#ifdef _WIN32
    // must call timeBeginPeriod(1) first
    Sleep(nMilliseconds);
#else
    timespec req, rem;
    req.tv_sec = 0;
    req.tv_nsec = nMilliseconds * 1000000;
    rem.tv_sec = 0;
    rem.tv_nsec = 0;
    while (nanosleep(&req, &rem) == 0)
    {
        if (rem.tv_nsec < 1000)
        {
            break;
        }
        req = rem;
        rem.tv_nsec = 0;
        rem.tv_sec = 0;
    }
#endif // _WIN32
}


static inline long GetCurThreadId()
{
#ifdef WIN32
    return (long)GetCurrentThreadId();
#else
    return (long)pthread_self();
#endif // WIN32

}

static PyObject* TimePointToPyFloat(const std::chrono::time_point<ClockType>& time)
{
    auto startNs = std::chrono::duration_cast<std::chrono::nanoseconds>(time.time_since_epoch());
    double pyTime = _PyTime_AsSecondsDouble(startNs.count());
    return PyFloat_FromDouble(pyTime);
}

class CThreadState
{
public:
    CThreadState(PyThreadState* state)
        : m_State(state), m_PrevState(nullptr)
    {
        m_IsMainThread = g_MainThreadId == GetCurThreadId();
    }

    void SwapState()
    {
        if (PyThreadState_GET() == m_State)
        {
            return;
        }
        PyThread_set_key_value(g_PyTLSKey, m_State);
        m_PrevState = PyThreadState_Swap(m_State);
        assert(PyThreadState_GET() == m_State);
    }

    void RestoreState()
    {
        if (m_PrevState == nullptr)
        {
            return;
        }
        PyThread_set_key_value(g_PyTLSKey, m_PrevState);
        PyThreadState_Swap(m_PrevState);
        assert(PyThreadState_GET() == m_PrevState);
        m_PrevState = nullptr;
    }

    bool IsMainThread() const
    {
        return m_IsMainThread;
    }

private:
    PyThreadState* m_State;
    PyThreadState* m_PrevState;
    bool m_IsMainThread;
};

#ifdef WITH_THREAD
class CGILHolder
{
public:
    CGILHolder() : m_State(PyGILState_Ensure())
    {
    }

    ~CGILHolder()
    {
        PyGILState_Release(m_State);
    }

private:
    PyGILState_STATE m_State;
};
#endif // WITH_THREAD

class CPyObjectHolder
{
public:
    CPyObjectHolder() : m_Object(nullptr)
    {
    }

    CPyObjectHolder(PyObject* object) : m_Object(object)
    {
    }

    ~CPyObjectHolder()
    {
        Py_XDECREF(m_Object);
    }

    CPyObjectHolder(const CPyObjectHolder& rhs)
    {
        m_Object = rhs.m_Object;
        Py_XINCREF(m_Object);
    }

    CPyObjectHolder& operator=(const CPyObjectHolder& rhs)
    {
        Py_XSETREF(m_Object, rhs.m_Object);
        return *this;
    }

    CPyObjectHolder(CPyObjectHolder&& rhs)
    {
        m_Object = rhs.m_Object;
        rhs.m_Object = nullptr;
    }

    CPyObjectHolder& operator=(CPyObjectHolder&& rhs)
    {
        if (this == &rhs)
        {
            return *this;
        }
        m_Object = rhs.m_Object;
        rhs.m_Object = nullptr;
        return *this;
    }

    PyObject* operator->() const
    {
        return m_Object;
    }

    PyObject& operator*() const
    {
        return *m_Object;
    }

    operator PyObject*()
    {
        return m_Object;
    }

    constexpr operator bool() const
    {
        return m_Object != nullptr;
    }

    bool operator==(const CPyObjectHolder& rhs)
    {
        return m_Object == rhs.m_Object;
    }

    bool operator !() const
    {
        return !operator bool();
    }

    bool operator==(PyObject* op)
    {
        return m_Object == op;
    }

    PyObject* Get() const
    {
        return m_Object;
    }

private:
    PyObject* m_Object;
};


class CLocalInjector
{
private:
    using TimePoint = std::chrono::time_point<ClockType>;

    class CFastArgWrapper
    {
    public:
        CFastArgWrapper(const std::shared_ptr<CLocalInjector>& obj)
            : injector(obj)
        {
        }

        std::shared_ptr<CLocalInjector> injector;
    };


    class CArgWrapper : public CFastArgWrapper
    {
    private:
        CArgWrapper(const std::shared_ptr<CLocalInjector>& obj)
            : CFastArgWrapper(obj), tracefunc(nullptr)
        {
        }

    public:
        Py_tracefunc tracefunc;
        CPyObjectHolder pyTraceobj;

        static CArgWrapper* Create(const std::shared_ptr<CLocalInjector>& obj)
        {
            return new CArgWrapper(obj);
        }

        static CArgWrapper* RestoreFromCapsule(PyObject* obj)
        {
            return static_cast<CArgWrapper*>(PyCapsule_GetPointer(obj, "ArgWrapper"));
        }

        CPyObjectHolder CreateCapsule()
        {
            return PyCapsule_New(this, "ArgWrapper", OnReleaseCapsule);
        }

    private:
        static void OnReleaseCapsule(PyObject* obj)
        {
            auto _this = static_cast<CArgWrapper*>(
                PyCapsule_GetPointer(obj, "ArgWrapper"));
            delete _this;
        }
    };

public:
    CLocalInjector() : m_IsValid(true), m_Callback(nullptr),
        m_ThState(PyThreadState_GET())
    {
    }

    ~CLocalInjector()
    {
        assert(!m_IsValid);
    }

    PyObject* GetCallback() const
    {
        return m_Callback;
    }

    void SetCallback(PyObject* func)
    {
        Py_XSETREF(m_Callback, func);
        Py_INCREF(func);
    }

    void SetDuration(uint32_t nMilliSec)
    {
        m_Duration = std::chrono::milliseconds(nMilliSec);
    }

    void SetDuration(std::chrono::milliseconds millisec)
    {
        m_Duration = millisec;
    }

    std::chrono::milliseconds GetDuration() const
    {
        return m_Duration;
    }

    void RecordStartTime()
    {
        m_StartTime = ClockType::now();
    }

    std::chrono::time_point<ClockType> GetStartTime() const
    {
        return m_StartTime;
    }

    bool IsMainThreadInjector() const
    {
        return m_ThState.IsMainThread();
    }

    void Release()
    {
        m_IsValid = false;
        Py_CLEAR(m_Callback);
    }

    bool IsValid() const
    {
        return m_IsValid;
    }

    static void FastCall(const std::shared_ptr<CLocalInjector>& injector)
    {
        if (!injector->IsValid())
        {
            return;
        }

        int res = Py_AddPendingCall(OnFastTrace, new CFastArgWrapper(injector));
        if (res == -1)
        {
            // TODO: if add pending failed, add it after some pending calls called
        }
    }

    static void Call(const std::shared_ptr<CLocalInjector>& injector)
    {
        if (!injector->IsValid())
        {
            return;
        }
        // wrap a injector ptr to keep its reference count
        auto wrapper = CArgWrapper::Create(injector);
        injector->m_ThState.SwapState();

        auto state = PyThreadState_GET();
        wrapper->tracefunc = state->c_tracefunc;
        Py_XINCREF(state->c_traceobj);
        wrapper->pyTraceobj = state->c_traceobj;
        const auto capsule = wrapper->CreateCapsule();
        if (!capsule)
        {
            Py_FatalError("Create capsule failed");
        }
        PyEval_SetTrace(OnTrace, capsule.Get());
        injector->m_ThState.RestoreState();
    }

protected:
    static int OnFastTrace(void* arg)
    {
        auto wrapper = reinterpret_cast<CFastArgWrapper*>(arg);
        auto &injector = wrapper->injector;
        if (!injector->IsValid())
        {
            delete wrapper;
            return 0;
        }
        PyObject* callback = injector->GetCallback();
        assert(callback);
        PyObject* pyStartTime = TimePointToPyFloat(injector->m_StartTime);
        PyObject* res = PyObject_CallFunction(callback, "O", pyStartTime);
        delete wrapper;
        if (res == nullptr)
        {
            return -1;
        }
        Py_DECREF(res);
        return 0;
    }

    static int OnTrace(PyObject* self, PyFrameObject* frame,
        int what, PyObject* arg)
    {
        PyObject* capsule = PyThreadState_GET()->c_traceobj;
        assert(capsule && PyCapsule_CheckExact(capsule));
        Py_INCREF(capsule);
        auto wrapper = CArgWrapper::RestoreFromCapsule(capsule);
        assert(wrapper != nullptr);
        // recover the old trace
        PyEval_SetTrace(wrapper->tracefunc, wrapper->pyTraceobj);

        PyObject* pyStartTime = TimePointToPyFloat(wrapper->injector->m_StartTime);
        PyObject* callback = wrapper->injector->GetCallback();
        if (!wrapper->injector->IsValid())
        {
            return 0;
        }
        PyObject* res = PyObject_CallFunction(callback, "(d)", pyStartTime);
        Py_DECREF(capsule);
        if (res == nullptr)
        {
            return -1;
        }
        Py_DECREF(res);
        return 0;
    }

private:
    bool m_IsValid;
    PyObject* m_Callback;
    std::chrono::milliseconds m_Duration;
    std::chrono::time_point<ClockType> m_StartTime;
    CThreadState m_ThState;
};


class CInjectorQueue
{
public:
    bool Empty() const
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        return m_Queue.empty();
    }

    void Push(const std::shared_ptr<CLocalInjector>& injector)
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_Queue.push(injector);
    }

    std::shared_ptr<CLocalInjector> Pop()
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        if (m_Queue.empty())
        {
            return nullptr;
        }
        auto injector = m_Queue.front();
        m_Queue.pop();
        return injector;
    }

private:
    std::queue<std::shared_ptr<CLocalInjector> > m_Queue;
    mutable std::mutex m_Mutex;
};

#ifdef WITH_THREAD
class CAttacher
{
private:
    CAttacher()
    {
    }

public:
    static CAttacher& Instance()
    {
        static CAttacher instance;
        return instance;
    }

    static void Attach(const std::shared_ptr<CLocalInjector>& injector)
    {
        if (injector->IsMainThreadInjector())
        {
            CLocalInjector::FastCall(injector);
        }
        else
        {
            CGILHolder gil;
            CLocalInjector::Call(injector);
        }
    }
};

#else
class CAttacher
{
private:
    CAttacher() : m_Appended(false)
    {
    }

public:
    static CAttacher& Instance()
    {
        static CAttacher instance;
        return instance;
    }

    static void Attach(const std::shared_ptr<CLocalInjector>& injector)
    {
        Instance().InjectRequest(injector);
    }

protected:
    static int OnCall(void* arg)
    {
        auto attacher = static_cast<CAttacher*>(arg);
        attacher->Handle();
        attacher->m_Appended = false;
        attacher->Handle();
        return 0;
    }

    bool InjectRequest(const std::shared_ptr<CLocalInjector>& injector)
    {
        m_Queue.Push(injector);
        // state: handling; ok
        // state: handled, appended set; ok
        // state: handled, before set appended; fail => need handle again
        if (m_Appended)
        {
            return true;
        }
        m_Appended = true;
        if (Py_AddPendingCall(OnCall, this) != 0)
        {
            return false;
        }
        return true;
    }

    void Handle()
    {
        while (!m_Queue.Empty())
        {
            auto injector = m_Queue.Pop();
            if (injector.get() == nullptr)
            {
                continue;
            }
            CLocalInjector::Call(injector);
        }
    }

private:
    CInjectorQueue m_Queue;
    std::atomic<bool> m_Appended;
};

#endif // WITH_THREAD

class CContextHelper
{
private:
    CContextHelper() :
        m_IsQuit(false), m_Running(false)
    {
    }

public:
    ~CContextHelper()
    {
        {
            std::lock_guard<std::mutex> lock(m_Mtx);
            m_IsQuit = true;
        }
        m_RunCond.notify_all();
        m_CheckTh.join();
    }

    static CContextHelper& Instance()
    {
        static CContextHelper instance;
        return instance;
    }

    void Start(const std::shared_ptr<CLocalInjector>& pInjector)
    {
        std::lock_guard<std::mutex> lock(m_CallMapMtx);
        AddTimeoutCallback(pInjector);
        if (!m_Running)
        {
            m_Running = true;
        }
        if (!m_CheckTh.joinable())
        {
            m_IsQuit = false;
            m_CheckTh = std::thread(
                std::bind(&CContextHelper::CheckThread, this));
        }
        m_RunCond.notify_one();
    }

    void Stop(std::shared_ptr<CLocalInjector>& pInjector)
    {
        std::lock_guard<std::mutex> lock(m_CallMapMtx);
        pInjector->Release();
        if (m_CallMap.find(pInjector.get()) == m_CallMap.end())
        {
            return;
        }
        if (m_Running)
        {
            // delete later
            m_DelVect.push_back(pInjector.get());
            return;
        }
        m_CallMap.erase(pInjector.get());
    }

protected:
    void CheckThread()
    {
        while (!m_IsQuit)
        {
            
#ifdef WIN32
            timeBeginPeriod(1);
#endif // WIN32
            do
            {
                std::chrono::nanoseconds minSpan(LLONG_MAX);
                auto startTime = ClockType::now();
                // TODO: use interval merge for optimization
                for (auto iter = m_CallMap.begin(); iter != m_CallMap.end();)
                {
                    auto& injector = iter->second;
                    auto span = ClockType::now() - injector->GetStartTime();
                    const auto& duration = injector->GetDuration();

                    if (span > duration)
                    {
                        CAttacher::Attach(injector);
                        assert(iter->second.use_count() > 1);
                        iter = m_CallMap.erase(iter);
                        continue;
                    }
                    else
                    {
                        minSpan = std::min(minSpan, duration - span);
                    }

                    if (m_IsQuit)
                    {
                        break;
                    }
                    ++iter;
                }

                Merge();
                if (m_CallMap.empty())
                {
                    m_Running = false;
                }
                auto endTime = ClockType::now();
                if (minSpan - (endTime - startTime) > INTERVAL_RESOLUTION)
                {
                    AccurateSleep(1);
                }
            } while (m_Running);
#ifdef WIN32
            timeEndPeriod(1);
#endif // WIN32
            if (!m_IsQuit)
            {
                std::unique_lock<std::mutex> lock(m_Mtx);
                m_RunCond.wait(lock);
            }
        }
    }

    void AddTimeoutCallback(const std::shared_ptr<CLocalInjector>& pInjector)
    {
        if (m_Running)
        {
            m_NewVect.push_back(pInjector);
            return;
        }
        m_CallMap[pInjector.get()] = pInjector;
    }

    void Merge()
    {
        std::lock_guard<std::mutex> lock(m_CallMapMtx);
        for (auto& item : m_DelVect)
        {
            m_CallMap.erase(item);
        }
        m_DelVect.clear();

        for (auto& item : m_NewVect)
        {
            // it maybe released
            if (!item->IsValid())
            {
                continue;
            }
            m_CallMap[item.get()] = item;
        }
        m_NewVect.clear();

    }

private:
    using CallMapType = std::unordered_map<
        CLocalInjector*, std::shared_ptr<CLocalInjector> >;

    std::thread m_CheckTh;
    std::atomic<bool> m_IsQuit;
    std::atomic<bool> m_Running;
    std::mutex m_Mtx;
    std::condition_variable m_RunCond;

    CallMapType m_CallMap;
    std::mutex m_CallMapMtx;
    std::vector<std::shared_ptr<CLocalInjector> > m_NewVect;
    std::vector<CLocalInjector*> m_DelVect;
};

#ifdef WITH_THREAD
static int GetPyThreadIndex()
{
    auto state = PyThreadState_GET();
    int index = -1;
    for (int i = 0; i < 100000; i++) {
        void* value = PyThread_get_key_value(i);
        if (value == state)
        {
            index = i;
            break;
        }
    }
    assert(index != -1);
    return index;
}
#endif // WITH_THREAD


struct PyInjector
{
    PyObject_HEAD;
    std::shared_ptr<CLocalInjector> injector;
};


static void DeallocPyInjector(PyInjector* self)
{
    if (!self->injector)
    {
        return;
    }
    self->injector->Release();
    self->injector = nullptr;
}

static int InitPyInjector(PyInjector* self, PyObject *args, PyObject *kwds)
{
    PyObject* callback;
    uint32_t time;
    if (!PyArg_ParseTuple(args, "IO", &time, &callback))
    {
        return -1;
    }
    self->injector = std::make_shared<CLocalInjector>();
    self->injector->SetCallback(callback);
    self->injector->SetDuration(time);
    return 0;
}

static PyObject* PyInjectorNew(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    PyInjector* self = reinterpret_cast<PyInjector*>(type->tp_alloc(type, 0));
    self->injector = nullptr;
    return reinterpret_cast<PyObject*>(self);
}

static PyObject* InjectorStart(PyObject* self, PyObject* args)
{
    auto pyInjector = reinterpret_cast<PyInjector*>(self);
    auto& injector = pyInjector->injector;
    injector->RecordStartTime();
    CContextHelper::Instance().Start(injector);
    Py_RETURN_NONE;
}

static PyObject* InjectorStop(PyObject* self, PyObject* args)
{
    auto pyInjector = reinterpret_cast<PyInjector*>(self);
    CContextHelper::Instance().Stop(pyInjector->injector);
    Py_RETURN_NONE;
}

static PyObject* InjectorReset(PyObject* self)
{
    auto pyInjector = reinterpret_cast<PyInjector*>(self);
    auto& injector = pyInjector->injector;
    PyObject* callback = injector->GetCallback();
    Py_XINCREF(callback);
    auto time = injector->GetDuration();
    injector->Release();

    // replace the injector
    injector = std::make_shared<CLocalInjector>();
    injector->SetCallback(callback);
    injector->SetDuration(time);
    injector->RecordStartTime();
    CContextHelper::Instance().Start(injector);

    Py_XDECREF(callback);
    Py_RETURN_NONE;
}

static PyMethodDef injector_methods[] = {
    { "start", (PyCFunction)InjectorStart, METH_NOARGS, nullptr },
    { "stop", (PyCFunction)InjectorStop, METH_NOARGS, nullptr },
    { "reset", (PyCFunction)InjectorReset, METH_NOARGS, nullptr },
    {nullptr, nullptr}
};

PyDoc_STRVAR(injector_doc,
"Injector(time: int, callback: callable)\n"
"time unit: milliseconds");

static PyTypeObject injector_type = {
    PyVarObject_HEAD_INIT(0, 0)                 /* Must fill in type value later */
    "Injector",                                 /* tp_name */
    sizeof(PyInjector),                         /* tp_basicsize */
    0,                                          /* tp_itemsize */
    (destructor)DeallocPyInjector,              /* tp_dealloc */
    0,                                          /* tp_print */
    0,                                          /* tp_getattr */
    0,                                          /* tp_setattr */
    0,                                          /* tp_reserved */
    0,                                          /* tp_repr */
    0,                                          /* tp_as_number */
    0,                                          /* tp_as_sequence */
    0,                                          /* tp_as_mapping */
    0,                                          /* tp_hash */
    0,                                          /* tp_call */
    0,                                          /* tp_str */
    0,                                          /* tp_getattro */
    0,                                          /* tp_setattro */
    0,                                          /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT,                         /* tp_flags */
    injector_doc,                               /* tp_doc */
    0,                                          /* tp_traverse */
    0,                                          /* tp_clear */
    0,                                          /* tp_richcompare */
    0,                                          /* tp_weaklistoffset */
    0,                                          /* tp_iter */
    0,                                          /* tp_iternext */
    injector_methods,                           /* tp_methods */
    0,                                          /* tp_members */
    0,                                          /* tp_getset */
    0,                                          /* tp_base */
    0,                                          /* tp_dict */
    0,                                          /* tp_descr_get */
    0,                                          /* tp_descr_set */
    0,                                          /* tp_dictoffset */
    (initproc)InitPyInjector,                   /* tp_init */
    0,                                          /* tp_alloc */
    PyInjectorNew,                              /* tp_new */
};

static PyMethodDef methods[] = {

    { nullptr, nullptr}
};

static struct PyModuleDef module = {
    PyModuleDef_HEAD_INIT,
    "_xtimeout",        /* m_name */
    "_xtimeout",        /* m_doc */
    -1,                 /* m_size */
    methods,            /* m_methods */
    NULL,               /* m_reload */
    NULL,               /* m_traverse */
    NULL,               /* m_clear */
    NULL,               /* m_free */
};


PyMODINIT_FUNC PyInit__xtimeout()
{
    if (PyType_Ready(&injector_type) != 0)
    {
        return nullptr;
    }
    PyObject* m = PyModule_Create(&module);
    if (m == nullptr)
    {
        return nullptr;
    }
    if (PyModule_AddObject(m, "Injector", (PyObject*)&injector_type) != 0)
    {
        return nullptr;
    }
#ifdef WITH_THREAD
    PyEval_InitThreads();
    g_PyTLSKey = GetPyThreadIndex();
    PyInterpreterState* interpreter = PyInterpreterState_Head();
    g_MainThreadId = interpreter->tstate_head->thread_id;
#endif
    return m;
}
