"""Render the desktop Bunker launcher from its shared page and text catalogue."""
from pathlib import Path
import sys
from jinja2 import Environment, FileSystemLoader, select_autoescape


def render_page(app, version, reincomp, active=None, linux=False):
    sys.path.insert(0, str(app))
    import bunker_text
    if active is not None:
        bunker_text.reload(active)
    paths = [Path(__file__).parent]
    if linux:
        paths.append(paths[0].parent / 'linux_player')
    templates = Environment(loader=FileSystemLoader(paths), autoescape=select_autoescape())
    bunker_text.install(templates)
    return templates.get_template('resource-setup.html').render(version=version, reincomp=reincomp, linux=linux)


def render(root, assets, reincomp):
    version = (root/'pods/VERSION').read_text(encoding='utf-8').splitlines()[0].removeprefix('Bunker ')
    return render_page(root / 'pods/app', version, reincomp, assets / 'translation/active.json')


if __name__ == '__main__':
    sys.stdout.buffer.write(render(Path(sys.argv[1]), Path(sys.argv[2]), sys.argv[3]).encode('utf-8'))
