"""Build the Windows Unity adapters against an APK-derived link plan."""
import csv
import shutil
import subprocess
import sys
from pathlib import Path

from follow_path import build as follow_path, build_scroll
from liveness import build as liveness
from metadata_accessors import lower

SOURCE = Path(__file__).resolve().parent
ROOT = SOURCE.parents[1]
WRAPS = (
    'sub_836175ec',
    '_ZN6bunker7windows14ModuleRegistry4OpenEPKci',
    '_ZN6bunker7windows8Pthreads3RunEySt16initializer_listIyE',
    '_ZN6bunker7windows8Pthreads7InstallEv',
    '_ZN6recomp7Runtime9AddTracesEPKNS_5TraceEy',
    '_ZN6bunker7windows10SignalHost11BindCurrentEy',
    '_ZN6recomp7Runtime9AddImportEySt8functionIFvR5StateEE',
    '_ZN6recomp21PublishGuestOverridesEv',
)


def run(*args):
    subprocess.run([str(v) for v in args], check=True)


def compile_player(work, dest, cxx, clang, toolchain, original, bunker, abi):
    work.mkdir(parents=True, exist_ok=True)
    dest.mkdir(parents=True, exist_ok=True)
    plan = [line.strip('"') for line in (work.parent/'build/link.rsp').read_text().splitlines()]
    flags = plan[:plan.index('-static')]
    flags.extend(['-I'+str(abi), '-I'+str(SOURCE)])
    bootstrap = work/'embedded-bootstrap.o'
    run(cxx, *flags, '-DREINCOMP_EMBEDDED_PLAYER', '-c', ROOT/'host/game_bootstrap.cpp', '-o', bootstrap)
    host = work/'runtime-host.o'
    run(cxx, *flags, '-c', SOURCE/'runtime-host.cpp', '-o', host)
    old, = [v for v in plan if v.endswith('/host/game-bootstrap.o')]
    plan[plan.index(old)] = bootstrap.as_posix()
    tools = (ROOT/'game/heads-il2cpp.txt', toolchain/'remill-lift.exe', toolchain/'poll-instrument.exe', clang)
    plan[-2:] = ['-shared', '-Wl,--exclude-all-symbols,--image-base,0x240000000',
                 '-Wl,'+','.join('--wrap='+n for n in WRAPS), host.as_posix(),
                 follow_path(work, original, tools).as_posix(), build_scroll(work, original, tools).as_posix(),
                 liveness(work, original, tools).as_posix(),
                 '-o', (dest/'GuestRuntime.dll').as_posix()]
    response = work/'runtime-link.rsp'
    response.write_text(''.join('"'+v+'"\n' for v in plan))
    run(cxx, '@'+str(response))
    run(sys.executable, SOURCE/'route-liveness-stop.py', dest/'GuestRuntime.dll', cxx.with_name('objdump.exe'))
    run(cxx, *flags, '-shared', '-static', SOURCE/'engine-calls.cpp', ROOT/'host/windows_shared_preferences.cpp',
        '-lshlwapi', '-lbcrypt', '-ladvapi32', '-lxinput9_1_0', '-ldinput8', '-ldxguid', '-o', dest/'EngineCalls.dll')
    compile_adapter(work, dest, cxx, flags, original, bunker, abi)
    compile_fsr(work, dest, cxx)


def compile_fsr(work, dest, cxx, link_flags=(), import_name=None):
    loader = work/(import_name or 'libvulkan-import.a')
    dlltool = cxx.with_name(cxx.name.replace('g++', 'dlltool'))
    if import_name:
        # dlltool uses the output argument to name COFF symbols. Use a stable
        # relative name so moving the checkout or build tree preserves them.
        subprocess.run([dlltool,'-d',SOURCE/'vulkan-loader.def','-l',import_name],cwd=work,check=True)
        # GNU PE ld sorts imports by the archive filename. The release put
        # Vulkan first. A numeric relative name preserves that on any drive.
        link_loader = '000-vulkan.a'
        shutil.copyfile(loader,work/link_loader)
    else:
        run(dlltool, '-d', SOURCE/'vulkan-loader.def', '-l', loader)
        link_loader = loader
    args = [cxx, '-std=c++17', '-O2', '-DNOMINMAX', '-shared', '-static',
        '-I'+str(ROOT/'third_party/fidelityfx-api'), '-I'+str(ROOT/'third_party/vulkan-headers/include'),
        *(SOURCE/name for name in ('fsr31.cpp','fsr31-interop.cpp','fsr31-camera.cpp','fsr31-pass.cpp')),
        link_loader, *link_flags, '-lopengl32', '-lgdi32', '-o', dest/'Fsr31.dll']
    subprocess.run(args,check=True,cwd=work if import_name else None)


def compile_adapter(work, dest, cxx, flags, original, bunker, abi):
    inventory = abi/'player-apis.csv'
    source = lower((SOURCE/'runtime-adapter.cpp').read_text(), original, inventory)
    with inventory.open() as stream:
        exports = [r['windows_player_name'] for r in csv.DictReader(stream) if r['translated_entry']=='True']
    for name in exports:
        if f' {name}(' not in source and f'({name},' not in source:
            source += f'\nextern "C" __declspec(dllexport) void {name}() {{ boundary("{name}"); }}\n'
    cpp = work/'runtime-adapter.generated.cpp'
    cpp.write_text(source)
    mobile = bunker/'android/app/src/main/cpp'
    modules = []
    for name in ('title_now','badge_pace','arena_season','big_hunt_season','render_pace','flying_mom',
                 'roulette_ads','bunker_button_layout'):
        obj = work/(name+'.o')
        run(cxx,'-x','c','-std=gnu11','-O2','-c',mobile/(name+'.c'),'-o',obj)
        modules.append(obj)
    # MethodInfo contains a guest address. Enter the original method through
    # the VM, and free its returned type name with the matching allocator.
    def adapted_module(name, changes, prefix='', suffix=''):
        source = (mobile/(name+'.c')).read_text(encoding='utf-8')
        for original_text, desktop_text in changes.items():
            if source.count(original_text) != 1:
                raise ValueError(f'The pinned Bunker {name} module changed')
            source = source.replace(original_text, desktop_text)
        adapted = work/(name+'.c')
        adapted.write_text(prefix+source+suffix,encoding='utf-8')
        obj = work/(name+'.o')
        run(cxx,'-x','c','-std=gnu11','-O2','-I'+str(mobile),'-c',adapted,'-o',obj)
        modules.append(obj)
    adapted_module('battle_mods_settings',{
        'rename(temp, path) != 0': '!MoveFileExA(temp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)',
    },'#include <windows.h>\n')
    # Unity already calls the translated battle HUD Update on its main thread.
    # Use the pinned Arena rules and popup without replacing a guest MethodInfo.
    adapted_module('battle_mods',{
        'update_original(self, m);': '(void)m;',
        'pthread_t thread;\n    if (pthread_create(&thread, NULL, install, NULL)) return -1;\n    pthread_detach(thread);\n    return 0;':
            'return look_up();',
    },suffix='\nvoid desktop_battle_update(void *self){update_hook(self,game.update);}\n')
    adapted_module('battle_mods_ui',{
        'free(type);':'il2cpp_free(type);',
        'FIELD(action, f_method_ptr, void *) = (void *)fn;':
            'FIELD(action, f_method_ptr, void *) = desktop_arena_tap_address(fn == row_tap);',
        'static void *action_for(':'static void row_tap(void *, Method *);\nstatic void *action_for(',
    },'extern void il2cpp_free(void *);\nextern void *desktop_arena_tap_address(unsigned);\n',
       '\nvoid desktop_arena_tap(unsigned row, void *target){(row ? row_tap : open_popup)(target,NULL);}\n')
    adapted_module('render_effects',{
        '((void *(*)(void *))active->pointer)(NULL)': 'il2cpp.runtime_invoke(active, NULL, NULL, NULL)',
        'free(type);': 'il2cpp_free(type);',
    },'extern void il2cpp_free(void *);\n')
    # Unity invokes Start/Update through our managed boundary. The delegate
    # held by the original button uses a registered guest callback address.
    run(dest.parent/'pods/python/python.exe', '-I', SOURCE/'header_emblem.py',
        dest/'WindowsPlayer_Data', bunker, work/'bunker_icon.h')
    adapted_module('bunker_button',{
        'free(type);': 'il2cpp_free(type);',
        'FIELD(action, f_method_ptr, void *) = (void *)tap;':
            'FIELD(action, f_method_ptr, void *) = desktop_bunker_tap_address();',
        'start_original(self, method);': '(void)method;',
        'update_original(self, method);': '(void)method;',
        'title_now_tick();': '/* polled by the desktop frame callback */',
        'corners(seen->row, row)': 'desktop_header_corners(seen->row, row)',
        'if (!seen->layout || seen->frames++ % 30) return;':
            'if (!seen->layout || seen->frames++ % 30 || !visible || !desktop_header_row_visible(seen->row)) return;',
        'if (!unboxed_bool(call(m_in_hierarchy, call(m_component_go, counter, NULL), NULL))) continue;\n'
        '        if (corners(call(m_component_tr, counter, NULL), box)) continue;':
            'if (desktop_header_counter_corners(counter, box)) continue;',
        'void *count = call(m_child_count, seen->row, NULL);\n'
        '    int shown = 0, n = count ? FIELD(count, 16, int) : 0;\n'
        '    for (int i = 0; i < n; i++)\n'
        '        shown += unboxed_bool(call(m_active_self, call(m_component_go, call(m_child, seen->row, (void *[]){&i}), NULL), NULL));':
            'int shown = desktop_header_buttons_shown(seen->row);',
        'components(seen->scope, k_counter)': 'desktop_header_counters(seen->scope, type_of(k_counter))',
        'call(m_set_spacing, seen->layout, (void *[]){&want});':
            'call(m_set_spacing, seen->layout, (void *[]){&want});\n'
            '        desktop_refresh_header_layout(seen->row);',
        'pthread_t thread;\n    if (pthread_create(&thread, NULL, install, NULL)) return -1;\n    pthread_detach(thread);\n    return 0;':
            'return look_up();',
    },'extern void il2cpp_free(void *);\nextern void *desktop_bunker_tap_address(void);\n'
       'extern void desktop_refresh_header_layout(void *row);\n'
       'extern int desktop_header_corners(void *transform, float out[4]);\n'
       'extern int desktop_header_row_visible(void *row);\n'
       'extern int desktop_header_counter_corners(void *counter, float out[4]);\n'
       'extern int desktop_header_buttons_shown(void *row);\n'
       'extern void *desktop_header_counters(void *scope, void *type);\n',
       '\nvoid desktop_header_start(void *self){start_hook(self,m_start);}\n'
       'void desktop_header_update(void *self){update_hook(self,m_update);}\n'
       'void desktop_header_tap(void *self){tap(self,NULL);}\n')
    run(cxx, *flags, '-DREINCOMP_BUNKER_MODULES', '-I'+str(mobile),
        '-static', '-shared', cpp, *modules, ROOT/'host/bunker_view.cpp',
        '-lole32','-luuid','-lcomctl32','-o',dest/'GameAssembly.dll')
