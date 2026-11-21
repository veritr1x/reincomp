"""Adapt the pinned lifter's file I/O for random access in a browser worker."""
from pathlib import Path
import sys


def replace_once(source, old, new):
    if source.count(old) != 1:
        raise ValueError("The pinned Remill input interface changed: " + old)
    return source.replace(old, new)


def prepare(source, output):
    text = source.read_text()
    text = replace_once(text, 'std::vector<uint8_t> bytes;', 'BrowserImage bytes;')
    start = text.index('    std::ifstream input(FLAGS_bytes_file')
    end = text.index('  } else {', start)
    text = text[:start] + '    memory.bytes.open(FLAGS_bytes_file);\n' + text[end:]
    marker = '  std::ifstream input(path);'
    binary = '''  if (path.ends_with(".u32")) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    const auto length = stream.tellg();
    if (length <= 0 || length % 4) throw std::invalid_argument("Invalid binary trace heads");
    std::vector<uint32_t> heads(static_cast<size_t>(length) / 4);
    stream.seekg(0);
    if (!stream.read(reinterpret_cast<char *>(heads.data()), length))
      throw std::runtime_error("Incomplete binary trace heads");
    return std::vector<uint64_t>(heads.begin(), heads.end());
  }
'''
    text = replace_once(text, marker, binary + marker)
    output.write_text('#include "browser-image.h"\n' + text)


if __name__ == '__main__':
    prepare(Path(sys.argv[1]), Path(sys.argv[2]))
