"""Exercise Linux Unity's actual C ABI against the staged game and native engine."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import ctypes as c
import json
import os
from pathlib import Path
import threading


def check_native_worker(export, domain):
    attach = export("thread_attach", c.c_void_p, c.c_void_p)
    detach = export("thread_detach", None, c.c_void_p)
    make = export("string_new", c.c_void_p, c.c_char_p)
    weak = export("gchandle_new_weakref", c.c_uint32, c.c_void_p, c.c_bool)
    target = export("gchandle_get_target", c.c_void_p, c.c_uint32)
    collect = export("gc_collect", None, c.c_int)
    read = c.CDLL(None).read
    read.argtypes, read.restype = (c.c_int, c.c_void_p, c.c_size_t), c.c_ssize_t
    reader, writer = os.pipe()
    ready = threading.Event()
    handles = []

    def native_worker():
        thread = attach(domain)
        if not thread:
            raise RuntimeError("could not attach the native worker")
        try:
            value = make(b"held on the native stack")
            handles.append(weak(value, False))
            ready.set()
            # The native read retains the managed pointer on this thread. EOF
            # releases the thread without writing into the managed object.
            read(reader, value, 1)
        finally:
            detach(thread)

    def collect_on_worker():
        thread = attach(domain)
        if not thread:
            raise RuntimeError("could not attach the collecting worker")
        try:
            collect(2)
        finally:
            detach(thread)

    with ThreadPoolExecutor(max_workers=1) as pool:
        worker = pool.submit(native_worker)
        try:
            if not ready.wait(10):
                raise RuntimeError("native worker failed to attach to the managed domain")
            for _ in range(3):
                collect(2)
            value = target(handles[0])
            if not value or export("string_length", c.c_int32, c.c_void_p)(value) != 24:
                raise ValueError("collection lost the worker's native stack root")
        finally:
            os.close(writer)
            try:
                worker.result(timeout=10)
            finally:
                os.close(reader)
                for handle in handles:
                    export("gchandle_free", None, c.c_uint32)(handle)
        print("PASS full collections preserve a managed object on an idle native worker, then resume and detach it")
        pool.submit(collect_on_worker).result(timeout=10)
    print("PASS a collection on the preload worker can suspend and resume the native main thread")


def check(stage, calls):
    assembly = c.CDLL(str(stage / "GameAssembly.so"))
    unity = c.CDLL(str(stage / "UnityPlayer.so"))

    def export(name, result, *args):
        fn = getattr(assembly, "il2cpp_" + name)
        fn.argtypes, fn.restype = args, result
        return fn

    class Array(c.Structure):
        _fields_ = [(key, c.c_uint64) for key in ("klass", "monitor", "bounds", "length")]

    class String(c.Structure):
        _fields_ = [("klass", c.c_void_p), ("monitor", c.c_void_p), ("length", c.c_int32),
                    ("chars", c.c_uint16 * 4)]

    array = Array(0x123456789ABCDEF0, 0, 0, 71)
    string = String(length=3, chars=(c.c_uint16 * 4)(97, 98, 99, 0))
    for name, result, value, expected in (
        ("array_length", c.c_uint32, array, array.length),
        ("object_get_class", c.c_void_p, array, array.klass),
        ("string_length", c.c_int32, string, 3),
        ("string_chars", c.c_void_p, string, c.addressof(string.chars)),
    ):
        if export(name, result, c.c_void_p)(c.byref(value)) != expected:
            raise ValueError("Incorrect result from staged il2cpp_" + name)
        print("PASS staged il2cpp_" + name)

    # RTLD_DI_LINKMAP returns the Linux loader's actual relocated engine base.
    dlinfo = c.CDLL(None).dlinfo
    dlinfo.argtypes = (c.c_void_p, c.c_int, c.c_void_p)
    link = c.POINTER(c.c_size_t)()
    if dlinfo(unity._handle, 2, c.byref(link)) or not link:
        raise RuntimeError("could not locate loaded UnityPlayer.so")
    name = b"UnityEngine.Matrix4x4::TRS_Injected"
    registrations = json.loads(calls.read_bytes())
    address = link[0] + registrations[name.decode()]
    add = export("add_internal_call", None, c.c_char_p, c.c_void_p)
    add(name, address)
    resolved = export("resolve_icall", c.c_void_p, c.c_char_p)(name)
    if resolved != address:
        raise ValueError("original IL2CPP registry did not return the native Unity callback")
    vec = c.c_float * 3
    quat = c.c_float * 4
    matrix = (c.c_float * 16)()
    transform = c.CFUNCTYPE(None, *(c.POINTER(c.c_float),) * 4)(resolved)
    transform(vec(5, 7, 11), quat(0, 0, 0, 1), vec(2, 3, 4), matrix)
    if list(matrix) != [2, 0, 0, 0, 0, 3, 0, 0, 0, 0, 4, 0, 5, 7, 11, 1]:
        raise ValueError("resolved native Unity transform returned a different matrix")
    print("PASS staged IL2CPP registration, resolution and native Unity transform")

    for method, offset in registrations.items():
        add(method.encode(), link[0] + offset)
    export("set_data_dir", None, c.c_char_p)(os.fsencode(stage / "Reincarnation_Data"))
    if not export("init", c.c_int, c.c_char_p)(b"IL2CPP Root Domain"):
        raise RuntimeError("original IL2CPP initialization failed")
    try:
        domain = export("domain_get", c.c_void_p)()
        if not domain:
            raise ValueError("IL2CPP returned a null managed domain")
        count = c.c_size_t()
        assemblies = export("domain_get_assemblies", c.POINTER(c.c_void_p), c.c_void_p, c.POINTER(c.c_size_t))(
            domain, c.byref(count))
        image_of = export("assembly_get_image", c.c_void_p, c.c_void_p)
        name_of = export("image_get_name", c.c_char_p, c.c_void_p)
        images = {}
        for i in range(count.value):
            managed_image = image_of(assemblies[i])
            images[name_of(managed_image).decode()] = managed_image
        if count.value != 110 or not {"mscorlib.dll", "Assembly-CSharp.dll", "UnityEngine.CoreModule.dll"} <= images.keys():
            raise ValueError("original game assembly inventory is incomplete")
        find_class = export("class_from_name", c.c_void_p, c.c_void_p, c.c_char_p, c.c_char_p)
        title = find_class(images["Assembly-CSharp.dll"], b"Dark.View.Title", b"TitleScreen")
        get_method = export("class_get_method_from_name", c.c_void_p, c.c_void_p, c.c_char_p, c.c_int)
        if not title or not get_method(title, b"Start", 0):
            raise ValueError("original title-screen Start method is missing from the game assembly")
        klass = find_class(images["mscorlib.dll"], b"System", b"String")
        value = export("string_new", c.c_void_p, c.c_char_p)(b"Linux")
        if not klass or not value or export("object_get_class", c.c_void_p, c.c_void_p)(value) != klass:
            raise ValueError("allocated string has the wrong managed class")
        if export("string_length", c.c_int32, c.c_void_p)(value) != 5:
            raise ValueError("allocated managed string has the wrong length")
        length_method = get_method(klass, b"get_Length", 0)
        if not length_method:
            raise ValueError("System.String.Length is absent from the game metadata")
        exception = c.c_void_p()
        invoke = export("runtime_invoke", c.c_void_p, c.c_void_p, c.c_void_p, c.c_void_p, c.POINTER(c.c_void_p))
        result = invoke(length_method, value, None, c.byref(exception))
        if exception.value or not result:
            raise ValueError("managed invocation of System.String.Length failed")
        unbox = export("object_unbox", c.POINTER(c.c_int32), c.c_void_p)
        if unbox(result)[0] != 5:
            raise ValueError("the managed invoker returned the wrong boxed length")
        print("PASS original managed domain, 110 assemblies, title-screen metadata and live System.String allocation")
        print("PASS reflection invocation and boxed return from the original System.String.Length getter")
        obj = find_class(images["mscorlib.dll"], b"System", b"Object")
        allocate = export("array_new", c.c_void_p, c.c_void_p, c.c_size_t)
        root, child, unrelated = (allocate(obj, 1) for _ in range(3))
        header = export("array_object_header_size", c.c_uint32)()
        store = export("gc_wbarrier_set_field", None, c.c_void_p, c.c_void_p, c.c_void_p)
        store(root, root + header, child)
        store(child, child + header, root)
        seen, worlds, users = [], [], []
        notify = c.CFUNCTYPE(None, c.POINTER(c.c_void_p), c.c_int, c.c_void_p)
        world = c.CFUNCTYPE(None)

        @notify
        def inspect(objects, n, user):
            seen.extend(objects[:n])
            users.append(user)

        started, stopped = world(lambda: worlds.append("started")), world(lambda: worlds.append("stopped"))
        begin = export("unity_liveness_calculation_begin", c.c_void_p,
                       c.c_void_p, c.c_int, notify, c.c_void_p, world, world)
        state = begin(obj, 32, inspect, unrelated, started, stopped)
        if not state:
            raise ValueError("original liveness calculation did not allocate a state")
        try:
            export("unity_liveness_calculation_from_root", None, c.c_void_p, c.c_void_p)(root, state)
        finally:
            export("unity_liveness_calculation_end", None, c.c_void_p)(state)
        if set(seen) != {root, child} or set(users) != {unrelated} or worlds != ["stopped", "started"]:
            raise ValueError(f"incorrect liveness reachability or callbacks: {seen}, {users}, {worlds}")
        print("PASS original liveness traversal, cyclic managed references, native stop and restart callbacks")
        check_native_worker(export, domain)
    finally:
        export("shutdown", None)()
    print("PASS original managed domain shutdown")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("stage", type=Path)
    parser.add_argument("engine_calls", type=Path)
    args = parser.parse_args()
    check(args.stage.resolve(), args.engine_calls)
