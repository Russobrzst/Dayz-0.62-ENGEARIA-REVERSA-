#!/usr/bin/env python3
"""Append a small x64 bootstrap to the exact supported DayZ 0.62 PE builds.

The result imports dayz_rotation_move_v12.dll and invokes its startup hook at
the original PE entry point. The source executable is never modified.
"""
from __future__ import annotations

import argparse
import hashlib
import struct
from pathlib import Path

SUPPORTED = {
    "423212bad091f47f05bd7621f6ba4040d4f3db4c93752c3f19ee0fa7a62bdb9d": "DayZ_x64.exe",
    "531330410a8894b81b10e9e2f3d562b4f8a4c35c925d7504aff5717a17ccccb6": "DayZServerP69_x64.exe",
}
DLL_NAME = b"dayz_rotation_move_v12.dll\0"
BRIDGE_EXPORT = b"DZ62_InstallDebugBinding\0"
KERNEL_NAME = b"kernel32.dll\0"
EXIT_EXPORT = b"ExitProcess\0"


def u16(data: bytearray | bytes, offset: int) -> int:
    return struct.unpack_from("<H", data, offset)[0]


def u32(data: bytearray | bytes, offset: int) -> int:
    return struct.unpack_from("<I", data, offset)[0]


def put16(data: bytearray, offset: int, value: int) -> None:
    struct.pack_into("<H", data, offset, value)


def put32(data: bytearray, offset: int, value: int) -> None:
    struct.pack_into("<I", data, offset, value)


def align(value: int, boundary: int) -> int:
    return (value + boundary - 1) // boundary * boundary


def checksum(data: bytearray, checksum_offset: int) -> int:
    total = 0
    for offset in range(0, len(data), 2):
        if checksum_offset <= offset < checksum_offset + 4:
            continue
        word = data[offset] | ((data[offset + 1] if offset + 1 < len(data) else 0) << 8)
        total = (total & 0xFFFF) + word + (total >> 16)
    total = (total & 0xFFFF) + (total >> 16)
    return (total + len(data)) & 0xFFFFFFFF


def parse(data: bytearray):
    if data[:2] != b"MZ":
        raise ValueError("Arquivo não tem cabeçalho MZ")
    pe = u32(data, 0x3C)
    if data[pe:pe + 4] != b"PE\0\0":
        raise ValueError("Assinatura PE inválida")
    coff = pe + 4
    sections_count = u16(data, coff + 2)
    optional_size = u16(data, coff + 16)
    optional = coff + 20
    if u16(data, optional) != 0x20B:
        raise ValueError("Somente PE32+ x64 é suportado")
    image_base = struct.unpack_from("<Q", data, optional + 24)[0]
    section_alignment, file_alignment = struct.unpack_from("<II", data, optional + 32)
    size_image, size_headers = struct.unpack_from("<II", data, optional + 56)
    entry_rva = u32(data, optional + 16)
    directories = optional + 112
    sections = []
    section_table = optional + optional_size
    for index in range(sections_count):
        offset = section_table + index * 40
        name = bytes(data[offset:offset + 8]).split(b"\0", 1)[0]
        virtual_size, virtual_address, raw_size, raw_pointer = struct.unpack_from("<IIII", data, offset + 8)
        sections.append((offset, name, virtual_size, virtual_address, raw_size, raw_pointer))
    return {
        "pe": pe, "coff": coff, "optional": optional, "optional_size": optional_size,
        "directories": directories, "sections_count": sections_count, "section_table": section_table,
        "sections": sections, "image_base": image_base, "section_alignment": section_alignment,
        "file_alignment": file_alignment, "size_image": size_image, "size_headers": size_headers,
        "entry_rva": entry_rva,
    }


def rva_offset(info, rva: int, length: int = 1) -> int:
    if rva < info["size_headers"] and rva + length <= info["size_headers"]:
        return rva
    for _, _, vs, va, rs, rp in info["sections"]:
        span = max(vs, rs)
        if va <= rva and rva + length <= va + span:
            delta = rva - va
            if delta + length > rs:
                break
            return rp + delta
    raise ValueError(f"RVA 0x{rva:x} não mapeia para bytes do arquivo")


def old_import_descriptors(data: bytearray, info):
    rva, size = struct.unpack_from("<II", data, info["directories"] + 8)
    if not rva or size < 20:
        raise ValueError("Tabela de imports ausente ou inválida")
    offset = rva_offset(info, rva, size)
    descriptors = []
    for cursor in range(offset, offset + size, 20):
        row = bytes(data[cursor:cursor + 20])
        if len(row) != 20:
            raise ValueError("Tabela de imports truncada")
        if row == bytes(20):
            return descriptors
        descriptors.append(row)
    raise ValueError("Tabela de imports não termina dentro do tamanho declarado")


def import_name(data: bytearray, info, descriptor: bytes) -> str:
    name_rva = struct.unpack_from("<I", descriptor, 12)[0]
    offset = rva_offset(info, name_rva)
    end = data.find(0, offset, min(len(data), offset + 512))
    if end < 0:
        raise ValueError("Nome de DLL importada não terminado")
    return bytes(data[offset:end]).decode("ascii", "replace")


def patch(source: Path, destination: Path) -> dict:
    original = bytearray(source.read_bytes())
    source_hash = hashlib.sha256(original).hexdigest()
    if source_hash not in SUPPORTED:
        raise ValueError(f"Build não suportado. SHA-256 observado: {source_hash}")
    info = parse(original)
    if any(section[1] == b".dzboot" for section in info["sections"]):
        raise ValueError("O executável já contém a seção .dzboot")

    descriptors = old_import_descriptors(original, info)
    names = [import_name(original, info, descriptor).casefold() for descriptor in descriptors]
    if DLL_NAME[:-1].decode().casefold() in names:
        raise ValueError("A DLL da rotação já aparece na tabela de imports")
    if "kernel32.dll" not in names and "kernelbase.dll" not in names:
        raise ValueError("Não encontrei kernel32.dll/kernelbase.dll para importar ExitProcess")

    new_section_header = info["section_table"] + info["sections_count"] * 40
    add_section = new_section_header + 40 <= info["size_headers"]
    if add_section:
        section_va = align(max(va + max(vs, rs) for _, _, vs, va, rs, _ in info["sections"]), info["section_alignment"])
        raw_pointer = align(len(original), info["file_alignment"])
        section_file_base = raw_pointer
    else:
        # The server image has no spare section-header slot. Extend its final
        # file-backed section, provided that no unrecognized overlay follows it.
        last_header, _, last_vs, last_va, last_rs, last_rp = info["sections"][-1]
        if last_rp + last_rs != len(original):
            raise ValueError("Sem slot de cabeçalho e com overlay após a última seção; patch conservador cancelado")
        section_file_base = last_rp + align(max(last_vs, last_rs), info["file_alignment"])
        section_va = last_va + align(max(last_vs, last_rs), info["file_alignment"])
        raw_pointer = section_file_base
    section = bytearray()

    def reserve(payload: bytes, boundary: int = 1) -> int:
        while len(section) % boundary:
            section.append(0)
        offset = len(section)
        section.extend(payload)
        return offset

    # Reserve descriptor space; existing descriptors retain their original RVAs.
    descriptor_offset = reserve(bytes((len(descriptors) + 3) * 20), 4)
    dll_name_offset = reserve(DLL_NAME)
    bridge_hint_offset = reserve(b"\0\0" + BRIDGE_EXPORT, 2)
    kernel_name_offset = reserve(KERNEL_NAME)
    exit_hint_offset = reserve(b"\0\0" + EXIT_EXPORT, 2)
    bridge_int_offset = reserve(bytes(16), 8)
    bridge_iat_offset = reserve(bytes(16), 8)
    kernel_int_offset = reserve(bytes(16), 8)
    kernel_iat_offset = reserve(bytes(16), 8)
    stub_offset = reserve(bytes(38), 16)

    def rva(local_offset: int) -> int:
        return section_va + local_offset

    def write_thunks(offset: int, name_rva: int):
        struct.pack_into("<QQ", section, offset, name_rva, 0)

    write_thunks(bridge_int_offset, rva(bridge_hint_offset))
    write_thunks(bridge_iat_offset, rva(bridge_hint_offset))
    write_thunks(kernel_int_offset, rva(exit_hint_offset))
    write_thunks(kernel_iat_offset, rva(exit_hint_offset))

    new_descriptors = list(descriptors)
    new_descriptors.append(struct.pack("<IIIII", rva(bridge_int_offset), 0, 0, rva(dll_name_offset), rva(bridge_iat_offset)))
    new_descriptors.append(struct.pack("<IIIII", rva(kernel_int_offset), 0, 0, rva(kernel_name_offset), rva(kernel_iat_offset)))
    new_descriptors.append(bytes(20))
    for index, descriptor in enumerate(new_descriptors):
        section[descriptor_offset + index * 20:descriptor_offset + (index + 1) * 20] = descriptor

    # Win64 ABI bootstrap: call install(nullptr); fail closed via ExitProcess on
    # anything other than Applied(0)/Unchanged(1); then jump to the stock OEP.
    stub_rva = rva(stub_offset)
    code = bytearray(b"\x48\x83\xEC\x28\x31\xC9\xFF\x15\0\0\0\0\x83\xF8\x01\x76\x0C\xB9\x01\0\0\0\xFF\x15\0\0\0\0\xCC\x48\x83\xC4\x28\xE9\0\0\0\0")
    bridge_call_next = stub_rva + 12
    struct.pack_into("<i", code, 8, rva(bridge_iat_offset) - bridge_call_next)
    exit_call_next = stub_rva + 28
    struct.pack_into("<i", code, 24, rva(kernel_iat_offset) - exit_call_next)
    jump_next = stub_rva + 38
    struct.pack_into("<i", code, 34, info["entry_rva"] - jump_next)
    section[stub_offset:stub_offset + len(code)] = code

    section_virtual_size = len(section)
    section_raw_size = align(section_virtual_size, info["file_alignment"])
    section.extend(bytes(section_raw_size - len(section)))
    if raw_pointer > len(original):
        original.extend(bytes(raw_pointer - len(original)))
    original.extend(section)

    if add_section:
        original[new_section_header:new_section_header + 40] = bytes(40)
        original[new_section_header:new_section_header + 8] = b".dzboot\0"
        struct.pack_into("<IIIIIIHHI", original, new_section_header + 8,
            section_virtual_size, section_va, section_raw_size, raw_pointer, 0, 0, 0, 0,
            0xE0000060)  # code + initialized data, read/write/execute
        put16(original, info["coff"] + 2, info["sections_count"] + 1)
    else:
        last_header, _, last_vs, last_va, last_rs, last_rp = info["sections"][-1]
        section_delta = section_va - last_va
        put32(original, last_header + 8, section_delta + section_virtual_size)
        put32(original, last_header + 16, last_rs + section_raw_size)
        flags = u32(original, last_header + 36)
        flags = (flags & ~0x02000000) | 0xE0000060
        put32(original, last_header + 36, flags)
    put32(original, info["optional"] + 16, stub_rva)
    put32(original, info["optional"] + 56, align(section_va + section_virtual_size, info["section_alignment"]))
    struct.pack_into("<II", original, info["directories"] + 8, rva(descriptor_offset), len(new_descriptors) * 20)

    # Authenticode cannot survive a modified image. Clear its file-offset data
    # directory and certificate bytes in this output copy.
    cert_offset, cert_size = struct.unpack_from("<II", original, info["directories"] + 4 * 8)
    if cert_offset and cert_size and cert_offset + cert_size <= len(original):
        original[cert_offset:cert_offset + cert_size] = bytes(cert_size)
    struct.pack_into("<II", original, info["directories"] + 4 * 8, 0, 0)
    checksum_offset = info["optional"] + 64
    put32(original, checksum_offset, 0)
    put32(original, checksum_offset, checksum(original, checksum_offset))

    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_bytes(original)

    # Re-read critical headers and verify imports, mapped stub bytes, and identity.
    verified = parse(bytearray(destination.read_bytes()))
    if verified["entry_rva"] != stub_rva or (add_section and verified["sections"][-1][1] != b".dzboot"):
        raise ValueError("Verificação pós-patch falhou: seção/entry point")
    import_rva, import_size = struct.unpack_from("<II", destination.read_bytes(), verified["directories"] + 8)
    imp_off = rva_offset(verified, import_rva, import_size)
    names_after = []
    for cursor in range(imp_off, imp_off + import_size, 20):
        row = destination.read_bytes()[cursor:cursor + 20]
        if row == bytes(20):
            break
        names_after.append(import_name(destination.read_bytes(), verified, row).casefold())
    if "dayz_rotation_move_v12.dll" not in names_after or "kernel32.dll" not in names_after:
        raise ValueError("Verificação pós-patch falhou: imports adicionados")
    return {
        "source": str(source), "source_sha256": source_hash,
        "patched": str(destination), "patched_sha256": hashlib.sha256(original).hexdigest(),
        "patched_bytes": len(original), "entry_rva": f"0x{stub_rva:x}",
        "original_entry_rva": f"0x{info['entry_rva']:x}", "imports": names_after,
        "authenticode": "signature directory cleared; source remains unchanged",
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path, help="Exact supported DayZ x64 executable")
    parser.add_argument("destination", type=Path, help="New output executable; never overwrites source")
    args = parser.parse_args()
    if args.source.resolve() == args.destination.resolve():
        raise SystemExit("Destino deve ser diferente do executável original")
    try:
        print(patch(args.source, args.destination))
    except Exception as error:
        raise SystemExit(f"Falha sem alterar o original: {error}")


if __name__ == "__main__":
    main()
