"""Fingerprint every MasterMemory table independent of the LZ4 encoder version."""
import hashlib
import struct

import lz4.block
import msgpack
from Crypto.Cipher import AES
from Crypto.Util.Padding import unpad
from patch_masterdata import DEFAULT_IV, DEFAULT_KEY, read_lz4_ext_header


def table_digest(encrypted):
    key, iv = bytes.fromhex(DEFAULT_KEY), bytes.fromhex(DEFAULT_IV)
    data = unpad(AES.new(key, AES.MODE_CBC, iv).decrypt(encrypted), AES.block_size)
    stream = msgpack.Unpacker(raw=False,strict_map_key=False)
    stream.feed(data)
    toc = next(stream)
    start = stream.tell()
    result = hashlib.sha256()
    end = 0
    for name, (offset,size) in sorted(toc.items(),key=lambda item:item[1][0]):
        if offset!=end or offset+size>len(data)-start:
            raise ValueError('Invalid master table range')
        end=offset+size
        table=data[start+offset:start+end]
        entry=msgpack.unpackb(table,raw=True,strict_map_key=False)
        if isinstance(entry,msgpack.ExtType) and entry.code==99:
            length,block=read_lz4_ext_header(entry.data)
            table=lz4.block.decompress(block,uncompressed_size=length)
        name=name.encode('utf-8')
        result.update(struct.pack('<II',len(name),len(table)))
        result.update(name)
        result.update(table)
    if start+end!=len(data):
        raise ValueError('Unexpected trailing master data')
    return result.hexdigest()
