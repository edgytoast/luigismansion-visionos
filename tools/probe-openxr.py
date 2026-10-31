"""Read-only OpenXR capability probe. Does not launch the game or create a session."""
import argparse
import ctypes as C
import json
import os
import winreg


class Extension(C.Structure):
    _fields_ = [('type', C.c_int), ('next', C.c_void_p), ('name', C.c_char * 128), ('version', C.c_uint32)]


class Application(C.Structure):
    _fields_ = [('name', C.c_char * 128), ('version', C.c_uint32),
                ('engine', C.c_char * 128), ('engineVersion', C.c_uint32), ('apiVersion', C.c_uint64)]


class InstanceCreate(C.Structure):
    _fields_ = [('type', C.c_int), ('next', C.c_void_p), ('flags', C.c_uint64),
                ('application', Application), ('layerCount', C.c_uint32), ('layers', C.c_void_p),
                ('extensionCount', C.c_uint32), ('extensions', C.c_void_p)]


class SystemGet(C.Structure):
    _fields_ = [('type', C.c_int), ('next', C.c_void_p), ('formFactor', C.c_int)]


class View(C.Structure):
    _fields_ = [('type', C.c_int), ('next', C.c_void_p),
                ('width', C.c_uint32), ('maxWidth', C.c_uint32),
                ('height', C.c_uint32), ('maxHeight', C.c_uint32),
                ('samples', C.c_uint32), ('maxSamples', C.c_uint32)]


def probe(loader):
    report = {'loader': os.path.abspath(loader)}
    try:
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r'SOFTWARE\Khronos\OpenXR\1') as key:
            report['active_runtime'] = winreg.QueryValueEx(key, 'ActiveRuntime')[0]
    except OSError as error:
        report['runtime_registry_error'] = str(error)
    xr = C.WinDLL(loader)

    def bind(name, args):
        function = getattr(xr, name)
        function.restype = C.c_int
        function.argtypes = args
        return function

    enumerate_extensions = bind('xrEnumerateInstanceExtensionProperties',
                                [C.c_char_p, C.c_uint32, C.POINTER(C.c_uint32), C.POINTER(Extension)])
    count = C.c_uint32()
    result = enumerate_extensions(None, 0, C.byref(count), None)
    report['enumerate_result'] = result
    if result < 0:
        return report
    extensions = (Extension * count.value)()
    for extension in extensions:
        extension.type = 2  # XR_TYPE_EXTENSION_PROPERTIES
    result = enumerate_extensions(None, count.value, C.byref(count), extensions)
    report['enumerate_result'] = result
    if result < 0:
        return report
    report['extensions'] = {entry.name.decode(): entry.version for entry in extensions}
    create = bind('xrCreateInstance', [C.POINTER(InstanceCreate), C.POINTER(C.c_void_p)])
    destroy = bind('xrDestroyInstance', [C.c_void_p])
    info = InstanceCreate()
    info.type = 3  # XR_TYPE_INSTANCE_CREATE_INFO
    info.application = Application(b'LuigisMansionVR capability probe', 1, b'LMVR', 1, 1 << 48)
    instance = C.c_void_p()
    result = create(C.byref(info), C.byref(instance))
    report['create_instance_result'] = result
    if result < 0:
        return report
    try:
        get_system = bind('xrGetSystem', [C.c_void_p, C.POINTER(SystemGet), C.POINTER(C.c_uint64)])
        system = C.c_uint64()
        result = get_system(instance, C.byref(SystemGet(4, None, 1)), C.byref(system))
        report['get_hmd_system_result'] = result
        if result < 0:
            return report
        enumerate_views = bind('xrEnumerateViewConfigurationViews',
                               [C.c_void_p, C.c_uint64, C.c_int, C.c_uint32,
                                C.POINTER(C.c_uint32), C.POINTER(View)])
        result = enumerate_views(instance, system, 2, 0, C.byref(count), None)
        report['enumerate_stereo_result'] = result
        if result < 0:
            return report
        views = (View * count.value)()
        for view in views:
            view.type = 41  # XR_TYPE_VIEW_CONFIGURATION_VIEW
        result = enumerate_views(instance, system, 2, count.value, C.byref(count), views)
        report['enumerate_stereo_result'] = result
        if result >= 0:
            report['stereo_views'] = [{'width': v.width, 'height': v.height, 'samples': v.samples} for v in views]
    finally:
        report['destroy_instance_result'] = destroy(instance)
    return report


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--loader', required=True, help='Absolute path to the installed OpenXR loader DLL')
    args = parser.parse_args()
    try:
        print(json.dumps(probe(args.loader), indent=2))
    except OSError as error:
        print(json.dumps({'error': str(error)}, indent=2))
        raise SystemExit(1)
