"""Render the Windows counterpart of Android's Bunker MainActivity with its translations."""
from pathlib import Path
import sys
from jinja2 import Environment, FileSystemLoader, select_autoescape


def render(root, assets, reincomp):
    app = root / 'pods/app'
    sys.path.insert(0, str(app))
    import bunker_text
    bunker_text.reload(assets / 'translation/active.json')
    templates = Environment(loader=FileSystemLoader(Path(__file__).parent),
                            autoescape=select_autoescape())
    bunker_text.install(templates)
    version = (root/'pods/VERSION').read_text(encoding='utf-8').splitlines()[0].removeprefix('Bunker ')
    return templates.get_template('resource-setup.html').render(version=version, reincomp=reincomp)


if __name__ == '__main__':
    sys.stdout.buffer.write(render(Path(sys.argv[1]), Path(sys.argv[2]), sys.argv[3]).encode('utf-8'))
