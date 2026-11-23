"""Exercise Linux Unity's actual C ABI against the staged game and native engine."""
import argparse
import ctypes as c
import json
from pathlib import Path


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
    address = link[0] + json.loads(calls.read_bytes())[name.decode()]
    export("add_internal_call", None, c.c_char_p, c.c_void_p)(name, address)
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


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("stage", type=Path)
    parser.add_argument("engine_calls", type=Path)
    args = parser.parse_args()
    check(args.stage.resolve(), args.engine_calls)
